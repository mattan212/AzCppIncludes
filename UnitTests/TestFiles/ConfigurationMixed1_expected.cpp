#include "a-pch.h"
#include "z-pch.h"
#include "a.h"
#include "aa.h"
//#include <xxx>
#include "bb.h"
//#include <zzz>
#include "e.h"

#ifdef TEST1
#include "alpha.h"
#include "zebra.h"
#elif TEST2
#include "beta.h"
#include "gamma.h"
#include <bravo.h>
#elif TEST3
#include "z-pch.h"
#include "e.h"
#include <c.h>
#include <d.h>
#endif

#include <c.h>
#include <d.h>
#include <yyy>

using namespace std;

extern IPlaceholderManager* g_pPlaceholderManager;

constexpr inline const UpdateRingSettings::Ramp EnableTryRepairReparseTagOnCF{ 1690 }; // owner=@mamargal enables CreateFile attempt to repair reparse tag when hits ERROR_REPARSE_TAG_INVALID while processing LC. 

// The realizer can touch the disk, so try not to overwhelm it with more than 3 async work items
const unsigned int c_asyncPoolMaxThreads = 3;

Realizer::Realizer(_In_ DriveInfo& drive) :
    _currentProcessedLC(nullptr),
    _pDrive(&drive),
    _allowLCTimeout(true),
    _processLCHook(nullptr),
    _lastTimeRealizerProcessingChanges(0),
    _scanIdDuringLastPostponedChangesRetry(0),
    _pLocalPostponedChanges(new PostponedChanges()),
    _asyncWorkPool(MakeSharedWorkerThreadPool(
        THREADNAME_REALIZER_POOL,
        ThreadType::RealizerWorkerPool,
        c_asyncPoolMaxThreads,
        FETCHER_THREAD_IDLE_TIME)),
    _spRealizerChangeInterpreter(std::make_unique<RealizerChangeInterpreter>())
{
}

Realizer::~Realizer()
{
    FreeAllLocalAndPostponedChanges();
    if (_pLocalPostponedChanges != nullptr)
    {
        delete _pLocalPostponedChanges;
        _pLocalPostponedChanges = nullptr;
    }

    // all LCs should be done running by this point
    _asyncWorkPool->Stop();
    _asyncWorkPool->WaitForStop();
}

/// <summary>
/// Entry point into the Realizer from Coreloop. This function pulls LocalChanges from the front of the Standard Priority FIFO queue in order
/// and hands them to ProcessLC to be routed to their correct handlers. It will continue to process changes from the front of the queue until the queue is empty or until the alloted timeslice
/// period has expired. If the timeslice has expired, the function exits with items remaining in the queue and control is returned to Coreloop so that it can handle any pending UX or high-priority requests.
/// Once those have been handled, the handleScanState function will again invoke handleLocalChanges.
/// 
/// If ProcessLC returns true for a given LC object then that LC is removed from the queue and is disposed.
/// If ProcessLC returns false, the function checks to see if it has been postponed. If it has, then the object is removed from the queue and inserted at the end of the postponedChanges queue.
/// If ProcessLC returns false and the LC has not been postponed, then the LC remains at the front of the queue and is again handed to ProcessLC.
/// If the processed LC was marked for deletion, the LC and its dependents will be removed from the queue and disposed. 
/// </summary>
void Realizer::ProcessChanges()
{
    OnStartLCProcessing();
    const unsigned int start = getTimerVal();
    DriveInfo* pDrive = DriveInfo::GetSkyDriveDrive();

    const bool notReadyScopesSupported =
        (pDrive != nullptr) &&
        GetFeatureSupport()->IsSupported(Feature::SharePointDataModel);

    if (notReadyScopesSupported)
    {
        // Note that the cache may be refreshed again during the LCs execution, e.g. following scope recovery 
        pDrive->RefreshCacheOfNotReadyTopLevelScopeIds();
    }

    LocalChange* pLC = nullptr;
    while (!g_clientState.GetStopCore() && !_localChanges.Empty())
    {
        pLC = _localChanges.Front();
        ODAssert(pLC != nullptr, Asserts::ClientCore::LocalChanges1);

        CheckPtrPadding(pLC);

        ScenarioTracking::TransferScope transferScope(_localChanges.GetScenarioTraceForLocalChange(pLC));
        LogLocalChangesHandlingQueuedChanges(LCToLog(pLC), _localChanges.Size());

        const se::set<ResourceID> emptyScopeIdsList;

        const int status = ProcessLC(
            pLC, 
            notReadyScopesSupported ? pDrive->GetCacheOfNotReadyTopLevelScopeIds() : emptyScopeIdsList);

        if (GetUpdateRingSettingsManager()->IsSafe(UpdateRingSettings::KillSwitch::RealizerMarkDeletedHandling) &&
            pLC->IsMarkedDeleted())
        {
            // In the current design, once an LC is marked for deletion it is immediatly being dropped along with
            // all of its dependants, regardless of its processing status. 
            // That is why we don't need to check whether it was marked for deletion prior to processing - 
            // we are not persisting the 'marked' field, so the LC is either marked and dropped here, or it is not marked.
            HandleMarkedForDeletionChange(pDrive, pLC);
        }
        else if (status)
        {
            FinalizeSuccessProcessLC(pDrive, pLC, true /* flushToDb */);

            // Delete the processed LC and remove it from the queue 
            _localChanges.PopFront();
        }
        else if (pLC->ShouldConvertToAnotherLCType())
        {
            ConvertLCInPlace(pLC);
        }
        else if (pLC->IsPostponed())
        {
            ODAssert(pDrive != nullptr, Asserts::ClientCore::LocalChanges2);
            bool ret = _localChanges.RemoveAndRelease(pLC);
            ODAssertEx(ret, Asserts::ClientCore::LocalChanges3, L"Failed to remove %ls from LC list", ResourceIdToString(pLC->resourceID).c_str());
            
            _pLocalPostponedChanges->PostponeChange(pDrive, pLC);
        }
        else
        {
            break;
        }

        if (ShouldTimeoutLCProcessing(start))
        {
            break;
        }
    }

    _lastTimeRealizerProcessingChanges = time(nullptr);
}

