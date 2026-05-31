$ErrorActionPreference = "Stop"

$root = Split-Path -Parent $MyInvocation.MyCommand.Path
$outDir = Join-Path $root "bin\native"
$source = Join-Path $root "MouseLogiNative.cpp"
$probeSource = Join-Path $root "MouseLogiProbe.cpp"
$hidProbeSource = Join-Path $root "MouseLogiHidProbe.cpp"
$exe = Join-Path $outDir "MouseLogiNative.exe"
$probeExe = Join-Path $outDir "MouseLogiProbe.exe"
$hidProbeExe = Join-Path $outDir "MouseLogiHidProbe.exe"
$sampleConfig = Join-Path $root "config.ini.sample"
$runtimeConfig = Join-Path $outDir "config.ini"

New-Item -ItemType Directory -Force -Path $outDir | Out-Null

$vswhere = Join-Path ${env:ProgramFiles(x86)} "Microsoft Visual Studio\Installer\vswhere.exe"
$installPath = $null
if (Test-Path $vswhere) {
    $installPath = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
}

if (-not $installPath) {
    $candidates = @(
        "C:\Program Files\Microsoft Visual Studio\2022\Community",
        "C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools"
    )
    $installPath = $candidates | Where-Object { Test-Path (Join-Path $_ "VC\Auxiliary\Build\vcvars64.bat") } | Select-Object -First 1
}

if (-not $installPath) {
    throw "Could not find Visual Studio C++ Build Tools."
}

$vcvars = Join-Path $installPath "VC\Auxiliary\Build\vcvars64.bat"
if (-not (Test-Path $vcvars)) {
    throw "Could not find vcvars64.bat under $installPath."
}

$cmd = Join-Path $env:TEMP ("build-mouselogi-native-{0}.cmd" -f ([Guid]::NewGuid().ToString("N")))
@"
@echo off
call "$vcvars" >nul
cl.exe /nologo /utf-8 /O2 /MT /EHsc /W4 /DUNICODE /D_UNICODE /Fo"$outDir\MouseLogiNative.obj" /Fe"$exe" "$source" user32.lib kernel32.lib shell32.lib hid.lib setupapi.lib /link /SUBSYSTEM:WINDOWS /OPT:REF /OPT:ICF
if errorlevel 1 exit /b %errorlevel%
cl.exe /nologo /utf-8 /O2 /MT /EHsc /W4 /DUNICODE /D_UNICODE /Fo"$outDir\MouseLogiProbe.obj" /Fe"$probeExe" "$probeSource" user32.lib kernel32.lib /link /SUBSYSTEM:CONSOLE /OPT:REF /OPT:ICF
if errorlevel 1 exit /b %errorlevel%
cl.exe /nologo /utf-8 /O2 /MT /EHsc /W4 /DUNICODE /D_UNICODE /Fo"$outDir\MouseLogiHidProbe.obj" /Fe"$hidProbeExe" "$hidProbeSource" user32.lib kernel32.lib hid.lib setupapi.lib /link /SUBSYSTEM:CONSOLE /OPT:REF /OPT:ICF
"@ | Set-Content -Encoding ASCII -Path $cmd

try {
    & cmd.exe /c "`"$cmd`""
    if ($LASTEXITCODE -ne 0) {
        throw "Native build failed with exit code $LASTEXITCODE."
    }
}
finally {
    Remove-Item -LiteralPath $cmd -Force -ErrorAction SilentlyContinue
}

if (-not (Test-Path $runtimeConfig)) {
    Copy-Item -Path $sampleConfig -Destination $runtimeConfig
}

Remove-Item -LiteralPath (Join-Path $outDir "MouseLogiNative.obj") -Force -ErrorAction SilentlyContinue
Remove-Item -LiteralPath (Join-Path $outDir "MouseLogiProbe.obj") -Force -ErrorAction SilentlyContinue
Remove-Item -LiteralPath (Join-Path $outDir "MouseLogiHidProbe.obj") -Force -ErrorAction SilentlyContinue

Write-Host "Built $exe"
Write-Host "Built $probeExe"
Write-Host "Built $hidProbeExe"
Write-Host "Config: $runtimeConfig"
