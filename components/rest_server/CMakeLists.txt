idf_component_register(SRCS "rest_server.c" 
                    INCLUDE_DIRS include
                    REQUIRES esp_http_server
                    PRIV_REQUIRES vfs)
