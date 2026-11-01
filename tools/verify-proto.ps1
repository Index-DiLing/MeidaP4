#Requires -Version 5.1
<#
.SYNOPSIS
    MeidaP4 完整版 · I0 线协议 + I1 描述符 离线一致性验证（verifier 独立入口）

.DESCRIPTION
    一条命令跑完三件事，输出 PASS/FAIL 表并给出 exit code：

      1. 向量完整性   : 用 test/host/gen_vectors.py 独立重算 tools/proto_vectors.json，
                        与仓库里的文件**逐字节**比对（防止黄金向量被手改 / 与生成器漂移）
      2. C 侧         : 原生 gcc 编译 components/webusb_proto/wu_codec.c + test/host/test_codec.c 并运行
                        （鲁棒性向量：resync / 坏 CRC / length=1025 / 分片喂入 / ZLP）
      3. JS 侧        : node 运行 web/proto.js（test/host/run_proto_js.mjs 驱动）对**同一批**向量断言
      4. I1 描述符    : 原生 gcc 编译 components/usb_composite/usb_composite_desc.c +
                        test/host/test_composite_desc.c，逐行核对 docs/contracts/frozen-allocation.md
                        §1/§2/§3/§4/§5/§6 与 §2.2 的 FIFO 预算

    退出码：
        0 = 全部 PASS
        1 = 至少一条断言 FAIL（实现与冻结契约/规约不符）
        2 = 工具或输入问题（缺 wu_codec.c / proto.js / usb_composite_desc.c，gcc/node 找不到…）

    ⚠️ 边界：本脚本证明的是「实现与 docs/protocol.md、docs/contracts/frozen-allocation.md 一致」，
       **不能**证明真机 USB 链路成立。设备未连接 —— 离线 PASS != 真机通过。

.PARAMETER Gcc
    原生 gcc 路径。默认 E:\MINGW\bin\gcc.exe（本机已实测 gcc 11.2.0），不存在则回退 PATH 上的 gcc。

.PARAMETER Node
    node 可执行文件。默认 node（本机 v24.19.0）。

.PARAMETER Python
    Python 3 解释器（只用于向量生成/校验）。默认 C:\Espressif\tools\python\v6.1\venv\Scripts\python.exe。

.PARAMETER SkipComposite
    跳过 I1 描述符测试（只在只想验证 I0 时用）。

.PARAMETER HarnessSelftest
    用 test/host/harness_selftest/ 里的**临时参考实现**替代产品代码，仅用于验证"测试骨架本身没 bug"。
    ⚠️ 该模式下的 PASS **不能**作为产品实现合规的证据。

.PARAMETER NoVectorCheck
    跳过"黄金向量与生成器逐字节一致"的检查。

.EXAMPLE
    pwsh -File tools/verify-proto.ps1
    pwsh -File tools/verify-proto.ps1 -HarnessSelftest      # 只验证测试骨架
    pwsh -File tools/verify-proto.ps1 -SkipComposite        # I1 还没落地时先验 I0
#>
[CmdletBinding()]
param(
    [string] $Gcc,
    [string] $Node    = 'node',
    [string] $Python,
    [switch] $SkipComposite,
    [switch] $HarnessSelftest,
    [switch] $NoVectorCheck
)

$ErrorActionPreference = 'Stop'

# 控制台按 UTF-8 输出（子进程输出里有中文）
try { [Console]::OutputEncoding = [System.Text.UTF8Encoding]::new($false) } catch { }
$env:PYTHONIOENCODING = 'utf-8'

$RepoRoot = Split-Path -Parent $PSScriptRoot
$OutDir   = Join-Path $RepoRoot 'test\host\out'
New-Item -ItemType Directory -Force -Path $OutDir | Out-Null

# ---------------------------------------------------------------------------
# 工具定位
# ---------------------------------------------------------------------------
function Resolve-Tool([string] $Explicit, [string[]] $Candidates, [string] $Name) {
    if ($Explicit) {
        if (Test-Path -LiteralPath $Explicit) { return (Resolve-Path -LiteralPath $Explicit).Path }
        Write-Warning "[verify-proto] 指定的 $Name 不存在：$Explicit"
    }
    foreach ($c in $Candidates) {
        if ($c -and (Test-Path -LiteralPath $c)) { return (Resolve-Path -LiteralPath $c).Path }
    }
    $cmd = Get-Command $Name -ErrorAction SilentlyContinue
    if ($cmd) { return $cmd.Source }
    return $null
}

