#Requires -Version 5.1
<#
.SYNOPSIS
    WS-net · NCM 描述符**离线**自检（host gcc，不需要 IDF、不需要设备、不占构建槽位）。

.DESCRIPTION
    编译并运行 components/usb_net/test/host/test_ncm_desc.c，把
    components/usb_net/usb_net_ncm.c 产出的 77 字节 NCM 片段与独立手写的期望值逐字节比对。

    期望值来源（不是从被测代码复制的）：
      * CDC-NCM 1.0 功能描述符 / USB 2.0 §9.6
      * tinyusb managed_components/espressif__tinyusb/src/device/usbd.h:1080-1102
        （TUD_CDC_NCM_DESCRIPTOR_arg11，去掉它的 IAD 8 字节）
      * docs/contracts/frozen-allocation.md §1/§2/§6

.EXAMPLE
    pwsh -File components/usb_net/test/host/run.ps1
#>
[CmdletBinding()]
param([string] $Compiler = 'gcc')

$ErrorActionPreference = 'Stop'

$here = Split-Path -Parent $PSCommandPath
$root = (Resolve-Path (Join-Path $here '..\..\..\..')).Path   # 工程根

$stubs   = Join-Path $here 'stubs'
$netDir  = Join-Path $root 'components\usb_net'          # usb_net_priv.h 在这里
$usbInc  = Join-Path $root 'components\usb_net\include'
$compInc = Join-Path $root 'components\usb_composite\include'
$outDir  = Join-Path $here 'out'
$exe     = Join-Path $outDir 'test_ncm_desc.exe'

New-Item -ItemType Directory -Force -Path $outDir | Out-Null

$args = @(
    '-std=gnu99', '-Wall', '-Wextra',
    '-I', $stubs, '-I', $netDir, '-I', $usbInc, '-I', $compInc,
    (Join-Path $here 'test_ncm_desc.c'),
    (Join-Path $root 'components\usb_net\usb_net_ncm.c'),
    '-o', $exe
)

Write-Host "[host-test] 编译：$Compiler $($args -join ' ')"
& $Compiler @args
if ($LASTEXITCODE -ne 0) { throw "[host-test] 编译失败 (exit $LASTEXITCODE)" }

Write-Host "[host-test] 运行：$exe"
& $exe
$code = $LASTEXITCODE
Write-Host "[host-test] exit code = $code"
exit $code
