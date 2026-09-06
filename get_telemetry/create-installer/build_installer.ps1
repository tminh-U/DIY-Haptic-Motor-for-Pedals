$ErrorActionPreference = 'Stop'

$scriptDir = $PSScriptRoot
$projectRoot = Split-Path $scriptDir -Parent
$issFile = Join-Path $scriptDir 'installer.iss'

Write-Host "==> Step 1: Compiling application..." -ForegroundColor Cyan
& (Join-Path $projectRoot 'build.ps1')

Write-Host "`n==> Step 2: Locating Inno Setup Compiler (ISCC.exe)..." -ForegroundColor Cyan
$isccCandidates = @(
    (Get-Command ISCC.exe -ErrorAction SilentlyContinue | Select-Object -ExpandProperty Source -First 1),
    "C:\Program Files\Inno Setup 7\ISCC.exe",
    "C:\Program Files\Inno Setup 6\ISCC.exe",
    "C:\Program Files (x86)\Inno Setup 6\ISCC.exe",
    (Join-Path $env:LOCALAPPDATA 'Programs\Inno Setup 7\ISCC.exe'),
    (Join-Path $env:LOCALAPPDATA 'Programs\Inno Setup 6\ISCC.exe')
) | Where-Object { $_ -and (Test-Path -LiteralPath $_) }

$iscc = $null
if ($isccCandidates.Count -gt 0) {
    $iscc = $isccCandidates[0]
} else {
    # Check scratch / local cache if present
    $scratchIscc = "C:\Users\tminh\.gemini\antigravity-cli\brain\860c68ba-bc82-465c-95e9-e290369c05f7\scratch\inno\ISCC.exe"
    if (Test-Path -LiteralPath $scratchIscc) {
        $iscc = $scratchIscc
    }
}

if (-not $iscc) {
    Write-Error "Inno Setup Compiler (ISCC.exe) was not found.`nPlease install Inno Setup from https://jrsoftware.org/isdl.php or run:`nwinget install --id JRSoftware.InnoSetup"
}

Write-Host "Using compiler: $iscc" -ForegroundColor Green
Write-Host "`n==> Step 3: Compiling installer package..." -ForegroundColor Cyan
& $iscc $issFile

if ($LASTEXITCODE -ne 0) {
    throw "Inno Setup compilation failed with exit code $LASTEXITCODE"
}

$outputDir = Join-Path $scriptDir 'Output'
$latestInstaller = Get-ChildItem -Path $outputDir -Filter '*.exe' | Sort-Object LastWriteTime -Descending | Select-Object -First 1

if ($latestInstaller) {
    $sizeMb = [math]::Round($latestInstaller.Length / 1MB, 2)
    Write-Host "`n[SUCCESS] Installer created successfully!" -ForegroundColor Green
    Write-Host "File: $($latestInstaller.FullName)" -ForegroundColor Yellow
    Write-Host "Size: $sizeMb MB" -ForegroundColor Yellow
}
