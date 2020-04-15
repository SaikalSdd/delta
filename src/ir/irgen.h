#pragma once

#include <vector>
#pragma warning(push, 0)
#include <llvm/ADT/DenseMap.h>
#include <llvm/ADT/StringMap.h>
#pragma warning(pop)
#include "../ast/decl.h"
#include "../ast/expr.h"
#include "../ast/stmt.h"
#include "../ir/ir.h"
#include "../sema/typecheck.h"

namespace delta {

class Module;
struct Type;
class Typechecker;
class IRGenerator;

struct IRGenScope {
    IRGenScope(IRGenerator& irGenerator) : irGenerator(&irGenerator) {}
    void onScopeEnd();
    void clear();

    struct DeferredDestructor {
        IRFunction* function;
        IRValue* value;
        const Decl* decl;
    };

    llvm::SmallVector<const Expr*, 8> deferredExprs;
    llvm::SmallVector<DeferredDestructor, 8> destructorsToCall;
    llvm::DenseMap<const Decl*, IRValue*> valuesByDecl;
    IRGenerator* irGenerator;
};

class IRGenerator {
public:
    IRGenerator();
    IRModule& emitModule(const Module& sourceModule);
    std::vector<IRModule*> getGeneratedModules() { return std::move(generatedModules); }

private:
    friend struct IRGenScope;

    void emitFunctionBody(const FunctionDecl& decl, IRFunction& function);
    void createDestructorCall(IRFunction* destructor, IRValue* receiver);

    /// 'decl' is null if this is the 'this' value.
    void setLocalValue(IRValue* value, const VariableDecl* decl);
    IRValue* getValueOrNull(const Decl* decl);
    IRValue* getValue(const Decl* decl);
    IRValue* getThis(Type targetType = Type());

    /// Emits and loads value.
    IRValue* emitExpr(const Expr& expr);
    /// Emits value without loading.
    IRValue* emitLvalueExpr(const Expr& expr);
    /// Emits value as a pointer, storing it in a temporary alloca if needed.
    IRValue* emitExprAsPointer(const Expr& expr);
    IRValue* emitExprOrEnumTag(const Expr& expr, IRValue*** enumValue);
    IRValue* emitExprWithoutAutoCast(const Expr& expr);
    IRValue* emitAutoCast(IRValue* value, const Expr& expr);
    IRValue* emitVarExpr(const VarExpr& expr);
    IRValue* emitStringLiteralExpr(const StringLiteralExpr& expr);
    IRValue* emitCharacterLiteralExpr(const CharacterLiteralExpr& expr);
    IRValue* emitIntLiteralExpr(const IntLiteralExpr& expr);
    IRValue* emitFloatLiteralExpr(const FloatLiteralExpr& expr);
    IRValue* emitBoolLiteralExpr(const BoolLiteralExpr& expr);
    IRValue* emitNullLiteralExpr(const NullLiteralExpr& expr);
    IRValue* emitUndefinedLiteralExpr(const UndefinedLiteralExpr& expr);
    IRValue* emitArrayLiteralExpr(const ArrayLiteralExpr& expr);
    IRValue* emitTupleExpr(const TupleExpr& expr);
    IRValue* emitImplicitNullComparison(IRValue* operand);
    IRValue* emitNot(const UnaryExpr& expr);
    IRValue* emitUnaryExpr(const UnaryExpr& expr);
    IRValue* emitConstantIncrement(const UnaryExpr& expr, int value);
    IRValue* emitLogicalAnd(const Expr& left, const Expr& right);
    IRValue* emitLogicalOr(const Expr& left, const Expr& right);
    IRValue* emitBinaryExpr(const BinaryExpr& expr);
    void emitAssignment(const BinaryExpr& expr);
    IRValue* emitExprForPassing(const Expr& expr, Type targetType);
    IRValue* emitOptionalConstruction(Type wrappedType, IRValue* arg);
    void emitAssert(IRValue* condition, SourceLocation location, llvm::StringRef message = "Assertion failed");
    IRValue* emitEnumCase(const EnumCase& enumCase, llvm::ArrayRef<NamedValue> associatedValueElements);
    IRValue* emitCallExpr(const CallExpr& expr, IRAllocaInst* thisAllocaForInit = nullptr);
    IRValue* emitBuiltinCast(const CallExpr& expr);
    IRValue* emitSizeofExpr(const SizeofExpr& expr);
    IRValue* emitAddressofExpr(const AddressofExpr& expr);
    IRValue* emitMemberAccess(IRValue* baseValue, const FieldDecl* field);
    IRValue* emitMemberExpr(const MemberExpr& expr);
    IRValue* emitTupleElementAccess(const MemberExpr& expr);
    IRValue* emitIndexExpr(const IndexExpr& expr);
    IRValue* emitUnwrapExpr(const UnwrapExpr& expr);
    IRValue* emitLambdaExpr(const LambdaExpr& expr);
    IRValue* emitIfExpr(const IfExpr& expr);
    IRValue* emitImplicitCastExpr(const ImplicitCastExpr& expr);

