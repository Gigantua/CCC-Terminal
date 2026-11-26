@echo off
if 1==1 echo eq1
if not 1==2 echo neq
if /i "abc"=="ABC" echo ieq
if 10 GTR 9 echo numeric gtr
if "10" LSS "9" echo string lss
if 5 EQU 5 (echo paren then) else (echo paren else)
if 5 EQU 6 (echo wrong) else (echo right else)
if exist t2_flow.bat echo exists
if not exist nothere.txt echo not exists
cmd /c exit 3
if errorlevel 3 echo el3
if errorlevel 4 echo el4
for %%i in (a b c) do echo item %%i
for /l %%n in (1,2,7) do echo n=%%n
for /l %%n in (5,-2,1) do echo down=%%n
for /f "tokens=1,3 delims=," %%a in ("x,y,z") do echo a=%%a b=%%b
for /f "tokens=2*" %%a in ("one two three four") do echo t2=%%a rest=%%b
for /f "skip=1 eol=#" %%l in (t2_data.txt) do echo line=%%l
for /f "usebackq delims=" %%l in (`echo from command`) do echo got=%%l
for %%f in (t2_flow.bat) do echo name=%%~nf ext=%%~xf
call :sub one "two words" three
echo after sub errorlevel=%errorlevel%
set V=outer
setlocal
set V=inner
echo in local V=%V%
endlocal
echo after endlocal V=%V%
setlocal enabledelayedexpansion
set CNT=0
for %%i in (1 2 3) do set /a CNT+=%%i & echo running !CNT!
echo total=!CNT!
set W=first
(
  set W=second
  echo percent=%W% bang=!W!
)
endlocal
goto skip
echo SHOULD NOT PRINT
:skip
echo jumped
call :shifty a b c d
goto :eof

:sub
echo sub args: %1 %2 %3 all=%*
echo stripped=%~2
exit /b 7

:shifty
echo before %1 %2
shift
echo after %1 %2
shift /2
echo shift2 %1 %2
goto :eof
