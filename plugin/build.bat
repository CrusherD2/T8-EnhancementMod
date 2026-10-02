@echo off
setlocal
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1

set DETOURS=%~dp0deps\Detours\src
if not exist "%DETOURS%\detours.h" (
    echo ERROR: Detours headers missing at %DETOURS%
    exit /b 1
)

set OUT=%~dp0build
if not exist "%OUT%" mkdir "%OUT%"

cl /nologo /std:c++20 /O2 /MT /EHsc /LD /DWIN32_LEAN_AND_MEAN /D_AMD64_ /I"%DETOURS%" ^
   "%~dp0src\main.cpp" "%~dp0src\models.cpp" "%~dp0src\swap.cpp" "%~dp0src\materials.cpp" "%~dp0src\dvars.cpp" "%DETOURS%\detours.cpp" "%DETOURS%\disasm.cpp" "%DETOURS%\modules.cpp" ^
   "%DETOURS%\image.cpp" "%DETOURS%\creatwth.cpp" ^
   /Fo"%OUT%\\" /Fe"%OUT%\bo3port.dll" /link /DLL || exit /b 1

copy /Y "%OUT%\bo3port.dll" "%~dp0..\ShieldConfig\project-bo4\plugins\bo3port.dll" >nul

echo built %OUT%\bo3port.dll
endlocal
