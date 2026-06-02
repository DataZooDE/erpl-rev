# Assemble the single-file Windows distributable:
#   erpl-rev.exe = launcher.exe + payload.tar(inner server + runtime DLLs) + footer
# footer = "ERPLREV\x01" + little-endian uint64 payload size (read by the launcher).
[CmdletBinding()]
param(
  [Parameter(Mandatory)][string]$Server,    # build\erpl_rev_server.exe
  [Parameter(Mandatory)][string]$Launcher,  # build\erpl_rev_launch.exe
  [Parameter(Mandatory)][string]$SdkLib,    # nwrfcsdk\win\lib
  [Parameter(Mandatory)][string]$DuckdbDir, # vendor\duckdb-1.5.3
  [Parameter(Mandatory)][string]$Out        # dist\erpl-rev-windows-amd64.exe
)
$ErrorActionPreference = 'Stop'

$stage = Join-Path $env:TEMP ("erplrev_stage_" + [System.Guid]::NewGuid().ToString('N'))
$pay   = Join-Path $stage 'payload'
New-Item -ItemType Directory -Path $pay -Force | Out-Null

Copy-Item $Server (Join-Path $pay 'erpl_rev_server.exe')
foreach ($l in 'sapnwrfc.dll','libsapucum.dll','icudt50.dll','icuin50.dll','icuuc50.dll') {
  Copy-Item (Join-Path $SdkLib $l) (Join-Path $pay $l)
}
Copy-Item (Join-Path $DuckdbDir 'duckdb.dll') (Join-Path $pay 'duckdb.dll')

Write-Host "Bundling:"; Get-ChildItem $pay | ForEach-Object { "  {0,12}  {1}" -f $_.Length, $_.Name }

$tar = Join-Path $stage 'payload.tar'
$names = (Get-ChildItem $pay | ForEach-Object Name)
& tar -cf $tar -C $pay @names
if ($LASTEXITCODE -ne 0) { throw "tar failed" }

New-Item -ItemType Directory -Path (Split-Path $Out -Parent) -Force | Out-Null
$fs = [System.IO.File]::Open($Out, 'Create')
foreach ($f in @($Launcher, $tar)) {
  $bytes = [System.IO.File]::ReadAllBytes($f)
  $fs.Write($bytes, 0, $bytes.Length)
}
$size  = (Get-Item $tar).Length
$magic = [byte[]]@(0x45,0x52,0x50,0x4C,0x52,0x45,0x56,0x01)  # "ERPLREV\x01"
$fs.Write($magic, 0, 8)
$le = [System.BitConverter]::GetBytes([uint64]$size)          # x64 is little-endian
$fs.Write($le, 0, 8)
$fs.Close()

Remove-Item $stage -Recurse -Force
Write-Host "-> $Out ($((Get-Item $Out).Length) bytes, payload $size bytes)"
