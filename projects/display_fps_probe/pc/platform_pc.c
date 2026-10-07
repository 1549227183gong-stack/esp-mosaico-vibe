/*
 * PC 端 GSP 模拟后端入口。
 *
 * 该文件只负责把 sim_bridge 的生命周期回调转接到 fps_probe 模块；
 * 设备侧调试计数器在 PC 端不可用，具体降级逻辑由 fps_probe.c 处理。
 */

// SPDX-License-Identifier: Apache-2.0

#include <stdio.h>

#include "gsp_sim_bridge.h"
#include "fps_probe.h"

static fps_probe_t *state;

esp_gsp_err_t gsp_bridge_app_init(esp_gsp_handle_t ui)
{
    return fps_probe_create(ui, &state);
}

void gsp_bridge_app_deinit(esp_gsp_handle_t ui)
{
    (void)ui;
    fps_probe_destroy(state);
    state = NULL;
    fprintf(stderr, "fps_probe_backend: stopped\n");
}
