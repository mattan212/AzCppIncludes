using System.Linq;
using Microsoft.VisualStudio.Text;

namespace SharedProject
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
    }
}
