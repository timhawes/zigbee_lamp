#include "light_output.h"

#include <inttypes.h>
#include <math.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "sdkconfig.h"
#if CONFIG_LAMP_OUTPUT_PWM
#include "driver/ledc.h"
#include "soc/ledc_struct.h"
#include "hal/ledc_ll.h"
#if CONFIG_LAMP_INPUT_TOUCH
#include "touch.h"
#endif
#else
#include "led_strip.h"
#endif

#define RENDER_PERIOD_MS 10
/* First-order smoothing in the perceptual (level) domain; ~120 ms time constant. */
#define SMOOTHING_ALPHA  (RENDER_PERIOD_MS / 120.0f)
#define SNAP_DISTANCE    (0.5f / 254.0f) /* within half a level of the target: done */
#define GAMMA            2.2f

static const char *TAG = "light_output";

static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static float s_target;          /* perceptual level 0..1 (0 = off) */
static light_effect_t s_effect;
static int64_t s_identify_until_us;
static volatile uint32_t s_raw; /* last value written to the output (see light_output_get_raw) */
static float s_test_duty = -1.0f; /* PWM duty override for the interference scan, 0..1; < 0 = off */

static float level_to_perceptual(bool on, uint8_t level)
{
    if (!on || level == 0) {
        return 0.0f;
    }
    if (level > 254) {
        level = 254;
    }
    return level / 254.0f;
}

/* ---- Backends. Channel values are linear brightness 0..1. min_one keeps a lit output from rounding to
 * zero. floor_scale (0..1) scales the PWM duty floor (LAMP_PWM_MIN_DUTY_PERMILLE). Each returns the raw value it
 * would write. ---- */

#if CONFIG_LAMP_OUTPUT_PWM

#define PWM_MODE    LEDC_LOW_SPEED_MODE
#define PWM_TIMER   LEDC_TIMER_0
#define PWM_CHANNEL LEDC_CHANNEL_0
#define PWM_CLK_HZ  80000000 /* LEDC_USE_PLL_DIV_CLK */

static uint32_t s_duty_full; /* duty count for 100 % */
static uint32_t s_duty_min, s_duty_max;

/* ---- Keeping the PWM edges away from the touch measurement ----
 * The LED driver's switching couples into the touch pad. Measured on the lamp (untouched, 2 kHz PWM, 1 kHz
 * drive, 2.2 MOhm), depending on where the PWM edges fall relative to the touch drive edges:
 *  - a rising edge anywhere except about 25-75 us after a drive edge shifts the reading by 15-80 %;
 *  - a falling edge about 5-25 us after a drive edge (while the pad is charging) shifts it by 15 %+;
 *  - on-times leaving a 12-39 us gap shift it by 2-3 % wherever the edges are.
 * A finger is 6-60 %. With PWM at twice the drive frequency, every PWM period contains one drive edge at a
 * fixed phase, so the edges can be placed (times are after a drive edge):
 *  - normal on-times: rise at 60 us;
 *  - long on-times: fall at 490 us, 10 us before the next drive edge;
 *  - gaps of 12-43 us: rounded to the nearer end (only levels ~245-251 are affected);
 *  - gaps under 12 us: fall just before the LEDC counter wraps (~29 us).
 * No pulse may cross the LEDC counter wrap: the LEDC loads a new duty at the wrap, and a shorter duty loaded in
 * the middle of a wrapping pulse leaves the output high for almost a whole period (a visible flash when
 * dimming down). So at start-up the LEDC counter is reset 30 us after a drive edge, which puts all the
 * pulses above inside one counter cycle. This keeps the untouched offset within about 1 %. */
#define PWM_ALIGN (CONFIG_LAMP_INPUT_TOUCH && CONFIG_LAMP_PWM_FREQ_HZ == 2 * CONFIG_LAMP_TOUCH_DRIVE_HZ)
/* At other multiples of twice the drive frequency (e.g. 20 kHz) every PWM period has the same phase relative to
 * the drive edges, so the edges can still be placed. Mapped on the lamp at 20 kHz (untouched offset against duty
 * and phase, 12 us baseline edge delay): an edge in roughly the first 15 us after a drive edge, while the pad is
 * charging, shifts the reading by up to 65 per mille; edges from about 20 us after a drive edge until just before
 * the next one shift it by under 10. So both edges are kept in [SAFE_FROM_US, PERIOD_US - SAFE_END_US]:
 *  - on-times that fit: rise at SAFE_FROM_US;
 *  - longer ones: fall at SAFE_FROM_US, so the off-gap lies in the window instead.
 * The LEDC counter starts at SAFE_FROM_US, which keeps every pulse inside one counter cycle. Without this the
 * phase was random at each boot and the worst offset 30-49 per mille; with it, about 10. */
