idf_component_register(SRCS "prov_webpage_mgr.c" 
                    INCLUDE_DIRS include
                    REQUIRES esp_netif wifi_provisioning esp_http_server
                    PRIV_REQUIRES captive_portal json esp_timer esp_event vfs)
