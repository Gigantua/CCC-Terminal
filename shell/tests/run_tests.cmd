@echo off
setlocal
cd /d "%~dp0"
set FAIL=0
for %%t in (t*_*.bat) do (
  cmd.exe /d /c .\%%t > "%TEMP%\cmd_%%~nt.txt" 2>&1
  ..\..\bin\ccc.exe /d /c .\%%t > "%TEMP%\ccc_%%~nt.txt" 2>&1
  fc "%TEMP%\cmd_%%~nt.txt" "%TEMP%\ccc_%%~nt.txt" > "%TEMP%\diff_%%~nt.txt"
  if errorlevel 1 (echo FAIL %%t & set FAIL=1) else (echo PASS %%t)
)
exit /b %FAIL%
