# display_fps_probe

用于实测并尝试提升 ESP-Mosaico 显示帧率的 Spike 工程。工程保持 GSP
1.5.1 场景和可移植 C UI，同一套 `ui/main.json` 同时服务设备与 PC 模拟。

## 目的

- 建立可复现的显示负载，分别测量局部脏区和全屏脏区下的帧率。
- 输出 `wall_fps`、`busy_fps`、单帧 `render_ms`/`submit_ms` 与区域规划计数，
  作为后续 QSPI 时钟、GSP tick、present 模式等变量矩阵的基线。
- 不修改 `reference/` 和官方固件；实验只在 `workspace/vibe` 开发分区进行。

## 负载与采样

两个模式每 10 秒自动切换：

- `LOCAL`：只让 80x80 方块在 480 像素宽度内往返，制造局部脏区。
- `FULL`：整屏双色背景交替，强制产生全屏脏区。

`main/fps_probe.c` 用 16ms 定时器驱动动画，用 1s 窗口差分 GSP 累计计数。
单行日志字段含义：

```text
mode=LOCAL wall_fps=59.9 busy_fps=59.8 render_ms=2.10 submit_ms=1.30 frames=60 regions=1/1 full=0 pixels=6400/6400 err=0
```

- `wall_fps`：墙钟时间内完成的非空闲帧率。
- `busy_fps`：GSP 实际忙于产帧的时间占比对应的帧率。
- `render_ms` / `submit_ms`：每帧栅格化与提交阶段平均耗时。
- `regions`：输入/输出脏区计划数；`full` 表示整屏提升次数。
- `pixels`：输入/输出脏区像素数；`err` 是最近一次 setter 错误码。

`lcd.perf_log = true` 还会每 5 秒输出 GSP 集成层的帧率日志，可用于交叉校验。

## 构建

设备构建固定使用 ESP-IDF 提交 `7b9cc1ac79f865983f59bb8ff3ff43eb74ff1dbe`。
当前 target 是 `esp32s31`，ESP-IDF 5.2.1 不支持该芯片，因此不能降到 5.2.1。

```powershell
$env:IDF_COMPONENT_STORAGE_URL="default"
python "E:\esp-mosaico\workspace\vibe\submodule\esp-mosaico-utils\mosaico-tools\skills\idf-low-noise-build\scripts\idf_low_noise_build.py" `
  --project "E:\esp-mosaico\workspace\vibe\projects\display_fps_probe" `
  --idf-path "C:\Espressif\frameworks\esp-idf-7b9cc1a" `
  build
```

构建产物：

- `build/display_fps_probe.bin`
- `build/esp-idf/main/gsp_gen_bundle/bundle_gsp.h`
- `build/esp-idf/main/gsp_gen_bundle/scene0/probe_objects.h`

## Windows 构建修复

gspc 0.6.1 在 Windows 上会把 depfile 里的资源路径写成扩展长度路径的转义形式
（`\\\\?\\E:\\...`），Ninja 会把它记为非法路径，导致第二次增量构建在启动阶段
直接失败。本工程在 `CMakeLists.txt` 中自动生成
`build/gspc-fix/gspc_depfile_fix.cmd` 包装器：先调用真实 gspc，再由
`tools/gspc_depfile_fix.py` 就地规范化 depfile，其余参数原样透传。

- 修复逻辑单元测试：`python -m unittest discover -s tools/tests -v`
- 临时禁用（例如上游修复后对比验证）：配置时加
  `-DMOSAICO_DISABLE_GSPC_DEPFILE_FIX=ON`
- 真实 gspc 路径记录在 CMake 缓存 `MOSAICO_GSPC_REAL_EXECUTABLE`，
  包装器不会递归调用自身

## PC 模拟

从本工程目录执行：

```powershell
cmake -S pc -B pc/build -G Ninja
cmake --build pc/build
```

PC 后端可以验证场景加载、可视性切换和定时器回调；sim_bridge 不提供
`esp_gsp_render_stats` 等设备调试计数器，因此 PC 端日志明确标记
`PC counters unavailable`。

## 烧录

烧录统一使用封装脚本 `tools/flash.ps1`（内部调用仓库 `mosaico.py` 的
`iris system-update`），禁止直接使用 `idf.py flash` 或 `esptool`。
脚本会自动解析工程 `build/project_description.json` 中记录的 ESP-IDF 路径，
无需手工设置 `IDF_PATH`：

```powershell
# 增量构建系统包并烧录（首次安装、分区表或资源变化时使用）
powershell -ExecutionPolicy Bypass -File tools\flash.ps1

# 设备处于 ROM 下载模式（无 ESP-Iris）时：先恢复基础固件再烧录
powershell -ExecutionPolicy Bypass -File tools\flash.ps1 -RecoverFirst

# 复用已有系统包，跳过构建
powershell -ExecutionPolicy Bypass -File tools\flash.ps1 -SkipBuild
```

脚本省略 `-Python` 时会自动探测可用解释器：PATH 中的 python →
ESP-Iris 运行时 → ESP-IDF 虚拟环境，优先选择能导入 pyserial 的解释器。

可选参数：`-DeviceId`（多设备时指定目标）、`-TimeoutSeconds`（烧录超时）、
`-RecoverTimeoutSeconds`（恢复超时）、`-Python`（python 解释器）、
`-IdfPath`（ESP-IDF 路径）、`-ExtraArgs`（透传给 `iris system-update`）。
仅在分区表和资源完全一致时才改用 `mosaico.py iris app-update`。

## 变量矩阵

基线测量完成后，按一次只改一个变量的原则扩展：

- CO5300 QSPI 时钟：40 / 45 / 50 MHz。
- `CONFIG_ESP_GSP_ACTIVE_TICK_MS`：10 / 6 / 16。
- `esp_display_present` 的 `fb.mode`：`AUTO` / `TE_SYNC` / `DOUBLE_PARTIAL`。
- `drawbuf.lines`、`drawbuf.buffers`、`te_compose_buffers`。

每次实验记录完整 sdkconfig、日志原始文件和结论；结论统一写入 `knowledge/`
并使用 `[事实]`、`[推断]`、`[未验证]` 标签标注证据等级。
