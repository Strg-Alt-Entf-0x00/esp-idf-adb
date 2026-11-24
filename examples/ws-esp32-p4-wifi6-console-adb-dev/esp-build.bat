@echo off
setlocal EnableExtensions

set "PROJECT_DIR=%~dp0"
set "ESP_BUILD_IDF_PATH="
set "ESP_BUILD_PORT="
set "ESP_BUILD_BAUD="

pushd "%PROJECT_DIR%" >nul
if errorlevel 1 (
    echo ERROR: Could not open project directory: "%PROJECT_DIR%"
    exit /b 1
)

if not exist "CMakeLists.txt" (
    echo ERROR: CMakeLists.txt was not found in "%PROJECT_DIR%".
    popd
    exit /b 1
)

if /i "%~1"=="help" goto :help
if /i "%~1"=="-h" goto :help
if /i "%~1"=="--help" goto :help

:parse_options
if "%~1"=="" goto :options_done
if /i "%~1"=="-p" goto :option_port
if /i "%~1"=="-b" goto :option_baud
goto :options_done

:option_port
if "%~2"=="" (
    echo ERROR: Missing serial port after -p.
    popd
    exit /b 2
)
set "ESP_BUILD_PORT=%~2"
shift
shift
goto :parse_options

:option_baud
if "%~2"=="" (
    echo ERROR: Missing baud rate after -b.
    popd
    exit /b 2
)
set "ESP_BUILD_BAUD=%~2"
shift
shift
goto :parse_options

:options_done
where idf.py >nul 2>&1
if not errorlevel 1 (
    call idf.py --version >nul 2>&1
    if not errorlevel 1 goto :idf_ready
)

if defined IDF_PATH if exist "%IDF_PATH%\export.bat" set "ESP_BUILD_IDF_PATH=%IDF_PATH%"
if not defined ESP_BUILD_IDF_PATH (
    for /d %%D in ("C:\esp\v6*\esp-idf") do (
        if not defined ESP_BUILD_IDF_PATH if exist "%%~fD\export.bat" set "ESP_BUILD_IDF_PATH=%%~fD"
    )
)
if not defined ESP_BUILD_IDF_PATH (
    for /d %%D in ("C:\esp\v5*\esp-idf") do (
        if not defined ESP_BUILD_IDF_PATH if exist "%%~fD\export.bat" set "ESP_BUILD_IDF_PATH=%%~fD"
    )
)
if not defined ESP_BUILD_IDF_PATH (
    for /d %%D in ("C:\esp\v*") do (
        if not defined ESP_BUILD_IDF_PATH if exist "%%~fD\esp-idf\export.bat" set "ESP_BUILD_IDF_PATH=%%~fD\esp-idf"
    )
)
if not defined ESP_BUILD_IDF_PATH (
    for /d %%D in ("C:\Espressif\frameworks\esp-idf-v6*" "%USERPROFILE%\Espressif\frameworks\esp-idf-v6*") do (
        if not defined ESP_BUILD_IDF_PATH if exist "%%~fD\export.bat" set "ESP_BUILD_IDF_PATH=%%~fD"
    )
)
if not defined ESP_BUILD_IDF_PATH (
    for /d %%D in ("C:\Espressif\frameworks\esp-idf-v5*" "%USERPROFILE%\Espressif\frameworks\esp-idf-v5*") do (
        if not defined ESP_BUILD_IDF_PATH if exist "%%~fD\export.bat" set "ESP_BUILD_IDF_PATH=%%~fD"
    )
)
if not defined ESP_BUILD_IDF_PATH (
    for /d %%D in ("C:\Espressif\frameworks\esp-idf*" "%USERPROFILE%\Espressif\frameworks\esp-idf*" "%USERPROFILE%\esp\esp-idf") do (
        if not defined ESP_BUILD_IDF_PATH if exist "%%~fD\export.bat" set "ESP_BUILD_IDF_PATH=%%~fD"
    )
)

