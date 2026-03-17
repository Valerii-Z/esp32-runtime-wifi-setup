#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <esp_system.h>
#include <esp_event.h>
#include <esp_wifi.h>
#include <esp_http_server.h>
#include <nvs_flash.h>
#include <esp_spiffs.h>
#include <freertos/FreerTOS.h>
#include <freertos/task.h>
#include <freertos/event_groups.h>
#include <freertos/queue.h>
#include <esp_err.h>
#include <esp_log.h>
#include <sys/stat.h>  
#include <sys/param.h>
#include <driver/gpio.h>
#include <esp_timer.h>

#define MAX_FAILURES 4  // max attempts connection 
#define MAX_APs 8
#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT BIT1
#define IP_GOTADR_BIT BIT2

#define BUTTON_PIN   GPIO_NUM_18
#define LED_PIN      GPIO_NUM_2

static QueueHandle_t gpio_evt_queue = NULL;
static const char *TAG = "WIFI_MANAGER";
httpd_handle_t server = NULL;
static EventGroupHandle_t s_wifi_event_group;
static int s_retry_num = 0;
static uint8_t s_disconnect_reason = 0;
char IP_STA[16];
char sta_ssid[32];
char *networks_list;

#define INDEX_HTML_PATH "/spiffs/index.html"
char index_html[2048];
char response_data[2048];

static void initi_web_page_buffer(void)
{
    esp_vfs_spiffs_conf_t conf = {
        .base_path = "/spiffs",
        .partition_label = NULL,
        .max_files = 5,
        .format_if_mount_failed = true};
    ESP_ERROR_CHECK(esp_vfs_spiffs_register(&conf));

    memset((void *)index_html, 0, sizeof(index_html));
    struct stat st;
    if (stat(INDEX_HTML_PATH, &st)){
        ESP_LOGE(TAG, "index.html not found");
        return;
    }
    FILE *fp = fopen(INDEX_HTML_PATH, "r");
    if (fread(index_html, st.st_size, 1, fp) == 0)
        ESP_LOGE(TAG, "fread failed");
    fclose(fp);
}

char * scan_wifi_networks(void) {
    uint16_t ap_count = MAX_APs;
    uint16_t total_ap = 0;
    wifi_ap_record_t ap_list[MAX_APs];

    // scan settings 
    wifi_scan_config_t scan_config = {
        .ssid = 0,
        .bssid = 0,
        .channel = 0,
        .show_hidden = false
    };

    // blocking scan
    ESP_ERROR_CHECK(esp_wifi_scan_start(&scan_config, true));
    ESP_ERROR_CHECK(esp_wifi_scan_get_ap_num(&total_ap));

    ESP_LOGI("SCAN", "Found APs: %d", total_ap);
    
    // read APs records
    ESP_ERROR_CHECK(esp_wifi_scan_get_ap_records(&ap_count, ap_list));
    if (ap_count > MAX_APs) ap_count = MAX_APs;
    
    // HTML list generation 
   static char html_fragment[2048];
    memset(html_fragment, 0, sizeof(html_fragment));
    
    strcat(html_fragment, "<select name='ssid' style='padding:10px; margin-bottom:10px;'>");
    for (int i = 0; i < ap_count; i++) {
        char item[128];
        // SSID is limited to 32 characters
        snprintf(item, sizeof(item),
                 "<option value='%.*s'>%.*s (RSSI: %d)</option>",
                 32, ap_list[i].ssid,
                 32, ap_list[i].ssid,
                 ap_list[i].rssi);
        strcat(html_fragment, item);
    }
    strcat(html_fragment, "</select>");
    return html_fragment;
}

//Handler for the initial page of the WiFi connection configuration web server. 
//Contains a form for selecting found SSIDs and entering a password.
esp_err_t index_get_handler(httpd_req_t *req) {   
    httpd_resp_sendstr_chunk(req,
        "<!DOCTYPE html><html><head><meta charset='utf-8'>"
        "<meta name='viewport' content='width=device-width, initial-scale=1'>"
        "<title>Wi-Fi Setup</title></head><body>"
        "<h2>Оберіть мережу</h2>"
        "<form method='POST' action='/save'>");

    httpd_resp_sendstr_chunk(req, networks_list);

    httpd_resp_sendstr_chunk(req,
        "<input type='password' name='password' placeholder='Пароль' "
        "style='width:60%; padding:10px;'>"
        "<br><br><input type='submit' value='Підключитись' "
        "style='padding:10px 20px;'>"
        "</form></body></html>");

    httpd_resp_sendstr_chunk(req, NULL);
    return ESP_OK;
}

