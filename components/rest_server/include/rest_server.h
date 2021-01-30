/* Restful server for Provisioning Webpage API example
 *
 * Based on ESP-IDF example project:
 * HTTP Restful API Server
 *
 * This example code is in the Public Domain (or CC0 licensed, at your option.)
 *
 * Unless required by applicable law or agreed to in writing, this
 * software is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
 * CONDITIONS OF ANY KIND, either express or implied.
 */

#ifndef REST_SERVER_H_
#define REST_SERVER_H_

#include <esp_http_server.h>

typedef esp_err_t (*uri_handler_func_t)(httpd_req_t* req);

/**
 * @brief   Creates an HTTP server instance and registers a common GET
 *          handler to serve web files.
 *
 * Matches incoming URI requests to the filesystem starting at the mount
 * point specified by base_path. If URI specifies directory instead of
 * file, "index.html" is appended to the requesting before responding.
 *
 * @warning Web files must already be mounted and available.
 *
 * @param[in] base_path Mount point in file system of web files. This
 *   should contain the contents of the /dist folder of the webpage
 *   build output. It is up to the application as to how this is
 *   accomplished.
 *
 * @return
 *  - ESP_OK      : Success
 *  - ESP_FAIL    : Fail
 */
esp_err_t rest_server_start(const char* base_path);

/**
 * @brief   Stops and removes the HTTP server instance created by
 *          rest_server_start().
 *
 * Has no effect if not started.
 */
void rest_server_stop(void);

/**
 * @brief   Gets the HTTP server handle.
 *
 * Use this if it is necessary to attach additional URI handlers.
 *
 * @note This returns a pointer to the handle, not the handle itself.
 *
 * This value can be passed directly as is to
 * wifi_prov_scheme_softap_set_httpd_handle().
 *
 * If used with the httpd_ API, it should be dereferenced first.
 *
 * @return
 *  - Pointer to HTTP handle if started.
 *  - NULL if not started.
 */
httpd_handle_t* rest_server_get_httpd_handle(void);

/**
 * @brief   Helper function to return a function pointer for the
 *          RESTful server's common GET handler.
 *
 * @note This helper function is needed for setting up captive portal.
 *
 * @return
 *  - Pointer to common GET function.
 */
uri_handler_func_t rest_server_get_common_get_handler(void);

/**
 * @brief   Helper function to return the opaque context structure
 *          reference that is registered with the common GET URI handler.
 *
 * @note This helper function is needed for setting up captive portal.
 *
 * @return
 *  - Context reference
 */
void* rest_server_get_common_get_ctx(void);

#endif /* REST_SERVER_H_ */
