#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_wifi.h"
#include "esp_now.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_http_server.h"
#include "nvs_flash.h"
#include "driver/i2c_master.h"

#define WIFI_SSID "SMART_HOME"
#define WIFI_PASS "12345678"

#define I2C_SDA 21
#define I2C_SCL 22
#define OLED_ADDR 0x3C
#define OLED_SPEED 10000

static const char *TAG = "ESP1_MASTER";

uint8_t mac_esp2[] = {0xEC,0xE3,0x34,0x6F,0x6F,0x40};
uint8_t mac_esp3[] = {0x68,0x25,0xDD,0x30,0xB0,0x6C};

typedef struct {
    char comando[20];
    int valor;
} mensaje_t;

static char html[16000];

static int puerta = 0;
static int led1 = 0;
static int led2 = 0;
static int led3 = 0;
static int led4 = 0;
static int brillo = 100;
static int modo_auto = 0;
static int panel_value = 0;
static int umbral_oscuridad = 1600;

static i2c_master_bus_handle_t i2c_bus = NULL;
static i2c_master_dev_handle_t oled_dev = NULL;
static int oled_ok = 0;

static const uint8_t font5x7[][5] = {
    [' ']={0,0,0,0,0},
    ['0']={0x3E,0x51,0x49,0x45,0x3E}, ['1']={0x00,0x42,0x7F,0x40,0x00},
    ['2']={0x42,0x61,0x51,0x49,0x46}, ['3']={0x21,0x41,0x45,0x4B,0x31},
    ['4']={0x18,0x14,0x12,0x7F,0x10}, ['5']={0x27,0x45,0x45,0x45,0x39},
    ['6']={0x3C,0x4A,0x49,0x49,0x30}, ['7']={0x01,0x71,0x09,0x05,0x03},
    ['8']={0x36,0x49,0x49,0x49,0x36}, ['9']={0x06,0x49,0x49,0x29,0x1E},
    ['A']={0x7E,0x11,0x11,0x11,0x7E}, ['B']={0x7F,0x49,0x49,0x49,0x36},
    ['C']={0x3E,0x41,0x41,0x41,0x22}, ['D']={0x7F,0x41,0x41,0x22,0x1C},
    ['E']={0x7F,0x49,0x49,0x49,0x41}, ['F']={0x7F,0x09,0x09,0x09,0x01},
    ['G']={0x3E,0x41,0x49,0x49,0x7A}, ['H']={0x7F,0x08,0x08,0x08,0x7F},
    ['I']={0x00,0x41,0x7F,0x41,0x00}, ['J']={0x20,0x40,0x41,0x3F,0x01},
    ['K']={0x7F,0x08,0x14,0x22,0x41}, ['L']={0x7F,0x40,0x40,0x40,0x40},
    ['M']={0x7F,0x02,0x0C,0x02,0x7F}, ['N']={0x7F,0x04,0x08,0x10,0x7F},
    ['O']={0x3E,0x41,0x41,0x41,0x3E}, ['P']={0x7F,0x09,0x09,0x09,0x06},
    ['R']={0x7F,0x09,0x19,0x29,0x46}, ['S']={0x46,0x49,0x49,0x49,0x31},
    ['T']={0x01,0x01,0x7F,0x01,0x01}, ['U']={0x3F,0x40,0x40,0x40,0x3F},
    ['V']={0x1F,0x20,0x40,0x20,0x1F}, ['W']={0x7F,0x20,0x18,0x20,0x7F},
    ['X']={0x63,0x14,0x08,0x14,0x63}, ['Y']={0x07,0x08,0x70,0x08,0x07},
    ['Z']={0x61,0x51,0x49,0x45,0x43},
    [':']={0x00,0x36,0x36,0x00,0x00},
    ['%']={0x62,0x64,0x08,0x13,0x23},
    ['-']={0x08,0x08,0x08,0x08,0x08}
};

