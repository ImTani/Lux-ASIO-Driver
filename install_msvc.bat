@echo off
echo ========================================================
echo Installing Visual Studio 2022 C++ Build Tools and ATL...
echo ========================================================
winget install Microsoft.VisualStudio.2022.BuildTools -e --override "--passive --wait --add Microsoft.VisualStudio.Workload.VCTools --add Microsoft.VisualStudio.Component.VC.ATL --includeRecommended"
pause