// Handler for a client-submitted form with ssid and password
esp_err_t wifi_config_post_handler(httpd_req_t *req) {
    char buf[128]; 
    int ret = httpd_req_recv(req, buf, MIN(req->content_len, sizeof(buf) - 1));
    if (ret <= 0) return ESP_FAIL;
    buf[ret] = '\0';
    // For simplicity, decoding url string is omitted
    char ssid_raw[32] = {0}, pass_raw[64] = {0};

    // getting SSID and password 
    httpd_query_key_value(buf, "ssid", ssid_raw, sizeof(ssid_raw));

    if (httpd_query_key_value(buf, "password", pass_raw, sizeof(pass_raw));

    ESP_LOGI(TAG, "Received: SSID='%s', PASS='%s'", ssid_raw, pass_raw);

   /*Attempt to connect to the access point in STA mode 
    using the received ssid and password values.*/
    // Initialize and populate wifi_config_t with SSID and password
    wifi_config_t wifi_config = {0};
    snprintf((char *)wifi_config.sta.ssid, sizeof(wifi_config.sta.ssid), "%s", ssid_raw);
    snprintf((char *)wifi_config.sta.password, sizeof(wifi_config.sta.password), "%s", pass_raw);

    // clear all event bits before start
    xEventGroupClearBits(s_wifi_event_group, WIFI_CONNECTED_BIT | WIFI_FAIL_BIT);
    s_disconnect_reason = 0; 
    s_retry_num = MAX_FAILURES; // Blocking auto connect 

    esp_wifi_disconnect(); 
    vTaskDelay(pdMS_TO_TICKS(100);
    esp_err_t err = esp_wifi_set_config(WIFI_IF_STA, &wifi_config);
    esp_wifi_connect();
    
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "Settings successfully stored in NVS");
        
        EventBits_t bits = xEventGroupWaitBits(s_wifi_event_group, 
            WIFI_FAIL_BIT | IP_GOTADR_BIT, 
            pdTRUE, pdFALSE, pdMS_TO_TICKS(15000));
        if (bits & IP_GOTADR_BIT) {
            ESP_LOGI(TAG, "Restart esp32");
            
            char msg[384];
            snprintf(msg, sizeof(msg),   "<html><head><meta charset='utf-8'><meta name='viewport' content='width=device-width,initial-scale=1'>"
            "<meta http-equiv='refresh' content='5;url=http://%s/'></head>"
            "<body style='text-align:center;font-family:sans-serif;padding:20px'>"
            "<h1>Success!</h1><p>IP: <b>%s</b></p><small>Redirect in 5 s...</small>"
            "</body></html>", IP_STA, IP_STA);
            httpd_resp_sendstr(req, msg);
            vTaskDelay(pdMS_TO_TICKS(1500));        
            esp_restart();
        } else { // WIFI_FAIL_BIT
            char err_text[128];
            switch (s_disconnect_reason) {
                case WIFI_REASON_AUTH_EXPIRE:
                case WIFI_REASON_AUTH_FAIL:
                case WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT:
                case 204: 
                    snprintf(err_text, sizeof(err_text), "Error: Wrong password");
                    break;
                case WIFI_REASON_NO_AP_FOUND:
                    snprintf(err_text, sizeof(err_text), "Error: Wrong SSID!");
                    break;
                default:
                    snprintf(err_text, sizeof(err_text), "Error connection (code %d)", s_disconnect_reason);
            }

            char resp[256];
            snprintf(resp, sizeof(resp), "<h1>%s</h1><a href='/'>Try again</a>", err_text);
            httpd_resp_set_type(req, "text/html; charset=utf-8");
            httpd_resp_sendstr(req, resp);
        }
        
    } else {
        // Error writing ssid and password in NVS
        ESP_LOGE(TAG, "Can't write settings in NVS: %s", esp_err_to_name(err));
        
        char error_msg[100];
        snprintf(error_msg, sizeof(error_msg), "Помилка збереження: %s", esp_err_to_name(err));
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, error_msg);
    }

    return ESP_OK;
}

httpd_handle_t start_conf_webserver(void) {
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    httpd_handle_t server = NULL;

    if (httpd_start(&server, &config) == ESP_OK) {
        
        httpd_uri_t index_get = {
            .uri      = "/",
            .method   = HTTP_GET,
            .handler  = index_get_handler, 
            .user_ctx = NULL
        };
        httpd_register_uri_handler(server, &index_get);

        // POST запит — отримуємо SSID та пароль
        httpd_uri_t config_post = {
            .uri      = "/save",
            .method   = HTTP_POST,
            .handler  = wifi_config_post_handler,
            .user_ctx = NULL
        };
        httpd_register_uri_handler(server, &config_post);
    }
    return server;
}