static esp_err_t oled_cmd(uint8_t cmd)
{
    if (!oled_ok || oled_dev == NULL) return ESP_FAIL;
    uint8_t data[2] = {0x00, cmd};
    return i2c_master_transmit(oled_dev, data, 2, 50 / portTICK_PERIOD_MS);
}

static esp_err_t oled_data(uint8_t *data, size_t len)
{
    if (!oled_ok || oled_dev == NULL) return ESP_FAIL;

    uint8_t buffer[129];

    if (len > 128) len = 128;

    buffer[0] = 0x40;
    memcpy(&buffer[1], data, len);

    return i2c_master_transmit(oled_dev, buffer, len + 1, 50 / portTICK_PERIOD_MS);
}

static void oled_clear(void)
{
    if (!oled_ok) return;

    uint8_t zeros[128];
    memset(zeros, 0x00, 128);

    for (int page = 0; page < 4; page++) {
        if (oled_cmd(0xB0 + page) != ESP_OK) return;
        if (oled_cmd(0x00) != ESP_OK) return;
        if (oled_cmd(0x10) != ESP_OK) return;
        if (oled_data(zeros, 128) != ESP_OK) return;
    }
}

static void oled_text(int page, int col, const char *text)
{
    if (!oled_ok) return;
    if (page < 0 || page > 3) return;

    if (oled_cmd(0xB0 + page) != ESP_OK) return;
    if (oled_cmd(0x00 + (col & 0x0F)) != ESP_OK) return;
    if (oled_cmd(0x10 + ((col >> 4) & 0x0F)) != ESP_OK) return;

    while (*text) {
        uint8_t ch = (uint8_t)*text++;
        uint8_t data[6] = {0,0,0,0,0,0};

        if (ch >= 128) ch = ' ';

        memcpy(data, font5x7[ch], 5);

        if (oled_data(data, 6) != ESP_OK) return;
    }
}

static void oled_update(void)
{
    if (!oled_ok) return;

    char l1[24];
    char l2[24];
    char l3[24];
    char l4[24];

    snprintf(l1, sizeof(l1), "SMART HOME");
    snprintf(l2, sizeof(l2), "P:%d B:%d%%", panel_value, brillo);
    snprintf(l3, sizeof(l3), "AUTO:%s", modo_auto ? "ON" : "OFF");
    snprintf(l4, sizeof(l4), "D:%s L:%d%d%d%d", puerta ? "OP" : "CL", led1, led2, led3, led4);

    oled_clear();
    vTaskDelay(pdMS_TO_TICKS(20));

    oled_text(0, 0, l1);
    oled_text(1, 0, l2);
    oled_text(2, 0, l3);
    oled_text(3, 0, l4);
}

static void oled_init_safe(void)
{
    oled_ok = 0;

    i2c_master_bus_config_t bus_config = {
        .i2c_port = I2C_NUM_1,
        .sda_io_num = I2C_SDA,
        .scl_io_num = I2C_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true
    };

    esp_err_t ret = i2c_new_master_bus(&bus_config, &i2c_bus);

    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "OLED no inicio bus I2C: %s", esp_err_to_name(ret));
        return;
    }

    i2c_device_config_t dev_config = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = OLED_ADDR,
        .scl_speed_hz = OLED_SPEED
    };

    ret = i2c_master_bus_add_device(i2c_bus, &dev_config, &oled_dev);

    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "OLED no agrego dispositivo: %s", esp_err_to_name(ret));
        return;
    }

    ret = i2c_master_probe(i2c_bus, OLED_ADDR, 100 / portTICK_PERIOD_MS);

    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "OLED no responde en 0x%02X: %s", OLED_ADDR, esp_err_to_name(ret));
        return;
    }

    oled_ok = 1;

    oled_cmd(0xAE);
    oled_cmd(0xD5); oled_cmd(0x80);
    oled_cmd(0xA8); oled_cmd(0x1F);
    oled_cmd(0xD3); oled_cmd(0x00);
    oled_cmd(0x40);
    oled_cmd(0x8D); oled_cmd(0x14);
    oled_cmd(0x20); oled_cmd(0x00);
    oled_cmd(0xA1);
    oled_cmd(0xC8);
    oled_cmd(0xDA); oled_cmd(0x02);
    oled_cmd(0x81); oled_cmd(0xCF);
    oled_cmd(0xD9); oled_cmd(0xF1);
    oled_cmd(0xDB); oled_cmd(0x40);
    oled_cmd(0xA4);
    oled_cmd(0xA6);
    oled_cmd(0xAF);

    oled_clear();
    oled_update();

    ESP_LOGI(TAG, "OLED activa");
}

