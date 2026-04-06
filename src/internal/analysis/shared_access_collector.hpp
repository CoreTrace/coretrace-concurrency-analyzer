// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "facts.hpp"

#include <vector>

namespace llvm
{
    class Module;
} // namespace llvm

namespace ctrace::concurrency::internal::analysis
{
    class SharedAccessCollector
    {
      public:
        [[nodiscard]] std::vector<PendingAccess> collect(const llvm::Module& module) const;
    };
} // namespace ctrace::concurrency::internal::analysis