esp_err_t get_req_handler(httpd_req_t *req)
{
    int response;
    sprintf(response_data, index_html, sta_ssid);
    
    response = httpd_resp_send(req, response_data, HTTPD_RESP_USE_STRLEN);
    return response;
}

httpd_handle_t start_main_webserver(void)
{
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();

    httpd_uri_t uri_get = {
        .uri = "/",
        .method = HTTP_GET,
        .handler = get_req_handler,
        .user_ctx = NULL};

        if (httpd_start(&server, &config) == ESP_OK)
           httpd_register_uri_handler(server, &uri_get);

    return server;
}

static void event_handler(void* arg, esp_event_base_t event_base,
                                int32_t event_id, void* event_data) {
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        // Сбрасываем флаги подключения
        xEventGroupClearBits(s_wifi_event_group, (WIFI_FAIL_BIT | WIFI_CONNECTED_BIT)); 
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        // Сбрасываем флаг подключения, если он был установлен
        xEventGroupClearBits(s_wifi_event_group, WIFI_CONNECTED_BIT); 
        wifi_event_sta_disconnected_t* event = (wifi_event_sta_disconnected_t*) event_data;
        s_disconnect_reason = event->reason; 
        if (s_retry_num < MAX_FAILURES) {
            esp_wifi_connect();
            s_retry_num++;
            ESP_LOGI(TAG, "Спроба підключення до роутера... (%d/%d)", s_retry_num, MAX_FAILURES);
        } else {
            ESP_LOGW(TAG, "Не вдалося підключитися.");
            xEventGroupSetBits(s_wifi_event_group, WIFI_FAIL_BIT);
        }
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        // Устанавливаем флаг "подключено"
        xEventGroupSetBits(s_wifi_event_group, (WIFI_CONNECTED_BIT | IP_GOTADR_BIT)); 
        s_retry_num = 0;
        ip_event_got_ip_t* event = (ip_event_got_ip_t*) event_data;
        snprintf(IP_STA, sizeof(IP_STA), IPSTR,IP2STR(&event->ip_info.ip));
        ESP_LOGI(TAG, "Підключено! IP: %s", IP_STA);
    }
}

void start_softap_and_scan(void) {
    // --- 1. Сканирование в режиме STA-only ---
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_start());
    
    vTaskDelay(pdMS_TO_TICKS(500)); // ждём поднятия STA

    // Сканируем Wi-Fi
    networks_list = scan_wifi_networks();
    esp_wifi_stop();

    // --- 2. Поднимаем SoftAP ---
    esp_netif_create_default_wifi_ap();
    wifi_config_t ap_config = {
        .ap = {
            .ssid = "ESP32_Setup",
            .ssid_len = strlen("ESP32_Setup"),
            .password = "12345678", // можно пустой, если WPA2 не нужен
            .max_connection = 4,
            .authmode = WIFI_AUTH_WPA2_PSK
        }
    };

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA)); // AP+STA
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI("APP", "SoftAP started with SSID: %s", ap_config.ap.ssid);
}

esp_err_t connect_wifi_sta(void){
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_start());
    // 4. Очікування результату (Блокуючий виклик з таймаутом)
    EventBits_t bits = xEventGroupWaitBits(s_wifi_event_group,
        WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
        pdTRUE, pdFALSE, portMAX_DELAY);
    if (bits & WIFI_CONNECTED_BIT) {
        ESP_LOGI(TAG, "Connected to AP");
        return ESP_OK;
    } else if (bits & WIFI_FAIL_BIT) {
        ESP_LOGE(TAG, "Failed to connect. Cleaning up STA...");
        return ESP_FAIL;
    }
    else return ESP_FAIL;
}

/* обработчик нажатия кнопки */
static void IRAM_ATTR button_isr_handler(void* arg) {
    uint32_t gpio_num = (uint32_t) arg;
    xQueueSendFromISR(gpio_evt_queue, &gpio_num, NULL); 
}

