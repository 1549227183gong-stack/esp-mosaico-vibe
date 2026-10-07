/*
 * 显示帧率探测模块（场景 "probe"）。
 *
 * 负载分两段循环，每段持续 10 秒：
 * - LOCAL：只移动 80x80 方块，制造局部脏区；
 * - FULL ：整屏双色背景交替，制造全屏脏区。
 *
 * 16ms 定时器驱动负载，1s 定时器采样 ESP-GSP 的帧计数与渲染阶段
 * 耗时，输出便于跨构建对比的单行指标。PC 后端只验证逻辑，不提供
 * 设备侧调试计数器。
 */

#include "fps_probe.h"

#include <stdio.h>
#include <string.h>

#include "bundle_gsp.h"

#if defined(ESP_PLATFORM)
#include "esp_gsp_debug.h"
#include "esp_log.h"
#include "esp_timer.h"
#endif

#define PROBE_TICK_PERIOD_MS  16U
#define PROBE_SAMPLE_PERIOD_MS 1000U
#define PROBE_MODE_PERIOD_MS  10000U
#define PROBE_MODE_TICKS      (PROBE_MODE_PERIOD_MS / PROBE_TICK_PERIOD_MS)

#define PROBE_MOVER_MIN_X     0
#define PROBE_MOVER_MAX_X     400
#define PROBE_MOVER_START_X   200
#define PROBE_MOVER_Y         340
#define PROBE_MOVER_STEP_X    8

#define PROBE_MODE_LOCAL      0U
#define PROBE_MODE_FULL       1U
#define PROBE_MODE_COUNT      2U

#define PROBE_FPS_TEXT_SIZE   16

#define PROBE_STATUS_BOOT     "BOOT"
#define PROBE_STATUS_LOCAL    "MOVE LOCAL"
#define PROBE_STATUS_FULL     "FULL FLIP"

#if defined(ESP_PLATFORM)
static const char *const TAG = "fps_probe";
#endif

typedef struct {
    uint32_t frames;
    uint64_t busy_us;
    uint64_t render_us;
    uint64_t submit_us;
    uint64_t wall_us;
} probe_sample_t;

struct fps_probe {
    esp_gsp_handle_t ui;
    void *tick_timer;
    void *sample_timer;
    uint32_t mode;
    uint32_t mode_ticks;
    int32_t mover_x;
    int32_t mover_dir;
    bool flip;
    esp_err_t last_error;
#if defined(ESP_PLATFORM)
    probe_sample_t prev;
#endif
};

static fps_probe_t s_instance;
static bool s_active;

static bool probe_is_active(const fps_probe_t *probe)
{
    return s_active && probe == &s_instance;
}

static void remember_error(fps_probe_t *probe, esp_err_t err)
{
    if (err != ESP_OK) {
        probe->last_error = err;
    }
}

static const char *mode_log_name(uint32_t mode)
{
    return mode == PROBE_MODE_FULL ? "FULL" : "LOCAL";
}

static const char *mode_display_name(uint32_t mode)
{
    return mode == PROBE_MODE_FULL ? PROBE_STATUS_FULL : PROBE_STATUS_LOCAL;
}

static void reset_visual(fps_probe_t *probe)
{
    remember_error(probe, gsp_probe_bg_a_set_visible(probe->ui, true));
    remember_error(probe, gsp_probe_bg_b_set_visible(probe->ui, false));
    remember_error(probe, gsp_probe_mover_set_position(
                               probe->ui, PROBE_MOVER_START_X, PROBE_MOVER_Y));
    remember_error(probe, gsp_probe_fps_text_set_text(probe->ui, "0.0"));
    remember_error(probe, gsp_probe_status_text_set_text(
                               probe->ui, PROBE_STATUS_BOOT));
}

