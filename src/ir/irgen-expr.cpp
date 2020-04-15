#include "irgen.h"
#include "../ast/module.h"
#include "../support/utility.h"

using namespace delta;

IRValue* IRGenerator::emitVarExpr(const VarExpr& expr) {
    return getValue(expr.getDecl());
}

IRValue* IRGenerator::emitStringLiteralExpr(const StringLiteralExpr& expr) {
    ASSERT(insertBlock, "CreateGlobalStringPtr requires block to insert into");
    auto* stringPtr = createGlobalStringPtr(expr.getValue());
    auto* size = ConstantInt::get(Type::getInt(), expr.getValue().size());
    auto* type = getLLVMType(BasicType::get("string", {}));
    auto* alloca = createEntryBlockAlloca(type, "__str");
    IRFunction* stringConstructor = nullptr;

    for (auto* decl : Module::getStdlibModule()->getSymbolTable().find("string.init")) {
        auto params = llvm::cast<ConstructorDecl>(decl)->getParams();
        if (params.size() == 2 && params[0].getType().isPointerType() && params[1].getType().isInt()) {
            stringConstructor = getFunctionProto(*llvm::cast<ConstructorDecl>(decl));
            break;
        }
    }

    ASSERT(stringConstructor);
    createCall(stringConstructor, {alloca, stringPtr, size});
    return alloca;
}

IRValue* IRGenerator::emitCharacterLiteralExpr(const CharacterLiteralExpr& expr) {
    return ConstantInt::get(getLLVMType(expr.getType()), expr.getValue());
}

IRValue* IRGenerator::emitIntLiteralExpr(const IntLiteralExpr& expr) {
    auto type = getLLVMType(expr.getType());
    // Integer literals may be typed as floating-point when used in a context
    // that requires a floating-point value. It might make sense to combine
    // IntLiteralExpr and FloatLiteralExpr into a single class.
    if (expr.getType().isFloatingPoint()) {
        return ConstantFP::get(type, expr.getValue().roundToDouble());
    }
    return ConstantInt::get(type, expr.getValue().extOrTrunc(type->getIntegerBitWidth()));
}

IRValue* IRGenerator::emitFloatLiteralExpr(const FloatLiteralExpr& expr) {
    return ConstantFP::get(getLLVMType(expr.getType()), expr.getValue());
}

IRValue* IRGenerator::emitBoolLiteralExpr(const BoolLiteralExpr& expr) {
    return expr.getValue() ? ConstantInt::getTrue(ctx) : ConstantInt::getFalse(ctx);
}

IRValue* IRGenerator::emitNullLiteralExpr(const NullLiteralExpr& expr) {
    if (expr.getType().isPointerTypeInLLVM()) {
        return ConstantPointerNull::get(llvm::cast<PointerType>(getLLVMType(expr.getType())));
    } else {
        return emitOptionalConstruction(expr.getType().getWrappedType(), nullptr);
    }
}

IRValue* IRGenerator::emitOptionalConstruction(Type wrappedType, IRValue* arg) {
    auto* decl = Module::getStdlibModule()->getSymbolTable().findOne("Optional");
    auto typeTemplate = llvm::cast<TypeTemplate>(decl);
    auto typeDecl = typeTemplate->instantiate(wrappedType);
    IRFunction* optionalConstructor = nullptr;

    for (auto* constructor : typeDecl->getConstructors()) {
        if (constructor->getParams().size() == (arg ? 1 : 0)) {
            optionalConstructor = getFunctionProto(*constructor);
            break;
        }
    }

    ASSERT(optionalConstructor);
    auto* alloca = createEntryBlockAlloca(getLLVMType(typeDecl->getType()));
    llvm::SmallVector<IRValue*, 2> args;
    args.push_back(alloca);
    if (arg) args.push_back(arg);
    createCall(optionalConstructor, args);
    return alloca;
}

IRValue* IRGenerator::emitUndefinedLiteralExpr(const UndefinedLiteralExpr& expr) {
    return UndefValue::get(getLLVMType(expr.getType()));
}

