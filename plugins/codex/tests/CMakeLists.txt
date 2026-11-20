# The adapter runs the test executable again as its fixture Codex CLI.
find_package(Qt6 REQUIRED COMPONENTS Test)
qt_add_executable(codex_adapter_test adapter_test.cpp)
target_link_libraries(codex_adapter_test PRIVATE Qt6::Test)
add_dependencies(codex_adapter_test codex-adapter)
add_test(NAME codex_adapter_test
    COMMAND codex_adapter_test "$<TARGET_FILE:codex-adapter>")
set_tests_properties(codex_adapter_test PROPERTIES TIMEOUT 30 LABELS threads)
