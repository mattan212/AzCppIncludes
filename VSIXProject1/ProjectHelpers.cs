using System.Linq;
using Microsoft.VisualStudio.Text;

namespace VSIX
{
    public static class ProjectHelpers
    {
        public static ITextDocument GetCurrentFile(ITextSnapshot snapshot)
        {
            foreach (var property in snapshot.TextBuffer.Properties.PropertyList)
            {
                if (property.Value is ITextDocument)
                {
                    ITextDocument doc = (ITextDocument)property.Value;
                    return doc;
                }
            }

            return null;
        }

        public static int GetLineNumber(SnapshotSpan span)
        {
            var line = span.Snapshot.Lines.FirstOrDefault(x => x.Extent.Span.Start > span.End);

            return line?.LineNumber - 1 ?? span.Snapshot.Lines.Count() - 1;
        }

        public static ITextSnapshotLine GetLine(SnapshotSpan span)
        {
            var lines = span.Snapshot.Lines.ToList();
            var index = lines.FindIndex(x => x.Extent.Span.Start > span.End);
            
            return index > 0 ? span.Snapshot.Lines.ElementAt(index - 1) : span.Snapshot.Lines.Last();
        }
    }
}