/// <summary>
/// When an LC is marked for deletion, we may drop all of its dependent changes.
/// We do that by retrieving them from the LC's dependency graph, and queueing them
/// in a list called markedForDeletion. Then we iterate over the list, mark all
/// of the peristed changes (by putting them in persistedChanges list), and execute
/// FinalizeSuccessProcessLC over each LC, without flushing to DB.
/// Finally we flush all the changes to DB to ensure a single transaction.
/// </summary>
void Realizer::HandleMarkedForDeletionChange(_In_ DriveInfo* pDrive, _In_ LocalChange* pLC)
{
    se::list<LocalChange*> markedForDeletion;
    se::list<LocalChange*> persistedChanges;

    pLC->GetChangeGraph()->GetDependents(markedForDeletion, true);
    markedForDeletion.push_back(pLC);

    LogRealizerDroppingMarkedDeletedChanges(markedForDeletion.size(), LCToLog(pLC), pLC->resourceID, pLC->parentScopeID);

    for (auto& pChangeToRemove : markedForDeletion)
    {
        // LC's postponed.pos is the value of the offset in the database in which this record was stored. 
        // If that value is higher than 0, it indicates that this record is persisted, which means that 
        // we will need to add it to the persistedChanges list for handling.
        if (pChangeToRemove->postponed.pos > 0)
        {
            persistedChanges.push_back(pChangeToRemove);
        }

        _localChanges.RemoveAndRelease(pChangeToRemove);
        FinalizeSuccessProcessLC(pDrive, pChangeToRemove, false /* flushToDb */);
    }

    // Remove all persisted LCs from DB in a single transaction
    LogRealizerRemovingMarkedDeletedChangesFromDB(persistedChanges.size());

    if (g_clientState.databaseManager->RemovePostponedLCs(persistedChanges))
    {
        TelemetryRealizerMarkedDeletedHandlingEvent(pLC->ChangeType(), markedForDeletion.size(), persistedChanges.size());
    }
    else
    {
        // If we failed to update the db then we are in an inconsistent state where the in-memory data
        // doesn't match the db, so we should terminate the core. The Realizer will retry this
        // again when the core is restarted.
        terminateCore(g_clientState.databaseManager->GetFlushDBError());
    }

    std::for_each(markedForDeletion.begin(), markedForDeletion.end(), [](LocalChange* pLocalChange)
    {
        delete pLocalChange;
    });
}

/// <summary>
/// Finalize a successful LC processing.
/// If the LC was previously postponed it will have a db record -
/// the flushToDb parameter is used to determine whether the record will be removed from the db when 
/// it's removed from the postponed changes queue.
/// </summary>
void Realizer::FinalizeSuccessProcessLC(_In_ DriveInfo* pDrive, _In_ LocalChange* pLC, _In_ const bool flushToDb)
{
    // Update sync status after removing change to process
    if (pDrive != nullptr && (pLC->changeType != LC_PERSIST_SYNC_TOKEN))
    {
        pDrive->SetTransferSyncStatusChanged();
    }

    // If LC processing succeeded (or LC has been dropped without execution), and it was previously postponed, complete postpone and, for persisted LCs, 
    // delete the LC from the DB. If PostponedRetryComplete has been already called during LC processing (while updating file/folder in the DB after 
    // successful execution of file/folder LCs), the postponed state has been reset by the previous call and the new call will do nothing.
    PostponedChanges::PostponedRetryComplete(pLC, flushToDb);

    // If this LC was created as a result of a repair action, notify listeners that it has finished processing.
    if (IsAnyFlagSet(pLC->flags, LocalChangeFlags::RepairLC))
    {
        g_clientState.pEventMachine->FireEvent1(
            EM_REPAIR_LC_DONE_PROCESSING,
            EM_HEALING_ITEM_RESOURCE_ID, pLC->resourceID
        );
    }
}

bool Realizer::HasChangesToProcess() const
{
    return (!_localChanges.Empty());
}

unsigned int Realizer::ChangesToProcessCount() const
{
    return _localChanges.Size();
}

void Realizer::AddChange(_In_ IRealizerChange& rc)
{
    _localChanges.PushBack(static_cast<LocalChange*>(&rc));
}

void Realizer::AddChangeFront(_In_ IRealizerChange& rc)
{
    _localChanges.PushFront(static_cast<LocalChange*>(&rc));
}

void Realizer::InsertNewLocalChangeAfterFrontChange(_In_ IRealizerChange& rc)
{
    LocalChange* pLC = static_cast<LocalChange*>(&rc);
    // save current front LC
    LocalChange* pFrontLC = nullptr;
    if (!_localChanges.Empty())
    {
        pFrontLC = _localChanges.Front();
    }

    // push new local changes to the front of the queue
    _localChanges.PushFront(pLC);

    // if the queue was not empty before this operation, remove previous front LC from the queue and re-add it to the front;
    // in this way, the front LC will still be a front, and the new LC will be in the queue immediately afterwards
    if (pFrontLC != nullptr)
    {
        _localChanges.RemoveAndRelease(pFrontLC);
        _localChanges.PushFront(pFrontLC);
    }
}

se::list<LocalChange*> Realizer::GetLocalChanges(_In_ const ResourceID& resourceID)
{
    return _localChanges.GetLocalChanges(resourceID);
}

se::list<LocalChange*> const Realizer::GetLocalChanges(_In_ ResourceID const& id, _In_ ResourceID const& scopeId) const
{
    return _localChanges.GetLocalChanges(id, scopeId);
}

se::list<std::unique_ptr<LocalChange>>& Realizer::GetLocalChanges()
{
    return _localChanges.GetLocalChanges();
}

LocalChange* Realizer::GetFirstChange() const
{
    return _localChanges.Front();
}

se::list<LocalChange*> const Realizer::GetLocalChangesOfType(_In_ ResourceID const& id, _In_ ResourceID const& scopeId, _In_ int changeType) const
{
    return _localChanges.GetLocalChangesOfType(id, scopeId, changeType);
}

LocalChange* Realizer::GetLastLocalChangeByIDAndType(_In_ const ResourceID& id, _In_ const ResourceID& scopeId, _In_ int changeType) const
{
    return _localChanges.GetLastLocalChangeByIDAndType(id, scopeId, changeType);
}

unsigned int Realizer::CountChangesInScopeOfType(_In_ const ResourceID& scopeId, _In_ int lcType) const
{
    return _localChanges.CountChangesInScopeOfType(scopeId, lcType);
}

std::shared_ptr<IWorkerThread>& Realizer::GetAsyncWorkPool()
{
    return _asyncWorkPool;
}

std::unique_ptr<IRealizerChangeInterpreter>& Realizer::GetRealizerChangeInterpreter()
{
    return _spRealizerChangeInterpreter;
}

/// <summary>
/// Returns true if a scan is pending on a local change completion. This check is an estimate
/// for performance optimization i.e. it is not exact and can return a false positive if there
/// are too many changes.
/// </summary>
bool Realizer::IsScanPendingOnChangeCompletionEstimate() const
{
    return (_pLocalPostponedChanges->IsScanPendingOnPostponedChangeEstimate() ||
            _localChanges.IsScanPendingOnChangeCompletionEstimate());
}

