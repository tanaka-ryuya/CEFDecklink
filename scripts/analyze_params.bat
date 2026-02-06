@echo off
echo Analyzing crash dump parameters...
"C:\Program Files (x86)\Windows Kits\10\Debuggers\x64\cdb.exe" -z crash_20260202_160102.dmp -c ".ecxr; kv; .frame 1; dv; dp; r; q" > crash_params.txt 2>&1
type crash_params.txt
