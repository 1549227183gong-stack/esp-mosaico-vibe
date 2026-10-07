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

`main/fps_probe.c` 用 10ms 定时器驱动动画（与 `CONFIG_ESP_GSP_ACTIVE_TICK_MS=10`
对齐），用 1s 窗口差分 GSP 累计计数。单行日志字段含义：

```text
mode=LOCAL wall_fps=58.4 busy_fps=122.4 render_us=369 submit_us=7735 frames=59 regions=6854/6854 full=0 pixels=721581296/721581296 err=0
```

- `wall_fps`：墙钟时间内完成的非空闲帧率。
- `busy_fps`：GSP 实际忙于产帧的时间占比对应的帧率。
- `render_us` / `submit_us`：窗口内每帧栅格化与提交阶段平均耗时。
- `regions`：输入/输出脏区计划累计数；`full` 表示整屏提升累计次数。
- `pixels`：输入/输出脏区像素累计数；`err` 是最近一次 setter 错误码。

`lcd.perf_log = true` 还会每 5 秒输出 GSP 集成层的帧率日志，可用于交叉校验。

## 实验结果（2026-10-08）

同一固件下两个模式 10s 自动切换，代表值：

| 轮次 | QSPI | 模式 | wall_fps | render_us | submit_us | TE 调度 |
| --- | --- | --- | --- | --- | --- | --- |
| 基线 | 40MHz | LOCAL | 57.4–58.4 | 367–370 | 7456–7965 | fast/start，Ttx≈844µs |
| 基线 | 40MHz | FULL | 19.8–20.0 | 4322–4353 | 31883–34185 | slow/end，Ttx=23040µs，占周期 138% |
| 本轮 | 80MHz | LOCAL | 57.4–59.4 | 366–371 | 7270–8335 | fast/start，Ttx≈844µs |
| 本轮 | 80MHz | FULL | 29.7–30.1 | 4310–4498 | 22315–22851 | fast/start，Ttx=11520µs，占周期 69% |

### 全屏为什么曾经只有 20 帧

- `[事实]` 480x480 RGB565 整屏 = 460800B。QSPI 4 线 @40MHz 的理论传输时间
  为 `460800×8/(40e6×4) = 23040µs`，已超过面板扫描周期 Tf≈16.8ms。
- `[事实]` TE 只允许在"GRAM 写行头不越过面板扫描行头"的相位发起传输。
  23040µs > 活跃扫描时间 Ta≈16.24ms，调度器判定 `slow/end`，每帧至少占满
  3 个 TE 周期。
- `[事实]` 面板扫描率 59.3Hz，TE 节流后的帧率只能取 `59.3/n`：
  40MHz → `59.3/3 = 19.8fps`，80MHz → `59.3/2 = 29.7fps`。实测与量化值一致。

### 整屏拆两段（40MHz）——无效

`[事实]` 整屏按行拆 2 段、每段单独等一次 TE 窗口：半屏 Ttx=11797µs
（required=71%）确实能进单个窗口，但一帧需要两次窗口准入，实测 FULL 仍为
19.8fps，且上下半屏落在不同扫描周期，撕裂风险上升。该分支保留在
`present_te_transport.c` 的 `TE_COMPOSE_FULL_SPLIT_SEGMENTS` 宏后，默认 1。

### 30fps 是不是全屏上限

- `[事实]` 80MHz 下 FULL 每帧串行路径为"栅格化 4310–4498µs + submit
  22315–22851µs ≈ 27ms"（submit 含等待 TE 窗口与 DMA 传输本身），已超过
  一个 TE 周期 16.78ms，所以每帧必然占用 2 个 TE 槽位。
- `[事实]` 即使把等待压到 0，可压缩骨架仍是传输 11520µs + 栅格化
  4310–4498µs ≈ 16.0ms，对照 16.78ms 只剩约 0.8ms 余量，整屏脏区还需
  cache 回写与首个 TE 相位对齐。
- `[推断]` 因此"渲染整屏 → 整屏推送"的串行流水只能锁在 2 个 TE 周期
  （29.7fps）。要接近 59.3fps，必须让栅格化与 DMA 传输重叠（双缓冲 +
  历史脏区跟踪）；继续提高总线时钟已无空间（80MHz 是 ESP32-S3 SPI 主机上限）。

根因与完整证据链见 `knowledge/display-fps.md`。

## QSPI 80MHz 实现方式