/// <summary>
/// Gets the remaining amount of time, in milliseconds, of the current realizer time slice of core loop.
/// Returns 0 if it is currently the due time or past it.
/// LCs can call this to determine how long to wait on async work before it should return from Process().
/// </summary>
unsigned int Realizer::GetTimeRemainingInSlice() const
{
    return _lcProcessingTimer.GetMSToTimeout();
}

// High Priority Queue Methods
void Realizer::ProcessHighPriorityChanges()
{
    unsigned int start = getTimerVal();
    DriveInfo* pDrive = DriveInfo::GetSkyDriveDrive();
    if (pDrive == nullptr)
    {
        return;
    }

    OnStartLCProcessing();

    int status = 0;
    LocalChange* pLC = nullptr;

    while ((g_clientState.GetStopCore() == false) && !_activeHydrationQueue.Empty())
    {
        pLC = _activeHydrationQueue.Front();

        if (pLC != nullptr)
        {
            ScenarioTracking::TransferScope transferScope(_activeHydrationQueue.GetScenarioTraceForLocalChange(pLC));

            const se::set<ResourceID> emptyScopeIdsList;
            status = ProcessLC(pLC, emptyScopeIdsList);

            if (status)
            {
                // Update sync status after removing change to process
                pDrive->SetTransferSyncStatusChanged();

                // Delete the processed LC and remove it from the queue 
                _activeHydrationQueue.PopFront();
            }
            else if (pLC->IsPostponed())
            {
                _activeHydrationQueue.RemoveAndRelease(pLC);
                _pLocalPostponedChanges->PostponeChange(pDrive, pLC);
            }
            else
            {
                break;
            }

            if (ShouldTimeoutLCProcessing(start))
            {
                break;
            }
        }
    }
}

bool Realizer::HasHighPriorityChanges() const
{
    return !_activeHydrationQueue.Empty();
}

unsigned int Realizer::HighPriorityChangesCount() const
{
    return _activeHydrationQueue.Size();
}

/// <summary>
/// There are some specific requirements that must be met in order for a RealizerChange to execute as a High Priority change.
/// The requirements list might change, but for now we assume the following must be true:
///    1. The RealizerChange must be able to run in any state of the ScanState machine (handleScanState).
///       This means that is may run in between time slices for the Scanner, the Realizer, Upload, etc.
///    2. The RealizerChange may not create new RealizerChanges to push in front of it on the queue.
///    3. The RealizerChange may not be postponed inside the PriorityChange processing
/// Currently there is only one RealizerChange type that is known to qualify for this queue and it is desirable to have run
/// in this queue: LC_HYDRATE_PLACEHOLDER. When considering adding a new type, we must evaluate all of the requirements to
/// make sure the new type meets them and also consider whether or not there are additional requirements that need to be imposed.
/// </summary>
void Realizer::AddPriorityChange(_In_ IRealizerChange& rc)
{
    if (rc.ChangeType() == LC_HYDRATE_PLACEHOLDER &&
        GetUpdateRingSettingsManager()->IsRampEnabled(UpdateRingSettings::Ramp::EnableActiveHydrationPriorityQueue))
    {
        // Since the ramp is on, add to the highpriority queue. Once added, this will progress through the high priority queue
        // regardless of ramp settings. Ramp controls will not be in place or evaluated at other stages of the pipeline. This
        // is the only point of control to ramp on/off the high priority pipeline.
        _activeHydrationQueue.PushBack(static_cast<LocalChange*>(&rc));

        LogRealizerAddHighPriorityChange(_activeHydrationQueue.Size());
    }
    else
    {
        AddChange(rc);
    }
}

LocalChange* Realizer::GetFirstHighPriorityChange() const
{
    return _activeHydrationQueue.Front();
}

// PostponedChanges Queue Methods
bool Realizer::HasPostponedChanges() const
{
    return (_pLocalPostponedChanges->Count() > 0);
}

void Realizer::RetryPostponedChanges()
{
    _pLocalPostponedChanges->RetryPostponedChanges(_localChanges);
}

PostponedChanges* Realizer::GetPostponedChanges() 
{ 
    return _pLocalPostponedChanges;
}

time_t Realizer::GetLastPostponedChangesRetryTime() const
{ 
    return _pLocalPostponedChanges->GetLastPostponedChangesRetryTime();
}

void Realizer::SetLastPostponedChangesRetryTime(_In_ time_t retryTime)
{ 
    _pLocalPostponedChanges->SetLastPostponedChangesRetryTime(retryTime);
}

int Realizer::GetScanIdDuringLastPostponedChangesRetry() const
{ 
    return _scanIdDuringLastPostponedChangesRetry;
}

void Realizer::SetScanIdDuringLastPostponedChangesRetry(_In_ int scanId)
{ 
    _scanIdDuringLastPostponedChangesRetry = scanId; 
}

/// <summary>
/// Checks if the postponed changes should be retried, according to the following logic:
/// 1. If the realizer didn't not run yet, or there hasn't passed more than MinTimeRealizerCooldown since realizer's last run, return false -
/// This is done in order to let the underlying conditions that caused postponed to retry (like async calls).
/// 2. If more than MinTimePostponedChangesRetrySeconds has elapsed and there are changes in the queue, 
/// OR more than MinTimePostponedChangesRetrySeconds and there are PPFirst changes in the queue, return true.
/// 
/// In all TRUE cases, if we find that there are changes in the Realizer's local changes queue, we log and abort the postponed changes retry.
/// </summary>
bool Realizer::ShouldRetryPostponedChanges() const
{
    bool res = false;
    const time_t now = time(nullptr);

    const time_t realizerProcessingDelta = now - _lastTimeRealizerProcessingChanges;
    const time_t postponedChangesDelta = now - GetLastPostponedChangesRetryTime();

    const bool shouldRetryDueToRetryElapsed = ((postponedChangesDelta >= _pLocalPostponedChanges->GetMinTimePostponedChangesRetrySeconds()) && 
                                         (_pLocalPostponedChanges->Count() > 0));

    const bool shouldPassDueToRealizerCooldown = (realizerProcessingDelta < _pLocalPostponedChanges->GetMinTimeRealizerCooldown());

    const bool shouldRetryDueToFirstRetryElapsed = (postponedChangesDelta >= _pLocalPostponedChanges->GetMinTimePostponedChangesFirstRetrySeconds()) && 
               _pLocalPostponedChanges->HasFirstRunLCs();

    // Realizer must run at least once, and there must be at least MinTimeRealizerCooldown before we retry postponed changes.
    if ((_lastTimeRealizerProcessingChanges == 0) || shouldPassDueToRealizerCooldown)
    {
        res = false;
    }
    else if (shouldRetryDueToRetryElapsed || shouldRetryDueToFirstRetryElapsed)
    {
        res = true;
    }

    // If we determined that we should retry postponed changes, but there are pending LCs in the Realizer's local changes queue
    // we should log & abort the retry.
    // This restriction is to prevent weird ordering problems where we could get some dependent or trailing
    // changes processed before their postponed change ancestors. Under normal conditions these trailing
    // changes would be postponed because the handlers will discover that the ancestors are postponed.
    // If we allow the ancestors to be retried here, though, the trailing changes won't be postponed
    // and they will be processed too soon.
    if (res && HasChangesToProcess())
    {
        LogCoreTimeToRetryPostponedChangesPrevented(ChangesToProcessCount(), postponedChangesDelta);
        res = false;
    }

    return res;
}

