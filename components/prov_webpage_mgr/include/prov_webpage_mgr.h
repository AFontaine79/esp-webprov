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

#ifndef PROVISIONING_WEBPAGE_MANAGER_H_
#define PROVISIONING_WEBPAGE_MANAGER_H_

#include <stdbool.h>

#include <esp_http_server.h>
#include <esp_netif.h>
#include <wifi_provisioning/manager.h>
#include <wifi_provisioning/scheme_softap.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief   Settings that will be forwarded to wifi_prov_mgr_start_provisioning.
 */
typedef struct {
    /**
     * Specify which protocomm security scheme to use.
     */
    wifi_prov_security_t security;

    /**
     * Pointer to proof of possession string (NULL if not needed).
     */
    const char* pop;

    /**
     * Wi-Fi SSID for soft AP.
     */
    const char* service_name;

    /**
     * Wi-Fi password for soft AP (NULL for open network).
     */
    const char* service_key;
} wifi_prov_mgr_start_settings_t;

/**
 * @brief   Settings that will be forwarded to captive_portal_start.
 */
typedef struct {
    /**
     * Network interface on which to operate captive portal
     */
    esp_netif_t* netif_handle;

    /**
     * Application's common GET handler
     */
    esp_err_t (*app_get_handler)(httpd_req_t* req);

    /**
     * Private context structure to forward to app's GET handler
     */
    void* app_get_ctx;
} captive_portal_settings_t;

/**
 * @brief   Structure for specifying the manager configuration
 */
typedef struct {
    /**
     * HTTP server on which to set up the protocomm provisioning endpoints.
     * Webserver must already be running on this handle with common GET
     * handler for all URI requests under "/". Provisioning webpages must
     * be mounted under "/prov".
     */
    httpd_handle_t* httpd_handle;

    /**
     * URI to which the webpage prov manager will jump after successful
     * connection to an AP. This will exit captive portal while doing so.
     * If set to NULL, webpage prov manager will remain at Connection
     * Success webpage after successful connection.
     */
    const char* homepage_uri;

    /**
     * Event handler that will be forwarded to wifi_prov_mgr for catching
     * events of type WIFI_PROV_EVENT. Set to WIFI_PROV_EVENT_HANDLER_NONE
     * if not used.
     */
    wifi_prov_event_handler_t app_wifi_prov_event_handler;

    /**
     * Settings to forward to the wifi_provisioning manager.
     */
    wifi_prov_mgr_start_settings_t wifi_prov_mgr_start_settings;

    /**
     * Specifies whether captive portal should be activated in addition
     * to starting soft AP.
     */
    bool enable_captive_portal;

    /**
     * Configuration for the captive portal, if used. This does not contain
     * all of the settings forwarded to captive_portal_start().
     * The httpd_handle will be the same as provided above and redirect_uri
     * is statically assigned to "/prov".
     */
    captive_portal_settings_t captive_portal_setup;
} prov_webpage_mgr_config_t;

/**
 * @brief   Initialize provisioning webpage manager
 *
 * Configures the manager and allocates internal resources
 *
 * Starts wifi_prov_mgr in soft AP mode.
 *
 * If captive portal is enabled, starts DNS server and enables 302
 * redirection to "/prov".
 *
 * Will cause WIFI_PROV_INIT and WIFI_PROV_START to be emitted.
 *
 * @warning The default network interfaces for AP and STA modes must
 * already be created.
 *
 * @warning esp_wifi_init() must already be called.
 *
 * @param[in] config Configuration structure
 *
 * @return
 *  - ESP_OK      : Success
 *  - ESP_FAIL    : Fail
 */
esp_err_t prov_webpage_mgr_start(const prov_webpage_mgr_config_t* p_config);

/**
 * @brief   Stops the provisioning webpage manager
 *
 * If captive portal is active, it is deactivated.
 *
 * Stops wifi_prov_mgr and disables soft AP interface.
 *
 * Will cause WIFI_PROV_END and WIFI_PROV_DEINIT to be emitted.
 */
void prov_webpage_mgr_stop(void);

#ifdef __cplusplus
}
#endif
#endif /* PROVISIONING_WEBPAGE_MANAGER_H_ */
