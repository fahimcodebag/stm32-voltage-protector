@echo off
cd /d D:\VoltageProtector\stimulus
"D:\MATLAB_install_dir\bin\matlab.exe" -batch "proteus_stimulus"
echo MATLAB_EXIT=%ERRORLEVEL%