IRValue* IRGenerator::emitArrayLiteralExpr(const ArrayLiteralExpr& expr) {
    auto* arrayType = ArrayType::get(getLLVMType(expr.getElements()[0]->getType()), expr.getElements().size());
    IRValue* array = UndefValue::get(arrayType);
    unsigned index = 0;
    for (auto& element : expr.getElements()) {
        auto* value = emitExpr(*element);
        array = createInsertValue(array, value, index++);
    }
    return array;
}

IRValue* IRGenerator::emitTupleExpr(const TupleExpr& expr) {
    IRValue* tuple = UndefValue::get(getLLVMType(expr.getType()));
    unsigned index = 0;
    for (auto& element : expr.getElements()) {
        tuple = createInsertValue(tuple, emitExpr(*element.getValue()), index++);
    }
    return tuple;
}

IRValue* IRGenerator::emitImplicitNullComparison(IRValue* operand) {
    auto* pointerType = llvm::cast<PointerType>(operand->getType());
    return createICmpNE(operand, ConstantPointerNull::get(pointerType));
}

IRValue* IRGenerator::emitNot(const UnaryExpr& expr) {
    auto* operand = emitExpr(expr.getOperand());
    if (operand->getType().isPointerType()) {
        operand = emitImplicitNullComparison(operand);
    }
    return createNot(operand);
}

IRValue* IRGenerator::emitUnaryExpr(const UnaryExpr& expr) {
    switch (expr.getOperator()) {
        case Token::Plus:
            return emitExpr(expr.getOperand());
        case Token::Minus: {
            auto operand = emitExpr(expr.getOperand());
            return expr.getOperand().getType().isFloatingPoint() ? createFNeg(operand) : createNeg(operand);
        }
        case Token::Star:
            return emitExpr(expr.getOperand());
        case Token::And:
            return emitExprAsPointer(expr.getOperand());
        case Token::Not:
            // FIXME: Temporary hack. Lower implicit null checks such as `if (ptr)` and `if (!ptr)` when expression lowering is implemented.
            if (expr.getOperand().getType().isOptionalType() && !expr.getOperand().getType().getWrappedType().isPointerType()) {
                auto operand = emitExpr(expr.getOperand());
                auto hasValue = createExtractValue(operand, optionalHasValueFieldIndex);
                return createNot(hasValue);
            }
            LLVM_FALLTHROUGH;
        case Token::Tilde:
            return emitNot(expr);
        case Token::Increment:
            return emitConstantIncrement(expr, 1);
        case Token::Decrement:
            return emitConstantIncrement(expr, -1);
        default:
            llvm_unreachable("invalid prefix operator");
    }
}

// TODO: Lower increment and decrement statements to compound assignments so this isn't needed.
IRValue* IRGenerator::emitConstantIncrement(const UnaryExpr& expr, int increment) {
    auto operandType = expr.getOperand().getType();
    auto* ptr = emitLvalueExpr(expr.getOperand());
    if (operandType.isPointerType() && llvm::isa<IRAllocaInst>(ptr)) {
        ptr = createLoad(ptr);
    }
    auto* value = createLoad(ptr);
    IRValue* result;

    if (value->getType().isInteger()) {
        result = createAdd(value, ConstantInt::getSigned(value->getType(), increment));
    } else if (value->getType().isPointerType()) {
        result = createInBoundsGEP(value, ConstantInt::getSigned(IntegerType::getInt(), increment));
    } else if (value->getType().isFloatingPoint()) {
        result = createFAdd(value, ConstantFP::get(value->getType(), increment));
    } else {
        llvm_unreachable("unknown increment/decrement operand type");
    }

    createStore(result, ptr);
    return nullptr;
}

