#include "irgen.h"
#pragma warning(push, 0)
#include <llvm/ADT/StringSwitch.h>
#pragma warning(pop)
#include "../ast/module.h"

using namespace delta;

void IRGenScope::onScopeEnd() {
    for (const Expr* expr : reverse(deferredExprs)) {
        irGenerator->emitExpr(*expr);
    }

    for (auto& p : reverse(destructorsToCall)) {
        if (p.decl && p.decl->hasBeenMoved()) continue;
        irGenerator->createDestructorCall(p.function, p.value);
    }
}

void IRGenScope::clear() {
    deferredExprs.clear();
    destructorsToCall.clear();
}

IRGenerator::IRGenerator() {
    scopes.push_back(IRGenScope(*this));
}

void IRGenerator::setLocalValue(IRValue* value, const VariableDecl* decl) {
    auto it = scopes.back().valuesByDecl.try_emplace(decl, value);
    ASSERT(it.second);

    if (decl) {
        deferDestructorCall(value, decl);
    }
}

IRValue* IRGenerator::getValueOrNull(const Decl* decl) {
    for (auto& scope : reverse(scopes)) {
        if (auto* value = scope.valuesByDecl.lookup(decl)) {
            return value;
        }
    }

    return nullptr;
}

IRValue* IRGenerator::getValue(const Decl* decl) {
    if (auto* value = getValueOrNull(decl)) {
        return value;
    }

    switch (decl->getKind()) {
        case DeclKind::VarDecl:
            return emitVarDecl(*llvm::cast<VarDecl>(decl));
        case DeclKind::FieldDecl:
            return emitMemberAccess(getThis(), llvm::cast<FieldDecl>(decl));
        case DeclKind::FunctionDecl:
            return getFunctionProto(*llvm::cast<FunctionDecl>(decl));
        default:
            llvm_unreachable("all cases handled");
    }
}

IRValue* IRGenerator::getThis(Type targetType) {
    auto value = getValue(nullptr);
    return value;
}

void IRGenerator::beginScope() {
    scopes.push_back(IRGenScope(*this));
}

void IRGenerator::endScope() {
    scopes.back().onScopeEnd();
    scopes.pop_back();
}

void IRGenerator::deferEvaluationOf(const Expr& expr) {
    scopes.back().deferredExprs.push_back(&expr);
}

/// Returns a destructor that only calls the destructors of the member variables, or null if
/// no such destructor is needed because none of the member variables have destructors.
DestructorDecl* IRGenerator::getDefaultDestructor(const TypeDecl& typeDecl) {
    ASSERT(typeDecl.getDestructor() == nullptr);

    for (auto& field : typeDecl.getFields()) {
        if (field.getType().getDestructor() != nullptr) {
            auto destructor = new DestructorDecl(const_cast<TypeDecl&>(typeDecl), typeDecl.getLocation());
            destructor->setBody({});
            return destructor;
        }
    }

    return nullptr;
}

void IRGenerator::deferDestructorCall(IRValue* receiver, const VariableDecl* decl) {
    auto type = decl->getType();
    IRFunction* proto = nullptr;

    if (auto* destructor = type.getDestructor()) {
        proto = getFunctionProto(*destructor);
    } else if (auto* typeDecl = type.getDecl()) {
        if (auto defaultDestructor = getDefaultDestructor(*typeDecl)) {
            proto = getFunctionProto(*defaultDestructor);
        }
    }

    if (proto) {
        scopes.back().destructorsToCall.push_back({proto, receiver, decl});
    }
}

void IRGenerator::emitDeferredExprsAndDestructorCallsForReturn() {
    for (auto& scope : llvm::reverse(scopes)) {
        scope.onScopeEnd();
    }
    scopes.back().clear();
}

IRAllocaInst* IRGenerator::createEntryBlockAlloca(Type type, const llvm::Twine& name) {
    auto* alloca = new IRAllocaInst{{}, type, name.str()};
    insertBlock->parent->body.front().instructions.push_back(alloca);
    return alloca;
}

