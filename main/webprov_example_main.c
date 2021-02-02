/* Provisioning Webpage API Example

   This example code is in the Public Domain (or CC0 licensed, at your option.)

   Unless required by applicable law or agreed to in writing, this
   software is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
   CONDITIONS OF ANY KIND, either express or implied.
*/

#include <stdio.h>
#include <string.h>

#include <freertos/FreeRTOS.h>
#include <freertos/event_groups.h>
#include <freertos/task.h>
#include <esp_timer.h>
#include <driver/gpio.h>

#include <cJSON.h>

#include "driver/gpio.h"
#include "esp_event.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_spiffs.h"
#include "esp_vfs_fat.h"
#include "esp_vfs_semihost.h"
#include "lwip/apps/netbiosns.h"
#include "mdns.h"
#include "nvs_flash.h"
#include "sdkconfig.h"
#include "sdmmc_cmd.h"
#include <esp_wifi.h>
#if CONFIG_EXAMPLE_WEB_DEPLOY_SD
#include "driver/sdmmc_host.h"
#endif

#include "prov_webpage_mgr.h"
#include "rest_server.h"

#define MDNS_INSTANCE "provisioning webpage server"

#define BUTTON_GPIO		GPIO_NUM_0

static const char* TAG = "app";

static esp_netif_t* _station_if_handle;
static esp_netif_t* _softap_if_handle;

/* Signal Wi-Fi events on this event-group */
const int WIFI_CONNECTED_EVENT = BIT0;
static EventGroupHandle_t _wifi_event_group;

static esp_timer_handle_t _wifi_reset_timer = NULL;

/* Event handler for catching system events */
static void event_handler(void* arg, esp_event_base_t event_base, int event_id, void* event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t* event = (ip_event_got_ip_t*)event_data;
        ESP_LOGI(TAG, "Connected with IP Address:" IPSTR, IP2STR(&event->ip_info.ip));
        /* Signal main application to continue execution */
        xEventGroupSetBits(_wifi_event_group, WIFI_CONNECTED_EVENT);
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        ESP_LOGI(TAG, "Disconnected. Connecting to the AP again...");
        esp_wifi_connect();
    }
}

static esp_err_t rest_web_api_handler(httpd_req_t* req)
{
    char content[64];

    /* Truncate if content length larger than the buffer */
    size_t recv_size = MIN(req->content_len, sizeof(content));

    int ret = httpd_req_recv(req, content, recv_size);
    if (ret <= 0) {  /* 0 return value indicates connection closed */
        /* Check if timeout occurred */
        if (ret == HTTPD_SOCK_ERR_TIMEOUT) {
            /* In case of timeout one can choose to retry calling
             * httpd_req_recv(), but to keep it simple, here we
             * respond with an HTTP 408 (Request Timeout) error */
            httpd_resp_send_408(req);
        }
        /* In case of error, returning ESP_FAIL will
         * ensure that the underlying socket is closed */
        return ESP_FAIL;
    }

    cJSON* req_root = NULL;
    cJSON* resp_root = cJSON_CreateObject();
    char* status_str = "bad json";

    if (recv_size > 0) {
        ESP_LOGD(TAG, "Received data: %.*s", recv_size, content);
    } else {
        goto custom_prov_exit;
    }

    req_root = cJSON_Parse((const char*)content);
    if (!req_root) {
        goto custom_prov_exit;
    }

    cJSON* command = cJSON_GetObjectItem(req_root, "command");
    if (!command) {
        goto custom_prov_exit;
    }

    char* cmd_str = cJSON_GetStringValue(command);
    if (!cmd_str) {
        goto custom_prov_exit;
    }

    if (strcmp(cmd_str, "get system uptime") == 0) {
    	int32_t sys_uptime_s = (int32_t)(esp_timer_get_time() / (1000U * 1000U));
    	char sys_uptime_str[16];
        sprintf(sys_uptime_str, "%d s", sys_uptime_s);
        cJSON* sys_uptime_json_str_obj = cJSON_CreateString(sys_uptime_str);
        cJSON_AddItemToObject(resp_root, "uptime", sys_uptime_json_str_obj);
        status_str = "ok";
    } else if (strcmp(cmd_str, "get button state") == 0) {
    	int button_level = gpio_get_level(BUTTON_GPIO);
    	char *button_state_str;
    	if (button_level) {
    		button_state_str = "up";
    	} else {
    		button_state_str = "down";
    	}
        cJSON* button_state_json_str_obj = cJSON_CreateString(button_state_str);
        cJSON_AddItemToObject(resp_root, "button", button_state_json_str_obj);
        status_str = "ok";
    } else if (strcmp(cmd_str, "clear wifi settings") == 0) {
    	/* Halt Wi-Fi, clear settings, and reset device three seconds from now. */
        if (esp_timer_start_once(_wifi_reset_timer, 3000 * 1000U) == ESP_OK) {
            status_str = "ok";
        } else {
            status_str = "command failed";
        }
    } else if (strcmp(cmd_str, "reset") == 0) {
		status_str = "command failed";
    } else {
        status_str = "bad command";
    }

custom_prov_exit:;
    char *resp_str;
    size_t resp_len;

    cJSON* resp_item = cJSON_CreateString(status_str);
    cJSON_AddItemToObject(resp_root, "status", resp_item);

    resp_str = (char*)cJSON_PrintUnformatted(resp_root);
    resp_len = strlen((const char*)resp_str);

    if (req_root)
        cJSON_Delete(req_root);
    cJSON_Delete(resp_root);

    /* Uncomment the following line if testing webpages from localhost on
     * your development machine. E.g. you are hosting the /dist folder
     * locally at localhost:5000 and still expect cross-domain web-api
     * requests to the device to work.
     */
//    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");

    ESP_ERROR_CHECK(httpd_resp_send(req, resp_str, resp_len));

    free(resp_str);

    return ESP_OK;
}

