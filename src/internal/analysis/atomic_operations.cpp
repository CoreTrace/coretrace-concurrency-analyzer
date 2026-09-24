// SPDX-License-Identifier: Apache-2.0
#include "atomic_operations.hpp"

#include <llvm/ADT/StringRef.h>
#include <llvm/Demangle/Demangle.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/InstrTypes.h>
#include <llvm/IR/Instructions.h>

#include <cstdlib>
#include <string>

namespace ctrace::concurrency::internal::analysis
{
    namespace
    {
        std::optional<MemoryOrder> fromIrOrdering(llvm::AtomicOrdering ordering)
        {
            switch (ordering)
            {
            case llvm::AtomicOrdering::Unordered:
            case llvm::AtomicOrdering::Monotonic:
                return MemoryOrder::Relaxed;
            case llvm::AtomicOrdering::Acquire:
                return MemoryOrder::Acquire;
            case llvm::AtomicOrdering::Release:
                return MemoryOrder::Release;
            case llvm::AtomicOrdering::AcquireRelease:
                return MemoryOrder::AcquireRelease;
            case llvm::AtomicOrdering::SequentiallyConsistent:
                return MemoryOrder::SequentiallyConsistent;
            case llvm::AtomicOrdering::NotAtomic:
                break;
            }
            return std::nullopt;
        }

        /// `std::memory_order` has the same values in libc++ and libstdc++: relaxed, consume,
        /// acquire, release, acq_rel, seq_cst.
        std::optional<MemoryOrder> fromMemoryOrderArgument(const llvm::Value& argument)
        {
            const auto* constant = llvm::dyn_cast<llvm::ConstantInt>(&argument);
            if (constant == nullptr)
                return std::nullopt;

            switch (constant->getZExtValue())
            {
            case 0:
                return MemoryOrder::Relaxed;
            case 1:
            case 2:
                return MemoryOrder::Acquire;
            case 3:
                return MemoryOrder::Release;
            case 4:
                return MemoryOrder::AcquireRelease;
            case 5:
                return MemoryOrder::SequentiallyConsistent;
            default:
                return std::nullopt;
            }
        }

        struct DemangledName
        {
            std::string context;
            std::string base;
        };

        std::optional<DemangledName> demangleFunction(llvm::StringRef mangled)
        {
            // The demangler keeps pointers into its input, so the input must outlive it.
            const std::string input = mangled.str();
            llvm::ItaniumPartialDemangler demangler;
            if (demangler.partialDemangle(input.c_str()) || !demangler.isFunction())
                return std::nullopt;

            std::size_t size = 0;
            char* context = demangler.getFunctionDeclContextName(nullptr, &size);
            char* base = demangler.getFunctionBaseName(nullptr, &size);
            std::optional<DemangledName> name;
            if (context != nullptr && base != nullptr)
            {
                // libc++ tags its members with the ABI version (`store[abi:ne200100]`).
                std::string baseName = base;
                baseName = baseName.substr(0, baseName.find("[abi:"));
                name = DemangledName{.context = context, .base = std::move(baseName)};
            }
            std::free(context);
            std::free(base);
            return name;
        }

        /// `std::atomic<T>` and the bases both libraries derive it from. `atomic_ref` is left out:
        /// its object is the referenced one, not the member function's `this`.
        bool isStdAtomicClass(llvm::StringRef context)
        {
            if (!context.starts_with("std::") || context.contains("atomic_ref"))
                return false;

            return context.contains("::atomic<") || context.contains("::__atomic_base<");
        }

        bool isStdNamespace(llvm::StringRef context)
        {
            return context == "std" || context == "std::__1";
        }

        /// The atomic member or fence a callee is, from its demangled name.
        std::optional<AtomicMember> atomicMemberOf(const llvm::Function& callee)
        {
            // Every member and fence recognized below spells "atomic" in its mangled name
            // (`6atomicI`, `13__atomic_base`, `19atomic_thread_fence`); checking that first spares
            // demangling every other callee of a unit.
            if (!callee.getName().starts_with("_Z") || !callee.getName().contains("atomic"))
                return std::nullopt;

            const std::optional<DemangledName> name = demangleFunction(callee.getName());
            if (!name.has_value())
                return std::nullopt;

            const llvm::StringRef base = name->base;
            if (isStdNamespace(name->context) && base == "atomic_thread_fence")
                return AtomicMember{.kind = AtomicOperationKind::Fence, .orderArgument = 0U};

            if (!isStdAtomicClass(name->context))
                return std::nullopt;

            // Arguments count the object as the first one.
            if (base == "store")
            {
                return AtomicMember{.kind = AtomicOperationKind::Store,
                                    .orderArgument = 2U,
                                    .storedValueArgument = 1U};
            }
            if (base == "operator=")
                return AtomicMember{.kind = AtomicOperationKind::Store, .storedValueArgument = 1U};
            if (base == "load")
                return AtomicMember{.kind = AtomicOperationKind::Load, .orderArgument = 1U};
            // A conversion operator reads the value with the default order; its demangled base
            // name is "operator" followed by the target type.
            if (base.starts_with("operator "))
                return AtomicMember{.kind = AtomicOperationKind::Load};
            if (base.starts_with("fetch_") || base == "exchange")
                return AtomicMember{.kind = AtomicOperationKind::ReadModifyWrite,
                                    .orderArgument = 2U};
            if (base.starts_with("compare_exchange_"))
                return AtomicMember{.kind = AtomicOperationKind::ReadModifyWrite,
                                    .orderArgument = 3U};
            if (base == "operator++" || base == "operator--" || base == "operator+=" ||
                base == "operator-=" || base == "operator&=" || base == "operator|=" ||
                base == "operator^=")
            {
                return AtomicMember{.kind = AtomicOperationKind::ReadModifyWrite};
            }
            return std::nullopt;
        }

