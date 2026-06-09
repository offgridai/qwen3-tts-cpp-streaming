rmdir /s /q build

powershell -ExecutionPolicy Bypass -File .\build.ps1 -Clean -UseNinja -EnableCuda -EnableCudaGraphs -Configuration Release