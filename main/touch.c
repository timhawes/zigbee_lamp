#include "touch.h"
#include "sdkconfig.h"

#if CONFIG_LAMP_INPUT_TOUCH

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/mcpwm_prelude.h"
#include "hal/mcpwm_ll.h"
#include "soc/soc_caps.h"
#include "esp_attr.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_timer.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "light_output.h"

#define CAP_RES_HZ      80000000 /* capture timer resolution: 12.5 ns */
#define GEN_RES_HZ      1000000  /* drive timer resolution */
#define FRAME_MS        10
#define CAL_SKIP_MS     20       /* let the first window fill before calibrating */
#define CAL_MS          300
#define TRACK_DOWN_K    0.05f                     /* fast: about 200 ms */
#define TRACK_UP_K      (FRAME_MS / 10000.0f)     /* slow: about 10 s */
#define NO_SIGNAL_WARN_MS 1000  /* warn after this long without edges, then every 5 s */
#define NOISE_DECAY     0.99f    /* interference peak-hold: halves in about 0.7 s */

/* Interference scan (see touch_scan), in 10 ms frames. */
#define SCAN_OFF_FRAMES    25    /* PWM off: settle, then average the reference */
#define SCAN_SETTLE_FRAMES 10
#define SCAN_RAMP_FRAMES   200   /* duty 0 -> 100 % */
#define SCAN_LAG_FRAMES    3     /* a reading reflects the duty set this many frames earlier */
#define SCAN_BANDS         10

static const char *TAG = "touch";

static portMUX_TYPE s_isr_lock = portMUX_INITIALIZER_UNLOCKED;
static portMUX_TYPE s_stats_lock = portMUX_INITIALIZER_UNLOCKED;
static uint32_t s_half_ticks;  /* half the drive period, in capture ticks */
static uint32_t s_acc;         /* sum of accepted edge delays (capture ticks) */
static uint32_t s_count;       /* number of accepted edges */
static bool s_last_was_rise;
static touch_stats_t s_stats;
static volatile bool s_touched;
static volatile int64_t s_stream_until_us;
static int s_drive_timer_id = -1;  /* hardware index of the drive timer, for touch_drive_time_us() */
static volatile float s_noise_pm;  /* interference peak-hold, normal operation */
static volatile int64_t s_scan_until_us;
static volatile bool s_scanning;
static volatile int s_scan_pm = -1; /* worst |delta| in the last complete sweep */

/* The capture timer is reset at every rising edge of SEND, so a rising capture is the charge delay and a
 * falling capture minus half a period is the discharge delay. Rises are only valid in the first half and
 * falls in the second, and they must alternate, which drops the extra edges a slow, noisy ramp can cause
 * near the threshold. */
static bool IRAM_ATTR on_capture(mcpwm_cap_channel_handle_t chan, const mcpwm_capture_event_data_t *e, void *ctx)
{
    uint32_t v = e->cap_value;
    portENTER_CRITICAL_ISR(&s_isr_lock);
    if (e->cap_edge == MCPWM_CAP_EDGE_POS) {
        if (v < s_half_ticks && !s_last_was_rise) {
            s_acc += v;
            s_count++;
            s_last_was_rise = true;
        }
    } else if (v >= s_half_ticks && s_last_was_rise) {
        s_acc += v - s_half_ticks;
        s_count++;
        s_last_was_rise = false;
    }
    portEXIT_CRITICAL_ISR(&s_isr_lock);
    return false;
}

static void publish(const touch_stats_t *st)
{
    portENTER_CRITICAL(&s_stats_lock);
    s_stats = *st;
    portEXIT_CRITICAL(&s_stats_lock);
    s_touched = st->touched;
}

