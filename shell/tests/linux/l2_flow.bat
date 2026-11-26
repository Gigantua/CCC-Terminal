@echo off
for /l %%i in (1,2,7) do echo l=%%i
for %%w in (alpha "beta gamma" delta) do echo w=%%~w
for /f "tokens=1,3 delims=," %%a in ("x,y,z") do echo f=%%a/%%b
for /f "tokens=2*" %%a in ("one two three four") do echo rest=%%a+%%b
for /f "usebackq skip=1 eol=#" %%l in ("%~dp0l2_data.txt") do echo data=%%l
for /f "usebackq tokens=*" %%l in (`echo from command`) do echo cmd=%%l
for /f "delims=" %%l in ('call echo nested call') do echo nested=%%l
set COUNT=0
:loop
set /a COUNT+=1
if %COUNT% LSS 3 goto loop
echo count=%COUNT%
call :args a b c d
call :tilde "some file.txt"
call "%~dp0l2_helper.bat" from-main
echo helper errorlevel=%errorlevel%
goto end
:args
echo first=%1
shift
echo after shift=%1 all=%*
goto :eof
:tilde
echo name=%~n1 ext=%~x1 quoted=%1 unquoted=%~1
goto :eof
:end
echo done
