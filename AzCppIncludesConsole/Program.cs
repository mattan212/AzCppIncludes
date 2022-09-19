using System;
using System.Collections.Generic;
using System.Linq;
using System.Text;
using System.Threading.Tasks;

namespace AzCppIncludesConsole
{
    internal class Program
    {
        static void Main(string[] args)
        {
            var path = args[0];

            var service = new AlphabetizeIncludesService(new Configuration
            {
                AngularBracketsBehavior = EAngularBracketsBehavior.Agnostic,
                PlacePCHAtTheTop = true
            });

            service.Run(path);
        }
    }
}
