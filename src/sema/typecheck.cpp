#include <algorithm>
#include <iterator>
#include <memory>
#include <string>
#include <vector>
#include <cstdlib>
#include <system_error>
#include <llvm/ADT/ArrayRef.h>
#include <llvm/ADT/StringMap.h>
#include <llvm/ADT/StringRef.h>
#include <llvm/ADT/SmallVector.h>
#include <llvm/ADT/STLExtras.h>
#include <llvm/Support/ErrorHandling.h>
#include <llvm/Support/FileSystem.h>
#include <llvm/Support/Path.h>
#include <llvm/Support/ErrorOr.h>
#include "typecheck.h"
#include "c-import.h"
#include "../ast/type.h"
#include "../ast/expr.h"
#include "../ast/decl.h"
#include "../ast/module.h"
#include "../ast/mangle.h"
#include "../package-manager/manifest.h"
#include "../support/utility.h"

using namespace delta;

void validateGenericArgCount(size_t genericParamCount, llvm::ArrayRef<Type> genericArgs,
                             llvm::StringRef name, SourceLocation location);

namespace delta {
llvm::StringMap<std::shared_ptr<Module>> allImportedModules;
}

std::vector<Module*> delta::getAllImportedModules() {
    return map(allImportedModules,
               [](const llvm::StringMapEntry<std::shared_ptr<Module>>& p) { return p.second.get(); });
}

namespace {

/// Storage for Decls that are not in the AST but are referenced by the symbol table.
std::vector<std::unique_ptr<Decl>> nonASTDecls;

std::vector<std::pair<FieldDecl*, bool>> currentFieldDecls;
Type functionReturnType = nullptr;
int breakableBlocks = 0;

}

static void checkReturnPointerToLocal(ReturnStmt& stmt) {
    if (!functionReturnType.isTupleType()) {
        auto* returnValue = stmt.getReturnValue();

        if (auto* prefixExpr = llvm::dyn_cast<PrefixExpr>(returnValue)) {
            if (prefixExpr->getOperator() == AND) {
                returnValue = &prefixExpr->getOperand();
            }
        }

        Type localVariableType;

        if (auto* varExpr = llvm::dyn_cast<VarExpr>(returnValue)) {
            switch (varExpr->getDecl()->getKind()) {
                case delta::DeclKind::VarDecl: {
                    auto* varDecl = llvm::cast<VarDecl>(varExpr->getDecl());
                    if (varDecl->getParent() && varDecl->getParent()->isFunctionDecl()) {
                        localVariableType = varDecl->getType();
                    }
                    break;
                }
                case delta::DeclKind::ParamDecl:
                    localVariableType = llvm::cast<ParamDecl>(varExpr->getDecl())->getType();
                    break;

                default:
                    break;
            }
        }

        if (localVariableType
            && functionReturnType.removeOptional().isPointerType()
            && functionReturnType.removeOptional().getPointee() == localVariableType) {
            warning(returnValue->getLocation(), "returning pointer to local variable ",
                    "(local variables will not exist after the function returns)");
        }
    }
}

void Typechecker::typecheckReturnStmt(ReturnStmt& stmt) {
    if (!stmt.getReturnValue()) {
        if (!functionReturnType.isVoid()) {
            error(stmt.getLocation(), "expected return statement to return a value of type '",
                  functionReturnType, "'");
        }
        return;
    }

    Type returnValueType = typecheckExpr(*stmt.getReturnValue());
    Type convertedType;

    if (!isImplicitlyConvertible(stmt.getReturnValue(), returnValueType, functionReturnType, &convertedType)) {
        error(stmt.getLocation(), "mismatching return type '", returnValueType, "', expected '",
              functionReturnType, "'");
    }

    stmt.getReturnValue()->setType(convertedType ? convertedType : returnValueType);
    checkReturnPointerToLocal(stmt);
    stmt.getReturnValue()->setMoved(true);
}

void Typechecker::typecheckVarStmt(VarStmt& stmt) {
    typecheckVarDecl(stmt.getDecl(), false);
}

void Typechecker::typecheckIncrementStmt(IncrementStmt& stmt) {
    auto type = typecheckExpr(stmt.getOperand());
    if (!type.isMutable()) {
        error(stmt.getLocation(), "cannot increment immutable value");
    }
    // TODO: check that operand supports increment operation.
}

void Typechecker::typecheckDecrementStmt(DecrementStmt& stmt) {
    auto type = typecheckExpr(stmt.getOperand());
    if (!type.isMutable()) {
        error(stmt.getLocation(), "cannot decrement immutable value");
    }
    // TODO: check that operand supports decrement operation.
}

void Typechecker::typecheckIfStmt(IfStmt& ifStmt) {
    Type conditionType = typecheckExpr(ifStmt.getCondition());
    if (!conditionType.isBool()) {
        error(ifStmt.getCondition().getLocation(), "'if' condition must have type 'bool'");
    }
    for (auto& stmt : ifStmt.getThenBody()) {
        typecheckStmt(*stmt);
    }
    for (auto& stmt : ifStmt.getElseBody()) {
        typecheckStmt(*stmt);
    }
}

