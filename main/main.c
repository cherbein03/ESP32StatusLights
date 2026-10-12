#include <stdint.h>
#include <stdbool.h>

#include "driver/gpio.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "nvs_flash.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "led_strip.h"

#include "esp_zigbee.h"
#include "ezbee/bdb.h"
#include "ezbee/zha.h"

#define LED_COUNT 6
#define FIRST_ENDPOINT 10
#define PAIR_BUTTON_GPIO GPIO_NUM_0
#define PAIR_BUTTON_HOLD_MS 3000
#define STATUS_LED_GPIO GPIO_NUM_8
#define STATUS_LED_BLINK_MS 500
#define STATUS_LED_CONNECTED_MS 10000
#define ALL_ZIGBEE_CHANNELS 0x07FFF800U
#define STORAGE_PARTITION_NAME "nvs"
#define MANUFACTURER_NAME "\x0C" "StatusLights"
#define MODEL_IDENTIFIER "\x0A" "H2Zero6LED"

static const char *TAG = "status_lights";
static const gpio_num_t led_gpios[LED_COUNT] = {
    GPIO_NUM_10,
    GPIO_NUM_11,
    GPIO_NUM_12,
    GPIO_NUM_13,
    GPIO_NUM_14,
    GPIO_NUM_22,
};
static esp_timer_handle_t steering_retry_timer;
static led_strip_handle_t onboard_status_led;
static QueueHandle_t status_led_events;

typedef enum {
    STATUS_LED_EVENT_PAIRING,
    STATUS_LED_EVENT_CONNECTED,
} status_led_event_t;

typedef enum {
    STATUS_LED_MODE_PAIRING,
    STATUS_LED_MODE_CONNECTED,
    STATUS_LED_MODE_OFF,
} status_led_mode_t;

static void status_led_clear(void)
{
    ESP_ERROR_CHECK(led_strip_clear(onboard_status_led));
}

static void status_led_show_pairing(void)
{
    ESP_ERROR_CHECK(led_strip_set_pixel(onboard_status_led, 0, 0, 0, 24));
    ESP_ERROR_CHECK(led_strip_refresh(onboard_status_led));
}

static void status_led_show_connected(void)
{
    ESP_ERROR_CHECK(led_strip_set_pixel(onboard_status_led, 0, 0, 24, 0));
    ESP_ERROR_CHECK(led_strip_refresh(onboard_status_led));
}

static void status_led_notify(status_led_event_t event)
{
    (void)xQueueOverwrite(status_led_events, &event);
}

static void onboard_status_led_task(void *arg)
{
    (void)arg;
    status_led_mode_t mode = STATUS_LED_MODE_PAIRING;
    status_led_event_t event;
    bool blink_on = false;

    while (true) {
        if (mode == STATUS_LED_MODE_PAIRING) {
            blink_on = !blink_on;
            if (blink_on) {
                status_led_show_pairing();
            } else {
                status_led_clear();
            }
            if (xQueueReceive(status_led_events, &event,
                              pdMS_TO_TICKS(STATUS_LED_BLINK_MS)) == pdTRUE) {
                mode = event == STATUS_LED_EVENT_CONNECTED
                           ? STATUS_LED_MODE_CONNECTED
                           : STATUS_LED_MODE_PAIRING;
                blink_on = false;
                if (mode == STATUS_LED_MODE_CONNECTED) {
                    status_led_show_connected();
                }
            }
        } else if (mode == STATUS_LED_MODE_CONNECTED) {
            if (xQueueReceive(status_led_events, &event,
                              pdMS_TO_TICKS(STATUS_LED_CONNECTED_MS)) == pdTRUE) {
                mode = event == STATUS_LED_EVENT_PAIRING
                           ? STATUS_LED_MODE_PAIRING
                           : STATUS_LED_MODE_CONNECTED;
                blink_on = false;
                if (mode == STATUS_LED_MODE_CONNECTED) {
                    status_led_show_connected();
                }
            } else {
                status_led_clear();
                mode = STATUS_LED_MODE_OFF;
            }
        } else if (xQueueReceive(status_led_events, &event, portMAX_DELAY) == pdTRUE) {
            mode = event == STATUS_LED_EVENT_CONNECTED
                       ? STATUS_LED_MODE_CONNECTED
                       : STATUS_LED_MODE_PAIRING;
            blink_on = false;
            if (mode == STATUS_LED_MODE_CONNECTED) {
                status_led_show_connected();
            }
        }
    }
}

