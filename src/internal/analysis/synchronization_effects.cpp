// SPDX-License-Identifier: Apache-2.0
#include "synchronization_effects.hpp"

#include "concurrency_symbol_classifier.hpp"
#include "ir_utils.hpp"

#include <llvm/IR/InstrTypes.h>
#include <llvm/IR/Value.h>

#include <optional>

namespace ctrace::concurrency::internal::analysis
{
    namespace
    {
        /// Position of the lock operand for a plain (non-RAII) lock operation. POSIX primitives and
        /// C++ member functions both carry it first, as the object pointer or the `this` pointer.
        constexpr unsigned kLockOperandIndex = 0;

        /// Guard constructors take `this` first and the guarded mutexes afterwards.
        constexpr unsigned kFirstGuardedLockIndex = 1;

        std::optional<LockEffectKind> plainLockEffect(CallKind kind)
        {
            switch (kind)
            {
            case CallKind::PThreadMutexLock:
            case CallKind::PThreadRwLockAcquire:
            case CallKind::PThreadSpinLock:
            case CallKind::StdMutexLock:
                return LockEffectKind::Acquire;
            case CallKind::PThreadMutexTryLock:
            case CallKind::PThreadRwLockTryAcquire:
            case CallKind::PThreadSpinTryLock:
            case CallKind::StdMutexTryLock:
                return LockEffectKind::TryAcquire;
            case CallKind::PThreadMutexUnlock:
            case CallKind::PThreadRwLockUnlock:
            case CallKind::PThreadSpinUnlock:
            case CallKind::StdMutexUnlock:
                return LockEffectKind::Release;
            default:
                return std::nullopt;
            }
        }
    } // namespace

    SynchronizationEffectResolver::SynchronizationEffectResolver(
        const ConcurrencySymbolClassifier& classifier, const llvm::DataLayout& layout)
        : classifier_(classifier), layout_(layout)
    {
    }

    std::vector<LockEffect> SynchronizationEffectResolver::resolve(const llvm::CallBase& call) const
    {
        const CallKind kind = classifier_.classify(call);
        if (call.arg_size() == 0)
            return {};

        if (const std::optional<LockEffectKind> plainEffect = plainLockEffect(kind);
            plainEffect.has_value())
        {
            const llvm::Value& lockOperand = *call.getArgOperand(kLockOperandIndex);
            const std::optional<std::string> lockId = canonicalLockId(lockOperand, &layout_);
            if (!lockId.has_value())
                return {};

            return {LockEffect{
                .kind = *plainEffect,
                .lockIds = {*lockId},
                .recursiveLock = designatesRecursiveLockType(lockOperand) ||
                                 classifier_.targetsRecursiveLock(call),
            }};
        }

        if (kind == CallKind::StdLockGuardDtor || kind == CallKind::StdLockGuardDeferredCtor)
        {
            const std::optional<std::string> guardId =
                canonicalStorageGroupId(*call.getArgOperand(kLockOperandIndex));
            if (!guardId.has_value())
                return {};

            // A deferred constructor binds the mutex to the guard without locking it; only the
            // destructor's release is modelled, and it will find an empty binding.
            if (kind == CallKind::StdLockGuardDeferredCtor)
                return {};

            return {LockEffect{
                .kind = LockEffectKind::GuardRelease,
                .guardId = *guardId,
            }};
        }

        if (kind != CallKind::StdLockGuardCtor)
            return {};

        const std::optional<std::string> guardId =
            canonicalStorageGroupId(*call.getArgOperand(kLockOperandIndex));
        if (!guardId.has_value())
            return {};

        LockEffect effect;
        effect.kind = LockEffectKind::GuardAcquire;
        effect.guardId = *guardId;
        for (unsigned index = kFirstGuardedLockIndex; index < call.arg_size(); ++index)
        {
            const llvm::Value& operand = *call.getArgOperand(index);
            if (!operand.getType()->isPointerTy())
                continue;

            if (const std::optional<std::string> lockId = canonicalLockId(operand, &layout_);
                lockId.has_value())
            {
                effect.lockIds.push_back(*lockId);
                effect.recursiveLock = effect.recursiveLock ||
                                       designatesRecursiveLockType(operand) ||
                                       classifier_.targetsRecursiveLock(call);
            }
        }

        if (effect.lockIds.empty())
            return {};

        return {std::move(effect)};
    }
} // namespace ctrace::concurrency::internal::analysis
