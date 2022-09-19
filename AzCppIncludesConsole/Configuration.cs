namespace AzCppIncludesConsole
{
    public enum EAngularBracketsBehavior { Agnostic, GroupAtTop, GroupAtBottom, }

    public class Configuration
    {
        public bool PlacePCHAtTheTop { get; set; }

        public EAngularBracketsBehavior AngularBracketsBehavior { get; set; }
    }
}
