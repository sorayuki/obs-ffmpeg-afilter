@echo off
setlocal

set "ROOT=%~dp0"
set "BUILD_DIR=%ROOT%build_x64"
set "DIST_DIR=%ROOT%dist"
set "INSTALL_DIR=%DIST_DIR%\install"
set "STAGE_DIR=%DIST_DIR%\package"
set "PACKAGE_NAME=obs-ffmpeg-afilter-0.4.0-windows-x64.zip"
set "PACKAGE_PATH=%ROOT%%PACKAGE_NAME%"

pushd "%ROOT%" || goto :error

echo [1/5] Configuring windows-x64...
cmake --preset windows-x64
if errorlevel 1 goto :error

echo [2/5] Building RelWithDebInfo...
cmake --build --preset windows-x64 --config RelWithDebInfo --parallel
if errorlevel 1 goto :error

echo [3/5] Installing to staging directory...
if exist "%INSTALL_DIR%" rmdir /s /q "%INSTALL_DIR%"
if exist "%STAGE_DIR%" rmdir /s /q "%STAGE_DIR%"
cmake --install "%BUILD_DIR%" --config RelWithDebInfo --prefix "%INSTALL_DIR%"
if errorlevel 1 goto :error

echo [4/5] Creating OBS package layout...
mkdir "%STAGE_DIR%\obs-plugins\64bit" || goto :error
mkdir "%STAGE_DIR%\data\obs-plugins\obs-ffmpeg-afilter" || goto :error
copy /y "%INSTALL_DIR%\obs-ffmpeg-afilter\bin\64bit\*" "%STAGE_DIR%\obs-plugins\64bit\" >nul
if errorlevel 1 goto :error
xcopy /e /i /y "%INSTALL_DIR%\obs-ffmpeg-afilter\data\*" "%STAGE_DIR%\data\obs-plugins\obs-ffmpeg-afilter\" >nul
if errorlevel 1 goto :error

echo [5/5] Creating %PACKAGE_NAME%...
if exist "%PACKAGE_PATH%" del /q "%PACKAGE_PATH%"
powershell.exe -NoLogo -NoProfile -ExecutionPolicy Bypass -Command ^
  "Compress-Archive -Path '%STAGE_DIR%\*' -DestinationPath '%PACKAGE_PATH%' -CompressionLevel Optimal"
if errorlevel 1 goto :error

rmdir /s /q "%INSTALL_DIR%"
rmdir /s /q "%STAGE_DIR%"
popd

echo Package created: %PACKAGE_PATH%
exit /b 0

:error
set "EXIT_CODE=%ERRORLEVEL%"
if "%EXIT_CODE%"=="0" set "EXIT_CODE=1"
popd 2>nul
echo Packaging failed with exit code %EXIT_CODE%.
exit /b %EXIT_CODE%
