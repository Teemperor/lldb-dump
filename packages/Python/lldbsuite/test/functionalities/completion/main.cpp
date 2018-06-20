#include <string>

class DummyClassForBreakpoints
{
public:
    int BreakpointMemberMethod(int x, int y)
    {
        return x + y;
    }
};

// Expression completion code
namespace LongNamespaceName { class NestedClass { long m; }; }

static int GlobalFunction() { return 44; }

class Expr {
public:
    int FooNoArgsBar() { return 1; }
    int FooWithArgsBar(int i) { return i; }
    int FooWithMultipleArgsBar(int i, int j) { return i + j; }
    int FooUnderscoreBar_() { return 4; }
    int FooNumbersBar1() { return 8; }
    int MemberVariableBar = 0;
    static int StaticMemberMethodBar() { return 82; }
};

int main()
{
    LongNamespaceName::NestedClass NestedFoo;
    long SomeLongVarNameWithCapitals = 44;
    int SomeIntVar = 33;
    std::string str;
    GlobalFunction();
    Expr some_expr;
    some_expr.FooNoArgsBar();
    some_expr.FooWithArgsBar(1);
    some_expr.FooUnderscoreBar_();
    some_expr.FooNumbersBar1();
    Expr::StaticMemberMethodBar();
    DummyClassForBreakpoints f;
    f.BreakpointMemberMethod(1, 2); // Break here
}
