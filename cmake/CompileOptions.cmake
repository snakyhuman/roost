# Warning set applied to every roost target. Qt and libghostty-vt come in as
# imported targets, so their headers are system headers and stay quiet.

add_library(roost_compile_options INTERFACE)
add_library(roost::compile_options ALIAS roost_compile_options)

target_compile_options(roost_compile_options INTERFACE
    -Wall
    -Wextra
    -Wpedantic
    -Wshadow
    -Wconversion
    -Wsign-conversion
    -Wold-style-cast
    -Wnon-virtual-dtor
    -Woverloaded-virtual
    -Wcast-qual
    -Wdouble-promotion
    -Wformat=2
    -Wimplicit-fallthrough
)

if(ROOST_WERROR)
    target_compile_options(roost_compile_options INTERFACE -Werror)
endif()

# Bounds checks in the standard library outside release builds. Both
# spellings: Linux links libstdc++, macOS links libc++.
target_compile_definitions(roost_compile_options INTERFACE
    $<$<NOT:$<CONFIG:Release>>:_GLIBCXX_ASSERTIONS>
    $<$<NOT:$<CONFIG:Release>>:_LIBCPP_HARDENING_MODE=_LIBCPP_HARDENING_MODE_EXTENSIVE>
)
