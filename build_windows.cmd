@echo off
setlocal
pushd "%~dp0"
python tools\check_environment.py
if errorlevel 1 goto failed
if "%~1"=="" (
    python "%IDF_PATH%\tools\idf.py" build
) else (
    python "%IDF_PATH%\tools\idf.py" %*
)
set "TDSH_BUILD_EXIT=%ERRORLEVEL%"
popd
exit /b %TDSH_BUILD_EXIT%
:failed
popd
exit /b 1
