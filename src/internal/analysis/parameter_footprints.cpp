// SPDX-License-Identifier: Apache-2.0
#include "parameter_footprints.hpp"

#include "concurrency_symbol_classifier.hpp"
#include "ir_utils.hpp"

#include <llvm/IR/DataLayout.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/InstrTypes.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/IntrinsicInst.h>
#include <llvm/IR/Operator.h>

#include <algorithm>
#include <deque>
#include <optional>
#include <unordered_map>

namespace ctrace::concurrency::internal::analysis
{
    namespace
    {
        /// Beyond this many separate ranges a footprint keeps their span instead, which only
        /// ever widens it.
        // ponytail: one span past 16 ranges; a real interval set if wide objects need more.
        constexpr std::size_t kMaxRangesPerKind = 16;

        /// How a value relates to the parameter's object: a pointer into it, whose place the
        /// access path gives, or something reached from it — a pointer loaded out of it, or one
        /// the path cannot follow — which may point anywhere the object leads.
        enum class Reach
        {
            IntoObject,
            FromObject,
        };

        void touchWhole(ParameterFootprint::Extent& extent, AccessKind kind)
        {
            (kind == AccessKind::Write ? extent.writesWholeObject : extent.readsWholeObject) = true;
        }

        void addRange(ParameterFootprint::Extent& extent, AccessKind kind,
                      ParameterFootprint::Range range)
        {
            (kind == AccessKind::Write ? extent.writes : extent.reads).push_back(range);
        }

        ParameterFootprint wholeObject()
        {
            ParameterFootprint footprint;
            for (ParameterFootprint::Extent* extent : {&footprint.all, &footprint.unshown})
            {
                touchWhole(*extent, AccessKind::Read);
                touchWhole(*extent, AccessKind::Write);
            }
            return footprint;
        }

        void mergeRanges(std::vector<ParameterFootprint::Range>& ranges)
        {
            if (ranges.empty())
                return;

            std::sort(ranges.begin(), ranges.end(),
                      [](const ParameterFootprint::Range& lhs, const ParameterFootprint::Range& rhs)
                      { return lhs.begin < rhs.begin; });

            std::vector<ParameterFootprint::Range> merged{ranges.front()};
            for (auto it = std::next(ranges.begin()); it != ranges.end(); ++it)
            {
                ParameterFootprint::Range& last = merged.back();
                if (it->begin < last.end)
                {
                    last.end = std::max(last.end, it->end);
                    last.isAtomic = last.isAtomic && it->isAtomic;
                    continue;
                }
                merged.push_back(*it);
            }

            if (merged.size() > kMaxRangesPerKind)
            {
                ParameterFootprint::Range span{merged.front().begin, merged.front().end, true};
                for (const ParameterFootprint::Range& range : merged)
                {
                    span.end = std::max(span.end, range.end);
                    span.isAtomic = span.isAtomic && range.isAtomic;
                }
                merged = {span};
            }

            ranges = std::move(merged);
        }

        bool isIgnoredIntrinsic(const llvm::CallBase& call)
        {
            if (llvm::isa<llvm::DbgInfoIntrinsic>(call))
                return true;

            const auto* intrinsic = llvm::dyn_cast<llvm::IntrinsicInst>(&call);
            if (intrinsic == nullptr)
                return false;

            switch (intrinsic->getIntrinsicID())
            {
            case llvm::Intrinsic::lifetime_start:
            case llvm::Intrinsic::lifetime_end:
            case llvm::Intrinsic::assume:
                return true;
            default:
                return false;
            }
        }

        /// Builds one footprint by following every value derived from the parameter.
        class FootprintBuilder
        {
          public:
            FootprintBuilder(const llvm::Argument& parameter, const llvm::DataLayout& layout)
                : parameter_(parameter), layout_(layout)
            {
                note(parameter, Reach::IntoObject);
            }