static void enviar_comando(uint8_t *mac, const char *comando, int valor)
{
    mensaje_t msg;
    memset(&msg, 0, sizeof(msg));

    strncpy(msg.comando, comando, sizeof(msg.comando) - 1);
    msg.valor = valor;

    esp_err_t res = esp_now_send(mac, (uint8_t *)&msg, sizeof(msg));

    if (res == ESP_OK) {
        ESP_LOGI(TAG, "Enviado a ESP-NOW: %s=%d", comando, valor);
    } else {
        ESP_LOGE(TAG, "Error enviando %s=%d : %s", comando, valor, esp_err_to_name(res));
    }
}

static void aplicar_auto_por_panel(void)
{
    if (!modo_auto) return;

    if (panel_value < umbral_oscuridad) {
        led1 = led2 = led3 = led4 = 1;

        brillo = 100 - ((panel_value * 100) / umbral_oscuridad);

        if (brillo < 30) brillo = 30;
        if (brillo > 100) brillo = 100;

        enviar_comando(mac_esp3, "BRILLO", brillo);
        ESP_LOGI(TAG, "BRILLO AUTO enviado a ESP3: %d", brillo);

        enviar_comando(mac_esp3, "LUCES", 1);
    } else {
        led1 = led2 = led3 = led4 = 0;
        enviar_comando(mac_esp3, "LUCES", 0);
    }

    oled_update();
}

static void recibir_espnow(const esp_now_recv_info_t *info, const uint8_t *data, int len)
{
    if (len != sizeof(mensaje_t)) return;

    mensaje_t msg;
    memcpy(&msg, data, sizeof(msg));

    ESP_LOGI(TAG, "Recibido %s=%d", msg.comando, msg.valor);

    if (strcmp(msg.comando, "PANEL") == 0) {
        panel_value = msg.valor;
        aplicar_auto_por_panel();
    }

    oled_update();
}

static void procesar_brillo(httpd_req_t *req)
{
    char query[64];
    char value[16];

    brillo = 100;

    if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK) {
        ESP_LOGI(TAG, "Query recibida: %s", query);

        if (httpd_query_key_value(query, "val", value, sizeof(value)) == ESP_OK) {
            brillo = atoi(value);

            if (brillo < 0) brillo = 0;
            if (brillo > 100) brillo = 100;

            enviar_comando(mac_esp3, "BRILLO", brillo);

            ESP_LOGI(TAG, "BRILLO enviado a ESP3: %d", brillo);
        }
    }
}

