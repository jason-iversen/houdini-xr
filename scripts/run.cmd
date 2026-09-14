@echo off
REM See build.cmd for why this goes through cmd rather than ./run.ps1 directly.
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0run.ps1" %*
