// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "facts.hpp"

namespace llvm
{
    class Module;
} // namespace llvm

namespace ctrace::concurrency::internal::analysis
{
    class ConcurrencySymbolClassifier;

    struct ProcessLifecycleCollection
    {
        std::vector<ProcessForkFact> forks;
        /// Some unit of the program collects its terminated children, or hands that duty to the
        /// system. Either way the child processes do not accumulate.
        bool reapsChildren = false;
        /// Functions that replace the process image, directly or through a callee. A child that
        /// execs discards everything the fork gave it, which settles both questions this
        /// analysis asks about a fork.
        std::unordered_set<std::string> functionsReachingExec;
    };

    /// Finds where the program duplicates itself, and what it does about the copy.
    class ProcessLifecycleCollector
    {
      public:
        explicit ProcessLifecycleCollector(const ConcurrencySymbolClassifier& classifier);

        [[nodiscard]] ProcessLifecycleCollection collect(const llvm::Module& module) const;

      private:
        const ConcurrencySymbolClassifier& classifier_;
    };
} // namespace ctrace::concurrency::internal::analysis
