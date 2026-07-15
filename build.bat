@echo off
setlocal
cd /d "%~dp0"

if exist "%SystemDrive%\msys64\ucrt64\bin\g++.exe" (
    set "PATH=%SystemDrive%\msys64\ucrt64\bin;%PATH%"
) else if exist "%SystemDrive%\msys64\mingw64\bin\g++.exe" (
    set "PATH=%SystemDrive%\msys64\mingw64\bin;%PATH%"
) else if exist "%SystemDrive%\mingw64\bin\g++.exe" (
    set "PATH=%SystemDrive%\mingw64\bin;%PATH%"
)

where g++ >nul 2>nul
if errorlevel 1 (
    echo ERROR: A 64-bit MinGW-w64 g++ compiler was not found.
    echo Install MSYS2 UCRT64, then run this script again.
    exit /b 1
)
where windres >nul 2>nul
if errorlevel 1 (
    echo ERROR: windres was not found next to the MinGW-w64 compiler.
    exit /b 1
)

for /f "delims=" %%T in ('g++ -dumpmachine') do set "GXX_TARGET=%%T"
for /f "delims=" %%B in ('echo __SIZEOF_POINTER__ ^| g++ -E -P -x c++ - 2^>nul') do set "GXX_POINTER_SIZE=%%B"
if not "%GXX_POINTER_SIZE%"=="8" (
    echo ERROR: Detected compiler target "%GXX_TARGET%" with %GXX_POINTER_SIZE%-byte pointers.
    echo ERROR: The bundled x64 cURL library requires a 64-bit MinGW-w64 compiler.
    exit /b 1
)

for /f "delims=" %%G in ('where g++') do if not defined GXX_PATH set "GXX_PATH=%%G"
for %%G in ("%GXX_PATH%") do set "GXX_BIN=%%~dpG"
echo Compiler: %GXX_PATH%
echo Compiler target: %GXX_TARGET% ^(64-bit^)

set "CURL_ROOT=%CD%\3rdparty\curl"
set "OUT_DIR=%CD%\outs"
set "RESOURCE_OBJECT=%OUT_DIR%\HUST-Network-Guard-resource.o"
if not exist "%CURL_ROOT%\include\curl\curl.h" (
    echo ERROR: The bundled cURL headers are missing.
    exit /b 1
)
if not exist "%CURL_ROOT%\lib\libcurl.dll.a" (
    echo ERROR: The bundled cURL import library is missing.
    exit /b 1
)
if not exist "%CURL_ROOT%\bin\libcurl-x64.dll" (
    echo ERROR: The bundled cURL runtime DLL is missing.
    exit /b 1
)
if not exist "%CD%\.env" (
    echo WARNING: .env is missing. Copy .env.example to .env and fill in the required values.
)
if not exist "%OUT_DIR%" (
    mkdir "%OUT_DIR%"
    if errorlevel 1 (
        echo ERROR: Could not create the output directory: %OUT_DIR%
        exit /b 1
    )
)

tasklist /FI "IMAGENAME eq HUST-Network-Guard.exe" /NH 2>nul | find /I "HUST-Network-Guard.exe" >nul
if not errorlevel 1 (
    echo ERROR: HUST-Network-Guard.exe is currently running.
    echo Right-click its tray icon, choose Exit, and run build.bat again.
    exit /b 1
)

echo Compiling application icon...
windres -I"%CD%\resources" -I"%CD%" ^
    "%CD%\resources\HUST-Network-Guard.rc" ^
    -O coff -o "%RESOURCE_OBJECT%"
if errorlevel 1 (
    echo ERROR: Resource compilation failed.
    exit /b 1
)

echo Building HUST-Network-Guard.exe...
g++ -std=c++17 -O2 -Wall -Wextra -pthread ^
    HUST-Network-Guard.cc "%RESOURCE_OBJECT%" ^
    -o "%OUT_DIR%\HUST-Network-Guard.exe" -mwindows ^
    -I"%CURL_ROOT%\include" -L"%CURL_ROOT%\lib" ^
    -static-libgcc -static-libstdc++ -lcurl -lshell32 -lbcrypt

if errorlevel 1 (
    del /Q "%RESOURCE_OBJECT%" >nul 2>nul
    echo ERROR: Compilation failed.
    exit /b 1
)
del /Q "%RESOURCE_OBJECT%" >nul 2>nul

copy /Y "%CURL_ROOT%\bin\libcurl-x64.dll" "%OUT_DIR%\libcurl-x64.dll" >nul
if errorlevel 1 (
    echo ERROR: Could not copy libcurl-x64.dll next to HUST-Network-Guard.exe.
    exit /b 1
)
if exist "%GXX_BIN%libwinpthread-1.dll" (
    copy /Y "%GXX_BIN%libwinpthread-1.dll" "%OUT_DIR%\libwinpthread-1.dll" >nul
    if errorlevel 1 (
        echo ERROR: Could not copy libwinpthread-1.dll next to HUST-Network-Guard.exe.
        exit /b 1
    )
)
if exist "%CD%\.env" (
    copy /Y "%CD%\.env" "%OUT_DIR%\.env" >nul
    if errorlevel 1 (
        echo ERROR: Could not copy .env to the output directory.
        exit /b 1
    )
)

echo Build completed in: %OUT_DIR%
endlocal
