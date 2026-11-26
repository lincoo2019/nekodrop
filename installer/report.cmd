@echo off
rem nekodrop diagnostic report: collects the logs and system details a bug report needs,
rem with SSIDs, MAC/IP addresses and names removed, into two files on the Desktop.
rem Double-click it; it asks for administrator rights.
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0report.ps1" %*
