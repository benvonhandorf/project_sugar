cmake_minimum_required(VERSION 3.22)

#
# The shared components are fetched by tag -- see main/idf_component.yml. Nothing
# is needed here for a normal build, and nothing has to be cloned beside this
# project.
#
# ESP_COMPONENTS_DIR is the development override, for working on a component and
# this project at the same time: a local component of the same name wins over the
# fetched one, so the checkout is what gets built and the tags are ignored. It is
# opt-in on purpose -- a checkout that shadowed the pinned versions merely by
# existing would make "does this build" depend on what else is on the disk.
#
#     idf.py -DESP_COMPONENTS_DIR=../esp_components build
#
if(NOT DEFINED ESP_COMPONENTS_DIR AND DEFINED ENV{ESP_COMPONENTS_DIR})
    set(ESP_COMPONENTS_DIR "$ENV{ESP_COMPONENTS_DIR}")
endif()

if(DEFINED ESP_COMPONENTS_DIR)
    if(NOT EXISTS "${ESP_COMPONENTS_DIR}")
        message(FATAL_ERROR
            "ESP_COMPONENTS_DIR is set to ${ESP_COMPONENTS_DIR}, which does not "
            "exist.\nUnset it to build against the tags in main/idf_component.yml.")
    endif()
    list(APPEND EXTRA_COMPONENT_DIRS "${ESP_COMPONENTS_DIR}")
    message(STATUS "Shared components overridden by checkout: ${ESP_COMPONENTS_DIR}")
endif()

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(sensor_stick_trigger)

#
# Build the LittleFS image from fs/ and flash it with the firmware.
#
# fs/config.json is gitignored and holds the device's real credentials;
# config/config.example.json is the committed template. A project that ships
# without a config.json still boots -- config_store reports the file is missing
# and the schema's defaults apply -- so a fresh clone is buildable.
#
littlefs_create_partition_image(res fs FLASH_IN_PROJECT)
