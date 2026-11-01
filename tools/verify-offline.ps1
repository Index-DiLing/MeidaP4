#Requires -Version 5.1
<#
.SYNOPSIS
    MeidaP4 探测固件 · 离线描述符校验汇总（verifier 独立验证入口）

.DESCRIPTION
    对 mode a 与 mode b 各自的**已编译 ELF** 跑一遍 tools/desc_lint.py，把逐段解析结果
    原样打印出来，最后汇总成一张表。

    这个脚本只做"跑 + 汇总"，所有断言逻辑都在 desc_lint.py 里（纯标准库、可独立运行）。

    退出码：
        0 = 两个模式全部断言通过
        1 = 至少一条断言失败（描述符与契约不符）
        2 = 工具/输入错误（ELF 缺失、工具链找不到、Python 缺失）

    注意：本脚本**不会**构建固件。请先各自构建：
        pwsh -File tools/build.ps1 -Mode a -Slot <slot>
        pwsh -File tools/build.ps1 -Mode b -Slot <slot>

.PARAMETER Slot
    构建槽位名，ELF 从 build-<slot>-<mode>/MeidaP4.elf 取。默认 'verify'。

.PARAMETER Modes
    要校验的模式，默认 @('a','b')。

.PARAMETER BuildDirA / BuildDirB
    直接指定某个模式的构建目录（覆盖 -Slot 推导）。

.PARAMETER Toolchain
    riscv32-esp-elf 工具目录（含 nm/objcopy）；不传则交给 desc_lint.py 自己找。

.PARAMETER StrictBaseline
    把"与冻结基线漂移"的 WARN（bcdDevice 与基线不同、两模式序列号相同）提升为 FAIL。
    默认关闭 —— 因为最终契约 §2 允许这些值随描述符改动而变化。

.PARAMETER NoIdfEnv
    跳过加载 tools/idf-env.ps1。desc_lint.py 本身不需要 IDF 环境（只需要 ELF + 工具链），
    默认仍然加载它是为了与项目其它工具保持一致的环境（并让工具链目录出现在 PATH 里）。

.EXAMPLE
    # 默认对**冻结槽位** final 跑（构建产物已归档，缺 ELF 时先重建）
    pwsh -File tools/verify-offline.ps1
    pwsh -File tools/build.ps1 -Mode a -Slot final      # 若 ELF 已被归档
    pwsh -File tools/build.ps1 -Mode b -Slot final
    pwsh -File tools/verify-offline.ps1

.EXAMPLE
    pwsh -File tools/verify-offline.ps1 -Slot usbcore -StrictBaseline
#>
[CmdletBinding()]
param(
    [string]   $Slot      = 'final',
    [string[]] $Modes     = @('a', 'b'),
    [string]   $BuildDirA,
    [string]   $BuildDirB,
    [string]   $Toolchain,
    [switch]   $StrictBaseline,
    [switch]   $NoIdfEnv
)

$ErrorActionPreference = 'Stop'

# --- 控制台按 UTF-8 输出：desc_lint.py 的报告里有中文 -------------------------
try {
    [Console]::OutputEncoding = [System.Text.Encoding]::UTF8
} catch {
    Write-Warning "无法把控制台输出编码切成 UTF-8，中文可能显示为乱码。"
}
$env:PYTHONIOENCODING = 'utf-8'

$RepoRoot   = Split-Path -Parent $PSScriptRoot
$LintScript = Join-Path $PSScriptRoot 'desc_lint.py'
if (-not (Test-Path -LiteralPath $LintScript)) {
    Write-Error "[verify-offline] 找不到 $LintScript"
    exit 2
}

# --- 1. 加载 IDF 环境（与项目其它脚本一致；失败不致命） -----------------------
$IdfEnv = Join-Path $PSScriptRoot 'idf-env.ps1'
if (-not $NoIdfEnv -and (Test-Path -LiteralPath $IdfEnv)) {
    try {
        . $IdfEnv -Quiet
    } catch {
        Write-Warning "[verify-offline] 加载 idf-env.ps1 失败（不影响描述符校验）: $($_.Exception.Message)"
    }
}

# --- 2. 找 Python：优先 IDF venv，其次 PATH ----------------------------------
$Python = $null
if ($global:IdfPyPython -and (Test-Path -LiteralPath $global:IdfPyPython)) {
    $Python = $global:IdfPyPython
} else {
    $cmd = Get-Command python -ErrorAction SilentlyContinue
    if ($cmd) { $Python = $cmd.Source }
}
if (-not $Python) {
    Write-Error "[verify-offline] 找不到 python 解释器（desc_lint.py 需要 Python 3）。"
    exit 2
}

