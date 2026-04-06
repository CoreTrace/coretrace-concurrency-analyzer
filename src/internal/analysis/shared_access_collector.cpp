// SPDX-License-Identifier: Apache-2.0
#include "shared_access_collector.hpp"

#include "ir_utils.hpp"

#include <llvm/IR/Function.h>
#include <llvm/IR/GlobalVariable.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/Module.h>

namespace ctrace::concurrency::internal::analysis
{
    namespace
    {
        bool shouldTrackGlobal(const llvm::GlobalVariable& global)
        {
            return global.hasExternalLinkage() && !global.isConstant() && !global.isThreadLocal();
        }
    } // namespace

    std::vector<PendingAccess> SharedAccessCollector::collect(const llvm::Module& module) const
    {
        std::vector<PendingAccess> accesses;

        for (const llvm::Function& function : module)
        {
            if (function.isDeclaration())
                continue;

            for (const llvm::BasicBlock& block : function)
            {
                for (const llvm::Instruction& instruction : block)
                {
                    const llvm::Value* pointerOperand = nullptr;
                    AccessKind kind = AccessKind::Read;

                    if (const auto* load = llvm::dyn_cast<llvm::LoadInst>(&instruction))
                    {
                        pointerOperand = load->getPointerOperand();
                        kind = AccessKind::Read;
                    }
                    else if (const auto* store = llvm::dyn_cast<llvm::StoreInst>(&instruction))
                    {
                        pointerOperand = store->getPointerOperand();
                        kind = AccessKind::Write;
                    }
                    else
                    {
                        continue;
                    }

                    if (pointerOperand == nullptr)
                        continue;

                    const llvm::GlobalVariable* global = resolveBaseGlobal(*pointerOperand);
                    if (global == nullptr || !shouldTrackGlobal(*global))
                        continue;

                    PendingAccess access;
                    access.function = &function;
                    access.instruction = &instruction;
                    access.fact.symbol = global->getName().str();
                    access.fact.functionId = functionId(function);
                    access.fact.kind = kind;
                    access.fact.location = makeSourceLocation(instruction);
                    accesses.push_back(std::move(access));
                }
            }
        }

        return accesses;
    }
} // namespace ctrace::concurrency::internal::analysis
