using Microsoft.VisualStudio.Text;
using Microsoft.VisualStudio.Text.Operations;
using Serilog;
using VSIXProject1;

namespace VSIX.Conditions
{
    public abstract class Condition
    {
        protected ILogger Logger;
        protected TextExtent _extent;
        protected ITextSnapshot _snapshot;

        public Condition(TextExtent extent, ITextSnapshot snapshot)
        {
            _extent = extent;
            _snapshot = snapshot;
            Logger = CppIncludesSuggestActionsSource.Logger;
        }

        public abstract bool Evaluate();
    }
}