            [[nodiscard]] bool nextUse(const llvm::Use*& use, Reach& reach)
            {
                while (pendingUses_.empty())
                {
                    if (pendingValues_.empty())
                        return false;

                    const llvm::Value* value = pendingValues_.front();
                    pendingValues_.pop_front();
                    for (const llvm::Use& valueUse : value->uses())
                        pendingUses_.push_back(&valueUse);
                }

                use = pendingUses_.front();
                pendingUses_.pop_front();
                reach = reach_.at(use->get());
                return true;
            }

            void note(const llvm::Value& value, Reach reach)
            {
                const auto [it, inserted] = reach_.emplace(&value, reach);
                if (inserted)
                {
                    pendingValues_.push_back(&value);
                    return;
                }

                if (reach == Reach::FromObject && it->second == Reach::IntoObject)
                {
                    it->second = Reach::FromObject;
                    pendingValues_.push_back(&value);
                }
            }

            /// Anything, anywhere in the object, and nothing the function's own accesses show.
            void escape()
            {
                for (ParameterFootprint::Extent* extent : {&footprint_.all, &footprint_.unshown})
                {
                    touchWhole(*extent, AccessKind::Read);
                    touchWhole(*extent, AccessKind::Write);
                }
            }

            /// Records an access the function makes itself through `pointer`, of `byteSize`
            /// bytes or, when zero, of the whole object `pointer` designates. One the access path
            /// places is an access of the function's own, shown at every call; one it cannot is
            /// not, and may be anywhere.
            void touchOwnAccess(const llvm::Value& pointer, Reach reach, AccessKind kind,
                                std::uint64_t byteSize, bool isAtomic)
            {
                const std::optional<MemoryRegion> place =
                    reach == Reach::IntoObject ? placeInObject(pointer, byteSize) : std::nullopt;
                if (!place.has_value())
                {
                    touchWhole(footprint_.all, kind);
                    if (!placedAtCalls(pointer, reach))
                        touchWhole(footprint_.unshown, kind);
                    return;
                }

                addRange(footprint_.all, kind, rangeOf(*place, isAtomic));
            }

            /// An access at the element an integer parameter picks is placed at each call, at the
            /// element the call passes or anywhere in the object when the call's is unknown.
            bool placedAtCalls(const llvm::Value& pointer, Reach reach) const
            {
                if (reach != Reach::IntoObject)
                    return false;

                const std::optional<RootBinding> root = rootInObject(pointer);
                return root.has_value() && root->index.has_value();
            }

            /// Records a thread handle the function writes through `pointer`, which no access
            /// of the function's own shows.
            void touchThreadHandle(const llvm::Value& pointer, Reach reach)
            {
                const std::optional<RootBinding> root =
                    reach == Reach::IntoObject ? rootInObject(pointer) : std::nullopt;
                if (!root.has_value() || !root->designated.hasKnownOffset ||
                    root->designated.byteSize == 0)
                {
                    touchWhole(footprint_.all, AccessKind::Write);
                    touchWhole(footprint_.unshown, AccessKind::Write);
                    return;
                }

                const ParameterFootprint::Range handle = rangeOf(root->designated, false);
                addRange(footprint_.all, AccessKind::Write, handle);
                addRange(footprint_.unshown, AccessKind::Write, handle);
            }

            /// Adds what a callee defined in the unit does through the pointer it is handed.
            /// Through a pointer the access path places, the callee's own accesses reach this
            /// function, and so does the effect inferred here for the rest: nothing of it is
            /// unshown. Through any other pointer none of it is carried, and the callee may
            /// reach anywhere the object leads.
            void touchThroughCallee(const llvm::Value& pointer, Reach reach,
                                    const ParameterFootprint& callee)
            {
                const std::optional<RootBinding> root =
                    reach == Reach::IntoObject ? rootInObject(pointer) : std::nullopt;
                if (!root.has_value() || !root->region.hasKnownOffset)
                {
                    for (const AccessKind kind : {AccessKind::Read, AccessKind::Write})
                    {
                        const bool touches = kind == AccessKind::Read ? callee.all.readsAnything()
                                                                      : callee.all.writesAnything();
                        if (!touches)
                            continue;
                        touchWhole(footprint_.all, kind);
                        if (!root.has_value())
                            touchWhole(footprint_.unshown, kind);
                    }
                    return;
                }

                const std::int64_t shift = root->region.byteOffset;
                for (const ParameterFootprint::Range& range : callee.all.reads)
                {
                    addRange(footprint_.all, AccessKind::Read,
                             {range.begin + shift, range.end + shift, range.isAtomic});
                }
                for (const ParameterFootprint::Range& range : callee.all.writes)
                {
                    addRange(footprint_.all, AccessKind::Write,
                             {range.begin + shift, range.end + shift, range.isAtomic});
                }
                if (callee.all.readsWholeObject)
                    touchDesignated(*root, AccessKind::Read);
                if (callee.all.writesWholeObject)
                    touchDesignated(*root, AccessKind::Write);
            }

