#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/queue.h>
#include <freertos/event_groups.h>
#include <esp_event.h>
#include <esp_netif.h>
#include <esp_wifi.h>
#include <esp_log.h>
#include <esp_err.h>
#include <esp_http_client.h>
#include <esp_crt_bundle.h>
#include <esp_sntp.h>
#include <nvs.h>
#include <nvs_flash.h>

#define TAG "TAILTRACE"
#define WIFI_CHANNEL_SWITCH_INTERVAL 100
#define WIFI_CHANNEL_MAX 13
#define CAPTURE_WINDOW_MS 60000
#define MAX_WIFI_SSID_LENGTH 32
#define MAX_WIFI_PASSWORD_LENGTH 64
#define MAX_DEVICE_TOKEN_LENGTH 1024
#define WIFI_CONNECTED_BIT BIT0
#define WIFI_DISCONNECTED_BIT BIT1
#define TELEMETRY_URL "https://tail-guard.polsia.io/api/telemetry"

typedef struct {
    uint8_t mac[6];
    int8_t rssi;
    uint16_t frequency_mhz;
    time_t observed_at;
} SightEvent;

static QueueHandle_t g_event_queue = NULL;
static EventGroupHandle_t g_wifi_event_group = NULL;
static char g_wifi_ssid[MAX_WIFI_SSID_LENGTH + 1];
static char g_wifi_password[MAX_WIFI_PASSWORD_LENGTH + 1];
static char g_device_token[MAX_DEVICE_TOKEN_LENGTH + 1];

static bool read_serial_line(const char *prompt, char *buffer, size_t capacity,
                             bool allow_empty) {
    printf("%s", prompt);
    fflush(stdout);

    if (!fgets(buffer, capacity, stdin)) {
        return false;
    }

    size_t length = strcspn(buffer, "\r\n");
    if (length == capacity - 1 && buffer[length] == '\0') {
        int next = getchar();
        if (next == '\r') {
            int after_carriage_return = getchar();
            if (after_carriage_return != '\n' && after_carriage_return != EOF) {
                next = after_carriage_return;
            } else {
                next = '\n';
            }
        }
        if (next != '\n' && next != EOF) {
            while (next != '\n' && next != EOF) {
                next = getchar();
            }
            return false;
        }
    }
    buffer[length] = '\0';
    return allow_empty || length > 0;
}

static bool load_provisioned_config(void) {
    nvs_handle_t handle;
    if (nvs_open("tailtrace", NVS_READONLY, &handle) != ESP_OK) {
        return false;
    }

    size_t ssid_length = sizeof(g_wifi_ssid);
    size_t password_length = sizeof(g_wifi_password);
    size_t token_length = sizeof(g_device_token);
    esp_err_t result = nvs_get_str(handle, "wifi_ssid", g_wifi_ssid, &ssid_length);
    if (result == ESP_OK) {
        result = nvs_get_str(handle, "wifi_password", g_wifi_password, &password_length);
    }
    if (result == ESP_OK) {
        result = nvs_get_str(handle, "device_token", g_device_token, &token_length);
    }
    nvs_close(handle);
    return result == ESP_OK;
}

static bool provision_over_serial(void) {
    nvs_handle_t handle;
    printf("Private USB serial setup. Credentials will be stored unencrypted in device NVS.\n");

    if (!read_serial_line("Wi-Fi network name: ", g_wifi_ssid, sizeof(g_wifi_ssid), false) ||
        !read_serial_line("Wi-Fi password (blank for an open network): ", g_wifi_password,
                          sizeof(g_wifi_password), true) ||
        !read_serial_line("Registered device token: ", g_device_token,
                          sizeof(g_device_token), false)) {
        ESP_LOGE(TAG, "Serial provisioning input was incomplete or too long");
        return false;
    }

    if (nvs_open("tailtrace", NVS_READWRITE, &handle) != ESP_OK) {
        ESP_LOGE(TAG, "Could not open NVS for provisioning");
        return false;
    }

    esp_err_t result = nvs_set_str(handle, "wifi_ssid", g_wifi_ssid);
    if (result == ESP_OK) {
        result = nvs_set_str(handle, "wifi_password", g_wifi_password);
    }
    if (result == ESP_OK) {
        result = nvs_set_str(handle, "device_token", g_device_token);
    }
    if (result == ESP_OK) {
        result = nvs_commit(handle);
    }
    nvs_close(handle);

    if (result != ESP_OK) {
        ESP_LOGE(TAG, "Could not save serial provisioning data");
        return false;
    }

    ESP_LOGI(TAG, "Device credentials stored; secret values were not logged");
    return true;
}

