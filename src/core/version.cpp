#include "core/version.hpp"

#include <ghostty/vt.h>

#include <string_view>

namespace roost {

namespace {

std::string_view GhosttyVtVersion() {
    GhosttyString version{};
    if (ghostty_build_info(GHOSTTY_BUILD_INFO_VERSION_STRING, &version) !=
        GHOSTTY_SUCCESS) {
        return "unknown";
    }
    return {reinterpret_cast<const char*>(version.ptr), version.len};
}

}  // namespace

std::string VersionString() {
    std::string result = ROOST_VERSION;
    result += " (libghostty-vt ";
    result += GhosttyVtVersion();
    result += ")";
    return result;
}

}  // namespace roost