            /// Adds a call to a function without a body: what its declaration allows, over the
            /// whole object the pointer designates. The effect inferred at that call is an access
            /// of the function's own when the pointer is placed.
            void touchThroughDeclaration(const llvm::Value& pointer, Reach reach, AccessKind kind)
            {
                const std::optional<RootBinding> root =
                    reach == Reach::IntoObject ? rootInObject(pointer) : std::nullopt;
                if (!root.has_value())
                {
                    touchWhole(footprint_.all, kind);
                    touchWhole(footprint_.unshown, kind);
                    return;
                }
                touchDesignated(*root, kind);
            }

            [[nodiscard]] ParameterFootprint finish()
            {
                for (ParameterFootprint::Extent* extent : {&footprint_.all, &footprint_.unshown})
                {
                    mergeRanges(extent->reads);
                    mergeRanges(extent->writes);
                }
                return std::move(footprint_);
            }

          private:
            static ParameterFootprint::Range rangeOf(const MemoryRegion& region, bool isAtomic)
            {
                return {region.byteOffset,
                        region.byteOffset + static_cast<std::int64_t>(region.byteSize), isAtomic};
            }

            std::optional<RootBinding> rootInObject(const llvm::Value& pointer) const
            {
                std::optional<RootBinding> root = resolveTrackedRoot(pointer, &layout_, 0);
                if (!root.has_value() || root->kind != RootBindingKind::Argument ||
                    root->argumentIndex != parameter_.getArgNo())
                {
                    return std::nullopt;
                }
                return root;
            }

            std::optional<MemoryRegion> placeInObject(const llvm::Value& pointer,
                                                      std::uint64_t byteSize) const
            {
                const std::optional<RootBinding> root =
                    resolveTrackedRoot(pointer, &layout_, byteSize);
                if (!root.has_value() || root->kind != RootBindingKind::Argument ||
                    root->argumentIndex != parameter_.getArgNo())
                {
                    return std::nullopt;
                }

                const MemoryRegion region = byteSize != 0 ? root->region : root->designated;
                if (!region.hasKnownOffset || region.byteSize == 0)
                    return std::nullopt;
                return region;
            }

            /// The object `root` designates, in `all`; its own access, so not in `unshown`.
            void touchDesignated(const RootBinding& root, AccessKind kind)
            {
                const MemoryRegion& designated = root.designated;
                if (!designated.hasKnownOffset || designated.byteSize == 0)
                {
                    touchWhole(footprint_.all, kind);
                    return;
                }
                addRange(footprint_.all, kind, rangeOf(designated, false));
            }

            const llvm::Argument& parameter_;
            const llvm::DataLayout& layout_;
            ParameterFootprint footprint_;
            std::unordered_map<const llvm::Value*, Reach> reach_;
            std::deque<const llvm::Value*> pendingValues_;
            std::deque<const llvm::Use*> pendingUses_;
        };

        /// Lifecycle calls whose first argument is the thread handle they write.
        bool writesThreadHandle(CallKind kind)
        {
            switch (kind)
            {
            case CallKind::PThreadCreate:
            case CallKind::StdThreadCtor:
            case CallKind::StdJThreadCtor:
            case CallKind::StdThreadMove:
            case CallKind::StdThreadJoin:
            case CallKind::StdThreadDetach:
            case CallKind::StdThreadDtor:
                return true;
            default:
                return false;
            }
        }

