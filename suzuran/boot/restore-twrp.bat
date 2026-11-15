@echo off
rem Restore TWRP to the recovery partition (FOTAKernel). Needs the phone in fastboot.
cd /d "%~dp0"
set "FB=C:\Program Files\platform-tools\fastboot.exe"
set "LOG=%~dp0restore-twrp.log"
echo === restore twrp %DATE% %TIME% === > "%LOG%"
"%FB%" devices >> "%LOG%" 2>&1
"%FB%" flash recovery "%~dp0..\TWRP_3.7.0_9-0-recovery_suzuran_2025-03-15.img" >> "%LOG%" 2>&1
echo === done %TIME% === >> "%LOG%"