static esp_err_t web_handler(httpd_req_t *req)
{
    const char *uri = req->uri;

    ESP_LOGI(TAG, "URI recibida: %s", uri);

    if (strcmp(uri, "/open") == 0) {
        puerta = 1;
        enviar_comando(mac_esp2, "PUERTA", 1);
    } else if (strcmp(uri, "/close") == 0) {
        puerta = 0;
        enviar_comando(mac_esp2, "PUERTA", 0);
    } else if (strcmp(uri, "/led1") == 0) {
        led1 = !led1;
        enviar_comando(mac_esp3, "LED1", led1);
    } else if (strcmp(uri, "/led2") == 0) {
        led2 = !led2;
        enviar_comando(mac_esp3, "LED2", led2);
    } else if (strcmp(uri, "/led3") == 0) {
        led3 = !led3;
        enviar_comando(mac_esp3, "LED3", led3);
    } else if (strcmp(uri, "/led4") == 0) {
        led4 = !led4;
        enviar_comando(mac_esp3, "LED4", led4);
    } else if (strcmp(uri, "/allon") == 0) {
        led1 = led2 = led3 = led4 = 1;
        enviar_comando(mac_esp3, "LUCES", 1);
    } else if (strcmp(uri, "/alloff") == 0) {
        led1 = led2 = led3 = led4 = 0;
        enviar_comando(mac_esp3, "LUCES", 0);
    } else if (strcmp(uri, "/autoon") == 0) {
        modo_auto = 1;
        aplicar_auto_por_panel();
    } else if (strcmp(uri, "/autooff") == 0) {
        modo_auto = 0;
        enviar_comando(mac_esp3, "AUTO", 0);
    } else if (strncmp(uri, "/brillo", 7) == 0) {
    procesar_brillo(req);
}
    oled_update();

    snprintf(html, sizeof(html),
        "<!DOCTYPE html><html><head>"
        "<meta name='viewport' content='width=device-width, initial-scale=1.0'>"
        "<title>Smart House</title>"
        "<style>"
        "*{box-sizing:border-box;}"
        "body{font-family:Arial;background:linear-gradient(135deg,#020617,#0f172a);color:white;text-align:center;margin:0;padding:18px;}"
        "h1{color:#38bdf8;margin:10px 0 0;font-size:32px;}"
        "h3{color:#94a3b8;margin-top:5px;}"
        ".card{background:rgba(30,41,59,.95);margin:16px auto;padding:20px;border-radius:24px;max-width:460px;box-shadow:0 10px 25px rgba(0,0,0,.35);border:1px solid rgba(148,163,184,.15);}"
        ".row{display:flex;justify-content:space-between;align-items:center;margin:15px 0;padding:10px 5px;}"
        ".label{text-align:left;font-size:18px;font-weight:bold;}"
        ".state{color:#22c55e;font-weight:bold;}"
        ".btn{display:block;text-decoration:none;color:white;padding:15px;margin:10px auto;border-radius:16px;font-size:17px;font-weight:bold;width:90%%;border:none;}"
        ".blue{background:#2563eb;}.red{background:#dc2626;}.green{background:#16a34a;}.gray{background:#475569;}"
        ".switch{position:relative;display:inline-block;width:70px;height:38px;}"
        ".slider{position:absolute;cursor:pointer;top:0;left:0;right:0;bottom:0;background:#64748b;transition:.3s;border-radius:40px;}"
        ".slider:before{position:absolute;content:'';height:30px;width:30px;left:4px;bottom:4px;background:white;transition:.3s;border-radius:50%%;}"
        ".checked{background:#22c55e;}.checked:before{transform:translateX(32px);}"
        "input[type=range]{width:90%%;height:12px;border-radius:10px;accent-color:#38bdf8;}"
        ".mini{color:#94a3b8;font-size:14px;}"
        "</style></head><body>"

        "<h1>SMART HOUSE</h1><h3>ESP32 Maestro Web</h3>"

        "<div class='card'><h2>Estado General</h2>"
        "<p>Panel solar ADC: <span class='state'>%d</span></p>"
        "<p>Puerta: <span class='state'>%s</span></p>"
        "<p>Automatico: <span class='state'>%s</span></p>"
        "<p>Intensidad: <span class='state'>%d%%</span></p>"
        "<p>OLED: <span class='state'>%s</span></p>"
        "</div>"

        "<div class='card'><h2>Puerta</h2>"
        "<a class='btn blue' href='/open'>ABRIR PUERTA</a>"
        "<a class='btn red' href='/close'>CERRAR PUERTA</a>"
        "</div>"

        "<div class='card'><h2>Luces Individuales</h2>"
        "<div class='row'><div class='label'>LED 1</div><a href='/led1' class='switch'><span class='slider %s'></span></a></div>"
        "<div class='row'><div class='label'>LED 2</div><a href='/led2' class='switch'><span class='slider %s'></span></a></div>"
        "<div class='row'><div class='label'>LED 3</div><a href='/led3' class='switch'><span class='slider %s'></span></a></div>"
        "<div class='row'><div class='label'>LED 4</div><a href='/led4' class='switch'><span class='slider %s'></span></a></div>"
        "<a class='btn green' href='/allon'>ENCENDER TODAS</a>"
        "<a class='btn red' href='/alloff'>APAGAR TODAS</a>"
        "</div>"

        "<div class='card'><h2>Intensidad</h2>"
        "<form action='/brillo' method='GET'>"
        "<input type='range' name='val' min='0' max='100' value='%d' oninput='this.nextElementSibling.value=this.value'>"
        "<output>%d</output><span>%%</span><br><br>"
        "<button class='btn blue' type='submit'>APLICAR INTENSIDAD</button>"
        "</form><p class='mini'>Control PWM de brillo de LEDs</p></div>"

        "<div class='card'><h2>Modo Automatico</h2>"
        "<a class='btn blue' href='/autoon'>AUTO ON</a>"
        "<a class='btn gray' href='/autooff'>AUTO OFF</a>"
        "<p class='mini'>ESP2 lee el panel solar y ESP3 ejecuta las luces.</p>"
        "</div>"

        "</body></html>",
        panel_value,
        puerta ? "ABIERTA" : "CERRADA",
        modo_auto ? "ON" : "OFF",
        brillo,
        oled_ok ? "ACTIVA" : "OFF",
        led1 ? "checked" : "",
        led2 ? "checked" : "",
        led3 ? "checked" : "",
        led4 ? "checked" : "",
        brillo,
        brillo
    );

    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, html, HTTPD_RESP_USE_STRLEN);

    return ESP_OK;
}

