@echo off
setlocal
cd /d "%~dp0"
if not defined VSCMD_VER (
  for /f "usebackq delims=" %%i in (`"%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do call "%%i\VC\Auxiliary\Build\vcvars64.bat" >nul
)
if not exist build mkdir build
set flags=/nologo /std:c++17 /EHsc /W4 /WX /utf-8 /O2 /MT /DUNICODE /D_UNICODE /DNOMINMAX /D_WIN32_WINNT=0x0A00
if /i "%~1"=="test" goto test
python -c "import sys; sys.exit(sys.version_info.major != 3)" >nul 2>&1
if errorlevel 1 (
  py -3 make_icon.py || exit /b 1
) else (
  python make_icon.py || exit /b 1
)
if /i "%~1"=="ui-test" goto uitest
rc /nologo /fo build\resources.res resources.rc || exit /b 1
cl %flags% app.cpp build\resources.res /Fo:build\app.obj /Fe:build\PyDesk.exe /link /SUBSYSTEM:WINDOWS
exit /b %errorlevel%
:test
cl %flags% tests.cpp /Fo:build\tests.obj /Fe:build\tests.exe && build\tests.exe
exit /b %errorlevel%
:uitest
rc /nologo /fo build\resources.res resources.rc || exit /b 1
cl %flags% /DPYDESK_UI_TEST app.cpp build\resources.res /Fo:build\ui-tests.obj /Fe:build\ui-tests.exe /link /SUBSYSTEM:CONSOLE && build\ui-tests.exe
exit /b %errorlevel%
