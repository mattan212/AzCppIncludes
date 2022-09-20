using System;
using System.Threading;
using AzCppIncludesConsole;
using Microsoft.VisualStudio.Language.Intellisense;
using Microsoft.VisualStudio.Text;

namespace SharedProject.SuggestedActions
{
    internal class AlphabetizeIncludesAction : BaseSuggestedAction, ISuggestedAction
    {
        AlphabetizeIncludesService _service; 

        public AlphabetizeIncludesAction(ITrackingSpan span) : base(span)
        {
            var configuration = new Configuration(SuggestedActionsSource.LocalStorePath);
            _service = new AlphabetizeIncludesService(configuration);
        }

        public override void Invoke(CancellationToken cancellationToken)
        {
            try
            {
                var filePath = ProjectHelpers.GetCurrentFile(_snapshot).FilePath;
                var result = _service.Run(filePath);
                SuggestedActionsSource.Logger.Information("Finsihed sorting in {filePath} with {result}", filePath, result.ToString());
            }
            catch (Exception e)
            {
                SuggestedActionsSource.Logger.Error("AlphabetizeIncludesAction Invoke exception: {@Exception}", e);
                Notify("whoops, something went wrong", e.Message);
            }
        }

        public override bool HasPreview => false;

        public override string DisplayText => "Sort #includes";

        public override bool HasActionSets => false;
    }
}