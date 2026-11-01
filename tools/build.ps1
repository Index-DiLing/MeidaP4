#Requires -Version 5.1
<#
.SYNOPSIS
    MeidaP4 firmware - one-command build with an explicit EIM environment.

.DESCRIPTION
    Every writer uses its OWN build directory and its OWN generated sdkconfig so that
    concurrent builds never race on the shared root files:

        build dir   : build-<Slot>-<Mode>
        sdkconfig   : sdkconfig.<Slot>-<Mode>
        defaults    : sdkconfig.defaults;sdkconfig.<Mode>.defaults

    NEVER run a bare `idf.py build` in this project - it would use the shared root
    `build/` and `sdkconfig`.

    Concurrency and the IDF component manager
    -----------------------------------------
    `dependencies.lock` / `managed_components/` are shared and mutually exclusive. Only
    the Lead edits `main/idf_component.yml` and only the Lead runs a build that can
    *change* the dependency set.

    ⚠️ Do NOT try to dodge that by setting IDF_COMPONENT_MANAGER=0:

      * ESP-IDF v6.1 only registers `<project>/managed_components/` when the component
        manager is ENABLED (tools/cmake/build.cmake:653-724). With it disabled the
        directory is not part of the build at all.
      * `espressif__esp_tinyusb`'s CMakeLists.txt:78-84 then adds a requirement on a
        component literally named `tinyusb` - which a registry install (directory
        `espressif__tinyusb`) cannot satisfy. Configure dies with
        "Failed to resolve component 'tinyusb'".

    So every writer runs WITH the component manager. The manager takes its own lock;
    when two builds configure at the same instant one of them reports a lock error.
    This script retries that automatically (see -Retries).

.PARAMETER Mode
    a    = single Vendor interface + WebUSB/WCID          (probe, CONFIG_TINYUSB_MTP_ENABLED=n)
    b    = MTP + Vendor composite + WebUSB/WCID           (probe, CONFIG_TINYUSB_MTP_ENABLED=y)
    prod = the full four-function composite firmware      (sdkconfig.prod.defaults)

.PARAMETER Slot
    Per-writer slot name; keeps concurrent writers apart. Default 'main' (Lead).

.PARAMETER Clean
    Delete this slot's build directory first (keeps the generated sdkconfig).

.PARAMETER Fresh
    Delete this slot's build directory AND generated sdkconfig (full reconfigure).

.PARAMETER Retries
    Max attempts when the component manager reports lock contention (default 3).

.EXAMPLE
    pwsh -File tools/build.ps1 -Mode prod -Slot webusb
#>
[CmdletBinding()]
param(
    [ValidateSet('a', 'b', 'prod')] [string] $Mode = 'a',
    [string] $Slot = 'main',
    # Extra sdkconfig defaults file(s) appended after sdkconfig.<Mode>.defaults.
    # Use it for throwaway diagnostic configs (e.g. sdkconfig.diag.defaults) without
    # touching the frozen per-mode defaults.
    [string] $ExtraDefaults,
    [switch] $Clean,
    [switch] $Fresh,
    [switch] $Quiet,
    [int] $Retries = 3,
    [Parameter(ValueFromRemainingArguments = $true)] [string[]] $IdfArgs
)

$ErrorActionPreference = 'Stop'

# 已经废弃的开关：$IdfArgs 是 ValueFromRemainingArguments，会把这些直接转发给 idf.py，
# 而 Click 会把 `-NoComponentManager` 拆成 `-N` + `oComponentManager` → 报
# "No such option: -N"，报错信息完全指不到真正原因（WS-storage 实际踩过）。
# 这里拦下来，直接说清楚该用什么。
foreach ($a in $IdfArgs) {
    if ($a -match '^-No' -or $a -eq '-NoComponentManager') {
        throw ("[build] '$a' 不是 idf.py 参数，而且 -NoComponentManager 已废弃：`n" +
               "        IDF v6.1 只在组件管理器启用时才注册 managed_components/，关掉后`n" +
               "        espressif__esp_tinyusb 会去要求一个名字就叫 'tinyusb' 的组件 → configure 必失败。`n" +
               "        直接去掉这个开关即可；脚本已内置组件管理器锁冲突自动重试。")
    }
}