static void iniciar_web(void)
{
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();

    config.uri_match_fn = httpd_uri_match_wildcard;
    config.lru_purge_enable = true;
    config.stack_size = 8192;

    httpd_handle_t server = NULL;

    httpd_uri_t uri_all = {
        .uri = "/*",
        .method = HTTP_GET,
        .handler = web_handler,
        .user_ctx = NULL
    };

    if (httpd_start(&server, &config) == ESP_OK) {
        httpd_register_uri_handler(server, &uri_all);
        ESP_LOGI(TAG, "Servidor web iniciado");
    }
}

static void agregar_peer(uint8_t *mac)
{
    esp_now_peer_info_t peer = {0};
    memcpy(peer.peer_addr, mac, 6);

    peer.channel = 1;
    peer.ifidx = WIFI_IF_STA;
    peer.encrypt = false;

    if (!esp_now_is_peer_exist(mac)) {
        ESP_ERROR_CHECK(esp_now_add_peer(&peer));
    }
}

static void iniciar_espnow(void)
{
    ESP_ERROR_CHECK(esp_now_init());
    ESP_ERROR_CHECK(esp_now_register_recv_cb(recibir_espnow));

    agregar_peer(mac_esp2);
    agregar_peer(mac_esp3);

    ESP_LOGI(TAG, "ESP-NOW listo");
}

static void iniciar_wifi(void)
{
    esp_netif_create_default_wifi_ap();
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();

    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    wifi_config_t ap_config = {
        .ap = {
            .ssid = WIFI_SSID,
            .ssid_len = strlen(WIFI_SSID),
            .channel = 1,
            .password = WIFI_PASS,
            .max_connection = 4,
            .authmode = WIFI_AUTH_WPA_WPA2_PSK
        }
    };

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap_config));
    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_ERROR_CHECK(esp_wifi_set_channel(1, WIFI_SECOND_CHAN_NONE));

    ESP_LOGI(TAG, "WiFi creado");
    ESP_LOGI(TAG, "SSID: %s", WIFI_SSID);
    ESP_LOGI(TAG, "PASS: %s", WIFI_PASS);
    ESP_LOGI(TAG, "IP: 192.168.4.1");
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

    iniciar_wifi();
    iniciar_espnow();
    iniciar_web();

    oled_init_safe();
    oled_update();

    ESP_LOGI(TAG, "ESP1 maestro listo");
}