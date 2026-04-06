// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "facts.hpp"

namespace llvm
{
    class Module;
} // namespace llvm

namespace ctrace::concurrency::internal::analysis
{
    class TUFactsBuilder
    {
      public:
        [[nodiscard]] TUFacts build(const llvm::Module& module) const;
    };
} // namespace ctrace::concurrency::internal::analysis