        /// Whether a value of this type may hold an address. Plain numbers do not lead anywhere,
        /// and following them would take every copy of a field for an escape.
        bool mayCarryAddress(const llvm::Type* type)
        {
            return !type->isVoidTy() && !type->isIntegerTy() && !type->isFloatingPointTy();
        }

        std::uint64_t storeSizeOf(const llvm::DataLayout& layout, llvm::Type* type)
        {
            if (type == nullptr || !type->isSized())
                return 0;

            return layout.getTypeStoreSize(type).getFixedValue();
        }

        std::uint64_t constantLength(const llvm::MemIntrinsic& intrinsic)
        {
            const auto* length = llvm::dyn_cast<llvm::ConstantInt>(intrinsic.getLength());
            return length == nullptr ? 0 : length->getZExtValue();
        }

        /// The value read back from a local variable the pointer was stored in carries the
        /// pointer on. A variable used any other way lets the pointer escape.
        bool forwardThroughLocalVariable(const llvm::AllocaInst& variable, Reach reach,
                                         FootprintBuilder& builder)
        {
            for (const llvm::User* user : variable.users())
            {
                if (llvm::isa<llvm::StoreInst>(user))
                    continue;

                if (const auto* load = llvm::dyn_cast<llvm::LoadInst>(user))
                {
                    builder.note(*load, reach);
                    continue;
                }

                if (const auto* call = llvm::dyn_cast<llvm::CallBase>(user);
                    call != nullptr && isIgnoredIntrinsic(*call))
                {
                    continue;
                }

                return false;
            }

            return true;
        }
    } // namespace

    bool ParameterFootprint::Extent::readsAnything() const noexcept
    {
        return readsWholeObject || !reads.empty();
    }

    bool ParameterFootprint::Extent::writesAnything() const noexcept
    {
        return writesWholeObject || !writes.empty();
    }

    ParameterFootprints::ParameterFootprints(const ConcurrencySymbolClassifier& classifier,
                                             const llvm::DataLayout& layout)
        : classifier_(classifier), layout_(layout)
    {
    }

    ParameterFootprint ParameterFootprints::of(const llvm::Function& function,
                                               unsigned argumentIndex)
    {
        const auto key = std::make_pair(&function, argumentIndex);
        if (const auto cached = cache_.find(key); cached != cache_.end())
            return cached->second;

        if (function.isDeclaration() || argumentIndex >= function.arg_size() ||
            !inProgress_.insert(key).second)
        {
            return wholeObject();
        }

        ParameterFootprint footprint = compute(function, argumentIndex);
        inProgress_.erase(key);
        cache_.emplace(key, footprint);
        return footprint;
    }

