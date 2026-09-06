$ErrorActionPreference = 'Stop'

$source = Join-Path $PSScriptRoot 'app_gui.cpp'
$output = Join-Path $PSScriptRoot 'get_telemetry.exe'

$rcFile = Join-Path $PSScriptRoot 'app.rc'
$resObj = Join-Path $PSScriptRoot 'app_res.o'
$extraObjects = @()
if (Test-Path -LiteralPath $rcFile) {
    Write-Host "==> Compiling Windows resources ($rcFile)..." -ForegroundColor Cyan
    & windres.exe $rcFile -O coff -o $resObj
    if ($LASTEXITCODE -eq 0 -and (Test-Path -LiteralPath $resObj)) {
        $extraObjects += $resObj
    } else {
        Write-Warning "Failed to compile $rcFile; proceeding without embedded resource."
    }
}

Write-Host "==> Compiling C++ application..." -ForegroundColor Cyan
& g++ -std=c++17 -O2 $source @extraObjects -o $output `
    -mwindows -static -static-libgcc -static-libstdc++ `
    -lcomctl32 -luxtheme -ldwmapi -lgdi32 -lwinmm `
    -lwinhttp -lshell32 -lole32 -luuid -s

if ($LASTEXITCODE -ne 0) {
    throw "Build failed with exit code $LASTEXITCODE"
}
Write-Host "Built $output with embedded icon resource." -ForegroundColor Green

# Ensure the latest Arduino CLI is present
function Ensure-LatestArduinoCli {
    param([string]$targetDir)

    $cliPath = Join-Path $targetDir 'arduino-cli.exe'
    Write-Host "`n==> Checking latest Arduino CLI version from GitHub..." -ForegroundColor Cyan

    try {
        $apiUri = "https://api.github.com/repos/arduino/arduino-cli/releases/latest"
        $release = Invoke-RestMethod -Uri $apiUri -UseBasicParsing -TimeoutSec 10
        $latestTag = $release.tag_name
        $cleanTag = $latestTag.TrimStart('v')

        $needDownload = $true
        if (Test-Path -LiteralPath $cliPath) {
            $versionOutput = & $cliPath version 2>&1 | Out-String
            if ($versionOutput -match "Version:\s*([0-9\.\-a-zA-Z]+)") {
                $currentVersion = $matches[1]
                if ($currentVersion -eq $cleanTag) {
                    Write-Host "Arduino CLI is already up to date ($latestTag)." -ForegroundColor Green
                    $needDownload = $false
                } else {
                    Write-Host "Current Arduino CLI ($currentVersion) differs from latest release ($latestTag). Updating..." -ForegroundColor Yellow
                }
            }
        }

        if ($needDownload) {
            $asset = $release.assets | Where-Object { $_.name -like "*Windows_64bit.zip" } | Select-Object -First 1
            if (-not $asset) {
                Write-Warning "Could not find Windows 64-bit asset in the latest Arduino CLI release."
                return
            }

            $tempZip = Join-Path $env:TEMP ("arduino-cli_" + [guid]::NewGuid().ToString("N") + ".zip")
            $tempExtract = Join-Path $env:TEMP ("arduino-cli_" + [guid]::NewGuid().ToString("N"))

            $assetSizeMb = [math]::Round($asset.size / 1MB, 2)
            Write-Host "Downloading Arduino CLI $latestTag ($assetSizeMb MB)..." -ForegroundColor Cyan
            Invoke-WebRequest -Uri $asset.browser_download_url -OutFile $tempZip -UseBasicParsing

            Write-Host "Extracting to $cliPath..." -ForegroundColor Cyan
            Expand-Archive -LiteralPath $tempZip -DestinationPath $tempExtract -Force

            $extractedCli = Join-Path $tempExtract 'arduino-cli.exe'
            if (Test-Path -LiteralPath $extractedCli) {
                Copy-Item -LiteralPath $extractedCli -Destination $cliPath -Force
                Write-Host "Successfully updated Arduino CLI to $latestTag!" -ForegroundColor Green
            }

            # Cleanup
            Remove-Item -LiteralPath $tempZip -Force -ErrorAction SilentlyContinue
            Remove-Item -LiteralPath $tempExtract -Recurse -Force -ErrorAction SilentlyContinue
        }
    } catch {
        if (Test-Path -LiteralPath $cliPath) {
            Write-Warning "Could not check/download latest Arduino CLI ($($_.Exception.Message)). Using existing local binary."
        } else {
            Write-Error "Failed to download Arduino CLI: $($_.Exception.Message)"
        }
    }
}

Ensure-LatestArduinoCli -targetDir $PSScriptRoot
