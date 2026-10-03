<#
.SYNOPSIS
  Installs fs25vr (stereoscopic 6DOF VR for Farming Simulator 25).

.DESCRIPTION
  Copies the bridge (dinput8.dll, openxr_loader.dll, fs25vr.ini) into <game>\x64 and the
  FS25_VR mod into your mods folder (honours the in-game mods-folder override).
  Works from a release package (x64\ + mod\ folders) or from a source checkout after build.bat.

.PARAMETER GameDir
  Farming Simulator 25 install folder. Detected from Steam if omitted.

.PARAMETER EyeResolution
  Switch the game to a window of -EyeWidth x -EyeHeight (vsync off, D3D12). Backs up game.xml.
  The bridge logs the ideal size for your headset in x64\fs25vr.log
  ("recommended render size for the centred frustum").

.PARAMETER AutoResolution
  Like -EyeResolution, but reads the ideal size for your headset from x64\fs25vr.log
  (written during your first VR session).

.PARAMETER Uninstall
  Removes the bridge and the mod and restores game.xml if it was changed.

.EXAMPLE
  powershell -ExecutionPolicy Bypass -File install.ps1
  powershell -ExecutionPolicy Bypass -File install.ps1 -EyeResolution -EyeWidth 5672 -EyeHeight 4336
  powershell -ExecutionPolicy Bypass -File install.ps1 -Uninstall
#>
param(
    [string]$GameDir = "",
    [string]$ProfileDir = "$([Environment]::GetFolderPath('MyDocuments'))\My Games\FarmingSimulator2025",
    [int]$EyeWidth = 2448,
    [int]$EyeHeight = 2448,
    [switch]$EyeResolution,
    [switch]$AutoResolution,
    [switch]$Uninstall
)
$ErrorActionPreference = "Stop"
$root = $PSScriptRoot

