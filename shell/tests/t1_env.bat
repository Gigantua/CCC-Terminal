@echo off
set A=Hello World
echo [%A%]
echo [%A:~0,5%] [%A:~-5%] [%A:~6%] [%A:~-5,3%] [%A:~2,-3%]
echo [%A:World=There%] [%A:*o=X%] [%A:l=%]
set B=
echo B is [%B%]
set "C=quoted value"
echo [%C%]
set A
set /a X=5*3+2
echo X=%X%
set /a Y=X*2, Z=(Y-4)/3
echo Y=%Y% Z=%Z%
set /a M=17 %% 5
echo M=%M%
set /a S=1^<^<4
echo S=%S%
set /a B1=0x1F ^& 0x0C, B2=6 ^| 9, B3=5 ^^ 3
echo B1=%B1% B2=%B2% B3=%B3%
set /a N=-7/2
echo N=%N%
set /a N+=10
echo N=%N%
set /a NOT1=!0, NOT2=~5
echo NOT1=%NOT1% NOT2=%NOT2%
set /a O=010 + 0x10
echo O=%O%
echo pct=%%
echo [%UNDEFINED_VAR%]
set ERR=
if defined A echo A defined
if not defined ERR echo ERR undefined
set LONGNAME_1=one
set LONGNAME_2=two
set LONGNAME
