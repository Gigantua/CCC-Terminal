@echo off
set T=%TEMP%\ccc_test_fixed
if exist "%T%" rd /s /q "%T%"
md "%T%\a\b\c"
if exist "%T%\a\b\c" echo md nested ok
cd /d "%T%"
echo hello> one.txt
echo world>> one.txt
type one.txt
copy one.txt two.txt >nul
type two.txt
copy one.txt + two.txt three.txt >nul
type three.txt
ren two.txt renamed.txt
if exist renamed.txt echo ren ok
move renamed.txt a\ >nul
if exist a\renamed.txt echo move ok
echo x> f1.log
echo y> f2.log
ren *.log *.bak
dir /b *.bak
del *.bak
dir /b
pushd a\b
echo in %CD:~-3%
popd
echo back %CD:~-8,1%
cd a
cd
cd ..
echo line1> multi.txt
echo line2>> multi.txt
type multi.txt one.txt 2>nul
dir /b /s | find /c "\"
rd /s /q a
if not exist a echo rd ok
del /q *.txt
dir /b 2>nul
cd /d "%TEMP%"
rd /s /q "%T%"
if not exist "%T%" echo cleanup ok
