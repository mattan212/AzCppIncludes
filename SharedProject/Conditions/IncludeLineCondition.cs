using System.IO;
using Microsoft.VisualStudio.Text;
using Microsoft.VisualStudio.Text.Operations;

namespace SharedProject.Conditions
{
    public class IncludeLineCondition : Condition
    {
        public IncludeLineCondition(TextExtent extent, ITextSnapshot snapshot) : base(extent, snapshot)
        {
        }

        public override bool Evaluate()
        {
            try
            {
                // These conditions must be met:
                // 1. This is a h/cpp file. 
                // 2. line starts with '#include '
                var filePath = ProjectHelpers.GetCurrentFile(_snapshot).FilePath;

                var extension = Path.GetExtension(filePath).ToLower();

                var isValid = (extension == ".cpp") || (extension == ".h");

                var lineNumber = ProjectHelpers.GetLineNumber(_extent.Span);

                var line = _snapshot.GetLineFromLineNumber(lineNumber);

                var lineText = line.GetText();

                isValid &= lineText.StartsWith("#include ");

                return isValid;
            }
            catch
            {
                return false;
            }
        }
    }
}
