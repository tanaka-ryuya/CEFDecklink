@echo off
"C:\Program Files (x86)\Windows Kits\10\Debuggers\x64\cdb.exe" -z crash_20260202_151934.dmp -c ".ecxr; k; q" > crash_analysis.txt 2>&1
type crash_analysis.txt
