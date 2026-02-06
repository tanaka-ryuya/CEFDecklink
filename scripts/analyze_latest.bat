@echo off
echo Analyzing all recent crash dumps...
for /f "tokens=*" %%f in ('dir /b /o-d crash_*.dmp 2^>nul') do (
    echo.
    echo ========================================
    echo Analyzing: %%f
    echo ========================================
    "C:\Program Files (x86)\Windows Kits\10\Debuggers\x64\cdb.exe" -z %%f -c ".ecxr; kv 20; r; q" 2>&1 | findstr /i /c:"Exception" /c:"Child-SP" /c:"RetAddr" /c:"Call Site" /c:"rax=" /c:"DeckLink"
    goto :done
)
:done