void Typechecker::typecheckSwitchStmt(SwitchStmt& stmt) {
    Type conditionType = typecheckExpr(stmt.getCondition());
    breakableBlocks++;
    for (auto& switchCase : stmt.getCases()) {
        Type caseType = typecheckExpr(*switchCase.getValue());

        if (!isImplicitlyConvertible(switchCase.getValue(), caseType, conditionType, nullptr)) {
            error(switchCase.getValue()->getLocation(), "case value type '", caseType,
                  "' doesn't match switch condition type '", conditionType, "'");
        }
        for (auto& caseStmt : switchCase.getStmts()) {
            typecheckStmt(*caseStmt);
        }
    }
    for (auto& defaultStmt : stmt.getDefaultStmts()) {
        typecheckStmt(*defaultStmt);
    }
    breakableBlocks--;
}

void Typechecker::typecheckWhileStmt(WhileStmt& whileStmt) {
    Type conditionType = typecheckExpr(whileStmt.getCondition());
    if (!conditionType.isBool()) {
        error(whileStmt.getCondition().getLocation(), "'while' condition must have type 'bool'");
    }
    breakableBlocks++;
    for (auto& stmt : whileStmt.getBody()) {
        typecheckStmt(*stmt);
    }
    breakableBlocks--;
}

void Typechecker::typecheckBreakStmt(BreakStmt& breakStmt) {
    if (breakableBlocks == 0) {
        error(breakStmt.getLocation(), "'break' is only allowed inside 'while' and 'switch' statements");
    }
}

static bool allowAssignmentOfUndefined(const Expr& lhs, const FunctionDecl* currentFunction) {
    if (auto* initDecl = llvm::dyn_cast<InitDecl>(currentFunction)) {
        switch (lhs.getKind()) {
            case ExprKind::VarExpr:
                if (auto* fieldDecl = llvm::dyn_cast<FieldDecl>(llvm::cast<VarExpr>(lhs).getDecl())) {
                    if (fieldDecl->getParent() == initDecl->getTypeDecl()) {
                        return true;
                    }
                }
                return false;

            case ExprKind::MemberExpr:
                if (auto* varExpr = llvm::dyn_cast<VarExpr>(llvm::cast<MemberExpr>(lhs).getBaseExpr())) {
                    if (varExpr->getIdentifier() == "this") {
                        return true;
                    }
                }
                return false;

            default:
                return false;
        }
    }

    return false;
}

void Typechecker::typecheckAssignStmt(AssignStmt& stmt) {
    Type lhsType = typecheckExpr(*stmt.getLHS(), true);
    Type rhsType = stmt.getRHS() ? typecheckExpr(*stmt.getRHS()) : nullptr;

    if (!stmt.getRHS() && !allowAssignmentOfUndefined(*stmt.getLHS(), currentFunction)) {
        error(stmt.getLocation(), "'undefined' is only allowed as an initial value");
    }

    if (stmt.getRHS()) {
        Type convertedType;

        if (isImplicitlyConvertible(stmt.getRHS(), rhsType, lhsType, &convertedType)) {
            stmt.getRHS()->setType(convertedType ? convertedType : rhsType);
        } else {
            error(stmt.getLocation(), "cannot assign '", rhsType, "' to variable of type '", lhsType, "'");
        }
    }

    if (!lhsType.isMutable()) {
        switch (stmt.getLHS()->getKind()) {
            case ExprKind::VarExpr:
                error(stmt.getLocation(), "cannot assign to immutable variable '",
                      llvm::cast<VarExpr>(stmt.getLHS())->getIdentifier(), "'");
            case ExprKind::MemberExpr:
                error(stmt.getLocation(), "cannot assign to immutable variable '",
                      llvm::cast<MemberExpr>(stmt.getLHS())->getMemberName(), "'");
            default:
                error(stmt.getLocation(), "cannot assign to immutable expression");
        }
    }

    if (rhsType && !isImplicitlyCopyable(rhsType) && !lhsType.removeOptional().isPointerType()) {
        stmt.getRHS()->setMoved(true);
        stmt.getLHS()->setMoved(false);
    }

    markFieldAsInitialized(*stmt.getLHS());
}

void Typechecker::typecheckCompoundStmt(CompoundStmt& stmt) {
    getCurrentModule()->getSymbolTable().pushScope();

    for (auto& substmt : stmt.getBody()) {
        typecheckStmt(*substmt);
    }

    getCurrentModule()->getSymbolTable().popScope();
}

