#include "irgen.h"
#pragma warning(push, 0)
#include <llvm/Support/SaveAndRestore.h>
#pragma warning(pop)
#include "../ast/mangle.h"

using namespace delta;

IRFunction* IRGenerator::getFunctionProto(const FunctionDecl& decl) {

}

void IRGenerator::emitFunctionBody(const FunctionDecl& decl, IRFunction& function) {
    setInsertPoint(new IRBasicBlock("", &function));
}

void IRGenerator::emitFunctionDecl(const FunctionDecl& decl) {
    IRFunction* function = getFunctionProto(decl);

    if (!decl.isExtern() && function->body.empty()) {
        emitFunctionBody(decl, *function);
    }

    module->functions.push_back(function);
}

std::vector<Type> IRGenerator::getFieldTypes(const TypeDecl& decl) {
    return map(decl.getFields(), [&](auto& field) { return getLLVMType(field.getType(), field.getLocation()); });
}

void IRGenerator::emitDecl(const Decl& decl) {
    llvm::SaveAndRestore setCurrentDecl(currentDecl, &decl);

    switch (decl.getKind()) {
        case DeclKind::ParamDecl:
            llvm_unreachable("handled via FunctionDecl");
        case DeclKind::FunctionDecl:
        case DeclKind::MethodDecl:
            emitFunctionDecl(llvm::cast<FunctionDecl>(decl));
            break;
        case DeclKind::GenericParamDecl:
            llvm_unreachable("cannot emit generic parameter declaration");
        case DeclKind::ConstructorDecl:
            emitFunctionDecl(llvm::cast<ConstructorDecl>(decl));
            break;
        case DeclKind::DestructorDecl:
            emitFunctionDecl(llvm::cast<DestructorDecl>(decl));
            break;
        case DeclKind::VarDecl:
            emitVarDecl(llvm::cast<VarDecl>(decl));
            break;
        case DeclKind::FieldDecl:
            llvm_unreachable("handled via TypeDecl");
        case DeclKind::ImportDecl:
        case DeclKind::FunctionTemplate:
        case DeclKind::TypeDecl:
        case DeclKind::TypeTemplate:
        case DeclKind::EnumDecl:
        case DeclKind::EnumCase:
            break;
    }
}