#define PWM_LOCK (CONFIG_LAMP_INPUT_TOUCH && CONFIG_LAMP_PWM_FREQ_HZ % (2 * CONFIG_LAMP_TOUCH_DRIVE_HZ) == 0)
#if PWM_LOCK
#define PERIOD_US         (1000000 / CONFIG_LAMP_PWM_FREQ_HZ)
#define ORIGIN_TOL_US     3
#define REALIGN_MS        10000
#if PWM_ALIGN
#define ORIGIN_US         30 /* LEDC counter zero, after a drive edge */
#else
#define SAFE_FROM_US      20
#define SAFE_END_US       2
#define ORIGIN_US         SAFE_FROM_US
#endif

static int32_t s_origin_us = ORIGIN_US;
static int32_t s_origin_cnt = -1; /* time from a drive edge to LEDC counter zero, in LEDC counts */

static uint32_t us_to_cnt(uint32_t us)
{
    return (uint64_t)us * s_duty_full / PERIOD_US;
}

/* Read the LEDC counter and the touch drive timer together; returns the time from a drive edge to LEDC
 * counter zero, in LEDC counts, or -1. */
static int32_t measure_origin(void)
{
    static portMUX_TYPE mux = portMUX_INITIALIZER_UNLOCKED;
    int32_t prev = -1;
    for (int attempt = 0; attempt < 5; attempt++) {
        portENTER_CRITICAL(&mux);
        uint32_t ledc = LEDC.timer_group[0].timer[PWM_TIMER].value.timer_cnt;
        int32_t drive_us = touch_drive_time_us();
        portEXIT_CRITICAL(&mux);
        if (drive_us < 0) {
            return -1;
        }
        int32_t cnt = (int32_t)((us_to_cnt(drive_us % PERIOD_US) + s_duty_full - ledc) % s_duty_full);
        /* Two reads in a row within 2 us of each other: accept. */
        int32_t diff = abs(cnt - prev);
        if (prev >= 0 && (diff < (int32_t)us_to_cnt(2) || diff > (int32_t)(s_duty_full - us_to_cnt(2)))) {
            return cnt;
        }
        prev = cnt;
    }
    return prev;
}

/* Reset the LEDC counter s_origin_us after a drive edge. Interrupts are off for up to one PWM period. */
static bool align_origin(void)
{
    static portMUX_TYPE mux = portMUX_INITIALIZER_UNLOCKED;
    if (touch_drive_time_us() < 0) {
        return false;
    }
    for (int attempt = 0; attempt < 5; attempt++) {
        portENTER_CRITICAL(&mux);
        /* Spin until the drive time (mod one PWM period) steps past s_origin_us, including across the wrap. */
        int32_t prev = touch_drive_time_us() % PERIOD_US;
        while (true) {
            int32_t now = touch_drive_time_us() % PERIOD_US;
            bool crossed = now >= prev ? (prev < s_origin_us && s_origin_us <= now)
                                       : (s_origin_us > prev || s_origin_us <= now);
            prev = now;
            if (crossed) {
                break;
            }
        }
        ledc_ll_timer_rst(&LEDC, PWM_MODE, PWM_TIMER);
        portEXIT_CRITICAL(&mux);
        s_origin_cnt = measure_origin();
        int32_t err = abs(s_origin_cnt - (int32_t)us_to_cnt(s_origin_us));
        if (err > (int32_t)(s_duty_full / 2)) {
            err = s_duty_full - err; /* phase 0: the measurement can land just below the wrap */
        }
        if (s_origin_cnt >= 0 && err <= (int32_t)us_to_cnt(ORIGIN_TOL_US)) {
            return true;
        }
    }
    ESP_LOGW(TAG, "Couldn't align the PWM to the touch drive (origin %" PRId32 " counts)", s_origin_cnt);
    return false;
}

