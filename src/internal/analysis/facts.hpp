// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "coretrace_concurrency_analysis.hpp"

#include <set>
#include <string>
#include <unordered_map>
#include <vector>

namespace llvm
{
    class Function;
    class Instruction;
} // namespace llvm

namespace ctrace::concurrency::internal::analysis
{
    struct EntryConcurrencyInfo
    {
        std::size_t staticSpawnCount = 0;
        bool hasSpawnInLoop = false;

        [[nodiscard]] bool isSelfConcurrent() const noexcept
        {
            return staticSpawnCount >= 2 || hasSpawnInLoop;
        }
    };

    struct SpawnFact
    {
        std::string entryFunctionId;
        SourceLocation location;
        bool insideLoop = false;
    };

    struct AccessFact
    {
        std::string symbol;
        std::string functionId;
        AccessKind kind = AccessKind::Read;
        SourceLocation location;
        std::set<std::string> heldLocks;
    };

    struct PendingAccess
    {
        const llvm::Function* function = nullptr;
        const llvm::Instruction* instruction = nullptr;
        AccessFact fact;
    };

    struct TUFacts
    {
        std::vector<SpawnFact> spawns;
        std::vector<AccessFact> accesses;
        std::unordered_map<std::string, EntryConcurrencyInfo> entryConcurrency;
    };
} // namespace ctrace::concurrency::internal::analysis