static void onboard_status_led_init(void)
{
    const led_strip_config_t led_config = {
        .strip_gpio_num = STATUS_LED_GPIO,
        .max_leds = 1,
        .led_model = LED_MODEL_WS2812,
        .color_component_format = LED_STRIP_COLOR_COMPONENT_FMT_GRB,
    };
    const led_strip_rmt_config_t rmt_config = {
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .resolution_hz = 10 * 1000 * 1000,
        .mem_block_symbols = 0,
        .flags = {
            .with_dma = false,
        },
    };

    ESP_ERROR_CHECK(led_strip_new_rmt_device(&led_config, &rmt_config,
                                             &onboard_status_led));
    ESP_ERROR_CHECK(led_strip_clear(onboard_status_led));
    status_led_events = xQueueCreate(1, sizeof(status_led_event_t));
    ESP_ERROR_CHECK(status_led_events != NULL ? ESP_OK : ESP_ERR_NO_MEM);
    ESP_ERROR_CHECK(xTaskCreate(onboard_status_led_task, "status_led", 3072,
                                NULL, 4, NULL) == pdPASS ? ESP_OK : ESP_FAIL);
}

static void factory_reset_callback(void *arg)
{
    (void)arg;
    ESP_LOGW(TAG, "Pairing reset requested; clearing Zigbee network data");
    status_led_notify(STATUS_LED_EVENT_PAIRING);
    esp_zigbee_factory_reset();
}

static void pairing_button_task(void *arg)
{
    (void)arg;
    bool pressed = false;
    TickType_t pressed_since = 0;

    while (true) {
        if (gpio_get_level(PAIR_BUTTON_GPIO) == 0) {
            if (!pressed) {
                pressed = true;
                pressed_since = xTaskGetTickCount();
            } else if ((xTaskGetTickCount() - pressed_since) >=
                       pdMS_TO_TICKS(PAIR_BUTTON_HOLD_MS)) {
                const esp_err_t err = esp_zigbee_task_queue_post(factory_reset_callback, NULL);
                if (err == ESP_OK) {
                    vTaskDelete(NULL);
                }
                ESP_LOGE(TAG, "Could not queue pairing reset: %s", esp_err_to_name(err));
                pressed = false;
            }
        } else {
            pressed = false;
        }

        vTaskDelay(pdMS_TO_TICKS(20));
    }
}

static void set_led_gpio(uint8_t endpoint, uint8_t on)
{
    if (endpoint < FIRST_ENDPOINT || endpoint >= FIRST_ENDPOINT + LED_COUNT) {
        return;
    }

    const uint8_t led_index = endpoint - FIRST_ENDPOINT;
    ESP_ERROR_CHECK(gpio_set_level(led_gpios[led_index], on != 0));
    ESP_LOGI(TAG, "LED %u (endpoint %u): %s", led_index + 1, endpoint,
             on ? "on" : "off");
}

static void steering_retry_callback(void *arg)
{
    (void)arg;
    esp_zigbee_lock_acquire(portMAX_DELAY);
    (void)ezb_bdb_start_top_level_commissioning(EZB_BDB_MODE_NETWORK_STEERING);
    esp_zigbee_lock_release();
}

