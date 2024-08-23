#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "nvs_flash.h" // สำคัญเลยตัวนี้ ต้องมีตอนทำ WiFi

#include "lwip/err.h"
#include "lwip/sys.h"

#include <mqtt_client.h> // ของ idf เราไม่ต้องไปติดตั้งอะไรเพิ่ม

// ota
#include "esp_ota_ops.h"
#include "esp_http_client.h"
#include "esp_https_ota.h"

// Kconfig
#define ESP_WIFI_SSID CONFIG_ESP_WIFI_SSID
#define ESP_WIFI_PASS CONFIG_ESP_WIFI_PASSWORD
#define MAXIMUM_RETRY CONFIG_ESP_MAXIMUM_RETRY

// mqtt
#define TOPIC_PREFIX CONFIG_TOPIC_PREFIX
#define THING_ID CONFIG_THING_ID
#define USERNAME CONFIG_USERNAME
#define PASSWORD CONFIG_PASSWORD
#define MQTT_URI CONFIG_MQTT_URI

#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT BIT1
#define TAG "wifi station"
static const char *TAG_OTA = "My_OTA";

#define version "v1.7"

static EventGroupHandle_t s_wifi_event_group;
static int s_retry_num = 0; // Without static, s_retry_num would be globally accessible from any file in the program
static const char *MQTT_TAG = "mqtt";
static int mqtt_tcp_transport_error = 0;
static int mqtt_client_init = 0;
static esp_mqtt_client_handle_t client;
char topic_in[64] = {0};
char topic_out[64] = {0};
char topic_checkin[64] = {0};
char topic_ota[64] = {0};

// void subscribe_handler(char *topic, char *data);
// void ota_callback(char *payload);

// Version 1.5

static const uint8_t mqtt_eclipseprojects_io_pem_start[] = "-----BEGIN CERTIFICATE-----\n" CONFIG_CERTIFICATE_OVERRIDE "\n-----END CERTIFICATE-----";
static const uint8_t github_cert_pem_start[] = "-----BEGIN CERTIFICATE-----\n" CONFIG_CERTIFICATE_GITHUB "\n-----END CERTIFICATE-----";

void ota_task(void *pvParameter);

esp_err_t _http_event_handler(esp_http_client_event_t *evt)
{
    switch (evt->event_id)
    {
    case HTTP_EVENT_ERROR:
        ESP_LOGD(TAG_OTA, "HTTP_EVENT_ERROR");
        break;
    case HTTP_EVENT_ON_CONNECTED:
        ESP_LOGD(TAG_OTA, "HTTP_EVENT_ON_CONNECTED");
        break;
    case HTTP_EVENT_HEADER_SENT:
        ESP_LOGD(TAG_OTA, "HTTP_EVENT_HEADER_SENT");
        break;
    case HTTP_EVENT_ON_HEADER:
        ESP_LOGD(TAG_OTA, "HTTP_EVENT_ON_HEADER, key=%s, value=%s", evt->header_key, evt->header_value);
        break;
    case HTTP_EVENT_ON_DATA:
        ESP_LOGD(TAG_OTA, "HTTP_EVENT_ON_DATA, len=%d", evt->data_len);
        break;
    case HTTP_EVENT_ON_FINISH:
        ESP_LOGD(TAG_OTA, "HTTP_EVENT_ON_FINISH");
        break;
    case HTTP_EVENT_DISCONNECTED:
        ESP_LOGD(TAG_OTA, "HTTP_EVENT_DISCONNECTED");
        break;
    case HTTP_EVENT_REDIRECT:
        ESP_LOGD(TAG_OTA, "HTTP_EVENT_REDIRECT");
        break;
    }
    return ESP_OK;
}

