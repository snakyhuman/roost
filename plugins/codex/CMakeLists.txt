cmake_minimum_required(VERSION 3.25)

# Windows can build and exercise the adapter independently of the Roost host.
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    project(roost_codex_adapter LANGUAGES CXX)
    set(CMAKE_CXX_STANDARD 23)
    set(CMAKE_CXX_STANDARD_REQUIRED ON)
    find_package(Qt6 6.8 REQUIRED COMPONENTS Core)
    qt_standard_project_setup()
    add_subdirectory(../common common)
    set(ROOST_PLUGINS_OUTPUT_DIR ${PROJECT_BINARY_DIR}/roost-plugins)
    include(CTest)
endif()

set(plugin_dir ${ROOST_PLUGINS_OUTPUT_DIR}/codex)
qt_add_executable(codex-adapter backend/adapter.cpp backend/main.cpp)
target_link_libraries(codex-adapter PRIVATE roost_plugin_rpc)
# Keep multi-config generators from adding a configuration subdirectory.
set_target_properties(codex-adapter PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "$<1:${plugin_dir}/backend>")

configure_file(plugin.json.in ${plugin_dir}/plugin.json @ONLY)

if(COMMAND roost_install_plugin_binary)
    install(FILES ${plugin_dir}/plugin.json
        DESTINATION ${ROOST_INSTALL_PLUGINS_DIR}/codex COMPONENT roost)
    roost_install_plugin_binary(codex codex-adapter backend)
endif()

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR AND BUILD_TESTING)
    add_subdirectory(tests)
endif()
