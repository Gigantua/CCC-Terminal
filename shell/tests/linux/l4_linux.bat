@echo off
printf "[%%s]" plain "two words" "quote\"inside" back\\slash
echo.
sh -c "exit 3"
echo sh exit=%errorlevel%
true && echo true ok
false || echo false ok
for %%p in (sh) do if not "%%~$PATH:p"=="" echo found sh on PATH
for %%p in (sh) do echo name=%%~nxp
for /f %%k in ('uname -s') do echo kernel=%%k
uname -s | tr a-z A-Z
echo lower| tr a-z A-Z
if exist /etc/hostname echo absolute path ok
md /tmp/ccc_linux_smoke
cd /tmp/ccc_linux_smoke
echo data> /tmp/ccc_linux_smoke/file.txt
type /tmp/ccc_linux_smoke/file.txt
dir /b /tmp/ccc_linux_smoke
copy /y /tmp/ccc_linux_smoke/file.txt /tmp/ccc_linux_smoke/copy.txt >nul
dir /b
echo %CD%
cd /d /
cd
rd /s /q /tmp/ccc_linux_smoke
if not exist /tmp/ccc_linux_smoke echo removed
set PATH=/nonexistent:%PATH%
sh -c "echo still found"
echo HOME is set& if defined HOME echo yes
