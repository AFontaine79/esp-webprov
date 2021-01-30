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

#include "captive_portal.h"

#include "lwip/sockets.h"
#include <esp_err.h>
#include <esp_log.h>
#include <stdbool.h>

#include "capt_dns.h"

/* Is there a constant available for this in the esp/lwip headers somewhere? */
#define PROV_WEBPAGE_URI_MAX (64)

static const char* TAG = "captive-portal";

/* Future enhancement should be to allow choice of http or https for scheme */
static const char* WEBPROV_URI_SCHEME = "http://";

static bool _is_started = false;

static char _portal_redirect_uri[PROV_WEBPAGE_URI_MAX];
static char _portal_redirect_full_url[PROV_WEBPAGE_URI_MAX];

static httpd_handle_t* _httpd_handle;
static uri_handler_func_t _app_get_handler;
static void* _app_get_ctx;

static esp_err_t captive_portal_common_get_handler(httpd_req_t* req)
{
    // Note: URIs coming in through httpd_req_t only contain the subdirectory/path
    //   portion of the URL. However, when responding with a 302, we should provide
    //   the complete URL.
    if (strncmp(req->uri, _portal_redirect_uri, strlen(_portal_redirect_uri)) == 0) {
        // Requested page matches the redirection URI.
        // Forward to application's GET handler for normal webpage handling.
        return _app_get_handler(req);
    } else {
        // Requested page does not match the redirection URI.
        // Send a 302 response with the full URL in the Location header.
        httpd_resp_set_status(req, "302 Found");
        httpd_resp_set_type(req, "text/plain");
        httpd_resp_set_hdr(req, "Location", _portal_redirect_full_url);
        httpd_resp_set_hdr(req, "Connection", "close");
        httpd_resp_send(req, NULL, 0);
        return ESP_OK;
    }
}

static esp_err_t build_full_portal_redirect_url(esp_netif_t* softap_if_handle)
{
    esp_netif_ip_info_t ip_info;
    esp_err_t ret;

    ret = esp_netif_get_ip_info(softap_if_handle, &ip_info);
    if (ret != ESP_OK)
        return ret;

    memset(_portal_redirect_full_url, 0, sizeof(_portal_redirect_full_url));

    strcpy(_portal_redirect_full_url, WEBPROV_URI_SCHEME);

    char* dotted_ip_addr_str = inet_ntoa(ip_info.ip);
    strlcat(_portal_redirect_full_url, dotted_ip_addr_str, sizeof(_portal_redirect_full_url));

    strlcat(_portal_redirect_full_url, _portal_redirect_uri, sizeof(_portal_redirect_full_url));

    return ESP_OK;
}

esp_err_t captive_portal_start(captive_portal_config_t* p_config)
{
    esp_err_t ret;

    if (_is_started) {
        return ESP_ERR_INVALID_STATE;
    }

    if (!p_config) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!p_config->netif_handle) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!p_config->httpd_handle) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!p_config->redirect_uri) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!p_config->app_get_handler) {
        return ESP_ERR_INVALID_ARG;
    }

    /* Start the DNS server to redirect DNS queries to this device. */
    ret = capt_dns_start(p_config->netif_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start captive dns server");
        return ret;
    }

    strlcpy(_portal_redirect_uri, p_config->redirect_uri, sizeof(_portal_redirect_uri));

    /* Build the full redirection URL for 302 response */
    ret = build_full_portal_redirect_url(p_config->netif_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to build redirection URL");
        capt_dns_stop();
        return ret;
    }

    /* Take over the common GET handler on the HTTP server */
    _app_get_handler = p_config->app_get_handler;

    ret = httpd_unregister_uri_handler(*(p_config->httpd_handle), "/*", HTTP_GET);
    if ((ret != ESP_OK) && (ret != ESP_ERR_NOT_FOUND)) {
        ESP_LOGE(TAG, "Failed to unregister application's GET handler for /*.");
        capt_dns_stop();
        return ret;
    }

    httpd_uri_t common_get_uri = {.uri = "/*",
                                  .method = HTTP_GET,
                                  .handler = captive_portal_common_get_handler,
                                  .user_ctx = p_config->app_get_ctx};
    ret = httpd_register_uri_handler(*(p_config->httpd_handle), &common_get_uri);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to register captive portal's GET handler for /*");
        capt_dns_stop();
        return ret;
    }

    /* Save additional parameters needed when stopping captive portal */
    _httpd_handle = p_config->httpd_handle;
    _app_get_ctx = p_config->app_get_ctx;

    _is_started = true;
    return ESP_OK;
}

void captive_portal_stop(void)
{
    if (!_is_started) {
        return;
    }

    capt_dns_stop();

    /* Unregister captive portal's common GET handler */
    httpd_unregister_uri_handler(*_httpd_handle, "/*", HTTP_GET);

    /* Restore application's common GET handler */
    httpd_uri_t common_get_uri = {
        .uri = "/*", .method = HTTP_GET, .handler = _app_get_handler, .user_ctx = _app_get_ctx};
    httpd_register_uri_handler(*_httpd_handle, &common_get_uri);
}
