idf_component_register(
    SRCS "app_main.c"
    INCLUDE_DIRS "."
    REQUIRES console esp_event esp_netif esp_wifi esp_wifi_remote esp_hosted
        nvs_flash esp-idf-adb
)

if(TARGET eh_host_feat_wifi)
    idf_component_get_property(esp_wifi_component_dir esp_wifi COMPONENT_DIR)
    idf_component_get_property(esp_hosted_component_dir espressif__esp_hosted COMPONENT_DIR)
    set(esp_wifi_remote_config_dir
        "${esp_hosted_component_dir}/port/idf_components/esp_wifi_remote/esp_wifi_remote/components/esp_wifi_remote/idf_v${IDF_VERSION_MAJOR}.${IDF_VERSION_MINOR}/include"
    )
    if(NOT EXISTS "${esp_wifi_remote_config_dir}/esp_wifi_default_config.h")
        message(FATAL_ERROR
            "ESP-Hosted Wi-Fi defaults are missing for ESP-IDF ${IDF_VERSION_MAJOR}.${IDF_VERSION_MINOR}"
        )
    endif()

    target_include_directories(eh_host_feat_wifi PRIVATE
        "${esp_wifi_component_dir}/include/local"
        "${esp_wifi_remote_config_dir}"
    )
    target_compile_definitions(eh_host_feat_wifi PRIVATE
        CONFIG_SLAVE_IDF_TARGET_ESP32C6=1
    )
    target_compile_options(eh_host_feat_wifi PRIVATE
        "SHELL:-include esp_wifi_default_config.h"
    )

    target_include_directories(${COMPONENT_LIB} PRIVATE
        "${esp_wifi_component_dir}/include/local"
        "${esp_wifi_remote_config_dir}"
    )
    target_compile_definitions(${COMPONENT_LIB} PRIVATE
        CONFIG_SLAVE_IDF_TARGET_ESP32C6=1
    )
    target_compile_options(${COMPONENT_LIB} PRIVATE
        "SHELL:-include esp_wifi_default_config.h"
    )
endif()
