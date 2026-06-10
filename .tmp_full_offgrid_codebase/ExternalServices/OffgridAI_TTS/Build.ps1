param(
    [switch]$Clean,
    [switch]$UseNinja,
    [switch]$EnableCuda,
    [switch]$EnableCudaGraphs,
    [ValidateSet('Debug','Release','RelWithDebInfo','MinSizeRel')]
    [string]$Configuration = 'Release',
    [string]$PluginRoot = '',
    [string]$CudaRoot = '',
    [string]$CudaArch = '',
    [string]$CMakeExe = '',
    [string]$NinjaExe = ''
)

$ErrorActionPreference = 'Stop'

function Resolve-ExistingPath([string[]]$Candidates, [string]$Description) {
    foreach ($Candidate in $Candidates) {
        if ([string]::IsNullOrWhiteSpace($Candidate)) { continue }
        if (Test-Path -LiteralPath $Candidate) { return (Resolve-Path -LiteralPath $Candidate).Path }
    }
    throw "Could not find $Description. Checked: $($Candidates -join '; ')"
}

function Resolve-CommandPath([string]$CommandName) {
    $Command = Get-Command $CommandName -ErrorAction SilentlyContinue
    if ($Command) { return $Command.Source }
    return ''
}

$ServiceRoot = Split-Path -Parent $MyInvocation.MyCommand.Path
if ([string]::IsNullOrWhiteSpace($PluginRoot)) {
    $PluginRoot = (Resolve-Path (Join-Path $ServiceRoot '..\..')).Path
} else {
    $PluginRoot = (Resolve-Path -LiteralPath $PluginRoot).Path
}

$BuildDir = Join-Path $ServiceRoot 'build'
$BinaryOutDir = Join-Path $PluginRoot 'Binaries\Win64'

if (-not (Test-Path (Join-Path $ServiceRoot 'CMakeLists.txt'))) {
    throw "CMakeLists.txt not found at $ServiceRoot"
}
if (-not (Test-Path (Join-Path $ServiceRoot 'qwen3-tts-cpp-streaming\engine\src\qwen3_tts.h'))) {
    throw "Missing qwen3-tts-cpp-streaming engine source under $ServiceRoot\qwen3-tts-cpp-streaming\engine"
}
if (-not (Test-Path (Join-Path $ServiceRoot 'qwen3-tts-cpp-streaming\engine\ggml\CMakeLists.txt'))) {
    throw "Missing ggml source under $ServiceRoot\qwen3-tts-cpp-streaming\engine\ggml"
}

if ($Clean -and (Test-Path $BuildDir)) {
    Remove-Item -LiteralPath $BuildDir -Recurse -Force
}
New-Item -ItemType Directory -Force -Path $BuildDir | Out-Null
New-Item -ItemType Directory -Force -Path $BinaryOutDir | Out-Null

if ([string]::IsNullOrWhiteSpace($CMakeExe)) { $CMakeExe = Resolve-CommandPath 'cmake.exe' }
if ([string]::IsNullOrWhiteSpace($CMakeExe)) {
    $CMakeExe = Resolve-ExistingPath @(
        'C:\Program Files\CMake\bin\cmake.exe',
        'C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe',
        'C:\Program Files\Microsoft Visual Studio\2022\Professional\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe',
        'C:\Program Files\Microsoft Visual Studio\2022\Enterprise\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe',
        'C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe'
    ) 'cmake.exe'
}

if ($UseNinja) {
    if ([string]::IsNullOrWhiteSpace($NinjaExe)) { $NinjaExe = Resolve-CommandPath 'ninja.exe' }
    if ([string]::IsNullOrWhiteSpace($NinjaExe)) {
        $NinjaExe = Resolve-ExistingPath @(
            'C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja\ninja.exe',
            'C:\Program Files\Microsoft Visual Studio\2022\Professional\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja\ninja.exe',
            'C:\Program Files\Microsoft Visual Studio\2022\Enterprise\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja\ninja.exe',
            'C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja\ninja.exe'
        ) 'ninja.exe'
    }
}

if ($EnableCuda) {
    if ([string]::IsNullOrWhiteSpace($CudaRoot)) {
        if ($env:CUDA_PATH -and (Test-Path -LiteralPath $env:CUDA_PATH)) {
            $CudaRoot = (Resolve-Path -LiteralPath $env:CUDA_PATH).Path
        } else {
            $CudaRoot = Resolve-ExistingPath @(
                'C:\Program Files\NVIDIA GPU Computing Toolkit\CUDA\v12.8',
                'C:\Program Files\NVIDIA GPU Computing Toolkit\CUDA\v12.9',
                'C:\Program Files\NVIDIA GPU Computing Toolkit\CUDA\v12.6',
                'C:\Program Files\NVIDIA GPU Computing Toolkit\CUDA\v12.4'
            ) 'CUDA Toolkit root'
        }
    }

    $NvccExe = Join-Path $CudaRoot 'bin\nvcc.exe'
    if (-not (Test-Path -LiteralPath $NvccExe)) { throw "nvcc.exe not found at $NvccExe" }

    $env:CUDA_PATH = $CudaRoot
    $env:CUDAToolkit_ROOT = $CudaRoot
    $env:PATH = (Join-Path $CudaRoot 'bin') + ';' + $env:PATH
}

