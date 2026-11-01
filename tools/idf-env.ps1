#Requires -Version 5.1
<#
.SYNOPSIS
    Load the EIM-installed ESP-IDF v6.1 environment explicitly into the current session.

.DESCRIPTION
    Why this script exists
    ----------------------
    * This machine's ESP-IDF lives at  C:\esp\v6.1\esp-idf
      (a junction to H:\.Import_CDiskLinker\esp\v6.1\esp-idf), tools in C:\Espressif\tools.
    * New shells - including DSH/pwsh sessions - inherit STALE and NON-EXISTENT values:
          IDF_PATH       = E:\esp32\.espressif\v6.0.2\esp-idf   (does not exist)
          IDF_TOOLS_PATH = E:\esp32\.espressif                  (does not exist)
    * ESP_IDF_VERSION must be set explicitly, otherwise idf.py dies with a TypeError:
          idf_component_manager/idf_extensions.py:388

    Always dot-source it so the caller's scope is modified:
        . .\tools\idf-env.ps1
        Invoke-IdfPy --version

.PARAMETER UseEimProfile
    Fall back to the authoritative EIM-generated profile
    (C:\Espressif\tools\Microsoft.v6.1.PowerShell_profile.ps1) instead of setting the
    variables by hand. Slower (it also registers tab completion) but authoritative.
#>
[CmdletBinding()]
param(
    [string] $IdfPath       = 'C:\esp\v6.1\esp-idf',
    [string] $IdfToolsPath  = 'C:\Espressif\tools',
    [string] $PythonVenv    = 'C:\Espressif\tools\python\v6.1\venv',
    [string] $EspIdfVersion = '6.1',
    [string] $IdfVersion    = '6.1.0',
    [string] $EimProfile    = 'C:\Espressif\tools\Microsoft.v6.1.PowerShell_profile.ps1',
    [switch] $UseEimProfile,
    [switch] $NoLocalStorage,
    [switch] $Quiet
)

$ErrorActionPreference = 'Stop'

function Write-Note([string] $Message) {
    if (-not $Quiet) { Write-Host "[idf-env] $Message" }
}

if ($UseEimProfile) {
    if (-not (Test-Path -LiteralPath $EimProfile)) {
        throw "[idf-env] EIM profile not found: '$EimProfile'. Pass -EimProfile or drop -UseEimProfile."
    }
    . $EimProfile
    Write-Note "loaded via EIM profile: $EimProfile (IDF_PATH=$env:IDF_PATH)"
    return
}

$idfPyScript = Join-Path $IdfPath 'tools\idf.py'
$pythonExe   = Join-Path $PythonVenv 'Scripts\python.exe'

if (-not (Test-Path -LiteralPath $idfPyScript)) {
    throw "[idf-env] ESP-IDF not found: '$idfPyScript'. Pass -IdfPath, or use -UseEimProfile."
}
if (-not (Test-Path -LiteralPath $pythonExe)) {
    throw "[idf-env] Python venv not found: '$pythonExe'. Pass -PythonVenv, or use -UseEimProfile."
}
if (-not (Test-Path -LiteralPath $IdfToolsPath)) {
    throw "[idf-env] IDF_TOOLS_PATH not found: '$IdfToolsPath'."
}

# Resolve a tool directory from a wildcard pattern; returns $null when nothing matches.
function Resolve-FirstDir([string] $Pattern) {
    $hit = Get-Item -Path $Pattern -ErrorAction SilentlyContinue | Select-Object -First 1
    if ($null -ne $hit -and $hit.PSIsContainer) { return $hit.FullName }
    return $null
}

