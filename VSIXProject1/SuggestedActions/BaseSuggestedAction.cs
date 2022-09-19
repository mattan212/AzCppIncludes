using System;
using System.Collections.Generic;
using System.Threading;
using System.Threading.Tasks;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Documents;
using Microsoft.VisualStudio.Imaging.Interop;
using Microsoft.VisualStudio.Language.Intellisense;
using Microsoft.VisualStudio.Text;
using Task = System.Threading.Tasks.Task;

namespace VSIX.SuggestedActions
{
    public abstract class BaseSuggestedAction : ISuggestedAction
    {
        protected readonly ITrackingSpan _span;
        protected readonly ITextSnapshot _snapshot;

        protected BaseSuggestedAction(ITrackingSpan span)
        {
            _span = span;
            _snapshot = span.TextBuffer.CurrentSnapshot;
        }

        public string IconAutomationText => null;
        public string InputGestureText => null;

        public virtual bool HasPreview => false;

        public ImageMoniker IconMoniker => default(ImageMoniker);

        public void Dispose()
        {
        }

        public bool TryGetTelemetryId(out Guid telemetryId)
        {
            telemetryId = Guid.Empty;
            return false;
        }

        public virtual Task<IEnumerable<SuggestedActionSet>> GetActionSetsAsync(CancellationToken cancellationToken) =>
            Task.FromResult<IEnumerable<SuggestedActionSet>>(null);

        public virtual Task<object> GetPreviewAsync(CancellationToken cancellationToken) => Task.FromResult<object>(null);

        public abstract void Invoke(CancellationToken cancellationToken);

        public virtual bool HasActionSets => false;
        public abstract string DisplayText { get; }

        protected TextBlock CreateTextBlock(double scale, string family, double horizontalPadding, double verticalPadding, string text)
        {
            var tb = new TextBlock();
            tb.FontSize *= scale;
            tb.FontFamily = new System.Windows.Media.FontFamily(family);
            tb.Inlines.Add(new Run(text));
            tb.Padding = new Thickness(horizontalPadding, verticalPadding, horizontalPadding, verticalPadding);
            return tb;
        }
    }
}