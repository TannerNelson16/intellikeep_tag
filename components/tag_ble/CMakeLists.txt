idf_component_register(
    SRCS "tag_ble.c"
    INCLUDE_DIRS "include" "../tag_config/include"
    REQUIRES bt nvs_flash
)
