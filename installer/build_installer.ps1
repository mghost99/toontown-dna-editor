# Builds the editor, stages exe + Panda3D runtime, and compiles the Inno Setup installer.
# Usage (PowerShell): installer\build_installer.ps1 [-Panda C:\Panda3D-1.11.0-x64]
param([string]$Panda = "C:\Panda3D-1.11.0-x64", [string]$Version = "0.0.3")
$ErrorActionPreference = "Stop"; $ProgressPreference = "SilentlyContinue"
$root = Split-Path -Parent $PSScriptRoot
& cmd /c "`"$root\build_windows.bat`"" ; if ($LASTEXITCODE) { throw "build failed" }
$stage = "$root\build-win\stage"; Remove-Item $stage -Recurse -Force -ErrorAction SilentlyContinue
New-Item -ItemType Directory "$stage\bin", "$stage\etc" | Out-Null
Copy-Item "$root\build-win\app\dna-editor.exe", "$root\assets\branding\dna-editor.ico" "$stage\bin"
# Panda3D runtime: every DLL the exe (and the GL display plugin) needs, found by walking dumpbin /dependents.
$vc = & "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -property installationPath
$dumpbin = Get-ChildItem "$vc\VC\Tools\MSVC" -Recurse -Filter dumpbin.exe | ?{ $_.FullName -match "Hostx64\\x64" } | select -First 1 -Expand FullName
$todo = New-Object System.Collections.Queue; $seen = @{}
$todo.Enqueue("$stage\bin\dna-editor.exe")
foreach ($seed in "libpandagl.dll", "libpandaegg.dll", "libp3ptloader.dll") { if (Test-Path "$Panda\bin\$seed") { Copy-Item "$Panda\bin\$seed" "$stage\bin"; $todo.Enqueue("$Panda\bin\$seed") } }
while ($todo.Count) {
  $f = $todo.Dequeue()
  foreach ($d in (& $dumpbin /nologo /dependents $f | ?{ $_ -match "^\s+\S+\.dll\s*$" } | %{ $_.Trim() })) {
    $k = $d.ToLower(); if ($seen[$k]) { continue }; $seen[$k] = 1
    $src = Join-Path "$Panda\bin" $d
    if (Test-Path $src) { Copy-Item $src "$stage\bin"; $todo.Enqueue($src) }
  }
}
if (-not (Test-Path "$stage\bin\libpandagl.dll")) { Copy-Item "$Panda\bin\libpandagl.dll" "$stage\bin" }
# MSVC runtime next to the exe (target may not have the VC++ redistributable)
$crt = Get-ChildItem "$vc\VC\Redist\MSVC" -Recurse -Directory -Filter "Microsoft.VC14*.CRT" | ?{ $_.FullName -match "\\x64\\" -and $_.FullName -notmatch "onecore|debug" } | select -First 1
if ($crt) { Copy-Item "$($crt.FullName)\*.dll" "$stage\bin" }
"load-display pandagl`nnotify-level warning`ndefault-directnotify-level warning`nframebuffer-hardware #t`n" | Set-Content "$stage\etc\Config.prc" -Encoding ascii
"staged: " + ((Get-ChildItem "$stage\bin").Name -join " ")
$iscc = @("${env:ProgramFiles(x86)}\Inno Setup 6\ISCC.exe", "$env:LOCALAPPDATA\Programs\Inno Setup 6\ISCC.exe", "$env:ProgramFiles\Inno Setup 6\ISCC.exe") | ?{ Test-Path $_ } | select -First 1
if (-not $iscc) { throw "Inno Setup 6 not found (winget install JRSoftware.InnoSetup)" }
& $iscc /Q "/DAppVersion=$Version" "/DStageDir=$stage" "$PSScriptRoot\dna-editor.iss"; if ($LASTEXITCODE) { throw "ISCC failed" }
Get-Item "$root\build-win\DNA-Editor-Setup-v$Version.exe" | select FullName, Length