static void apply_mode(fps_probe_t *probe, uint32_t mode)
{
    probe->mode = mode;
    probe->mode_ticks = 0U;
    probe->flip = false;
    remember_error(probe, gsp_probe_bg_a_set_visible(probe->ui, true));
    remember_error(probe, gsp_probe_bg_b_set_visible(probe->ui, false));
    remember_error(probe, gsp_probe_status_text_set_text(
                               probe->ui, mode_display_name(mode)));
}

static void move_mover(fps_probe_t *probe)
{
    probe->mover_x += probe->mover_dir * PROBE_MOVER_STEP_X;
    if (probe->mover_x >= PROBE_MOVER_MAX_X) {
        probe->mover_x = PROBE_MOVER_MAX_X;
        probe->mover_dir = -1;
    } else if (probe->mover_x <= PROBE_MOVER_MIN_X) {
        probe->mover_x = PROBE_MOVER_MIN_X;
        probe->mover_dir = 1;
    }
    remember_error(probe, gsp_probe_mover_set_position(
                               probe->ui, probe->mover_x, PROBE_MOVER_Y));
}

static void tick(esp_gsp_handle_t ui, void *ctx)
{
    fps_probe_t *probe = ctx;
    if (!probe_is_active(probe)) {
        return;
    }

    ++probe->mode_ticks;
    if (probe->mode_ticks >= PROBE_MODE_TICKS) {
        apply_mode(probe, (probe->mode + 1U) % PROBE_MODE_COUNT);
        return;
    }

    if (probe->mode == PROBE_MODE_FULL) {
        probe->flip = !probe->flip;
        remember_error(probe, gsp_probe_bg_a_set_visible(ui, !probe->flip));
        remember_error(probe, gsp_probe_bg_b_set_visible(ui, probe->flip));
        return;
    }

    move_mover(probe);
}

#if defined(ESP_PLATFORM)
static void sample_snapshot(esp_gsp_handle_t ui, probe_sample_t *out)
{
    esp_gsp_render_stats(ui, &out->frames, &out->busy_us);
    esp_gsp_render_phases(ui, &out->render_us, &out->submit_us);
    out->wall_us = (uint64_t)esp_timer_get_time();
}

static uint32_t rate_x10(uint32_t frames, uint64_t elapsed_us)
{
    if (elapsed_us == 0U) {
        return 0U;
    }
    const uint64_t scaled = (uint64_t)frames * 10000000ULL + elapsed_us / 2U;
    return (uint32_t)(scaled / elapsed_us);
}

// 求每帧平均耗时（微秒，四舍五入）。
static uint32_t per_frame_us(uint64_t total_us, uint32_t frames)
{
    if (frames == 0U) {
        return 0U;
    }
    return (uint32_t)((total_us + frames / 2U) / frames);
}

static void log_sample(const fps_probe_t *probe, uint32_t frames,
                       uint64_t wall_us, uint64_t busy_us,
                       uint64_t render_us, uint64_t submit_us)
{
    const uint32_t wall_x10 = rate_x10(frames, wall_us);
    const uint32_t busy_x10 = rate_x10(frames, busy_us);
    const uint32_t render_avg_us = per_frame_us(render_us, frames);
    const uint32_t submit_avg_us = per_frame_us(submit_us, frames);
    esp_gsp_region_stats_t regions = {0};
    esp_gsp_region_stats(probe->ui, &regions);

    ESP_LOGI(TAG,
             "mode=%s wall_fps=%u.%u busy_fps=%u.%u "
             "render_us=%u submit_us=%u frames=%u "
             "regions=%u/%u full=%u pixels=%llu/%llu err=%d",
             mode_log_name(probe->mode), wall_x10 / 10U, wall_x10 % 10U,
             busy_x10 / 10U, busy_x10 % 10U,
             render_avg_us, submit_avg_us, frames,
             regions.input_regions, regions.output_regions,
             regions.full_promotions,
             (unsigned long long)regions.input_pixels,
             (unsigned long long)regions.output_pixels,
             (int)probe->last_error);
}