$toolPatterns = @(
    (Join-Path $IdfToolsPath 'ccache\*\ccache-*-windows-x86_64'),
    (Join-Path $IdfToolsPath 'cmake\*\bin'),
    (Join-Path $IdfToolsPath 'dfu-util\*\dfu-util-*-win64'),
    (Join-Path $IdfToolsPath 'esp-clang\*\esp-clang\bin'),
    (Join-Path $IdfToolsPath 'esp-clangd\*\esp-clangd\bin'),
    (Join-Path $IdfToolsPath 'esp32ulp-elf\*\esp32ulp-elf\bin'),
    (Join-Path $IdfToolsPath 'esp32ulp-elf\*\esp32ulp-elf\esp32ulp-elf\bin'),
    (Join-Path $IdfToolsPath 'idf-exe\*'),
    (Join-Path $IdfToolsPath 'ninja\*'),
    (Join-Path $IdfToolsPath 'openocd-esp32\*\openocd-esp32\bin'),
    (Join-Path $IdfToolsPath 'riscv32-esp-elf-gdb\*\riscv32-esp-elf-gdb\bin'),
    (Join-Path $IdfToolsPath 'riscv32-esp-elf\*\riscv32-esp-elf\bin'),
    (Join-Path $IdfToolsPath 'riscv32-esp-elf\*\riscv32-esp-elf\riscv32-esp-elf\bin'),
    (Join-Path $IdfToolsPath 'xtensa-esp-elf-gdb\*\xtensa-esp-elf-gdb\bin'),
    (Join-Path $IdfToolsPath 'xtensa-esp-elf\*\xtensa-esp-elf\bin'),
    (Join-Path $IdfToolsPath 'xtensa-esp-elf\*\xtensa-esp-elf\xtensa-esp-elf\bin'),
    (Join-Path $PythonVenv 'Scripts')
)

$resolved = [System.Collections.Generic.List[string]]::new()
foreach ($pattern in $toolPatterns) {
    $dir = Resolve-FirstDir $pattern
    if ($null -ne $dir -and -not $resolved.Contains($dir)) { $resolved.Add($dir) }
}

# ESP_ROM_ELF_DIR must carry a trailing separator (EIM writes one).
$romElfDir = Resolve-FirstDir (Join-Path $IdfToolsPath 'esp-rom-elfs\*')
$openocdScripts = Resolve-FirstDir (Join-Path $IdfToolsPath 'openocd-esp32\*\openocd-esp32\share\openocd\scripts')

# --- environment variables ---
$env:IDF_PATH                = $IdfPath
$env:IDF_TOOLS_PATH          = $IdfToolsPath
$env:IDF_PYTHON_ENV_PATH     = $PythonVenv
$env:ESP_IDF_VERSION         = $EspIdfVersion
$env:IDF_VERSION             = $IdfVersion
$env:IDF_CCACHE_ENABLE       = '1'
$env:PYTHONUTF8              = '1'
$env:PYTHONIOENCODING        = 'utf-8'
if ($null -ne $romElfDir) { $env:ESP_ROM_ELF_DIR = $romElfDir.TrimEnd('\') + '\' }
if ($null -ne $openocdScripts) { $env:OPENOCD_SCRIPTS = $openocdScripts }
if (-not $NoLocalStorage) {
    # Same as EIM: lets the component manager resolve components from the local cache
    # (C:\Espressif\tools\components) before hitting the registry - works offline.
    $env:IDF_COMPONENT_LOCAL_STORAGE_URL = 'file://' + $IdfToolsPath
}

# --- PATH: tool dirs first, then whatever the caller already had ---
$sep = [System.IO.Path]::PathSeparator
$existing = ($env:PATH -split [regex]::Escape($sep)) | Where-Object { $_ -ne '' }
$ordered  = @($resolved) + ($existing | Where-Object { $resolved -notcontains $_ })
$env:PATH = ($ordered -join $sep)

# --- helpers for the caller ---
$global:IdfPyPython = $pythonExe
$global:IdfPyScript = $idfPyScript
function global:Invoke-IdfPy {
    & $global:IdfPyPython $global:IdfPyScript @args
}

Write-Note "IDF_PATH       = $env:IDF_PATH"
Write-Note "IDF_TOOLS_PATH = $env:IDF_TOOLS_PATH"
Write-Note "ESP_IDF_VERSION= $env:ESP_IDF_VERSION"
Write-Note "python         = $env:IDF_PYTHON_ENV_PATH"
Write-Note "$($resolved.Count) tool dirs prepended to PATH; use Invoke-IdfPy (or the idf.py alias from -UseEimProfile)."
