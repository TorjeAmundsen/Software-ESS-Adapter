# Builds "Software ESS Adapter.exe" with the C++ toolchain on PATH and packs what end users need
# into dist/software-ess-adapter.zip. Needs CMake, Ninja, and either MSVC cl (from a Visual Studio
# developer prompt) or MinGW-w64 g++ on PATH. cl wins if both are there.
$ErrorActionPreference = "Stop"
Set-Location $PSScriptRoot

# One build folder per compiler, so switching between them never trips over a cached compiler.
if (Get-Command cl -ErrorAction SilentlyContinue) {
    $build = "build/msvc"
    $compilers = "-DCMAKE_C_COMPILER=cl", "-DCMAKE_CXX_COMPILER=cl"
} else {
    $build = "build/mingw"
    $compilers = "-DCMAKE_C_COMPILER=gcc", "-DCMAKE_CXX_COMPILER=g++"
}

cmake -S . -B $build -G Ninja -DCMAKE_BUILD_TYPE=Release @compilers
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

cmake --build $build --target software-ess-adapter
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

$stage = "$build/package"
Remove-Item $stage -Recurse -Force -ErrorAction SilentlyContinue
New-Item "$stage/dolphin" -ItemType Directory | Out-Null
Copy-Item "$build/Software ESS Adapter.exe" $stage  # license texts are built into the exe (About window)
Copy-Item "dolphin/Software ESS Adapter.ini" "$stage/dolphin"

New-Item dist -ItemType Directory -Force | Out-Null
Compress-Archive "$stage/*" dist/software-ess-adapter.zip -Force
Write-Host "Built $PSScriptRoot\dist\software-ess-adapter.zip ($build)"