$Gcc = Resolve-Tool $Gcc @('E:\MINGW\bin\gcc.exe', 'C:\MINGW\bin\gcc.exe') 'gcc'
$Python = Resolve-Tool $Python @('C:\Espressif\tools\python\v6.1\venv\Scripts\python.exe') 'python'
$nodeCmd = Get-Command $Node -ErrorAction SilentlyContinue
$NodePath = if ($nodeCmd) { $nodeCmd.Source } else { $null }

$results = @()
function Add-Result {
    param([string] $Step, [string] $Status, [string] $Assert, [string] $Pass, [string] $Fail, [string] $Note)
    $script:results += [pscustomobject]@{
        Step = $Step; Status = $Status; Assert = $Assert; Pass = $Pass; Fail = $Fail; Note = $Note
    }
}

function Show-Block([string] $Title) {
    Write-Host ''
    Write-Host '------------------------------------------------------------------------------'
    Write-Host ">>>> $Title"
    Write-Host '------------------------------------------------------------------------------'
}

$AssertRe = '断言汇总:\s*(\d+)\s*条，通过\s*(\d+)，失败\s*(\d+)'

function Invoke-And-Parse {
    param([string] $Exe, [string[]] $Argv, [string] $WorkDir = $RepoRoot)
    $out = & $Exe @Argv 2>&1
    $code = $LASTEXITCODE
    $out | ForEach-Object { Write-Host $_ }
    $text = ($out | Out-String)
    $assert = '-'; $pass = '-'; $fail = '-'
    $m = [regex]::Match($text, $AssertRe)
    if ($m.Success) { $assert = $m.Groups[1].Value; $pass = $m.Groups[2].Value; $fail = $m.Groups[3].Value }
    return [pscustomobject]@{ Exit = $code; Assert = $assert; Pass = $pass; Fail = $fail; Text = $text }
}

# ---------------------------------------------------------------------------
$VectorsRel = 'tools\proto_vectors.json'
$Vectors    = Join-Path $RepoRoot $VectorsRel
$CodecC     = Join-Path $RepoRoot 'components\webusb_proto\wu_codec.c'
$ProtoJs    = Join-Path $RepoRoot 'web\proto.js'
$DescC      = Join-Path $RepoRoot 'components\usb_composite\usb_composite_desc.c'
$DescIntH   = Join-Path $RepoRoot 'components\usb_composite\usb_composite_internal.h'
$Selftest   = Join-Path $RepoRoot 'test\host\harness_selftest'

Write-Host '=============================================================================='
Write-Host 'MeidaP4 完整版 · 离线一致性验证（I0 线协议 + I1 描述符）'
Write-Host '  仓库   : ' -NoNewline; Write-Host $RepoRoot
Write-Host '  gcc    : ' -NoNewline; Write-Host $(if ($Gcc) { $Gcc } else { '(未找到)' })
Write-Host '  node   : ' -NoNewline; Write-Host $(if ($NodePath) { $NodePath } else { '(未找到)' })
Write-Host '  python : ' -NoNewline; Write-Host $(if ($Python) { $Python } else { '(未找到)' })
Write-Host '  规约   : docs/protocol.md + docs/contracts/frozen-allocation.md'
if ($HarnessSelftest) {
    Write-Host '  ⚠️ 模式 : -HarnessSelftest（用临时参考实现验证测试骨架，**不是**产品验证）'
}
Write-Host '=============================================================================='

# ---- 0. 输入文件清单与摘要 -------------------------------------------------
Write-Host ''
Write-Host '输入文件：'
foreach ($f in @(
        @{ p = (Join-Path $RepoRoot 'docs\protocol.md');             n = 'docs/protocol.md' },
        @{ p = (Join-Path $RepoRoot 'docs\contracts\frozen-allocation.md'); n = 'docs/contracts/frozen-allocation.md' },
        @{ p = $Vectors;    n = 'tools/proto_vectors.json' },
        @{ p = $CodecC;     n = 'components/webusb_proto/wu_codec.c' },
        @{ p = $ProtoJs;    n = 'web/proto.js' },
        @{ p = $DescC;      n = 'components/usb_composite/usb_composite_desc.c' }
    )) {
    if (Test-Path -LiteralPath $f.p) {
        $h = (Get-FileHash -LiteralPath $f.p -Algorithm SHA256).Hash
        Write-Host ('  [ok]      {0,-48} {1,8} B  sha256={2}' -f $f.n, (Get-Item -LiteralPath $f.p).Length, $h.Substring(0, 16))
    } else {
        Write-Host ('  [缺失]    {0}' -f $f.n)
    }
}

