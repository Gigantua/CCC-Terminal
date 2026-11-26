@echo off
echo a & echo b
echo ok && echo and-ran
cmd /c exit 1 || echo or-ran
cmd /c exit 1 && echo SHOULD NOT
echo piped | find "piped"
echo esc ^& amp ^| pipe ^> gt
echo "quoted & kept | too"
echo caret^
continued
(echo grouped 1 & echo grouped 2) > "%TEMP%\ccc_grp.txt"
type "%TEMP%\ccc_grp.txt"
del "%TEMP%\ccc_grp.txt"
echo to stderr 1>&2
echo both 2>&1
nonexistent_command_xyz 2>nul
echo errorlevel after unknown=%errorlevel%
echo.
echo(paren
echo: colon
echo off-text
@echo visible
set X=1
if %X%==1 (
  echo multi
  echo line block
)
echo done
