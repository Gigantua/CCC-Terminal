@echo off
set n=0
:dup
set /a n+=1
echo first dup %n%
if %n% lss 3 goto dup
goto next
:dup
echo second dup found by the forward search
:next
echo after next
for /l %%i in (1,1,5) do (
  echo loop %%i
  if %%i==3 goto outloop
)
echo not reached
:outloop
echo out of loop
(
  echo block one
  goto afterblock
  echo block two not reached
)
:afterblock
echo after block
call :sub A
echo sub returned %errorlevel%
call :sub B & echo same line after call
for %%x in (p q r) do call :sub %%x
goto :tail
:sub
echo in sub %1
if "%1"=="B" exit /b 7
goto :eof
:tail
echo tail errorlevel %errorlevel%
set k=0
:count
set /a k+=1
if %k% lss 5 goto count
echo count %k%
echo @echo chained %%1> "%TEMP%\ccc_chain.bat"
"%TEMP%\ccc_chain.bat" done
echo never printed after chain
