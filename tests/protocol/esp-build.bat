@echo off
setlocal EnableExtensions

set "PROJECT_DIR=%~dp0"
set "CONFIG_FILE=%PROJECT_DIR%esp-build-config.json"
set "ESP_BUILD_IDF_PATH="
set "ESP_BUILD_TARGET="
set "ESP_BUILD_PORT="
set "ESP_BUILD_FLASH_BAUD="
set "ESP_BUILD_MONITOR_BAUD="
set "ESP_BUILD_BAUD_OVERRIDE="
set "ESP_BUILD_ACTION="
set "ESP_BUILD_CLEAN_PROMPT="
set "ESP_BUILD_CCACHE="
set "ESP_BUILD_CONFIG_VALID="
set "ESP_BUILD_AFTER_FLASH_MONITOR="
set "ESP_BUILD_USE_DEFAULT_ACTION="

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

if not exist "%CONFIG_FILE%" (
    echo ERROR: Project configuration was not found: "%CONFIG_FILE%"
    popd
    exit /b 1
)

for /f "tokens=1,* delims==" %%A in ('powershell.exe -NoProfile -Command "$ErrorActionPreference = 'Stop'; $c = Get-Content -Raw -LiteralPath $env:CONFIG_FILE | ConvertFrom-Json; if ($c.version -ne 1) { throw 'version must be 1' }; if ($null -ne $c.esp_idf.path -and ($c.esp_idf.path -isnot [string] -or [string]::IsNullOrWhiteSpace($c.esp_idf.path))) { throw 'esp_idf.path must be a non-empty path string or null' }; if ($null -ne $c.serial.port -and ($c.serial.port -isnot [string] -or [string]::IsNullOrWhiteSpace($c.serial.port))) { throw 'serial.port must be a non-empty string or null' }; foreach ($key in @('flash_baudrate','monitor_baudrate')) { $value = $c.serial.$key; if ($null -ne $value -and ($value -isnot [int] -or $value -lt 1 -or $value -gt 2000000)) { throw ('serial.' + $key + ' must be an integer between 1 and 2000000 or null') } }; if (@('build','flash','flash-monitor','ask') -notcontains $c.workflow.default_action) { throw 'workflow.default_action must be build, flash, flash-monitor, or ask' }; if ($c.workflow.clean_build_prompt -isnot [bool] -or $c.build.ccache -isnot [bool]) { throw 'workflow.clean_build_prompt and build.ccache must be true or false' }; 'IDF_PATH=' + [string]$c.esp_idf.path; 'PORT=' + [string]$c.serial.port; 'FLASH_BAUD=' + [string]$c.serial.flash_baudrate; 'MONITOR_BAUD=' + [string]$c.serial.monitor_baudrate; 'DEFAULT_ACTION=' + [string]$c.workflow.default_action; 'CLEAN_PROMPT=' + [string]$c.workflow.clean_build_prompt; 'CCACHE=' + [string]$c.build.ccache; 'CONFIG_VALID=1'"') do (
    if /i "%%A"=="IDF_PATH" set "ESP_BUILD_IDF_PATH=%%B"
    if /i "%%A"=="PORT" set "ESP_BUILD_PORT=%%B"
    if /i "%%A"=="FLASH_BAUD" set "ESP_BUILD_FLASH_BAUD=%%B"
    if /i "%%A"=="MONITOR_BAUD" set "ESP_BUILD_MONITOR_BAUD=%%B"
    if /i "%%A"=="DEFAULT_ACTION" set "ESP_BUILD_ACTION=%%B"
    if /i "%%A"=="CLEAN_PROMPT" set "ESP_BUILD_CLEAN_PROMPT=%%B"
    if /i "%%A"=="CCACHE" set "ESP_BUILD_CCACHE=%%B"
    if /i "%%A"=="CONFIG_VALID" set "ESP_BUILD_CONFIG_VALID=%%B"
)
if not "%ESP_BUILD_CONFIG_VALID%"=="1" (
    echo ERROR: Invalid or unsupported settings in "%CONFIG_FILE%".
    popd
    exit /b 1
)

:parse_options
if "%~1"=="" goto :options_done
if /i "%~1"=="help" goto :help
if /i "%~1"=="-h" goto :help
if /i "%~1"=="--help" goto :help
if /i "%~1"=="-t" goto :option_target
if /i "%~1"=="-p" goto :option_port
if /i "%~1"=="-b" goto :option_baud
if /i "%~1"=="flash" if /i "%~2"=="monitor" goto :option_flash_monitor
if /i "%~1"=="build" goto :option_action
if /i "%~1"=="clean" goto :option_action
if /i "%~1"=="clean-build" goto :option_action
if /i "%~1"=="flash" goto :option_action
if /i "%~1"=="monitor" goto :option_action
if /i "%~1"=="flash-monitor" goto :option_action
echo ERROR: Unknown option or action "%~1".
goto :help_error

:option_target
if "%~2"=="" (
    echo ERROR: Missing target after -t. Use esp32 or esp32p4.
    popd
    exit /b 2
)
if /i not "%~2"=="esp32" if /i not "%~2"=="esp32p4" (
    echo ERROR: Unsupported target "%~2". Use esp32 or esp32p4.
    popd
    exit /b 2
)
set "ESP_BUILD_TARGET=%~2"
shift
shift
goto :parse_options

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
set "ESP_BUILD_BAUD_OVERRIDE=%~2"
shift
shift
goto :parse_options

