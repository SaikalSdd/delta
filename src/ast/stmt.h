#pragma once

#include <vector>
#include <memory>
#include <boost/variant.hpp>
#include "expr.h"

namespace delta {

struct VarDecl;
class Stmt;

enum class StmtKind {
    ReturnStmt,
    VariableStmt,
    IncrementStmt,
    DecrementStmt,
    CallStmt,
    DeferStmt,
    IfStmt,
    SwitchStmt,
    WhileStmt,
    AssignStmt,
};

struct ReturnStmt {
    std::vector<Expr> values;
    SrcLoc srcLoc;
};

struct VariableStmt {
    VarDecl* decl; // FIXME: decl is owned.
};

struct IncrementStmt {
    Expr operand;
    SrcLoc srcLoc; // Location of '++'.
};

struct DecrementStmt {
    Expr operand;
    SrcLoc srcLoc; // Location of '--'.
};

struct CallStmt {
    CallExpr expr;
};

struct DeferStmt {
    Expr expr;
};

struct IfStmt {
    Expr condition;
    std::vector<Stmt> thenBody;
    std::vector<Stmt> elseBody;
};

struct SwitchCase {
    Expr value;
    std::vector<Stmt> stmts;
};

struct SwitchStmt {
    Expr condition;
    std::vector<SwitchCase> cases;
};

struct WhileStmt {
    Expr condition;
    std::vector<Stmt> body;
};

struct AssignStmt {
    Expr lhs;
    Expr rhs;
    SrcLoc srcLoc; // Location of '='.
};

class Stmt {
public:
#define DEFINE_STMTKIND_GETTER_AND_CONSTRUCTOR(KIND) \
    Stmt(KIND&& value) : data(std::move(value)) { } \
    \
    bool is##KIND() const { return getKind() == StmtKind::KIND; } \
    \
    KIND& get##KIND() { \
        assert(is##KIND()); \
        return boost::get<KIND>(data); \
    } \
    const KIND& get##KIND() const { \
        assert(is##KIND()); \
        return boost::get<KIND>(data); \
    }
    DEFINE_STMTKIND_GETTER_AND_CONSTRUCTOR(ReturnStmt)
    DEFINE_STMTKIND_GETTER_AND_CONSTRUCTOR(VariableStmt)
    DEFINE_STMTKIND_GETTER_AND_CONSTRUCTOR(IncrementStmt)
    DEFINE_STMTKIND_GETTER_AND_CONSTRUCTOR(DecrementStmt)
    DEFINE_STMTKIND_GETTER_AND_CONSTRUCTOR(CallStmt)
    DEFINE_STMTKIND_GETTER_AND_CONSTRUCTOR(DeferStmt)
    DEFINE_STMTKIND_GETTER_AND_CONSTRUCTOR(IfStmt)
    DEFINE_STMTKIND_GETTER_AND_CONSTRUCTOR(SwitchStmt)
    DEFINE_STMTKIND_GETTER_AND_CONSTRUCTOR(WhileStmt)
    DEFINE_STMTKIND_GETTER_AND_CONSTRUCTOR(AssignStmt)
#undef DEFINE_STMTKIND_GETTER_AND_CONSTRUCTOR

    Stmt(Stmt&&) = default;
    StmtKind getKind() const { return static_cast<StmtKind>(data.which()); }

private:
    boost::variant<ReturnStmt, VariableStmt, IncrementStmt, DecrementStmt,
        CallStmt, DeferStmt, IfStmt, SwitchStmt, WhileStmt, AssignStmt> data;
};

}
