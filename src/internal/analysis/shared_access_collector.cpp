// SPDX-License-Identifier: Apache-2.0
#include "shared_access_collector.hpp"

#include "ir_utils.hpp"
#include "llvm_function_analysis_provider.hpp"

#include <llvm/IR/Function.h>
#include <llvm/IR/GlobalVariable.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/Module.h>

namespace ctrace::concurrency::internal::analysis
{
    std::vector<PendingAccess> SharedAccessCollector::collect(const llvm::Module& module) const
    {
        std::vector<PendingAccess> accesses;
        std::vector<const llvm::GlobalVariable*> trackedGlobals;
        trackedGlobals.reserve(module.global_size());
        for (const llvm::GlobalVariable& global : module.globals())
        {
            if (shouldTrackSharedGlobal(global))
                trackedGlobals.push_back(&global);
        }

        LlvmFunctionAnalysisProvider analysisProvider;

        for (const llvm::Function& function : module)
        {
            if (function.isDeclaration())
                continue;

            llvm::AAResults& aaResults = analysisProvider.getAAResults(function);

            for (const llvm::BasicBlock& block : function)
            {
                for (const llvm::Instruction& instruction : block)
                {
                    const llvm::Value* pointerOperand = nullptr;
                    AccessKind kind = AccessKind::Read;
                    AliasProvenance aliasProvenance = AliasProvenance::Direct;

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

                    std::optional<RootBinding> root = resolveTrackedRoot(*pointerOperand);
                    if (!root.has_value())
                    {
                        const std::optional<AliasResolvedGlobal> aliasResolvedGlobal =
                            resolveAliasGlobal(instruction, aaResults, trackedGlobals);
                        if (!aliasResolvedGlobal.has_value())
                            continue;

                        root = RootBinding::global(aliasResolvedGlobal->symbol);
                        aliasProvenance = aliasResolvedGlobal->aliasProvenance;
                    }

                    PendingAccess access;
                    access.function = &function;
                    access.instruction = &instruction;
                    access.root = *root;
                    access.fact.functionId = functionId(function);
                    access.fact.kind = kind;
                    access.fact.aliasProvenance = aliasProvenance;
                    const ResolvedSourceLocations locations = resolveSourceLocations(instruction);
                    access.fact.loweredLocation = locations.loweredLocation;
                    access.fact.userLocation = locations.userLocation;
                    accesses.push_back(std::move(access));
                }
            }
        }

        return accesses;
    }
} // namespace ctrace::concurrency::internal::analysis
