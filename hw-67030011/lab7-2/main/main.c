#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "nvs_flash.h"
#include "driver/gpio.h"
#include "network_provisioning/manager.h"
#include "network_provisioning/scheme_softap.h"

static const char *TAG = "LAB7_2_SOFTAP";

#define FACTORY_RESET_BUTTON_GPIO  GPIO_NUM_21   // ปุ่ม Factory Reset (Active-Low ต่อลง GND)
#define LED_PIN_WIFI_STA           GPIO_NUM_2    // LED 1: Wi-Fi STA Status
#define LED_PIN_SOFTAP_PROV        GPIO_NUM_5    // LED 3: SoftAP Provisioning Status
#define PROV_POP_KEY               "abcd1234"   // Proof-of-Possession (PoP)

typedef enum {
    LED_STA_OFF = 0,
    LED_STA_CONNECTED,       // Heartbeat: ติด 200ms ในทุก 1 วินาที
    LED_STA_DISCONNECTED     // Alert: ติด 200ms ดับ 200ms
} led_sta_mode_t;

typedef enum {
    LED_SOFTAP_OFF = 0,
    LED_SOFTAP_LISTENING,    // Slow Blink: ติด 500ms ดับ 500ms
    LED_SOFTAP_TRANSFERRING  // Fast Blink: ติด 100ms ดับ 100ms
} led_softap_mode_t;

static volatile led_sta_mode_t g_led_sta_mode = LED_STA_OFF;
static volatile led_softap_mode_t g_led_softap_mode = LED_SOFTAP_OFF;

/* FreeRTOS Background Task สำหรับควบคุมไฟสถานะ LED 1 และ LED 3 */
static void led_status_task(void *pvParameters)
{
    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << LED_PIN_WIFI_STA) | (1ULL << LED_PIN_SOFTAP_PROV),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&io_conf);

    uint32_t tick = 0;
    while (1) {
        // จัดการ LED 1 (Wi-Fi STA)
        if (g_led_sta_mode == LED_STA_CONNECTED) {
            gpio_set_level(LED_PIN_WIFI_STA, (tick % 10 < 2) ? 1 : 0);
        } else if (g_led_sta_mode == LED_STA_DISCONNECTED) {
            gpio_set_level(LED_PIN_WIFI_STA, (tick % 4 < 2) ? 1 : 0);
        } else {
            gpio_set_level(LED_PIN_WIFI_STA, 0);
        }

        // จัดการ LED 3 (SoftAP)
        if (g_led_softap_mode == LED_SOFTAP_LISTENING) {
            gpio_set_level(LED_PIN_SOFTAP_PROV, (tick % 10 < 5) ? 1 : 0); // 500ms / 500ms
        } else if (g_led_softap_mode == LED_SOFTAP_TRANSFERRING) {
            gpio_set_level(LED_PIN_SOFTAP_PROV, (tick % 2 < 1) ? 1 : 0);  // 100ms / 100ms
        } else {
            gpio_set_level(LED_PIN_SOFTAP_PROV, 0);
        }

        tick++;
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

/* ตรวจสอบการกดปุ่ม Factory Reset (GPIO 21) ค้างไว้ 3 วินาที */
static bool check_factory_reset_button(void)
{
    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << FACTORY_RESET_BUTTON_GPIO),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&io_conf);

    int hold_count = 0;
    while (gpio_get_level(FACTORY_RESET_BUTTON_GPIO) == 0) {
        vTaskDelay(pdMS_TO_TICKS(100));
        hold_count++;
        if (hold_count >= 30) {
            ESP_LOGW(TAG, "=================================================");
            ESP_LOGW(TAG, ">>> FACTORY RESET TRIGGERED! ERASING NVS FLASH <<<");
            ESP_LOGW(TAG, "=================================================");
            return true;
        }
    }
    return false;
}