static void sample_tick(esp_gsp_handle_t ui, void *ctx)
{
    fps_probe_t *probe = ctx;
    if (!probe_is_active(probe)) {
        return;
    }

    probe_sample_t now;
    sample_snapshot(ui, &now);
    const uint32_t frames = now.frames - probe->prev.frames;
    const uint64_t wall_us = now.wall_us - probe->prev.wall_us;
    const uint64_t busy_us = now.busy_us - probe->prev.busy_us;
    const uint64_t render_us = now.render_us - probe->prev.render_us;
    const uint64_t submit_us = now.submit_us - probe->prev.submit_us;
    probe->prev = now;

    log_sample(probe, frames, wall_us, busy_us, render_us, submit_us);

    char fps_text[PROBE_FPS_TEXT_SIZE];
    const uint32_t fps_x10 = rate_x10(frames, wall_us);
    snprintf(fps_text, sizeof(fps_text), "%u.%u",
             (unsigned int)(fps_x10 / 10U),
             (unsigned int)(fps_x10 % 10U));
    remember_error(probe, gsp_probe_fps_text_set_text(ui, fps_text));
    remember_error(probe, gsp_probe_status_text_set_text(
                               ui, mode_display_name(probe->mode)));
}
#else
static void sample_tick(esp_gsp_handle_t ui, void *ctx)
{
    fps_probe_t *probe = ctx;
    if (!probe_is_active(probe)) {
        return;
    }
    remember_error(probe, gsp_probe_fps_text_set_text(ui, "0.0"));
    fprintf(stderr, "fps_probe: sample mode=%s (PC counters unavailable)\n",
            mode_log_name(probe->mode));
}
#endif

esp_err_t fps_probe_create(esp_gsp_handle_t ui, fps_probe_t **out_probe)
{
    if (ui == NULL || out_probe == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    *out_probe = NULL;
    if (s_active) {
        return ESP_ERR_INVALID_STATE;
    }

    fps_probe_t *probe = &s_instance;
    memset(probe, 0, sizeof(*probe));
    probe->ui = ui;
    probe->last_error = ESP_OK;
    probe->mover_x = PROBE_MOVER_START_X;
    probe->mover_dir = 1;
    apply_mode(probe, PROBE_MODE_LOCAL);
    if (probe->last_error != ESP_OK) {
        const esp_err_t err = probe->last_error;
        reset_visual(probe);
        memset(probe, 0, sizeof(*probe));
        return err;
    }

    probe->tick_timer = esp_gsp_timer_create(
        ui, PROBE_TICK_PERIOD_MS, tick, probe);
    if (probe->tick_timer == NULL) {
        reset_visual(probe);
        memset(probe, 0, sizeof(*probe));
        return ESP_ERR_NO_MEM;
    }

    probe->sample_timer = esp_gsp_timer_create(
        ui, PROBE_SAMPLE_PERIOD_MS, sample_tick, probe);
    if (probe->sample_timer == NULL) {
        (void)esp_gsp_timer_delete(ui, probe->tick_timer);
        reset_visual(probe);
        memset(probe, 0, sizeof(*probe));
        return ESP_ERR_NO_MEM;
    }

#if defined(ESP_PLATFORM)
    sample_snapshot(ui, &probe->prev);
#endif
    s_active = true;
    *out_probe = probe;
    return ESP_OK;
}

void fps_probe_destroy(fps_probe_t *probe)
{
    if (!probe_is_active(probe)) {
        return;
    }

    fps_probe_t *instance = &s_instance;
    s_active = false;
    if (instance->sample_timer != NULL) {
        (void)esp_gsp_timer_delete(instance->ui, instance->sample_timer);
        instance->sample_timer = NULL;
    }
    if (instance->tick_timer != NULL) {
        (void)esp_gsp_timer_delete(instance->ui, instance->tick_timer);
        instance->tick_timer = NULL;
    }
    reset_visual(instance);
    memset(instance, 0, sizeof(*instance));
}
