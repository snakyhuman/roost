qt_add_executable(roostd
    main.cpp
)

target_link_libraries(roostd PRIVATE roost_core Qt6::Core)