$Root = Split-Path -Parent $PSScriptRoot
. (Join-Path $PSScriptRoot 'idf-env.ps1') -Quiet:$Quiet

$buildDir   = Join-Path $Root "build-$Slot-$Mode"
$sdkconfig  = Join-Path $Root "sdkconfig.$Slot-$Mode"
$defaults   = "sdkconfig.defaults;sdkconfig.$Mode.defaults"

if (-not (Test-Path -LiteralPath (Join-Path $Root 'sdkconfig.defaults'))) {
    throw "[build] sdkconfig.defaults not found in '$Root'."
}
if (-not (Test-Path -LiteralPath (Join-Path $Root "sdkconfig.$Mode.defaults"))) {
    throw "[build] sdkconfig.$Mode.defaults not found in '$Root'."
}
if ($ExtraDefaults) {
    foreach ($extra in ($ExtraDefaults -split ';')) {
        if (-not (Test-Path -LiteralPath (Join-Path $Root $extra))) {
            throw "[build] extra defaults file not found: '$extra'"
        }
    }
    $defaults = "$defaults;$ExtraDefaults"
}

if ($env:IDF_COMPONENT_MANAGER -eq '0') {
    Write-Warning "[build] IDF_COMPONENT_MANAGER=0 is set in this environment. That is NOT supported for this project (see the .DESCRIPTION) - clearing it."
    Remove-Item Env:\IDF_COMPONENT_MANAGER -ErrorAction SilentlyContinue
}

if ($Fresh) {
    Remove-Item -LiteralPath $sdkconfig -Force -ErrorAction SilentlyContinue
    Remove-Item -LiteralPath $buildDir -Recurse -Force -ErrorAction SilentlyContinue
} elseif ($Clean) {
    Remove-Item -LiteralPath $buildDir -Recurse -Force -ErrorAction SilentlyContinue
}

Write-Host "[build] mode      : $Mode"
Write-Host "[build] slot      : $Slot"
Write-Host "[build] build dir : $buildDir"
Write-Host "[build] sdkconfig : $sdkconfig"
Write-Host "[build] defaults  : $defaults"

# ---------------------------------------------------------------------------
# 防"实验配置的假绿"（WS-storage 2026-10 实测发现，会静默骗过所有人）
#
# IDF 6.1 的 Kconfig 策略是 `use sdkconfig`（构建日志原文：
#   "NOTE: Kconfig defaults policy: use sdkconfig"）
# ⇒ **已存在的 sdkconfig 会覆盖 defaults 文件**。
#
# 于是有两个方向都会得到假绿：
#   1) 加了 -ExtraDefaults 但槽位里已有一份 sdkconfig ⇒ 实验配置**没生效**，
#      你以为在做诊断构建，其实还是原配置；
#   2) 用实验配置生成过 sdkconfig 后，再用**正式命令**构建 ⇒ 正式命令**也没生效**，
#      你拿到的 exit 0 是**实验配置**的，而不是冻结配置的。
#
# 对策：把"生成这份 sdkconfig 时用的 defaults 链"**及其内容哈希**戳在旁边的 stamp 文件里，
# 一发现不一致就**自动删掉 sdkconfig** 强制重新生成 —— 让这件事在结构上不可能发生。
#
# ⚠️ 为什么必须带**内容哈希**（Lead 2026-10-21 补）：只记链字符串的话，
#    **改了 defaults 文件的内容而链不变**时 stamp 不会变 → 又一份"静默沿用旧配置"的假绿。
#    这正是本工程反复出现的那一类问题（构建系统状态与写入者以为的不一致）。
# ---------------------------------------------------------------------------
$stampPath = Join-Path $Root "sdkconfig.$Slot-$Mode.defaults-stamp"
$stampParts = @($defaults)
foreach ($f in ($defaults -split ';')) {
    $fp = Join-Path $Root $f
    if (Test-Path -LiteralPath $fp) {
        $stampParts += "$f=$((Get-FileHash -LiteralPath $fp -Algorithm SHA256).Hash)"
    } else {
        $stampParts += "$f=MISSING"
    }
}
$stampValue = ($stampParts -join '|')
if (Test-Path -LiteralPath $sdkconfig) {
    if (Test-Path -LiteralPath $stampPath) {
        $stamp = (Get-Content -LiteralPath $stampPath -Raw).Trim()
        if ($stamp -ne $stampValue) {
            Write-Host "[build] ############################################################"
            Write-Host "[build] # 检测到 defaults 链**或其内容**变了，本槽位的 sdkconfig 已过期："
            Write-Host "[build] #   生成时: $stamp"
            Write-Host "[build] #   本次要: $stampValue"
            Write-Host "[build] # 已存在的 sdkconfig 会覆盖 defaults 文件 -> 直接构建会得到"
            Write-Host "[build] # **假绿**。已自动删除该 sdkconfig 强制重新生成。"
            Write-Host "[build] ############################################################"
            Remove-Item -LiteralPath $sdkconfig -Force -ErrorAction SilentlyContinue
        }
    } else {
        Write-Host "[build] NOTE: 本槽位的 sdkconfig 早于本次防护引入，无法判断它由哪套 defaults 生成。"
        Write-Host "[build]       如果这个槽位跑过 -ExtraDefaults，请加 -Fresh 重跑一次。"
    }
}
Set-Content -LiteralPath $stampPath -Value $stampValue -NoNewline

