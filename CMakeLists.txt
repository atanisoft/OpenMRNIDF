set(SOURCE_DIRS
    src/dcc
    src/executor
    src/freertos_drivers/esp32
    src/freertos_drivers/arduino
    src/openlcb
    src/os
    src/utils
)

if(CONFIG_ENABLE_ARDUINO_DEPENDS)
    list(APPEND SOURCE_DIRS src)
endif()

set(IDF_DEPS
    app_update
    driver
    esp_system
    heap
    vfs
    esp_wifi
    espcoredump
    bootloader_support
    esp_app_format)

idf_component_register(SRC_DIRS "${SOURCE_DIRS}"
                       INCLUDE_DIRS "src"
                       REQUIRES "${IDF_DEPS}")

###############################################################################
# Suppress compilation warnings in OpenMRN
###############################################################################

target_compile_options(${COMPONENT_LIB} PUBLIC -DESP32)
target_compile_options(${COMPONENT_LIB} PRIVATE -Wno-type-limits)
target_compile_options(${COMPONENT_LIB} PRIVATE -Wno-ignored-qualifiers)
target_compile_options(${COMPONENT_LIB} PRIVATE -Wno-missing-field-initializers)
target_compile_options(${COMPONENT_LIB} PRIVATE $<$<COMPILE_LANGUAGE:CXX>:-Wno-class-memaccess>)
target_compile_options(${COMPONENT_LIB} PRIVATE -Wno-implicit-fallthrough)
target_compile_options(${COMPONENT_LIB} PRIVATE -Wno-error=nonnull)
target_compile_options(${COMPONENT_LIB} PUBLIC $<$<COMPILE_LANGUAGE:CXX>:-Wno-volatile>)
target_compile_options(${COMPONENT_LIB} PRIVATE $<$<COMPILE_LANGUAGE:CXX>:-Wno-redundant-move>)

###############################################################################
# Silence a few warnings for various files to reduce compilation noise
###############################################################################
set_source_files_properties(src/freertos_drivers/esp32/Esp32WiFiManager.cpp PROPERTIES COMPILE_FLAGS
    "-Wno-ignored-qualifiers -DESP32_WIFIMGR_SOCKETPARAMS_LOG_LEVEL=VERBOSE -DESP32_WIFIMGR_MDNS_LOOKUP_LOG_LEVEL=VERBOSE")