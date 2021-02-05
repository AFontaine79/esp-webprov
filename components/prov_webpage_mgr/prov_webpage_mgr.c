// Copyright 2021 Aaron Fontaine
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "prov_webpage_mgr.h"

#include <cJSON.h>
#include <fcntl.h>

#include <esp_err.h>
#include <esp_event.h>
#include <esp_log.h>
#include <esp_timer.h>
#include <esp_vfs.h>

#include "captive_portal.h"

/* Is there a constant available for this in the esp/lwip headers somewhere? */
#define PROV_WEBPAGE_URI_MAX (64)

/**
 * This is the amount of time, after receiving the "shutdown prov" command from
 * the provisioning webpages to wait in shutdown stage 1. In this stage, the
 * captive portal is stopped but the soft AP is still active. This allows the
 * webpage to transition out of the captive portal and to the device homepage
 * while still on the device's network.s
 */
#define HANDOFF_DELAY_S (30)

#define WEBPROV_CHECK(a, str, goto_tag, ...)                                      \
    do {                                                                          \
        if (!(a)) {                                                               \
            ESP_LOGE(TAG, "%s(%d): " str, __FUNCTION__, __LINE__, ##__VA_ARGS__); \
            goto goto_tag;                                                        \
        }                                                                         \
    } while (0)

#define ACQUIRE_LOCK(mux) assert(xSemaphoreTake(mux, portMAX_DELAY) == pdTRUE)
#define RELEASE_LOCK(mux) assert(xSemaphoreGive(mux) == pdTRUE)

static const char* TAG = "prov-webpage-mgr";

static const char* WEBPROV_URI_PATH = "/prov";
static const char* CUSTOM_PROV_ENDPOINT = "prov-custom";

static char _homepage_uri[PROV_WEBPAGE_URI_MAX];

/* Mutex to lock/unlock access to provisioning singleton
 * context data. This is allocated only once on first init
 * and never deleted as prov_webpage_mgr is a singleton */
static SemaphoreHandle_t _webprov_ctx_lock = NULL;

/* Timer for reseting the wifi_prov_mgr after reasonable delay for responding to webpage JS */
static esp_timer_handle_t _wifi_prov_reset_timer = NULL;

/* Timers for shutting down the wifi_prov_mgr */
/* First stage shuts down captive portal only after reasonable delay for responding to JS.
 * The webprov mgr and soft AP stay active allowing the JavaScript to jump to the device
 * homepage.
 * Second stage completely shuts down prov manager and soft AP.
 * At this point, the device (phone/tablet/laptop) connected to the device should revert
 * back to its default network and be able to see the device on that network. The device
 * homepage should still work on the default network due to mDNS/netbios.
 */
static esp_timer_handle_t _wifi_prov_shutdown_stage1_timer = NULL;
static esp_timer_handle_t _wifi_prov_shutdown_stage2_timer = NULL;

/* It turns out we need to keep track of the HTTP server handle here as well since we
 * manually need to unregister the protocomm endpoint handler URIs after stopping the
 * wifi_prov_mgr service. This seems like an oversight in the wifi_provisioning component.
 */
static httpd_handle_t* _httpd_handle;

/* Event handler for catching system events */
static void wifi_prov_event_handler(void* arg, esp_event_base_t event_base, int event_id,
                                    void* event_data)
{
    if (event_base == WIFI_PROV_EVENT) {
        switch (event_id) {
        case WIFI_PROV_START:
            ESP_LOGI(TAG, "Provisioning started");
            break;
        case WIFI_PROV_CRED_RECV:
            {
                wifi_sta_config_t* wifi_sta_cfg = (wifi_sta_config_t*)event_data;
                ESP_LOGI(TAG,
                         "Received Wi-Fi credentials"
                         "\n\tSSID     : %s\n\tPassword : %s",
                         (const char*)wifi_sta_cfg->ssid, (const char*)wifi_sta_cfg->password);
                break;
            }
        case WIFI_PROV_CRED_FAIL:
            {
                wifi_prov_sta_fail_reason_t* reason = (wifi_prov_sta_fail_reason_t*)event_data;
                ESP_LOGE(TAG, "Provisioning failed!\n\tReason : %s",
                         (*reason == WIFI_PROV_STA_AUTH_ERROR)
                             ? "Wi-Fi station authentication failed"
                             : "Wi-Fi access-point not found");
                break;
            }
        case WIFI_PROV_CRED_SUCCESS:
            ESP_LOGI(TAG, "Provisioning successful");
            break;
        case WIFI_PROV_END:
            /* De-initialize manager once provisioning is finished */
            wifi_prov_mgr_deinit();

            /* Now that the service is deinitialized, unregister this event handler */
            esp_event_handler_unregister(WIFI_PROV_EVENT, ESP_EVENT_ANY_ID, &wifi_prov_event_handler);
            break;
        default:
            break;
        }
    }
}

/**
 * Timer callback to reset the WiFi Prov Manager back into a
 * state to accept scan and config commands.
 */
static void reset_wifi_prov_service(void* arg)
{
    ESP_LOGI(TAG, "Resetting the Wi-Fi provisioning service back to ready state.");
    wifi_prov_mgr_reset_to_ready_state();
}

static void shutdown_wifi_prov_service_stage1(void* arg)
{
    ESP_LOGI(TAG, "Webprov shutdown stage 1: captive portal only.");

    // Deactivate the captive portal
    // (Does nothing if not started)
    captive_portal_stop();

    // Wait HANDOFF_DELAY more seconds before shutting down wifi provisioning
    // and soft AP completely.
    esp_timer_start_once(_wifi_prov_shutdown_stage2_timer, HANDOFF_DELAY_S * 1000U * 1000U);
}

static void shutdown_wifi_prov_service_stage2(void* arg)
{
    ESP_LOGI(TAG, "Webprov shutdown stage 2: provisioning endpoints and soft AP.");
    prov_webpage_mgr_stop();
}

/* Handler for the optional provisioning endpoint "/prov-custom".
 * Custom commands use JSON.
 */
esp_err_t custom_prov_extensions_handler(uint32_t session_id, const uint8_t* inbuf, ssize_t inlen,
                                         uint8_t** outbuf, ssize_t* outlen, void* priv_data)
{
    cJSON* req_root = NULL;
    cJSON* resp_root = cJSON_CreateObject();
    char* status_str = "bad json";

    if (inbuf) {
        ESP_LOGI(TAG, "Received data: %.*s", inlen, (char*)inbuf);
    } else {
        goto custom_prov_exit;
    }

    req_root = cJSON_Parse((const char*)inbuf);
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

    if (strcmp(cmd_str, "reset prov") == 0) {
        /* This timer provides a slight delay before resetting wifi prov mgr
         * so that response can back out to webpage JavaScript first.
         */
        if (esp_timer_start_once(_wifi_prov_reset_timer, 500 * 1000U) == ESP_OK) {
            status_str = "ok";
        } else {
            status_str = "command failed";
        }
    } else if (strcmp(cmd_str, "shutdown prov") == 0) {
        /* This timer provides a slight delay before stopping captive portal. */
        if (esp_timer_start_once(_wifi_prov_shutdown_stage1_timer, 100 * 1000U) == ESP_OK) {
            status_str = "ok";
        } else {
            status_str = "command failed";
        }
    } else if (strcmp(cmd_str, "get homepage") == 0) {
        status_str = "ok";
        cJSON* uri_str = cJSON_CreateString(_homepage_uri);
        cJSON_AddItemToObject(resp_root, "uri", uri_str);
    } else {
        status_str = "bad command";
    }

custom_prov_exit:;
    cJSON* resp_item = cJSON_CreateString(status_str);
    cJSON_AddItemToObject(resp_root, "status", resp_item);
    *outbuf = (uint8_t*)cJSON_PrintUnformatted(resp_root);
    *outlen = strlen((const char*)*outbuf);

    if (req_root)
        cJSON_Delete(req_root);
    cJSON_Delete(resp_root);

    return ESP_OK;
}

esp_err_t create_timers(void)
{
    esp_timer_create_args_t wifi_prov_reset_timer_conf = {.callback = reset_wifi_prov_service,
                                                          .arg = NULL,
                                                          .dispatch_method = ESP_TIMER_TASK,
                                                          .name = "wifi_prov_restart_tm"};
    WEBPROV_CHECK(esp_timer_create(&wifi_prov_reset_timer_conf, &_wifi_prov_reset_timer) == ESP_OK,
                  "Failed to create wifi_prov_reset_timer", timer_create_err);

    esp_timer_create_args_t wifi_prov_shutdown_stage1_timer_conf = {
        .callback = shutdown_wifi_prov_service_stage1,
        .arg = NULL,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "wifi_prov_restart_tm"};
    WEBPROV_CHECK(esp_timer_create(&wifi_prov_shutdown_stage1_timer_conf,
                                   &_wifi_prov_shutdown_stage1_timer) == ESP_OK,
                  "Failed to create wifi_prov_shutdown_timer", timer_create_err);

    esp_timer_create_args_t wifi_prov_shutdown_stage2_timer_conf = {
        .callback = shutdown_wifi_prov_service_stage2,
        .arg = NULL,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "wifi_prov_restart_tm"};
    WEBPROV_CHECK(esp_timer_create(&wifi_prov_shutdown_stage2_timer_conf,
                                   &_wifi_prov_shutdown_stage2_timer) == ESP_OK,
                  "Failed to create wifi_prov_shutdown_timer", timer_create_err);

    return ESP_OK;

timer_create_err:
    return ESP_FAIL;
}

esp_err_t prov_webpage_mgr_start(const prov_webpage_mgr_config_t* p_config)
{
    if (!_webprov_ctx_lock) {
        /* Create mutex if this is the first time init is being called.
         * This is created only once and never deleted because if some
         * other thread is trying to take this mutex while it is being
         * deleted from another thread then the reference may become
         * invalid and cause exception */
        _webprov_ctx_lock = xSemaphoreCreateMutex();
        if (!_webprov_ctx_lock) {
            ESP_LOGE(TAG, "Failed to create mutex");
            return ESP_ERR_NO_MEM;
        }
    }

    /* Configuration for the provisioning manager */
    wifi_prov_mgr_config_t config = {
        .scheme = wifi_prov_scheme_softap,
        .scheme_event_handler = WIFI_PROV_EVENT_HANDLER_NONE,
        .app_event_handler = p_config->app_wifi_prov_event_handler,
    };

    /* Initialize provisioning manager with the
     * configuration parameters set above */
    WEBPROV_CHECK(wifi_prov_mgr_init(config) == ESP_OK, "Failed to init wifi_prov_mgr", err1);

    /* Pass the HTTP server handle from the RESTful server to the prov manager before it's started.
     */
    wifi_prov_scheme_softap_set_httpd_handle((void*)p_config->httpd_handle);

    /* Prevent auto-stop of prov mgr after successful connection.
     * This is instead managed directly by prov_webpage_mgr */
    wifi_prov_mgr_disable_auto_stop(200);

    WEBPROV_CHECK(esp_event_handler_register(WIFI_PROV_EVENT, ESP_EVENT_ANY_ID,
                                             &wifi_prov_event_handler, NULL) == ESP_OK,
                  "Failed to register event handler for WIFI_PROV events", err1);

    /* Endpoint for custom extensions to the provisioning manager */
    /* Endpoint must be created before starting service */
    wifi_prov_mgr_endpoint_create(CUSTOM_PROV_ENDPOINT);

    /* Start provisioning service */
    /* This will automatically use the same HTTP handle as the RESTful server due to the above call.
     */
    WEBPROV_CHECK(wifi_prov_mgr_start_provisioning(
                      p_config->wifi_prov_mgr_start_settings.security,
                      p_config->wifi_prov_mgr_start_settings.pop,
                      p_config->wifi_prov_mgr_start_settings.service_name,
                      p_config->wifi_prov_mgr_start_settings.service_key) == ESP_OK,
                  "Failed to start wifi_prov_mgr", err1);

    /* Endpoint for custom extensions to the provisioning manager */
    /* Handler must be registered after starting service */
    WEBPROV_CHECK(wifi_prov_mgr_endpoint_register(CUSTOM_PROV_ENDPOINT,
                                                  custom_prov_extensions_handler, NULL) == ESP_OK,
                  "Failed to register /prov-custom endpoint for custom wifi prov commands", err2);

    if (p_config->enable_captive_portal) {
        captive_portal_config_t cp_config = {
            .netif_handle = p_config->captive_portal_setup.netif_handle,
            .httpd_handle = p_config->httpd_handle,
            .redirect_uri = WEBPROV_URI_PATH,
            .app_get_handler = p_config->captive_portal_setup.app_get_handler,
            .app_get_ctx = p_config->captive_portal_setup.app_get_ctx};
        WEBPROV_CHECK(captive_portal_start(&cp_config) == ESP_OK,
                      "Failed to start DNS server for the captive portal", err2);
    }

    WEBPROV_CHECK(create_timers() == ESP_OK, "", err2);

    strlcpy(_homepage_uri, p_config->homepage_uri, sizeof(_homepage_uri));
    _httpd_handle = p_config->httpd_handle;

    return ESP_OK;

err2:
    wifi_prov_mgr_stop_provisioning();
err1:
    return ESP_FAIL;
}

void prov_webpage_mgr_stop(void)
{
    ESP_LOGI(TAG, "Stopping prov webpage manager");

    /*Deactivate the captive portal
     * (Does nothing if not started)
     */
    captive_portal_stop();

    /* Unregister our own endpoint as part of cleanup. This is necessary
     * since otherwise the URI handler sticks around in our HTTP server.
     */
    wifi_prov_mgr_endpoint_unregister(CUSTOM_PROV_ENDPOINT);

    /* Stop the provisioning service.
     * This also turns off the softAP interface.
     */
    wifi_prov_mgr_stop_provisioning();

    /* When providing an external HTTP server to wifi_prov_mgr, it will not
     * automatically unregister its own URI handlers from our server when
     * the service is stopped.
     * If we then try to start the service again with the same HTTP server,
     * ESP-IDF will assert/abort when it tries to register for an already
     * existing handler.
     * Therefore, we need to manually unregister wifi_prov_mgr's handlers
     * after stopping the service.
     * This is an ENCAPSULATION BREAKING workaround for an oversight in
     * ESP-IDF.
     */
    httpd_unregister_uri_handler(*_httpd_handle, "/proto-ver", HTTP_POST);
    httpd_unregister_uri_handler(*_httpd_handle, "/prov-session", HTTP_POST);
    httpd_unregister_uri_handler(*_httpd_handle, "/prov-config", HTTP_POST);
    httpd_unregister_uri_handler(*_httpd_handle, "/prov-scan", HTTP_POST);

    /* Free and delete the timer instances. */
    if (_wifi_prov_reset_timer) {
        esp_timer_stop(_wifi_prov_reset_timer);
        esp_timer_delete(_wifi_prov_reset_timer);
        _wifi_prov_reset_timer = NULL;
    }
    if (_wifi_prov_shutdown_stage1_timer) {
        esp_timer_stop(_wifi_prov_shutdown_stage1_timer);
        esp_timer_delete(_wifi_prov_shutdown_stage1_timer);
        _wifi_prov_shutdown_stage1_timer = NULL;
    }
    if (_wifi_prov_shutdown_stage2_timer) {
        esp_timer_stop(_wifi_prov_shutdown_stage2_timer);
        esp_timer_delete(_wifi_prov_shutdown_stage2_timer);
        _wifi_prov_shutdown_stage2_timer = NULL;
    }
}
