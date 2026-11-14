// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstdint>
#include <map>
#include <set>
#include <utility>
#include <vector>

namespace llvm
{
    class DataLayout;
    class Function;
} // namespace llvm

namespace ctrace::concurrency::internal::analysis
{
    class ConcurrencySymbolClassifier;

    /// What a function may do to the object one of its pointer parameters designates, following
    /// the calls it makes. Offsets are relative to where the parameter points.
    struct ParameterFootprint
    {
        struct Range
        {
            std::int64_t begin = 0;
            std::int64_t end = 0;
            bool isAtomic = false;
        };

        /// Bytes read and written, apart. A flag is set when that may be anywhere in the
        /// object: through a pointer loaded from it, through an access the access path cannot
        /// place, or once the parameter has escaped.
        struct Extent
        {
            std::vector<Range> reads;
            std::vector<Range> writes;
            bool readsWholeObject = false;
            bool writesWholeObject = false;

            [[nodiscard]] bool readsAnything() const noexcept;
            [[nodiscard]] bool writesAnything() const noexcept;
        };

        /// The locks the function takes, tries or releases through the parameter, which `all`
        /// leaves out: the runtime changes them under its own synchronization, so no data access
        /// shows them, but their bytes change. Each range is a lock whose type names a primitive,
        /// placed and sized by that type; `wholeObject` is the parameter itself handed as a lock,
        /// whatever object it points to.
        struct Locks
        {
            std::vector<Range> ranges;
            bool wholeObject = false;
        };

        /// Everything the function does through the parameter.
        Extent all;
        /// The part of `all` none of the function's own accesses shows at a call: what it
        /// reaches without the access path placing it, and the thread handles it starts, joins
        /// or detaches. Its own accesses reach every call, with the locks held around them.
        Extent unshown;
        Locks locks;
        /// Whether the function hands the object on to something that may write it beyond its own
        /// accesses, which `all` leaves out since synchronization is understood where it happens:
        /// a thread it starts on it, a synchronization or thread call other than as a thread handle
        /// it fills, a lock `locks` places or the attributes it starts a thread with, a function
        /// that may keep its address, or an escape.
        bool handedOn = false;
    };

    /// Footprints of the functions a module defines, computed on demand and kept for the module.
    class ParameterFootprints
    {
      public:
        ParameterFootprints(const ConcurrencySymbolClassifier& classifier,
                            const llvm::DataLayout& layout);

        /// Footprint of the definition `function` on its parameter `argumentIndex`. A parameter
        /// reached again while its own footprint is being computed may do anything.
        [[nodiscard]] ParameterFootprint of(const llvm::Function& function, unsigned argumentIndex);

      private:
        [[nodiscard]] ParameterFootprint compute(const llvm::Function& function,
                                                 unsigned argumentIndex);

        const ConcurrencySymbolClassifier& classifier_;
        const llvm::DataLayout& layout_;
        std::map<std::pair<const llvm::Function*, unsigned>, ParameterFootprint> cache_;
        std::set<std::pair<const llvm::Function*, unsigned>> inProgress_;
    };
} // namespace ctrace::concurrency::internal::analysis
