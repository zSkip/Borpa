# Downloads the solvers Borpa bundles and lays them out as the app expects:
#   <Destination>\gmsh\gmsh.exe (+ its licence files)
#   <Destination>\calculix\ccx_i4_static.exe
# Pass the folder to CMake as BORPA_SOLVERS_SOURCE to install it as <install>\solvers, or point
# the BORPA_SOLVERS_DIR environment variable at it to run a development build with it.
# Every download is pinned to a SHA-256 and refused if it does not match.
#
#   powershell -ExecutionPolicy Bypass -File scripts\borpa_fetch_solvers.ps1 -Destination C:\borpa-solvers
param(
    [Parameter(Mandatory = $true)][string]$Destination
)
$ErrorActionPreference = 'Stop'

$packages = @(
    @{
        Name   = 'gmsh'
        Url    = 'https://gmsh.info/bin/Windows/gmsh-4.15.2-Windows64.zip'
        Sha256 = 'A8208B56AEDEE3434907A893E303422ACACD9003140766E09F2ABBC664BA7157'
        # Paths inside the zip, copied flat into <Destination>\gmsh
        Files  = @('gmsh-4.15.2-Windows64\gmsh.exe', 'gmsh-4.15.2-Windows64\LICENSE.txt',
                   'gmsh-4.15.2-Windows64\CREDITS.txt', 'gmsh-4.15.2-Windows64\README.txt')
    },
    @{
        Name   = 'calculix'
        Url    = 'https://www.dhondt.de/calculix_2.24_4win.zip'
        Sha256 = '8C22C627A2E134B988121A9028A4B0C20FA9AA834448C94040C5CEFADEE49905'
        # The static build only: the others need Intel MKL. Run it on one thread
        # (solver-bakeoff.md section 5.3).
        Files  = @('calculix_2.24_4win\ccx_i4_static.exe', 'calculix_2.24_4win\README_Install')
    }
)

$work = Join-Path ([System.IO.Path]::GetTempPath()) ("borpa-solvers-" + [guid]::NewGuid())
New-Item -ItemType Directory -Force $work | Out-Null
try {
    foreach ($p in $packages) {
        $zip = Join-Path $work ($p.Name + '.zip')
        Write-Host "Downloading $($p.Url)"
        Invoke-WebRequest -Uri $p.Url -OutFile $zip -UseBasicParsing
        $hash = (Get-FileHash $zip -Algorithm SHA256).Hash
        if ($hash -ne $p.Sha256) {
            throw "$($p.Name): SHA-256 $hash does not match the pinned $($p.Sha256)"
        }
        $unpacked = Join-Path $work $p.Name
        Expand-Archive -Path $zip -DestinationPath $unpacked -Force
        $target = Join-Path $Destination $p.Name
        New-Item -ItemType Directory -Force $target | Out-Null
        foreach ($f in $p.Files) {
            Copy-Item (Join-Path $unpacked $f) $target -Force
        }
    }
    # CalculiX's zip carries no licence file. It is GPL-2.0-or-later (dhondt.de); a release must
    # add the licence text here and offer the source of the exact build shipped.
    Set-Content -Path (Join-Path $Destination 'calculix\LICENSE-NOTE.txt') -Encoding utf8 -Value @(
        'CalculiX 2.24 (ccx_i4_static.exe), by Guido Dhondt. Licence: GNU GPL version 2 or later.',
        'Binary and source: https://www.dhondt.de/'
    )
    Write-Host "Solvers are in $Destination"
} finally {
    Remove-Item -Recurse -Force $work -ErrorAction SilentlyContinue
}
