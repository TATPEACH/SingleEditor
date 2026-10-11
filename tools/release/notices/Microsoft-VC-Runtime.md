# Microsoft Visual C++ runtime

This distribution includes unmodified Microsoft Visual C++ runtime libraries:
msvcp140.dll, msvcp140_atomic_wait.dll, vcruntime140.dll and vcruntime140_1.dll.
They are Microsoft components, separate from Single Editor's source license.
The exact library versions and hashes are recorded in the release evidence.

The libraries are copied from the licensed Visual Studio installation's
VC/redist/MSVC/x64 CRT directory, not from Windows system directories.
Redistribution remains subject to the applicable Microsoft software license
and the official REDIST list:
https://learn.microsoft.com/en-us/visualstudio/releases/2026/redistribution

App-local deployment keeps portable use independent of an installer modifying
the system runtime. Library updates must be supplied with application updates:
https://learn.microsoft.com/en-us/cpp/windows/choosing-a-deployment-method

This notice identifies the components and their source. It does not replace
Microsoft's terms or the application's final release license review.
