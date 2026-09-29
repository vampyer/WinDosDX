@echo off
rem Builds the modern-application tests with the host Visual Studio 2022
rem toolset, exactly like a third-party program would be built, into
rem output-modernapps\<arch>. Usage: build.cmd [x86|x64]
rem
rem The Visual C++ runtime DLLs are copied next to the tests (an "app-local"
rem deployment, which Microsoft's redistribution terms allow); they are not
rem installed into system32.
setlocal
set "ARCH=%~1"
if "%ARCH%"=="" set "ARCH=x86"
if /i "%ARCH%"=="x86" (set "VCVARS=x64_x86") else (set "VCVARS=x64")
set "SRC=%~dp0"
set "OUT=%~dp0..\..\..\..\output-modernapps\%ARCH%"
set "VS=C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools"
set "MSVC=%VS%\VC\Tools\MSVC\14.44.35207"
set "REDIST=%VS%\VC\Redist\MSVC\14.44.35112\%ARCH%\Microsoft.VC143.CRT"
set "WINSDK=C:\Program Files (x86)\Windows Kits\10"
set "WINSDKVER=10.0.26100.0"

call "%VS%\VC\Auxiliary\Build\vcvarsall.bat" %VCVARS% >nul
if errorlevel 1 exit /b 1
rem vswhere is missing on this Build Tools install; set the paths explicitly.
set "INCLUDE=%MSVC%\include;%WINSDK%\Include\%WINSDKVER%\ucrt;%WINSDK%\Include\%WINSDKVER%\um;%WINSDK%\Include\%WINSDKVER%\shared"
set "LIB=%MSVC%\lib\%ARCH%;%WINSDK%\Lib\%WINSDKVER%\ucrt\%ARCH%;%WINSDK%\Lib\%WINSDKVER%\um\%ARCH%"
if /i "%ARCH%"=="x86" (set "PATH=%MSVC%\bin\Hostx64\x86;%MSVC%\bin\Hostx64\x64;%PATH%") else (set "PATH=%MSVC%\bin\Hostx64\x64;%PATH%")

if not exist "%OUT%" mkdir "%OUT%"
pushd "%OUT%"
set "CFLAGS=/nologo /W3 /O2 /D_CRT_SECURE_NO_WARNINGS"
cl %CFLAGS% /MD /DTEST_NAME=\"crt_dynamic\" "%SRC%crt_basic.c" /Fe:crt_dynamic.exe || goto :fail
cl %CFLAGS% /MT /DTEST_NAME=\"crt_static\" "%SRC%crt_basic.c" /Fe:crt_static.exe || goto :fail
cl %CFLAGS% /MD "%SRC%win7api.c" /Fe:win7api.exe || goto :fail
cl %CFLAGS% /MD "%SRC%fsapi.c" /Fe:fsapi.exe || goto :fail
cl %CFLAGS% /MD "%SRC%fenvtest.c" /Fe:fenvtest.exe || goto :fail
cl %CFLAGS% /MD "%SRC%c99math.c" /Fe:c99math.exe || goto :fail
cl %CFLAGS% /MD /EHsc /std:c++17 "%SRC%cpp17.cpp" /Fe:cpp17.exe || goto :fail
cl %CFLAGS% /MD "%SRC%netinfo.c" /Fe:netinfo.exe /link ws2_32.lib iphlpapi.lib || goto :fail
del /q *.obj 2>nul
copy /y "%REDIST%\vcruntime140.dll" . >nul || goto :fail
copy /y "%REDIST%\msvcp140.dll" . >nul || goto :fail
if /i "%ARCH%"=="x64" copy /y "%REDIST%\vcruntime140_1.dll" . >nul
copy /y "%SRC%run_all.cmd" . >nul || goto :fail
rem COM2 wrapper for the QEMU harness: the guest console cannot be read
rem from outside, but output redirected to COM2 lands in a host file.
> run_netinfo.cmd echo @echo off
>>run_netinfo.cmd echo title wdx-autorun
>>run_netinfo.cmd echo echo A-SCRIPT-STARTED
>>run_netinfo.cmd echo echo B-WRITING-COM2
>>run_netinfo.cmd echo echo tick ^> COM2
>>run_netinfo.cmd echo echo C-WROTE-COM2 errorlevel=^%%errorlevel^%%
>>run_netinfo.cmd echo dir D:\reactos\tests\modern
>>run_netinfo.cmd echo cd /d D:\reactos\tests\modern
>>run_netinfo.cmd echo D-RUNNING-NETINFO
>>run_netinfo.cmd echo set WDX_NO_COM=1
>>run_netinfo.cmd echo netinfo.exe
>>run_netinfo.cmd echo set WDX_NO_COM=
>>run_netinfo.cmd echo echo E-NETINFO-EXIT ^%%errorlevel^%%
>>run_netinfo.cmd echo cmd /k
popd
echo Built modern-app tests in %OUT%
exit /b 0

:fail
popd
echo Build failed.
exit /b 1