$worst = 0
function Raise([int] $level) { if ($level -gt $script:worst) { $script:worst = $level } }

# ---------------------------------------------------------------------------
# 1. 黄金向量完整性
# ---------------------------------------------------------------------------
Show-Block '1. 黄金向量完整性（gen_vectors.py 独立重算 vs 仓库文件）'
if ($NoVectorCheck) {
    Write-Host '[skip] -NoVectorCheck'
    Add-Result '向量完整性' 'SKIP' '-' '-' '-' '-NoVectorCheck'
} elseif (-not $Python) {
    Write-Host '[ERROR] 找不到 Python3，无法重算向量'
    Add-Result '向量完整性' 'ERROR' '-' '-' '-' '找不到 Python3'
    Raise 2
} else {
    $tmp = Join-Path $env:TEMP ('meida_p4_vectors_{0}.json' -f ([guid]::NewGuid().ToString('N').Substring(0, 8)))
    & $Python (Join-Path $RepoRoot 'test\host\gen_vectors.py') --out $tmp | ForEach-Object { Write-Host $_ }
    if ($LASTEXITCODE -ne 0) {
        Write-Host '[ERROR] gen_vectors.py 失败'
        Add-Result '向量完整性' 'ERROR' '-' '-' '-' 'gen_vectors.py 退出码非 0'
        Raise 2
    } else {
        $hA = (Get-FileHash -LiteralPath $tmp        -Algorithm SHA256).Hash
        $hB = (Get-FileHash -LiteralPath $Vectors    -Algorithm SHA256).Hash
        if ($hA -eq $hB) {
            Write-Host "[ok] 逐字节一致 sha256=$($hA.Substring(0,16))..."
            Add-Result '向量完整性' 'PASS' '-' '-' '-' '生成器重算 == 仓库文件'
        } else {
            Write-Host "[FAIL] 仓库里的 tools/proto_vectors.json 与生成器输出不一致"
            Write-Host "       仓库   : $hB"
            Write-Host "       重算后 : $hA"
            Write-Host "       差异内容已留在 $tmp（可 Compare-Object 对比）"
            Add-Result '向量完整性' 'FAIL' '-' '-' '-' '向量文件被改动或生成器漂移'
            Raise 1
        }
    }
}

# ---------------------------------------------------------------------------
# 2. C 侧 codec
# ---------------------------------------------------------------------------
Show-Block '2. C 侧编解码（原生 gcc）'
$codecSrc = $CodecC
$codecLabel = 'components/webusb_proto/wu_codec.c'
if ($HarnessSelftest) {
    $codecSrc = Join-Path $Selftest 'ref_codec_for_harness_selftest.c'
    $codecLabel = 'harness_selftest 参考实现（非产品代码）'
}
if (-not $Gcc) {
    Write-Host '[ERROR] 找不到 gcc'
    Add-Result 'test_codec (C)' 'ERROR' '-' '-' '-' '找不到 gcc'
    Raise 2
} elseif (-not (Test-Path -LiteralPath $codecSrc)) {
    Write-Host "[ERROR] 缺少 $codecLabel —— 该文件由 WS-webusb（task-3）产出，尚未落地"
    Add-Result 'test_codec (C)' 'ERROR' '-' '-' '-' "缺少 $codecLabel"
    Raise 2
} else {
    Write-Host "被验证实现 : $codecLabel"
    $exe = Join-Path $OutDir 'test_codec.exe'
    $cf = & $Gcc -std=c11 -Wall -Wextra -O1 `
        -I (Join-Path $RepoRoot 'components\webusb_proto\include') `
        (Join-Path $RepoRoot 'test\host\test_codec.c') $codecSrc -o $exe 2>&1
    $cc = $LASTEXITCODE
    $cf | ForEach-Object { Write-Host $_ }
    if ($cc -ne 0) {
        Write-Host '[FAIL] 编译失败'
        Add-Result 'test_codec (C)' 'FAIL' '-' '-' '-' '编译失败'
        Raise 1
    } else {
        $warnCount = @($cf | Where-Object { $_ -match 'warning:' }).Count
        if ($warnCount -gt 0) { Write-Host "[info] 编译产生 $warnCount 条 warning（见上）" }
        $r = Invoke-And-Parse -Exe $exe -Argv @($Vectors)
        $st = if ($r.Exit -eq 0) { 'PASS' } else { 'FAIL' }
        Add-Result 'test_codec (C)' $st $r.Assert $r.Pass $r.Fail $(if ($warnCount -gt 0) { "$warnCount warning" } else { '无 warning' })
        if ($r.Exit -ne 0) { Raise 1 }
    }
}