`main/board_display.c` 不再调用 BSP 的 `bsp_display_new()`（其
`BSP_LCD_PIXEL_CLOCK_HZ` 硬编码 40MHz），改为在工程内复刻最小初始化路径：
电源与板型判定、QSPI 总线、CO5300 命令表、驱动能力、TE 配置，只把面板 IO
时钟与 TE 估算时钟改成 80MHz。触摸与启动画面交接仍复用 BSP 公共接口
（`bsp_touch_new` / `mosaico_boot_handoff_consume`）。

风险：80MHz 超出官方 BSP 既有配置，属实验性外推，长稳、EMI 与不同温度下的
信号完整性尚未复核（`[未验证]`）。

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

## 本地 present 组件补丁

TE 局部脏区推送实验在 `components/espressif__esp_display_present/` 内实现
（基线为官方 1.1.1 全量复制）。`main/idf_component.yml` 通过 `override_path`
把该传递依赖固定到工程内副本，防止组件管理器用注册表版本覆盖补丁。
补丁要点：

- producer 侧在 tile 提交成功时累积帧级脏区并集（`te_frame_dirty_add()`）。
- transport 侧优先使用 submit AREAS，其次使用帧累积并集，首帧强制整屏。
- 局部区超过 64KB bounce 缓冲或映射失败时自动回退整屏推送。
- 多缓冲 + pipeline 路径保持整屏推送（交替缓冲下帧累积脏区不可靠）。

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

# 重复烧录同一份构建：自动核对并跳过（实测约 9 秒）
powershell -ExecutionPolicy Bypass -File tools\flash.ps1

# 强制重新烧录（忽略无变化短路）
powershell -ExecutionPolicy Bypass -File tools\flash.ps1 -Force

# 设备处于 ROM 下载模式（无 ESP-Iris）时：先恢复基础固件再烧录
powershell -ExecutionPolicy Bypass -File tools\flash.ps1 -RecoverFirst

# 复用已有系统包，跳过构建
powershell -ExecutionPolicy Bypass -File tools\flash.ps1 -SkipBuild
```

无变化短路的判定条件（全部命中才跳过）：

1. `build/*-system-update.irisfw` 与 `build/display_fps_probe.elf` 的
   SHA-256 与 `build/.flash-state.json` 中上次成功烧录的记录一致；
2. 工程源码与 `idf_component.yml` 中 `override_path` 本地组件均未晚于
   构建产物修改；
3. 设备 `device-status` 返回的 `firmware_sha256` 与本地 ELF 一致，
   且 `firmware_mode=normal`。

任一条件不满足都会退回完整构建与烧录流程。烧录耗时构成实测：
完整流程约 39–53 秒（其中设备端写入约 20 秒，受 NAND 速度限制，
端口配置无法优化），短路核对约 9 秒。

脚本省略 `-Python` 时会自动探测可用解释器：PATH 中的 python →
ESP-Iris 运行时 → ESP-IDF 虚拟环境，优先选择能导入 pyserial 的解释器。

可选参数：`-DeviceId`（多设备时指定目标）、`-TimeoutSeconds`（烧录超时）、
`-RecoverTimeoutSeconds`（恢复超时）、`-Python`（python 解释器）、
`-IdfPath`（ESP-IDF 路径）、`-Force`（强制重烧）、
`-ExtraArgs`（透传给 `iris system-update`）。
仅在分区表和资源完全一致时才改用 `mosaico.py iris app-update`；实测本设备
在 system-update 后直接 app-update 会因缺少 Vibe Mode 验证记录要求先
`recover`，因此默认流程仍走 system-update。

## 变量矩阵

已完成：`CONFIG_ESP_GSP_ACTIVE_TICK_MS=10`（原 16ms 错位）、`fb.mode` 解析为
`TE_SYNC`、TE 局部脏区推送补丁、整屏拆段对照（无效，默认关闭）、
CO5300 QSPI 40MHz → 80MHz（全屏 19.8 → 29.7fps）。后续仍按一次只改一个变量
的原则扩展：

- 栅格化与 DMA 重叠（双缓冲 + 历史脏区跟踪），目标突破 `59.3/2`。
- `esp_display_present` 的 `fb.mode`：`DOUBLE_PARTIAL` / patch 版多缓冲局部推送。
- `drawbuf.lines`、`drawbuf.buffers`、`te_compose_buffers`。
- QSPI 回退档位（60 / 50MHz）用于对比 80MHz 的信号裕量。

每次实验记录完整 sdkconfig、日志原始文件和结论；结论统一写入 `knowledge/`
并使用 `[事实]`、`[推断]`、`[未验证]` 标签标注证据等级。
