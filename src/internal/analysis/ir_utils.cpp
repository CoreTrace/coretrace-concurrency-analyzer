// SPDX-License-Identifier: Apache-2.0
#include "ir_utils.hpp"

#include <llvm/IR/DebugInfoMetadata.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/GlobalVariable.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/Operator.h>
#include <llvm/IR/Value.h>

#include <filesystem>

namespace ctrace::concurrency::internal::analysis
{
    namespace
    {
        std::string normalizeFunctionName(llvm::StringRef name)
        {
            if (name.starts_with("\x01"))
                name = name.drop_front();
            return name.str();
        }
    } // namespace

    const llvm::GlobalVariable* resolveBaseGlobal(const llvm::Value& value)
    {
        const llvm::Value* current = value.stripPointerCastsAndAliases();
        while (current != nullptr)
        {
            if (const auto* global = llvm::dyn_cast<llvm::GlobalVariable>(current))
                return global;

            if (const auto* gep = llvm::dyn_cast<llvm::GEPOperator>(current))
            {
                current = gep->getPointerOperand()->stripPointerCastsAndAliases();
                continue;
            }

            return nullptr;
        }

        return nullptr;
    }

    std::optional<std::string> canonicalGlobalId(const llvm::Value& value)
    {
        const llvm::GlobalVariable* global = resolveBaseGlobal(value);
        if (global == nullptr)
            return std::nullopt;

        return normalizeFunctionName(global->getName());
    }

    std::string functionId(const llvm::Function& function)
    {
        return normalizeFunctionName(function.getName());
    }

    std::string functionDisplayName(const llvm::Function& function)
    {
        if (const llvm::DISubprogram* subprogram = function.getSubprogram())
        {
            if (!subprogram->getName().empty())
                return subprogram->getName().str();
        }

        return functionId(function);
    }

    SourceLocation makeSourceLocation(const llvm::Instruction& instruction)
    {
        SourceLocation location;
        location.function = functionDisplayName(*instruction.getFunction());

        const llvm::DebugLoc debugLocation = instruction.getDebugLoc();
        if (!debugLocation)
            return location;

        location.line = debugLocation.getLine();
        location.column = debugLocation.getCol();
        location.endLine = location.line;
        location.endColumn = location.column;

        const std::string filename = debugLocation->getFilename().str();
        const std::string directory = debugLocation->getDirectory().str();
        if (filename.empty())
            return location;

        std::filesystem::path filePath(filename);
        if (!directory.empty() && filePath.is_relative())
            filePath = std::filesystem::path(directory) / filePath;
        location.file = filePath.lexically_normal().string();
        return location;
    }
} // namespace ctrace::concurrency::internal::analysis