void Typechecker::markFieldAsInitialized(Expr& expr) const {
    if (currentFunction->isInitDecl()) {
        switch (expr.getKind()) {
            case ExprKind::VarExpr: {
                auto* varExpr = llvm::cast<VarExpr>(&expr);

                if (auto* fieldDecl = llvm::dyn_cast<FieldDecl>(varExpr->getDecl())) {
                    auto it = llvm::find_if(currentFieldDecls, [&](std::pair<FieldDecl*, bool>& p) {
                        return p.first == fieldDecl;
                    });

                    if (it != currentFieldDecls.end()) {
                        it->second = true; // Mark member variable as initialized.
                    }
                }

                break;
            }
            case ExprKind::MemberExpr: {
                auto* memberExpr = llvm::cast<MemberExpr>(&expr);

                if (auto* varExpr = llvm::dyn_cast<VarExpr>(memberExpr->getBaseExpr())) {
                    if (varExpr->getIdentifier() == "this") {
                        auto it = llvm::find_if(currentFieldDecls, [&](std::pair<FieldDecl*, bool>& p) {
                            return p.first->getName() == memberExpr->getMemberName();
                        });

                        if (it != currentFieldDecls.end()) {
                            it->second = true; // Mark member variable as initialized.
                        }
                    }
                }

                break;
            }
            default:
                break;
        }
    }
}

void Typechecker::typecheckStmt(Stmt& stmt) {
    switch (stmt.getKind()) {
        case StmtKind::ReturnStmt: typecheckReturnStmt(llvm::cast<ReturnStmt>(stmt)); break;
        case StmtKind::VarStmt: typecheckVarStmt(llvm::cast<VarStmt>(stmt)); break;
        case StmtKind::IncrementStmt: typecheckIncrementStmt(llvm::cast<IncrementStmt>(stmt)); break;
        case StmtKind::DecrementStmt: typecheckDecrementStmt(llvm::cast<DecrementStmt>(stmt)); break;
        case StmtKind::ExprStmt: typecheckExpr(llvm::cast<ExprStmt>(stmt).getExpr()); break;
        case StmtKind::DeferStmt: typecheckExpr(llvm::cast<DeferStmt>(stmt).getExpr()); break;
        case StmtKind::IfStmt: typecheckIfStmt(llvm::cast<IfStmt>(stmt)); break;
        case StmtKind::SwitchStmt: typecheckSwitchStmt(llvm::cast<SwitchStmt>(stmt)); break;
        case StmtKind::WhileStmt: typecheckWhileStmt(llvm::cast<WhileStmt>(stmt)); break;
        case StmtKind::ForStmt: llvm_unreachable("ForStmt should be lowered into a WhileStmt"); break;
        case StmtKind::BreakStmt: typecheckBreakStmt(llvm::cast<BreakStmt>(stmt)); break;
        case StmtKind::AssignStmt: typecheckAssignStmt(llvm::cast<AssignStmt>(stmt)); break;
        case StmtKind::CompoundStmt: typecheckCompoundStmt(llvm::cast<CompoundStmt>(stmt)); break;
    }
}

void Typechecker::typecheckType(Type type, SourceLocation location) {
    switch (type.getKind()) {
        case TypeKind::BasicType: {
            if (type.isBuiltinType()) {
                validateGenericArgCount(0, type.getGenericArgs(), type.getName(), location);
                break;
            }

            auto* basicType = llvm::cast<BasicType>(type.getBase());

            for (auto genericArg : basicType->getGenericArgs()) {
                typecheckType(genericArg, location);
            }

            auto decls = findDecls(mangleTypeDecl(basicType->getName(), basicType->getGenericArgs()));

            if (decls.empty()) {
                auto decls = findDecls(basicType->getName());

                if (decls.empty()) {
                    error(location, "unknown type '", type, "'");
                }

                auto p = llvm::cast<TypeTemplate>(decls[0])->instantiate(basicType->getGenericArgs());
                addToSymbolTable(*p);
                typecheckTypeDecl(*p);
            } else {
                switch (decls[0]->getKind()) {
                    case DeclKind::TypeDecl:
                        if (auto* deinitDecl = llvm::cast<TypeDecl>(decls[0])->getDeinitializer()) {
                            typecheckFunctionDecl(*deinitDecl);
                        }
                        break;
                    case DeclKind::TypeTemplate:
                        validateGenericArgCount(llvm::cast<TypeTemplate>(decls[0])->getGenericParams().size(),
                                                basicType->getGenericArgs(), basicType->getName(), location);
                        break;
                    default:
                        break;
                }
            }

            break;
        }
        case TypeKind::ArrayType:
            typecheckType(type.getElementType(), location);
            break;
        case TypeKind::TupleType:
            for (auto subtype : type.getSubtypes()) {
                typecheckType(subtype, location);
            }
            break;
        case TypeKind::FunctionType:
            for (auto paramType : type.getParamTypes()) {
                typecheckType(paramType, location);
            }
            typecheckType(type.getReturnType(), location);
            break;
        case TypeKind::PointerType: {
            if (type.getPointee().isArrayWithRuntimeSize()) {
                if (findDecls(mangleTypeDecl("ArrayRef", type.getPointee().getElementType())).empty()) {
                    auto& arrayRef = llvm::cast<TypeTemplate>(findDecl("ArrayRef", SourceLocation::invalid()));
                    auto* instantiation = arrayRef.instantiate({type.getPointee().getElementType()});
                    addToSymbolTable(*instantiation);
                    declsToTypecheck.push_back(instantiation);
                }
            } else {
                typecheckType(type.getPointee(), location);
            }
            break;
        }
        case TypeKind::OptionalType:
            typecheckType(type.getWrappedType(), location);
            break;
    }
}

