// SPDX-License-Identifier: Apache-2.0
#include "lock_effect_application.hpp"

#include <llvm/IR/BasicBlock.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/InstrTypes.h>
#include <llvm/IR/Instruction.h>

#include <algorithm>

namespace ctrace::concurrency::internal::analysis
{
    FunctionLockEffects collectFunctionLockEffects(const llvm::Function& function,
                                                   const SynchronizationEffectResolver& resolver)
    {
        FunctionLockEffects effects;

        for (const llvm::BasicBlock& block : function)
        {
            for (const llvm::Instruction& instruction : block)
            {
                const auto* call = llvm::dyn_cast<llvm::CallBase>(&instruction);
                if (call == nullptr)
                    continue;

                std::vector<LockEffect> instructionEffects = resolver.resolve(*call);
                if (instructionEffects.empty())
                    continue;

                for (const LockEffect& effect : instructionEffects)
                {
                    if (effect.kind != LockEffectKind::GuardAcquire)
                        continue;

                    std::vector<std::string>& bound = effects.guardBindings[effect.guardId];
                    for (const std::string& lockId : effect.lockIds)
                    {
                        if (std::find(bound.begin(), bound.end(), lockId) == bound.end())
                            bound.push_back(lockId);
                    }
                }

                effects.effectsByInstruction.emplace(&instruction, std::move(instructionEffects));
            }
        }

        return effects;
    }

    LockStateChange lockStateChangeFor(const llvm::Instruction& instruction,
                                       const FunctionLockEffects& effects)
    {
        LockStateChange change;

        const auto effectsIt = effects.effectsByInstruction.find(&instruction);
        if (effectsIt == effects.effectsByInstruction.end())
            return change;

        for (const LockEffect& effect : effectsIt->second)
        {
            switch (effect.kind)
            {
            case LockEffectKind::Acquire:
            case LockEffectKind::GuardAcquire:
                change.acquired.insert(change.acquired.end(), effect.lockIds.begin(),
                                       effect.lockIds.end());
                change.orderedAcquired.insert(change.orderedAcquired.end(), effect.lockIds.begin(),
                                              effect.lockIds.end());
                if (effect.recursiveLock)
                {
                    change.recursivelyAcquirable.insert(change.recursivelyAcquirable.end(),
                                                        effect.lockIds.begin(),
                                                        effect.lockIds.end());
                }
                break;
            case LockEffectKind::TryAcquire:
                change.acquired.insert(change.acquired.end(), effect.lockIds.begin(),
                                       effect.lockIds.end());
                if (effect.recursiveLock)
                {
                    change.recursivelyAcquirable.insert(change.recursivelyAcquirable.end(),
                                                        effect.lockIds.begin(),
                                                        effect.lockIds.end());
                }
                break;
            case LockEffectKind::Release:
                change.released.insert(change.released.end(), effect.lockIds.begin(),
                                       effect.lockIds.end());
                break;
            case LockEffectKind::GuardRelease:
                if (const auto boundIt = effects.guardBindings.find(effect.guardId);
                    boundIt != effects.guardBindings.end())
                {
                    change.released.insert(change.released.end(), boundIt->second.begin(),
                                           boundIt->second.end());
                }
                break;
            }
        }

        return change;
    }
} // namespace ctrace::concurrency::internal::analysis
