#pragma once

#include <memory>
#pragma warning(push, 0)
#include <llvm/Support/MemoryBuffer.h>
#pragma warning(pop)

namespace llvm {
class StringRef;
}

namespace delta {

class Module;
class Expr;

void parse(llvm::StringRef filePath, Module& module);
std::unique_ptr<Expr> parseExpr(std::unique_ptr<llvm::MemoryBuffer> input, Module& module);

}