void Typechecker::typecheckParamDecl(ParamDecl& decl) {
    if (getCurrentModule()->getSymbolTable().contains(decl.getName())) {
        error(decl.getLocation(), "redefinition of '", decl.getName(), "'");
    }

    if (decl.getType().isMutable()) {
        error(decl.getLocation(), "parameter types cannot be 'mutable'");
    }

    typecheckType(decl.getType(), decl.getLocation());
    getCurrentModule()->getSymbolTable().add(decl.getName(), &decl);
}

void Typechecker::addToSymbolTableWithName(Decl& decl, llvm::StringRef name, bool global) const {
    if (getCurrentModule()->getSymbolTable().contains(name)) {
        error(decl.getLocation(), "redefinition of '", name, "'");
    }

    if (global) {
        getCurrentModule()->getSymbolTable().addGlobal(name, &decl);
    } else {
        getCurrentModule()->getSymbolTable().add(name, &decl);
    }
}

void Typechecker::addToSymbolTable(FunctionTemplate& decl) const {
    if (getCurrentModule()->getSymbolTable().findWithMatchingPrototype(*decl.getFunctionDecl())) {
        error(decl.getLocation(), "redefinition of '", mangle(decl), "'");
    }
    getCurrentModule()->getSymbolTable().addGlobal(mangle(decl), &decl);
}

void Typechecker::addToSymbolTable(FunctionDecl& decl) const {
    if (getCurrentModule()->getSymbolTable().findWithMatchingPrototype(decl)) {
        error(decl.getLocation(), "redefinition of '", mangle(decl), "'");
    }
    getCurrentModule()->getSymbolTable().addGlobal(mangle(decl), &decl);
}

void Typechecker::addToSymbolTable(TypeTemplate& decl) const {
    addToSymbolTableWithName(decl, decl.getTypeDecl()->getName(), true);
}

void Typechecker::addToSymbolTable(TypeDecl& decl) const {
    addToSymbolTableWithName(decl, mangle(decl), true);

    for (auto& memberDecl : decl.getMemberDecls()) {
        if (auto* nonTemplateMethod = llvm::dyn_cast<MethodDecl>(memberDecl.get())) {
            addToSymbolTable(*nonTemplateMethod);
        }
    }
}

void Typechecker::addToSymbolTable(EnumDecl& decl) const {
    addToSymbolTableWithName(decl, mangle(decl), true);
}

void Typechecker::addToSymbolTable(VarDecl& decl, bool global) const {
    addToSymbolTableWithName(decl, decl.getName(), global);
}

template<typename DeclT>
void Typechecker::addToSymbolTableNonAST(DeclT& decl) const {
    std::string name = decl.getName();
    nonASTDecls.push_back(llvm::make_unique<DeclT>(std::move(decl)));
    getCurrentModule()->getSymbolTable().add(std::move(name), nonASTDecls.back().get());

    if (std::is_same<DeclT, TypeDecl>::value) {
        auto& typeDecl = llvm::cast<TypeDecl>(*nonASTDecls.back());
        llvm::cast<BasicType>(*typeDecl.getType()).setDecl(&typeDecl);
    }
}

void Typechecker::addToSymbolTable(FunctionDecl&& decl) const {
    addToSymbolTableNonAST(decl);
}

void Typechecker::addToSymbolTable(TypeDecl&& decl) const {
    addToSymbolTableNonAST(decl);
}

void Typechecker::addToSymbolTable(VarDecl&& decl) const {
    addToSymbolTableNonAST(decl);
}

void Typechecker::addIdentifierReplacement(llvm::StringRef source, llvm::StringRef target) const {
    ASSERT(!target.empty());
    getCurrentModule()->getSymbolTable().addIdentifierReplacement(source, target);
}

template<typename ModuleContainer>
static llvm::SmallVector<Decl*, 1> findDeclsInModules(llvm::StringRef name,
                                                      const ModuleContainer& modules) {
    llvm::SmallVector<Decl*, 1> decls;

    for (auto& module : modules) {
        llvm::ArrayRef<Decl*> matches = module->getSymbolTable().find(name);
        decls.append(matches.begin(), matches.end());
    }

    return decls;
}

template<typename ModuleContainer>
static Decl* findDeclInModules(llvm::StringRef name, SourceLocation location, const ModuleContainer& modules) {
    auto decls = findDeclsInModules(name, modules);

    switch (decls.size()) {
        case 1: return decls[0];
        case 0: return nullptr;
        default: error(location, "ambiguous reference to '", name, "'");
    }
}

llvm::ArrayRef<std::shared_ptr<Module>> getStdlibModules() {
    auto it = allImportedModules.find("std");
    if (it == allImportedModules.end()) return {};
    return it->second;
}