#if !PWM_ALIGN
/* LEDC hpoint for a duty (see SAFE_FROM_US above). Relative to the measured origin; "pwm phase" moves the origin
 * for experiments, and pulses may then cross the counter wrap (harmless at this frequency: one period). */
static uint32_t hpoint_for(uint32_t duty)
{
    if (s_origin_cnt < 0 || duty == 0 || duty >= s_duty_full) {
        return 0;
    }
    /* Rise at SAFE_FROM_US; for long on-times fall there instead, i.e. rise one off-gap later. */
    int32_t rise = (int32_t)us_to_cnt(SAFE_FROM_US);
    if (duty > us_to_cnt(PERIOD_US - SAFE_END_US - SAFE_FROM_US)) {
        rise += (int32_t)(s_duty_full - duty);
    }
    int32_t full = (int32_t)s_duty_full, slack = (int32_t)us_to_cnt(ORIGIN_TOL_US);
    int32_t hpoint = ((rise - s_origin_cnt) % full + full) % full;
    /* Absorb the origin's measurement error rather than wrapping by a few counts. */
    if (hpoint > full - slack) {
        hpoint = 0;
    } else if (hpoint + (int32_t)duty > full && hpoint + (int32_t)duty - full <= slack) {
        hpoint = full - duty;
    }
    return hpoint;
}
#endif
#endif /* PWM_LOCK */

#if PWM_ALIGN
#define RISE_US           60
#define FALL_LATEST_US    490
#define GAP_BAND_MIN_US   12 /* off-gaps strictly between these are avoided */
#define GAP_BAND_MAX_US   43
#define WRAP_MARGIN_US    1  /* short-gap pulses end this long before the counter wraps */

/* Round on-times whose off-gap falls in the band to the nearer end; drop gaps too short to place. */
static uint32_t adjust_duty(uint32_t duty)
{
    if (duty == 0 || duty >= s_duty_full) {
        return duty;
    }
    uint32_t gap = s_duty_full - duty;
    uint32_t lo = us_to_cnt(GAP_BAND_MIN_US), hi = us_to_cnt(GAP_BAND_MAX_US);
    if (gap > lo && gap < hi) {
        gap = (gap - lo < hi - gap) ? lo : hi;
    }
    if (gap <= us_to_cnt(WRAP_MARGIN_US)) {
        gap = 0; /* full on */
    }
    return s_duty_full - gap;
}

/* LEDC hpoint (counter value at the rising edge) for a duty from adjust_duty(). */
static uint32_t hpoint_for(uint32_t duty)
{
    if (s_origin_cnt < 0 || duty == 0 || duty >= s_duty_full) {
        return 0;
    }
    uint32_t gap = s_duty_full - duty;
    int32_t hpoint;
    if (gap <= us_to_cnt(GAP_BAND_MIN_US)) {
        hpoint = (int32_t)(s_duty_full - us_to_cnt(WRAP_MARGIN_US) - duty);
    } else if (duty <= us_to_cnt(FALL_LATEST_US - RISE_US)) {
        hpoint = (int32_t)us_to_cnt(RISE_US) - s_origin_cnt;
    } else {
        hpoint = (int32_t)(us_to_cnt(FALL_LATEST_US) - duty) - s_origin_cnt;
    }
    /* Never let a pulse cross the counter wrap (see above). */
    if (hpoint < 0) {
        hpoint = 0;
    }
    if (hpoint + duty > s_duty_full) {
        hpoint = s_duty_full - duty;
    }
    return hpoint;
}
#endif

#define LEDC_FRAC_BITS 8 /* the LEDC clock divider has 8 fractional bits */

/* Highest resolution whose clock divider is exact, so the PWM runs at exactly the configured frequency.
 * An inexact divider (e.g. 15 bits at 2 kHz gives 2003.2 Hz) beats against the touch drive: the PWM couples
 * into the pad, and the reading swings at the difference frequency (3.2 Hz), which made a firm touch toggle
 * the lamp repeatedly. Falls back to the highest resolution if no exact divider gives at least 10 bits. */
