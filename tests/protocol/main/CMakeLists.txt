idf_component_register(
    SRCS "test_adb_protocol.c"
    INCLUDE_DIRS "."
    PRIV_INCLUDE_DIRS "../../../src"
    REQUIRES esp-idf-adb esp_netif unity mbedtls
)