void ota_task(void *pvParameter)
{
    ESP_LOGI(TAG_OTA, "Starting OTA task");
    char *api_url = (char *)pvParameter;
    vTaskDelay(1000 / portTICK_PERIOD_MS);
    esp_http_client_config_t config = {
        .url = api_url,
        .cert_pem = (const char *)github_cert_pem_start,
        .event_handler = _http_event_handler,
        .keep_alive_enable = true,
    };
    ESP_LOGI("Certificate", "Cert Content:\n%s", github_cert_pem_start);
    esp_https_ota_config_t ota_config = {
        .http_config = &config,
    };
    ESP_LOGI(TAG_OTA, "Attempting to download update from %s", config.url);
    esp_err_t ret = esp_https_ota(&ota_config);
    if (ret == ESP_OK)
    {
        ESP_LOGI(TAG_OTA, "OTA Succeed, Rebooting...");
        // xEventGroupSetBits(s_ota_event_group, OTA_SUCCESS_BIT);
    }
    else
    {
        ESP_LOGE(TAG_OTA, "Firmware upgrade failed");
        // xEventGroupSetBits(s_ota_event_group, OTA_FAIL_BIT);
    }
    vTaskDelete(NULL);
}

esp_err_t start_ota(const char *api_url)
{
    xTaskCreatePinnedToCore(&ota_task, "ota_task", 8192, (void *)api_url, 5, NULL, 0);
    return 0;
}

static void event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START)
    {
        esp_wifi_connect();
    }
    else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED)
    {
        if (s_retry_num < MAXIMUM_RETRY)
        {
            esp_wifi_connect();
            s_retry_num++;
            ESP_LOGI(TAG, "retry to connect to the AP");
        }
        else
        {
            xEventGroupSetBits(s_wifi_event_group, WIFI_FAIL_BIT);
        }
        ESP_LOGI(TAG, "connect to the AP fail");
    }
    else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP)
    {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        ESP_LOGI(TAG, "got ip:" IPSTR, IP2STR(&event->ip_info.ip));
        s_retry_num = 0;
        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
    }
}

static void mqtt_event_handler(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data)
{
    ESP_LOGI(MQTT_TAG, "Event dispatched from event loop base=%s, event_id=%ld", base, event_id);
    // printf("mqtt: Event dispatched from event loop base=%s, event_id=%ld\n", base, event_id);
    esp_mqtt_event_handle_t event = (esp_mqtt_event_handle_t)event_data;
    esp_mqtt_client_handle_t client = event->client;
    // int msg_id;
    static char received_message[100]; // Adjust size as needed
    switch ((esp_mqtt_event_id_t)event_id)
    {
    case MQTT_EVENT_CONNECTED:
        ESP_LOGI(MQTT_TAG, "MQTT_EVENT_CONNECTED");     // หลุดการเชื่อมต่อ ก็ต้อง subscribe อีกครั้ง
        esp_mqtt_client_subscribe(client, topic_in, 0); // subscribe ได้ จะทำ event นี้ 1 ครั้ง
        esp_mqtt_client_subscribe(client, topic_out, 0);
        esp_mqtt_client_subscribe(client, topic_checkin, 0);
        esp_mqtt_client_subscribe(client, topic_ota, 0);
        break;
    case MQTT_EVENT_DISCONNECTED: // หลุดการเชื่อมต่อกับ broker
        ESP_LOGI(MQTT_TAG, "MQTT_EVENT_DISCONNECTED");
        break;
    case MQTT_EVENT_DATA:
        ESP_LOGI(MQTT_TAG, "MQTT_EVENT_DATA");
        ESP_LOGI(MQTT_TAG, "TOPIC=%.*s", event->topic_len, event->topic);
        ESP_LOGI(MQTT_TAG, "DATA=%.*s", event->data_len, event->data);
        // subscribe_handler(event->topic, event->data);
        printf("'%.*s'\n", event->topic_len, event->topic); // Check for unexpected characters
        printf("'%s'\n", topic_ota);

        if (strstr(event->topic, topic_ota))
        {
            strncpy(received_message, event->data, event->data_len);
            received_message[event->data_len] = '\0'; // Null-terminate the string
            printf("'%s'\n", received_message);
            start_ota(received_message);
        }
        break;
    case MQTT_EVENT_ERROR:
        ESP_LOGI(MQTT_TAG, "MQTT_EVENT_ERROR");
        if (event->error_handle->error_type == MQTT_ERROR_TYPE_TCP_TRANSPORT)
        {
            mqtt_tcp_transport_error++;
            ESP_LOGI(MQTT_TAG, "mqtt_tcp_transport_error: %d\n", mqtt_tcp_transport_error);
            if (mqtt_tcp_transport_error == 5)
            {
                esp_mqtt_client_reconnect(client);
                mqtt_tcp_transport_error = 0;
            }
        }
        break;
    default:
        ESP_LOGI(MQTT_TAG, "Other event id:%d\r\n", event->event_id);
        break;
    }
}

