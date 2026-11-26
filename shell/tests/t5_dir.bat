@echo off
cd /d "%~dp0"
dir /b /o:n t*.bat
dir /b /o:-n t*.bat
dir /b /a:-d /o:en t*_*.*
dir /b /l T1_ENV.BAT
dir /b /s t2_data.txt
dir /w /o:n t*.bat | find "t1_env"
dir /d /o:n t*.bat | find "t1_env"
dir /x /o:n t1_env.bat | find "t1_env"
dir /q /o:n t1_env.bat | find "t1_env"
dir /-c /o:n t2_flow.bat | find "t2_flow"
dir /t:c /o:n t1_env.bat | find /c "t1_env"
dir /a:d /b ..
dir /b nothing_here.xyz 2>nul
echo el=%errorlevel%
dir /zz 2>nul
echo el=%errorlevel%
del /a:x nothing.xyz 2>nul
echo el=%errorlevel%
