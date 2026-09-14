@echo off
REM .cmd files aren't gated by PowerShell's script execution policy, which
REM matters here because Google Drive's virtual G: drive reports as FAT32
REM (see fsutil), so RemoteSigned can't read a normal Zone.Identifier tag on
REM it and blocks .ps1 scripts stored there outright.
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0build.ps1" %*
