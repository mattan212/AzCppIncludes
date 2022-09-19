using Microsoft.VisualStudio.Language.Intellisense;
using Microsoft.VisualStudio.Text.Editor;
using Microsoft.VisualStudio.Text;
using Microsoft.VisualStudio.Text.Operations;
using Microsoft.VisualStudio.Utilities;
using System;
using System.Collections.Generic;
using System.ComponentModel.Composition;
using System.Linq;
using System.Text;
using System.Threading.Tasks;
using System.Threading;
using Microsoft.Build.Framework;
using Microsoft.Internal.VisualStudio.PlatformUI;
using Microsoft.VisualStudio.TextManager.Interop;
using System.IO;
using Serilog;
using ILogger = Serilog.ILogger;
using VSIX.Conditions;
using VSIX;
using VSIX.SuggestedActions;

namespace VSIXProject1
{
    [Export(typeof(ISuggestedActionsSourceProvider))]
    [Name("Test Suggested Actions")]
    [ContentType("text")]
    internal class TestSuggestedActionsSourceProvider : ISuggestedActionsSourceProvider
    {
        [Import(typeof(ITextStructureNavigatorSelectorService))]
        internal ITextStructureNavigatorSelectorService NavigatorService { get; set; }

        public ISuggestedActionsSource CreateSuggestedActionsSource(ITextView textView, ITextBuffer textBuffer)
        {
            if (textBuffer == null || textView == null)
            {
                return null;
            }
            return new CppIncludesSuggestActionsSource(this, textView, textBuffer);
        }
    }

    internal class CppIncludesSuggestActionsSource : ISuggestedActionsSource
    {
        private readonly TestSuggestedActionsSourceProvider _factory;
        private readonly ITextBuffer _textBuffer;
        private readonly ITextView _textView;
        public event EventHandler<EventArgs> SuggestedActionsChanged;

        public static ILogger Logger;

        public CppIncludesSuggestActionsSource(TestSuggestedActionsSourceProvider testSuggestedActionsSourceProvider, ITextView textView, ITextBuffer textBuffer)
        {
            _factory = testSuggestedActionsSourceProvider;
            _textBuffer = textBuffer;
            _textView = textView;

            Logger = CreateLogger();
        }

        private ILogger CreateLogger()
        {
            var LOG_PATH = Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData),
                @"Microsoft\VisualStudio\VsixLogs\AzCppIncludes.txt");

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
#pragma warning disable VSTHRD105 // Avoid method overloads that assume TaskScheduler.Current
            return Task.Factory.StartNew(() =>
            {
                return ExtractWordAndEvaluate(out TextExtent extent);
            });
#pragma warning restore VSTHRD105 // Avoid method overloads that assume TaskScheduler.Current
        }

        public IEnumerable<SuggestedActionSet> GetSuggestedActions(ISuggestedActionCategorySet requestedActionCategories, SnapshotSpan range, CancellationToken cancellationToken)
        {
            var actions = new List<ISuggestedAction>();

            if (TryGetWordUnderCaret(out TextExtent extent))
            {
                var trackingSpan = range.Snapshot.CreateTrackingSpan(extent.Span, SpanTrackingMode.EdgeInclusive);

                // Check whether meets the conditions to display interface lightbulb
                Condition condition;

                condition = new IncludeLineCondition(extent, extent.Span.Snapshot);
                if (condition.Evaluate())
                {
                    var action = new AlphabetizeIncludesAction(trackingSpan);
                    actions.Add(action);
                }
            }

            return new[] { new SuggestedActionSet("misc", actions) };
        }

        public void Dispose()
        {
        }

        public bool TryGetTelemetryId(out Guid telemetryId)
        {
            // This is a sample provider and doesn't participate in LightBulb telemetry
            telemetryId = Guid.Empty;
            return false;
        }
    }
}
