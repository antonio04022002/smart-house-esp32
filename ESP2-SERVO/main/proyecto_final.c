#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_wifi.h"
#include "esp_now.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "nvs_flash.h"
#include "driver/ledc.h"
#include "esp_adc/adc_oneshot.h"

#define SERVO_GPIO 23
#define PANEL_ADC_CHANNEL ADC_CHANNEL_6   // GPIO34

#define SERVO_MIN_US 500
#define SERVO_MAX_US 2400

static const char *TAG = "ESP2_SERVO_PANEL";

// MAC de ESP1: si no la sabes, usa broadcast por ahora
uint8_t mac_esp1[] = {0xFF,0xFF,0xFF,0xFF,0xFF,0xFF};

typedef struct {
    char comando[20];
    int valor;
} mensaje_t;

static adc_oneshot_unit_handle_t adc_handle;

static int puerta = 0;
static int panel_value = 0;

static void servo_angle(int angle)
{
    if (angle < 0) angle = 0;
    if (angle > 180) angle = 180;

    int pulse_us = SERVO_MIN_US + ((SERVO_MAX_US - SERVO_MIN_US) * angle) / 180;
    uint32_t duty = (pulse_us * 65535) / 20000;

    ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, duty);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0);
}

static void config_servo(void)
{
    ledc_timer_config_t timer = {
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .timer_num = LEDC_TIMER_0,
        .duty_resolution = LEDC_TIMER_16_BIT,
        .freq_hz = 50,
        .clk_cfg = LEDC_AUTO_CLK
    };

    ESP_ERROR_CHECK(ledc_timer_config(&timer));

    ledc_channel_config_t channel = {
        .gpio_num = SERVO_GPIO,
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel = LEDC_CHANNEL_0,
        .intr_type = LEDC_INTR_DISABLE,
        .timer_sel = LEDC_TIMER_0,
        .duty = 0,
        .hpoint = 0
    };

    ESP_ERROR_CHECK(ledc_channel_config(&channel));
}

static void config_adc(void)
{
    adc_oneshot_unit_init_cfg_t init_config = {
        .unit_id = ADC_UNIT_1
    };

    ESP_ERROR_CHECK(adc_oneshot_new_unit(&init_config, &adc_handle));

    adc_oneshot_chan_cfg_t adc_config = {
        .bitwidth = ADC_BITWIDTH_12,
        .atten = ADC_ATTEN_DB_12
    };

    ESP_ERROR_CHECK(adc_oneshot_config_channel(adc_handle, PANEL_ADC_CHANNEL, &adc_config));
}

static void enviar_panel(int valor)
{
    mensaje_t msg;
    memset(&msg, 0, sizeof(msg));

    strcpy(msg.comando, "PANEL");
    msg.valor = valor;

    esp_err_t res = esp_now_send(mac_esp1, (uint8_t *)&msg, sizeof(msg));

    if (res == ESP_OK) {
        ESP_LOGI(TAG, "Panel enviado: %d", valor);
    } else {
        ESP_LOGE(TAG, "Error enviando panel");
    }
}

static void recibir_espnow(const esp_now_recv_info_t *info, const uint8_t *data, int len)
{
    if (len != sizeof(mensaje_t)) return;

    mensaje_t msg;
    memcpy(&msg, data, sizeof(msg));

    ESP_LOGI(TAG, "Recibido: %s = %d", msg.comando, msg.valor);

    if (strcmp(msg.comando, "PUERTA") == 0) {
        puerta = msg.valor;

        if (puerta) {
            servo_angle(90);
            ESP_LOGI(TAG, "Puerta abierta");
        } else {
            servo_angle(0);
            ESP_LOGI(TAG, "Puerta cerrada");
        }
    }
}

static void tarea_panel(void *arg)
{
    while (1) {
        adc_oneshot_read(adc_handle, PANEL_ADC_CHANNEL, &panel_value);

        ESP_LOGI(TAG, "PANEL ADC: %d", panel_value);

        enviar_panel(panel_value);

        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

static void agregar_peer_master(void)
{
    esp_now_peer_info_t peer = {0};

    memcpy(peer.peer_addr, mac_esp1, 6);
    peer.channel = 1;
    peer.ifidx = WIFI_IF_STA;
    peer.encrypt = false;

    if (!esp_now_is_peer_exist(mac_esp1)) {
        ESP_ERROR_CHECK(esp_now_add_peer(&peer));
    }
}

static void iniciar_wifi_espnow(void)
{
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();

    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_ERROR_CHECK(esp_wifi_set_channel(1, WIFI_SECOND_CHAN_NONE));

    ESP_ERROR_CHECK(esp_now_init());
    ESP_ERROR_CHECK(esp_now_register_recv_cb(recibir_espnow));

    agregar_peer_master();

    ESP_LOGI(TAG, "ESP-NOW listo");
}

void app_main(void)
{
    esp_err_t ret = nvs_flash_init();

    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ESP_ERROR_CHECK(nvs_flash_init());
    }

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    config_servo();
    config_adc();

    servo_angle(0);

    iniciar_wifi_espnow();

    xTaskCreate(tarea_panel, "tarea_panel", 4096, NULL, 5, NULL);

    ESP_LOGI(TAG, "ESP2 servo + panel listo");
}