# ---------------------------------------------------------------------------
# 入口选择：用**环境变量**传给 CMake，而不是 -D
#
# 为什么不能用 -D（Lead 2026-10-21 实测）：IDF 的 component manager 在"缺 Kconfig 值"时
# 会**重跑一次 configure**；实测第一遍里 `-DMEIDAP4_MODE=prod` **不可见**
# （日志显示第一遍 `MEIDAP4_MODE=a`、第二遍才是 prod），而第一遍注册的 include/REQUIRES
# 已经生效 → main_prod.cpp 的编译行带着 usb_probe/mtp_probe 的 include
# → `fatal error: mtp_backend.h: No such file or directory`。
#
# 环境变量被**所有子进程继承**（包括任何 cmake 重跑），所以第一遍就能看见。
# 这也是本工程反复得到的同一条教训：**构建结构只能依赖"第一遍就可见"的输入。**
# ---------------------------------------------------------------------------
$env:MEIDAP4_MODE = $Mode
Write-Host "[build] entry mode : $env:MEIDAP4_MODE (env MEIDAP4_MODE)"
$lockPatterns = @(
    'component manager.*lock',
    'Failed to acquire.*lock',
    'lock file.*exists',
    'Another instance of the component manager',
    'Could not lock'
)

$logPath = Join-Path ([System.IO.Path]::GetTempPath()) "meidap4-build-$Slot-$Mode.log"
$code = 1

Push-Location $Root
try {
    for ($attempt = 1; $attempt -le [Math]::Max(1, $Retries); $attempt++) {
        if ($attempt -gt 1) {
            Write-Host "[build] retry $attempt/$Retries (component-manager lock contention)"
        }
        Invoke-IdfPy -B $buildDir "-DSDKCONFIG=$sdkconfig" "-DSDKCONFIG_DEFAULTS=$defaults" @IdfArgs build 2>&1 |
            Tee-Object -FilePath $logPath
        $code = $LASTEXITCODE
        if ($code -eq 0) { break }

        $hit = $false
        if (Test-Path -LiteralPath $logPath) {
            foreach ($p in $lockPatterns) {
                if (Select-String -LiteralPath $logPath -Pattern $p -Quiet) { $hit = $true; break }
            }
        }
        if (-not $hit -or $attempt -ge $Retries) { break }

        Write-Host "[build] lock contention detected; waiting 20 s before retry"
        Start-Sleep -Seconds 20
    }
} finally {
    Pop-Location
}

if ($code -eq 0) {
    $project = Split-Path -Leaf $Root
    Write-Host "[build] OK -> $(Join-Path $buildDir "$project.bin")"
} else {
    Write-Host "[build] FAILED (exit $code)  full log: $logPath"
}
exit $code