IRAllocaInst* IRGenerator::createTempAlloca(IRValue* value, const llvm::Twine& name) {
    auto* alloca = createEntryBlockAlloca(value->getType(), name);
    createStore(value, alloca);
    return alloca;
}

IRValue* IRGenerator::createLoad(IRValue* value) {
    insertBlock->instructions.push_back();
    //    return createLoad(value, value->getName() + ".load");
}

void IRGenerator::createStore(IRValue* value, IRValue* pointer) {
    ASSERT(pointer->getType().isPointerType());
    ASSERT(pointer->getType().getPointee() == value->getType());

    insertBlock->instructions.push_back();
    //    createStore(value, pointer);
}

void IRGenerator::createCall(IRValue* function, llvm::ArrayRef<IRValue*> args) {
    auto call = new IRCallInst{{}, function, args};
    insertBlock->instructions.push_back(call);
}

IRValue* IRGenerator::emitAssignmentLHS(const Expr& lhs) {
    IRValue* value = emitLvalueExpr(lhs);

    // Don't call destructor for LHS when assigning to fields in constructor.
    if (auto* constructorDecl = llvm::dyn_cast<ConstructorDecl>(currentDecl)) {
        if (auto* varExpr = llvm::dyn_cast<VarExpr>(&lhs)) {
            if (auto* fieldDecl = llvm::dyn_cast<FieldDecl>(varExpr->getDecl())) {
                if (fieldDecl->getParentDecl() == constructorDecl->getTypeDecl()) {
                    return value;
                }
            }
        }
    }

    // Call destructor for LHS.
    if (auto* basicType = llvm::dyn_cast<BasicType>(lhs.getType().getBase())) {
        if (auto* typeDecl = basicType->getDecl()) {
            if (auto* destructor = typeDecl->getDestructor()) {
                createDestructorCall(getFunctionProto(*destructor), value);
            }
        }
    }

    return value;
}

void IRGenerator::createDestructorCall(IRFunction* destructor, IRValue* receiver) {
    if (!receiver->getType().isPointerType()) {
        receiver = createTempAlloca(receiver);
    }
    createCall(destructor, receiver);
}

IRValue* IRGenerator::getFunctionForCall(const CallExpr& call) {
    const Decl* decl = call.getCalleeDecl();
    if (!decl) return nullptr;

    switch (decl->getKind()) {
        case DeclKind::FunctionDecl:
        case DeclKind::MethodDecl:
        case DeclKind::ConstructorDecl:
        case DeclKind::DestructorDecl:
            return getFunctionProto(*llvm::cast<FunctionDecl>(decl));
        case DeclKind::VarDecl:
        case DeclKind::ParamDecl:
            return getValue(decl);
        case DeclKind::FieldDecl:
            if (call.getReceiver()) {
                return emitMemberAccess(emitLvalueExpr(*call.getReceiver()), llvm::cast<FieldDecl>(decl));
            } else {
                return getValue(decl);
            }
        default:
            llvm_unreachable("invalid callee decl");
    }
}

IRModule& IRGenerator::emitModule(const Module& sourceModule) {
    ASSERT(!module);
    module = new IRModule;
    module->name = sourceModule.getName();

    for (auto& sourceFile : sourceModule.getSourceFiles()) {
        for (auto& decl : sourceFile.getTopLevelDecls()) {
            emitDecl(*decl);
        }
    }

    for (size_t i = 0; i < functionInstantiations.size(); ++i) {
        auto& instantiation = functionInstantiations[i];

        if (!instantiation.decl->isExtern() && instantiation.function->empty()) {
            currentDecl = instantiation.decl;
            emitFunctionBody(*instantiation.decl, *instantiation.function);
        }
    }

    generatedModules.push_back(module);
    module = nullptr;
    return *generatedModules.back();
}
