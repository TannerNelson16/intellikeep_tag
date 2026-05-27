cmake_minimum_required(VERSION 3.16)

set(EXTRA_COMPONENT_DIRS "${CMAKE_SOURCE_DIR}/components")

if(NOT DEFINED ENV{IDF_PATH} OR "$ENV{IDF_PATH}" STREQUAL "")
    message(FATAL_ERROR "IDF_PATH is not set. Source ESP-IDF export.sh before configuring this project.")
endif()

if(NOT EXISTS "$ENV{IDF_PATH}/tools/cmake/project.cmake")
    message(FATAL_ERROR
        "IDF_PATH points to '$ENV{IDF_PATH}', but ESP-IDF was not found there. "
        "Set IDF_PATH to a valid ESP-IDF checkout and source its export.sh.")
endif()

include($ENV{IDF_PATH}/tools/cmake/project.cmake)

project(intellikeep_tag)

set(TAG_HW_VARIANT "seeed_c3" CACHE STRING "Hardware build variant (seeed_c3 or production)")
set_property(CACHE TAG_HW_VARIANT PROPERTY STRINGS seeed_c3 production)

set(TAG_BEHAVIOR_VARIANT "standard" CACHE STRING "Behavior build variant (standard or demo)")
set_property(CACHE TAG_BEHAVIOR_VARIANT PROPERTY STRINGS standard demo)

# Battery percentage curve selector.
# Switch this between "cr2032" and "lir2032" depending on the installed coin cell.
set(TAG_BATTERY_PROFILE "lir2032" CACHE STRING "Battery profile (cr2032 or lir2032)")
set_property(CACHE TAG_BATTERY_PROFILE PROPERTY STRINGS cr2032 lir2032)

if(TAG_HW_VARIANT STREQUAL "seeed_c3")
    set(TAG_HW_VARIANT_VALUE 1)
elseif(TAG_HW_VARIANT STREQUAL "production")
    set(TAG_HW_VARIANT_VALUE 2)
else()
    message(FATAL_ERROR "Unsupported TAG_HW_VARIANT='${TAG_HW_VARIANT}'. Expected seeed_c3 or production.")
endif()

if(TAG_BEHAVIOR_VARIANT STREQUAL "standard")
    set(TAG_BEHAVIOR_VARIANT_VALUE 1)
    set(TAG_DEFAULT_WAKE_PERIOD_US_VALUE 300000000ULL)
    set(TAG_DEFAULT_ADV_WINDOW_MS_VALUE 75U)
    set(TAG_DEFAULT_NAME_VALUE "\"Tag\"")
elseif(TAG_BEHAVIOR_VARIANT STREQUAL "demo")
    set(TAG_BEHAVIOR_VARIANT_VALUE 2)
    set(TAG_DEFAULT_WAKE_PERIOD_US_VALUE 5000000ULL)
    set(TAG_DEFAULT_ADV_WINDOW_MS_VALUE 4000U)
    set(TAG_DEFAULT_NAME_VALUE "\"DemoTag\"")
else()
    message(FATAL_ERROR "Unsupported TAG_BEHAVIOR_VARIANT='${TAG_BEHAVIOR_VARIANT}'. Expected standard or demo.")
endif()

if(TAG_BATTERY_PROFILE STREQUAL "cr2032")
    set(TAG_BATTERY_PROFILE_VALUE 1)
elseif(TAG_BATTERY_PROFILE STREQUAL "lir2032")
    set(TAG_BATTERY_PROFILE_VALUE 2)
else()
    message(FATAL_ERROR "Unsupported TAG_BATTERY_PROFILE='${TAG_BATTERY_PROFILE}'. Expected cr2032 or lir2032.")
endif()

add_compile_definitions(
    TAG_HW_VARIANT=${TAG_HW_VARIANT_VALUE}
    TAG_HW_VARIANT_SEEED_C3=1
    TAG_HW_VARIANT_PRODUCTION=2
    TAG_BEHAVIOR_VARIANT=${TAG_BEHAVIOR_VARIANT_VALUE}
    TAG_BEHAVIOR_VARIANT_STANDARD=1
    TAG_BEHAVIOR_VARIANT_DEMO=2
    TAG_BATTERY_PROFILE=${TAG_BATTERY_PROFILE_VALUE}
    TAG_BATTERY_PROFILE_CR2032=1
    TAG_BATTERY_PROFILE_LIR2032=2
    TAG_DEFAULT_WAKE_PERIOD_US=${TAG_DEFAULT_WAKE_PERIOD_US_VALUE}
    TAG_DEFAULT_ADV_WINDOW_MS=${TAG_DEFAULT_ADV_WINDOW_MS_VALUE}
    TAG_DEFAULT_NAME=${TAG_DEFAULT_NAME_VALUE}
)
