#include "amp.h"

#include "driver/gptimer.h"
#include "driver/ledc.h"
#include "esp_attr.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "hal/ledc_ll.h"
#include "soc/ledc_struct.h"

static const char *TAG = "amp";

#define BITS  10
#define TOP   (1 << BITS)                /* 1024: 80 MHz / 1024 = 78.1 kHz */
#define MID   (TOP / 2)
#define RING  4096                       /* 256 ms */
#define CH    LEDC_CHANNEL_0
#define MODE  LEDC_LOW_SPEED_MODE

static gptimer_handle_t s_timer;
static uint16_t s_ring[RING];
static volatile uint32_t s_head, s_tail;   /* written by amp_write / the interrupt */
static volatile uint16_t s_rest = 0;        /* the duty when the queue is empty */
static int s_volume = 60;
static bool s_on;

static bool IRAM_ATTR tick(gptimer_handle_t t, const gptimer_alarm_event_data_t *e, void *arg)
{
    uint32_t duty = s_rest;
    if (s_tail != s_head) {
        duty = s_ring[s_tail % RING];
        s_tail++;
    }
    ledc_ll_set_duty_int_part(&LEDC, MODE, CH, duty);
    ledc_ll_set_duty_start(&LEDC, MODE, CH);
    ledc_ll_ls_channel_update(&LEDC, MODE, CH);
    return false;
}

esp_err_t amp_init(int gpio)
{
    ledc_timer_config_t t = { .speed_mode = MODE, .duty_resolution = BITS, .timer_num = LEDC_TIMER_0,
                              .freq_hz = 80000000 / TOP, .clk_cfg = LEDC_USE_APB_CLK };
    esp_err_t e = ledc_timer_config(&t);
    if (e != ESP_OK) return e;
    ledc_channel_config_t c = { .gpio_num = gpio, .speed_mode = MODE, .channel = CH, .timer_sel = LEDC_TIMER_0, .duty = 0 };
    if ((e = ledc_channel_config(&c)) != ESP_OK) return e;
    gptimer_config_t gc = { .clk_src = GPTIMER_CLK_SRC_DEFAULT, .direction = GPTIMER_COUNT_UP, .resolution_hz = 16000000 };
    if ((e = gptimer_new_timer(&gc, &s_timer)) != ESP_OK) return e;
    gptimer_alarm_config_t ac = { .alarm_count = 16000000 / AMP_FS, .reload_count = 0, .flags.auto_reload_on_alarm = true };
    gptimer_set_alarm_action(s_timer, &ac);
    gptimer_event_callbacks_t cb = { .on_alarm = tick };
    gptimer_register_event_callbacks(s_timer, &cb, NULL);
    gptimer_enable(s_timer);
    ESP_LOGI(TAG, "PWM audio on GPIO%d: %d kHz, %d bits", gpio, 80000 / TOP, BITS);
    return ESP_OK;
}

static void push(uint16_t duty)
{
    while (s_head - s_tail >= RING) vTaskDelay(1);
    s_ring[s_head % RING] = duty;
    s_head++;
}

static void ramp(int from, int to)          /* 30 ms, so the speaker doesn't thump */
{
    const int n = AMP_FS * 3 / 100;
    for (int i = 1; i <= n; i++) push((uint16_t)(from + (to - from) * i / n));
}

void amp_begin(void)
{
    if (s_on) return;
    s_head = s_tail = 0;
    s_rest = 0;
    gptimer_start(s_timer);
    ramp(0, MID);
    s_rest = MID;
    s_on = true;
}

bool amp_write(const int16_t *mono, int samples)
{
    if (!s_on) return false;
    int g = s_volume * s_volume;            /* perceptual: volume squared, 0..10000 */
    for (int i = 0; i < samples; i++) {
        int v = MID + (int)((int64_t)mono[i] * g * MID / (10000LL * 32768));
        push((uint16_t)(v < 0 ? 0 : v > TOP - 1 ? TOP - 1 : v));
    }
    return true;
}

void amp_end(void)
{
    if (!s_on) return;
    ramp(MID, 0);
    s_rest = 0;
    while (s_tail != s_head) vTaskDelay(1);
    vTaskDelay(pdMS_TO_TICKS(5));
    gptimer_stop(s_timer);
    ledc_stop(MODE, CH, 0);
    ledc_set_duty(MODE, CH, 0);
    ledc_update_duty(MODE, CH);
    s_on = false;
}

void amp_volume(int level) { s_volume = level < 0 ? 0 : level > 100 ? 100 : level; }
int amp_get_volume(void) { return s_volume; }