#if CONFIG_LAMP_OUTPUT_PWM
/* One frame of the interference scan. Returns false when no scan is running. */
static bool scan_frame(float raw)
{
    static int k = -1; /* frame within the cycle, -1 when not scanning */
    static double ref_sum;
    static uint32_t ref_n;
    static float ref;
    static int worst, band[SCAN_BANDS];

    if (esp_timer_get_time() >= s_scan_until_us) {
        if (k >= 0) {
            light_output_set_test_duty(-1.0f);
            k = -1;
            s_scanning = false;
            s_scan_pm = -1;
            s_noise_pm = 0.0f;
            printf("SCAN stopped\n");
        }
        return false;
    }
    if (k < 0) {
        k = 0;
        s_scanning = true;
        s_scan_pm = -1;
    }
    if (k == 0) {
        ref_sum = 0.0;
        ref_n = 0;
        worst = 0;
        memset(band, 0, sizeof(band));
        light_output_set_test_duty(0.0f);
    }
    if (k < SCAN_OFF_FRAMES) {
        if (k >= SCAN_SETTLE_FRAMES) {
            ref_sum += raw;
            ref_n++;
        }
        if (k == SCAN_OFF_FRAMES - 1) {
            ref = ref_sum / ref_n;
        }
        k++;
        return true;
    }
    int r = k - SCAN_OFF_FRAMES;
    if (r <= SCAN_RAMP_FRAMES) {
        light_output_set_test_duty((float)r / SCAN_RAMP_FRAMES);
    }
    int m = r - SCAN_LAG_FRAMES; /* the ramp step this reading reflects */
    if (m >= 0) {
        int d = abs((int)lroundf((raw - ref) * 1000.0f / ref));
        int b = m * SCAN_BANDS / (SCAN_RAMP_FRAMES + 1);
        if (d > band[b]) {
            band[b] = d;
        }
        if (d > worst) {
            worst = d;
        }
    }
    if (m == SCAN_RAMP_FRAMES) {
        s_scan_pm = worst;
        printf("SCAN worst=%d ref=%.0fns by duty (per mille, 0-100%% in 10%% bands):", worst, ref * 1e9f / CAP_RES_HZ);
        for (int i = 0; i < SCAN_BANDS; i++) {
            printf(" %d", band[i]);
        }
        printf("\n");
        k = 0; /* next cycle */
        return true;
    }
    k++;
    return true;
}
#else
static bool scan_frame(float raw)
{
    return false;
}
#endif

static void touch_task(void *arg)
{
    const float ns_per_tick = 1e9f / CAP_RES_HZ;
    uint32_t frame_sum[2] = { 0 }, frame_n[2] = { 0 };
    int idx = 0;
    float baseline = 0.0f;
    double cal_sum = 0.0;
    uint32_t cal_n = 0, elapsed_ms = 0, touched_ms = 0, below_ms = 0, no_signal_ms = 0;
    touch_stats_t st = { 0 };

    TickType_t wake = xTaskGetTickCount();
    while (true) {
        vTaskDelayUntil(&wake, pdMS_TO_TICKS(FRAME_MS));

        portENTER_CRITICAL(&s_isr_lock);
        frame_sum[idx] = s_acc;
        frame_n[idx] = s_count;
        s_acc = 0;
        s_count = 0;
        portEXIT_CRITICAL(&s_isr_lock);
        uint32_t this_sum = frame_sum[idx], this_n = frame_n[idx];
        idx ^= 1;

        /* 20 ms window: one full 50 Hz mains cycle. */
        uint32_t n = frame_n[0] + frame_n[1];
        st.edges = n;
        if (n == 0) {
            no_signal_ms += FRAME_MS;
            if (no_signal_ms % 5000 == NO_SIGNAL_WARN_MS) {
                ESP_LOGW(TAG, "No edges on the receive pin: check the resistor and wiring");
            }
            st.touched = false;
            touched_ms = 0;
            publish(&st);
            continue;
        }
        no_signal_ms = 0;
        float raw = (float)(frame_sum[0] + frame_sum[1]) / n;
        st.raw_ns = raw * ns_per_tick;

        if (!st.calibrated) {
            /* Assumes nobody is touching the pad in the first few hundred ms after power-up. */
            elapsed_ms += FRAME_MS;
            if (elapsed_ms > CAL_SKIP_MS) {
                cal_sum += raw;
                cal_n++;
            }
            if (elapsed_ms >= CAL_MS && cal_n > 0) {
                baseline = cal_sum / cal_n;
                st.calibrated = true;
                ESP_LOGI(TAG, "Calibrated: baseline %.0f ns", baseline * ns_per_tick);
            }
            st.baseline_ns = baseline * ns_per_tick;
            publish(&st);
            continue;
        }

        if (scan_frame(raw) || light_output_get_test_duty() >= 0.0f) {
            /* The PWM is being swept or held at a test duty: ignore touches, and leave the baseline alone. */
            st.touched = false;
            touched_ms = 0;
            below_ms = 0;
            st.delta_permille = 0;
            publish(&st);
            continue;
        }

        int delta_pm = (int)((raw - baseline) * 1000.0f / baseline);
        if (!st.touched) {
            if (delta_pm >= CONFIG_LAMP_TOUCH_ON_PERMILLE) {
                st.touched = true;
                touched_ms = 0;
                below_ms = 0;
            }
        } else if (delta_pm < CONFIG_LAMP_TOUCH_OFF_PERMILLE) {
            /* Ignore short dips (see LAMP_TOUCH_RELEASE_MS). */
            below_ms += FRAME_MS;
            if (below_ms >= CONFIG_LAMP_TOUCH_RELEASE_MS) {
                st.touched = false;
            }
        } else {
            below_ms = 0;
            touched_ms += FRAME_MS;
            if (touched_ms >= CONFIG_LAMP_TOUCH_STUCK_MS) {
                ESP_LOGW(TAG, "Touched for %d ms: recalibrating", CONFIG_LAMP_TOUCH_STUCK_MS);
                baseline = raw;
                st.touched = false;
            }
        }

        if (!st.touched) {
            baseline += (raw - baseline) * (raw < baseline ? TRACK_DOWN_K : TRACK_UP_K);
            s_noise_pm = fmaxf((float)abs(delta_pm), s_noise_pm * NOISE_DECAY);
        }

        st.baseline_ns = baseline * ns_per_tick;
        st.delta_permille = delta_pm;
        publish(&st);

        if (esp_timer_get_time() < s_stream_until_us) {
            /* time, this frame's delta alone, the 20 ms window's delta, edges this frame, state, light output */
            int frame_pm = this_n ? (int)(((float)this_sum / this_n - baseline) * 1000.0f / baseline) : 0;
            printf("TR %lu %d %d %lu %d %lu\n", (unsigned long)(esp_timer_get_time() / 1000), frame_pm, delta_pm,
                   (unsigned long)this_n, st.touched, (unsigned long)light_output_get_raw());
        }
    }
}

