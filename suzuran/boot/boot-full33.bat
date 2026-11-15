@echo off
rem RAM-boots loader-xnu-full33.img (built by loader\build-full33.py into the folder above this one).
rem Phone in fastboot mode. Needs the iPhone 8 root on the phone. Writes nothing to flash.
cd /d "%~dp0"
set "FB=C:\Program Files\platform-tools\fastboot.exe"
set "LOG=%~dp0boot-full33.log"
echo === boot loader-xnu-full33 %DATE% %TIME% === > "%LOG%"
"%FB%" devices >> "%LOG%" 2>&1
"%FB%" boot "%~dp0..\loader-xnu-full33.img" >> "%LOG%" 2>&1
echo === done %TIME% === >> "%LOG%"
