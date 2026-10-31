#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/queue.h>
#include <esp_system.h>
#include <esp_event.h>
#include <esp_netif.h>
#include <esp_wifi.h>
#include <esp_log.h>
#include <esp_http_client.h>
#include <esp_err.h>
#include <nvs_flash.h>
#include <esp_timer.h>

#define TAG "TAILTRACE"
#define WIFI_CHANNEL_SWITCH_INTERVAL 100
#define WIFI_CHANNEL_MAX 13
#define MAX_BATCH_SIZE 32

typedef struct {
    uint8_t mac[6];
    int8_t rssi;
    uint32_t ts_ms;
    int count;
} SightEvent;

static QueueHandle_t g_event_queue = NULL;

static void mac_to_str(const uint8_t *mac, char *out) {
    snprintf(out, 18, "%02X:%02X:%02X:%02X:%02X:%02X",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

static void queue_push_event(const uint8_t *src_mac, int8_t rssi) {
    if (!g_event_queue) {
        return;
    }

    SightEvent event;
    memcpy(event.mac, src_mac, 6);
    event.rssi = rssi;
    event.ts_ms = (uint32_t)(esp_timer_get_time() / 1000ULL);
    event.count = 1;

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

static void send_payload_to_dashboard(const SightEvent *events, size_t count) {
    char json[4096];
    char *p = json;
    int len = snprintf(p, 4096, "{\"device_id\":\"esp32-tailtrace-sensor\",\"events\":[");
    p += len;

    for (size_t i = 0; i < count; i++) {
        char mac[18];
        mac_to_str(events[i].mac, mac);

        int written = snprintf(
            p,
            4096 - (p - json),
            "{\"mac\":\"%s\",\"rssi\":%d,\"count\":%d,\"ts_ms\":%u}%s",
            mac,
            events[i].rssi,
            events[i].count,
            events[i].ts_ms,
            (i + 1 < count) ? "," : ""
        );

        if (written < 0) {
            break;
        }
        p += written;
    }

    snprintf(p, 4096 - (p - json), "]}");

    ESP_LOGI(TAG, "Sending payload to TailTrace Guard:\n%s", json);

    esp_http_client_config_t config = {
        .url = "https://your-tailtrace-guard.example.com/api/v1/events",
        .method = HTTP_METHOD_POST,
        .timeout_ms = 60000,
        .cert_pem = NULL,
        .event_handler = NULL,
    };

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) {
        ESP_LOGE(TAG, "Failed to initialize HTTP client");
        return;
    }

    esp_http_client_set_header(client, "Content-Type", "application/json");
    esp_http_client_set_header(client, "Authorization", "Bearer YOUR_API_TOKEN");
    esp_http_client_set_post_field(client, json, strlen(json));

    esp_err_t err = esp_http_client_perform(client);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "HTTP status: %d", esp_http_client_get_status_code(client));
    } else {
        ESP_LOGE(TAG, "HTTP send failed: %s", esp_err_to_name(err));
    }

    esp_http_client_cleanup(client);
}

static void batching_task(void *arg) {
    SightEvent buffer[MAX_BATCH_SIZE];
    size_t count = 0;

    while (1) {
        SightEvent event;
        if (xQueueReceive(g_event_queue, &event, pdMS_TO_TICKS(5000)) == pdTRUE) {
            if (count < MAX_BATCH_SIZE) {
                buffer[count++] = event;
            }

            if (count >= MAX_BATCH_SIZE) {
                send_payload_to_dashboard(buffer, count);
                count = 0;
            }
        }

        if (count > 0 && uxQueueMessagesWaiting(g_event_queue) == 0) {
            send_payload_to_dashboard(buffer, count);
            count = 0;
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

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_NULL));
    ESP_ERROR_CHECK(esp_wifi_set_promiscuous(true));
    ESP_ERROR_CHECK(esp_wifi_set_promiscuous_rx_cb(wifi_sniffer_cb));
    ESP_ERROR_CHECK(esp_wifi_start());

    g_event_queue = xQueueCreate(128, sizeof(SightEvent));
    if (!g_event_queue) {
        ESP_LOGE(TAG, "Failed to create event queue");
        return;
    }

    xTaskCreate(wifi_channel_task, "wifi_channel_task", 4096, NULL, 5, NULL);
    xTaskCreate(batching_task, "batching_task", 8192, NULL, 4, NULL);

    ESP_LOGI(TAG, "TailTrace Guard sensor started");
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