    void emitDeferredExprsAndDestructorCallsForReturn();
    void emitBlock(llvm::ArrayRef<Stmt*> stmts, IRBasicBlock* continuation);
    void emitReturnStmt(const ReturnStmt& stmt);
    void emitVarStmt(const VarStmt& stmt);
    void emitIfStmt(const IfStmt& ifStmt);
    void emitSwitchStmt(const SwitchStmt& switchStmt);
    void emitForStmt(const ForStmt& forStmt);
    void emitBreakStmt(const BreakStmt&);
    void emitContinueStmt(const ContinueStmt&);
    IRValue* emitAssignmentLHS(const Expr& lhs);
    void emitCompoundStmt(const CompoundStmt& stmt);
    void emitStmt(const Stmt& stmt);

    void emitDecl(const Decl& decl);
    void emitFunctionDecl(const FunctionDecl& decl);
    IRValue* emitVarDecl(const VarDecl& decl);

    IRValue* getFunctionForCall(const CallExpr& call);
    IRFunction* getFunctionProto(const FunctionDecl& decl);
    IRAllocaInst* createEntryBlockAlloca(Type type, const llvm::Twine& name = "");
    IRAllocaInst* createTempAlloca(IRValue* value, const llvm::Twine& name = "");
    IRValue* createLoad(IRValue* value);
    void createStore(IRValue* value, IRValue* pointer);
    void createCall(IRValue* function, llvm::ArrayRef<IRValue*> args);
    void createCondBr(IRValue* condition, IRBasicBlock* trueBlock, IRBasicBlock* falseBlock);
    void createBr(IRBasicBlock* destination);
    IRValue* createPhi(std::vector<std::pair<IRValue*, IRBasicBlock*>>, const llvm::Twine& name = "");

    std::vector<Type> getFieldTypes(const TypeDecl& decl);
    Type getBuiltinType(llvm::StringRef name);
    Type getEnumType(const EnumDecl& enumDecl);
    Type getStructType(Type type);
    Type getLLVMType(Type type, SourceLocation location = SourceLocation());
    IRValue* getArrayLength(const Expr& object, Type objectType);
    IRValue* getArrayIterator(const Expr& object, Type objectType);

    void beginScope();
    void endScope();
    void deferEvaluationOf(const Expr& expr);
    DestructorDecl* getDefaultDestructor(const TypeDecl& typeDecl);
    void deferDestructorCall(IRValue* receiver, const VariableDecl* decl);
    IRGenScope& globalScope() { return scopes.front(); }

    void setInsertPoint(IRBasicBlock* block);

private:
    struct FunctionInstantiation {
        const FunctionDecl* decl;
        IRFunction* function;
    };

    std::vector<IRGenScope> scopes;

    IRModule* module = nullptr;
    std::vector<IRModule*> generatedModules;

    std::vector<FunctionInstantiation> functionInstantiations;
    const Decl* currentDecl;

    /// The basic blocks to branch to on a 'break'/'continue' statement.
    llvm::SmallVector<IRBasicBlock*, 4> breakTargets;
    llvm::SmallVector<IRBasicBlock*, 4> continueTargets;

    IRBasicBlock* insertBlock;

    static const int optionalHasValueFieldIndex = 0;
    static const int optionalValueFieldIndex = 1;
};

} // namespace delta
