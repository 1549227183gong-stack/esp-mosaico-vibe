# hello_world_fps

官方 Hello World 设计（GSP 1.5.1 场景 + 可移植 C UI）的帧率优化版，用于在
ESP-Mosaico 开发分区验证：同一套官方 UI 在 `display_fps_probe` 的显示优化
下的实际表现。

与官方模板的差异只有显示链路优化，UI 与交互保持官方设计原样：

- `main/board_display.c`：复刻 BSP 最小初始化路径，CO5300 QSPI 时钟从
  40MHz 提高到 80MHz，QSPI GPIO 驱动能力从 BSP 默认档 0 提高到档 2。
- `components/espressif__esp_display_present/`：基线 1.1.1 的本地补丁版，
  增加 TE 局部脏区推送与写入窗口 4 像素对齐；`main/idf_component.yml`
  用 `override_path` 固定到该副本。
- `CMakeLists.txt`：Windows 下 gspc depfile 路径修复（工具见 `tools/`）。
- `main/main.c`：打开 GSP 周期帧率日志（`lcd.perf_log = true`）。

优化依据与实测数据：`display_fps_probe` 的 README 与工作区根仓库
`knowledge/display-fps.md`（40MHz → 80MHz 后 FULL 19.8 → 29.7fps、
LOCAL 58fps）。

## 构建与烧录

设备构建固定使用 ESP-IDF 提交
`7b9cc1ac79f865983f59bb8ff3ff43eb74ff1dbe`（target `esp32s31`，
ESP-IDF 5.2.1 不支持）。烧录统一走封装脚本，禁止直接使用 `idf.py flash`
或 `esptool`：

```powershell
# 增量构建系统包并通过 Vibe Mode 写入设备
powershell -ExecutionPolicy Bypass -File tools\flash.ps1

# 设备处于 ROM 下载模式时：先恢复基础固件再烧录
powershell -ExecutionPolicy Bypass -File tools\flash.ps1 -RecoverFirst
```

无变化短路、`-SkipBuild`、`-Force` 等参数与 `display_fps_probe` 的
`tools/flash.ps1` 一致。

## PC 模拟

```powershell
cmake -S pc -B pc/build -G Ninja
cmake --build pc/build
```

可移植后端只验证场景与交互逻辑，不提供设备侧帧率计数器。
