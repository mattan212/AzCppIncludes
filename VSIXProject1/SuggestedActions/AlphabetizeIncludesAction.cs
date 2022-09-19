using System;
using System.Threading;
using AzCppIncludesConsole;
using Microsoft.VisualStudio.Language.Intellisense;
using Microsoft.VisualStudio.Text;
using VSIX;
using VSIXProject1;
using Configuration = AzCppIncludesConsole.Configuration;

namespace VSIX.SuggestedActions
{
    internal class AlphabetizeIncludesAction : BaseSuggestedAction, ISuggestedAction
    {
        public AlphabetizeIncludesAction(ITrackingSpan span) : base(span)
        {
        }
        
        public override void Invoke(CancellationToken cancellationToken)
        {
            try
            {
                var service = new AlphabetizeIncludesService(new Configuration());
                var filePath = ProjectHelpers.GetCurrentFile(_snapshot).FilePath;
                service.Run(filePath);
            }
            catch (Exception e)
            {
                CppIncludesSuggestActionsSource.Logger.Error("AlphabetizeIncludesAction Invoke exception: {@Exception}", e);
            }

            //TelemetryManager.LogEvent(EEventType.SetDebuggerToUnitTest);
        }

        public override bool HasPreview => false;

        public override string DisplayText => "Sort #includes";

        public override bool HasActionSets => false;
    }
}