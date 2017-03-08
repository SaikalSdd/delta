#pragma once

#include <vector>
#include <string>
#include <memory>
#include <ostream>
#include <boost/variant.hpp>
#include <llvm/ADT/ArrayRef.h>

namespace delta {

enum class TypeKind {
    BasicType,
    ArrayType,
    TupleType,
    FuncType,
    PtrType,
};

class Type;

struct BasicType {
    std::string name;
};

struct ArrayType {
    std::unique_ptr<Type> elementType;
    int64_t size;
    ArrayType(std::unique_ptr<Type> type, int64_t size) : elementType(std::move(type)), size(size) { }
    ArrayType(const ArrayType&);
    ArrayType& operator=(const ArrayType&);
};

struct TupleType {
    std::vector<Type> subtypes;
};

struct FuncType {
    std::vector<Type> returnTypes;
    std::vector<Type> paramTypes;
};

struct PtrType {
    std::unique_ptr<Type> pointeeType;
    bool ref;

    PtrType(std::unique_ptr<Type> type, bool ref) : pointeeType(std::move(type)), ref(ref) { }
    PtrType(const PtrType&);
    PtrType& operator=(const PtrType&);
};

class Type {
public:
#define DEFINE_TYPEKIND_GETTER_AND_CONSTRUCTOR(KIND) \
    Type(KIND&& value, bool isMutable = false) : data(std::move(value)), mutableFlag(isMutable) { \
        if (TypeKind::KIND == TypeKind::TupleType && getSubtypes().size() == 1) { \
            auto type = std::move(getSubtypes()[0]); \
            *this = std::move(type); \
        } \
    } \
    \
    bool is##KIND() const { return getKind() == TypeKind::KIND; } \
    \
    KIND& get##KIND() { \
        assert(is##KIND()); \
        return boost::get<KIND>(data); \
    } \
    const KIND& get##KIND() const { \
        assert(is##KIND()); \
        return boost::get<KIND>(data); \
    }
    DEFINE_TYPEKIND_GETTER_AND_CONSTRUCTOR(BasicType)
    DEFINE_TYPEKIND_GETTER_AND_CONSTRUCTOR(ArrayType)
    DEFINE_TYPEKIND_GETTER_AND_CONSTRUCTOR(TupleType)
    DEFINE_TYPEKIND_GETTER_AND_CONSTRUCTOR(FuncType)
    DEFINE_TYPEKIND_GETTER_AND_CONSTRUCTOR(PtrType)
#undef DEFINE_TYPEKIND_GETTER_AND_CONSTRUCTOR

    void appendType(Type type);
    bool isImplicitlyConvertibleTo(const Type&) const;
    bool isSigned() const;
    bool isMutable() const { return mutableFlag; }
    void setMutable(bool isMutable) { mutableFlag = isMutable; }
    TypeKind getKind() const { return static_cast<TypeKind>(data.which()); }
    void printTo(std::ostream& stream, bool omitTopLevelMutable) const;
    std::string toString() const;

    llvm::StringRef getName() const { return getBasicType().name; }
    const Type& getElementType() const { return *getArrayType().elementType; }
    int getArraySize() const { return getArrayType().size; }
    llvm::ArrayRef<Type> getSubtypes() const { return getTupleType().subtypes; }
    llvm::ArrayRef<Type> getReturnTypes() const { return getFuncType().returnTypes; }
    llvm::ArrayRef<Type> getParamTypes() const { return getFuncType().paramTypes; }
    const Type& getPointee() const { return *getPtrType().pointeeType; }
    const Type& getReferee() const { assert(isRef()); return getPointee(); }
    bool isRef() const { return isPtrType() && getPtrType().ref; }

private:
    boost::variant<BasicType, ArrayType, TupleType, FuncType, PtrType> data;
    bool mutableFlag;
};

bool operator==(const Type&, const Type&);
bool operator!=(const Type&, const Type&);
std::ostream& operator<<(std::ostream&, const Type&);

}
