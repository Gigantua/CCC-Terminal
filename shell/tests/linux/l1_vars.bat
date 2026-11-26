@echo off
set A=Hello World
echo [%A%] [%A:~0,5%] [%A:~-5%] [%A:World=There%] [%A:*o=X%]
set "Q=quoted value"
echo [%Q%]
set /a X=5*3+2, Y=X*2, Z=(Y-4)/3
echo X=%X% Y=%Y% Z=%Z%
set /a M=17 %% 5
echo M=%M%
set CCCVAR_ONE=1
set CCCVAR_TWO=2
set CCCVAR
if %X%==17 (echo eq) else (echo ne)
if /i "abc"=="ABC" echo case-insensitive
if 10 GTR 9 echo numeric compare
if "a" LSS "b" echo string compare
if not defined NOPE_VAR echo undefined ok
setlocal enabledelayedexpansion
set N=0
for %%i in (a b c) do set /a N+=1 & set L=!L!%%i
echo N=!N! L=!L!
endlocal
echo after endlocal N=[%N%]
setlocal
set INNER=1
endlocal
if not defined INNER echo setlocal restored
ver >nul
echo errorlevel=%errorlevel%
cmd_that_does_not_exist_xyz 2>nul
if errorlevel 1 echo missing command sets errorlevel
call :sub one "two words"
echo sub returned %errorlevel%
echo ok && echo and-branch
cmd_that_does_not_exist_xyz 2>nul || echo or-branch
goto :eof
:sub
echo sub args [%1] [%2] [%~2]
exit /b 5