static void event_handler(void* arg, esp_event_base_t event_base, int32_t event_id, void* event_data)
{
    if (event_base == NETWORK_PROV_EVENT) {
        switch (event_id) {
            case NETWORK_PROV_START:
                ESP_LOGI(TAG, "[PROV EVENT]: SoftAP Provisioning Started!");
                g_led_softap_mode = LED_SOFTAP_LISTENING;
                break;
            case NETWORK_PROV_WIFI_CRED_RECV: {
                wifi_sta_config_t *sta_cfg = (wifi_sta_config_t *)event_data;
                ESP_LOGI(TAG, "=================================================");
                ESP_LOGI(TAG, "[CREDENTIALS RECEIVED]:");
                ESP_LOGI(TAG, "  -> Target SSID     : %s", (const char *)sta_cfg->ssid);
                ESP_LOGI(TAG, "  -> Target Password : %s", (const char *)sta_cfg->password);
                ESP_LOGI(TAG, "=================================================");
                g_led_softap_mode = LED_SOFTAP_TRANSFERRING;
                break;
            }
            case NETWORK_PROV_WIFI_CRED_FAIL:
                ESP_LOGE(TAG, "[ERROR]: Wi-Fi Connection failed with provided credentials!");
                g_led_softap_mode = LED_SOFTAP_LISTENING;
                break;
            case NETWORK_PROV_WIFI_CRED_SUCCESS:
                ESP_LOGI(TAG, "[SUCCESS]: Provisioning Completed Successfully!");
                g_led_softap_mode = LED_SOFTAP_OFF;
                break;
            case NETWORK_PROV_END:
                ESP_LOGI(TAG, "[PROV EVENT]: De-initializing Provisioning Manager");
                network_prov_mgr_deinit();
                break;
            default:
                break;
        }
    } else if (event_base == WIFI_EVENT) {
        if (event_id == WIFI_EVENT_AP_STACONNECTED) {
            ESP_LOGI(TAG, "[SOFTAP]: Mobile Phone connected to ESP32 SoftAP!");
            g_led_softap_mode = LED_SOFTAP_TRANSFERRING;
        } else if (event_id == WIFI_EVENT_AP_STADISCONNECTED) {
            ESP_LOGW(TAG, "[SOFTAP]: Mobile Phone disconnected from ESP32 SoftAP");
            g_led_softap_mode = LED_SOFTAP_LISTENING;
        } else if (event_id == WIFI_EVENT_STA_START) {
            g_led_sta_mode = LED_STA_DISCONNECTED;
            esp_wifi_connect();
        } else if (event_id == WIFI_EVENT_STA_DISCONNECTED) {
            ESP_LOGW(TAG, "Wi-Fi Disconnected. Reconnecting...");
            g_led_sta_mode = LED_STA_DISCONNECTED;
            esp_wifi_connect();
        }
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t* event = (ip_event_got_ip_t*) event_data;
        ESP_LOGI(TAG, "=================================================");
        ESP_LOGI(TAG, "[ONLINE]: Connected with IP: " IPSTR, IP2STR(&event->ip_info.ip));
        ESP_LOGI(TAG, "=================================================");
        g_led_sta_mode = LED_STA_CONNECTED;
        g_led_softap_mode = LED_SOFTAP_OFF;
    }
}

void app_main(void)
{
    xTaskCreate(led_status_task, "led_task", 2048, NULL, 5, NULL);

    if (check_factory_reset_button()) {
        ESP_LOGW(TAG, "[FORENSIC]: User requested Flash Erase via Button!");
        ESP_ERROR_CHECK(nvs_flash_erase());
    }

    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ESP_ERROR_CHECK(nvs_flash_init());
    }

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    // ลงทะเบียน Event Handlers
    ESP_ERROR_CHECK(esp_event_handler_register(NETWORK_PROV_EVENT, ESP_EVENT_ANY_ID, &event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &event_handler, NULL));

    esp_netif_create_default_wifi_sta();
    esp_netif_create_default_wifi_ap();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    // กำหนดค่า Provisioning Manager เป็น SoftAP Scheme
    network_prov_mgr_config_t config = {
        .scheme = network_prov_scheme_softap,
        .scheme_event_handler = NETWORK_PROV_EVENT_HANDLER_NONE
    };
    ESP_ERROR_CHECK(network_prov_mgr_init(config));

    bool provisioned = false;
    ESP_ERROR_CHECK(network_prov_mgr_is_wifi_provisioned(&provisioned));

    if (!provisioned) {
        // สร้างชื่อ SoftAP เฉพาะตัวจาก MAC Address
        uint8_t mac[6];
        esp_wifi_get_mac(WIFI_IF_STA, mac);
        char service_name[16];
        snprintf(service_name, sizeof(service_name), "PROV_%02X%02X%02X", mac[3], mac[4], mac[5]);

        ESP_LOGI(TAG, "Starting SoftAP Provisioning (SSID: %s, PoP: %s)", service_name, PROV_POP_KEY);

        // Security 1 with Proof-of-Possession
        network_prov_security_t security = NETWORK_PROV_SECURITY_1;
        const char *pop = PROV_POP_KEY;

        ESP_ERROR_CHECK(network_prov_mgr_start_provisioning(security, (const void *)pop, service_name, NULL));

        ESP_LOGI(TAG, "--------------------------------------------------");
        ESP_LOGI(TAG, "[QR CODE URL]: Copy or click URL to scan QR Code:");
        ESP_LOGI(TAG, "https://espressif.github.io/esp-jumpstart/qrcode.html?data=%%7B%%22ver%%22%%3A%%22v1%%22%%2C%%22name%%22%%3A%%22%s%%22%%2C%%22pop%%22%%3A%%22%s%%22%%2C%%22transport%%22%%3A%%22softap%%22%%7D",
                 service_name, pop);
        ESP_LOGI(TAG, "Payload JSON: {\"ver\":\"v1\",\"name\":\"%s\",\"pop\":\"%s\",\"transport\":\"softap\"}",
                 service_name, pop);
        ESP_LOGI(TAG, "--------------------------------------------------");
    } else {
        ESP_LOGI(TAG, "Already provisioned! Starting Wi-Fi Station");
        network_prov_mgr_deinit();
        ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
        ESP_ERROR_CHECK(esp_wifi_start());
    }

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
