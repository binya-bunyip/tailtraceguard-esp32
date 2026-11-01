#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/queue.h>
#include <esp_wifi.h>
#include <esp_log.h>
#include <esp_err.h>
#include <nvs_flash.h>

#define TAG "TAILTRACE"
#define WIFI_CHANNEL_SWITCH_INTERVAL 100
#define WIFI_CHANNEL_MAX 13

typedef struct {
    uint8_t mac[6];
    int8_t rssi;
} SightEvent;

static QueueHandle_t g_event_queue = NULL;

static void queue_push_event(const uint8_t *src_mac, int8_t rssi) {
    if (!g_event_queue) {
        return;
    }

    SightEvent event;
    memcpy(event.mac, src_mac, 6);
    event.rssi = rssi;

    xQueueSend(g_event_queue, &event, portMAX_DELAY);
}

static void wifi_sniffer_cb(void *buf, wifi_promiscuous_pkt_type_t type) {
    if (type != WIFI_PKT_MGMT) {
        return;
    }

    wifi_promiscuous_pkt_t *pkt = (wifi_promiscuous_pkt_t *)buf;
    uint8_t *frame = pkt->payload;

    // Management frame subtype 0x40 = Probe Request
    if ((frame[0] & 0xF0) != 0x40) {
        return;
    }

    uint8_t *src_mac = &frame[10];
    int8_t rssi = pkt->rx_ctrl.rssi;
    queue_push_event(src_mac, rssi);
}

static void wifi_channel_task(void *arg) {
    while (1) {
        for (int channel = 1; channel <= WIFI_CHANNEL_MAX; channel++) {
            esp_wifi_set_channel(channel, WIFI_SECOND_CHAN_NONE);
            vTaskDelay(pdMS_TO_TICKS(WIFI_CHANNEL_SWITCH_INTERVAL));
        }
    }
}

static void observation_task(void *arg) {
    unsigned int count = 0;
    TickType_t window_start = xTaskGetTickCount();

    while (1) {
        SightEvent event;
        if (xQueueReceive(g_event_queue, &event, pdMS_TO_TICKS(1000)) == pdTRUE) {
            count++;
        }

        if (xTaskGetTickCount() - window_start >= pdMS_TO_TICKS(60000)) {
            ESP_LOGI(TAG, "Captured %u Wi-Fi probe requests; telemetry is not configured", count);
            count = 0;
            window_start = xTaskGetTickCount();
        }
    }
}

void app_main(void) {
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_NULL));
    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_ERROR_CHECK(esp_wifi_set_promiscuous_rx_cb(wifi_sniffer_cb));
    ESP_ERROR_CHECK(esp_wifi_set_promiscuous(true));

    g_event_queue = xQueueCreate(128, sizeof(SightEvent));
    if (!g_event_queue) {
        ESP_LOGE(TAG, "Failed to create event queue");
        return;
    }

    xTaskCreate(wifi_channel_task, "wifi_channel_task", 4096, NULL, 5, NULL);
    xTaskCreate(observation_task, "observation_task", 4096, NULL, 4, NULL);

    ESP_LOGI(TAG, "TailTrace Guard sensor started");
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