static void queue_push_event(const uint8_t *src_mac, int8_t rssi, uint8_t channel) {
    if (!g_event_queue) {
        return;
    }

    SightEvent event;
    memcpy(event.mac, src_mac, 6);
    event.rssi = rssi;
    event.frequency_mhz = 2407 + channel * 5;
    time(&event.observed_at);

    xQueueSend(g_event_queue, &event, 0);
}

static void wifi_sniffer_cb(void *buf, wifi_promiscuous_pkt_type_t type) {
    if (type != WIFI_PKT_MGMT) {
        return;
    }

    wifi_promiscuous_pkt_t *pkt = (wifi_promiscuous_pkt_t *)buf;
    const uint8_t *frame = pkt->payload;

    if (pkt->rx_ctrl.sig_len < 24 || (frame[0] & 0xFC) != 0x40) {
        return;
    }

    uint8_t channel = pkt->rx_ctrl.channel;
    if (channel < 1 || channel > WIFI_CHANNEL_MAX) {
        return;
    }

    queue_push_event(&frame[10], pkt->rx_ctrl.rssi, channel);
}

static void wifi_event_handler(void *arg, esp_event_base_t event_base,
                               int32_t event_id, void *event_data) {
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        xEventGroupClearBits(g_wifi_event_group, WIFI_CONNECTED_BIT);
        xEventGroupSetBits(g_wifi_event_group, WIFI_DISCONNECTED_BIT);
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        xEventGroupSetBits(g_wifi_event_group, WIFI_CONNECTED_BIT);
    }
}

static bool connect_to_wifi(void) {
    wifi_config_t wifi_config = {0};
    strlcpy((char *)wifi_config.sta.ssid, g_wifi_ssid, sizeof(wifi_config.sta.ssid));
    strlcpy((char *)wifi_config.sta.password, g_wifi_password,
            sizeof(wifi_config.sta.password));

    ESP_ERROR_CHECK(esp_wifi_set_promiscuous(false));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));

    for (int attempt = 0; attempt < 3; attempt++) {
        xEventGroupClearBits(g_wifi_event_group,
                            WIFI_CONNECTED_BIT | WIFI_DISCONNECTED_BIT);
        if (esp_wifi_connect() != ESP_OK) {
            continue;
        }

        EventBits_t bits = xEventGroupWaitBits(
            g_wifi_event_group, WIFI_CONNECTED_BIT | WIFI_DISCONNECTED_BIT,
            pdTRUE, pdFALSE, pdMS_TO_TICKS(20000));
        if (bits & WIFI_CONNECTED_BIT) {
            return true;
        }
    }

    return false;
}

static bool synchronize_clock(void) {
    static bool sntp_started = false;
    if (!sntp_started) {
        esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);
        esp_sntp_setservername(0, "pool.ntp.org");
        esp_sntp_init();
        sntp_started = true;
    }

    for (int attempt = 0; attempt < 20; attempt++) {
        if (esp_sntp_get_sync_status() == SNTP_SYNC_STATUS_COMPLETED) {
            return true;
        }
        vTaskDelay(pdMS_TO_TICKS(1000));
    }

    return false;
}

static bool format_timestamp(time_t timestamp, char *buffer, size_t capacity) {
    struct tm utc_time;
    if (!gmtime_r(&timestamp, &utc_time)) {
        return false;
    }
    return strftime(buffer, capacity, "%Y-%m-%dT%H:%M:%SZ", &utc_time) > 0;
}

static bool send_telemetry(const SightEvent *event) {
    char address[18];
    char observed_at[24];
    char json[192];
    char authorization[MAX_DEVICE_TOKEN_LENGTH + 8];
    snprintf(address, sizeof(address), "%02X:%02X:%02X:%02X:%02X:%02X",
             event->mac[0], event->mac[1], event->mac[2], event->mac[3],
             event->mac[4], event->mac[5]);

    if (!format_timestamp(event->observed_at, observed_at, sizeof(observed_at))) {
        ESP_LOGW(TAG, "Skipping observation with invalid UTC timestamp");
        return false;
    }

    int json_length = snprintf(
        json, sizeof(json),
        "{\"observedAt\":\"%s\",\"observedAddress\":\"%s\","
        "\"protocol\":\"wifi\",\"rssiDbm\":%d,\"frequencyMHz\":%u}",
        observed_at, address, event->rssi, event->frequency_mhz);
    if (json_length < 0 || (size_t)json_length >= sizeof(json)) {
        ESP_LOGW(TAG, "Skipping observation that could not be serialized");
        return false;
    }

    snprintf(authorization, sizeof(authorization), "Bearer %s", g_device_token);
    esp_http_client_config_t http_config = {
        .url = TELEMETRY_URL,
        .method = HTTP_METHOD_POST,
        .timeout_ms = 15000,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };
    esp_http_client_handle_t client = esp_http_client_init(&http_config);
    if (!client) {
        ESP_LOGE(TAG, "Could not initialize HTTPS client");
        return false;
    }

    esp_http_client_set_header(client, "Content-Type", "application/json");
    esp_http_client_set_header(client, "Authorization", authorization);
    esp_http_client_set_post_field(client, json, json_length);
    esp_err_t result = esp_http_client_perform(client);
    int status_code = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);

    if (result != ESP_OK || status_code < 200 || status_code >= 300) {
        ESP_LOGW(TAG, "Telemetry request failed (HTTP %d)", status_code);
        return false;
    }

    return true;
}

