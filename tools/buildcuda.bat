@echo off
set "ROOT=%~dp0.."
pushd "%ROOT%"
if exist build rmdir /s /q build
powershell -ExecutionPolicy Bypass -File .\engine\build.ps1 -Clean -UseNinja -EnableCuda -EnableCudaGraphs -Configuration Release
popd
