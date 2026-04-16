// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "facts.hpp"

#include <vector>

namespace llvm
{
    class CallBase;
    class Module;
} // namespace llvm

namespace ctrace::concurrency::internal::analysis
{
    class ConcurrencySymbolClassifier;

    struct DirectCallSite
    {
        const llvm::CallBase* call = nullptr;
        std::string callerFunctionId;
        std::string calleeFunctionId;
        SourceLocation loweredLocation;
        SourceLocation userLocation;
        bool insideLoop = false;
    };

    [[nodiscard]] std::vector<DirectCallSite>
    collectDirectCallSites(const llvm::Module& module,
                           const ConcurrencySymbolClassifier& classifier);
} // namespace ctrace::concurrency::internal::analysis
