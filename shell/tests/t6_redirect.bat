@echo off
set PS=powershell -NoLogo -NoProfile -NonInteractive -Command
set T=%TEMP%\ccc_t6
if exist "%T%" rd /s /q "%T%"
md "%T%"

echo [1 stdout only, stderr to nul]
%PS% "Write-Output 'out line'; [Console]::Error.WriteLine('err line')" 2>nul
echo [2 both merged]
%PS% "[Console]::Out.WriteLine('out first'); [Console]::Out.Flush(); Start-Sleep -Milliseconds 50; [Console]::Error.WriteLine('err second')" 2>&1
echo [3 stderr to stdout, stdout to nul]
%PS% "Write-Output 'hidden'; [Console]::Error.WriteLine('only err')" 2>&1 1>nul
echo [4 stderr to file]
%PS% "[Console]::Error.WriteLine('err to file')" 2> "%T%\err.txt"
type "%T%\err.txt"
echo [5 stdout to file, then append]
%PS% "1..5 | ForEach-Object { $_ * $_ }" > "%T%\out.txt"
%PS% "'appended'" >> "%T%\out.txt"
type "%T%\out.txt"
echo [6 builtin piped into powershell]
echo piped into powershell| %PS% "$input | ForEach-Object { $_.ToUpper() }"
echo [7 block piped into powershell]
(echo a& echo b& echo c) | %PS% "($input | Measure-Object).Count"
echo [8 powershell piped into find]
%PS% "1..12" | find "1"
echo [9 exit code]
%PS% "exit 42"
echo exit=%errorlevel%
%PS% "exit 0"
echo exit=%errorlevel%
echo [10 stdin from file]
%PS% "$t = [Console]::In.ReadToEnd(); ($t -split \"`n\" | Where-Object { $_.Trim() }).Count" < "%T%\out.txt"
echo [11 block redirected as a whole]
(%PS% "'in block'" & echo after block) > "%T%\blk.txt"
type "%T%\blk.txt"
echo [12 FOR /F over powershell output]
for /f "delims=" %%l in ('%PS% "1..3 | ForEach-Object { 'row ' + $_ }"') do echo got %%l
echo [13 environment reaches the child]
set CCC_VAR=hello from the parent
%PS% "$env:CCC_VAR"
echo [14 SET /P from a file]
set /p FIRST=<"%T%\out.txt"
echo FIRST=%FIRST%
echo [15 script file with quoted arguments]
> "%T%\args.ps1" echo $args ^| ForEach-Object { "arg=[$_]" }
powershell -NoLogo -NoProfile -ExecutionPolicy Bypass -File "%T%\args.ps1" one "two words" three
echo [16 large output through a pipe]
%PS% "1..200000" | find /c /v ""
echo [17 large output to a file]
%PS% "1..200000" > "%T%\big.txt"
%PS% "(Get-FileHash -Algorithm MD5 '%T%\big.txt').Hash"
echo [18 nested cmd inside ccc/cmd]
cmd /d /c "echo nested & exit /b 5"
echo nested exit=%errorlevel%
rd /s /q "%T%"
echo done