/// <summary>
/// Free pending changes in all priority queues. If the scope state is not specified then remove all LCs.
/// Otherwise remove only the LCs that belong to the specified scope.
/// </summary>
void Realizer::FreeLocalChangesByScope(_In_opt_ ResourceID* pScopeID)
{
    FreeLocalChangesByScopeFromQueue(_localChanges, pScopeID);
    FreeLocalChangesByScopeFromQueue(_activeHydrationQueue, pScopeID);
}

void Realizer::FreeLocalChangesByScopeFromQueue(_Inout_ LocalChangesContainer& container, _In_opt_ ResourceID* pScopeID)
{
    // We need to use an se::list for changesToRemove so that we can maintain the same order of the original localChanges list.
    // If we use an unordered_set, we may end up deleting a dependent child before its dependency, which will work but is less efficient.
    se::list<LocalChange const*> changesToRemove;
    se::list<std::unique_ptr<LocalChange>> const &localChangeSet = container.GetLocalChanges();

    LocalChange* currentlyProcessingLC = _currentProcessedLC;

    std::for_each(localChangeSet.begin(), localChangeSet.end(), [&](std::unique_ptr<LocalChange> const& pLC)
    {
        // LC_ALL_LIBS_SYNCED does not have its parentScopeID set anywhere (since we never use that ID).
        // So we skip the check on the parentScopeID for this local change here and just remove it from the list.
        if (pLC->changeType == LC_ALL_LIBS_SYNCED)
        {
            changesToRemove.push_back(pLC.get());
        }
        // Don't free RemoveLib LCs if we are removing LCs for a specific scope
        else if ((pScopeID != nullptr) && (pLC->changeType == LC_REMOVE_LIB))
        {
            LogFreeLocalChangesByScopeFromQueueSkippingRemoveLib(*pScopeID);
        }
        // Don't free the LC that invoked the call for this method
        else if (pLC.get() != currentlyProcessingLC)
        {
            nfODAssertEx((!pLC->HasScopeID() || !pLC->parentScopeID.IsEmpty()), Asserts::ClientCore::Drive4,
                         L"Parent scope id is missing from local change, type: %d", pLC->changeType);

            if ((pScopeID == nullptr) || (!pLC->HasScopeID()) || (pLC->parentScopeID == *pScopeID))
            {
                changesToRemove.push_back(pLC.get());
                if (pLC->changeType == LC_REPLACE_FILE)
                {
                    // any pending ReplaceFile LCs need to be cancelled using FileFetchFailed or the download stats will leak and never get cleaned up
                    ReplaceFileLC* pRF = static_cast<ReplaceFileLC*>(pLC.get());
                    pRF->FileFetchFailed(HYDRATION_STATUS_VALIDATIONFAILED, DownloadAbortReason::ScopeRemoved);
                }
                else if (pLC->changeType == LC_SYNC_COMPLETE)
                {
                    // for a pending SyncComplete LC we need to decrement sync complete count,
                    // otherwise it will not reach zero and it may prevent execution of initial-sync-completed logic
                    SyncComplete* pSC = static_cast<SyncComplete*>(pLC.get());
                    _pDrive->DecrementSyncCompleteCount(*pSC);
                }
            }
        }
    });

    std::for_each(changesToRemove.begin(), changesToRemove.end(), [&container](LocalChange const *pLCToRemove)
    {
        container.Remove(pLCToRemove);
    });
}

void Realizer::FreeAllLocalAndPostponedChanges()
{
    _pLocalPostponedChanges->FreeChanges(*_pDrive, nullptr);
    FreeLocalChangesByScope(nullptr);
}

void Realizer::PruneReplaceFileChanges(_In_ const ResourceID& resourceID, _In_ PCWSTR newTempFilePath)
{
    se::list<LocalChange*> localChangeSet = _localChanges.GetLocalChanges(resourceID);

    std::for_each(localChangeSet.begin(), localChangeSet.end(), [&](_In_ LocalChange* pLC)
    {
        if (pLC->changeType == LC_REPLACE_FILE)
        {
            CleanUpReplaceFile(pLC, newTempFilePath);
            _localChanges.Remove(pLC);
        }
    });
}

/// <summary>
/// This helper is called when a sub-scope is being unmounted and its LCs have to be dropped. All LCs for the specified 
/// sub-scope will be dropped except LC_SYNC_COMPLETE which is needed to complete initial sync.
/// </summary>
void Realizer::FreeLocalChangesOnSubScopeUnMount(_In_ const ScopeState& scopeState)
{
    ODAssert(scopeState.scopeType == SyncScopeType::SubScope, Asserts::ClientCore::Drive5);

    DriveInfo* pDrive = DriveInfo::GetSkyDriveDrive();

    if (pDrive == nullptr)
    {
        return;
    }

    // We need to use an se::list for changeToRemove so that we can maintain the same order of the original localChanges list.
    // If we use an unordered_set, we may end up deleting a dependent child before its dependency, which will work but is less efficient.
    se::list<LocalChange const*> changesToRemove;
    se::list<std::unique_ptr<LocalChange>> const &localChangeSet = _localChanges.GetLocalChanges();
    UINT32 numberOfLCsToRemove = 0;
    std::for_each(localChangeSet.begin(), localChangeSet.end(), [&](std::unique_ptr<LocalChange> const &pLC)
    {
        nfODAssertEx(!pLC->parentScopeID.IsEmpty(), Asserts::ClientCore::Drive6, L"Parent scope id is missing from local change, type: %d", pLC->changeType);
        if ((pLC->driveID == pDrive->driveID) && (pLC->parentScopeID == scopeState.scopeId) && (pLC->changeType != LC_SYNC_COMPLETE))
        {
            changesToRemove.push_back(pLC.get());
            if (pLC->changeType == LC_REPLACE_FILE)
            {
                // any pending ReplaceFile LCs need to be cancelled using FileFetchFailed or the download stats will leak and never get cleaned up
                ReplaceFileLC* pRF = static_cast<ReplaceFileLC*>(pLC.get());
                pRF->FileFetchFailed(HYDRATION_STATUS_VALIDATIONFAILED, DownloadAbortReason::SubscopeUnmount);
            }

            numberOfLCsToRemove++;
        }
    });

    LogFreeLocalChangesOnSubScopeUnMount(numberOfLCsToRemove);
    std::for_each(changesToRemove.begin(), changesToRemove.end(), [&](LocalChange const *pLCToRemove)
    {
        _localChanges.Remove(pLCToRemove);
    });
}

