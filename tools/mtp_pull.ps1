# Pull a file out of the MeidaP4 MTP volume (SD card) to a local folder,
# i.e. what Explorer does when the user right-clicks -> Copy on the device volume.
param(
    [Parameter(Mandatory=$true)][string]$Name,
    [string]$DestDir = 'out',
    [string]$DeviceName = 'MeidaP4 SD Card'
)
$ErrorActionPreference = 'Stop'
$dest = (Resolve-Path -LiteralPath $DestDir).Path
$shell = New-Object -ComObject Shell.Application
$thispc = $shell.NameSpace(17)
$dev = $null
foreach ($item in $thispc.Items()) {
    if ($item.Name -like "*$DeviceName*") { $dev = $item; break }
}
if ($null -eq $dev) { Write-Host "[mtp] 没找到 MTP 设备 '$DeviceName'"; exit 1 }
$folder = $dev.GetFolder
for ($depth = 0; $depth -lt 4; $depth++) {
    $items = @($folder.Items())
    $files = @($items | Where-Object { -not $_.IsFolder })
    if ($files.Count -gt 0 -or $items.Count -eq 0) { break }
    $next = $items | Where-Object { $_.IsFolder } | Select-Object -First 1
    if ($null -eq $next) { break }
    $folder = $next.GetFolder
    if ($null -eq $folder) { break }
}
$target = $folder.Items() | Where-Object { $_.Name -eq $Name } | Select-Object -First 1
if ($null -eq $target) {
    Write-Host "[mtp] 设备卷里没有 '$Name'；现有条目："
    $folder.Items() | ForEach-Object { Write-Host ("  - " + $_.Name) }
    exit 1
}
$destFolder = $shell.NameSpace($dest)
$destFolder.CopyHere($target, 20)
$local = Join-Path $dest $Name
for ($i = 0; $i -lt 40; $i++) {
    Start-Sleep -Milliseconds 500
    if (Test-Path -LiteralPath $local) {
        Write-Host "[mtp] OK：已取出 $local（等待 $([math]::Round($i*0.5,1)) s）"
        exit 0
    }
}
Write-Host "[mtp] 超时：$local 未出现"
exit 2
