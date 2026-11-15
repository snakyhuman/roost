// Naming gate fixture: a PascalCase method in the QML-facing layer, which
// clang-tidy must reject. See tools/check_naming.sh.

namespace roost {

class SessionList {
public:
    int ListSessions() const { return Count_; }

private:
    int Count_ = 0;
};

}  // namespace roost