static void clear_wifi_settings_and_restart(void* arg)
{
	/* Stop Wi-Fi operations before clearing settings */
	esp_wifi_stop();

    wifi_config_t wifi_cfg_empty;
    memset(&wifi_cfg_empty, 0, sizeof(wifi_config_t));

    esp_wifi_set_storage(WIFI_STORAGE_FLASH);
    esp_wifi_set_config(ESP_IF_WIFI_STA, &wifi_cfg_empty);

    esp_restart();
}

static void get_device_service_name(char* service_name, size_t max)
{
    uint8_t eth_mac[6];
    const char* ssid_prefix = CONFIG_EXAMPLE_SOFTAP_SSID_PREFIX;
    esp_wifi_get_mac(WIFI_IF_STA, eth_mac);
    snprintf(service_name, max, "%s%02X%02X%02X", ssid_prefix, eth_mac[3], eth_mac[4], eth_mac[5]);
}

static void get_homepage_uri(char* homepage_uri, size_t max)
{
    strlcpy(homepage_uri, "http://", max);
    strlcat(homepage_uri, CONFIG_EXAMPLE_MDNS_HOST_NAME, max);
    strlcat(homepage_uri, ".local", max);
}

static void wifi_init_sta(void)
{
    /* Start Wi-Fi in station mode */
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_start());
}

/**
 * Copy/paste of wifi_prov_mgr_is_provisioned function except with
 * unnecessary check of manager initialization removed.
 */
esp_err_t wifi_is_provisioned(bool* provisioned)
{
    *provisioned = false;

    /* Get Wi-Fi Station configuration */
    wifi_config_t wifi_cfg;
    if (esp_wifi_get_config(ESP_IF_WIFI_STA, &wifi_cfg) != ESP_OK) {
        return ESP_FAIL;
    }

    if (strlen((const char*)wifi_cfg.sta.ssid)) {
        *provisioned = true;
    }
    return ESP_OK;
}

static void initialise_mdns(void)
{
    mdns_init();
    mdns_hostname_set(CONFIG_EXAMPLE_MDNS_HOST_NAME);
    mdns_instance_name_set(MDNS_INSTANCE);

    mdns_txt_item_t serviceTxtData[] = {{"board", "esp32"}, {"path", "/"}};

    ESP_ERROR_CHECK(mdns_service_add("ESP32-WebServer", "_http", "_tcp", 80, serviceTxtData,
                                     sizeof(serviceTxtData) / sizeof(serviceTxtData[0])));
}

