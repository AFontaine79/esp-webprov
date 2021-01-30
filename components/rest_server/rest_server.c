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
#include <fcntl.h>
#include <stdbool.h>
#include <string.h>

#include "rest_server.h"

#include "esp_err.h"
#include "esp_log.h"
#include "esp_vfs.h"

static const char* TAG = "rest-server";

#define REST_CHECK(a, str, goto_tag, ...)                                         \
    do {                                                                          \
        if (!(a)) {                                                               \
            ESP_LOGE(TAG, "%s(%d): " str, __FUNCTION__, __LINE__, ##__VA_ARGS__); \
            goto goto_tag;                                                        \
        }                                                                         \
    } while (0)

#define FILE_PATH_MAX   (ESP_VFS_PATH_MAX + 128)
#define SCRATCH_BUFSIZE (10240)

typedef struct rest_server_context {
    char base_path[ESP_VFS_PATH_MAX + 1];
    char scratch[SCRATCH_BUFSIZE];
} rest_server_context_t;

#define CHECK_FILE_EXTENSION(filename, ext) \
    (strcasecmp(&filename[strlen(filename) - strlen(ext)], ext) == 0)

/* Save the server handle here */
static rest_server_context_t* _rest_context = NULL;
static httpd_handle_t _server_handle = NULL;

typedef struct file_ext_to_mimetype {
    char* file_ext;
    char* mimetype;
    bool is_zipped;
} file_ext_to_mimetype_t;

file_ext_to_mimetype_t file_ext_mappings[] = {
    {".html", "text/html", true}, {".js", "application/javascript", true},
    {".css", "text/css", true},   {".proto", "text/plain", true},
    {".png", "image/png", false}, {".ico", "image/x-icon", false},
    {".svg", "text/xml", true},   {".txt", "text/plain", true},
};

#define NUM_FILE_TYPES (sizeof(file_ext_mappings) / sizeof(struct file_ext_to_mimetype))

/* Set HTTP response content type according to file extension */
static esp_err_t set_content_type_from_file(httpd_req_t* req, char* filepath,
                                            size_t filepath_max_len)
{
    const char* type = "text/plain";
#if CONFIG_EXAMPLE_MINIFY_AND_GZIP_WEBPAGES
    bool is_zipped = false;
#endif

    int idx;
    for (idx = 0; idx < NUM_FILE_TYPES; idx++) {
        if (CHECK_FILE_EXTENSION(filepath, file_ext_mappings[idx].file_ext)) {
            type = file_ext_mappings[idx].mimetype;
#if CONFIG_EXAMPLE_MINIFY_AND_GZIP_WEBPAGES
            is_zipped = file_ext_mappings[idx].is_zipped;
#endif
            break;
        }
    }

#if CONFIG_EXAMPLE_MINIFY_AND_GZIP_WEBPAGES
    if (is_zipped) {
        strlcat(filepath, ".gz", filepath_max_len);
        httpd_resp_set_hdr(req, "Content-Encoding", "gzip");
    }
#endif
    return httpd_resp_set_type(req, type);
}

static bool uri_is_file(httpd_req_t* req)
{
    // Check ending of URI string against all known file extensions.
    // Return true if match is found. False otherwise.
    int idx;
    for (idx = 0; idx < NUM_FILE_TYPES; idx++) {
        if (CHECK_FILE_EXTENSION(req->uri, file_ext_mappings[idx].file_ext)) {
            return true;
        }
    }
    return false;
}

/* Send HTTP response with the contents of the requested file */
static esp_err_t rest_common_get_handler(httpd_req_t* req)
{
    char filepath[FILE_PATH_MAX];

    strlcpy(filepath, _rest_context->base_path, sizeof(filepath));
    strlcat(filepath, req->uri, sizeof(filepath));

    if (req->uri[strlen(req->uri) - 1] == '/') {
        // URI already ends with "/", indicating a directory.
        // Thus, we know we need to append "index.html" for file lookup.
        strlcat(filepath, "index.html", sizeof(filepath));
    } else {
        if (!uri_is_file(req)) {
            // The URI is not a file but also does not end with "/".
            // Thus, we *assume* it is specifying a directory.
            strlcat(filepath, "/index.html", sizeof(filepath));
        }
    }

    set_content_type_from_file(req, filepath, sizeof(filepath));

    ESP_LOGI(TAG, "Request URI: %s", req->uri);
    ESP_LOGI(TAG, "Corresponding filepath: %s", filepath);

    int fd = open(filepath, O_RDONLY, 0);
    if (fd == -1) {
        ESP_LOGE(TAG, "Failed to open file : %s", filepath);
        /* Respond with 500 Internal Server Error */
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to read existing file");
        return ESP_FAIL;
    }

    char* chunk = _rest_context->scratch;
    ssize_t read_bytes;
    do {
        /* Read file in chunks into the scratch buffer */
        read_bytes = read(fd, chunk, SCRATCH_BUFSIZE);
        if (read_bytes == -1) {
            ESP_LOGE(TAG, "Failed to read file : %s", filepath);
        } else if (read_bytes > 0) {
            /* Send the buffer contents as HTTP response chunk */
            if (httpd_resp_send_chunk(req, chunk, read_bytes) != ESP_OK) {
                close(fd);
                ESP_LOGE(TAG, "File sending failed!");
                /* Abort sending file */
                httpd_resp_sendstr_chunk(req, NULL);
                /* Respond with 500 Internal Server Error */
                httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to send file");
                return ESP_FAIL;
            }
        }
    } while (read_bytes > 0);
    /* Close file after sending complete */
    close(fd);
    ESP_LOGI(TAG, "File sending complete");
    /* Respond with an empty chunk to signal HTTP response completion */
    httpd_resp_send_chunk(req, NULL, 0);
    return ESP_OK;
}

esp_err_t rest_server_start(const char* base_path)
{
    REST_CHECK(base_path, "wrong base path", err);
    _rest_context = calloc(1, sizeof(rest_server_context_t));
    REST_CHECK(_rest_context, "No memory for rest context", err);
    strlcpy(_rest_context->base_path, base_path, sizeof(_rest_context->base_path));

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.uri_match_fn = httpd_uri_match_wildcard;

    ESP_LOGI(TAG, "Starting internal HTTP server");
    REST_CHECK(httpd_start(&_server_handle, &config) == ESP_OK, "Start server failed", err_start);

    /* URI handler for getting web server files */
    httpd_uri_t common_get_uri = {.uri = "/*",
                                  .method = HTTP_GET,
                                  .handler = rest_common_get_handler,
                                  .user_ctx = _rest_context};
    httpd_register_uri_handler(_server_handle, &common_get_uri);

    return ESP_OK;
err_start:
    free(_rest_context);
err:
    return ESP_FAIL;
}

void rest_server_stop(void)
{
    /* Stop handler for provisiong webpage files */
    ESP_LOGI(TAG, "Unregistering handler for /*");
    httpd_unregister_uri_handler(_server_handle, "/*", HTTP_GET);

    /* Stop HTTPD server */
    ESP_LOGI(TAG, "Stopping internal HTTPD server");
    httpd_stop(_server_handle);
    _server_handle = NULL;

    if (_rest_context)
        free(_rest_context);

    return;
}

httpd_handle_t* rest_server_get_httpd_handle(void)
{
    return &_server_handle;
}

uri_handler_func_t rest_server_get_common_get_handler(void)
{
    return &rest_common_get_handler;
}

void* rest_server_get_common_get_ctx(void)
{
    return (void*)_rest_context;
}