Decl& Typechecker::findDecl(llvm::StringRef name, SourceLocation location, bool everywhere) const {
    ASSERT(!name.empty());

    if (Decl* match = findDeclInModules(name, location, llvm::makeArrayRef(getCurrentModule()))) {
        return *match;
    }

    for (auto& field : currentFieldDecls) {
        if (field.first->getName() == name) {
            return *field.first;
        }
    }

    if (Decl* match = findDeclInModules(name, location, getStdlibModules())) {
        return *match;
    }

    if (everywhere) {
        if (Decl* match = findDeclInModules(name, location, getAllImportedModules())) {
            return *match;
        }
    } else {
        if (Decl* match = findDeclInModules(name, location, getCurrentSourceFile()->getImportedModules())) {
            return *match;
        }
    }

    error(location, "unknown identifier '", name, "'");
}

std::vector<Decl*> Typechecker::findDecls(llvm::StringRef name, bool everywhere,
                                          TypeDecl* receiverTypeDecl) const {
    std::vector<Decl*> decls;

    if (!receiverTypeDecl && currentFunction) {
        receiverTypeDecl = currentFunction->getTypeDecl();
    }

    if (receiverTypeDecl) {
        for (auto& decl : receiverTypeDecl->getMemberDecls()) {
            if (auto* functionDecl = llvm::dyn_cast<FunctionDecl>(decl.get())) {
                if (functionDecl->getName() == name) {
                    decls.emplace_back(decl.get());
                }
            } else if (auto* functionTemplate = llvm::dyn_cast<FunctionTemplate>(decl.get())) {
                if (mangle(*functionTemplate) == name) {
                    decls.emplace_back(decl.get());
                }
            }
        }

        for (auto& field : receiverTypeDecl->getFields()) {
            if (field.getName() == name || mangle(field) == name) {
                decls.emplace_back(&field);
            }
        }
    }

    if (getCurrentModule()->getName() != "std") {
        append(decls, findDeclsInModules(name, llvm::makeArrayRef(getCurrentModule())));
    }

    append(decls, findDeclsInModules(name, getStdlibModules()));
    append(decls, everywhere ? findDeclsInModules(name, getAllImportedModules())
                             : findDeclsInModules(name, getCurrentSourceFile()->getImportedModules()));
    return decls;
}

bool allPathsReturn(llvm::ArrayRef<std::unique_ptr<Stmt>> block) {
    if (block.empty()) return false;

    switch (block.back()->getKind()) {
        case StmtKind::ReturnStmt:
            return true;
        case StmtKind::IfStmt: {
            auto& ifStmt = llvm::cast<IfStmt>(*block.back());
            return allPathsReturn(ifStmt.getThenBody()) && allPathsReturn(ifStmt.getElseBody());
        }
        case StmtKind::SwitchStmt: {
            auto& switchStmt = llvm::cast<SwitchStmt>(*block.back());
            return llvm::all_of(switchStmt.getCases(),
                                [](const SwitchCase& c) { return allPathsReturn(c.getStmts()); })
                   && allPathsReturn(switchStmt.getDefaultStmts());
        }
        default:
            return false;
    }
}

void Typechecker::typecheckGenericParamDecls(llvm::ArrayRef<GenericParamDecl> genericParams) {
    for (auto& genericParam : genericParams) {
        if (getCurrentModule()->getSymbolTable().contains(genericParam.getName())) {
            error(genericParam.getLocation(), "redefinition of '", genericParam.getName(), "'");
        }

        for (Type constraint : genericParam.getConstraints()) {
            typecheckType(constraint, genericParam.getLocation());
        }
    }
}

void Typechecker::typecheckParams(llvm::MutableArrayRef<ParamDecl> params) {
    for (auto& param : params) {
        typecheckParamDecl(param);
    }
}

