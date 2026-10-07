<#
.SYNOPSIS
    烧录 display_fps_probe 到 ESP-Mosaico 开发设备。
.DESCRIPTION
    统一通过仓库入口 mosaico.py 烧录，禁止直接调用 idf.py flash / esptool。

    默认流程：
      1. 增量构建 system-update 系统包（应用 + 资源 + 分区表）；
      2. 通过 Vibe Mode 原子写入设备。

    设备处于 ROM 下载模式（无 Vibe Mode / ESP-Iris）时，先用 -RecoverFirst
    恢复基础固件，再执行系统更新。

.PARAMETER SkipBuild
    跳过构建，复用 build 目录中已有的 *-system-update.irisfw。

.PARAMETER RecoverFirst
    先执行 mosaico.py recover 恢复基础固件（Vibe Mode），再烧录本工程。

.PARAMETER DeviceId
    目标设备 ID；只连接一台设备时可以省略。

.PARAMETER TimeoutSeconds
    system-update 烧录超时（秒）；默认使用 mosaico.py 的 900 秒。

.PARAMETER RecoverTimeoutSeconds
    recover 恢复超时（秒）；默认使用 mosaico.py 的 180 秒。

.PARAMETER Python
    python 解释器路径；省略时自动探测（要求 3.10 及以上）：
    先尝试 PATH 中的 python，再尝试 ESP-Iris 运行时与
    ESP-IDF 虚拟环境，优先选择能导入 pyserial 的解释器。

.PARAMETER IdfPath
    ESP-IDF 检出路径；默认读取环境变量 IDF_PATH，
    再回退到工程 build/project_description.json 中记录的路径。

.PARAMETER ExtraArgs
    额外参数，原样透传给 mosaico.py iris system-update。

.EXAMPLE
    powershell -ExecutionPolicy Bypass -File tools\flash.ps1

.EXAMPLE
    powershell -ExecutionPolicy Bypass -File tools\flash.ps1 -RecoverFirst