static uint32_t pick_duty_resolution(void)
{
    uint32_t max_bits = ledc_find_suitable_duty_resolution(PWM_CLK_HZ, CONFIG_LAMP_PWM_FREQ_HZ);
    if (max_bits >= LEDC_TIMER_BIT_MAX) {
        max_bits = LEDC_TIMER_BIT_MAX - 1;
    }
    const uint64_t scaled_clk = (uint64_t)PWM_CLK_HZ << LEDC_FRAC_BITS;
    for (uint32_t bits = max_bits; bits >= 10; bits--) {
        if (scaled_clk % ((uint64_t)CONFIG_LAMP_PWM_FREQ_HZ << bits) == 0) {
            return bits;
        }
    }
    ESP_LOGW(TAG, "No exact LEDC divider for %d Hz: the PWM frequency will be slightly off", CONFIG_LAMP_PWM_FREQ_HZ);
    return max_bits;
}

static esp_err_t backend_init(void)
{
    uint32_t bits = pick_duty_resolution();
    ESP_RETURN_ON_FALSE(bits >= 8, ESP_ERR_INVALID_ARG, TAG, "PWM frequency too high for 8-bit resolution");
#if CONFIG_LAMP_INPUT_TOUCH && !PWM_LOCK
    ESP_LOGW(TAG, "PWM %d Hz isn't a multiple of twice the touch drive %d Hz: the PWM can't be phase-locked, "
             "expect false touches", CONFIG_LAMP_PWM_FREQ_HZ, CONFIG_LAMP_TOUCH_DRIVE_HZ);
#endif
#if CONFIG_LAMP_PWM_POWERON_DELAY_MS > 0
    esp_reset_reason_t reason = esp_reset_reason();
    if (reason == ESP_RST_POWERON || reason == ESP_RST_BROWNOUT) {
        /* The pin is still floating; the driver's pull-down keeps the LED off while the supply settles. */
        ESP_LOGI(TAG, "Power-on: PWM pin left floating for %d ms", CONFIG_LAMP_PWM_POWERON_DELAY_MS);
        vTaskDelay(pdMS_TO_TICKS(CONFIG_LAMP_PWM_POWERON_DELAY_MS));
    }
#endif
    s_duty_full = 1u << bits;
    s_duty_min = (uint64_t)s_duty_full * CONFIG_LAMP_PWM_MIN_DUTY_PERMILLE / 1000;
    s_duty_max = (uint64_t)s_duty_full * CONFIG_LAMP_PWM_MAX_DUTY_PERMILLE / 1000;

    ledc_timer_config_t timer = {
        .speed_mode = PWM_MODE,
        .timer_num = PWM_TIMER,
        .duty_resolution = bits,
        .freq_hz = CONFIG_LAMP_PWM_FREQ_HZ,
        .clk_cfg = LEDC_USE_PLL_DIV_CLK,
    };
    ESP_RETURN_ON_ERROR(ledc_timer_config(&timer), TAG, "LEDC timer");
    ledc_channel_config_t chan = {
        .gpio_num = CONFIG_LAMP_PWM_GPIO,
        .speed_mode = PWM_MODE,
        .channel = PWM_CHANNEL,
        .timer_sel = PWM_TIMER,
        .duty = 0,
#if CONFIG_LAMP_PWM_INVERT
        .flags.output_invert = 1,
#endif
    };
    ESP_RETURN_ON_ERROR(ledc_channel_config(&chan), TAG, "LEDC channel");
    ESP_LOGI(TAG, "PWM on GPIO%d: %d Hz, %" PRIu32 "-bit", CONFIG_LAMP_PWM_GPIO, CONFIG_LAMP_PWM_FREQ_HZ, bits);
#if PWM_LOCK
    /* Touch sensing starts first (button_init), so its drive timer is already running. */
    if (align_origin()) {
        ESP_LOGI(TAG, "PWM aligned: LEDC counter starts %" PRIu32 " us after a touch drive edge",
                 (uint32_t)((uint64_t)s_origin_cnt * PERIOD_US / s_duty_full));
    }
#endif
    return ESP_OK;
}

static uint32_t backend_map(float r, float g, float b, bool min_one, float floor_scale)
{
    float v = fmaxf(r, fmaxf(g, b)); /* single channel: effects render at their brightest component */
    if (v <= 0.0f) {
        return 0;
    }
    uint32_t duty = lroundf(s_duty_min * floor_scale + (s_duty_max - s_duty_min) * v);
    if (min_one && duty == 0) {
        duty = 1;
    }
#if PWM_ALIGN
    duty = adjust_duty(duty);
#endif
    return duty;
}

