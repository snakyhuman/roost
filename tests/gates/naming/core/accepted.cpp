// Naming gate fixture: core C++ style, which clang-tidy must accept.
// Methods are PascalCase here (root .clang-tidy). See tools/check_naming.sh.

namespace roost {

class SessionList {
public:
    int ListSessions() const { return Count_; }

private:
    int Count_ = 0;
};

}  // namespace roost
