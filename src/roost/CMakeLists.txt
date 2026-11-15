qt_add_executable(roost
    main.cpp
)

# NO_CACHEGEN: qmlcachegen compiles QML bindings to C++ ahead of time, and v1
# does not use AOT until its licence is settled (design.md §10, §14). QML is
# compiled at run time instead, as plugin QML is anyway.
qt_add_qml_module(roost
    URI Roost.Shell
    QML_FILES Main.qml
    NO_CACHEGEN
)

target_link_libraries(roost PRIVATE roost_core Qt6::Gui Qt6::Qml Qt6::Quick)

set_target_properties(roost PROPERTIES
    MACOSX_BUNDLE OFF
)
