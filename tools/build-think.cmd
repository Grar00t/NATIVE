@echo off
setlocal
pushd "%~dp0.." || exit /b 1
if not exist build mkdir build
cl /nologo /std:c11 /O2 /W4 /WX /DTHINK_WITH_AGENTS /c tools\think.c /Fobuild\think-core.obj || exit /b 1
cl /nologo /std:c++17 /utf-8 /EHsc /O2 /W4 /WX /c tools\agents.cpp tools\agent_connectors.cpp /Fobuild\ || exit /b 1
link /nologo /OUT:build\think.exe build\think-core.obj build\agents.obj build\agent_connectors.obj
exit /b %errorlevel%
