# adm installer for Windows.
#
#   powershell -ExecutionPolicy Bypass -c "irm https://raw.githubusercontent.com/gslf/adm/main/install.ps1 | iex"
#
# Options (environment variables):
#   ADM_VERSION          release tag to install (default: latest)
#   ADM_INSTALL_DIR      install directory (default: %LOCALAPPDATA%\Programs\adm)
#   ADM_NO_MODIFY_PATH   set to 1 to leave the user PATH untouched
#
# ADM_DOWNLOAD_BASE overrides the URL the release assets are fetched from
# (used by CI to test unpublished builds).

$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'

function Install-Adm {
  $repo = 'gslf/adm'
  $version = $env:ADM_VERSION
  $installDir = if ($env:ADM_INSTALL_DIR) { $env:ADM_INSTALL_DIR } else { Join-Path $env:LOCALAPPDATA 'Programs\adm' }
  $modifyPath = $env:ADM_NO_MODIFY_PATH -ne '1'

  # Windows PowerShell 5.1 does not enable TLS 1.2 by default.
  [Net.ServicePointManager]::SecurityProtocol = [Net.ServicePointManager]::SecurityProtocol -bor [Net.SecurityProtocolType]::Tls12

  if (-not $version) {
    # Follow the /releases/latest redirect: unlike the API it is not rate limited.
    $request = [Net.WebRequest]::Create("https://github.com/$repo/releases/latest")
    $response = $request.GetResponse()
    $version = $response.ResponseUri.Segments[-1]
    $response.Close()
    if (-not $version) { throw 'cannot determine the latest release; set ADM_VERSION' }
  }
  if (-not $version.StartsWith('v')) { $version = "v$version" }

  # Only x86_64 builds are published; Windows on ARM runs them under emulation.
  $target = 'windows-x86_64'
  $base = if ($env:ADM_DOWNLOAD_BASE) { $env:ADM_DOWNLOAD_BASE } else { "https://github.com/$repo/releases/download/$version" }
  $archive = "adm-$version-$target.zip"

  $tmp = Join-Path ([IO.Path]::GetTempPath()) ("adm-install-" + [Guid]::NewGuid())
  New-Item -ItemType Directory -Path $tmp | Out-Null
  try {
    Write-Host "adm-install: downloading adm $version ($target)"
    $zip = Join-Path $tmp $archive
    Invoke-WebRequest -UseBasicParsing -Uri "$base/$archive" -OutFile $zip

    $sums = Join-Path $tmp 'SHA256SUMS.txt'
    $haveSums = $true
    try { Invoke-WebRequest -UseBasicParsing -Uri "$base/SHA256SUMS.txt" -OutFile $sums } catch { $haveSums = $false }
    if ($haveSums) {
      $expected = $null
      foreach ($line in Get-Content $sums) {
        $parts = $line -split '\s+', 2
        if ($parts.Count -eq 2 -and $parts[1].TrimStart('*') -eq $archive) { $expected = $parts[0] }
      }
      if (-not $expected) { throw "no checksum for $archive in SHA256SUMS.txt" }
      $actual = (Get-FileHash -Algorithm SHA256 $zip).Hash
      if ($actual -ne $expected.ToUpperInvariant()) { throw "checksum mismatch for $archive" }
      Write-Host 'adm-install: checksum verified'
    } else {
      Write-Host 'adm-install: warning: SHA256SUMS.txt not available, skipping checksum verification'
    }

    # Extract next to the destination (Move-Item cannot cross drives), then
    # replace the previous installation as a whole so stale files do not linger.
    $parent = Split-Path -Parent $installDir
    if ($parent) { New-Item -ItemType Directory -Force -Path $parent | Out-Null }
    $staging = "$installDir.new"
    if (Test-Path $staging) { Remove-Item -Recurse -Force $staging }
    Expand-Archive -Path $zip -DestinationPath $staging
    if (-not (Test-Path (Join-Path $staging 'adm.exe'))) {
      Remove-Item -Recurse -Force $staging
      throw 'archive does not contain adm.exe'
    }
    if (Test-Path $installDir) { Remove-Item -Recurse -Force $installDir }
    Move-Item $staging $installDir
  } finally {
    Remove-Item -Recurse -Force $tmp -ErrorAction SilentlyContinue
  }

  Write-Host "adm-install: installed $installDir\adm.exe"
  if (Test-Path "$installDir\adm.conf.example") {
    Write-Host "adm-install: example configuration: $installDir\adm.conf.example (copy to %USERPROFILE%\.adm.conf)"
  }

  # Read the raw registry value so %VARIABLE% entries stay unexpanded.
  $key = Get-Item 'HKCU:\Environment'
  $userPath = $key.GetValue('Path', '', 'DoNotExpandEnvironmentNames')
  $entries = @($userPath -split ';' | Where-Object { $_ })
  if ($entries -contains $installDir -or $entries -contains "$installDir\") { return }
  if (-not $modifyPath) {
    Write-Host "adm-install: $installDir is not in PATH; add it to use adm"
    return
  }
  Set-ItemProperty -Path 'HKCU:\Environment' -Name 'Path' -Type ExpandString -Value (($entries + $installDir) -join ';')
  # Setting a variable through .NET broadcasts WM_SETTINGCHANGE, so new
  # terminals started from Explorer see the updated PATH.
  [Environment]::SetEnvironmentVariable('ADM_INSTALLER_REFRESH', '1', 'User')
  [Environment]::SetEnvironmentVariable('ADM_INSTALLER_REFRESH', $null, 'User')
  $env:Path = "$env:Path;$installDir"
  Write-Host "adm-install: added $installDir to the user PATH (open a new terminal to use adm)"
}

Install-Adm
