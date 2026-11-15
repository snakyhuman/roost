// Naming gate fixture: a camelCase method outside src/qml_api/, which
// clang-tidy must reject. See tools/check_naming.sh.

namespace roost {

class SessionList {
public:
    int listSessions() const { return Count_; }

private:
    int Count_ = 0;
};

}  // namespace roost