static void button_task(void* arg) {
    uint32_t io_num;
    int64_t press_start_time = 0;
    bool is_pressed = false;
    bool reset_ready = false;
    int blink_cnt = 0;

    for(;;) {
        // 1. Швидка перевірка черги (майже не блокує)
        if (xQueueReceive(gpio_evt_queue, &io_num, pdMS_TO_TICKS(50))) {
            int level = gpio_get_level(BUTTON_PIN);
            if (level == 0) {
                is_pressed = true;
                reset_ready = false;
                press_start_time = esp_timer_get_time();
                gpio_set_level(LED_PIN, 1);
                ESP_LOGI("BTN", "Затиснуто");
            } else {
                if (is_pressed && reset_ready) {
                    gpio_set_level(LED_PIN, 0);
                    ESP_LOGW("BTN", "Скидання!");
                    esp_wifi_restore();
                    vTaskDelay(pdMS_TO_TICKS(500));
                    esp_restart();
                }
                is_pressed = false;
                reset_ready = false;
                gpio_set_level(LED_PIN, 0);
                ESP_LOGI("BTN", "Відпущено");
            }
        }

        // 2. Логіка блимання (виконується кожні 50 мс)
        if (is_pressed) {
            int64_t duration = (esp_timer_get_time() - press_start_time) / 1000;
            blink_cnt++;
            if (duration > 5000) {
                reset_ready = true;
                // Швидке блимання: кожні 200 мс (інвертуємо кожні 2 цикли по 100мс)
                gpio_set_level(LED_PIN, (duration / 200) % 2);
            }   
        }
        // 3. ФІКСОВАНА ПАУЗА (дає час іншим задачам і задає ритм блиманню)
        vTaskDelay(pdMS_TO_TICKS(50)); 
    }
}

void init_button(void) {
    gpio_config_t io_conf = {
        .intr_type = GPIO_INTR_ANYEDGE, // Ловимо обидва фронти
        .mode = GPIO_MODE_INPUT,
        .pin_bit_mask = (1ULL << BUTTON_PIN),
        .pull_up_en = GPIO_PULLUP_ENABLE,
    };
    gpio_config(&io_conf);

    gpio_evt_queue = xQueueCreate(10, sizeof(uint32_t));   
    xTaskCreate(button_task, "button_task", 3072, NULL, 10, NULL);  
    gpio_install_isr_service(0);
    gpio_isr_handler_add(BUTTON_PIN, button_isr_handler, (void*) BUTTON_PIN);
}

void init_led(void) {
    gpio_reset_pin(LED_PIN);
    gpio_set_direction(LED_PIN, GPIO_MODE_OUTPUT);
}

void app_main(void){
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND)
    {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);
    // 2. Запускаємо задачу кнопки (вона працює завжди в фоні)
    init_button();
    init_led();

    s_wifi_event_group = xEventGroupCreate();
    ESP_ERROR_CHECK(esp_netif_init());

    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    //ESP_ERROR_CHECK(esp_wifi_restore());  Для тестов

    esp_event_handler_instance_t instance_any_id;
    esp_event_handler_instance_t instance_got_ip;
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
        &event_handler, NULL, &instance_any_id));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
        &event_handler, NULL,&instance_got_ip));
        
        // Проверяем, была ли в NVS ранее сохранённая конфигурация
    wifi_config_t conf = {0};
    esp_wifi_get_config(WIFI_IF_STA, &conf);

    if (conf.sta.ssid[0] == 0) {
        // КЕЙС А: Налаштувань немає (або вони щойно стерті кнопкою)
        ESP_LOGI("APP", "Запуск режиму конфігурації (SoftAP)");
        start_softap_and_scan();
        vTaskDelay(pdMS_TO_TICKS(1000));        
        start_conf_webserver(); // Ваш сервер №1
    } else {
        // КЕЙС Б: Налаштування є — пробуємо підключитись
        ESP_LOGI("APP", "Спроба підключення до %s", conf.sta.ssid);
        if (connect_wifi_sta() == ESP_OK) {
            ESP_LOGI("APP", "Запуск у режимі STA");
            snprintf(sta_ssid, sizeof(sta_ssid), (char*)conf.sta.ssid);
            initi_web_page_buffer();
            start_main_webserver(); // Ваш сервер №2 (основний)
        } else {
            // Помилка підключення (пароль невірний або роутер вимкнено)
            ESP_LOGE("APP", "Не вдалося підключитись. Автозапуск SoftAP.");
        esp_wifi_stop(); // Зупиняємо спроби STA, щоб вони не заважали SoftAP
        s_retry_num = 0; // Скидаємо лічильник для майбутніх спроб
        start_softap_and_scan();
        vTaskDelay(pdMS_TO_TICKS(1000));
        start_conf_webserver();
        }
    }
}
