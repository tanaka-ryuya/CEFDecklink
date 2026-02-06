@echo off
"C:\Program Files (x86)\Windows Kits\10\Debuggers\x64\cdb.exe" -z crash_20260202_160102.dmp -c ".ecxr; kv 30; dv; q" > crash_analysis_160102.txt 2>&1
type crash_analysis_160102.txt
