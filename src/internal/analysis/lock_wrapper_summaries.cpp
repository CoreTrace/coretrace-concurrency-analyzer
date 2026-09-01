// SPDX-License-Identifier: Apache-2.0
#include "lock_wrapper_summaries.hpp"

#include "ir_utils.hpp"
#include "lock_effect_application.hpp"
#include "synchronization_effects.hpp"

#include <llvm/IR/BasicBlock.h>
#include <llvm/IR/Dominators.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/Module.h>

#include <algorithm>
#include <string>

namespace ctrace::concurrency::internal::analysis
{
    namespace
    {
        /// Enough rounds for wrappers calling wrappers, and a bound so a cycle in the call graph
        /// cannot spin. Real lock helpers nest one or two deep.
        constexpr unsigned kMaxRounds = 4;

        std::string parameterPlaceholder(unsigned argumentIndex)
        {
            return "%arg" + std::to_string(argumentIndex);
        }

        /// Net effect the function has on the lock passed as `argumentIndex`, as observed by its
        /// caller once the call returns.
        ///
        /// Only an effect that happens on every path out is reported. A conditional acquisition
        /// would tell the caller it holds a lock it may not hold, which turns a real race into a
        /// silent one — the failure mode worth avoiding here.
        std::optional<ParameterLockEffect>
        netEffectOnParameter(const llvm::Function& function, unsigned argumentIndex,
                             const FunctionLockEffects& effects,
                             const llvm::DominatorTree& dominatorTree)
        {
            const std::string placeholder = parameterPlaceholder(argumentIndex);

            const llvm::Instruction* acquisition = nullptr;
            const llvm::Instruction* release = nullptr;
            bool recursiveLock = false;
            unsigned acquisitionCount = 0;
            unsigned releaseCount = 0;

            for (const auto& [instruction, instructionEffects] : effects.effectsByInstruction)
            {
                for (const LockEffect& effect : instructionEffects)
                {
                    if (std::find(effect.lockIds.begin(), effect.lockIds.end(), placeholder) ==
                        effect.lockIds.end())
                    {
                        continue;
                    }

                    if (effect.kind == LockEffectKind::Acquire ||
                        effect.kind == LockEffectKind::TryAcquire ||
                        effect.kind == LockEffectKind::GuardAcquire)
                    {
                        acquisition = instruction;
                        ++acquisitionCount;
                        recursiveLock = recursiveLock || effect.recursiveLock;
                    }
                    else
                    {
                        release = instruction;
                        ++releaseCount;
                    }
                }
            }

            // A function that both takes and drops the lock leaves its caller as it found it, and
            // one that touches it more than once is past what this summary can describe.
            if (acquisitionCount + releaseCount != 1)
                return std::nullopt;

            const llvm::Instruction* effectSite = acquisition != nullptr ? acquisition : release;
            for (const llvm::BasicBlock& block : function)
            {
                if (!llvm::isa<llvm::ReturnInst>(block.getTerminator()))
                    continue;

                if (!dominatorTree.dominates(effectSite, block.getTerminator()))
                    return std::nullopt;
            }

            return ParameterLockEffect{
                .argumentIndex = argumentIndex,
                .acquires = acquisition != nullptr,
                .recursiveLock = recursiveLock,
            };
        }
    } // namespace

    LockWrapperSummaries collectLockWrapperSummaries(const llvm::Module& module,
                                                     const ConcurrencySymbolClassifier& classifier,
                                                     const llvm::DataLayout& layout)
    {
        LockWrapperSummaries summaries;

        for (unsigned round = 0; round < kMaxRounds; ++round)
        {
            LockWrapperSummaries discovered;
            // Read from the previous round and write to a fresh map, so a summary never depends on
            // the order functions happen to be visited in.
            const SynchronizationEffectResolver resolver(classifier, layout, &summaries,
                                                         /*nameParameterLocks=*/true);

            for (const llvm::Function& function : module)
            {
                if (function.isDeclaration() || function.arg_empty())
                    continue;

                const FunctionLockEffects effects = collectFunctionLockEffects(function, resolver);
                if (effects.effectsByInstruction.empty())
                    continue;

                llvm::Function& mutableFunction = const_cast<llvm::Function&>(function);
                const llvm::DominatorTree dominatorTree(mutableFunction);

                std::vector<ParameterLockEffect> parameterEffects;
                for (const llvm::Argument& argument : function.args())
                {
                    if (!argument.getType()->isPointerTy())
                        continue;

                    if (const std::optional<ParameterLockEffect> effect = netEffectOnParameter(
                            function, argument.getArgNo(), effects, dominatorTree);
                        effect.has_value())
                    {
                        parameterEffects.push_back(*effect);
                    }
                }

                if (!parameterEffects.empty())
                    discovered.emplace(functionId(function), std::move(parameterEffects));
            }

            if (discovered == summaries)
                break;

            summaries = std::move(discovered);
        }

        return summaries;
    }
} // namespace ctrace::concurrency::internal::analysis