#if CONFIG_EXAMPLE_WEB_DEPLOY_SEMIHOST
esp_err_t init_fs(void)
{
    //    esp_err_t ret =
    //    esp_vfs_semihost_register(CONFIG_EXAMPLE_WEB_MOUNT_POINT,
    //    CONFIG_EXAMPLE_HOST_PATH_TO_MOUNT);
    esp_err_t ret = esp_vfs_semihost_register(CONFIG_EXAMPLE_WEB_MOUNT_POINT, NULL);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to register semihost driver (%s)!", esp_err_to_name(ret));
        return ESP_FAIL;
    }
    return ESP_OK;
}
#endif

#if CONFIG_EXAMPLE_WEB_DEPLOY_SD
esp_err_t init_fs(void)
{
    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    sdmmc_slot_config_t slot_config = SDMMC_SLOT_CONFIG_DEFAULT();

    gpio_set_pull_mode(15, GPIO_PULLUP_ONLY); // CMD
    gpio_set_pull_mode(2, GPIO_PULLUP_ONLY);  // D0
    gpio_set_pull_mode(4, GPIO_PULLUP_ONLY);  // D1
    gpio_set_pull_mode(12, GPIO_PULLUP_ONLY); // D2
    gpio_set_pull_mode(13, GPIO_PULLUP_ONLY); // D3

    esp_vfs_fat_sdmmc_mount_config_t mount_config = {
        .format_if_mount_failed = true, .max_files = 4, .allocation_unit_size = 16 * 1024};

    sdmmc_card_t* card;
    esp_err_t ret = esp_vfs_fat_sdmmc_mount(CONFIG_EXAMPLE_WEB_MOUNT_POINT, &host, &slot_config,
                                            &mount_config, &card);
    if (ret != ESP_OK) {
        if (ret == ESP_FAIL) {
            ESP_LOGE(TAG, "Failed to mount filesystem.");
        } else {
            ESP_LOGE(TAG, "Failed to initialize the card (%s)", esp_err_to_name(ret));
        }
        return ESP_FAIL;
    }
    /* print card info if mount successfully */
    sdmmc_card_print_info(stdout, card);
    return ESP_OK;
}
#endif

#if CONFIG_EXAMPLE_WEB_DEPLOY_SF
esp_err_t init_fs(void)
{
    esp_vfs_spiffs_conf_t conf = {.base_path = CONFIG_EXAMPLE_WEB_MOUNT_POINT,
                                  .partition_label = NULL,
                                  .max_files = 5,
                                  .format_if_mount_failed = false};
    esp_err_t ret = esp_vfs_spiffs_register(&conf);

    if (ret != ESP_OK) {
        if (ret == ESP_FAIL) {
            ESP_LOGE(TAG, "Failed to mount or format filesystem");
        } else if (ret == ESP_ERR_NOT_FOUND) {
            ESP_LOGE(TAG, "Failed to find SPIFFS partition");
        } else {
            ESP_LOGE(TAG, "Failed to initialize SPIFFS (%s)", esp_err_to_name(ret));
        }
        return ESP_FAIL;
    }

    size_t total = 0, used = 0;
    ret = esp_spiffs_info(NULL, &total, &used);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to get SPIFFS partition information (%s)", esp_err_to_name(ret));
    } else {
        ESP_LOGI(TAG, "Partition size: total: %d, used: %d", total, used);
    }
    return ESP_OK;
}
#endif

void button_init(void)
{
    gpio_config_t gpioConfig = {
        .pin_bit_mask = (1ULL << BUTTON_GPIO),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&gpioConfig);
}

