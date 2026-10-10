# Builds "Software ESS Adapter.exe" and packs what end users need into
# dist/software-ess-adapter.zip. Needs CMake, Ninja, and either Visual Studio with the C++ workload
# or MinGW-w64 g++ on PATH. MSVC wins if both are there, since releases are built with it too.
$ErrorActionPreference = "Stop"
Set-Location $PSScriptRoot

# Outside a developer prompt, find Visual Studio and load its x64 build environment.
if (-not (Get-Command cl -ErrorAction SilentlyContinue)) {
    $vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
    $vs = if (Test-Path $vswhere) {
        & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
    }
    if ($vs) {
        $env:PATH = "$(Split-Path $vswhere);$env:PATH"  # VS's own setup script calls vswhere too
        Import-Module "$vs\Common7\Tools\Microsoft.VisualStudio.DevShell.dll"
        Enter-VsDevShell -VsInstallPath $vs -SkipAutomaticLocation -DevCmdArguments "-arch=x64 -host_arch=x64" | Out-Null
    }
}

# One build folder per compiler, so switching between them never trips over a cached compiler.
if (Get-Command cl -ErrorAction SilentlyContinue) {
    $build = "build/msvc"
    $compilers = "-DCMAKE_C_COMPILER=cl", "-DCMAKE_CXX_COMPILER=cl"
} else {
    # Old MinGW.org (GCC 6, 32-bit) has no C++17 or std::thread and fails deep in the build.
    $gxx = Get-Command g++ -ErrorAction SilentlyContinue
    $version = if ($gxx) { [int]((& g++ -dumpversion) -split "\.")[0] } else { 0 }
    $machine = if ($gxx) { & g++ -dumpmachine } else { "" }
    if ($version -lt 9 -or $machine -notlike "x86_64-*") {
        $found = if ($gxx) { "$($gxx.Source) is GCC $version for $machine." } else { "No g++ found on PATH." }
        Write-Error "$found Needs MSVC cl or 64-bit MinGW-w64 GCC 9 or newer, such as MSYS2's C:\msys64\mingw64\bin, first on PATH."
    }
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
