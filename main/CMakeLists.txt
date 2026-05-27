idf_component_register(
    SRCS "tag_main.c"
    INCLUDE_DIRS "."
    REQUIRES tag_ble tag_hw tag_config
)

if(TAG_BEHAVIOR_VARIANT STREQUAL "demo")
    target_compile_definitions(${COMPONENT_LIB} PRIVATE
        TAG_DEFAULT_ADV_WINDOW_MS=4000U
    )
else()
    target_compile_definitions(${COMPONENT_LIB} PRIVATE
        TAG_DEFAULT_ADV_WINDOW_MS=75U
    )
endif()