function Find-GameDir {
    $candidates = @()
    try {
        $steam = (Get-ItemProperty "HKCU:\Software\Valve\Steam" -ErrorAction Stop).SteamPath
        $vdf = Join-Path $steam "steamapps\libraryfolders.vdf"
        $libs = @($steam)
        if (Test-Path $vdf) {
            $libs += (Select-String -Path $vdf -Pattern '"path"\s+"([^"]+)"' -AllMatches).Matches |
                ForEach-Object { $_.Groups[1].Value.Replace('\\', '\') }
        }
        $candidates += $libs | ForEach-Object { Join-Path $_ "steamapps\common\Farming Simulator 25" }
    } catch { }
    # installed-programs list (standalone / GIANTS store / other installers)
    foreach ($key in "HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall\*",
                     "HKLM:\SOFTWARE\WOW6432Node\Microsoft\Windows\CurrentVersion\Uninstall\*",
                     "HKCU:\SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall\*") {
        Get-ItemProperty $key -ErrorAction SilentlyContinue |
            Where-Object { $_.DisplayName -match "Farming Simulator (25|2025)" -and $_.InstallLocation } |
            ForEach-Object { $candidates += $_.InstallLocation }
    }
    # Epic Games Store
    $epic = Join-Path $env:ProgramData "Epic\EpicGamesLauncher\Data\Manifests"
    if (Test-Path $epic) {
        Get-ChildItem $epic -Filter *.item -ErrorAction SilentlyContinue | ForEach-Object {
            try {
                $m = Get-Content $_.FullName -Raw | ConvertFrom-Json
                if ($m.DisplayName -match "Farming Simulator 25") { $candidates += $m.InstallLocation }
            } catch { }
        }
    }
    $candidates += "C:\Program Files (x86)\Farming Simulator 2025", "C:\Program Files\Farming Simulator 2025"
    foreach ($c in $candidates) {
        if (Test-Path (Join-Path $c "x64\FarmingSimulator2025Game.exe")) { return $c }
    }
    return $null
}

function Test-GameDir($dir) { return $dir -and (Test-Path (Join-Path $dir "x64\FarmingSimulator2025Game.exe")) }

if (-not $GameDir) { $GameDir = Find-GameDir }
while (-not (Test-GameDir $GameDir)) {
    if ($GameDir) { Write-Host "Farming Simulator 25 was not found in: $GameDir" }
    else { Write-Host "Could not find Farming Simulator 25 automatically." }
    Write-Host "Paste the game's install folder (the one that contains FarmingSimulator2025.exe)"
    Write-Host "and press Enter, or just press Enter to cancel:"
    $answer = Read-Host
    if ($null -eq $answer -or -not $answer.Trim()) { throw "Cancelled - nothing was installed." }
    $GameDir = $answer.Trim().Trim('"')
}
$x64 = Join-Path $GameDir "x64"

if (Get-Process FarmingSimulator2025Game, FarmingSimulator2025 -ErrorAction SilentlyContinue) {
    throw "Farming Simulator 25 is running. Close the game first, then try again."
}

$mods = Join-Path $ProfileDir "mods"
$settingsXml = Join-Path $ProfileDir "gameSettings.xml"
if (Test-Path $settingsXml) {
    $o = ([xml](Get-Content $settingsXml)).gameSettings.modsDirectoryOverride
    if ($o -and $o.active -eq "true" -and $o.directory) { $mods = $o.directory.Replace('/', '\') }
}
$zip = Join-Path $mods "FS25_VR.zip"

if ($Uninstall) {
    foreach ($f in "dinput8.dll", "dinput8.pdb", "openxr_loader.dll", "fs25vr.ini", "fs25vr.log", "fs25vr_profile.csv") {
        Remove-Item (Join-Path $x64 $f) -ErrorAction SilentlyContinue
    }
    Remove-Item $zip -ErrorAction SilentlyContinue
    $bak = Join-Path $ProfileDir "game.xml.fs25vr-backup"
    if (Test-Path $bak) { Copy-Item $bak (Join-Path $ProfileDir "game.xml") -Force; Remove-Item $bak }
    Write-Host "fs25vr removed from $GameDir"
    return
}

# Where the files come from: a release package or a source checkout.
$release = Test-Path (Join-Path $root "x64\dinput8.dll")
if ($release) {
    $dll = Join-Path $root "x64\dinput8.dll"
    $loader = Join-Path $root "x64\openxr_loader.dll"
    $ini = Join-Path $root "x64\fs25vr.ini"
} else {
    $dll = Join-Path $root "build\dinput8.dll"
    $loader = Join-Path $root "third_party\openxr\x64\bin\openxr_loader.dll"
    $ini = Join-Path $root "dist\fs25vr.ini"
    if (-not (Test-Path $dll)) { throw "Build first (build.bat), or run this from a release package." }
}

Copy-Item $dll $x64 -Force
Copy-Item $loader $x64 -Force
if (-not $release) { Copy-Item (Join-Path $root "build\dinput8.pdb") $x64 -Force -ErrorAction SilentlyContinue }
if (-not (Test-Path (Join-Path $x64 "fs25vr.ini"))) { Copy-Item $ini $x64 }  # keep the player's settings

New-Item -ItemType Directory -Force $mods | Out-Null
Remove-Item $zip -ErrorAction SilentlyContinue
if ($release) {
    Copy-Item (Join-Path $root "mod\FS25_VR.zip") $zip
} else {
    # Build the zip by hand: Windows PowerShell 5.1 (.NET Framework) stores "scripts\VRMain.lua" with
    # a backslash, which the game cannot resolve. Entry names must use forward slashes.
    Add-Type -AssemblyName System.IO.Compression
    Add-Type -AssemblyName System.IO.Compression.FileSystem
    $src = (Resolve-Path (Join-Path $root "mod\FS25_VR")).Path
    $archive = [System.IO.Compression.ZipFile]::Open($zip, [System.IO.Compression.ZipArchiveMode]::Create)
    try {
        foreach ($f in Get-ChildItem $src -Recurse -File) {
            $name = $f.FullName.Substring($src.Length + 1).Replace('\', '/')
            [System.IO.Compression.ZipFileExtensions]::CreateEntryFromFile($archive, $f.FullName, $name) | Out-Null
        }
    } finally {
        $archive.Dispose()
    }
}

# Files downloaded from the internet carry Windows' "came from another computer" mark. On some
# PCs that stops the game from loading the (unsigned) DLLs: the mod shows up but VR never starts.
foreach ($f in (Join-Path $x64 "dinput8.dll"), (Join-Path $x64 "openxr_loader.dll"), (Join-Path $x64 "fs25vr.ini"), $zip) {
    if (Test-Path $f) { Unblock-File -Path $f -ErrorAction SilentlyContinue }
}

if ($AutoResolution) {
    $log = Join-Path $x64 "fs25vr.log"
    $m = if (Test-Path $log) { Select-String -Path $log -Pattern 'recommended render size for the centred frustum: (\d+)x(\d+)' | Select-Object -Last 1 }
    if (-not $m) {
        throw "No recommended size yet. Play once in VR (load a savegame with the headset on), quit the game, then run this again."
    }
    $EyeWidth = [int]$m.Matches[0].Groups[1].Value
    $EyeHeight = [int]$m.Matches[0].Groups[2].Value
    $EyeResolution = $true
    Write-Host "Recommended size for your headset: ${EyeWidth}x${EyeHeight}"
}

if ($EyeResolution) {
    $gameXml = Join-Path $ProfileDir "game.xml"
    $bak = "$gameXml.fs25vr-backup"
    if (-not (Test-Path $bak)) { Copy-Item $gameXml $bak }
    [xml]$x = Get-Content $gameXml
    $d = $x.game.graphic.display
    $d.width = "$EyeWidth"; $d.height = "$EyeHeight"; $d.fullscreenMode = "windowed"
    $d.vsync.InnerText = "false"
    $x.game.graphic.renderer = "D3D_12"
    $x.Save($gameXml)
    Write-Host "game.xml: windowed ${EyeWidth}x${EyeHeight}, vsync off, D3D12 (backup: $bak)"
}

Write-Host "fs25vr installed:"
Write-Host "  $x64\dinput8.dll, openxr_loader.dll, fs25vr.ini"
Write-Host "  $zip  (enable 'VR (OpenXR, stereoscopic 6DOF)' when starting a savegame)"