if not defined ESP_BUILD_IDF_PATH goto :idf_not_ready
echo Activating ESP-IDF from "%ESP_BUILD_IDF_PATH%"...
call "%ESP_BUILD_IDF_PATH%\export.bat"
if errorlevel 1 (
    echo ERROR: ESP-IDF environment setup failed.
    popd
    exit /b 1
)

:idf_ready
where idf.py >nul 2>&1
if errorlevel 1 goto :idf_not_ready
call idf.py --version >nul 2>&1
if errorlevel 1 goto :idf_not_ready

if "%~1"=="" goto :build
if /i "%~1"=="build" goto :build
if /i "%~1"=="clean" goto :clean
if /i "%~1"=="clean-build" goto :clean_build
if /i "%~1"=="flash" if /i "%~2"=="monitor" goto :flash_monitor
if /i "%~1"=="flash" goto :flash
if /i "%~1"=="monitor" goto :monitor
if /i "%~1"=="flash-monitor" goto :flash_monitor

echo ERROR: Unknown action "%~1".
goto :help_error

:build
echo Building ESP-IDF project...
call idf.py --ccache build
if errorlevel 1 goto :command_failed
goto :success

:clean
echo Cleaning generated build files...
call idf.py fullclean
if errorlevel 1 goto :command_failed
goto :success

:clean_build
echo Cleaning generated build files...
call idf.py fullclean
if errorlevel 1 goto :command_failed
goto :build

:flash
if defined ESP_BUILD_PORT (
    if defined ESP_BUILD_BAUD (
        call idf.py -p "%ESP_BUILD_PORT%" -b "%ESP_BUILD_BAUD%" flash
    ) else (
        call idf.py -p "%ESP_BUILD_PORT%" flash
    )
) else if defined ESP_BUILD_BAUD (
    call idf.py -b "%ESP_BUILD_BAUD%" flash
) else (
    call idf.py flash
)
if errorlevel 1 goto :command_failed
goto :success

:monitor
if defined ESP_BUILD_PORT (
    if defined ESP_BUILD_BAUD (
        call idf.py -p "%ESP_BUILD_PORT%" -b "%ESP_BUILD_BAUD%" monitor
    ) else (
        call idf.py -p "%ESP_BUILD_PORT%" monitor
    )
) else if defined ESP_BUILD_BAUD (
    call idf.py -b "%ESP_BUILD_BAUD%" monitor
) else (
    call idf.py monitor
)
if errorlevel 1 goto :command_failed
goto :success

:flash_monitor
if defined ESP_BUILD_PORT (
    if defined ESP_BUILD_BAUD (
        call idf.py -p "%ESP_BUILD_PORT%" -b "%ESP_BUILD_BAUD%" flash monitor
    ) else (
        call idf.py -p "%ESP_BUILD_PORT%" flash monitor
    )
) else if defined ESP_BUILD_BAUD (
    call idf.py -b "%ESP_BUILD_BAUD%" flash monitor
) else (
    call idf.py flash monitor
)
if errorlevel 1 goto :command_failed
goto :success

:success
echo.
echo ESP-IDF command completed successfully.
popd
exit /b 0

:command_failed
set "ESP_BUILD_EXIT_CODE=%ERRORLEVEL%"
if "%ESP_BUILD_EXIT_CODE%"=="0" set "ESP_BUILD_EXIT_CODE=1"
echo.
echo ESP-IDF command failed with exit code %ESP_BUILD_EXIT_CODE%.
popd
exit /b %ESP_BUILD_EXIT_CODE%

:idf_not_ready
echo ERROR: ESP-IDF could not be activated.
echo Install ESP-IDF 6.0.2 or newer, use its Command Prompt, or set IDF_PATH.
popd
exit /b 1

:help
echo Usage: esp-build.bat [-p COMx] [-b BAUD] [build^|clean^|clean-build^|flash^|monitor^|flash monitor]
echo.
echo With no action, builds the project. ESP-IDF is detected from the current
echo environment, IDF_PATH, or common Windows installation directories.
popd
exit /b 0

:help_error
echo.
echo Use "esp-build.bat help" to see available actions.
popd
exit /b 2