static void capture_window(void) {
    ESP_ERROR_CHECK(esp_wifi_set_promiscuous(false));
    esp_wifi_disconnect();
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_NULL));
    ESP_ERROR_CHECK(esp_wifi_set_channel(1, WIFI_SECOND_CHAN_NONE));
    ESP_ERROR_CHECK(esp_wifi_set_promiscuous(true));

    TickType_t window_start = xTaskGetTickCount();
    uint8_t channel = 1;
    while (xTaskGetTickCount() - window_start < pdMS_TO_TICKS(CAPTURE_WINDOW_MS)) {
        esp_err_t result = esp_wifi_set_channel(channel, WIFI_SECOND_CHAN_NONE);
        if (result != ESP_OK) {
            ESP_LOGW(TAG, "Could not set Wi-Fi scan channel %u", channel);
        }
        channel = channel == WIFI_CHANNEL_MAX ? 1 : channel + 1;
        vTaskDelay(pdMS_TO_TICKS(WIFI_CHANNEL_SWITCH_INTERVAL));
    }

    ESP_ERROR_CHECK(esp_wifi_set_promiscuous(false));
}

static void upload_queued_observations(void) {
    SightEvent pending[128];
    size_t pending_count = 0;
    while (pending_count < sizeof(pending) / sizeof(pending[0]) &&
           xQueueReceive(g_event_queue, &pending[pending_count], 0) == pdTRUE) {
        pending_count++;
    }

    for (size_t index = 0; index < pending_count; index++) {
        if (!send_telemetry(&pending[index])) {
            for (size_t retry = index; retry < pending_count; retry++) {
                xQueueSend(g_event_queue, &pending[retry], 0);
            }
            ESP_LOGW(TAG, "Telemetry failed; unsent observations remain queued");
            return;
        }
    }

    ESP_LOGI(TAG, "Uploaded %u Wi-Fi observations", (unsigned int)pending_count);
}

static void telemetry_task(void *arg) {
    while (1) {
        if (connect_to_wifi() && synchronize_clock()) {
            break;
        }
        ESP_LOGW(TAG, "Wi-Fi or UTC synchronization unavailable; telemetry is paused");
        esp_wifi_disconnect();
        vTaskDelay(pdMS_TO_TICKS(5000));
    }

    while (1) {
        capture_window();
        if (connect_to_wifi()) {
            if (synchronize_clock()) {
                upload_queued_observations();
            } else {
                ESP_LOGW(TAG, "UTC synchronization failed; observations remain queued");
            }
        } else {
            ESP_LOGW(TAG, "Wi-Fi reconnect failed; observations remain queued");
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

    if (!load_provisioned_config() && !provision_over_serial()) {
        ESP_LOGE(TAG, "Restart device with a private USB serial session to provision it");
        return;
    }

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();
    g_wifi_event_group = xEventGroupCreate();
    if (!g_wifi_event_group) {
        ESP_LOGE(TAG, "Failed to create Wi-Fi event group");
        return;
    }

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                               wifi_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                               wifi_event_handler, NULL));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_ERROR_CHECK(esp_wifi_set_promiscuous_rx_cb(wifi_sniffer_cb));

    g_event_queue = xQueueCreate(128, sizeof(SightEvent));
    if (!g_event_queue) {
        ESP_LOGE(TAG, "Failed to create event queue");
        return;
    }

    xTaskCreate(telemetry_task, "telemetry_task", 12288, NULL, 4, NULL);

    ESP_LOGI(TAG, "TailTrace Guard Wi-Fi telemetry started");
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
