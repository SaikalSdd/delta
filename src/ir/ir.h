#pragma once

#include <string>
#include <vector>
#include "../ast/type.h"

namespace llvm {
class StringRef;
}

namespace delta {

struct IRFunction;

struct IRValue {
    Type getType() const { return type; }
    llvm::StringRef getName() const { name; }
};

struct IRInstruction : IRValue {};

struct IRAllocaInst : IRInstruction {
    Type type;
    std::string name;
};

struct IRCallInst : IRInstruction {
    IRValue* function;
    std::vector<IRValue*> args;
};

struct IRBasicBlock {
    std::string name;
    IRFunction* parent;
    std::vector<IRInstruction*> instructions;

    IRBasicBlock(std::string name, IRFunction* parent = nullptr);
};

struct IRFunction : IRValue {
    std::vector<IRBasicBlock*> body;
};

struct IRGlobalVariable : IRValue {
    std::string name;
    IRValue value;
};

struct IRModule {
    std::string name;
    std::vector<IRFunction*> functions;
    std::vector<IRGlobalVariable*> globalVariables;
};

} // namespace delta
