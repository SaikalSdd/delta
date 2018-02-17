#include <sstream>
#pragma warning(push, 0)
#include <llvm/ADT/StringRef.h>
#include <llvm/ADT/StringSwitch.h>
#include <llvm/ADT/STLExtras.h>
#include <llvm/Support/ErrorHandling.h>
#pragma warning(pop)
#include "type.h"
#include "decl.h"
#include "../support/utility.h"

using namespace delta;

#define DEFINE_BUILTIN_TYPE_GET_AND_IS(TYPE, NAME) \
    Type Type::get##TYPE(bool isMutable, SourceLocation location) { \
        static BasicType type(#NAME, /*genericArgs*/ {}); \
        return Type(&type, isMutable, location); \
    } \
    bool Type::is##TYPE() const { return isBasicType() && getName() == #NAME; }

DEFINE_BUILTIN_TYPE_GET_AND_IS(Void, void)
DEFINE_BUILTIN_TYPE_GET_AND_IS(Bool, bool)
DEFINE_BUILTIN_TYPE_GET_AND_IS(Int, int)
DEFINE_BUILTIN_TYPE_GET_AND_IS(Int8, int8)
DEFINE_BUILTIN_TYPE_GET_AND_IS(Int16, int16)
DEFINE_BUILTIN_TYPE_GET_AND_IS(Int32, int32)
DEFINE_BUILTIN_TYPE_GET_AND_IS(Int64, int64)
DEFINE_BUILTIN_TYPE_GET_AND_IS(UInt, uint)
DEFINE_BUILTIN_TYPE_GET_AND_IS(UInt8, uint8)
DEFINE_BUILTIN_TYPE_GET_AND_IS(UInt16, uint16)
DEFINE_BUILTIN_TYPE_GET_AND_IS(UInt32, uint32)
DEFINE_BUILTIN_TYPE_GET_AND_IS(UInt64, uint64)
DEFINE_BUILTIN_TYPE_GET_AND_IS(Float, float)
DEFINE_BUILTIN_TYPE_GET_AND_IS(Float32, float32)
DEFINE_BUILTIN_TYPE_GET_AND_IS(Float64, float64)
DEFINE_BUILTIN_TYPE_GET_AND_IS(Float80, float80)
DEFINE_BUILTIN_TYPE_GET_AND_IS(String, String)
DEFINE_BUILTIN_TYPE_GET_AND_IS(Char, char)
DEFINE_BUILTIN_TYPE_GET_AND_IS(Null, null)
DEFINE_BUILTIN_TYPE_GET_AND_IS(Undefined, undefined)
#undef DEFINE_BUILTIN_TYPE_GET_AND_IS

bool Type::isArrayWithConstantSize() const {
    return isArrayType() && getArraySize() >= 0;
}

bool Type::isArrayWithRuntimeSize() const {
    return isArrayType() && getArraySize() == ArrayType::runtimeSize;
}

bool Type::isArrayWithUnknownSize() const {
    return isArrayType() && getArraySize() == ArrayType::unknownSize;
}

bool Type::isBuiltinScalar(llvm::StringRef typeName) {
    return llvm::StringSwitch<bool>(typeName)
        .Cases("int", "int8", "int16", "int32", "int64", true)
        .Cases("uint", "uint8", "uint16", "uint32", "uint64", true)
        .Cases("float", "float32", "float64", "float80", "bool", "char", true)
        .Default(false);
}

bool Type::isEnumType() const {
    if (auto* basicType = llvm::dyn_cast<BasicType>(getBase())) {
        return basicType->getDecl() && basicType->getDecl()->isEnumDecl();
    }
    return false;
}

Type Type::resolve(const llvm::StringMap<Type>& replacements) const {
    if (!typeBase) return Type(nullptr, isMutable(), location);

    switch (getKind()) {
        case TypeKind::BasicType: {
            auto it = replacements.find(getName());
            if (it != replacements.end()) {
                // TODO: Handle generic arguments for type placeholders.
                Type resolved = it->second.asMutable(isMutable());
                resolved.setLocation(location);
                return resolved;
            }

            auto genericArgs = map(getGenericArgs(), [&](Type t) { return t.resolve(replacements); });
            return BasicType::get(getName(), std::move(genericArgs), isMutable(), location);
        }
        case TypeKind::ArrayType:
            return ArrayType::get(getElementType().resolve(replacements), getArraySize(), isMutable(), location);

        case TypeKind::TupleType: {
            auto elements = map(getTupleElements(), [&](const TupleElement& element) {
                return TupleElement{ element.name, element.type.resolve(replacements) };
            });
            return TupleType::get(std::move(elements), isMutable(), location);
        }
        case TypeKind::FunctionType: {
            auto paramTypes = map(getParamTypes(), [&](Type t) { return t.resolve(replacements); });
            return FunctionType::get(getReturnType().resolve(replacements), std::move(paramTypes), isMutable(), location);
        }
        case TypeKind::PointerType:
            return PointerType::get(getPointee().resolve(replacements), isMutable(), location);
        case TypeKind::OptionalType:
            return OptionalType::get(getWrappedType().resolve(replacements), isMutable(), location);
    }
    llvm_unreachable("all cases handled");
}

