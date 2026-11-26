@echo off
set T=%TEMP%\ccc_test_messages
if exist "%T%" rd /s /q "%T%"
md "%T%"
cd /d "%T%"
echo ECHO-STATE
echo.
echo
echo on
echo
@echo off
verify
verify on
verify
verify off
verify x
echo el=%errorlevel%
echo UNKNOWN
nosuchcommand_xyz
echo el=%errorlevel%
echo MD
md sub
md sub
echo el=%errorlevel%
echo COPY
echo data> one.txt
copy one.txt one.txt
echo el=%errorlevel%
copy missing.txt x.txt
echo el=%errorlevel%
copy one.txt two.txt
copy *.txt sub
copy /X one.txt two.txt
echo MOVE
move missing.txt x.txt
move one.txt three.txt
move *.txt one.txt
move sub sub2
echo REN
ren missing.txt x.txt
echo x> a.txt
echo x> b.txt
ren a.txt b.txt
echo DEL
del missing.txt
del /X a.txt
echo TYPE
type missing.txt
echo DIR
dir missing.txt 2>&1 | find /v "Volume"
dir /b missing.txt
echo el=%errorlevel%
dir /Y
dir /b
echo SETA
set /a 1/0
echo el=%errorlevel%
set /a 5%%0
echo el=%errorlevel%
set /a 1+
echo el=%errorlevel%
set /a 08
echo el=%errorlevel%
set /a 1 2
echo el=%errorlevel%
set /a (1
echo el=%errorlevel%
set /a
echo el=%errorlevel%
echo SET
set nosuchvariable_xyz
echo el=%errorlevel%
echo SETLOCAL
setlocal bogus
endlocal
echo GOTO
call :nolabel
call :missing
echo FOR
for /f %%a in (missing.txt) do echo %%a
echo el=%errorlevel%
echo PROMPTS
pause < nul
echo n> no.txt
copy /-y three.txt two.txt < no.txt
rd /s sub2 < no.txt
del /p two.txt < no.txt
echo LINKS
mklink /H hard.txt three.txt
mklink /J junction sub2
del /s hard.txt
echo ASSOC
assoc .nosuchext_xyz
ftype nosuchtype_xyz
echo SUMMARY
md sums
for /l %%i in (1,1,100) do echo 0123456789abc>> sums\big.txt
echo x> sums\small.txt
dir /s sums | findstr /b /c:"     " | findstr /v /c:"free" /c:"frei"
echo SYNTAX
rd
echo NOPATH
setlocal
set PATH=
path
endlocal
echo CALLBAT
call missing_batch_xyz.bat
cd /d "%TEMP%"
rd /s /q "%T%"
goto nolabel_xyz
echo not reached

:nolabel
goto
echo not reached either

:missing
goto missing_label_xyz
echo not reached at all
