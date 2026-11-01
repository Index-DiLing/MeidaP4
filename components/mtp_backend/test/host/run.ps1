#Requires -Version 5.1
<#
.SYNOPSIS
    MeidaP4 · MTP 描述符片段的主机侧离线测试（原生 gcc，无 IDF、无硬件）

.DESCRIPTION
    用**上游真实的** `tusb.h` 里的 `TUD_MTP_DESCRIPTOR` 宏生成参考字节，
    与固件里同一份实现（test_mtp_desc.c 直接 #include ../../mtp_backend.c）生成的片段逐字节比对。

    覆盖：
      * FS / HS 两种速度；
      * 冻结号（IF0/EP1/EP2）与**故意偏离的号**（证明没有硬编码）；
      * 结构语义（bNumEndpoints=3、顺序 int IN → bulk OUT → bulk IN、类 06/01/01、MPS 64/512）；
      * 错误路径（cap 不足 / alloc 不完整 / 方向位不对）；
      * usb_function_t 注册形态（配对表整表、WCID=no、on_out=NULL …）。

    退出码：0 = 全部通过；1 = 有断言失败；2 = 编译失败（找不到 gcc 等）。
    写作用域：components/mtp_backend/test/host/**
.EXAMPLE
    pwsh -File components/mtp_backend/test/host/run.ps1
#>
[CmdletBinding()]
param(
    [string] $Cc = 'gcc'
)

$ErrorActionPreference = 'Stop'

$Here = $PSScriptRoot
$Root = (Resolve-Path (Join-Path $Here '..\..\..\..')).Path

$TinyusbSrc    = Join-Path $Root 'managed_components\espressif__tinyusb\src'
$EspTinyusbInc = Join-Path $Root 'managed_components\espressif__esp_tinyusb\include'
$CompositeInc  = Join-Path $Root 'components\usb_composite\include'
$SdStorageInc  = Join-Path $Root 'components\sd_storage\include'
$MtpBackendInc = Join-Path $Root 'components\mtp_backend\include'

foreach ($p in @($TinyusbSrc, $EspTinyusbInc, $CompositeInc, $SdStorageInc, $MtpBackendInc)) {
    if (-not (Test-Path -LiteralPath $p)) {
        Write-Error "[mtp-desc-test] 缺少依赖目录: $p"
        exit 2
    }
}

$Exe = Join-Path ([System.IO.Path]::GetTempPath()) 'meidap4_mtp_desc_test.exe'
$Src = Join-Path $Here 'test_mtp_desc.c'
$Stubs = Join-Path $Here 'stubs\stubs.c'

# 顺序要紧：$Here 必须最前（我的测试版 tusb_config.h 要盖住 esp_tinyusb 那份），
# 其次是 stubs（esp_err.h / esp_log.h / sdkconfig.h）。
$incs = @(
    "-I$Here",
    "-I$(Join-Path $Here 'stubs')",
    "-I$TinyusbSrc",
    "-I$EspTinyusbInc",
    "-I$CompositeInc",
    "-I$SdStorageInc",
    "-I$MtpBackendInc"
)

Write-Host "[mtp-desc-test] root      : $Root"
Write-Host "[mtp-desc-test] compiler  : $Cc"
Write-Host "[mtp-desc-test] tinyusb   : $TinyusbSrc"
Write-Host ""

& $Cc -std=gnu11 -Wall -Wextra @incs -o $Exe $Src $Stubs
$cc = $LASTEXITCODE
if ($cc -ne 0) {
    Write-Host "[mtp-desc-test] 编译失败 (exit $cc)"
    exit 2
}

& $Exe
exit $LASTEXITCODE