static void init_mqtt()
{
    if (!mqtt_client_init)
    {
        char topic[64];
        char payload[64];
        sprintf(topic, "%s/%s/will", TOPIC_PREFIX, THING_ID);
        sprintf(topic_in, "%s/%s/in", TOPIC_PREFIX, THING_ID);
        sprintf(topic_out, "%s/%s/out", TOPIC_PREFIX, THING_ID);
        sprintf(topic_checkin, "%s/%s/checkin", TOPIC_PREFIX, THING_ID);
        sprintf(topic_ota, "%s/%s/ota", TOPIC_PREFIX, THING_ID);
        sprintf(payload, "{\"device_id\": \"%s\"}", THING_ID);
        const esp_mqtt_client_config_t mqtt_cfg = {
            .broker.address.uri = MQTT_URI,
            .broker.verification.certificate = (const char *)mqtt_eclipseprojects_io_pem_start,
            .credentials.username = USERNAME,
            .credentials.authentication.password = PASSWORD,
            .session.keepalive = 30,
            .session.last_will.topic = topic,
            .session.last_will.msg = payload,
            .network.disable_auto_reconnect = false, // ถ้ากำหนดเป็น True จะไม่ auto reconnect
            .network.reconnect_timeout_ms = 5000,    // จะ reconnect หลังจากหลุดการเชื่อมต่อ reconnect ทีละ 5000 ms
        };
        client = esp_mqtt_client_init(&mqtt_cfg);
        esp_mqtt_client_register_event(client, MQTT_EVENT_ANY, mqtt_event_handler, NULL);
        esp_mqtt_client_start(client); // need to have this command to initiate a connection to the MQTT broker
        mqtt_client_init = true;
    }
}

void wifi_init_sta()
{
    s_wifi_event_group = xEventGroupCreate();

    // create default connection about preparing WiFi Driver
    esp_wifi_disconnect();
    vTaskDelay(500 / portTICK_PERIOD_MS);
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    // register event callback related to event_handler
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &event_handler, NULL));

    // WiFi config
    wifi_config_t wifi_config = {
        .sta = {
            .ssid = ESP_WIFI_SSID,
            .password = ESP_WIFI_PASS,
            .threshold.authmode = WIFI_AUTH_WEP,
            .sae_pwe_h2e = WPA3_SAE_PWE_BOTH,
        },
    };
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    // Option Drop WiFi Power
    int8_t power;
    esp_wifi_get_max_tx_power(&power);
    ESP_LOGI(TAG, "esp_wifi_set_max_tx_power: %d", power);
    ESP_ERROR_CHECK(esp_wifi_set_max_tx_power(60));
    esp_wifi_get_max_tx_power(&power);
    ESP_LOGI(TAG, "esp_wifi_set_max_tx_power: %d", power);
    ESP_LOGI(TAG, "wifi_init_sta finished.");
    // +++++++++++++++++++++++++++++++++++++++++++++++++++

    EventBits_t bits = xEventGroupWaitBits(s_wifi_event_group,
                                           WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
                                           pdFALSE,
                                           pdFALSE,
                                           portMAX_DELAY);
    if (bits & WIFI_CONNECTED_BIT)
    {
        ESP_LOGI(TAG, "connected to ap SSID:%s password:%s", ESP_WIFI_SSID, ESP_WIFI_PASS);
        init_mqtt();
    }
    else if (bits & WIFI_FAIL_BIT)
    {
        ESP_LOGI(TAG, "Failed to connect to SSID:%s, password:%s",
                 ESP_WIFI_SSID, ESP_WIFI_PASS);
    }
    else
    {
        ESP_LOGE(TAG, "UNEXPECTED EVENT");
    }
}

void app_main()
{
    vTaskDelay(2000 / portTICK_PERIOD_MS);
    ESP_LOGI(TAG_OTA, "version:%s", version);

    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND)
    {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret); // ใช้ macro เช็คในการเช็ค initial flash

    wifi_init_sta(); // สามารถเอาไป run อยู่ใน task ได้ แต่ไม่ต้องใส่ while(true) นะ เพราะจะ run แค่ครั้งเดียว
}