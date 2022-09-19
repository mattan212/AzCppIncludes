using AzCppIncludesConsole;

namespace UnitTests
{
    [TestClass]
    public class AlphabetizeIncludesServiceTests
    {
        AlphabetizeIncludesService service = new AlphabetizeIncludesService(new Configuration());

        [TestMethod]
        public void ConfigurationPchOnTop()
        {
            service = new AlphabetizeIncludesService(new Configuration { PlacePCHAtTheTop = true });

            var fileName = "ConfigurationPchOnTop";
            var sourceFilePath = Path.Combine("TestFiles", $"{fileName}.cpp");
            var testFilePath = Path.Combine("TestFiles", $"output.cpp");
            var expectedFilePath = Path.Combine("TestFiles", $"{fileName}_expected.cpp");

            File.Copy(sourceFilePath, testFilePath, true);

            service.Run(testFilePath);

            var actualLines = File.ReadAllLines(testFilePath);
            var expectedLines = File.ReadAllLines(expectedFilePath);

            for (var i = 0; i < actualLines.Length; i++)
            {
                Assert.AreEqual(expectedLines[i], actualLines[i]);
            }
        }

        [TestMethod]
        public void ConfigurationPchDefault()
        {
            service = new AlphabetizeIncludesService(new Configuration { PlacePCHAtTheTop = false });

            var fileName = "ConfigurationPchNotOnTop";
            var sourceFilePath = Path.Combine("TestFiles", $"{fileName}.cpp");
            var testFilePath = Path.Combine("TestFiles", $"output.cpp");
            var expectedFilePath = Path.Combine("TestFiles", $"{fileName}_expected.cpp");

            File.Copy(sourceFilePath, testFilePath, true);

            service.Run(testFilePath);

            var actualLines = File.ReadAllLines(testFilePath);
            var expectedLines = File.ReadAllLines(expectedFilePath);

            for (var i = 0; i < actualLines.Length; i++)
            {
                Assert.AreEqual(expectedLines[i], actualLines[i]);
            }
        }

        [TestMethod]
        public void ConfigurationAngularBracketsAgnostic()
        {
            service = new AlphabetizeIncludesService(new Configuration { AngularBracketsBehavior = EAngularBracketsBehavior.Agnostic });

            var fileName = "ConfigurationAngularBracketsAgnostic";
            var sourceFilePath = Path.Combine("TestFiles", $"{fileName}.cpp");
            var testFilePath = Path.Combine("TestFiles", $"output.cpp");
            var expectedFilePath = Path.Combine("TestFiles", $"{fileName}_expected.cpp");

            File.Copy(sourceFilePath, testFilePath, true);

            service.Run(testFilePath);

            var actualLines = File.ReadAllLines(testFilePath);
            var expectedLines = File.ReadAllLines(expectedFilePath);

            for (var i = 0; i < actualLines.Length; i++)
            {
                Assert.AreEqual(expectedLines[i], actualLines[i]);
            }
        }

        [TestMethod]
        public void ConfigurationAngularBracketsOnTop()
        {
            service = new AlphabetizeIncludesService(new Configuration { AngularBracketsBehavior = EAngularBracketsBehavior.GroupAtTop });

            var fileName = "ConfigurationAngularBracketsOnTop";
            var sourceFilePath = Path.Combine("TestFiles", $"{fileName}.cpp");
            var testFilePath = Path.Combine("TestFiles", $"output.cpp");
            var expectedFilePath = Path.Combine("TestFiles", $"{fileName}_expected.cpp");

            File.Copy(sourceFilePath, testFilePath, true);

            service.Run(testFilePath);

            var actualLines = File.ReadAllLines(testFilePath);
            var expectedLines = File.ReadAllLines(expectedFilePath);

            for (var i = 0; i < actualLines.Length; i++)
            {
                Assert.AreEqual(expectedLines[i], actualLines[i]);
            }
        }

        [TestMethod]
        public void ConfigurationAngularBracketsOnBottom()
        {
            service = new AlphabetizeIncludesService(new Configuration { AngularBracketsBehavior = EAngularBracketsBehavior.GroupAtBottom });

            var fileName = "ConfigurationAngularBracketsOnBottom";
            var sourceFilePath = Path.Combine("TestFiles", $"{fileName}.cpp");
            var testFilePath = Path.Combine("TestFiles", $"output.cpp");
            var expectedFilePath = Path.Combine("TestFiles", $"{fileName}_expected.cpp");

            File.Copy(sourceFilePath, testFilePath, true);

            service.Run(testFilePath);

            var actualLines = File.ReadAllLines(testFilePath);
            var expectedLines = File.ReadAllLines(expectedFilePath);

            for (var i = 0; i < actualLines.Length; i++)
            {
                Assert.AreEqual(expectedLines[i], actualLines[i]);
            }
        }

        [TestMethod]
        public void ConfigurationMixed1()
        {
            service = new AlphabetizeIncludesService(new Configuration 
            { 
                PlacePCHAtTheTop = true,
                AngularBracketsBehavior = EAngularBracketsBehavior.GroupAtBottom 
            });

            var fileName = "ConfigurationMixed1";
            var sourceFilePath = Path.Combine("TestFiles", $"{fileName}.cpp");
            var testFilePath = Path.Combine("TestFiles", $"output.cpp");
            var expectedFilePath = Path.Combine("TestFiles", $"{fileName}_expected.cpp");

            File.Copy(sourceFilePath, testFilePath, true);

            service.Run(testFilePath);

            var actualLines = File.ReadAllLines(testFilePath);
            var expectedLines = File.ReadAllLines(expectedFilePath);

            for (var i = 0; i < actualLines.Length; i++)
            {
                Assert.AreEqual(expectedLines[i], actualLines[i]);
            }
        }
    }
}