using System;
using System.Collections.Generic;
using System.Threading;
using System.Threading.Tasks;
using System.Windows.Interop;
using Microsoft.VisualStudio.Imaging.Interop;
using Microsoft.VisualStudio.Language.Intellisense;
using Microsoft.VisualStudio.Shell;
using Microsoft.VisualStudio.Text;
using AzCppIncludesVsix.Xaml;
using Task = System.Threading.Tasks.Task;

namespace SharedProject.SuggestedActions
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

        protected void Notify(string header, string body)
        {
            var dialog = new NotificationDialog(header, body);

            ThreadHelper.ThrowIfNotOnUIThread();
#if Dev16
            var hwnd = new IntPtr(SuggestedActionsSource.Dte.MainWindow.HWnd);
#else
            var hwnd = SuggestedActionsSource.Dte.MainWindow.HWnd;
#endif
            var window = (System.Windows.Window)HwndSource.FromHwnd(hwnd).RootVisual;
            dialog.Owner = window;

            bool? result = dialog.ShowDialog();
        }
    }
}