add_library(roost_plugin_rpc STATIC json_rpc.cpp)
target_include_directories(roost_plugin_rpc PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_link_libraries(roost_plugin_rpc PUBLIC Qt6::Core)
