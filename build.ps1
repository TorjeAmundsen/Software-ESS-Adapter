# Builds "Software ESS Adapter.exe" with the C++ toolchain on PATH and packs what end users need
# into dist/software-ess-adapter.zip. Needs CMake, Ninja, and either MinGW-w64 g++ or MSVC cl
# (from a Visual Studio developer prompt) on PATH.
$ErrorActionPreference = "Stop"
Set-Location $PSScriptRoot

cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

cmake --build build --target software-ess-adapter
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

$stage = "build/package"
Remove-Item $stage -Recurse -Force -ErrorAction SilentlyContinue
New-Item "$stage/dolphin" -ItemType Directory | Out-Null
Copy-Item "build/Software ESS Adapter.exe" $stage  # license texts are built into the exe (About window)
Copy-Item "dolphin/Software ESS Adapter.ini" "$stage/dolphin"

New-Item dist -ItemType Directory -Force | Out-Null
Compress-Archive "$stage/*" dist/software-ess-adapter.zip -Force
Write-Host "Built $PSScriptRoot\dist\software-ess-adapter.zip"
