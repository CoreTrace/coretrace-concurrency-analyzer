// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "facts.hpp"

namespace llvm
{
    class Module;
} // namespace llvm

namespace ctrace::concurrency::internal::analysis
{
    class ProgramSymbolIndex;

    class TUFactsBuilder
    {
      public:
        /// `program` carries what the whole project knows and this module cannot see on its own:
        /// a thread entry spawned from another unit, a global declared here and defined there.
        /// Null for a single-unit run, where those questions have no answer.
        [[nodiscard]] TUFacts build(const llvm::Module& module,
                                    const ProgramSymbolIndex* program = nullptr) const;
    };
} // namespace ctrace::concurrency::internal::analysis
