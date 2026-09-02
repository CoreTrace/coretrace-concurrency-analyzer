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
    struct DirectCallSite;

    /// Finds signal handlers that call what a handler may not call.
    ///
    /// A handler interrupts its own thread at an arbitrary instruction, so it may run while the
    /// interrupted code is halfway through a data structure. Allocating, printing or locking
    /// there re-enters that structure, and POSIX consequently limits a handler to a short list
    /// of async-signal-safe calls.
    class SignalHandlerCollector
    {
      public:
        explicit SignalHandlerCollector(const ConcurrencySymbolClassifier& classifier);

        [[nodiscard]] std::vector<SignalHandlerFact>
        collect(const llvm::Module& module,
                const std::vector<DirectCallSite>& directCallSites) const;

      private:
        const ConcurrencySymbolClassifier& classifier_;
    };
} // namespace ctrace::concurrency::internal::analysis
