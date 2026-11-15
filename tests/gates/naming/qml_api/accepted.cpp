// Naming gate fixture: the QML-facing style, which clang-tidy must accept.
// Methods are camelCase here (.clang-tidy links to src/qml_api/.clang-tidy).
// See tools/check_naming.sh.

namespace roost {

class SessionList {
public:
    int listSessions() const { return Count_; }

private:
    int Count_ = 0;
};

}  // namespace roost