    ParameterFootprint ParameterFootprints::compute(const llvm::Function& function,
                                                    unsigned argumentIndex)
    {
        const llvm::Argument& parameter = *function.getArg(argumentIndex);
        if (!parameter.getType()->isPointerTy())
            return {};

        FootprintBuilder builder(parameter, layout_);
        const llvm::Use* use = nullptr;
        Reach reach = Reach::IntoObject;
        while (builder.nextUse(use, reach))
        {
            const llvm::Value& value = *use->get();
            const llvm::User* user = use->getUser();

            if (const auto* gep = llvm::dyn_cast<llvm::GEPOperator>(user))
            {
                if (gep->getPointerOperand() == &value)
                    builder.note(*gep, reach);
                else
                    builder.escape();
                continue;
            }

            if (llvm::isa<llvm::BitCastInst>(user) || llvm::isa<llvm::AddrSpaceCastInst>(user))
            {
                builder.note(*user, reach);
                continue;
            }

            if (llvm::isa<llvm::PHINode>(user) || llvm::isa<llvm::SelectInst>(user))
            {
                builder.note(*user, Reach::FromObject);
                continue;
            }

            if (llvm::isa<llvm::ICmpInst>(user) || llvm::isa<llvm::ReturnInst>(user))
                continue;

            if (const auto* load = llvm::dyn_cast<llvm::LoadInst>(user))
            {
                builder.touchOwnAccess(value, reach, AccessKind::Read,
                                       storeSizeOf(layout_, load->getType()), load->isAtomic());
                // What is read out of the object may lead further into memory the object owns.
                if (mayCarryAddress(load->getType()))
                    builder.note(*load, Reach::FromObject);
                continue;
            }

            if (const auto* store = llvm::dyn_cast<llvm::StoreInst>(user))
            {
                if (store->getPointerOperand() == &value)
                {
                    builder.touchOwnAccess(
                        value, reach, AccessKind::Write,
                        storeSizeOf(layout_, store->getValueOperand()->getType()),
                        store->isAtomic());
                    continue;
                }

                const auto* variable = llvm::dyn_cast<llvm::AllocaInst>(
                    store->getPointerOperand()->stripPointerCasts());
                if (variable == nullptr || !forwardThroughLocalVariable(*variable, reach, builder))
                    builder.escape();
                continue;
            }

            if (const auto* atomicRmw = llvm::dyn_cast<llvm::AtomicRMWInst>(user))
            {
                if (atomicRmw->getPointerOperand() != &value)
                {
                    builder.escape();
                    continue;
                }
                builder.touchOwnAccess(value, reach, AccessKind::Write,
                                       storeSizeOf(layout_, atomicRmw->getType()), true);
                continue;
            }

            if (const auto* exchange = llvm::dyn_cast<llvm::AtomicCmpXchgInst>(user))
            {
                if (exchange->getPointerOperand() != &value)
                {
                    builder.escape();
                    continue;
                }
                builder.touchOwnAccess(
                    value, reach, AccessKind::Write,
                    storeSizeOf(layout_, exchange->getCompareOperand()->getType()), true);
                continue;
            }

            if (const auto* call = llvm::dyn_cast<llvm::CallBase>(user))
            {
                if (isIgnoredIntrinsic(*call))
                    continue;

                if (!call->isArgOperand(use))
                {
                    // Called through, or bundled: nothing says what happens to it.
                    builder.escape();
                    continue;
                }

                const unsigned argumentNumber = call->getArgOperandNo(use);
                if (const auto* intrinsic = llvm::dyn_cast<llvm::MemIntrinsic>(call))
                {
                    const bool isDestination = argumentNumber == 0;
                    builder.touchOwnAccess(value, reach,
                                           isDestination ? AccessKind::Write : AccessKind::Read,
                                           constantLength(*intrinsic), false);
                    continue;
                }

                // Synchronization is understood where it happens, not as data. Starting, joining
                // or detaching a thread still writes the handle it is given, which is part of the
                // object when the object holds its thread; no access of the function's own shows
                // that write.
                if (const CallKind kind = classifier_.classify(*call); kind != CallKind::Unknown)
                {
                    if (writesThreadHandle(kind) && argumentNumber == 0)
                        builder.touchThreadHandle(value, reach);
                    continue;
                }

                const llvm::Function* callee = classifier_.directCallee(*call);
                if (callee != nullptr && !callee->isDeclaration() &&
                    argumentNumber < callee->arg_size())
                {
                    builder.touchThroughCallee(value, reach, of(*callee, argumentNumber));
                }
                else if (!call->doesNotAccessMemory(argumentNumber))
                {
                    builder.touchThroughDeclaration(value, reach,
                                                    call->onlyReadsMemory(argumentNumber)
                                                        ? AccessKind::Read
                                                        : AccessKind::Write);
                }

                // A pointer returned by a call handed the object may lead anywhere it does.
                if (mayCarryAddress(call->getType()) && !call->use_empty())
                    builder.note(*call, Reach::FromObject);
                continue;
            }

            builder.escape();
        }

        return builder.finish();
    }
} // namespace ctrace::concurrency::internal::analysis
