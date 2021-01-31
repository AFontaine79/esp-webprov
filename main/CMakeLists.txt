idf_component_register(SRCS "webprov_example_main.c" 
                    INCLUDE_DIRS "." "..")

if(CONFIG_EXAMPLE_WEB_DEPLOY_SF)
    set(WEB_SRC_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../front/web-demo")
    if(EXISTS ${WEB_SRC_DIR}/dist)
        spiffs_create_partition_image(www ${WEB_SRC_DIR}/dist FLASH_IN_PROJECT)
    else()
        set(WEB_PUBLISH_DIR "${WEB_SRC_DIR}/dist")
        message(STATUS "WEB_PUBLISH_DIR=${WEB_PUBLISH_DIR}")
        execute_process(COMMAND mkdir ${WEB_PUBLISH_DIR})
    endif()
endif()