bool Realizer::IsLocalChangeHashing()
{
    const bool found = std::any_of(_localChanges.GetLocalChanges().begin(), _localChanges.GetLocalChanges().end(),
                                   [](_In_ std::unique_ptr<LocalChange> const& pLC)
                                   {
                                       return (pLC->changeType == LC_HASH);
                                   });

    return found;
}

bool Realizer::IsHashing()
{
    LocalChange *pLC = _localChanges.Front();
    if (pLC->changeType == LC_HASH)
    {
        LocalChangeHash* pLCH = static_cast<LocalChangeHash*>(pLC);
        if (pLCH->hashInfo.GetCurrentResult() == HashResult::Working)
        {
            return true;
        }
    }
    return false;
}

/// <summary>
/// Get the name and folderID of the given item as they would be if realized.
/// Do this by walking the list of LCs and finding the final CREATE or
/// MOVE in the list with this resourceID and scopeID and returning its values.
/// </summary>
_Success_(return) bool Realizer::GetAppliedNameAndParentID(_In_ ResourceID const& resourceID, _In_ ResourceID const& scopeID, _Inout_z_ wchar_t* name, _Out_ ResourceID& parentResourceID)
{
    ODAssert(name != nullptr, Asserts::ClientCore::LocalChanges181);
    ODAssert(!scopeID.IsEmpty(), Asserts::ClientCore::LocalChanges182);

    LocalChange* pFoundLC = nullptr;

    // Iterate through the change list and find the LAST create or move of this file so we can
    // take its parentID and filename.
    se::list<LocalChange*> localChanges = _localChanges.GetLocalChanges(resourceID, scopeID);
    auto it = std::find_if(localChanges.rbegin(), localChanges.rend(), [&](LocalChange *pLC)
    {
        return (pLC->IsCreate() || pLC->IsMoveRename());
    });

    if (it != localChanges.rend())
    {
        pFoundLC = *it;
    }

    if (pFoundLC == nullptr)
    {
        // We never found this change.
        return false;
    }

    parentResourceID = pFoundLC->parentResourceID;
    StringCopy(name, ONEDRIVE_CLIENT_MAX_PATH, pFoundLC->Name());

    return true;
}

/// <summary>
/// Get the filename and folderID of the given fileID as they would be if realized.
/// Do this by walking the list of LCs and finding the final LC_CREATE_FILE or
/// LC_MOVE_FILE in the list with this fileID and scopeID and returning its values.
/// This function mirrors the functionality of PostponedChanges::GetAppliedFileNameAndParentID.
/// </summary>
bool Realizer::GetAppliedFileNameAndParentID(_In_ const ResourceID& resourceID, _In_ const ResourceID& scopeID, _Inout_z_ wchar_t* fileName, _Out_ ResourceID& parentResourceID)
{
    return GetAppliedNameAndParentID(resourceID, scopeID, fileName, parentResourceID);
}

/// <summary>
/// Get the folderName and parentID of the given folderID as they would be if realized.
/// Do this by walking the list of LCs and finding the final LC_CREATE_FOLDER or
/// LC_MOVE_FOLDER in the list with this folderID and scopeID and returning its values.
/// This function mirrors the functionality of PostponedChanges::GetAppliedFolderNameAndParentID.
/// </summary>
bool Realizer::GetAppliedFolderNameAndParentID(_In_ const ResourceID& resourceID, _In_ const ResourceID& scopeID, _Inout_z_ wchar_t* folderName, _Out_ ResourceID& parentResourceID)
{
    return GetAppliedNameAndParentID(resourceID, scopeID, folderName, parentResourceID);
}

/// <summary>
/// Get the entry name and parentID of an entry. If this is a postponed change, search the
/// postponed changes class for these values, otherwise, search the LC list for these values, limiting
/// results to the correct/matching subScopeID.
/// </summary>
bool Realizer::GetCurrentNameAndParentID(_In_ const ResourceID& resourceID, _In_ bool isFolder, _In_ const ResourceID& subScopeID, _Inout_z_ wchar_t* entryName, _Out_ ResourceID& parentResourceID)
{
const bool postponedChange = _pLocalPostponedChanges->IsPostponed(_pDrive, resourceID, subScopeID);
    bool lookupSucceeded = false;

    // In theory there should not be any local changes in the localChanges list for an item that is postponed.
    // This is because when an item is postponed all trailing changes of the same resourceID will also be postponed when
    // they are encountered.
    if (postponedChange)
    {
        lookupSucceeded = isFolder ?
            _pLocalPostponedChanges->GetAppliedFolderNameAndParentID(resourceID, subScopeID, entryName, parentResourceID) :
            _pLocalPostponedChanges->GetAppliedFileNameAndParentID(resourceID, subScopeID, entryName, parentResourceID);
    }
    else
    {
        lookupSucceeded = GetAppliedNameAndParentID(resourceID, subScopeID, entryName, parentResourceID);
    }

    return lookupSucceeded;
}

/// <summary>
/// Returns true if all items in the queues have the flags set; returns false otherwise.
/// </summary>
bool Realizer::CheckAllQueuedChangesForFlags(IRealizerFlags flags) const
{
    LocalChangeFlags::Flags lcFlags = static_cast<LocalChangeFlags::Flags>(flags);
    return (_localChanges.CheckAllQueuedLocalChangesForFlags(lcFlags) && _activeHydrationQueue.CheckAllQueuedLocalChangesForFlags(lcFlags));
}

/// <summary>
/// Finds the local changes in the Realizer's postponed changes queue and local changes queue (in that order) that matches the 
/// provided resource and scope IDs and the isMatch condition.
/// Return a list of all matching changes.
/// </summary>
se::list<LocalChange*> Realizer::FindPendingLocalChanges(
    _In_ ResourceID& resourceID, 
    _In_ ResourceID& scopeID, 
    _In_ const std::function<bool(LocalChange*)>& isMatch) const
{
    se::list<LocalChange*> result;

    // Get the LC for the item from postponed changes
    for (const auto& pPostponed : _pLocalPostponedChanges->GetAllChanges())
    {
        if ((pPostponed->resourceID == resourceID) && isMatch(pPostponed))
        {
            result.push_back(pPostponed);
        }
    }

    // If pLC is still null, it could have been readded to LocalChanges
    se::list<LocalChange*> changes = GetLocalChanges(resourceID, scopeID);
    for (const auto& change : changes)
    {
        if (isMatch(change))
        {
            result.push_back(change);
        }
    }

    return result;
}

