using Newtonsoft.Json;
using System;
using System.IO;

namespace AzCppIncludesConsole
{
    public enum EAngularBracketsBehavior { Agnostic, GroupAtTop, GroupAtBottom, }

    public class Configuration
    {
        private static string FileName = "AzCppIncludesConfiguration.json";

        public Configuration()
        {
            // default
        }

        public Configuration(string path)
        {
            path = Path.Combine(path, FileName);

            try
            {
                if (File.Exists(path))
                {
                    var content = File.ReadAllText(path);
                    var config = JsonConvert.DeserializeObject<Configuration>(content);
                    this.PlacePCHAtTheTop = config.PlacePCHAtTheTop;
                    this.AngularBracketsBehavior = config.AngularBracketsBehavior;
                }
                else
                {
                    File.WriteAllText(path, JsonConvert.SerializeObject(this, Formatting.Indented));
                }
            }
            catch (Exception e)
            {
                // don't use config file
            }
        }

        // Default to keep pch at the top
        public bool PlacePCHAtTheTop { get; set; } = true; 

        // Default to Agnostic - angular brackets will be mixed in with the rest.
        public EAngularBracketsBehavior AngularBracketsBehavior { get; set; } = EAngularBracketsBehavior.Agnostic;
    }
}
