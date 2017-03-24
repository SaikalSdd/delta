#include <iostream>
#include <cstdio>
#include <vector>
#include <algorithm>
#include <llvm/ADT/StringRef.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/LegacyPassManager.h>
#include <llvm/Target/TargetMachine.h>
#include <llvm/Support/TargetRegistry.h>
#include <llvm/Support/TargetSelect.h>
#include <llvm/Support/FileSystem.h>
#include "utility.h"
#include "../ast/ast_printer.h"
#include "../ast/decl.h"
#include "../parser/parse.h"
#include "../sema/typecheck.h"
#include "../irgen/irgen.h"

using namespace delta;

namespace delta {
const char* currentFileName;
}

namespace {

/// If `args` contains `flag`, removes it and returns true, otherwise returns false.
bool checkFlag(llvm::StringRef flag, std::vector<llvm::StringRef>& args) {
    const auto it = std::find(args.begin(), args.end(), flag);
    const bool contains = it != args.end();
    if (contains) args.erase(it);
    return contains;
}

std::vector<llvm::StringRef> collectStringOptionValues(llvm::StringRef flagPrefix,
                                                       std::vector<llvm::StringRef>& args) {
    std::vector<llvm::StringRef> values;
    for (auto arg = args.begin(); arg != args.end();) {
        if (arg->startswith(flagPrefix)) {
            values.push_back(arg->substr(flagPrefix.size()));
            arg = args.erase(arg);
        } else {
            ++arg;
        }
    }
    return values;
}

void emitMachineCode(llvm::Module& module, llvm::StringRef fileName,
                     llvm::TargetMachine::CodeGenFileType fileType) {
    llvm::InitializeNativeTarget();
    llvm::InitializeNativeTargetAsmPrinter();
    llvm::InitializeNativeTargetAsmParser();

    std::string targetTriple = llvm::sys::getDefaultTargetTriple();
    module.setTargetTriple(targetTriple);

    std::string errorMessage;
    auto* target = llvm::TargetRegistry::lookupTarget(targetTriple, errorMessage);
    if (!target) printErrorAndExit(errorMessage);

    llvm::TargetOptions options;
    auto* targetMachine = target->createTargetMachine(targetTriple, "generic", "", options,
                                                      llvm::Optional<llvm::Reloc::Model>());
    module.setDataLayout(targetMachine->createDataLayout());

    std::error_code error;
    llvm::raw_fd_ostream file(fileName, error, llvm::sys::fs::F_None);
    if (error) printErrorAndExit(error.message());

    llvm::legacy::PassManager passManager;
    if (targetMachine->addPassesToEmitFile(passManager, file, fileType)) {
        printErrorAndExit("TargetMachine can't emit a file of this type");
    }

    passManager.run(module);
    file.flush();
}

void printHelp() {
    llvm::outs() <<
        "OVERVIEW: Delta compiler\n"
        "\n"
        "USAGE: delta [options] <inputs>\n"
        "\n"
        "OPTIONS:\n"
        "  -c                    - Compile only, generating an .o file; don't link\n"
        "  -emit-assembly        - Emit assembly code\n"
        "  -fsyntax-only         - Perform parsing and type checking\n"
        "  -help                 - Display this help\n"
        "  -I<directory>         - Add a header search path for C import\n"
        "  -o=stdout             - Print the generated LLVM IR to stdout\n"
        "  -print-ast            - Print the abstract syntax tree to stdout\n";
}

} // anonymous namespace

int main(int argc, char** argv) {
    std::vector<llvm::StringRef> args(argv + 1, argv + argc);

    if (checkFlag("-help", args) || checkFlag("--help", args) || checkFlag("-h", args)) {
        printHelp();
        return 0;
    }

    const bool syntaxOnly = checkFlag("-fsyntax-only", args);
    const bool compileOnly = checkFlag("-c", args);
    const bool printAST = checkFlag("-print-ast", args);
    const bool outputToStdout = checkFlag("-o=stdout", args);
    const bool emitAssembly = checkFlag("-emit-assembly", args) || checkFlag("-S", args);
    const std::vector<llvm::StringRef> includePaths = collectStringOptionValues("-I", args);

    for (llvm::StringRef arg : args) {
        if (arg.startswith("-")) {
            printErrorAndExit("unsupported option '", arg, "'");
        }
    }

    if (args.empty()) {
        printErrorAndExit("no input files");
    }

    std::vector<std::unique_ptr<Decl>> globalAST;

    for (llvm::StringRef filePath : args) {
        currentFileName = filePath.data();
        parse(currentFileName, globalAST);
    }

    if (printAST) {
        std::cout << globalAST;
        return 0;
    }

    typecheck(globalAST, includePaths);

    if (syntaxOnly) return 0;

    auto& module = irgen::compile(globalAST);

    if (outputToStdout) {
        module.print(llvm::outs(), nullptr);
        return 0;
    }

    std::string outputFile = emitAssembly ? "output.s" : "output.o";
    auto fileType = emitAssembly ? llvm::TargetMachine::CGFT_AssemblyFile
                                 : llvm::TargetMachine::CGFT_ObjectFile;
    emitMachineCode(module, outputFile, fileType);

    if (compileOnly || emitAssembly) return 0;

    // Link the output.
    int ccExitStatus = std::system(("cc " + outputFile).c_str());
    std::remove(outputFile.c_str());
    return ccExitStatus;
}
