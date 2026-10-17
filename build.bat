@echo off
rem Build hudfix2142 (x86). Output: dist\dinput8.dll + dist\hudfix2142.ini
setlocal
call "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvarsall.bat" x86 >nul || exit /b 1
cd /d "%~dp0"
if not exist obj mkdir obj
if not exist dist mkdir dist
cl /nologo /std:c++17 /O2 /W4 /MT /EHsc /GS- /D_CRT_SECURE_NO_WARNINGS /DUNICODE /D_UNICODE /Foobj\ ^
   /LD src\dllmain.cpp /Fe:obj\hudfix2142.dll /link user32.lib || exit /b 1
copy /y obj\hudfix2142.dll dist\dinput8.dll >nul
copy /y hudfix2142.ini dist\ >nul
echo Built dist\dinput8.dll
