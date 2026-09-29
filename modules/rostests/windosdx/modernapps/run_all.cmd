@echo off
rem Runs every modern-application test in this folder and prints a summary.
cd /d "%~dp0"
set PASSED=0
set FAILED=0
for %%T in (crt_static crt_dynamic win7api fsapi fenvtest c99math cpp17 netinfo) do call :run %%T
echo.
echo Modern app tests: %PASSED% passed, %FAILED% failed
exit /b %FAILED%

:run
".\%1.exe"
if errorlevel 1 (set /a FAILED+=1 & echo *** %1 FAILED ^(exit code %errorlevel%^)) else (set /a PASSED+=1)
goto :eof
