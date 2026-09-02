// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "facts.hpp"

#include <unordered_map>

namespace llvm
{
    class Module;
} // namespace llvm

namespace ctrace::concurrency::internal::analysis
{
    class ConcurrencySymbolClassifier;

    /// The object a thread entry receives, when the program proves that object is shared.
    ///
    /// A field reached through a parameter has no name: its address is decided at run time, so
    /// two accesses to it cannot be compared the way two accesses to a global can. What the
    /// spawn sites do give is the object itself — and when the same one is handed to two threads,
    /// or to one thread started in a loop, its storage becomes an identity both accesses can be
    /// judged against.
    ///
    /// Sharing must be shown, never assumed. An entry handed a different object each time is
    /// exactly the shape that made the earlier attempt at this report races between threads that
    /// had nothing in common.
    struct SharedObjectBinding
    {
        /// Storage the object lives in, as `canonicalStorageGroupId` names it.
        std::string objectId;
        /// Parameter of the entry the object arrives on.
        unsigned argumentIndex = 0;
    };

    /// Entry function id to the shared object it is handed. Absent when nothing is proven.
    using SharedObjectBindings = std::unordered_map<std::string, SharedObjectBinding>;

    class SharedObjectBindingCollector
    {
      public:
        explicit SharedObjectBindingCollector(const ConcurrencySymbolClassifier& classifier);

        [[nodiscard]] SharedObjectBindings collect(const llvm::Module& module) const;

      private:
        const ConcurrencySymbolClassifier& classifier_;
    };
} // namespace ctrace::concurrency::internal::analysis
