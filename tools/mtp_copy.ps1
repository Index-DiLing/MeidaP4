# Copy a local file into the MeidaP4 MTP volume (SD card) through the Windows shell,
# simulating what Explorer does when the user drags a file into "MeidaP4 SD Card".
param(
    [Parameter(Mandatory=$true)][string]$Source,
    [string]$DeviceName = 'MeidaP4 SD Card'
)
$ErrorActionPreference = 'Stop'
$src = (Resolve-Path -LiteralPath $Source).Path
$shell = New-Object -ComObject Shell.Application
$thispc = $shell.NameSpace(17)   # "This PC"
$dev = $null
foreach ($item in $thispc.Items()) {
    if ($item.Name -like "*$DeviceName*") { $dev = $item; break }
}
if ($null -eq $dev) {
    Write-Host "[mtp] 没找到 MTP 设备 '$DeviceName'；This PC 下现有："
    $thispc.Items() | ForEach-Object { Write-Host ("  - " + $_.Name) }
    exit 1
}
Write-Host "[mtp] 目标设备：$($dev.Name)"
$folder = $dev.GetFolder
if ($null -eq $folder) { Write-Host "[mtp] GetFolder 失败"; exit 1 }
# MTP 在 Shell 里有 "便携设备 -> 存储" 两层，真正的卷根目录在更下面一层。
for ($depth = 0; $depth -lt 4; $depth++) {
    $items = @($folder.Items())
    $files = @($items | Where-Object { -not $_.IsFolder })
    if ($files.Count -gt 0 -or $items.Count -eq 0) { break }
    $next = $items | Where-Object { $_.IsFolder } | Select-Object -First 1
    if ($null -eq $next) { break }
    $folder = $next.GetFolder
    if ($null -eq $folder) { break }
}
$before = @($folder.Items() | ForEach-Object { $_.Name })
Write-Host "[mtp] 拷贝 $src -> $($dev.Name)\"
# 4 = no progress dialog, 16 = "yes to all", 20 = no confirmation
$folder.CopyHere($src, 20)
$name = Split-Path -Leaf $src
for ($i = 0; $i -lt 40; $i++) {
    Start-Sleep -Milliseconds 500
    $after = @($folder.Items() | ForEach-Object { $_.Name })
    if ($after -contains $name) {
        Write-Host "[mtp] OK：设备卷里已出现 $name（等待 $([math]::Round($i*0.5,1)) s）"
        exit 0
    }
}
Write-Host "[mtp] 超时：$name 未出现在设备卷里"
Write-Host ("[mtp] 当前条目：" + (($folder.Items() | ForEach-Object { $_.Name }) -join ', '))
exit 2
