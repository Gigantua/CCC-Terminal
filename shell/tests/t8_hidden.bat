@echo off
setlocal
cd /d "%~dp0"
<nul set /p =no newline
echo  after
echo;semicolon
echo/slash
echo,comma
echo=equals
echo+plus
echo[bracket
echo]close
echo\back
echo:colon
echo(paren
echo ^& ^| ^< ^> ^^
set /a "x=0x10+010" >nul
echo hex+octal %x%
set /a y=1^<^<4, z=~0, w=!0, v=10/3, u=-7%%3 >nul
echo shifts %y% %z% %w% %v% %u%
set "a=1" & set "b=2" & echo same line [%a%%b%]
echo next line [%a%%b%]
call echo call doubles %%a%%
set x=
if not defined x echo x undefined
>"%TEMP%\ccc_lead.txt" echo leading redirect
type "%TEMP%\ccc_lead.txt"
2>nul dir /b nonexistent_zz && echo found || echo not found
copy nul "%TEMP%\ccc_empty.txt" >nul
for %%f in ("%TEMP%\ccc_empty.txt") do echo empty size %%~zf
type nul > "%TEMP%\ccc_empty2.txt"
for %%f in ("%TEMP%\ccc_empty2.txt") do echo empty2 size %%~zf
cmd /c exit /b 3
echo exit code %errorlevel%
echo %=ExitCode%
for /f "delims=" %%a in ('echo nested ^& echo second') do echo [%%a]
for /f "tokens=2,* delims=," %%a in ("one,two,three,four") do echo tokens %%a / %%b
for /f "usebackq delims=" %%a in (`echo backq`) do echo %%a
set var=Hello World
echo %var:World=There% %var:~-5% %var:~0,5% %var:*o=0%
echo !var! literal without delayed
setlocal enabledelayedexpansion
set n=0
for /l %%i in (1,1,3) do set /a n+=%%i >nul & echo loop !n!
endlocal
call :args "a b" c "" d
goto :after
:args
echo count args [%1] [%~1] [%2] [%3] [%4] [%*]
shift
echo shifted [%1] [%~2]
goto :eof
:after
if "%~1"=="" echo no batch args
echo %__CD__%x
if /i "ABC"=="abc" echo case insensitive
if 10 gtr 9 echo numeric compare
if "10" gtr "9" echo string compare
ver | find "Windows" >nul && echo pipe ok
(echo a) & (echo b)
echo done