/// <summary>
/// Checks if the LC can be processed by ensuring the dependency validation passes if the change
/// enumeration used the flat format.
/// </summary>
/// <returns>true if the change be dropped due to validation failure. False otherwise.</returns>
bool Realizer::ShouldChangeBeDropped(_In_ LocalChange* pLC)
{
    if (pLC->enumerationContext.UsingFlatFormat())
    {
        LocalChangeValidationContext context(LocalChangeValidationSource::Realizer);

        if (!pLC->Validate(context))
        {
            SyncTelemetryHelper::RecordChangeValidationFailure(*pLC, context);

            if (context.shouldDropChange)
            {
                LogDroppingChangeDueToValidationFailure(pLC->resourceID, pLC->changeType, static_cast<int>(context.lastProblem));
                return true;
            }
        }
    }

    return false;
}

LocalChange* Realizer::GetLastChange()
{
    return _localChanges.Back();
}

void Realizer::RemoveFirstChange()
{
    _localChanges.PopFront();
}

LocalChange* Realizer::GetLastHighPriorityChange()
{
    return _activeHydrationQueue.Back();
}

void Realizer::RemoveFirstHighPriorityChange()
{
    _activeHydrationQueue.PopFront();
}

bool Realizer::RemoveAndReleaseChange(_In_ LocalChange* pLC)
{
    return _localChanges.RemoveAndRelease(pLC);
}

/// <summary>
/// Releases individual elements in _localChanges, followed by calling RemoveAllChanges.
/// </summary>
void Realizer::ReleaseAllChanges()
{
    _localChanges.ReleaseAll();
}

void Realizer::RemoveAllChanges()
{
    _localChanges.RemoveAll();
}

/// <summary>
/// Add the set of ordered changes to the front of the local changes queue. It is important to maintain
/// the ordering of the items. The input list is first reversed and then each item is added to the front
/// of the local changes queue so that the order is maintained.
/// </summary>
void Realizer::MoveOrderedChangesToFrontOfLocalChanges(_Inout_ OrderedLocalChangesEx& changes)
{
    SingleList<LocalChange> tempList;
    initSingleList(&tempList);
    changes.AppendOrderedChanges(tempList);

    SingleList<LocalChange> reverseOrderedList;
    initSingleList(&reverseOrderedList);
    SingleListIteratorT<LocalChange> tempLi(&tempList);
    while (tempLi.Advance())
    {
        addFirst(&reverseOrderedList, tempLi.GetCur());
    }
    freeSingleList(&tempList);

    SingleListIteratorT<LocalChange> reversedLi(&reverseOrderedList);
    while (reversedLi.Advance())
    {
        _localChanges.PushFront(reversedLi.GetCur());
    }
    freeSingleList(&reverseOrderedList);
}

unsigned int Realizer::CountChangesInScope(_In_ ResourceID const& scopeId) const
{
    return _localChanges.CountChangesInScope(scopeId);
}

void Realizer::SetTestMode()
{
    _allowLCTimeout = false;
}

/// <summary>
/// Marks the beginning of a realizer time slice of core loop.
/// Does everything needed before LCs can be processed.
/// </summary>
void Realizer::OnStartLCProcessing()
{
    // In the initial sync case, let the core loop process as many LCs as possible, esp. when Placeholders are enabled.
    // The time limit is set at 430ms to account for time slicing and allowing core message entries to be processed as
    // soon as possible, while giving the client a good slice of time to chew thru the LC queue.
    // Each core loop iteration should come back well within 500ms to allow for sufficient responsiveness to UX or external
    // requests so 430ms is picked.
    const unsigned int timesliceDuration = ((_pDrive->waitingForInitialSync) && g_pPlaceholderManager->ArePlaceholdersEnabled())
        ? MAX_TIME_HANDLING_LOCAL_CHANGES_MS_INITIALSYNC
        : MAX_TIME_HANDLING_LOCAL_CHANGES_MS;

    _lcProcessingTimer.Reset(timesliceDuration);
}

/// <summary>
/// Helper for handleLocalChanges to determine how long it can continue processing LCs. Under certain conditions,
/// we want the LCs to get as much of an opportunity to finish processing as possible, so the timeout for this
/// is increased. In most other cases, we want the core loop to be as responsive to external requests as possible,
/// while not letting CPU usage go very high. The function encapsulates the logic & ramps to determine this.
/// </summary>
bool Realizer::ShouldTimeoutLCProcessing(const unsigned int startTime)
{
    return (_allowLCTimeout && _lcProcessingTimer.HasTimedOut());
}

/// <summary>
/// Returns true, if the local change belong to a LibScope/LibScope with LibFolder scopes which are not ready.
/// Note: This should be called before adjusting the scope Ids for LibFolderScopes.
/// </summmary>
bool Realizer::IsLCInNotReadyScope(_In_ const LocalChange& localChange, _In_ const se::set<ResourceID>& topLevelScopesNotReady)
{
    return (!localChange.parentScopeID.IsEmpty() &&
        (topLevelScopesNotReady.find(localChange.parentScopeID) != topLevelScopesNotReady.end()));
}

/// <summary>
/// This function is to be called in the case where a LocalChange determines it needs to be replaced/converted into another type.
/// The function will drive the LocalChange to produce a new LC of the new type.
/// This new LC will inherit the postponed state of the original LC. This is important because if the original
/// LC has been postponed in the past it is no longer protected against loss by the SyncToken so the new LC
/// needs to be written as postponed in order to save the state into the DB.
/// Once this is completed, the new LC will replace the original LC in the LocalChanges queue.
/// </summary>
void Realizer::ConvertLCInPlace(_Inout_ LocalChange* pLC)
{
    ODAssert(ThreadType::GetThreadType() == ThreadType::CoreLoop, Asserts::ClientCore::LocalChanges4);

    // create the new one
    LocalChange* pNewLC = pLC->ConvertToAnotherLCType(*_pDrive);

    if (pNewLC != nullptr)
    {
        // if the original was postponed before, persist the new one
        if ((pLC->postponed.count > 0) && (pLC->IsPersistable() && (pNewLC->IsPersistable())))
        {
            PostponedChanges::TransferPostponedStatus(*pLC, *pNewLC);
        }
        // pop the original from the front
        ODAssert(_localChanges.Front() == pLC, Asserts::ClientCore::LocalChanges5);
        _localChanges.PopFront();
        // push the new one to the front of the queue
        LogInPlaceConversionMovedNewChangeToTheFrontOfTheLCQueue(pNewLC->changeType, pNewLC->resourceID);
        _localChanges.PushFront(pNewLC);
    }
}

