idf_component_register(
    SRCS "tag_hw.c"
    INCLUDE_DIRS "include" "../tag_config/include"
    REQUIRES driver esp_adc nvs_flash esp_driver_i2c esp_hw_support
)

if(TAG_BATTERY_PROFILE STREQUAL "lir2032")
    set(TAG_BATTERY_PROFILE_COMPONENT_VALUE 2)
else()
    set(TAG_BATTERY_PROFILE_COMPONENT_VALUE 1)
endif()

if(TAG_HW_VARIANT STREQUAL "production")
    target_compile_definitions(${COMPONENT_LIB} PRIVATE
        TAG_HW_VARIANT=2
        TAG_HW_VARIANT_SEEED_C3=1
        TAG_HW_VARIANT_PRODUCTION=2
        TAG_BATTERY_PROFILE=${TAG_BATTERY_PROFILE_COMPONENT_VALUE}
        TAG_BATTERY_PROFILE_CR2032=1
        TAG_BATTERY_PROFILE_LIR2032=2
    )
else()
    target_compile_definitions(${COMPONENT_LIB} PRIVATE
        TAG_HW_VARIANT=1
        TAG_HW_VARIANT_SEEED_C3=1
        TAG_HW_VARIANT_PRODUCTION=2
        TAG_BATTERY_PROFILE=${TAG_BATTERY_PROFILE_COMPONENT_VALUE}
        TAG_BATTERY_PROFILE_CR2032=1
        TAG_BATTERY_PROFILE_LIR2032=2
    )
endif()