namespace {
std::vector<std::unique_ptr<BasicType>> basicTypes;
std::vector<std::unique_ptr<ArrayType>> arrayTypes;
std::vector<std::unique_ptr<TupleType>> tupleTypes;
std::vector<std::unique_ptr<FunctionType>> functionTypes;
std::vector<std::unique_ptr<PointerType>> ptrTypes;
std::vector<std::unique_ptr<OptionalType>> optionalTypes;
} // namespace

#define FETCH_AND_RETURN_TYPE(TYPE, CACHE, EQUALS, ...) \
    auto it = llvm::find_if(CACHE, [&](const std::unique_ptr<TYPE>& t) { return EQUALS; }); \
    if (it != CACHE.end()) return Type(it->get(), isMutable, location); \
    CACHE.emplace_back(new TYPE(__VA_ARGS__)); \
    return Type(CACHE.back().get(), isMutable, location);

Type BasicType::get(llvm::StringRef name, llvm::ArrayRef<Type> genericArgs, bool isMutable, SourceLocation location) {
    FETCH_AND_RETURN_TYPE(BasicType, basicTypes, t->getName() == name && t->getGenericArgs() == genericArgs, name,
                          genericArgs);
}

Type ArrayType::get(Type elementType, int64_t size, bool isMutable, SourceLocation location) {
    FETCH_AND_RETURN_TYPE(ArrayType, arrayTypes, t->getElementType() == elementType && t->getSize() == size,
                          elementType, size);
}

Type TupleType::get(std::vector<TupleElement>&& elements, bool isMutable, SourceLocation location) {
    FETCH_AND_RETURN_TYPE(TupleType, tupleTypes, t->getElements() == llvm::makeArrayRef(elements), std::move(elements));
}

Type FunctionType::get(Type returnType, std::vector<Type>&& paramTypes, bool isMutable, SourceLocation location) {
    FETCH_AND_RETURN_TYPE(FunctionType, functionTypes,
                          t->getReturnType() == returnType && t->getParamTypes() == llvm::makeArrayRef(paramTypes),
                          returnType, std::move(paramTypes));
}

Type PointerType::get(Type pointeeType, bool isMutable, SourceLocation location) {
    FETCH_AND_RETURN_TYPE(PointerType, ptrTypes, t->getPointeeType() == pointeeType, pointeeType);
}

Type OptionalType::get(Type wrappedType, bool isMutable, SourceLocation location) {
    FETCH_AND_RETURN_TYPE(OptionalType, optionalTypes, t->getWrappedType() == wrappedType, wrappedType);
}

#undef FETCH_AND_RETURN_TYPE

bool delta::operator==(const TupleElement& a, const TupleElement& b) {
    return a.name == b.name && a.type == b.type;
}

std::vector<ParamDecl> FunctionType::getParamDecls(SourceLocation location) const {
    std::vector<ParamDecl> paramDecls;
    paramDecls.reserve(paramTypes.size());

    for (Type paramType : paramTypes) {
        paramDecls.push_back(ParamDecl(paramType, "", location));
    }

    return paramDecls;
}

bool Type::isSigned() const {
    if (!isBasicType()) {
        return false;
    }
    llvm::StringRef name = getName();
    return name == "int" || name == "int8" || name == "int16" || name == "int32" || name == "int64";
}

bool Type::isUnsigned() const {
    ASSERT(isBasicType());
    llvm::StringRef name = getName();
    return name == "uint" || name == "uint8" || name == "uint16" || name == "uint32" || name == "uint64";
}

void Type::setMutable(bool isMutable) {
    if (typeBase) {
        switch (typeBase->getKind()) {
            case TypeKind::ArrayType: {
                auto& array = llvm::cast<ArrayType>(*typeBase);
                *this = ArrayType::get(array.getElementType().asMutable(isMutable), array.getSize(), isMutable, location);
                return;
            }
            case TypeKind::TupleType: {
                auto& tuple = llvm::cast<TupleType>(*typeBase);
                auto elements = map(tuple.getElements(), [&](const TupleElement& e) {
                    return TupleElement{ e.name, e.type.asMutable(isMutable) };
                });
                *this = TupleType::get(std::move(elements), isMutable, location);
                return;
            }
            default:
                break;
        }
    }

    mutableFlag = isMutable;
}

llvm::StringRef Type::getName() const {
    return llvm::cast<BasicType>(typeBase)->getName();
}

Type Type::getElementType() const {
    return llvm::cast<ArrayType>(typeBase)->getElementType().asMutable(isMutable()).withLocation(location);
}

int64_t Type::getArraySize() const {
    return llvm::cast<ArrayType>(typeBase)->getSize();
}

llvm::ArrayRef<TupleElement> Type::getTupleElements() const {
    return llvm::cast<TupleType>(typeBase)->getElements();
}

