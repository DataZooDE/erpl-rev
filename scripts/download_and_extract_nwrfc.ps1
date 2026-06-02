[CmdletBinding()]
param(
    [string]$SRC = 's3://erpl-resources/sapnwrfc/nwrfc750P_13-70002755_win.zip',
    [string]$TARGET = './nwrfcsdk/win/'
)

# Download + extract the SAP NW RFC SDK (Windows) — same mechanism as DataZooDE/erpl.
function Download-Extract-Move {
    param([string]$s3_url, [string]$target_dir)

    $tmp_download_dir = "$env:TEMP\nwrfc_downloaded"
    $tmp_extract_dir  = "$env:TEMP\nwrfc_extracted"
    New-Item -ItemType Directory -Path $tmp_download_dir, $tmp_extract_dir, $target_dir -Force | Out-Null

    aws s3 cp $s3_url $tmp_download_dir
    $zip = [System.IO.Path]::GetFileName($s3_url)
    Expand-Archive -Path "$tmp_download_dir\$zip" -DestinationPath $tmp_extract_dir -Force
    Move-Item -Path "$tmp_extract_dir\nwrfcsdk\*" -Destination $target_dir -Force
    Remove-Item -Path $tmp_download_dir, $tmp_extract_dir -Recurse -Force
}

Download-Extract-Move -s3_url $SRC -target_dir $TARGET