# ---------------------------------------------------------------------------
# 3. JS 侧 codec（同一批向量）
# ---------------------------------------------------------------------------
Show-Block '3. JS 侧编解码（node，同一批向量）'
$jsTarget = $ProtoJs
$jsLabel = 'web/proto.js'
if ($HarnessSelftest) {
    $jsTarget = Join-Path $Selftest 'ref_proto_for_harness_selftest.mjs'
    $jsLabel = 'harness_selftest 参考实现（非产品代码）'
}
if (-not $NodePath) {
    Write-Host '[ERROR] 找不到 node'
    Add-Result 'proto.js (JS)' 'ERROR' '-' '-' '-' '找不到 node'
    Raise 2
} elseif (-not (Test-Path -LiteralPath $jsTarget)) {
    Write-Host "[ERROR] 缺少 $jsLabel —— 该文件由 WS-webusb（task-3）产出，尚未落地"
    Add-Result 'proto.js (JS)' 'ERROR' '-' '-' '-' "缺少 $jsLabel"
    Raise 2
} else {
    Write-Host "被验证实现 : $jsLabel"
    $r = Invoke-And-Parse -Exe $NodePath -Argv @(
        (Join-Path $RepoRoot 'test\host\run_proto_js.mjs'), $Vectors, $jsTarget
    )
    $st = switch ($r.Exit) { 0 { 'PASS' } 1 { 'FAIL' } default { 'ERROR' } }
    Add-Result 'proto.js (JS)' $st $r.Assert $r.Pass $r.Fail ''
    if ($r.Exit -eq 1) { Raise 1 } elseif ($r.Exit -ne 0) { Raise 2 }
}

# ---------------------------------------------------------------------------
# 3b. I0 状态机（纯转移函数 + 参数化时间；Lead 2026-10 裁决要求）
# ---------------------------------------------------------------------------
Show-Block '3b. I0 状态机（ReliableChannel / LinkSession，时间参数化）'
$smRunner = Join-Path $RepoRoot 'test\host\run_proto_sm.mjs'
$smVectors = Join-Path $RepoRoot 'test\host\sm_vectors.json'
$smTarget = $jsTarget
if ($HarnessSelftest) {
    $smTarget = Join-Path $Selftest 'ref_sm_for_harness_selftest.mjs'
}
if (-not $NodePath) {
    Add-Result 'proto SM (JS)' 'ERROR' '-' '-' '-' '找不到 node'
    Raise 2
} elseif (-not (Test-Path -LiteralPath $smVectors) -or -not (Test-Path -LiteralPath $smRunner)) {
    Write-Host '[ERROR] 缺少 test\host\sm_vectors.json 或 run_proto_sm.mjs'
    Add-Result 'proto SM (JS)' 'ERROR' '-' '-' '-' '缺 SM 向量/runner'
    Raise 2
} else {
    if ($Python -and -not $HarnessSelftest) {
        $tmp2 = Join-Path $env:TEMP ('meida_p4_sm_{0}.json' -f ([guid]::NewGuid().ToString('N').Substring(0, 8)))
        & $Python (Join-Path $RepoRoot 'test\host\gen_sm_vectors.py') --out $tmp2 | Out-Null
        if ($LASTEXITCODE -eq 0) {
            $hA = (Get-FileHash -LiteralPath $tmp2 -Algorithm SHA256).Hash
            $hB = (Get-FileHash -LiteralPath $smVectors -Algorithm SHA256).Hash
            if ($hA -eq $hB) {
                Write-Host '[ok] sm_vectors.json 与生成器重算一致'
            } else {
                Write-Host '[FAIL] sm_vectors.json 与生成器输出不一致（请重跑 test\host\gen_sm_vectors.py）'
                Add-Result 'SM 向量完整性' 'FAIL' '-' '-' '-' 'sm_vectors.json 与生成器漂移'
                Raise 1
            }
        }
    }
    Write-Host "被验证实现 : $smTarget"
    $r = Invoke-And-Parse -Exe $NodePath -Argv @($smRunner, $smVectors, $smTarget)
    $st = switch ($r.Exit) { 0 { 'PASS' } 1 { 'FAIL' } 3 { 'SKIP' } default { 'ERROR' } }
    $note = if ($r.Exit -eq 3) { '接口未就绪：proto.js 还没提供 reliableStep/linkStep（状态机语义此时未被验证）' } else { '' }
    Add-Result 'proto SM (JS)' $st $r.Assert $r.Pass $r.Fail $note
    if ($r.Exit -eq 1) { Raise 1 } elseif ($r.Exit -ne 0 -and $r.Exit -ne 3) { Raise 2 }
}

