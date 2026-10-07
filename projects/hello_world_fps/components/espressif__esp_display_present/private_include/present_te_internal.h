/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include "esp_display_present_te_compose.h"
#include "esp_display_present_frame_tracker.h"
#include "esp_display_present_drawbuf.h"
#include "esp_display_present_te.h"
#include "esp_display_present_target.h"
#include "present_async_copy.h"
#include "present_buffer_repair.h"
#include "present_te_pool.h"

#include "esp_lcd_panel_ops.h"

#define PRESENT_TE_COMPOSE_MAX_BUFFERS ESP_DISPLAY_PRESENT_MAX_INFLIGHT_FRAMES

/*
 * 局部脏区 bounce 缓冲容量。脏区并集超过该容量时提交路径回退整屏推送，
 * 因此这里是"局部优化生效范围"与 PSRAM 占用的折中：64KB 可覆盖
 * 约 240x136 或 128x256 的局部刷新区域。
 */
#define PRESENT_TE_DIRTY_BOUNCE_BYTES (64U * 1024U)

struct esp_display_present_te_compose {
    /* Sync / frame lifecycle */
    esp_display_present_frame_tracker_t tracker;
    esp_display_present_te_sync_context_t *te_ctx;
    uint8_t te_timeouts;
    uint64_t building_previous_frame;
    bool te_degraded;
    bool stopped;
    uint8_t compose_buffer_count;
    bool has_active_buffer;
    esp_display_present_target_t *target;
    void *display_buffer;
    present_buffer_repair_state_t repair;
    present_te_pool_t pool;

    /* Panel out */
    esp_lcd_panel_handle_t panel;
    esp_display_present_rotation_t rotation;
    uint16_t logical_width;
    uint16_t logical_height;
    size_t max_damage_areas;
    uint32_t transfer_timeout_ms;
    uint32_t acquire_timeout_ms;

    esp_display_present_drawbuf_pool_t drawbuf_pool;
    uint8_t next_drawbuf;
    void *ppa_handle;
    /** Swap + tile copies overlap rendering when the drawbuf pool has two. */
    present_async_copy_t async_copy;

    struct {
        uint8_t *pixels;
        uint8_t *buffers[PRESENT_TE_COMPOSE_MAX_BUFFERS];
        size_t stride_bytes;
        uint16_t width;
        uint16_t height;
        uint8_t color_bytes;
        bool panel_byte_order;
    } draw;

    /*
     * 局部脏区推送用的紧凑 bounce 缓冲。面板驱动的 draw_bitmap 只接受行内
     * 紧凑排列的像素，而整屏合成缓冲的 stride 是全屏行宽；提交单缓冲帧时
     * 先把脏区并集逐行裁到这块缓冲，再只推送该区域。容量不足时回退整屏
     * 推送。仅单缓冲（非 pipeline）提交路径使用。
     */
    uint8_t *dirty_bounce;
    size_t dirty_bounce_bytes;

    /*
     * 帧脏区累积（逻辑坐标）。单缓冲提交路径用它记录本帧 tile 提交的并集：
     * producer 明确上报 AREAS 时以 AREAS 为准，否则用该并集推导局部推送，
     * 避免整屏重推。首帧 ever_presented=false 时强制整屏。
     */
    esp_display_present_area_t frame_dirty_union;
    bool frame_dirty_valid;
    bool ever_presented;
};

esp_err_t present_te_compose_push_frame(
    esp_display_present_te_compose_t *te_compose,
    const esp_display_presenter_submit_t *submit, uint8_t *pixels,
    bool *out_submitted_any);

esp_err_t present_te_compose_repair_create(
    esp_display_present_te_compose_t *te_compose);
void present_te_compose_repair_destroy(
    esp_display_present_te_compose_t *te_compose);
