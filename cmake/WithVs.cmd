@echo off
setlocal
set "TURBODB_VCPKG_ROOT=%VCPKG_ROOT%"
call "%VSDEVCMD_BAT%" -arch=x64 -host_arch=x64 >nul
if errorlevel 1 exit /b %errorlevel%
set "VCPKG_ROOT=%TURBODB_VCPKG_ROOT%"
for %%I in ("%VSINSTALLDIR%.") do set "VCPKG_VISUAL_STUDIO_PATH=%%~fI"
%*
exit /b %errorlevel%