# ---------------------------------------------------------------------------
# 4. I1 描述符（frozen-allocation 逐行）
# ---------------------------------------------------------------------------
Show-Block '4. I1 描述符 / 端点 / FIFO 预算（frozen-allocation.md 逐行）'
if ($SkipComposite) {
    Write-Host '[skip] -SkipComposite'
    Add-Result 'test_composite_desc' 'SKIP' '-' '-' '-' '-SkipComposite'
} elseif (-not $Gcc) {
    Add-Result 'test_composite_desc' 'ERROR' '-' '-' '-' '找不到 gcc'
    Raise 2
} elseif (-not (Test-Path -LiteralPath $DescC)) {
    Write-Host '[ERROR] 缺少 components/usb_composite/usb_composite_desc.c'
    Add-Result 'test_composite_desc' 'ERROR' '-' '-' '-' '缺少 usb_composite_desc.c'
    Raise 2
} elseif (-not (Test-Path -LiteralPath $DescIntH)) {
    Write-Host '[ERROR] 缺少 components/usb_composite/usb_composite_internal.h（装配器内部头，由 Lead 产出）'
    Add-Result 'test_composite_desc' 'ERROR' '-' '-' '-' '缺 usb_composite_internal.h'
    Raise 2
} elseif ((Get-Content -LiteralPath $DescC -Raw) -match 'PLACEHOLDER') {
    Write-Host '[ERROR] usb_composite_desc.c 仍是占位实现（文件里含 PLACEHOLDER），无法断言冻结表'
    Add-Result 'test_composite_desc' 'ERROR' '-' '-' '-' '占位实现未替换'
    Raise 2
} else {
    $exe = Join-Path $OutDir 'test_composite_desc.exe'
    $cf = & $Gcc -std=c11 -Wall -Wextra -O1 `
        -I (Join-Path $RepoRoot 'components\usb_composite\include') `
        -I (Join-Path $RepoRoot 'components\usb_composite') `
        -I (Join-Path $RepoRoot 'test\host\shim') `
        (Join-Path $RepoRoot 'test\host\test_composite_desc.c') $DescC -o $exe 2>&1
    $cc = $LASTEXITCODE
    $cf | ForEach-Object { Write-Host $_ }
    if ($cc -ne 0) {
        Write-Host '[FAIL] 编译失败'
        Add-Result 'test_composite_desc' 'FAIL' '-' '-' '-' '编译失败'
        Raise 1
    } else {
        $warnCount = @($cf | Where-Object { $_ -match 'warning:' }).Count
        if ($warnCount -gt 0) { Write-Host "[info] 编译产生 $warnCount 条 warning（见上）" }
        $r = Invoke-And-Parse -Exe $exe -Argv @()
        $st = if ($r.Exit -eq 0) { 'PASS' } else { 'FAIL' }
        Add-Result 'test_composite_desc' $st $r.Assert $r.Pass $r.Fail $(if ($warnCount -gt 0) { "$warnCount warning" } else { '无 warning' })
        if ($r.Exit -ne 0) { Raise 1 }

        # 负例路径：超预算/非法端点计数必须被拒绝（同一个 exe 的另一种模式）
        Write-Host ''
        Write-Host '[负例] 超预算/非法计数必须被拒绝'
        $r2 = Invoke-And-Parse -Exe $exe -Argv @('--over-budget')
        $st2 = if ($r2.Exit -eq 0) { 'PASS' } else { 'FAIL' }
        Add-Result 'test_composite_desc(负例)' $st2 $r2.Assert $r2.Pass $r2.Fail '超 IN 预算必须被拒'
        if ($r2.Exit -ne 0) { Raise 1 }
    }

    # ---- 4b. 集成：WS-net **真实** NCM 片段 × 真实装配器（期望值不变，不给真片段开后门） ----
    Write-Host ''
    Write-Host '[4b] 集成：components/usb_net/usb_net_ncm.c 的真实片段 -> 真实装配器'
    $NetNcm   = Join-Path $RepoRoot 'components\usb_net\usb_net_ncm.c'
    $NetStubs = Join-Path $RepoRoot 'components\usb_net\test\host\stubs'
    $Bridge   = Join-Path $RepoRoot 'test\host\real_ncm_bridge.c'
    if (-not (Test-Path -LiteralPath $NetNcm) -or -not (Test-Path -LiteralPath $NetStubs)) {
        Write-Host '[skip] 缺少 components/usb_net/usb_net_ncm.c 或它的 test/host/stubs'
        Add-Result 'test_composite_desc(真实NCM)' 'SKIP' '-' '-' '-' '缺 WS-net 文件'
    } else {
        $exe2 = Join-Path $OutDir 'test_composite_desc_realncm.exe'
        $cf2 = & $Gcc -std=gnu99 -Wall -Wextra -O1 -DUSE_REAL_NCM=1 `
            -I $NetStubs `
            -I (Join-Path $RepoRoot 'components\usb_net\include') `
            -I (Join-Path $RepoRoot 'components\usb_net') `
            -I (Join-Path $RepoRoot 'components\usb_composite\include') `
            -I (Join-Path $RepoRoot 'components\usb_composite') `
            (Join-Path $RepoRoot 'test\host\test_composite_desc.c') $DescC `
            $NetNcm $Bridge -o $exe2 2>&1
        $cc2 = $LASTEXITCODE
        $cf2 | ForEach-Object { Write-Host $_ }
        if ($cc2 -ne 0) {
            Write-Host '[FAIL] 集成编译失败'
            Add-Result 'test_composite_desc(真实NCM)' 'FAIL' '-' '-' '-' '编译失败'
            Raise 1
        } else {
            $r3 = Invoke-And-Parse -Exe $exe2 -Argv @()
            $st3 = if ($r3.Exit -eq 0) { 'PASS' } else { 'FAIL' }
            Add-Result 'test_composite_desc(真实NCM)' $st3 $r3.Assert $r3.Pass $r3.Fail '真片段×真装配器'
            if ($r3.Exit -ne 0) { Raise 1 }
        }
    }
}

# ---------------------------------------------------------------------------
# 汇总
# ---------------------------------------------------------------------------
Write-Host ''
Write-Host '=============================================================================='
Write-Host '汇总'
Write-Host '=============================================================================='
$results | Format-Table -AutoSize Step, Status, Assert, Pass, Fail, Note

$overall = switch ($worst) {
    0 { 'PASS — 离线一致性全部通过（I0 线协议 + I1 描述符）' }
    1 { 'FAIL — 有断言不满足，见上面逐条 FAIL' }
    default { 'ERROR — 工具或输入缺失（缺 wu_codec.c / proto.js / usb_composite_desc.c / gcc / node）' }
}
Write-Host "总判定: $overall"
Write-Host ''
Write-Host '边界声明（必须一起引用，不得省略）：'
Write-Host '  * 本脚本证明的只是「实现与 docs/protocol.md、docs/contracts/frozen-allocation.md 一致」。'
Write-Host '  * **不能**证明真机 USB 链路成立：枚举、Windows 驱动绑定、device interface 注册、'
Write-Host '    Chrome claimInterface、NCM 上网、MTP 浏览、FIFO 实际分配、供电与热稳定，全都不在覆盖范围。'
Write-Host '  * 当前设备未连接 -> 离线 PASS != 真机通过。真机项见 docs/device-test-requests.md。'
Write-Host '=============================================================================='

exit $worst
