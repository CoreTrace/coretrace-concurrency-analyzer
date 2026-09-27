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

        std::vector<Range> reads;
        std::vector<Range> writes;
        /// Set when the function may read, or write, anywhere in the object: through a pointer
        /// it loads from the object, through an access it cannot place, or once the parameter
        /// has escaped.
        bool readsWholeObject = false;
        bool writesWholeObject = false;

        [[nodiscard]] bool empty() const noexcept;
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