        std::optional<AtomicOperation> operationOfMemberCall(const llvm::CallBase& call,
                                                             const AtomicMember& member)
        {
            AtomicOperation operation{.kind = member.kind, .order = member.impliedOrder};
            if (member.orderArgument.has_value())
            {
                if (call.arg_size() <= *member.orderArgument)
                    return std::nullopt;
                operation.order =
                    fromMemoryOrderArgument(*call.getArgOperand(*member.orderArgument));
            }
            if (member.kind == AtomicOperationKind::Fence)
                return operation;

            if (call.arg_size() == 0)
                return std::nullopt;
            operation.object = call.getArgOperand(0);
            if (member.storedValueArgument.has_value())
            {
                if (call.arg_size() <= *member.storedValueArgument)
                    return std::nullopt;
                operation.storedValue = call.getArgOperand(*member.storedValueArgument);
            }
            return operation;
        }
    } // namespace

    std::optional<AtomicOperation>
    AtomicOperationRecognizer::operationOf(const llvm::Instruction& instruction)
    {
        if (const auto* store = llvm::dyn_cast<llvm::StoreInst>(&instruction))
        {
            if (!store->isAtomic())
                return std::nullopt;
            return AtomicOperation{.kind = AtomicOperationKind::Store,
                                   .order = fromIrOrdering(store->getOrdering()),
                                   .object = store->getPointerOperand(),
                                   .storedValue = store->getValueOperand()};
        }
        if (const auto* load = llvm::dyn_cast<llvm::LoadInst>(&instruction))
        {
            if (!load->isAtomic())
                return std::nullopt;
            return AtomicOperation{.kind = AtomicOperationKind::Load,
                                   .order = fromIrOrdering(load->getOrdering()),
                                   .object = load->getPointerOperand()};
        }
        if (const auto* rmw = llvm::dyn_cast<llvm::AtomicRMWInst>(&instruction))
        {
            return AtomicOperation{.kind = AtomicOperationKind::ReadModifyWrite,
                                   .order = fromIrOrdering(rmw->getOrdering()),
                                   .object = rmw->getPointerOperand()};
        }
        if (const auto* exchange = llvm::dyn_cast<llvm::AtomicCmpXchgInst>(&instruction))
        {
            return AtomicOperation{.kind = AtomicOperationKind::ReadModifyWrite,
                                   .order = fromIrOrdering(exchange->getSuccessOrdering()),
                                   .object = exchange->getPointerOperand()};
        }
        if (const auto* fence = llvm::dyn_cast<llvm::FenceInst>(&instruction))
        {
            return AtomicOperation{.kind = AtomicOperationKind::Fence,
                                   .order = fromIrOrdering(fence->getOrdering())};
        }
        if (const auto* call = llvm::dyn_cast<llvm::CallBase>(&instruction))
        {
            const llvm::Function* callee = call->getCalledFunction();
            if (callee == nullptr)
                return std::nullopt;

            auto [it, inserted] = members_.try_emplace(callee);
            if (inserted)
                it->second = atomicMemberOf(*callee);
            if (!it->second.has_value())
                return std::nullopt;
            return operationOfMemberCall(*call, *it->second);
        }
        return std::nullopt;
    }

    bool releasesAtLeast(MemoryOrder order)
    {
        return order == MemoryOrder::Release || order == MemoryOrder::AcquireRelease ||
               order == MemoryOrder::SequentiallyConsistent;
    }

    bool acquiresAtLeast(MemoryOrder order)
    {
        return order == MemoryOrder::Acquire || order == MemoryOrder::AcquireRelease ||
               order == MemoryOrder::SequentiallyConsistent;
    }
} // namespace ctrace::concurrency::internal::analysis
