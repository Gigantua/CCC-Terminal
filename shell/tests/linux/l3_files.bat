@echo off
if exist work rd /s /q work
md work
cd work
md sub
echo hello> one.txt
echo world>> one.txt
type one.txt
copy one.txt two.txt >nul
copy one.txt + two.txt three.txt >nul
type three.txt
ren two.txt renamed.txt
if exist renamed.txt echo ren ok
move renamed.txt sub >nul
cd sub
if exist renamed.txt echo move ok
cd ..
echo x> f1.log
echo y> f2.log
ren *.log *.bak
dir /b *.bak
for %%f in (*.bak) do echo found %%f
del *.bak
if not exist f1.bak echo del ok
dir /b
pushd sub
dir /b
popd
echo hidden 2>nul 1>&2
(echo grouped one& echo grouped two) > group.txt
type group.txt
echo appended>> group.txt
for /f %%n in ('type group.txt') do echo line %%n
echo piped value| set /p PIPED=
echo piped=[%PIPED%]
set /p FIRST=< one.txt
echo first=[%FIRST%]
nonexistent_command_xyz > out.txt 2>&1
for %%s in (out.txt) do if %%~zs GTR 0 echo error captured
echo exit code test & call :code
echo code=%errorlevel%
cd ..
rd /s /q work
if not exist work echo cleanup ok
goto :eof
:code
exit /b 6