static bool zigbee_signal_handler(const ezb_app_signal_t *signal)
{
    const ezb_app_signal_type_t signal_type = ezb_app_signal_get_type(signal);

    switch (signal_type) {
    case EZB_ZDO_SIGNAL_SKIP_STARTUP:
        ESP_LOGI(TAG, "Initializing Zigbee commissioning");
        (void)ezb_bdb_start_top_level_commissioning(EZB_BDB_MODE_INITIALIZATION);
        break;

    case EZB_BDB_SIGNAL_DEVICE_FIRST_START:
    case EZB_BDB_SIGNAL_DEVICE_REBOOT: {
        const ezb_bdb_comm_status_t status =
            *(const ezb_bdb_comm_status_t *)ezb_app_signal_get_params(signal);
        if (status == EZB_BDB_STATUS_SUCCESS) {
            if (ezb_bdb_is_factory_new()) {
                status_led_notify(STATUS_LED_EVENT_PAIRING);
                ESP_LOGI(TAG, "Searching for the Hubitat Zigbee network");
                (void)ezb_bdb_start_top_level_commissioning(EZB_BDB_MODE_NETWORK_STEERING);
            } else {
                status_led_notify(STATUS_LED_EVENT_CONNECTED);
            }
        }
        break;
    }

    case EZB_BDB_SIGNAL_STEERING: {
        const ezb_bdb_comm_status_t status =
            *(const ezb_bdb_comm_status_t *)ezb_app_signal_get_params(signal);
        if (status == EZB_BDB_STATUS_SUCCESS) {
            status_led_notify(STATUS_LED_EVENT_CONNECTED);
            ESP_LOGI(TAG, "Joined Zigbee network");
        } else {
            status_led_notify(STATUS_LED_EVENT_PAIRING);
            ESP_LOGW(TAG, "Network steering failed (0x%02x); retrying in 5 seconds", status);
            (void)esp_timer_start_once(steering_retry_timer, 5000000);
        }
        break;
    }

    default:
        ESP_LOGI(TAG, "Zigbee signal: %s", ezb_app_signal_to_string(signal_type));
        break;
    }

    return true;
}

static void zigbee_zcl_action_handler(ezb_zcl_core_action_callback_id_t callback_id,
                                      void *message)
{
    if (callback_id != EZB_ZCL_CORE_SET_ATTR_VALUE_CB_ID || message == NULL) {
        return;
    }

    const ezb_zcl_set_attr_value_message_t *attr_message = message;
    if (attr_message->info.cluster_id != EZB_ZCL_CLUSTER_ID_ON_OFF ||
        attr_message->info.cluster_role != EZB_ZCL_CLUSTER_SERVER ||
        attr_message->info.status != EZB_ZCL_STATUS_SUCCESS) {
        return;
    }

    set_led_gpio(attr_message->info.dst_ep,
                 *(const uint8_t *)attr_message->in.attribute.data.value);
}

static void create_light_endpoints(void)
{
    ezb_af_device_desc_t device = ezb_af_create_device_desc();

    for (uint8_t index = 0; index < LED_COUNT; ++index) {
        const uint8_t endpoint = FIRST_ENDPOINT + index;
        ezb_zha_on_off_light_config_t light_config = EZB_ZHA_ON_OFF_LIGHT_CONFIG();
        ezb_af_ep_desc_t endpoint_desc = ezb_zha_create_on_off_light(endpoint, &light_config);
        ezb_zcl_cluster_desc_t basic_cluster = ezb_af_endpoint_get_cluster_desc(
            endpoint_desc, EZB_ZCL_CLUSTER_ID_BASIC, EZB_ZCL_CLUSTER_SERVER);

        ESP_ERROR_CHECK(ezb_zcl_basic_cluster_desc_add_attr(
            basic_cluster, EZB_ZCL_ATTR_BASIC_MANUFACTURER_NAME_ID,
            (void *)MANUFACTURER_NAME));
        ESP_ERROR_CHECK(ezb_zcl_basic_cluster_desc_add_attr(
            basic_cluster, EZB_ZCL_ATTR_BASIC_MODEL_IDENTIFIER_ID,
            (void *)MODEL_IDENTIFIER));
        ESP_ERROR_CHECK(ezb_af_device_add_endpoint_desc(device, endpoint_desc));
    }

    ESP_ERROR_CHECK(ezb_af_device_desc_register(device));
    ezb_zcl_core_action_handler_register(zigbee_zcl_action_handler);
}

