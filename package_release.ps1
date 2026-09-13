# ============================================================================
#  package_release.ps1 -- builds the PerformLive beta installer.
#
#      powershell -ExecutionPolicy Bypass -File package_release.ps1
#
#  Produces  dist\PerformLive-<version>-Windows.exe : one file, the installer
#  itself (Installer & Packaging Standard P1, P28). No zip.
#
#  The version comes from CMakeLists.txt's project(VERSION), which the exe also
#  reads, and the script refuses to run if Main.cpp reports anything else --
#  the filename, the installer and the app cannot disagree (Build Standard B34).
#
#  Needs Inno Setup 6 (ISCC.exe). Unsigned: this is a test build.
# ============================================================================
param([switch]$SkipBuild)
$ErrorActionPreference = "Stop"
trap { Write-Error $_; exit 1 }

$repo  = $PSScriptRoot
$build = Join-Path $repo "build_release"
$exe   = Join-Path $build "EzPlay_artefacts\Release\PerformLive.exe"
$dist  = Join-Path $repo "dist"
$stage = Join-Path $dist "stage"
$wizardDir = Join-Path $dist "wizard"

$iscc = Join-Path $env:LOCALAPPDATA "Programs\Inno Setup 6\ISCC.exe"
if (-not (Test-Path $iscc)) { $iscc = Join-Path ${env:ProgramFiles(x86)} "Inno Setup 6\ISCC.exe" }
if (-not (Test-Path $iscc)) { throw "Inno Setup 6 (ISCC.exe) not found" }

# ---- one version, checked in two places ------------------------------------
$cmakeText = Get-Content (Join-Path $repo "CMakeLists.txt") -Raw
if ($cmakeText -notmatch 'project\(EzPlay VERSION (\d+\.\d+\.\d+)\)') { throw "CMakeLists.txt has no three-part project VERSION" }
$version = $Matches[1]
$mainText = Get-Content (Join-Path $repo "Main.cpp") -Raw
if ($mainText -notmatch ('getApplicationVersion\(\) override\s*\{\s*return "' + [regex]::Escape($version) + '";')) {
  throw "Main.cpp getApplicationVersion() does not return $version -- the versions must agree (B34)"
}

# ---- build -------------------------------------------------------------------
if (-not $SkipBuild) {
  Write-Host "Building Release..." -ForegroundColor Cyan
  $vcvars = "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat"
  $cmake  = "C:\Program Files\Microsoft Visual Studio\18\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
  cmd /c "`"$vcvars`" >nul 2>&1 && `"$cmake`" --build `"$build`" --config Release" | Out-Host
  if ($LASTEXITCODE -ne 0) { throw "Release build failed" }
} else {
  Write-Warning "SkipBuild: packaging the existing exe as it is."
}
if (-not (Test-Path $exe)) { throw "No Release exe at $exe" }

# The same end date Beta.h computes: local midnight, 30 days after its fixed
# kReleaseDate. Read from Beta.h so the installer can never disagree with the app.
$betaText = Get-Content (Join-Path $repo "Beta.h") -Raw
if ($betaText -notmatch 'kReleaseDate = "([A-Z][a-z]{2}) +(\d{1,2}) (\d{4})"') { throw "Beta.h has no kReleaseDate in the expected form" }
$release = [datetime]::ParseExact("$($Matches[1]) $($Matches[2]) $($Matches[3])", "MMM d yyyy", [Globalization.CultureInfo]::InvariantCulture)
$endsAt  = $release.Date.AddDays(30)
$lastDay = $endsAt.AddDays(-1).ToString("d MMMM yyyy", [Globalization.CultureInfo]::GetCultureInfo("en-GB"))

# ---- stage exactly what gets installed ---------------------------------------
if (Test-Path $stage) { Remove-Item $stage -Recurse -Force }
New-Item -ItemType Directory -Force -Path $stage, (Join-Path $stage "fonts"), (Join-Path $stage "art") | Out-Null

Copy-Item $exe (Join-Path $stage "PerformLive.exe")
Copy-Item (Join-Path $repo "fonts\*.ttf") (Join-Path $stage "fonts")
# Baked UI art: the dial housing, the Amanorsac lockup and the creators QR code.
Get-ChildItem (Join-Path $repo "art") -File | Where-Object { $_.Extension -in ".png", ".jpg" } |
  ForEach-Object { Copy-Item $_.FullName (Join-Path $stage "art") }