if (-not (Resolve-CommandPath 'cl.exe')) {
    Write-Warning 'cl.exe was not found in PATH. Run this from an x64 Native Tools command prompt, or call vcvars64.bat before this script.'
}

$OffgridCuda = if ($EnableCuda) { 'ON' } else { 'OFF' }
$OffgridCudaGraphs = if ($EnableCudaGraphs) { 'ON' } else { 'OFF' }

# Keep each -D assignment as one complete array element. This avoids CMake seeing a stray "ON"
# positional argument, which was the cause of the previous CPU-only configure.
$ConfigureArgs = New-Object System.Collections.Generic.List[string]
$ConfigureArgs.Add('-S')
$ConfigureArgs.Add($ServiceRoot)
$ConfigureArgs.Add('-B')
$ConfigureArgs.Add($BuildDir)
$ConfigureArgs.Add("-DOFFGRID_TTS_ENABLE_CUDA=$OffgridCuda")
$ConfigureArgs.Add("-DOFFGRID_TTS_ENABLE_CUDA_GRAPHS=$OffgridCudaGraphs")
$ConfigureArgs.Add("-DGGML_CUDA=$OffgridCuda")
$ConfigureArgs.Add("-DGGML_CUDA_GRAPHS=$OffgridCudaGraphs")
$ConfigureArgs.Add('-DGGML_BACKEND_DL=OFF')
$ConfigureArgs.Add('-DGGML_BUILD_EXAMPLES=OFF')
$ConfigureArgs.Add('-DGGML_BUILD_TESTS=OFF')
$ConfigureArgs.Add('-DGGML_STATIC=ON')
$ConfigureArgs.Add('-DBUILD_SHARED_LIBS=OFF')

if ($UseNinja) {
    $ConfigureArgs.Add('-G')
    $ConfigureArgs.Add('Ninja')
    $ConfigureArgs.Add("-DCMAKE_BUILD_TYPE=$Configuration")
    $ConfigureArgs.Add("-DCMAKE_MAKE_PROGRAM=$NinjaExe")
}

if ($EnableCuda) {
    $ConfigureArgs.Add("-DCUDAToolkit_ROOT=$CudaRoot")
    $ConfigureArgs.Add("-DCMAKE_CUDA_COMPILER=$(Join-Path $CudaRoot 'bin\nvcc.exe')")
    if (-not [string]::IsNullOrWhiteSpace($CudaArch)) {
        $ConfigureArgs.Add("-DCMAKE_CUDA_ARCHITECTURES=$CudaArch")
    }
}

Write-Host "[OffgridAI] cmake: $CMakeExe"
if ($UseNinja) { Write-Host "[OffgridAI] ninja: $NinjaExe" }
if ($EnableCuda) { Write-Host "[OffgridAI] cuda:  $CudaRoot" }
Write-Host "[OffgridAI] configuring TTS service..."
Write-Host "[OffgridAI] cmake args: $($ConfigureArgs -join ' ')"
& $CMakeExe @ConfigureArgs
if ($LASTEXITCODE -ne 0) { throw "CMake configure failed with exit code $LASTEXITCODE" }

Write-Host "[OffgridAI] building TTS service..."
$BuildArgs = New-Object System.Collections.Generic.List[string]
$BuildArgs.Add('--build')
$BuildArgs.Add($BuildDir)
if (-not $UseNinja) {
    $BuildArgs.Add('--config')
    $BuildArgs.Add($Configuration)
}
& $CMakeExe @BuildArgs
if ($LASTEXITCODE -ne 0) { throw "Build failed with exit code $LASTEXITCODE" }

$CandidateExes = @(
    (Join-Path $BuildDir 'bin\OffgridAI_TTS.exe'),
    (Join-Path $BuildDir 'OffgridAI_TTS.exe'),
    (Join-Path $BuildDir "$Configuration\OffgridAI_TTS.exe"),
    (Join-Path $BuildDir "bin\$Configuration\OffgridAI_TTS.exe")
)
$BuiltExe = Resolve-ExistingPath $CandidateExes 'built OffgridAI_TTS.exe'
$OutExe = Join-Path $BinaryOutDir 'OffgridAI_TTS.exe'
Copy-Item -LiteralPath $BuiltExe -Destination $OutExe -Force

Get-ChildItem -LiteralPath $BuildDir -Recurse -File -Include '*.dll' -ErrorAction SilentlyContinue | ForEach-Object {
    Copy-Item -LiteralPath $_.FullName -Destination (Join-Path $BinaryOutDir $_.Name) -Force
}

Write-Host "[OffgridAI] SUCCESS: copied $BuiltExe"
Write-Host "[OffgridAI]          to $OutExe"