/// <summary>
/// Determine the correct parentScopeID for the given local change.
/// A local change in a teamsite will receive the LibScope parentScopeID if it's generated from the
/// RealizerChangeInterpreter, as that's the scope(state) used during ChangeEnumeration, even if its
/// actual parent scope corresponding to the LC's client item is a LibFolderScope. Check for this
/// (and various similar scenarios related to team site) and perform the correction if needed.
/// 
/// This function needs to be run each time an LC is processed, as the LibFolderScope can be mounted and unmounted
/// without clearing the local change queues. So if a local change is postponed, when it is processed again, the
/// parentScopeID might need to be re-adjusted. This means the parentScopeID can be transient.
/// </summary>
void Realizer::AdjustParentScope(_Inout_ LocalChange& lc)
{
    // If a new LC can come from ChangeEnumeration, and can occur as part of teamsite sync relationship,
    // add it to the check below.
    static_assert(LC_MAX_VALUE == LC_MIGRATION_COMPLETE);

    if ((lc.changeType == LC_CREATE_FOLDER) || (lc.changeType == LC_CREATE_FILE) ||
        (lc.changeType == LC_DELETE_FOLDER) || (lc.changeType == LC_DELETE_FILE) ||
        (lc.changeType == LC_MOVE_FOLDER) || (lc.changeType == LC_MOVE_FILE) ||
        (lc.changeType == LC_CHANGE_FOLDER) || (lc.changeType == LC_CHANGE_FILE) ||
        (lc.changeType == LC_REPLACE_FILE) ||
        (lc.changeType == LC_FILE_PRE_CONFLICT) ||
        (lc.changeType == LC_UNDO_DRIVE_CHANGE) ||
        (lc.changeType == LC_CONVERT_ONENOTE) ||
        (lc.changeType == LC_MIGRATE_FOLDER) || (lc.changeType == LC_MIGRATE_FILE))
    {
        ODAssert(!lc.parentResourceID.IsEmpty(), Asserts::ClientCore::LocalChanges261);

        // There're three scenarios where the parentScopeID could be incorrect (only applicable to teamsite sync)
        // 1) The parentScopeID is the scopeID of the LibScope while it should be that of a child LibFolderScope:
        //      For LCs for items under the said LibFolderScope that came from SyncServiceProxy, this is expected,
        //      as SSP assigned the scopeID of the scopeState used in ChangeEnumeration to the LC.
        //      This can also happen if the LC is postponed and a LibFolderScope is mounted above the LC's item
        //      before the LC is reprocessed.
        // 2) The parentScopeID is the scopeID of a LibFolderScope while it should be that of the parent LibScope.
        //      This is opposite of scenario 1, which happens if the LC is postponed and the LibFolderScope above
        //      the LC's item is unmounted before the LC is reprocessed.
        // 3) The parentScopeID is the scopeID of a LibFolderScope while it should be that of a different LibFolderScope.
        //      This is special cases of scenario 1 and 2. While LibFolderScopes cannot be nested (they cannot have
        //      child scopes), the client items they're associated to can form ancestor/descendant relationship due
        //      to gap folders (permission to item and its grandchild but not its direct child). Similar to previous
        //      scenarios, when one of the LibFolderScopes are transitioned between mounted/unmounted,
        //      parentScopeIDs need to be updated.

        ClientFolder* pParentFolder = _pDrive->GetFolder(lc.parentResourceID);
        ScopeInfo* pCurrentScope = _pDrive->GetScopeNotMatchingFlags(lc.parentScopeID, SyncScopeFlags::Removed);
        ScopeInfo* pParentAsScope = _pDrive->GetScopeNotMatchingFlags(lc.parentResourceID, SyncScopeFlags::Removed);

        // If the scope is not related to teamsite sync, the previously discussed cases are not relevant.
        // For these LCs, the assigned parent scopeID should already be correct from ChangeEnumeration.
        // We can still perform the correction logic if the scope is unknown since the logic in the individual branches
        // can be generalized to those LCs.
        bool correctScopeType = ((pCurrentScope == nullptr) ||
            (pCurrentScope->GetScopeType() == SyncScopeType::FolderScope) ||
            ((pCurrentScope->GetScopeType() == SyncScopeType::LibScope) && pCurrentScope->IsTeamSiteScope(pCurrentScope->GetLibraryType())));

        if (correctScopeType)
        {
            if (_pDrive->pRealizer->GetPostponedChanges()->IsCreatePostponed(_pDrive, lc.parentResourceID, lc.parentScopeID))
            {
                // If the parent create is postponed, this LC will be postponed. After the parent is processed,
                // we will retry this and find the correct parentScopeID at that time.
                ODAssert(!lc.parentScopeID.IsEmpty(), Asserts::ClientCore::LocalChanges262);
            }
            else if (pParentAsScope != nullptr)
            {
                // This covers the case where a scope boundary is crossed and the parent of this clientItem
                // is a scope. In this case, since the parent clientItem has its parentScopeID set to the
                // scope above it and not the scope associated with it, its parentScopeID would be incorrect
                // for this LC.
                // Use the resourceID so that the invariant (parentScopeID = resourceID of nearest ancestor
                // scope in clientItem tree) is preserved.
                // This also covers LC for top level item (parent is not a ClientItem), as it must be a direct child
                // of a scope.
                lc.parentScopeID = lc.parentResourceID;
            }
            else if (pParentFolder == nullptr)
            {
                // The parent folder must have been deleted, as postponed create and top-level items have been
                // ruled out. We leave the previously assigned scopeID in place, as no further information can be
                // gathered (parent's not a scope and we don't know about parent folder).
                // This covers a case where the tombstone of an item is sent twice. If the parent is removed (and
                // so is this item), we leave the scopeID unchaged (so it refers to a valid scope), and when
                // the remove LC is processed it will correctly no-op.
            }
            else
            {
                // The parent folder exists but is not associated to a scope. This LC is a non-direct descendant
                // of a scope, and its parent folder will have the current parentScopeID. This is guaranteed to
                // not cross a scope boundary, as a previous branch covers the scenario.
                lc.parentScopeID = pParentFolder->parentScopeID;
            }
        }
    }
}

