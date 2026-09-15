# The config walker is generated from config/config_schema.json. Every section
# there $refs the schema owned by the component that consumes it, resolved by
# component name -- so the reference does not depend on where those components
# live.
#
# The guard is mandatory: ESP-IDF re-runs this file in `cmake -P` script mode to
# collect requirements, and js2c_generate_sections() does not exist in that pass.
if(NOT CMAKE_BUILD_EARLY_EXPANSION)
    js2c_generate_sections("${CMAKE_CURRENT_LIST_DIR}/../config/config_schema.json"
                           BASENAME app_config OUT_C GEN_C)
    get_filename_component(GEN_DIR "${GEN_C}" DIRECTORY)
endif()

# No REQUIRES, deliberately: project.cmake grants main an implicit dependency on
# every component in the build only while REQUIRES and PRIV_REQUIRES are both
# unset. Naming one component here would silently strip the rest.
idf_component_register(
    SRCS         "main.c"
                 "config_reader.c"
                 "app_console.c"
                 "app_control.c"
                 "app_http.c"
                 "app_status.c"
                 "app_time.c"
                 "batch.c"
                 "board.c"
                 "scale_store.c"
                 "sensors.c"
                 "trigger.c"
                 "weight.c"
                 "${GEN_C}"
    INCLUDE_DIRS "." "${GEN_DIR}")

target_compile_options(${COMPONENT_LIB} PRIVATE -Wall -Wextra -Werror)

# LOG_ERROR must be in scope before js2c_builtins.h inside the generated unit
# only, so hand-written sources keep their own error handling.
set_source_files_properties("${GEN_C}" TARGET_DIRECTORY ${COMPONENT_LIB}
    PROPERTIES COMPILE_OPTIONS "-include;js2c_error_capture.h")