IRValue* IRGenerator::emitLogicalAnd(const Expr& left, const Expr& right) {
    auto* rhsBlock = new IRBasicBlock("and.rhs", insertBlock->parent);
    auto* endBlock = new IRBasicBlock("and.end", insertBlock->parent);

    IRValue* lhs = emitExpr(left);
    createCondBr(lhs, rhsBlock, endBlock);
    auto* lhsBlock = insertBlock;

    setInsertPoint(rhsBlock);
    IRValue* rhs = emitExpr(right);
    createBr(endBlock);
    rhsBlock = insertBlock;

    setInsertPoint(endBlock);
    return createPhi({{lhs, lhsBlock}, {rhs, rhsBlock}}, "and");
}

IRValue* IRGenerator::emitLogicalOr(const Expr& left, const Expr& right) {
    auto* rhsBlock = new IRBasicBlock("or.rhs", insertBlock->parent);
    auto* endBlock = new IRBasicBlock("or.end", insertBlock->parent);

    IRValue* lhs = emitExpr(left);
    createCondBr(lhs, endBlock, rhsBlock);
    auto* lhsBlock = insertBlock;

    setInsertPoint(rhsBlock);
    IRValue* rhs = emitExpr(right);
    createBr(endBlock);
    rhsBlock = insertBlock;

    setInsertPoint(endBlock);
    return createPhi({{lhs, lhsBlock}, {rhs, rhsBlock}}, "or");
}

