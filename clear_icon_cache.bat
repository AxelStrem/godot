@echo off
echo Clearing Windows icon cache...

echo Stopping Explorer...
taskkill /f /im explorer.exe

echo Deleting icon cache files...
del /a /q "%localappdata%\IconCache.db"
del /a /f /q "%localappdata%\Microsoft\Windows\Explorer\iconcache*"

echo Restarting Explorer...
start explorer.exe

echo Icon cache cleared. Please restart any pinned applications.
pause
