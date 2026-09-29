// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "facts.hpp"

#include <unordered_set>
#include <vector>

namespace llvm
{
    class CallBase;
    class Module;
} // namespace llvm

namespace ctrace::concurrency::internal::analysis
{
    class ConcurrencySymbolClassifier;
    class LlvmFunctionAnalysisProvider;

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
                           const ConcurrencySymbolClassifier& classifier,
                           LlvmFunctionAnalysisProvider& analyses);

    /// The calls among `sites` whose callee reaches their caller again through direct calls:
    /// the calls that close a cycle, whether a function calls itself or several call each other.
    [[nodiscard]] std::unordered_set<const llvm::CallBase*>
    callsClosingCycles(const std::vector<DirectCallSite>& sites);
} // namespace ctrace::concurrency::internal::analysis