#>
[CmdletBinding()]
param(
    [switch]$SkipBuild,
    [switch]$RecoverFirst,
    [string]$DeviceId,
    [int]$TimeoutSeconds = 0,
    [int]$RecoverTimeoutSeconds = 0,
    [string]$Python,
    [string]$IdfPath,
    [string[]]$ExtraArgs = @()
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

# 判断目录是否为可用的 ESP-IDF 检出。
function Test-IdfCheckout {
    param([string]$Path)
    if ([string]::IsNullOrWhiteSpace($Path)) {
        return $false
    }
    return (Test-Path -LiteralPath (Join-Path $Path "tools\idf.py") -PathType Leaf) -and
           (Test-Path -LiteralPath (Join-Path $Path "tools\idf_tools.py") -PathType Leaf)
}

# 判断 python 解释器能否导入 pyserial（recover 的 ROM 口检测依赖它）。
function Test-PythonPyserial {
    param([string]$PythonPath)
    if ([string]::IsNullOrWhiteSpace($PythonPath) -or
        -not (Test-Path -LiteralPath $PythonPath -PathType Leaf)) {
        return $false
    }
    $previousPreference = $ErrorActionPreference
    $ErrorActionPreference = "Continue"
    try {
        & $PythonPath -c "import serial" 2>&1 | Out-Null
        return ($null -ne $LASTEXITCODE -and $LASTEXITCODE -eq 0)
    } catch {
        return $false
    } finally {
        $ErrorActionPreference = $previousPreference
    }
}

# 收集候选 python：PATH 中的 python、ESP-Iris 运行时、ESP-IDF 虚拟环境。
function Get-PythonCandidates {
    $candidates = New-Object System.Collections.Generic.List[string]

    $pythonCommand = Get-Command python -ErrorAction SilentlyContinue
    if ($null -ne $pythonCommand) {
        $commandPath = $pythonCommand.Source
        if ([string]::IsNullOrWhiteSpace($commandPath)) {
            $commandPath = $pythonCommand.Definition
        }
        if (-not [string]::IsNullOrWhiteSpace($commandPath)) {
            $candidates.Add($commandPath)
        }
    }

    # ESP-Iris 运行时：mosaico.py 自己创建的按源码路径隔离的解释器。
    $runtimeRoot = Join-Path $env:LOCALAPPDATA "esp-mosaico\runtimes\esp-iris"
    if (Test-Path -LiteralPath $runtimeRoot -PathType Container) {
        Get-ChildItem -LiteralPath $runtimeRoot -Directory -ErrorAction SilentlyContinue |
            Sort-Object Name -Descending |
            ForEach-Object {
                Get-ChildItem -Path (Join-Path $_.FullName "py*\Scripts\python.exe") `
                    -ErrorAction SilentlyContinue |
                    ForEach-Object { $candidates.Add($_.FullName) }
            }
    }

    # ESP-IDF 虚拟环境：recover / 构建过程常用的解释器。
    if (-not [string]::IsNullOrWhiteSpace($env:IDF_PYTHON_ENV_PATH)) {
        $candidates.Add((Join-Path $env:IDF_PYTHON_ENV_PATH "Scripts\python.exe"))
    }
    $idfEnvsRoot = "C:\Espressif\python_env"
    if (Test-Path -LiteralPath $idfEnvsRoot -PathType Container) {
        Get-ChildItem -LiteralPath $idfEnvsRoot -Directory -ErrorAction SilentlyContinue |
            Sort-Object Name -Descending |
            ForEach-Object { $candidates.Add((Join-Path $_.FullName "Scripts\python.exe")) }
    }

    return @($candidates | Select-Object -Unique)
}

# 执行一条 python 命令并返回退出码。
function Invoke-PythonCommand {
    param(
        [string]$PythonPath,
        [string[]]$Arguments,
        [string]$WorkingDirectory
    )
    Push-Location -LiteralPath $WorkingDirectory
    try {
        # 原生命令输出直接送宿主，避免混入函数返回值（返回值只保留退出码）。
        & $PythonPath @Arguments | Out-Host
        if ($null -eq $LASTEXITCODE) {
            return 1
        }
        return [int]$LASTEXITCODE
    } finally {
        Pop-Location
    }
}

# 1. 定位工程与仓库根目录（脚本位于 <工程>/tools）。
$projectRoot = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot "..")).Path
$repoRoot = (Resolve-Path -LiteralPath (Join-Path $projectRoot "..\..")).Path
$entryScript = Join-Path $repoRoot "mosaico.py"
if (-not (Test-Path -LiteralPath $entryScript -PathType Leaf)) {
    throw "未找到 mosaico.py 入口：$entryScript"
}

# 2. 选择 python 解释器：显式参数优先；否则逐个探测候选解释器，
#    选中第一个能导入 pyserial 的（PATH 中的 python 往往缺这一项）。
if ([string]::IsNullOrWhiteSpace($Python)) {
    $selectedPython = $null
    $fallbackPython = $null
    foreach ($candidate in (Get-PythonCandidates)) {
        if ([string]::IsNullOrWhiteSpace($fallbackPython)) {
            $fallbackPython = $candidate
        }
        if (Test-PythonPyserial $candidate) {
            $selectedPython = $candidate
            break
        }
    }
    if ($null -eq $selectedPython) {
        $selectedPython = $fallbackPython
    }
    if ([string]::IsNullOrWhiteSpace($selectedPython)) {
        throw "未找到可用的 python，请通过 -Python 指定解释器路径。"
    }
    $Python = $selectedPython
}
if (-not (Test-Path -LiteralPath $Python -PathType Leaf)) {
    throw "python 解释器不存在：$Python"
}
if (-not (Test-PythonPyserial $Python)) {
    Write-Warning "[烧录] $Python 缺少 pyserial；-RecoverFirst 的 ROM 口检测可能失败。"
}

# 3. 解析 ESP-IDF 路径：显式参数 > IDF_PATH 环境变量 > 构建记录。
$buildDescription = Join-Path $projectRoot "build\project_description.json"
if ([string]::IsNullOrWhiteSpace($IdfPath) -and
    -not [string]::IsNullOrWhiteSpace($env:IDF_PATH)) {
    $IdfPath = $env:IDF_PATH
}
if (-not (Test-IdfCheckout $IdfPath) -and
    (Test-Path -LiteralPath $buildDescription -PathType Leaf)) {
    try {
        $description = Get-Content -LiteralPath $buildDescription -Raw -Encoding UTF8 |
            ConvertFrom-Json
        if (-not [string]::IsNullOrWhiteSpace($description.idf_path)) {
            $IdfPath = $description.idf_path
        }
    } catch {
        # 构建记录不可用时保留原值，随后统一报错。
    }
}
if (-not (Test-IdfCheckout $IdfPath)) {
    throw "未找到可用的 ESP-IDF，请设置 IDF_PATH 或用 -IdfPath 指定（需包含 tools\idf.py）。"
}
$env:IDF_PATH = (Resolve-Path -LiteralPath $IdfPath).Path

# 4. 本工作区已验证的组件源配置；只影响本脚本启动的进程。
$env:IDF_COMPONENT_STORAGE_URL = "default"

Write-Host "[烧录] 工程：$projectRoot"
Write-Host "[烧录] ESP-IDF：$env:IDF_PATH"
Write-Host "[烧录] python：$Python"

# 5. ROM 模式下先恢复基础固件（Vibe Mode），否则没有 ESP-Iris 可写入。
if ($RecoverFirst) {
    $recoverArguments = @($entryScript, "recover")
    if ($RecoverTimeoutSeconds -gt 0) {
        $recoverArguments += @("--timeout", "$RecoverTimeoutSeconds")
    }
    Write-Host "[烧录] 恢复基础固件：python $($recoverArguments -join ' ')"
    $recoverExit = Invoke-PythonCommand -PythonPath $Python `
        -Arguments $recoverArguments -WorkingDirectory $repoRoot
    if ($recoverExit -ne 0) {
        Write-Host "[烧录] 基础固件恢复失败（退出码 $recoverExit），已中止。" -ForegroundColor Red
        exit $recoverExit
    }
}

# 6. -SkipBuild 时提前确认系统包存在，避免复用了旧包或空目录。
if ($SkipBuild) {
    $existingBundle = @(
        Get-ChildItem -LiteralPath (Join-Path $projectRoot "build") `
            -Filter "*-system-update.irisfw" -File -ErrorAction SilentlyContinue
    ) | Select-Object -First 1
    if ($null -eq $existingBundle) {
        throw "未找到已有系统包（build\*-system-update.irisfw）；请去掉 -SkipBuild 先构建。"
    }
}

# 7. 构建并烧录系统更新包。
$cliArguments = @(
    $entryScript,
    "iris", "system-update",
    "--project", "projects/display_fps_probe"
)
if ($SkipBuild) {
    $cliArguments += "--skip-build"
}
if (-not [string]::IsNullOrWhiteSpace($DeviceId)) {
    $cliArguments += @("--device-id", $DeviceId)
}
if ($TimeoutSeconds -gt 0) {
    $cliArguments += @("--timeout", "$TimeoutSeconds")
}
if ($ExtraArgs.Count -gt 0) {
    $cliArguments += $ExtraArgs
}

Write-Host "[烧录] 命令：python $($cliArguments -join ' ')"
$flashExit = Invoke-PythonCommand -PythonPath $Python `
    -Arguments $cliArguments -WorkingDirectory $repoRoot

if ($flashExit -ne 0) {
    Write-Host "[烧录] 失败，mosaico.py 退出码：$flashExit" -ForegroundColor Red
    exit $flashExit
}

Write-Host "[烧录] 完成：display_fps_probe 已写入设备。" -ForegroundColor Green
exit 0
