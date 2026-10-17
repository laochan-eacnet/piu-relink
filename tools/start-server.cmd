@echo off
setlocal
rem Run beside piu.exe so the default database/assets stay under server\.
rem Requires Python 3 only. Arguments are passed to piuserver.py unchanged.
pushd "%~dp0" || exit /b 1
where py >nul 2>&1
if errorlevel 1 goto python
py -3 server\piuserver.py %*
goto done
:python
python server\piuserver.py %*
:done
set "status=%errorlevel%"
popd
if not "%status%"=="0" pause
exit /b %status%