IRValue* IRGenerator::emitBinaryExpr(const BinaryExpr& expr) {
    if (expr.isAssignment()) {
        emitAssignment(expr);
        return nullptr;
    }

    if (expr.getCalleeDecl() != nullptr) {
        return emitCallExpr(expr);
    }

    switch (expr.getOperator()) {
        case Token::AndAnd:
            return emitLogicalAnd(expr.getLHS(), expr.getRHS());
        case Token::OrOr:
            return emitLogicalOr(expr.getLHS(), expr.getRHS());
        default:
            auto* lhs = emitExprOrEnumTag(expr.getLHS(), nullptr);
            auto* rhs = emitExprOrEnumTag(expr.getRHS(), nullptr);
            return emitCallExpr(expr.getOperator(), lhs, rhs, expr.getLHS().getType();
    }
}

void IRGenerator::emitAssignment(const BinaryExpr& expr) {
    if (expr.getRHS().isUndefinedLiteralExpr()) return;

    IRValue* lhsLvalue = emitAssignmentLHS(expr.getLHS());
    auto rhsValue = emitExprForPassing(expr.getRHS(), lhsLvalue->getType().getPointee());
    createStore(rhsValue, lhsLvalue);
}

static bool isBuiltinArrayToArrayRefConversion(Type sourceType, Type targetType) {
    return sourceType.removePointer().isArrayWithConstantSize() && targetType.getDecl() && targetType.getDecl()->isStruct() &&
           targetType.getName() == "ArrayRef";
}

IRValue* IRGenerator::emitExprForPassing(const Expr& expr, Type targetType) {
    if (!targetType) {
        return emitExpr(expr);
    }

    // TODO: Handle implicit conversions in a separate function.

    if (isBuiltinArrayToArrayRefConversion(expr.getType(), targetType)) {
        ASSERT(expr.getType().removePointer().isArrayWithConstantSize());
        auto* value = emitExprAsPointer(expr);
        auto* elementPtr = createConstGEP2_32(nullptr, value, 0, 0);
        auto* arrayRef = createInsertValue(UndefValue::get(targetType), elementPtr, 0);
        auto size = ConstantInt::get(Type::getInt(), expr.getType().removePointer().getArraySize());
        return createInsertValue(arrayRef, size, 1);
    }

    // Handle implicit conversions to type 'T[*]'.
    if (expr.getType().removePointer().isArrayWithConstantSize() && targetType.isPointerType() && !targetType.getPointee().isArrayType()) {
        return createBitOrPointerCast(emitLvalueExpr(expr), targetType);
    }

    // Handle implicit conversions to void pointer.
    if (expr.getType().isPointerTypeInLLVM() && targetType.isPointerType() && targetType.getPointee().isVoid()) {
        return createBitCast(emitExpr(expr), targetType);
    }

    // TODO: Refactor the following.
    auto* value = emitLvalueExpr(expr);

    if (targetType.isPointerType() && value->getType() == targetType.getPointee()) {
        return createTempAlloca(value);
    } else if (value->getType().isPointerType() && targetType != value->getType()) {
        value = createLoad(value);
        if (value->getType().isPointerType() && targetType != value->getType()) {
            value = createLoad(value);
        }
        return value;
    } else {
        return value;
    }
}

void IRGenerator::emitAssert(IRValue* condition, SourceLocation location, llvm::StringRef message) {
    condition = createIsNull(condition, "assert.condition");
    auto* function = insertBlock->parent;
    auto* failBlock = new IRBasicBlock("assert.fail", function);
    auto* successBlock = new IRBasicBlock("assert.success", function);
    auto* assertFail = getFunctionProto(*llvm::cast<FunctionDecl>(Module::getStdlibModule()->getSymbolTable().findOne("assertFail")));
    createCondBr(condition, failBlock, successBlock);
    setInsertPoint(failBlock);
    auto messageAndLocation = llvm::join_items("", message, " at ", llvm::sys::path::filename(location.file), ":",
                                               std::to_string(location.line), ":", std::to_string(location.column), "\n");
    createCall(assertFail, createGlobalStringPtr(messageAndLocation));
    createUnreachable();
    setInsertPoint(successBlock);
}

IRValue* IRGenerator::emitEnumCase(const EnumCase& enumCase, llvm::ArrayRef<NamedValue> associatedValueElements) {
    auto enumDecl = enumCase.getEnumDecl();
    auto tag = emitExpr(*enumCase.getValue());
    if (!enumDecl->hasAssociatedValues()) return tag;

    // TODO: Could reuse variable alloca instead of always creating a new one here.
    auto* enumValue = createEntryBlockAlloca(getLLVMType(enumDecl->getType()), "enum");
    createStore(tag, createStructGEP(enumValue, 0, "tag"));

    if (!associatedValueElements.empty()) {
        // TODO: This is duplicated in emitTupleExpr.
        IRValue* associatedValue = UndefValue::get(getLLVMType(enumCase.getAssociatedType()));
        int index = 0;
        for (auto& element : associatedValueElements) {
            associatedValue = createInsertValue(associatedValue, emitExpr(*element.getValue()), index++);
        }
        auto* associatedValuePtr = createPointerCast(createStructGEP(enumValue, 1, "associatedValue"),
                                                     associatedValue->getType().getPointerTo());
        createStore(associatedValue, associatedValuePtr);
    }

    return enumValue;
}

IRValue* IRGenerator::emitCallExpr(const CallExpr& expr, IRAllocaInst* thisAllocaForInit) {
    if (expr.isBuiltinConversion()) {
        return emitBuiltinConversion(*expr.getArgs().front().getValue(), expr.getType());
    }

    if (expr.isBuiltinCast()) {
        return emitBuiltinCast(expr);
    }

    if (expr.getFunctionName() == "assert") {
        emitAssert(emitExpr(*expr.getArgs().front().getValue()), expr.getCallee().getLocation());
        return nullptr;
    }

    if (auto* enumCase = llvm::dyn_cast_or_null<EnumCase>(expr.getCalleeDecl())) {
        return emitEnumCase(*enumCase, expr.getArgs());
    }

    if (expr.getReceiver() && expr.getReceiverType().removePointer().isArrayType()) {
        if (expr.getFunctionName() == "size") {
            return getArrayLength(*expr.getReceiver(), expr.getReceiverType().removePointer());
        }
        if (expr.getFunctionName() == "iterator") {
            return getArrayIterator(*expr.getReceiver(), expr.getReceiverType().removePointer());
        }
        llvm_unreachable("unknown array member function");
    }

    if (expr.isMoveInit()) {
        auto* receiverValue = emitExpr(*expr.getReceiver());
        auto* argumentValue = emitExpr(*expr.getArgs()[0].getValue());
        createStore(argumentValue, receiverValue);
        return nullptr;
    }

    IRValue* calleeValue = getFunctionForCall(expr);

    if (!calleeValue) {
        return nullptr;
    }

    FunctionType* functionType;

    if (auto* function = llvm::dyn_cast<IRFunction>(calleeValue)) {
        functionType = function->getFunctionType();
    } else {
        if (!llvm::isa<FunctionType>(calleeValue->getType().getPointee())) {
            calleeValue = createLoad(calleeValue);
        }
        functionType = llvm::cast<FunctionType>(calleeValue->getType().getPointee());
    }

    auto param = functionType->param_begin();
    auto paramEnd = functionType->param_end();

    llvm::SmallVector<IRValue*, 16> args;

    auto* calleeDecl = expr.getCalleeDecl();

    if (calleeDecl->isMethodDecl()) {
        if (auto* constructorDecl = llvm::dyn_cast<ConstructorDecl>(calleeDecl)) {
            if (thisAllocaForInit) {
                args.emplace_back(thisAllocaForInit);
            } else if (currentDecl->isConstructorDecl() && expr.getFunctionName() == "init") {
                args.emplace_back(getThis(*param));
            } else {
                args.emplace_back(createEntryBlockAlloca(getLLVMType(constructorDecl->getTypeDecl()->getType())));
            }
        } else if (expr.getReceiver()) {
            args.emplace_back(emitExprForPassing(*expr.getReceiver(), *param));
        } else {
            args.emplace_back(getThis());
        }
        ++param;
    }

    for (const auto& arg : expr.getArgs()) {
        auto* paramType = param != paramEnd ? *param++ : nullptr;
        auto* argValue = emitExprForPassing(*arg.getValue(), paramType);
        if (paramType) ASSERT(argValue->getType() == paramType);
        args.push_back(argValue);
    }

    if (calleeDecl->isConstructorDecl()) {
        createCall(calleeValue, args);
        return args[0];
    } else {
        return createCall(calleeValue, args);
    }
}

IRValue* IRGenerator::emitBuiltinCast(const CallExpr& expr) {
    auto* value = emitExpr(*expr.getArgs().front().getValue());
    auto* type = getLLVMType(expr.getGenericArgs().front());

    if (value->getType().isInteger() && type->isInteger()) {
        return createIntCast(value, type, expr.getArgs().front().getValue()->getType().isSigned());
    }

    return createBitOrPointerCast(value, type);
}

IRValue* IRGenerator::emitSizeofExpr(const SizeofExpr& expr) {
    return ConstantExpr::getSizeOf(getLLVMType(expr.getType()));
}

IRValue* IRGenerator::emitAddressofExpr(const AddressofExpr& expr) {
    IRValue* value = emitExpr(expr.getOperand());
    Type uintptr = getLLVMType(Type::getUIntPtr());
    return createPtrToInt(value, uintptr, "address");
}

IRValue* IRGenerator::emitMemberAccess(IRValue* baseValue, const FieldDecl* field) {
    auto baseTypeDecl = field->getParentDecl();
    auto baseType = baseValue->getType();

    if (baseType.isPointerType()) {
        baseType = baseType.getPointee();
        if (baseType.isPointerType()) {
            baseType = baseType.getPointee();
            baseValue = createLoad(baseValue);
        }

        if (baseTypeDecl->isUnion()) {
            return createBitCast(baseValue, getLLVMType(field->getType())->getPointerTo(), field->getName());
        } else {
            auto index = baseTypeDecl->getFieldIndex(field);
            if (!baseType->isSized()) {
                emitTypeDecl(*baseTypeDecl);
            }
            return createStructGEP(nullptr, baseValue, index, field->getName());
        }
    } else {
        auto index = baseTypeDecl->isUnion() ? 0 : baseTypeDecl->getFieldIndex(field);
        return createExtractValue(baseValue, index, field->getName());
    }
}

IRValue* IRGenerator::getArrayLength(const Expr& object, Type objectType) {
    if (objectType.isArrayWithRuntimeSize()) {
        return createExtractValue(emitExpr(object), 1, "size");
    } else {
        return ConstantInt::get(Type::getInt(), objectType.getArraySize());
    }
}

IRValue* IRGenerator::getArrayIterator(const Expr& object, Type objectType) {
    auto* type = getLLVMType(BasicType::get("ArrayIterator", objectType.getElementType()));
    auto* value = emitExprAsPointer(object);
    auto* elementPtr = createConstInBoundsGEP2_32(nullptr, value, 0, 0);
    auto* size = getArrayLength(object, objectType);
    auto* end = createInBoundsGEP(elementPtr, size);
    auto* iterator = createInsertValue(UndefValue::get(type), elementPtr, 0);
    return createInsertValue(iterator, end, 1);
}

IRValue* IRGenerator::emitMemberExpr(const MemberExpr& expr) {
    if (auto* enumCase = llvm::dyn_cast_or_null<EnumCase>(expr.getDecl())) {
        return emitEnumCase(*enumCase, {});
    }

    if (expr.getBaseExpr()->getType().removePointer().isTupleType()) {
        return emitTupleElementAccess(expr);
    }

    return emitMemberAccess(emitLvalueExpr(*expr.getBaseExpr()), llvm::cast<FieldDecl>(expr.getDecl()));
}

IRValue* IRGenerator::emitTupleElementAccess(const MemberExpr& expr) {
    unsigned index = 0;
    for (auto& element : expr.getBaseExpr()->getType().removePointer().getTupleElements()) {
        if (element.name == expr.getMemberName()) break;
        ++index;
    }

    auto* baseValue = emitLvalueExpr(*expr.getBaseExpr());
    if (baseValue->getType().isPointerType() && baseValue->getType().getPointee().isPointerType()) {
        baseValue = createLoad(baseValue);
    }

    if (baseValue->getType().isPointerType()) {
        return createStructGEP(nullptr, baseValue, index, expr.getMemberName());
    } else {
        return createExtractValue(baseValue, index, expr.getMemberName());
    }
}

IRValue* IRGenerator::emitIndexExpr(const IndexExpr& expr) {
    if (!expr.getBase()->getType().removePointer().isArrayType()) {
        return emitCallExpr(expr);
    }

    auto* value = emitLvalueExpr(*expr.getBase());
    Type lhsType = expr.getBase()->getType();

    if (lhsType.isArrayWithRuntimeSize()) {
        if (value->getType().isPointerType()) {
            value = createLoad(value);
        }
        auto* ptr = createExtractValue(value, 0);
        auto* index = emitExpr(*expr.getIndex());
        return createGEP(ptr, index);
    }

    if (value->getType().isPointerType() && value->getType().getPointee().isPointerType() &&
        value->getType().getPointee() == getLLVMType(lhsType)) {
        value = createLoad(value);
    }

    if (lhsType.isArrayWithUnknownSize()) {
        return createGEP(value, emitExpr(*expr.getIndex()));
    }

    return createGEP(value, {ConstantInt::get(Type::getInt(), 0), emitExpr(*expr.getIndex())});
}

IRValue* IRGenerator::emitUnwrapExpr(const UnwrapExpr& expr) {
    auto* value = emitExpr(expr.getOperand());
    llvm::StringRef message = "Unwrap failed";

    if (expr.getOperand().getType().isPointerTypeInLLVM()) {
        emitAssert(value, expr.getLocation(), message);
        return value;
    } else {
        emitAssert(createExtractValue(value, optionalHasValueFieldIndex), expr.getLocation(), message);
        return createExtractValue(value, optionalValueFieldIndex);
    }
}

IRValue* IRGenerator::emitLambdaExpr(const LambdaExpr& expr) {
    auto functionDecl = expr.getFunctionDecl();

    auto insertBlockBackup = insertBlock;
    auto scopesBackup = std::move(scopes);

    emitFunctionDecl(*functionDecl);

    scopes = std::move(scopesBackup);
    if (insertBlockBackup) setInsertPoint(insertBlockBackup);

    VarExpr varExpr(functionDecl->getName(), functionDecl->getLocation());
    varExpr.setDecl(functionDecl);
    varExpr.setType(expr.getType());
    return emitVarExpr(varExpr);
}

IRValue* IRGenerator::emitIfExpr(const IfExpr& expr) {
    auto* condition = emitExpr(*expr.getCondition());
    if (condition->getType().isPointerType()) {
        condition = emitImplicitNullComparison(condition);
    }
    auto* function = insertBlock->parent;
    auto* thenBlock = new IRBasicBlock("if.then", function);
    auto* elseBlock = new IRBasicBlock("if.else");
    auto* endIfBlock = new IRBasicBlock("if.end");
    createCondBr(condition, thenBlock, elseBlock);

    setInsertPoint(thenBlock);
    auto* thenValue = emitExpr(*expr.getThenExpr());
    createBr(endIfBlock);
    thenBlock = insertBlock;

    function->getBasicBlockList().push_back(elseBlock);
    setInsertPoint(elseBlock);
    auto* elseValue = emitExpr(*expr.getElseExpr());
    createBr(endIfBlock);
    elseBlock = insertBlock;

    function->getBasicBlockList().push_back(endIfBlock);
    setInsertPoint(endIfBlock);
    return createPhi({{thenValue, thenBlock}, {elseValue, elseBlock}}, "phi");
}

IRValue* IRGenerator::emitImplicitCastExpr(const ImplicitCastExpr& expr) {
    if (expr.getType().isOptionalType() &&
        !(expr.getType().getWrappedType().isPointerType() || expr.getType().getWrappedType().isFunctionType()) &&
        expr.getOperand()->getType() == expr.getType().getWrappedType()) {
        return emitOptionalConstruction(expr.getOperand()->getType(), emitExprWithoutAutoCast(*expr.getOperand()));
    }

    if (expr.getOperand()->isStringLiteralExpr() && expr.getType().removeOptional().isPointerType() &&
        expr.getType().removeOptional().getPointee().isChar()) {
        return createGlobalStringPtr(llvm::cast<StringLiteralExpr>(expr.getOperand())->getValue());
    }

    return emitExprWithoutAutoCast(*expr.getOperand());
}

IRValue* IRGenerator::emitExprWithoutAutoCast(const Expr& expr) {
    switch (expr.getKind()) {
        case ExprKind::VarExpr:
            return emitVarExpr(llvm::cast<VarExpr>(expr));
        case ExprKind::StringLiteralExpr:
            return emitStringLiteralExpr(llvm::cast<StringLiteralExpr>(expr));
        case ExprKind::CharacterLiteralExpr:
            return emitCharacterLiteralExpr(llvm::cast<CharacterLiteralExpr>(expr));
        case ExprKind::IntLiteralExpr:
            return emitIntLiteralExpr(llvm::cast<IntLiteralExpr>(expr));
        case ExprKind::FloatLiteralExpr:
            return emitFloatLiteralExpr(llvm::cast<FloatLiteralExpr>(expr));
        case ExprKind::BoolLiteralExpr:
            return emitBoolLiteralExpr(llvm::cast<BoolLiteralExpr>(expr));
        case ExprKind::NullLiteralExpr:
            return emitNullLiteralExpr(llvm::cast<NullLiteralExpr>(expr));
        case ExprKind::UndefinedLiteralExpr:
            return emitUndefinedLiteralExpr(llvm::cast<UndefinedLiteralExpr>(expr));
        case ExprKind::ArrayLiteralExpr:
            return emitArrayLiteralExpr(llvm::cast<ArrayLiteralExpr>(expr));
        case ExprKind::TupleExpr:
            return emitTupleExpr(llvm::cast<TupleExpr>(expr));
        case ExprKind::UnaryExpr:
            return emitUnaryExpr(llvm::cast<UnaryExpr>(expr));
        case ExprKind::BinaryExpr:
            return emitBinaryExpr(llvm::cast<BinaryExpr>(expr));
        case ExprKind::CallExpr:
            return emitCallExpr(llvm::cast<CallExpr>(expr));
        case ExprKind::SizeofExpr:
            return emitSizeofExpr(llvm::cast<SizeofExpr>(expr));
        case ExprKind::AddressofExpr:
            return emitAddressofExpr(llvm::cast<AddressofExpr>(expr));
        case ExprKind::MemberExpr:
            return emitMemberExpr(llvm::cast<MemberExpr>(expr));
        case ExprKind::IndexExpr:
            return emitIndexExpr(llvm::cast<IndexExpr>(expr));
        case ExprKind::UnwrapExpr:
            return emitUnwrapExpr(llvm::cast<UnwrapExpr>(expr));
        case ExprKind::LambdaExpr:
            return emitLambdaExpr(llvm::cast<LambdaExpr>(expr));
        case ExprKind::IfExpr:
            return emitIfExpr(llvm::cast<IfExpr>(expr));
        case ExprKind::ImplicitCastExpr:
            return emitImplicitCastExpr(llvm::cast<ImplicitCastExpr>(expr));
    }
    llvm_unreachable("all cases handled");
}

IRValue* IRGenerator::emitExpr(const Expr& expr) {
    auto* value = emitLvalueExpr(expr);

    if (value) {
        // FIXME: Temporary
        if (auto implicitCastExpr = llvm::dyn_cast<ImplicitCastExpr>(&expr)) {
            if (value->getType().isPointerType() && value->getType().getPointee() == getLLVMType(implicitCastExpr->getOperand()->getType())) {
                return createLoad(value);
            }
        }

        if (value->getType().isPointerType() && value->getType().getPointee() == getLLVMType(expr.getType())) {
            return createLoad(value);
        }
    }

    return value;
}

IRValue* IRGenerator::emitLvalueExpr(const Expr& expr) {
    return emitAutoCast(emitExprWithoutAutoCast(expr), expr);
}

IRValue* IRGenerator::emitExprAsPointer(const Expr& expr) {
    auto* value = emitLvalueExpr(expr);
    if (!value->getType().isPointerType()) {
        value = createTempAlloca(value);
    }
    return value;
}

IRValue* IRGenerator::emitExprOrEnumTag(const Expr& expr, IRValue*** enumValue) {
    if (auto* memberExpr = llvm::dyn_cast<MemberExpr>(&expr)) {
        if (auto* enumCase = llvm::dyn_cast_or_null<EnumCase>(memberExpr->getDecl())) {
            return emitExpr(*enumCase->getValue());
        }
    }

    if (auto* enumDecl = llvm::dyn_cast_or_null<EnumDecl>(expr.getType().getDecl())) {
        if (enumDecl->hasAssociatedValues()) {
            auto* value = emitLvalueExpr(expr);
            if (enumValue) *enumValue = value;
            return createLoad(createStructGEP(value, 0, value->getName() + ".tag"));
        }
    }

    return emitExpr(expr);
}

IRValue* IRGenerator::emitAutoCast(IRValue* value, const Expr& expr) {
    // Handle optionals that have been implicitly unwrapped due to data-flow analysis.
    if (expr.hasAssignableType() && expr.getAssignableType().isOptionalType() &&
        !expr.getAssignableType().getWrappedType().isPointerType() && expr.getType() == expr.getAssignableType().getWrappedType()) {
        return createConstInBoundsGEP2_32(value->getType().getPointee(), value, 0, optionalValueFieldIndex);
    }

    if (value && expr.hasType()) {
        auto type = getLLVMType(expr.getType());

        if (type != value->getType()) {
            if (value->getType().isFloatingPoint()) {
                return createFPCast(value, type);
            } else if (value->getType().isInteger()) {
                if (expr.getType().isFloatingPoint()) {
                    return createSIToFP(value, type); // TODO: Handle signed vs unsigned
                } else {
                    return createIntCast(value, type, expr.getType().isSigned());
                }
            }
        }
    }

    return value;
}

void IRGenerator::setInsertPoint(IRBasicBlock* block) {
    insertBlock = block;
}
