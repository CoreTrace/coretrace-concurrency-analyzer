// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "coretrace_concurrency_analysis.hpp"

#include <set>
#include <string>
#include <unordered_map>
#include <utility>
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

    enum class RootBindingKind
    {
        Global,
        Argument,
    };

    struct RootBinding
    {
        RootBindingKind kind = RootBindingKind::Global;
        std::string symbol;
        unsigned argumentIndex = 0;

        [[nodiscard]] static RootBinding global(std::string globalSymbol)
        {
            return RootBinding{
                .kind = RootBindingKind::Global,
                .symbol = std::move(globalSymbol),
            };
        }

        [[nodiscard]] static RootBinding argument(unsigned index)
        {
            return RootBinding{
                .kind = RootBindingKind::Argument,
                .argumentIndex = index,
            };
        }
    };

    struct AccessFact
    {
        std::string symbol;
        std::string functionId;
        AccessKind kind = AccessKind::Read;
        SourceLocation loweredLocation;
        SourceLocation userLocation;
        bool allowCallsiteProjection = false;
        std::set<std::string> heldLocks;
    };

    struct PendingAccess
    {
        const llvm::Function* function = nullptr;
        const llvm::Instruction* instruction = nullptr;
        RootBinding root;
        AccessFact fact;
    };

    struct TUFacts
    {
        std::vector<SpawnFact> spawns;
        std::vector<AccessFact> accesses;
        std::unordered_map<std::string, EntryConcurrencyInfo> entryConcurrency;
    };
} // namespace ctrace::concurrency::internal::analysis
