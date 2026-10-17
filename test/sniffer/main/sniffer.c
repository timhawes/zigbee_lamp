/*
 * Passive IEEE 802.15.4 sniffer for debugging Zigbee joins.
 * Prints one line per received frame:  F <channel> <rssi> <lqi> <hex PSDU incl. FCS>
 * Send "c<NN>\n" on the console to change channel.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "esp_ieee802154.h"
#include "esp_log.h"
#include "sdkconfig.h"

typedef struct {
    uint8_t len;
    int8_t rssi;
    uint8_t lqi;
    uint8_t channel;
    uint8_t psdu[127];
} rx_frame_t;

static QueueHandle_t s_queue;
static volatile uint8_t s_channel = CONFIG_SNIFFER_CHANNEL;
static volatile uint32_t s_dropped;

void esp_ieee802154_receive_done(uint8_t *frame, esp_ieee802154_frame_info_t *info)
{
    rx_frame_t f;
    f.len = frame[0] > 127 ? 127 : frame[0];
    f.rssi = info->rssi;
    f.lqi = info->lqi;
    f.channel = s_channel;
    memcpy(f.psdu, frame + 1, f.len);
    BaseType_t woken = pdFALSE;
    if (xQueueSendFromISR(s_queue, &f, &woken) != pdTRUE) {
        s_dropped++;
    }
    esp_ieee802154_receive_handle_done(frame);
    portYIELD_FROM_ISR(woken);
}

static void print_task(void *arg)
{
    static char line[16 + 127 * 2 + 2];
    rx_frame_t f;
    uint32_t reported_drops = 0;
    while (true) {
        if (xQueueReceive(s_queue, &f, pdMS_TO_TICKS(1000)) == pdTRUE) {
            int n = snprintf(line, sizeof(line), "F %u %d %u ", f.channel, f.rssi, f.lqi);
            for (int i = 0; i < f.len; i++) {
                n += snprintf(line + n, sizeof(line) - n, "%02x", f.psdu[i]);
            }
            puts(line);
        }
        if (s_dropped != reported_drops) {
            reported_drops = s_dropped;
            printf("D %lu\n", (unsigned long)reported_drops);
        }
    }
}

static void set_channel(uint8_t ch)
{
    s_channel = ch;
    esp_ieee802154_set_channel(ch);
    esp_ieee802154_receive();
    printf("C %u\n", ch);
}

void app_main(void)
{
    s_queue = xQueueCreate(64, sizeof(rx_frame_t));
    esp_ieee802154_enable();
    esp_ieee802154_set_promiscuous(true);
    esp_ieee802154_set_rx_when_idle(true);
    esp_ieee802154_set_coordinator(false);
    set_channel(CONFIG_SNIFFER_CHANNEL);
    xTaskCreate(print_task, "print", 4096, NULL, 5, NULL);

    char buf[8];
    while (true) {
        if (fgets(buf, sizeof(buf), stdin) && buf[0] == 'c') {
            int ch = atoi(buf + 1);
            if (ch >= 11 && ch <= 26) {
                set_channel(ch);
            }
        }
        vTaskDelay(pdMS_TO_TICKS(50));
    }
}