esp_err_t touch_init(void)
{
    const uint32_t period_ticks = GEN_RES_HZ / CONFIG_LAMP_TOUCH_DRIVE_HZ;
    s_half_ticks = CAP_RES_HZ / CONFIG_LAMP_TOUCH_DRIVE_HZ / 2;

    /* Capture timer first: it fixes the group clock (160 MHz / 2) that the drive timer then divides. */
    mcpwm_cap_timer_handle_t cap_timer;
    mcpwm_capture_timer_config_t cap_timer_cfg = {
        .group_id = 0,
        .clk_src = MCPWM_CAPTURE_CLK_SRC_DEFAULT,
        .resolution_hz = CAP_RES_HZ,
    };
    ESP_RETURN_ON_ERROR(mcpwm_new_capture_timer(&cap_timer_cfg, &cap_timer), TAG, "capture timer");

    mcpwm_timer_handle_t timer;
    mcpwm_timer_config_t timer_cfg = {
        .group_id = 0,
        .clk_src = MCPWM_TIMER_CLK_SRC_DEFAULT,
        .resolution_hz = GEN_RES_HZ,
        .count_mode = MCPWM_TIMER_COUNT_MODE_UP,
        .period_ticks = period_ticks,
    };
    ESP_RETURN_ON_ERROR(mcpwm_new_timer(&timer_cfg, &timer), TAG, "timer");

    mcpwm_oper_handle_t oper;
    mcpwm_operator_config_t oper_cfg = { .group_id = 0 };
    ESP_RETURN_ON_ERROR(mcpwm_new_operator(&oper_cfg, &oper), TAG, "operator");
    ESP_RETURN_ON_ERROR(mcpwm_operator_connect_timer(oper, timer), TAG, "connect");

    mcpwm_cmpr_handle_t cmpr;
    mcpwm_comparator_config_t cmpr_cfg = { .flags.update_cmp_on_tez = true };
    ESP_RETURN_ON_ERROR(mcpwm_new_comparator(oper, &cmpr_cfg, &cmpr), TAG, "comparator");
    ESP_RETURN_ON_ERROR(mcpwm_comparator_set_compare_value(cmpr, period_ticks / 2), TAG, "compare");

    /* SEND: high at the start of each period, low at half period. */
    mcpwm_gen_handle_t gen;
    mcpwm_generator_config_t gen_cfg = { .gen_gpio_num = CONFIG_LAMP_TOUCH_SEND_GPIO };
    ESP_RETURN_ON_ERROR(mcpwm_new_generator(oper, &gen_cfg, &gen), TAG, "generator");
    ESP_RETURN_ON_ERROR(mcpwm_generator_set_action_on_timer_event(gen,
                        MCPWM_GEN_TIMER_EVENT_ACTION(MCPWM_TIMER_DIRECTION_UP, MCPWM_TIMER_EVENT_EMPTY, MCPWM_GEN_ACTION_HIGH)),
                        TAG, "gen action");
    ESP_RETURN_ON_ERROR(mcpwm_generator_set_action_on_compare_event(gen,
                        MCPWM_GEN_COMPARE_EVENT_ACTION(MCPWM_TIMER_DIRECTION_UP, cmpr, MCPWM_GEN_ACTION_LOW)),
                        TAG, "gen action");

    /* Reset the capture timer at the start of every drive period. */
    mcpwm_sync_handle_t sync;
    mcpwm_timer_sync_src_config_t sync_cfg = { .timer_event = MCPWM_TIMER_EVENT_EMPTY };
    ESP_RETURN_ON_ERROR(mcpwm_new_timer_sync_src(timer, &sync_cfg, &sync), TAG, "sync");
    mcpwm_capture_timer_sync_phase_config_t phase = {
        .sync_src = sync,
        .count_value = 0,
        .direction = MCPWM_TIMER_DIRECTION_UP,
    };
    ESP_RETURN_ON_ERROR(mcpwm_capture_timer_set_phase_on_sync(cap_timer, &phase), TAG, "sync phase");

    mcpwm_cap_channel_handle_t chan;
    mcpwm_capture_channel_config_t chan_cfg = {
        .gpio_num = CONFIG_LAMP_TOUCH_RECV_GPIO,
        .prescale = 1,
        .flags.pos_edge = true,
        .flags.neg_edge = true,
    };
    ESP_RETURN_ON_ERROR(mcpwm_new_capture_channel(cap_timer, &chan_cfg, &chan), TAG, "capture channel");
    mcpwm_capture_event_callbacks_t cbs = { .on_cap = on_capture };
    ESP_RETURN_ON_ERROR(mcpwm_capture_channel_register_event_callbacks(chan, &cbs, NULL), TAG, "callbacks");
    ESP_RETURN_ON_ERROR(mcpwm_capture_channel_enable(chan), TAG, "channel enable");

    ESP_RETURN_ON_ERROR(mcpwm_capture_timer_enable(cap_timer), TAG, "capture enable");
    ESP_RETURN_ON_ERROR(mcpwm_capture_timer_start(cap_timer), TAG, "capture start");
    ESP_RETURN_ON_ERROR(mcpwm_timer_enable(timer), TAG, "timer enable");
    ESP_RETURN_ON_ERROR(mcpwm_timer_start_stop(timer, MCPWM_TIMER_START_NO_STOP), TAG, "timer start");

    /* The driver doesn't expose the hardware timer index; it's the running timer with our period. */
    for (int i = 0; i < SOC_MCPWM_TIMERS_PER_GROUP; i++) {
        if (MCPWM0.timer[i].timer_cfg1.timer_mod != 0 &&
            HAL_FORCE_READ_U32_REG_FIELD(MCPWM0.timer[i].timer_cfg0, timer_period) == period_ticks - 1) {
            s_drive_timer_id = i;
            break;
        }
    }
    if (s_drive_timer_id < 0) {
        ESP_LOGW(TAG, "Drive timer not found: PWM edges won't be aligned to the touch measurement");
    }

    BaseType_t ok = xTaskCreate(touch_task, "touch", 3072, NULL, 5, NULL);
    ESP_RETURN_ON_FALSE(ok == pdPASS, ESP_ERR_NO_MEM, TAG, "task create failed");
    ESP_LOGI(TAG, "Touch sensing: send GPIO%d, receive GPIO%d, %d Hz", CONFIG_LAMP_TOUCH_SEND_GPIO,
             CONFIG_LAMP_TOUCH_RECV_GPIO, CONFIG_LAMP_TOUCH_DRIVE_HZ);
    return ESP_OK;
}

bool touch_is_touched(void)
{
    return s_touched;
}

int32_t touch_drive_time_us(void)
{
    if (s_drive_timer_id < 0) {
        return -1;
    }
    /* The timer counts at GEN_RES_HZ (1 MHz), so its value is already in microseconds. */
    return mcpwm_ll_timer_get_count_value(&MCPWM0, s_drive_timer_id);
}

void touch_stream(uint32_t duration_ms)
{
    s_stream_until_us = esp_timer_get_time() + (int64_t)duration_ms * 1000;
}

int touch_interference_permille(bool *scanning)
{
    bool scan = s_scanning;
    if (scanning) {
        *scanning = scan;
    }
    return scan ? s_scan_pm : (int)lroundf(s_noise_pm);
}

void touch_scan(uint32_t duration_ms)
{
    s_scan_until_us = duration_ms ? esp_timer_get_time() + (int64_t)duration_ms * 1000 : 0;
}

void touch_get_stats(touch_stats_t *out)
{
    portENTER_CRITICAL(&s_stats_lock);
    *out = s_stats;
    portEXIT_CRITICAL(&s_stats_lock);
}

#endif /* CONFIG_LAMP_INPUT_TOUCH */