void Typechecker::typecheckFunctionDecl(FunctionDecl& decl) {
    if (decl.isTypechecked()) return;
    if (decl.isExtern()) return; // TODO: Typecheck parameters and return type of extern functions.

    TypeDecl* receiverTypeDecl = decl.getTypeDecl();

    getCurrentModule()->getSymbolTable().pushScope();
    SAVE_STATE(currentFunction);
    currentFunction = &decl;

    typecheckParams(decl.getParams());

    typecheckType(decl.getReturnType(), decl.getLocation());
    if (decl.getReturnType().isMutable()) error(decl.getLocation(), "return types cannot be 'mutable'");

    if (!decl.isExtern()) {
        SAVE_STATE(functionReturnType);
        functionReturnType = decl.getReturnType();
        SAVE_STATE(currentFieldDecls);
        if (receiverTypeDecl) {
            for (auto& field : receiverTypeDecl->getFields()) {
                currentFieldDecls.emplace_back(&field, false);
            }

            Type thisType = receiverTypeDecl->getTypeForPassing(decl.isMutating());
            addToSymbolTable(VarDecl(thisType, "this", nullptr, &decl, *getCurrentModule(), decl.getLocation()));
        }

        bool delegatedInit = false;

        if (decl.hasBody()) {
            for (auto& stmt : decl.getBody()) {
                typecheckStmt(*stmt);

                if (!decl.isInitDecl()) continue;

                if (auto* exprStmt = llvm::dyn_cast<ExprStmt>(stmt.get())) {
                    if (auto* callExpr = llvm::dyn_cast<CallExpr>(&exprStmt->getExpr())) {
                        if (auto* initDecl = llvm::dyn_cast<InitDecl>(callExpr->getCalleeDecl())) {
                            if (initDecl->getTypeDecl() == receiverTypeDecl) {
                                delegatedInit = true;
                            }
                        }
                    }
                }
            }
        }

        if (decl.isInitDecl() && !delegatedInit) {
            for (auto& fieldAndInitialized : currentFieldDecls) {
                if (!fieldAndInitialized.second) {
                    warning(decl.getLocation(), "initializer doesn't initialize member variable '",
                            fieldAndInitialized.first->getName(), "'");
                }
            }
        }
    }

    getCurrentModule()->getSymbolTable().popScope();

    if ((!receiverTypeDecl || !receiverTypeDecl->isInterface())
        && !decl.getReturnType().isVoid() && !allPathsReturn(decl.getBody())) {
        error(decl.getLocation(), "'", decl.getName(), "' is missing a return statement");
    }

    decl.setTypechecked(true);
}

void Typechecker::typecheckFunctionTemplate(FunctionTemplate& decl) {
    typecheckGenericParamDecls(decl.getGenericParams());
}

void Typechecker::typecheckTypeDecl(TypeDecl& decl) {
    for (Type interface : decl.getInterfaces()) {
        if (interface.isBasicType()) {
            if (auto* interfaceDecl = getTypeDecl(llvm::cast<BasicType>(*interface))) {
                if (interfaceDecl->isInterface()) {
                    checkImplementsInterface(decl, *interfaceDecl, decl.getLocation());

                    for (auto& method : interfaceDecl->getMethods()) {
                        auto& methodDecl = llvm::cast<MethodDecl>(*method);

                        if (methodDecl.hasBody()) {
                            auto copy = methodDecl.instantiate({{ "This", decl.getType() }}, decl);
                            addToSymbolTable(*copy);
                            decl.addMethod(std::move(copy));
                        }
                    }

                    continue;
                }
            }
        }
        error(decl.getLocation(), "'", interface, "' is not an interface");
    }

    TypeDecl* realDecl;
    std::unique_ptr<TypeDecl> thisTypeResolved;

    if (decl.isInterface()) {
        thisTypeResolved = llvm::cast<TypeDecl>(decl.instantiate({{ "This", decl.getType() }}, {}));
        realDecl = thisTypeResolved.get();
    } else {
        realDecl = &decl;
    }

    for (auto& fieldDecl : realDecl->getFields()) {
        typecheckFieldDecl(fieldDecl);
    }

    for (auto& memberDecl : realDecl->getMemberDecls()) {
        typecheckMemberDecl(*memberDecl);
    }

    llvm::cast<BasicType>(*decl.getType()).setDecl(&decl);
}

void Typechecker::typecheckTypeTemplate(TypeTemplate& decl) {
    typecheckGenericParamDecls(decl.getGenericParams());
}

void Typechecker::typecheckEnumDecl(EnumDecl& decl) {
    std::vector<const EnumCase*> cases = map(decl.getCases(), [](const EnumCase& c) { return &c; });

    std::sort(cases.begin(), cases.end(), [](const EnumCase* a, const EnumCase* b) {
        return a->getName() < b->getName();
    });

    auto it = std::adjacent_find(cases.begin(), cases.end(), [](const EnumCase* a, const EnumCase* b) {
        return a->getName() == b->getName();
    });

    if (it != cases.end()) {
        error((*it)->getLocation(), "duplicate enum case '", (*it)->getName(), "'");
    }

    for (auto& enumCase : decl.getCases()) {
        typecheckExpr(*enumCase.getValue());
    }

    llvm::cast<BasicType>(*decl.getEnumType()).setDecl(&decl);
}

TypeDecl* Typechecker::getTypeDecl(const BasicType& type) {
    if (auto* typeDecl = type.getDecl()) {
        return typeDecl;
    }

    auto decls = findDecls(mangleTypeDecl(type.getName(), type.getGenericArgs()));

    if (!decls.empty()) {
        ASSERT(decls.size() == 1);
        return llvm::dyn_cast_or_null<TypeDecl>(decls[0]);
    }

    decls = findDecls(mangleTypeDecl(type.getName(), {}));
    if (decls.empty()) return nullptr;
    ASSERT(decls.size() == 1);
    auto instantiation = llvm::cast<TypeTemplate>(decls[0])->instantiate(type.getGenericArgs());
    addToSymbolTable(*instantiation);
    declsToTypecheck.push_back(instantiation);
    return instantiation;
}

