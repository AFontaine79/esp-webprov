idf_component_register(SRCS "captive_portal.c" 
                    INCLUDE_DIRS include
                    REQUIRES esp_netif esp_http_server
                    PRIV_REQUIRES capt_dns)