:option_action
set "ESP_BUILD_ACTION=%~1"
shift
goto :parse_options

:option_flash_monitor
set "ESP_BUILD_ACTION=flash-monitor"
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

if defined ESP_BUILD_TARGET (
    echo Setting target to %ESP_BUILD_TARGET%...
    call idf.py set-target "%ESP_BUILD_TARGET%"
    if errorlevel 1 goto :command_failed
)

if "%~1"=="" goto :default_build
set "ESP_BUILD_ACTION=%~1"

:dispatch
if /i "%ESP_BUILD_ACTION%"=="build" goto :build
if /i "%ESP_BUILD_ACTION%"=="clean" goto :clean
if /i "%ESP_BUILD_ACTION%"=="clean-build" goto :clean_build
if /i "%ESP_BUILD_ACTION%"=="flash" if /i "%~2"=="monitor" goto :flash_monitor
if /i "%ESP_BUILD_ACTION%"=="flash" goto :flash
if /i "%ESP_BUILD_ACTION%"=="monitor" goto :monitor
if /i "%ESP_BUILD_ACTION%"=="flash-monitor" goto :flash_monitor
echo ERROR: Unknown action "%ESP_BUILD_ACTION%".
goto :help_error

:default_build
set "ESP_BUILD_USE_DEFAULT_ACTION=1"
goto :build

:build
if /i "%ESP_BUILD_CLEAN_PROMPT%"=="true" if exist "build\" (
    choice /C YN /N /M "Build directory exists. Clean before build? [Y/N] "
    if errorlevel 2 goto :run_build
    call idf.py fullclean
    if errorlevel 1 goto :command_failed
)

:run_build
echo Building protocol test application...
if /i "%ESP_BUILD_CCACHE%"=="true" (
    call idf.py --ccache build
) else (
    call idf.py --no-ccache build
)
if errorlevel 1 goto :command_failed
if defined ESP_BUILD_USE_DEFAULT_ACTION goto :default_action_after_build
goto :success

:default_action_after_build
if /i "%ESP_BUILD_ACTION%"=="build" goto :success
if /i "%ESP_BUILD_ACTION%"=="flash" goto :flash
if /i "%ESP_BUILD_ACTION%"=="flash-monitor" (
    set "ESP_BUILD_AFTER_FLASH_MONITOR=1"
    goto :flash
)
if /i "%ESP_BUILD_ACTION%"=="ask" (
    choice /C YN /N /M "Build complete. Flash to device? [Y/N] "
    if errorlevel 2 goto :success
    choice /C YN /N /M "Open serial monitor after flashing? [Y/N] "
    if not errorlevel 2 set "ESP_BUILD_AFTER_FLASH_MONITOR=1"
    goto :flash
)
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
    if defined ESP_BUILD_BAUD_OVERRIDE (
        call idf.py -p "%ESP_BUILD_PORT%" -b "%ESP_BUILD_BAUD_OVERRIDE%" flash
    ) else if defined ESP_BUILD_FLASH_BAUD (
        call idf.py -p "%ESP_BUILD_PORT%" -b "%ESP_BUILD_FLASH_BAUD%" flash
    ) else (
        call idf.py -p "%ESP_BUILD_PORT%" flash
    )
) else if defined ESP_BUILD_BAUD_OVERRIDE (
    call idf.py -b "%ESP_BUILD_BAUD_OVERRIDE%" flash
) else if defined ESP_BUILD_FLASH_BAUD (
    call idf.py -b "%ESP_BUILD_FLASH_BAUD%" flash
) else (
    call idf.py flash
)
if errorlevel 1 goto :command_failed
goto :after_flash

:after_flash
if not defined ESP_BUILD_AFTER_FLASH_MONITOR goto :success
goto :monitor

:flash_monitor
set "ESP_BUILD_AFTER_FLASH_MONITOR=1"
goto :flash

:monitor
if defined ESP_BUILD_PORT (
    if defined ESP_BUILD_BAUD_OVERRIDE (
        call idf.py -p "%ESP_BUILD_PORT%" -b "%ESP_BUILD_BAUD_OVERRIDE%" monitor
    ) else if defined ESP_BUILD_MONITOR_BAUD (
        call idf.py -p "%ESP_BUILD_PORT%" -b "%ESP_BUILD_MONITOR_BAUD%" monitor
    ) else (
        call idf.py -p "%ESP_BUILD_PORT%" monitor
    )
) else if defined ESP_BUILD_BAUD_OVERRIDE (
    call idf.py -b "%ESP_BUILD_BAUD_OVERRIDE%" monitor
) else if defined ESP_BUILD_MONITOR_BAUD (
    call idf.py -b "%ESP_BUILD_MONITOR_BAUD%" monitor
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
echo Usage: esp-build.bat [-t esp32^|esp32p4] [-p COMx] [-b BAUD] [build^|clean^|clean-build^|flash^|flash monitor^|monitor]
echo.
echo With no action, runs workflow.default_action from esp-build-config.json.
echo Use -t to select a target before building or flashing.
echo clean and clean-build preserve sdkconfig and managed_components.
popd
exit /b 0

:help_error
echo.
echo Use "esp-build.bat help" to see available actions.
popd
exit /b 2