/// <summary>
/// This function is the handler routing function in the Realizer. Given a LocalChange by handleLocalChanges, it invokes the correct handler method and returns the resulting value
/// back to handleLocalChanges.
/// </summary>
int Realizer::ProcessLC(_In_ LocalChange* pLC, _In_ const se::set<ResourceID>& topLevelScopesNotReady)
{
    ClientPerfCounters::timers.realizer.Start();
    
    ETWLogEvent(&RealizingStart);

    if (_processLCHook != nullptr)
    {
        _processLCHook(pLC);
    }

    // At this point the parentScopeIds for the LC`s will always be for LibScopes on Business.
    // Also the top level scopes will also be only populated for Business
    if (!pLC->CanProcessWhenScopesNotReady() && IsLCInNotReadyScope(*pLC, topLevelScopesNotReady))
    {
        LogLocalChangesConsumeChangeDueToScopeNotBeingReady(pLC->parentScopeID, pLC->resourceID, pLC->changeType);
        return TRUE;
    }

    // Since ChangeEnum is always done with libscope, and libfolderscope can mount/unmount without the corresponding 
    // LCs being updated, make corrections before processing LC.
    AdjustParentScope(*pLC);

    pLC->LogInfo();

    ScopeInfo* pScope = nullptr;
    DriveInfo* pDrive = findMountedDrive(pLC->driveID);
    if ((pDrive != nullptr) && pLC->HasScopeID())
    {
        pScope = pDrive->GetScope(pLC->parentScopeID);
        if ((pLC->changeType != LC_SYNC_COMPLETE) && // We don't want to drop SyncComplete, because we need to clear the waitingForInitialSync bool on the drive.
            ((pScope == nullptr) || (pScope->IsAnyScopeFlagSet(SyncScopeFlags::Removed))))
        {
            LogProcessLCForRemovedScope(pLC->parentScopeID);
            return TRUE;
        }
        else if (pLC->SupportsVaultItemType() && (pScope->GetScopeType() == SyncScopeType::VaultScope) &&
            !static_cast<VaultScopeInfo*>(pScope)->IsUnlocked(false /* considerTransitioningToUnlockedAsLocked */, false /* considerInErrorAsLocked */))
        {
            LogProcessLCForLockedVault(pLC->parentScopeID, pLC->resourceID);
            static_cast<VaultScopeInfo*>(pScope)->ScheduleResyncForPostponedChangesOnLockedVault(); // does nothing if resync is already scheduled
            return TRUE;
        }
    }

    if (ShouldChangeBeDropped(pLC))
    {
        return TRUE;
    }

    SyncPerftrack::phTransitionPerfStats.StartProcessLC(pLC);

    _currentProcessedLC = pLC;

    const bool status = pLC->Process();

    _currentProcessedLC = nullptr;
    SyncPerftrack::phTransitionPerfStats.StopProcessLC(pLC, status);

    pLC->SendProcessResultTelemetry(status);
    pLC->UpdateExecutionCounterAndSendTelemetry();

    if (status)
    {
        if (pDrive != nullptr)
        {
            if (pLC->changeType >= 0 && pLC->changeType <= LC_MAX_VALUE)
            {
                pDrive->numProcessedLocalChangesThisScanState_DO_NOT_USE++;
                pDrive->numProcessedLocalChanges[pLC->changeType]++;

                // Increment the count of the current LocalChangeMetadata for instrumenting file and folder usage  
                bool isSharedFolderItem = pDrive->IsSyncScopeRoot(pLC->parentScopeID, SyncScopeType::SubScope);
                StatusType lcStatus = g_pSyncHelperUtils->GetFileAndFolderStatus(pDrive, pLC->resourceID);
                LocalChangeMetadata lcMetadata(isSharedFolderItem, pLC->IsLocalChange(), lcStatus, pLC->changeType);
                pDrive->aggregatedLocalChangeMetadata[lcMetadata]++;
            }

            pLC->FullScanIfApplicable(pDrive);
        }
    }

    if (pDrive != nullptr)
    {
        if (pLC->IsPostponed())
        {
            pDrive->perfStats.IncrementPostponedCount(pLC->parentScopeID, pLC->changeType);
        }
        else
        {
            // For cloud files namespace creation, we track processed LCs that belong to other irrelevant
            // scopes (the scopes that are not going through initial sync).
            if (!status && (pLC->changeType == LC_CREATE_FILE))
            {
                CreateNewFile* pCNF = static_cast<CreateNewFile*>(pLC);
                // OneNote files created locally are created as .URL files, so the client generates a hash
                // for these items
                if (pCNF->isOneNote)
                {
                    pDrive->perfStats.SetIsOneNoteHashUpcoming();
                }
            }

            if (pLC->HasScopeID() && (pScope != nullptr)) // Protect against unmapped sync scopes, etc, just like the check above.
            {
                pDrive->perfStats.TrackChangesOnOtherScopes(*pScope, *pLC);
            }
        }
    }

    ETWLogEvent(&RealizingEnd);
    ClientPerfCounters::AddRealizerTimeElapsed(ClientPerfCounters::timers.realizer.Stop());

    pLC->lastProcessTime = time(nullptr);

    return status;
}

/// <summary>
/// Determine realizer is ready for allowing core to run idle jobs.
/// Idle jobs are allowed when there are not pending changes to process 
/// both in local changes and postponed changes queue. 
/// </summary>
bool Realizer::IsReadyForIdleJobs()
{
    return (!HasChangesToProcess() && !HasPostponedChanges());
}

/// <summary>
/// Queues a RepairReparseTagLC in the realizer's changes queue to try to fix the repair tag of the file in fullPath,
/// which is colliding with the file we're trying to create. 
/// NOTE: The calling LC MUST postpone itself after invoking this method. Failing to do so may end in an infinite cycle.
/// </summary>
void Realizer::TryRepairReparseTag(_In_ se::wstring& fullPath, _In_ LocalChange& lc, _In_ const FILTER_STATUS filterStatus) 
{
    const int retryMod = 1000;
    bool isInvalidReparseTagStatus = false;
#ifdef WIN32
    isInvalidReparseTagStatus = AreAllFlagsSet(filterStatus, ERROR_REPARSE_TAG_INVALID);
#endif

    if (GetUpdateRingSettingsManager()->IsRampEnabled(EnableTryRepairReparseTagOnCF) &&
        (lc.postponed.count % retryMod == 0) && 
        isInvalidReparseTagStatus)
    {
        FileSystemID fsID;
        fsID.ReadFS(fullPath.c_str());

        RepairPlaceholderTagLC* pRPHT = new RepairPlaceholderTagLC(lc.parentScopeID, lc.resourceID, fsID, fullPath);

        AddChangeFront(*pRPHT);

        TelemetryCreateFileTryRepairReparseTag(filterStatus, lc.Type());
    }
}