static void zigbee_main_task(void *arg)
{
    (void)arg;
    const esp_zigbee_config_t config = {
        .device_config = {
            .device_type = EZB_NWK_DEVICE_TYPE_ROUTER,
            .install_code_policy = false,
            .zczr_config = {
                .max_children = 10,
            },
        },
        .platform_config = {
            .storage_partition_name = STORAGE_PARTITION_NAME,
            .radio_config = {
                .radio_mode = ESP_ZIGBEE_RADIO_MODE_NATIVE,
            },
        },
    };

    ESP_ERROR_CHECK(esp_zigbee_init(&config));
    ezb_aps_secur_enable_distributed_security(false);
    ESP_ERROR_CHECK(ezb_bdb_set_primary_channel_set(ALL_ZIGBEE_CHANNELS));
    ESP_ERROR_CHECK(ezb_bdb_set_secondary_channel_set(ALL_ZIGBEE_CHANNELS));
    ESP_ERROR_CHECK(ezb_app_signal_add_handler(zigbee_signal_handler));
    create_light_endpoints();
    ESP_ERROR_CHECK(esp_zigbee_start(false));
    esp_zigbee_launch_mainloop();
    vTaskDelete(NULL);
}

void app_main(void)
{
    ESP_LOGI(TAG, "Booting, reset reason: %d", esp_reset_reason());

    gpio_config_t gpio_settings = {
        .pin_bit_mask = (1ULL << GPIO_NUM_10) | (1ULL << GPIO_NUM_11) |
                (1ULL << GPIO_NUM_12) | (1ULL << GPIO_NUM_13) |
                (1ULL << GPIO_NUM_14) | (1ULL << GPIO_NUM_22),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };

    for (uint8_t index = 0; index < LED_COUNT; ++index) {
        ESP_ERROR_CHECK(gpio_set_level(led_gpios[index], 0));
    }
    ESP_ERROR_CHECK(gpio_config(&gpio_settings));
    for (uint8_t index = 0; index < LED_COUNT; ++index) {
        ESP_ERROR_CHECK(gpio_set_level(led_gpios[index], 1));
    }
    vTaskDelay(pdMS_TO_TICKS(2000));
    for (uint8_t index = 0; index < LED_COUNT; ++index) {
        ESP_ERROR_CHECK(gpio_set_level(led_gpios[index], 0));
    }
    onboard_status_led_init();

    const gpio_config_t button_settings = {
        .pin_bit_mask = 1ULL << PAIR_BUTTON_GPIO,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&button_settings));

    esp_err_t nvs_status = nvs_flash_init();
    if (nvs_status == ESP_ERR_NVS_NO_FREE_PAGES ||
        nvs_status == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "NVS needs recovery (%s); erasing NVS", esp_err_to_name(nvs_status));
        ESP_ERROR_CHECK(nvs_flash_erase());
        nvs_status = nvs_flash_init();
    }
    ESP_ERROR_CHECK(nvs_status);
    const esp_timer_create_args_t retry_timer_args = {
        .callback = steering_retry_callback,
        .name = "zigbee_steering_retry",
    };
    ESP_ERROR_CHECK(esp_timer_create(&retry_timer_args, &steering_retry_timer));
    ESP_ERROR_CHECK(xTaskCreate(pairing_button_task, "pair_button", 3072, NULL, 5, NULL)
                        == pdPASS ? ESP_OK : ESP_FAIL);
    ESP_LOGI(TAG, "Starting four-endpoint Zigbee light router");
    ESP_ERROR_CHECK(xTaskCreate(zigbee_main_task, "zigbee_main", 6144, NULL, 5, NULL)
                        == pdPASS ? ESP_OK : ESP_FAIL);
}