function Resolve-Elf([string] $Mode, [string] $ExplicitDir) {
    $dir = if ($ExplicitDir) { $ExplicitDir } else { Join-Path $RepoRoot "build-$Slot-$Mode" }
    return (Join-Path $dir 'MeidaP4.elf')
}

Write-Host ""
Write-Host "=============================================================================="
Write-Host "MeidaP4 离线描述符校验汇总（独立验证）"
Write-Host "  仓库     : $RepoRoot"
Write-Host "  槽位     : $Slot"
Write-Host "  Python   : $Python"
Write-Host "  校验器   : $LintScript"
Write-Host "=============================================================================="

$results = @()
$worst   = 0

foreach ($mode in $Modes) {
    $mode = $mode.ToLowerInvariant()
    if ($mode -notin @('a', 'b')) {
        Write-Warning "[verify-offline] 跳过未知模式 '$mode'"
        continue
    }

    $explicit = if ($mode -eq 'a') { $BuildDirA } else { $BuildDirB }
    $elf      = Resolve-Elf $mode $explicit

    $argv = @($LintScript, '--mode', $mode)
    if ($explicit) { $argv += @('--build-dir', $explicit) } else { $argv += @('--slot', $Slot) }
    if ($Toolchain) { $argv += @('--toolchain', $Toolchain) }
    if ($StrictBaseline) { $argv += @('--strict-baseline') }

    Write-Host ""
    Write-Host ">>>> mode $mode -> $elf"
    if (-not (Test-Path -LiteralPath $elf)) {
        Write-Host "     [跳过] ELF 不存在。先构建: pwsh -File tools/build.ps1 -Mode $mode -Slot $Slot"
        $results += [pscustomobject]@{
            Mode = $mode; Elf = $elf; Total = '-'; Passed = '-'; Failed = '-'
            Exit = 2; Result = 'ELF 缺失'
        }
        if ($worst -lt 2) { $worst = 2 }
        continue
    }

    # 报告直接流到控制台（可读性优先），同时抓一份用于汇总
    $output = & $Python @argv 2>&1
    $code   = $LASTEXITCODE
    $output | ForEach-Object { Write-Host $_ }

    $total = '-'; $passed = '-'; $failed = '-'; $warned = '-'
    $summary = $output | Where-Object { $_ -match '^\[断言汇总\]' } | Select-Object -Last 1
    if ($summary -and $summary -match '^\[断言汇总\]\s*(\d+)\s*条，通过\s*(\d+)，失败\s*(\d+)，跳过\s*(\d+)，WARN\s*(\d+)') {
        $total  = [int]$Matches[1]
        $passed = [int]$Matches[2]
        $failed = [int]$Matches[3]
        $warned = [int]$Matches[5]
    }

    $verdict = switch ($code) {
        0 { 'PASS' }
        1 { 'FAIL（断言不满足）' }
        default { 'ERROR（工具/输入）' }
    }
    if ($code -gt $worst) { $worst = $code }

    $results += [pscustomobject]@{
        Mode = $mode; Elf = $elf; Total = $total; Passed = $passed; Failed = $failed
        Warn = $warned; Exit = $code; Result = $verdict
    }
}

# --- 3. 汇总表 ---------------------------------------------------------------
Write-Host ""
Write-Host "=============================================================================="
Write-Host "汇总"
Write-Host "=============================================================================="
$results | Format-Table -AutoSize Mode, Total, Passed, Failed, Warn, Exit, Result

foreach ($r in $results) {
    if ($r.Exit -ne 0) {
        Write-Host ("  mode {0}: {1}  ->  {2}" -f $r.Mode, $r.Result, $r.Elf)
    }
}

$overall = switch ($worst) {
    0 { 'PASS - 两个模式的描述符断言全部通过（离线部分）' }
    1 { 'FAIL - 有描述符断言不满足，见上面的逐条 FAIL' }
    default { 'ERROR - 工具或输入有问题（ELF 未构建 / 工具链找不到）' }
}
Write-Host ""
Write-Host "总判定: $overall"
Write-Host "对象  : 冻结槽位 '$Slot' 的 build-$Slot-<mode>/MeidaP4.elf"
Write-Host "        若 ELF 不存在（构建产物已归档到 archive/probe-batch0/builds/），先重建："
Write-Host "          pwsh -File tools/build.ps1 -Mode a -Slot $Slot"
Write-Host "          pwsh -File tools/build.ps1 -Mode b -Slot $Slot"
Write-Host "提示  : 离线校验只覆盖「冻结版代码与最终契约是否一致」，**不是端到端功能判定**；"
Write-Host "        Chrome 能否看到设备取决于 Windows 侧 device interface 注册，见 docs/probe-verify.md。"
Write-Host "=============================================================================="

exit $worst
