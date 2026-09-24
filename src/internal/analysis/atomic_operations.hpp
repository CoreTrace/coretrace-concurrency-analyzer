// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <optional>
#include <unordered_map>

namespace llvm
{
    class Function;
    class Instruction;
    class Value;
} // namespace llvm

namespace ctrace::concurrency::internal::analysis
{
    /// The C++ memory orders, with `consume` folded into `acquire` as every compiler does.
    enum class MemoryOrder
    {
        Relaxed,
        Acquire,
        Release,
        AcquireRelease,
        SequentiallyConsistent,
    };

    enum class AtomicOperationKind
    {
        Load,
        Store,
        ReadModifyWrite,
        Fence,
    };

    struct AtomicOperation
    {
        AtomicOperationKind kind = AtomicOperationKind::Load;
        /// Absent when the order is not a constant at the operation, so nothing can be said
        /// about what it orders.
        std::optional<MemoryOrder> order;
        /// The atomic object; null for a fence.
        const llvm::Value* object = nullptr;
        /// What a store writes; null for every other kind.
        const llvm::Value* storedValue = nullptr;
    };

    /// What a standard library atomic member does, read once from its name: the operation, and
    /// where its call carries the order and the stored value.
    struct AtomicMember
    {
        AtomicOperationKind kind = AtomicOperationKind::Load;
        /// Index of the `std::memory_order` argument; absent when the member implies the order.
        std::optional<unsigned> orderArgument;
        /// The order a member without an order argument always uses.
        MemoryOrder impliedOrder = MemoryOrder::SequentiallyConsistent;
        std::optional<unsigned> storedValueArgument;
    };

    /// Recognizes the atomic operations of one unit, whether an instruction is atomic itself or
    /// calls a standard library atomic member or fence.
    ///
    /// Without optimization a standard library keeps `x.store(v, order)` as a call that receives
    /// the order as an argument, so the order is read at the call site rather than inside the
    /// library, where every order is spelled out behind a switch. Members are recognized from
    /// their demangled names, once per callee.
    class AtomicOperationRecognizer
    {
      public:
        [[nodiscard]] std::optional<AtomicOperation>
        operationOf(const llvm::Instruction& instruction);

      private:
        std::unordered_map<const llvm::Function*, std::optional<AtomicMember>> members_;
    };

    [[nodiscard]] bool releasesAtLeast(MemoryOrder order);
    [[nodiscard]] bool acquiresAtLeast(MemoryOrder order);
} // namespace ctrace::concurrency::internal::analysis