/* Duty for a fraction of full scale, bypassing gamma, smoothing and the duty floor (interference scan). */
static uint32_t backend_test_map(float f)
{
    uint32_t duty = lroundf(fminf(fmaxf(f, 0.0f), 1.0f) * s_duty_full);
#if PWM_ALIGN
    duty = adjust_duty(duty);
#endif
    return duty;
}

static void backend_write(uint32_t duty)
{
#if PWM_LOCK
    ledc_set_duty_with_hpoint(PWM_MODE, PWM_CHANNEL, duty, hpoint_for(duty));
#else
    ledc_set_duty(PWM_MODE, PWM_CHANNEL, duty);
#endif
    ledc_update_duty(PWM_MODE, PWM_CHANNEL);
}

/* Called every render period. Both timers run from the same PLL, so the alignment shouldn't drift; this
 * checks it now and then and corrects it if it has. */
static void backend_poll(uint32_t raw)
{
#if PWM_LOCK
    static uint32_t since_ms;
    if ((since_ms += RENDER_PERIOD_MS) < REALIGN_MS) {
        return;
    }
    since_ms = 0;
    int32_t cnt = measure_origin();
    if (cnt < 0) {
        return;
    }
    int32_t diff = abs(cnt - s_origin_cnt);
    if (diff > (int32_t)us_to_cnt(ORIGIN_TOL_US) && diff < (int32_t)(s_duty_full - us_to_cnt(ORIGIN_TOL_US))) {
        ESP_LOGW(TAG, "PWM alignment drifted by %" PRId32 " counts: realigning", diff);
        align_origin();
        backend_write(raw);
    }
#endif
}

#else /* WS2812 */

static led_strip_handle_t s_strip;

static esp_err_t backend_init(void)
{
    led_strip_config_t strip_config = {
        .strip_gpio_num = CONFIG_LAMP_LED_GPIO,
        .max_leds = 1,
        .led_model = LED_MODEL_WS2812,
        .color_component_format = LED_STRIP_COLOR_COMPONENT_FMT_GRB,
    };
    led_strip_rmt_config_t rmt_config = {
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .resolution_hz = 10 * 1000 * 1000,
    };
    ESP_RETURN_ON_ERROR(led_strip_new_rmt_device(&strip_config, &rmt_config, &s_strip), TAG, "LED init failed");
    led_strip_clear(s_strip);
    return ESP_OK;
}

/* Packed as 0xRRGGBB. */
static uint32_t backend_map(float r, float g, float b, bool min_one, float floor_scale)
{
    const float max = CONFIG_LAMP_LED_MAX_BRIGHTNESS;
    uint32_t ri = lroundf(r * max), gi = lroundf(g * max), bi = lroundf(b * max);
    if (min_one) {
        if (r > 0.0f && ri == 0) ri = 1;
        if (g > 0.0f && gi == 0) gi = 1;
        if (b > 0.0f && bi == 0) bi = 1;
    }
    return (ri << 16) | (gi << 8) | bi;
}

static void backend_write(uint32_t rgb)
{
    led_strip_set_pixel(s_strip, 0, rgb >> 16, (rgb >> 8) & 0xFF, rgb & 0xFF);
    led_strip_refresh(s_strip);
}

static void backend_poll(uint32_t raw)
{
}

#endif

