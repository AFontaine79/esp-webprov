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

#ifndef CAPTIVE_PORTAL_H_
#define CAPTIVE_PORTAL_H_

#include <esp_http_server.h>
#include <esp_netif.h>

typedef esp_err_t (*uri_handler_func_t)(httpd_req_t* req);

typedef struct captive_portal_config {
    /**
     * Handle for the network interface on which to operate the captive portal
     * (typically the softap interface).
     *
     * This parameter is used to:
     *   - Ignore DNS and HTTP requests originating from other interfaces
     *   - Know the IP address to return for DNS queries
     *   - Use the IP address in the Location header of the "302 Found" captive
     *     portal redirection HTTP response.
     *
     * @note The handle for the softap interface is returned by
     * esp_netif_create_default_wifi_ap().
     */
    esp_netif_t* netif_handle;

    /**
     * HTTP server on which to operate the captive portal. Captive portal will
     * take over GET requests on this server. The server must already be
     * started and running.
     */
    httpd_handle_t* httpd_handle;

    /**
     * The subdirectory to which redirects should point. This is the subdirectory
     * portion of the URL only and should not include the protocol or domain.
     *   - Correct:   "/config"
     *   - Incorrect: "192.168.4.1/config"
     *   - Incorrect: "http://192.168.4.1/config"
     */
    const char* redirect_uri;

    /**
     * The application's common GET handler function.
     *
     * esp_http_server lacks a mechanism to get an already registered URI handler.
     * However, it needs to take over the common GET handler and forward requests
     * only when the match the redirect URI.
     *
     * This parameter is used:
     *   - When forwarding HTTP requests that match the URI redirect string.
     *   - When captive portal is stopped, it reregisters this handler with the
     *     HTTP server.
     */
    uri_handler_func_t app_get_handler;

    /**
     * Context pointer to forward to application's common get handler.
     * This comes through the user_ctx field of the httpd_req_t object.
     */
    void* app_get_ctx;
} captive_portal_config_t;

/**
 * @brief   Starts the captive portal on the specified network interface and
 *          httpd server.
 *
 * @note Uses capt_dns module to set up captive DNS server.
 *
 * @note Takes over common GET handler of http server while portal is active.
 *
 * @param[in] config Configuration of the captive portal
 *
 * @return
 *  - ESP_OK				: Success
 *  - ESP_ERR_INVALID_ARG	: A required config setting was not provided
 *  - ESP_ERR_INVALID_STATE	: Captive portal is already started
 */
esp_err_t captive_portal_start(captive_portal_config_t* p_config);

/**
 * @brief   Stops the captive portal.
 */
void captive_portal_stop(void);

#endif /* CAPTIVE_PORTAL_H_ */
