@echo off
rem Send recovery.bin (next to this script) to the WR1500 in recovery mode.
rem Usage: tftp.bat [router-ip]      (default 192.168.1.6)
setlocal
set "DIR=%~dp0"

if not exist "%DIR%recovery.bin" (
    echo recovery.bin not found in %DIR%
    goto fail
)

set "PY="
py -3 -c "import sys; sys.exit(sys.version_info < (3, 7))" >nul 2>&1 && set "PY=py -3"
if not defined PY python -c "import sys; sys.exit(sys.version_info < (3, 7))" >nul 2>&1 && set "PY=python"
if not defined PY python3 -c "import sys; sys.exit(sys.version_info < (3, 7))" >nul 2>&1 && set "PY=python3"
if not defined PY (
    echo Python 3.7+ is required: https://www.python.org/downloads/
    goto fail
)

%PY% "%DIR%recovery_install.py" "%DIR%recovery.bin" %*
if errorlevel 1 goto fail
pause
exit /b 0

:fail
pause
exit /b 1