void Typechecker::typecheckVarDecl(VarDecl& decl, bool isGlobal) {
    if (!isGlobal && getCurrentModule()->getSymbolTable().contains(decl.getName())) {
        error(decl.getLocation(), "redefinition of '", decl.getName(), "'");
    }

    Type declaredType = decl.getType();
    Type initType = nullptr;

    if (decl.getInitializer()) {
        initType = typecheckExpr(*decl.getInitializer());
    } else if (isGlobal) {
        error(decl.getLocation(), "global variables cannot be uninitialized");
    }

    if (declaredType) {
        typecheckType(declaredType, decl.getLocation());

        if (initType) {
            Type convertedType;

            if (isImplicitlyConvertible(decl.getInitializer(), initType, declaredType, &convertedType)) {
                decl.getInitializer()->setType(convertedType ? convertedType : initType);
            } else {
                const char* hint;

                if (initType.isNull()) {
                    ASSERT(!declaredType.isOptionalType());
                    hint = " (add '?' to the type to make it nullable)";
                } else {
                    hint = "";
                }

                error(decl.getInitializer()->getLocation(), "cannot initialize variable of type '", declaredType,
                      "' with '", initType, "'", hint);
            }
        }
    } else {
        if (initType.isNull()) {
            error(decl.getLocation(), "couldn't infer type of '", decl.getName(), "', add a type annotation");
        }

        initType.setMutable(decl.getType().isMutable());
        decl.setType(initType);
    }

    if (!isGlobal) addToSymbolTable(decl, false);

    if (decl.getInitializer() && !isImplicitlyCopyable(decl.getType())) {
        decl.getInitializer()->setMoved(true);
    }
}

void Typechecker::typecheckFieldDecl(FieldDecl& decl) {
    typecheckType(decl.getType(), decl.getLocation());
}

std::error_code parseSourcesInDirectoryRecursively(llvm::StringRef directoryPath, Module& module,
                                                   ParserFunction& parse) {
    std::error_code error;
    llvm::sys::fs::recursive_directory_iterator it(directoryPath, error), end;

    for (; it != end; it.increment(error)) {
        if (error) break;

        if (llvm::sys::path::extension(it->path()) == ".delta") {
            parse(it->path(), module);
        }
    }

    return error;
}

llvm::ErrorOr<const Module&> importDeltaModule(SourceFile* importer,
                                               const PackageManifest* manifest,
                                               llvm::ArrayRef<std::string> importSearchPaths,
                                               llvm::ArrayRef<std::string> frameworkSearchPaths,
                                               ParserFunction& parse,
                                               llvm::StringRef moduleExternalName,
                                               llvm::StringRef moduleInternalName = "") {
    if (moduleInternalName.empty()) moduleInternalName = moduleExternalName;

    auto it = allImportedModules.find(moduleInternalName);
    if (it != allImportedModules.end()) {
        if (importer) importer->addImportedModule(it->second);
        return *it->second;
    }

    auto module = std::make_shared<Module>(moduleInternalName);
    std::error_code error;

    if (manifest) {
        for (auto& dependency : manifest->getDeclaredDependencies()) {
            if (dependency.getPackageIdentifier() == moduleInternalName) {
                error = parseSourcesInDirectoryRecursively(dependency.getFileSystemPath(), *module, parse);
                goto done;
            }
        }
    }

    for (llvm::StringRef importPath : importSearchPaths) {
        llvm::sys::fs::directory_iterator it(importPath, error), end;
        for (; it != end; it.increment(error)) {
            if (error) goto done;
            if (!llvm::sys::fs::is_directory(it->path())) continue;
            if (llvm::sys::path::filename(it->path()) != moduleExternalName) continue;

            error = parseSourcesInDirectoryRecursively(it->path(), *module, parse);
            goto done;
        }
    }

done:
    if (error || module->getSourceFiles().empty()) {
        return error;
    }

    if (importer) importer->addImportedModule(module);
    allImportedModules[module->getName()] = module;
    typecheckModule(*module, /* TODO: Pass the package manifest of `module` here. */ nullptr,
                    importSearchPaths, frameworkSearchPaths, parse);
    return *module;
}

void Typechecker::typecheckImportDecl(ImportDecl& decl, const PackageManifest* manifest,
                                      llvm::ArrayRef<std::string> importSearchPaths,
                                      llvm::ArrayRef<std::string> frameworkSearchPaths,
                                      ParserFunction& parse) {
    if (importDeltaModule(currentSourceFile, manifest, importSearchPaths, frameworkSearchPaths,
                          parse, decl.getTarget())) {
        return;
    }

    if (!importCHeader(*currentSourceFile, decl.getTarget(), importSearchPaths, frameworkSearchPaths)) {
        error(decl.getLocation(), "couldn't find module or C header '", decl.getTarget(), "'");
    }
}