void app_main(void)
{
    /* Initialize NVS partition */
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        /* NVS partition was truncated
         * and needs to be erased */
        ESP_ERROR_CHECK(nvs_flash_erase());

        /* Retry nvs_flash_init */
        ESP_ERROR_CHECK(nvs_flash_init());
    }

    /* Initialize TCP/IP */
    ESP_ERROR_CHECK(esp_netif_init());

    /* Initialize button input GPIO */
    button_init();

    /* This timer exists to provide a slight delay after accepting
     * the Clear Wi-Fi Settings command.
     */
    esp_timer_create_args_t wifi_reset_timer_confg = {
        .callback = clear_wifi_settings_and_restart,
        .arg = NULL,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "wifi_reset_tm"};
    ESP_ERROR_CHECK(esp_timer_create(&wifi_reset_timer_confg, &_wifi_reset_timer));

    /* Initialize the default event loop */
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    /* Create event flags to signal success/fail of the provisioning process. */
    _wifi_event_group = xEventGroupCreate();

    _station_if_handle = esp_netif_create_default_wifi_sta();
    _softap_if_handle = esp_netif_create_default_wifi_ap();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    /* Set regulatory domain to FCC.
     * Operating in the U.S.  Allowed channels are 1 through 11.
     * Note that this has no effect on the channels scanned by the scan
     * operation of the wifi_scan endpoint of wifi_provisioning.
     * It scans all 14 channels anyway. */
    wifi_country_t regConfig = {
        .cc = "USA",
        .schan = 1,
        .nchan = 11,
        .policy = WIFI_COUNTRY_POLICY_MANUAL,
        .max_tx_power = 0,
    };
    ESP_ERROR_CHECK(esp_wifi_set_country(&regConfig));

    /* Initialize mDNS and NetBios to make the device discoverable via its
     * host-name. */
    initialise_mdns();
    netbiosns_init();
    netbiosns_set_name(CONFIG_EXAMPLE_MDNS_HOST_NAME);

    /* Initialize filesystem specified in menuconfig: SPI Flash, SD Card, or
     * Semihost */
    ESP_ERROR_CHECK(init_fs());

    /* Start the web server, telling it where the web files are mounted. */
    ESP_ERROR_CHECK(rest_server_start(CONFIG_EXAMPLE_WEB_MOUNT_POINT));

    /* URI for handling commands from web pages */
    httpd_uri_t web_api_uri = {.uri = "/web-api",
                               .method = HTTP_POST,
                               .handler = rest_web_api_handler,
                               .user_ctx = NULL};
    httpd_register_uri_handler(*(rest_server_get_httpd_handle()), &web_api_uri);

    /* Let's find out if the device is provisioned */
    bool provisioned = false;
    ESP_ERROR_CHECK(wifi_is_provisioned(&provisioned));

    /* If device is not yet provisioned start provisioning service */
    if (!provisioned) {
        ESP_LOGI(TAG, "Starting provisioning webpage manager");

        /* Register custom event handlers. */
        ESP_ERROR_CHECK(
            esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &event_handler, NULL));

        /* Get strings needed for prov_webpage_mgr configuration */
        char service_name[12];
        get_device_service_name(service_name, sizeof(service_name));
        char homepage_uri[32];
        get_homepage_uri(homepage_uri, sizeof(homepage_uri));

        /* Configure and start prov_webpage_mgr. */
        prov_webpage_mgr_config_t webprov_config = {
            .httpd_handle = rest_server_get_httpd_handle(),
            .homepage_uri = homepage_uri,
            .app_wifi_prov_event_handler =
                {
                    .event_cb = NULL,
                    .user_data = NULL,
                },
            .wifi_prov_mgr_start_settings =
                {
                    .security = WIFI_PROV_SECURITY_0,
                    .pop = NULL,
                    .service_name = service_name,
                    .service_key = CONFIG_EXAMPLE_WIFI_PASSWORD,
                },
            .enable_captive_portal = true,
            .captive_portal_setup =
                {
                    .netif_handle = _softap_if_handle,
                    .app_get_handler = rest_server_get_common_get_handler(),
                    .app_get_ctx = rest_server_get_common_get_ctx(),
                },
        };
        ESP_ERROR_CHECK(prov_webpage_mgr_start(&webprov_config));
    } else {
        ESP_LOGI(TAG, "Already provisioned, starting Wi-Fi STA");

        /* Register custom event handlers. */
        ESP_ERROR_CHECK(
            esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &event_handler, NULL));
        ESP_ERROR_CHECK(
            esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &event_handler, NULL));

        /* Start Wi-Fi station */
        wifi_init_sta();
    }

    /* Wait for Wi-Fi connection */
    xEventGroupWaitBits(_wifi_event_group, WIFI_CONNECTED_EVENT, false, true, portMAX_DELAY);
    ESP_LOGI(TAG, "Device is provisioned and connected.");
}
