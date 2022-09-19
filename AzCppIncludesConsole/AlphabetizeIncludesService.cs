using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Text;

namespace AzCppIncludesConsole
{
    public enum EResult { Success, FileNotFound, FileReadError }

    public class AlphabetizeIncludesService
    {
        Configuration _configuration;

        public AlphabetizeIncludesService(Configuration configuration)
        {
            _configuration = configuration;
        }

        public EResult Run(string filePath)
        {
            if (!File.Exists(filePath))
            {
                return EResult.FileNotFound;
            }

            var encoding = GetEncoding(filePath);
            var lines = File.ReadAllLines(filePath);

            var indices = new List<int>();
            for (var i = 0; i < lines.Length; i++)
            {
                if (lines[i].StartsWith("#include"))
                {
                    indices.Add(i);
                }
                else if (lines[i].StartsWith("#ifdef") || lines[i].StartsWith("#elif"))
                {
                    var ifdefIndices = new List<int>();
                    for (var j = i + 1; j < lines.Length; j++)
                    {
                        if (lines[j].StartsWith("#endif") || lines[j].StartsWith("#elif"))
                        {
                            i = j - 1;
                            break;
                        }
                        else if (lines[j].StartsWith("#include"))
                        {
                            ifdefIndices.Add(j);
                        }
                    }
                    Sort(lines, ifdefIndices);
                }
            }
            Sort(lines, indices);

            File.WriteAllLines(filePath, lines, encoding);

            return EResult.Success;
        }

        private void Sort(string[] lines, List<int> indices)
        {
            var includes = new List<string>();

            foreach (var index in indices)
            {
                includes.Add(lines[index]);
            }

            var comparer = new IncludeComparer(_configuration);

            includes = includes.OrderBy(x => x, comparer).ToList();

            var i = 0;
            foreach (var index in indices)
            {
                lines[index] = includes[i++];
            }
        }

        /// <summary>
        /// Determines a text file's encoding by analyzing its byte order mark (BOM).
        /// Defaults to ASCII when detection of the text file's endianness fails.
        /// </summary>
        /// <param name="filename">The text file to analyze.</param>
        /// <returns>The detected encoding.</returns>
        public static Encoding GetEncoding(string filename)
        {
            // Read the BOM
            var bom = new byte[4];
            using (var file = new FileStream(filename, FileMode.Open, FileAccess.Read))
            {
                file.Read(bom, 0, 4);
            }

            // Analyze the BOM
#pragma warning disable SYSLIB0001 // Type or member is obsolete
            if (bom[0] == 0x2b && bom[1] == 0x2f && bom[2] == 0x76) return Encoding.UTF7;
#pragma warning restore SYSLIB0001 // Type or member is obsolete
            if (bom[0] == 0xef && bom[1] == 0xbb && bom[2] == 0xbf) return Encoding.UTF8;
            if (bom[0] == 0xff && bom[1] == 0xfe && bom[2] == 0 && bom[3] == 0) return Encoding.UTF32; //UTF-32LE
            if (bom[0] == 0xff && bom[1] == 0xfe) return Encoding.Unicode; //UTF-16LE
            if (bom[0] == 0xfe && bom[1] == 0xff) return Encoding.BigEndianUnicode; //UTF-16BE
            if (bom[0] == 0 && bom[1] == 0 && bom[2] == 0xfe && bom[3] == 0xff) return new UTF32Encoding(true, true);  //UTF-32BE

            // We actually have no idea what the encoding is if we reach this point, so
            // you may wish to return null instead of defaulting to ASCII
            return Encoding.ASCII;
        }

        private class IncludeComparer : IComparer<string>
        {
            private Configuration _configuration;

            public IncludeComparer(Configuration configuration)
            {
                _configuration = configuration;
            }

            public int Compare(string a, string b)
            {
                if (a == null || b == null)
                {
                    return 0;
                }

                if (_configuration.PlacePCHAtTheTop)
                {
                    a = a.Contains("-pch") ? "!" + a : a;
                    b = b.Contains("-pch") ? "!" + b : b;
                }

                switch (_configuration.AngularBracketsBehavior)
                {
                    case EAngularBracketsBehavior.Agnostic:
                        a = a.Replace('<', '"').Replace('>', '"');
                        b = b.Replace('<', '"').Replace('>', '"');
                        break;
                    case EAngularBracketsBehavior.GroupAtTop:
                        a = a.Contains('<') ? "!" + a : a;
                        b = b.Contains('<') ? "!" + b : b;
                        break;
                    case EAngularBracketsBehavior.GroupAtBottom:
                        a = a.Contains('<') ? "~" + a : a;
                        b = b.Contains('<') ? "~" + b : b;
                        break;
                }

                var res = 0;
                if (a != b)
                {
                    var list = new List<string> { a, b }.OrderBy(x => x).ToList();

                    return list[0] == a ? -1 : 1;
                }

                return res;
            }
        }
    }
}