Copy-Item (Join-Path $repo "logo.png") $stage

$readme = (Get-Content (Join-Path $repo "installer\README.txt") -Raw).Replace("{{VERSION}}", $version).Replace("{{LAST_DAY}}", $lastDay)
if ($readme -match "\{\{") { throw "README.txt still contains a placeholder (B56)" }
Set-Content -Path (Join-Path $stage "README.txt") -Value $readme -Encoding ascii

# ---- refuse to ship anything personal, or any sound --------------------------
$forbidden = Get-ChildItem $stage -Recurse -File | Where-Object {
  $_.Extension -in @(".wav",".mp3",".flac",".aiff",".aif",".ogg",".m4a",".perform",".env",".json",".db",".settings",".log") -or
  $_.Name -like "*autosave*" -or $_.Name -like "*library*"
}
if ($forbidden) {
  Write-Host "REFUSING TO PACKAGE -- these must not ship:" -ForegroundColor Red
  $forbidden | ForEach-Object { Write-Host "  $($_.FullName)" }
  throw "aborted"
}

# ---- wizard artwork: the lockup on black (Master Standard 4) ------------------
Add-Type -AssemblyName System.Drawing
if (Test-Path $wizardDir) { Remove-Item $wizardDir -Recurse -Force }
New-Item -ItemType Directory -Force -Path $wizardDir | Out-Null

function New-WizardBitmap([string]$source, [int]$w, [int]$h, [double]$fill, [string]$out) {
  $src = [System.Drawing.Image]::FromFile($source)
  $bmp = New-Object System.Drawing.Bitmap $w, $h
  $g = [System.Drawing.Graphics]::FromImage($bmp)
  $g.InterpolationMode = [System.Drawing.Drawing2D.InterpolationMode]::HighQualityBicubic
  $g.Clear([System.Drawing.Color]::Black)
  $scale = [Math]::Min($w * $fill / $src.Width, $h * $fill / $src.Height)
  $dw = [int]($src.Width * $scale); $dh = [int]($src.Height * $scale)
  $g.DrawImage($src, [int](($w - $dw) / 2), [int](($h - $dh) / 2), $dw, $dh)
  $g.Dispose(); $src.Dispose()
  $bmp.Save($out, [System.Drawing.Imaging.ImageFormat]::Bmp); $bmp.Dispose()
}
$wizardImage = Join-Path $wizardDir "wizard.bmp"
$wizardSmall = Join-Path $wizardDir "wizard-small.bmp"
New-WizardBitmap (Join-Path $repo "art\amanorsac-logo.jpg") 328 628 0.95 $wizardImage
New-WizardBitmap (Join-Path $repo "logo_icon.png")          110 110 0.80 $wizardSmall

# The exe's own icon, which JUCE generates from logo_icon.png (P19).
$icon = Get-ChildItem $build -Recurse -Filter "icon.ico" -File | Select-Object -First 1
if (-not $icon) { throw "No icon.ico found under $build -- JUCE generates it during the build" }

# ---- compile the installer ----------------------------------------------------
Write-Host "Compiling installer..." -ForegroundColor Cyan
& $iscc /Q "/DAppVersion=$version" "/DStageDir=$stage" "/DIconFile=$($icon.FullName)" `
        "/DWizardImage=$wizardImage" "/DWizardSmall=$wizardSmall" "/DLastDay=$lastDay" `
        "/DLicenceFile=$(Join-Path $repo 'installer\LICENCE.txt')" "/O$dist" `
        (Join-Path $repo "installer\PerformLive.iss")
if ($LASTEXITCODE -ne 0) { throw "Inno Setup failed" }

$installer = Join-Path $dist "PerformLive-$version-Windows.exe"
if (-not (Test-Path $installer)) { throw "Expected $installer was not produced" }
$item = Get-Item $installer

Write-Host ""
Write-Host "Built $($item.Name)" -ForegroundColor Green
Write-Host ("  size:     {0:N0} bytes ({1:N1} MB)" -f $item.Length, ($item.Length / 1MB))
Write-Host "  sha256:   $((Get-FileHash $installer -Algorithm SHA256).Hash)"
Write-Host "  last day: $lastDay"
Write-Host "  signed:   no (test build)"
