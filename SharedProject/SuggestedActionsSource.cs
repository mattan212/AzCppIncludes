using System;
using System.Collections.Generic;
using System.ComponentModel.Composition;
using System.IO;
using System.Threading;
using System.Threading.Tasks;
using EnvDTE;
using Microsoft.VisualStudio.Language.Intellisense;
using Microsoft.VisualStudio.Shell;
using Microsoft.VisualStudio.Text;
using Microsoft.VisualStudio.Text.Editor;
using Microsoft.VisualStudio.Text.Operations;
using Microsoft.VisualStudio.Utilities;
using Serilog;
using SharedProject.Conditions;
using SharedProject.SuggestedActions;
using Task = System.Threading.Tasks.Task;

namespace SharedProject
{

    [Export(typeof(ISuggestedActionsSourceProvider))]
    [Name("Test Suggested Actions")]
    [ContentType("text")]
    public class TestSuggestedActionsSourceProvider : ISuggestedActionsSourceProvider
    {
        [Import]
        internal SVsServiceProvider ServiceProvider = null;

        [Import]
        ITextDocumentFactoryService DocumentFactory = null;

        [Import(typeof(ITextStructureNavigatorSelectorService))]
        internal ITextStructureNavigatorSelectorService NavigatorService { get; set; }

        public ISuggestedActionsSource CreateSuggestedActionsSource(ITextView textView, ITextBuffer textBuffer)
        {
            ThreadHelper.ThrowIfNotOnUIThread();
            DTE dte = (DTE)ServiceProvider.GetService(typeof(DTE));

            if (textBuffer == null || textView == null)
            {
                return null;
            }
            return new SuggestedActionsSource(this, textView, textBuffer, dte, DocumentFactory);
        }
    }

    internal class SuggestedActionsSource : ISuggestedActionsSource
    {
        private readonly TestSuggestedActionsSourceProvider _factory;
        private readonly ITextBuffer _textBuffer;
        private readonly ITextView _textView;
        public event EventHandler<EventArgs> SuggestedActionsChanged;
        public static ILogger Logger;
        public static DTE Dte;
        public static ITextDocumentFactoryService DocumentFactory;
        public static string LocalStorePath = Path.Combine(
            Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData),
            @"Microsoft\VisualStudio\Vsix\AzCppIncludes\");

        public SuggestedActionsSource(TestSuggestedActionsSourceProvider testSuggestedActionsSourceProvider,
            ITextView textView, ITextBuffer textBuffer, DTE dte, ITextDocumentFactoryService documentFactory)
        {
            _factory = testSuggestedActionsSourceProvider;
            _textBuffer = textBuffer;
            _textView = textView;
            Logger = CreateLogger();
            Dte = dte;
            DocumentFactory = documentFactory;
        }

        private ILogger CreateLogger()
        {
            var LOG_PATH = Path.Combine(LocalStorePath, "AzCppIncludesVsixLogs.txt");

            var FILE_SIZE_LIMIT = 1024 * 1024;
            var RETAINED_FILE_COUNT_LIMIT = 5;

            var logger = new LoggerConfiguration()
                .WriteTo.File(
                    LOG_PATH,
                    rollingInterval: RollingInterval.Day,
                    retainedFileCountLimit: RETAINED_FILE_COUNT_LIMIT,
                    fileSizeLimitBytes: FILE_SIZE_LIMIT)
                .CreateLogger();

            return logger;
        }

        private bool TryGetWordUnderCaret(out TextExtent wordExtent)
        {
            ITextCaret caret = _textView.Caret;
            SnapshotPoint point;

            if (caret.Position.BufferPosition > 0)
            {
                point = caret.Position.BufferPosition - 1;
            }
            else
            {
                wordExtent = default(TextExtent);
                return false;
            }

            ITextStructureNavigator navigator = _factory.NavigatorService.GetTextStructureNavigator(_textBuffer);

            wordExtent = navigator.GetExtentOfWord(point);

            return true;
        }

        private bool ExtractWordAndEvaluate(out TextExtent extent)
        {
            var res = false;
            if (TryGetWordUnderCaret(out extent))
            {
                var conditions = new List<Condition>
                {
                    new IncludeLineCondition(extent, extent.Span.Snapshot),
                };

                foreach (var condition in conditions)
                {
                    try
                    {
                        if (condition.Evaluate())
                        {
                            var lineNumber = ProjectHelpers.GetLineNumber(extent.Span);
                            var line = extent.Span.Snapshot.GetLineFromLineNumber(lineNumber).GetText();
                            Logger.Information("Extent {word} meets required condition for {condition}.",
                                line, condition.GetType());
                            res = true;
                        }
                    }
                    catch (Exception ex)
                    {
                        Logger.Error("SuggestedActionsSource::ExtractWordAndEvaluate caught an exception while evaulating {condition}. {ex}",
                            nameof(condition), ex);
                    }
                }
            }
            return res;
        }

        public Task<bool> HasSuggestedActionsAsync(ISuggestedActionCategorySet requestedActionCategories, SnapshotSpan range, CancellationToken cancellationToken)
        {
            return Task.Factory.StartNew(() =>
            {
                return ExtractWordAndEvaluate(out TextExtent extent);
            });
        }

        public IEnumerable<SuggestedActionSet> GetSuggestedActions(ISuggestedActionCategorySet requestedActionCategories, SnapshotSpan range, CancellationToken cancellationToken)
        {
            var actions = new List<ISuggestedAction>();

            if (TryGetWordUnderCaret(out TextExtent extent))
            {
                var trackingSpan = range.Snapshot.CreateTrackingSpan(extent.Span, SpanTrackingMode.EdgeInclusive);

                // Check whether meets the conditions to display interface AzCppIncludes
                Condition condition;
                
                condition = new IncludeLineCondition(extent, extent.Span.Snapshot);
                if (condition.Evaluate())
                {
                    var action = new AlphabetizeIncludesAction(trackingSpan);
                    actions.Add(action);
                }
            }

            return new[] { new SuggestedActionSet(actions) };
        }

        public bool TryGetTelemetryId(out Guid telemetryId)
        {
            // Don't participate in telemetry collection
            telemetryId = Guid.Empty;
            return false;
        }

        public void Dispose()
        {
        }
    }
}
