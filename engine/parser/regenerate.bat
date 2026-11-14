@echo off
setlocal
if "%~1"=="" (
    echo Usage: regenerate.bat ^<configured-user-preset^> 1>&2
    exit /b 2
)
pushd "%~dp0../.."
if errorlevel 1 exit /b %errorlevel%
call cmake\WithVs.cmd cmake --build --preset "%~1" --target toad_syntax
set "regenerate_result=%errorlevel%"
popd
exit /b %regenerate_result%
