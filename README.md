# AzCppIncludesVsix 
visual studio extension for sorting your headers alphabetically

### Project Wiki 🤖

This VSIX sorts your c++ headers, that's it.
By default, it would keep -pch files at the top, and would treat angular brackets as case agnostic.
Given the following includes, this is the expected output:
- #include "foo-pch.h"
- #include "alice.h"
- #include <bob.h>
- #include "cat.h"

You can modify those default by setting configurations here: *%localappdata%\Microsoft\VisualStudio\Vsix\AzCppIncludes\AzCppIncludesConfiguration.json*:
1. PlacePCHAtTheTop (bool) whether to leave the -pch headers untouched (typically at the top of the file).
2. AngularBracketsBehavior (0-2): 0 - Agnostic, they will be tread the same as other headers. 1 - grouped and sorted at the top. 2 - groupd and sorted at the bottom.

- Note: The configurations file will be automatically created after first run
- In case you need them, logs for vsix can be found here: *%localappdata%\Microsoft\VisualStudio\Vsix\AzCppIncludes\* 

Hopefully you find this extension useful.
