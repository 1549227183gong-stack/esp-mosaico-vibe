/*
 * 显示帧率探测模块对外接口（场景 "probe"）。
 *
 * 职责：以固定节奏驱动一个可复现的两段式动画负载，并按 1s 窗口采样
 * ESP-GSP 的帧计数与渲染阶段耗时，输出可跨构建对比的指标行。
 *
 * 边界：只调用 esp_gsp / esp_gsp_debug 公共 API；不触碰 BSP、面板驱动
 * 与 QSPI 总线参数（那些通过 sdkconfig 与 board_display.c 调整）。
 *
 * 主要依赖：esp-gsp（场景数据来自 ui/main.json 编译产物）。
 *
 * 对外契约：
 * - fps_probe_create() 成功后才注册定时器；失败返回 esp_err_t 且不占用句柄。
 * - fps_probe_destroy() 需在 GSP 句柄销毁之前调用；可安全重复调用。
 * - 定时器回调运行在 GSP 渲染任务上下文，调用方不要并发销毁。
 * - 单实例模块：重复 create 返回 ESP_ERR_INVALID_STATE。
 */
#pragma once

#include "esp_gsp.h"

/* 不透明句柄：内部状态由 fps_probe.c 独占，避免跨模块访问。 */
typedef struct fps_probe fps_probe_t;

/**
 * @brief 初始化探测画面并启动采样定时器。
 *
 * @param ui         已启动的 GSP 句柄，生命周期需覆盖整个探测过程。
 * @param out_probe  成功时写入模块单例句柄；失败时写 NULL。
 * @return ESP_OK；参数非法返回 ESP_ERR_INVALID_ARG；已创建返回
 *         ESP_ERR_INVALID_STATE；定时器创建失败返回 ESP_ERR_NO_MEM。
 */
esp_err_t fps_probe_create(esp_gsp_handle_t ui, fps_probe_t **out_probe);

/**
 * @brief 停止采样定时器并释放单例。
 *
 * @param probe  fps_probe_create() 返回的句柄；NULL 或非本模块句柄时忽略。
 */
void fps_probe_destroy(fps_probe_t *probe);
