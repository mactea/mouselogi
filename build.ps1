$ErrorActionPreference = "Stop"

$root = Split-Path -Parent $MyInvocation.MyCommand.Path
$outDir = Join-Path $root "bin"
$exe = Join-Path $outDir "MouseLogi.exe"
$source = Join-Path $root "Program.cs"
$sampleConfig = Join-Path $root "config.ini.sample"
$runtimeConfig = Join-Path $outDir "config.ini"

New-Item -ItemType Directory -Force -Path $outDir | Out-Null

$candidates = @(
    "C:\Windows\Microsoft.NET\Framework64\v4.0.30319\csc.exe",
    "C:\Windows\Microsoft.NET\Framework\v4.0.30319\csc.exe"
)

$csc = $candidates | Where-Object { Test-Path $_ } | Select-Object -First 1
if (-not $csc) {
    throw "Could not find the .NET Framework C# compiler."
}

& $csc `
    /nologo `
    /target:winexe `
    /optimize+ `
    /platform:anycpu `
    "/out:$exe" `
    /reference:System.Windows.Forms.dll `
    /reference:System.Drawing.dll `
    "$source"

if (-not (Test-Path $runtimeConfig)) {
    Copy-Item -Path $sampleConfig -Destination $runtimeConfig
}

Write-Host "Built $exe"
Write-Host "Config: $runtimeConfig"
