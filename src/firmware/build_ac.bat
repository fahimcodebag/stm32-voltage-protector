@echo off
rem Same firmware, with defaults for the AC (mid-rail biased) front end used by
rem the MATLAB stimulus in Proteus: VOFF/IOFF 2048, VSPAN 800, ISPAN 40, RESTORE 1.
setlocal
set VP_OUT=VoltageProtector_ac
set VP_EXTRA=-DVP_AC_FRONTEND=1
call "%~dp0build.bat"
endlocal
