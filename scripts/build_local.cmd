@echo off
call "C:\Program Files\Microsoft Visual Studio\18\Enterprise\VC\Auxiliary\Build\vcvars64.bat" >nul
cmake --build "%~dp0..\build" -- -j8
exit /b %errorlevel%