void Typechecker::typecheckTopLevelDecl(Decl& decl, const PackageManifest* manifest,
                                        llvm::ArrayRef<std::string> importSearchPaths,
                                        llvm::ArrayRef<std::string> frameworkSearchPaths,
                                        ParserFunction& parse) {
    switch (decl.getKind()) {
        case DeclKind::ParamDecl: llvm_unreachable("no top-level parameter declarations");
        case DeclKind::FunctionDecl: typecheckFunctionDecl(llvm::cast<FunctionDecl>(decl)); break;
        case DeclKind::MethodDecl: llvm_unreachable("no top-level method declarations");
        case DeclKind::GenericParamDecl: llvm_unreachable("no top-level parameter declarations");
        case DeclKind::InitDecl: llvm_unreachable("no top-level initializer declarations");
        case DeclKind::DeinitDecl: llvm_unreachable("no top-level deinitializer declarations");
        case DeclKind::FunctionTemplate: typecheckFunctionTemplate(llvm::cast<FunctionTemplate>(decl)); break;
        case DeclKind::TypeDecl: typecheckTypeDecl(llvm::cast<TypeDecl>(decl)); break;
        case DeclKind::TypeTemplate: typecheckTypeTemplate(llvm::cast<TypeTemplate>(decl)); break;
        case DeclKind::EnumDecl: typecheckEnumDecl(llvm::cast<EnumDecl>(decl)); break;
        case DeclKind::VarDecl: typecheckVarDecl(llvm::cast<VarDecl>(decl), true); break;
        case DeclKind::FieldDecl: llvm_unreachable("no top-level field declarations");
        case DeclKind::ImportDecl: typecheckImportDecl(llvm::cast<ImportDecl>(decl), manifest,
                                                       importSearchPaths, frameworkSearchPaths, parse); break;
    }
}

void Typechecker::typecheckMemberDecl(Decl& decl) {
    switch (decl.getKind()) {
        case DeclKind::MethodDecl:
        case DeclKind::InitDecl:
        case DeclKind::DeinitDecl: typecheckFunctionDecl(llvm::cast<MethodDecl>(decl)); break;
        case DeclKind::FieldDecl: typecheckFieldDecl(llvm::cast<FieldDecl>(decl)); break;
        case DeclKind::FunctionTemplate: typecheckFunctionTemplate(llvm::cast<FunctionTemplate>(decl)); break;
        default: llvm_unreachable("invalid member declaration kind");
    }
}

void Typechecker::postProcess() {
    SAVE_STATE(isPostProcessing);
    isPostProcessing = true;

    while (!declsToTypecheck.empty()) {
        auto currentDeclsToTypecheck = std::move(declsToTypecheck);

        for (auto* decl : currentDeclsToTypecheck) {
            switch (decl->getKind()) {
                case DeclKind::FunctionDecl:
                case DeclKind::MethodDecl:
                case DeclKind::InitDecl:
                case DeclKind::DeinitDecl:
                    typecheckFunctionDecl(*llvm::cast<FunctionDecl>(decl));
                    break;
                case DeclKind::FunctionTemplate:
                    typecheckFunctionTemplate(*llvm::cast<FunctionTemplate>(decl));
                    break;
                case DeclKind::TypeDecl:
                    typecheckTypeDecl(*llvm::cast<TypeDecl>(decl));
                    break;
                default:
                    llvm_unreachable("invalid deferred decl");
            }
        }
    }
}

static void checkUnusedDecls(const Module& module) {
    for (auto& sourceFile : module.getSourceFiles()) {
        for (auto& decl : sourceFile.getTopLevelDecls()) {
            if (decl->isReferenced()) continue;

            if (decl->isFunctionDecl() || decl->isFunctionTemplate()) {
                if (decl->getName() == "main") continue;

                warning(decl->getLocation(), "unused declaration '", decl->getName(), "'");
            }
        }
    }
}

void delta::typecheckModule(Module& module, const PackageManifest* manifest,
                            llvm::ArrayRef<std::string> importSearchPaths,
                            llvm::ArrayRef<std::string> frameworkSearchPaths,
                            ParserFunction& parse) {
    auto stdlibModule = importDeltaModule(nullptr, nullptr, importSearchPaths, frameworkSearchPaths,
                                          parse, "stdlib", "std");
    if (!stdlibModule) {
        printErrorAndExit("couldn't import the standard library: ", stdlibModule.getError().message());
    }

    // Infer the types of global variables for use before their declaration.
    for (auto& sourceFile : module.getSourceFiles()) {
        Typechecker typechecker(&module, &sourceFile);

        for (auto& decl : sourceFile.getTopLevelDecls()) {
            if (auto* varDecl = llvm::dyn_cast<VarDecl>(decl.get())) {
                typechecker.typecheckVarDecl(*varDecl, true);
            }
        }

        typechecker.postProcess();
    }

    for (auto& sourceFile : module.getSourceFiles()) {
        Typechecker typechecker(&module, &sourceFile);

        for (auto& decl : sourceFile.getTopLevelDecls()) {
            if (!decl->isVarDecl()) {
                typechecker.typecheckTopLevelDecl(*decl, manifest, importSearchPaths,
                                                  frameworkSearchPaths, parse);
                typechecker.postProcess();
            }
        }
    }

    if (module.getName() != "std") {
        checkUnusedDecls(module);
    }
}