static void render_task(void *arg)
{
    float current = 0.0f;
    float prev_target = 0.0f;
    float off_from = 0.0f; /* perceptual level when the last fade to off began */
    uint32_t last_raw = UINT32_MAX;
    TickType_t wake = xTaskGetTickCount();

    while (true) {
        vTaskDelayUntil(&wake, pdMS_TO_TICKS(RENDER_PERIOD_MS));

        portENTER_CRITICAL(&s_lock);
        float target = s_target;
        light_effect_t effect = s_effect;
        int64_t identify_until = s_identify_until_us;
        float test_duty = s_test_duty;
        portEXIT_CRITICAL(&s_lock);

        int64_t now_us = esp_timer_get_time();
        if (effect == LIGHT_EFFECT_NONE && now_us < identify_until) {
            effect = LIGHT_EFFECT_IDENTIFY;
        }

        current += (target - current) * SMOOTHING_ALPHA;
        if (fabsf(target - current) < SNAP_DISTANCE) {
            current = target;
        }

        /* The minimum-brightness clamp only applies while on, so fading to off ends cleanly. While fading to
         * off, the PWM duty floor shrinks in step with the fade; otherwise the tail of the fade would sit at the
         * floor (the minimum brightness) for about half a second before going dark. */
        if (target <= 0.0f && prev_target > 0.0f) {
            off_from = current;
        }
        prev_target = target;
        bool min_one = target > 0.0f;
        float floor_scale = min_one ? 1.0f : (off_from > 0.0f ? fminf(current / off_from, 1.0f) : 0.0f);
        float lin = powf(current, GAMMA);
        float r = lin, g = lin, b = lin;
        uint32_t ms = (uint32_t)(now_us / 1000);
        switch (effect) {
        case LIGHT_EFFECT_PAIRING: {
            /* 2 s breathing period, blue */
            float phase = (ms % 2000) / 2000.0f;
            float v = 0.5f - 0.5f * cosf(2.0f * (float)M_PI * phase);
            r = g = 0.0f;
            b = 0.02f + 0.3f * v;
            min_one = true;
            break;
        }
        case LIGHT_EFFECT_IDENTIFY: {
            float v = ((ms / 500) % 2) ? 0.0f : 1.0f;
            r = g = b = v;
            break;
        }
        case LIGHT_EFFECT_RESET: {
            /* 2 Hz: stays under 3 flashes per second (photosensitivity guidance). */
            float v = ((ms / 250) % 2) ? 0.0f : 0.5f;
            r = v;
            g = b = 0.0f;
            break;
        }
        default:
            break;
        }

        /* Only write to the output when the value changes. */
        uint32_t raw = backend_map(r, g, b, min_one, floor_scale);
#if CONFIG_LAMP_OUTPUT_PWM
        if (test_duty >= 0.0f) {
            raw = backend_test_map(test_duty);
        }
#else
        (void)test_duty;
#endif
        if (raw != last_raw) {
            backend_write(raw);
            last_raw = raw;
            s_raw = raw;
        }
        backend_poll(last_raw);
    }
}

esp_err_t light_output_init(void)
{
    ESP_RETURN_ON_ERROR(backend_init(), TAG, "output init failed");

    BaseType_t ok = xTaskCreate(render_task, "light_out", 3072, NULL, 6, NULL);
    ESP_RETURN_ON_FALSE(ok == pdPASS, ESP_ERR_NO_MEM, TAG, "render task create failed");
    return ESP_OK;
}

void light_output_set(bool on, uint8_t level)
{
    float b = level_to_perceptual(on, level);
    portENTER_CRITICAL(&s_lock);
    s_target = b;
    portEXIT_CRITICAL(&s_lock);
}

void light_output_set_effect(light_effect_t effect)
{
    portENTER_CRITICAL(&s_lock);
    s_effect = effect;
    portEXIT_CRITICAL(&s_lock);
}

void light_output_identify_for(uint32_t duration_ms)
{
    portENTER_CRITICAL(&s_lock);
    s_identify_until_us = duration_ms ? esp_timer_get_time() + (int64_t)duration_ms * 1000 : 0;
    portEXIT_CRITICAL(&s_lock);
}

esp_err_t light_output_set_pwm_phase(uint32_t us)
{
#if CONFIG_LAMP_OUTPUT_PWM && PWM_LOCK
    if (us >= PERIOD_US) {
        return ESP_ERR_INVALID_ARG;
    }
    s_origin_us = us;
    return align_origin() ? ESP_OK : ESP_FAIL;
#else
    return ESP_ERR_NOT_SUPPORTED;
#endif
}

float light_output_get_test_duty(void)
{
    portENTER_CRITICAL(&s_lock);
    float duty = s_test_duty;
    portEXIT_CRITICAL(&s_lock);
    return duty;
}

void light_output_set_test_duty(float duty)
{
    portENTER_CRITICAL(&s_lock);
    s_test_duty = duty;
    portEXIT_CRITICAL(&s_lock);
}

uint32_t light_output_get_raw(void)
{
    return s_raw;
}