llvm::ArrayRef<Type> Type::getGenericArgs() const {
    return llvm::cast<BasicType>(typeBase)->getGenericArgs();
}

Type Type::getReturnType() const {
    return llvm::cast<FunctionType>(typeBase)->getReturnType().withLocation(location);
}

llvm::ArrayRef<Type> Type::getParamTypes() const {
    return llvm::cast<FunctionType>(typeBase)->getParamTypes();
}

Type Type::getPointee() const {
    return llvm::cast<PointerType>(typeBase)->getPointeeType().withLocation(location);
}

Type Type::getWrappedType() const {
    return llvm::cast<OptionalType>(typeBase)->getWrappedType().withLocation(location);
}

bool delta::operator==(Type lhs, Type rhs) {
    if (lhs.isMutable() != rhs.isMutable()) return false;
    switch (lhs.getKind()) {
        case TypeKind::BasicType:
            return rhs.isBasicType() && lhs.getName() == rhs.getName() && lhs.getGenericArgs() == rhs.getGenericArgs();
        case TypeKind::ArrayType:
            return rhs.isArrayType() && lhs.getElementType() == rhs.getElementType() &&
                   lhs.getArraySize() == rhs.getArraySize();
        case TypeKind::TupleType:
            return rhs.isTupleType() && lhs.getTupleElements() == rhs.getTupleElements();
        case TypeKind::FunctionType:
            return rhs.isFunctionType() && lhs.getReturnType() == rhs.getReturnType() &&
                   lhs.getParamTypes() == rhs.getParamTypes();
        case TypeKind::PointerType:
            return rhs.isPointerType() && lhs.getPointee() == rhs.getPointee();
        case TypeKind::OptionalType:
            return rhs.isOptionalType() && lhs.getWrappedType() == rhs.getWrappedType();
    }
    llvm_unreachable("all cases handled");
}

bool delta::operator!=(Type lhs, Type rhs) {
    return !(lhs == rhs);
}

TypeDecl* Type::getDecl() const {
    auto* basicType = llvm::dyn_cast<BasicType>(typeBase);
    return basicType ? basicType->getDecl() : nullptr;
}

DeinitDecl* Type::getDeinitializer() const {
    auto* typeDecl = getDecl();
    return typeDecl ? typeDecl->getDeinitializer() : nullptr;
}

void Type::printTo(std::ostream& stream, bool omitTopLevelMutable) const {
    switch (typeBase->getKind()) {
        case TypeKind::BasicType: {
            if (isMutable() && !omitTopLevelMutable) stream << "mutable ";
            stream << getName();

            auto genericArgs = llvm::cast<BasicType>(typeBase)->getGenericArgs();
            if (!genericArgs.empty()) {
                stream << "<";
                for (auto& type : genericArgs) {
                    type.printTo(stream, false);
                    if (&type != &genericArgs.back()) stream << ", ";
                }
                stream << ">";
            }

            break;
        }
        case TypeKind::ArrayType:
            getElementType().printTo(stream, omitTopLevelMutable);
            stream << "[";
            switch (getArraySize()) {
                case ArrayType::runtimeSize:
                    break;
                case ArrayType::unknownSize:
                    stream << "?";
                    break;
                default:
                    stream << getArraySize();
                    break;
            }
            stream << "]";
            break;
        case TypeKind::TupleType:
            stream << "(";
            for (auto& element : getTupleElements()) {
                stream << element.name << ": ";
                element.type.printTo(stream, omitTopLevelMutable);
                if (&element != &getTupleElements().back()) stream << ", ";
            }
            stream << ")";
            break;
        case TypeKind::FunctionType:
            stream << "(";
            for (const Type& paramType : getParamTypes()) {
                stream << paramType;
                if (&paramType != &getParamTypes().back()) stream << ", ";
            }
            stream << ") -> ";
            getReturnType().printTo(stream, true);
            break;
        case TypeKind::PointerType:
            if (getPointee().isFunctionType()) {
                stream << '(';
            }
            getPointee().printTo(stream, false);
            if (isMutable() && !omitTopLevelMutable) {
                stream << " mutable";
            }
            if (getPointee().isFunctionType()) {
                stream << ')';
            }
            stream << '*';
            break;
        case TypeKind::OptionalType:
            if (getWrappedType().isFunctionType()) {
                stream << '(';
            }
            getWrappedType().printTo(stream, false);
            if (isMutable() && !omitTopLevelMutable) {
                stream << " mutable";
            }
            if (getWrappedType().isFunctionType()) {
                stream << ')';
            }
            stream << '?';
            break;
    }
}

std::string Type::toString(bool omitTopLevelMutable) const {
    std::ostringstream stream;
    printTo(stream, omitTopLevelMutable);
    return stream.str();
}

std::ostream& delta::operator<<(std::ostream& stream, Type type) {
    type.printTo(stream, true);
    return stream;
}

llvm::raw_ostream& delta::operator<<(llvm::raw_ostream& stream, Type type) {
    std::ostringstream stringstream;
    type.printTo(stringstream, true);
    return stream << stringstream.str();
}
