// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "coretrace_concurrency_analysis.hpp"

#include <cstdint>
#include <optional>
#include <set>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace llvm
{
    class Function;
    class Instruction;
} // namespace llvm

namespace ctrace::concurrency::internal::analysis
{
    using ThreadEntrySet = std::unordered_set<std::string>;

    /// Synthetic task identifier for code executed by the initial thread of the program
    /// (`main` and everything it reaches outside of a spawned entry). Modelling it as a task
    /// keeps main-vs-worker conflicts inside the same may-happen-in-parallel relation as
    /// worker-vs-worker conflicts.
    [[nodiscard]] inline const std::string& rootTaskId()
    {
        static const std::string id = "<main-task>";
        return id;
    }

    [[nodiscard]] inline bool isRootTask(const std::string& entryId)
    {
        return entryId == rootTaskId();
    }

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

    /// Byte range touched inside a root object. Field sensitivity is expressed as an offset and a
    /// size rather than a chain of indices, because the lowering elides the index of the first
    /// element: `g[0]` and `g.first` reach the IR as a plain pointer to `g`.
    struct MemoryRegion
    {
        /// False when a variable index makes the offset unknown; such a region overlaps any other.
        bool hasKnownOffset = true;
        std::int64_t byteOffset = 0;
        /// Extent in bytes; zero means unknown, and therefore covers the whole object.
        std::uint64_t byteSize = 0;

        [[nodiscard]] bool mayOverlap(const MemoryRegion& other) const noexcept
        {
            if (!hasKnownOffset || !other.hasKnownOffset)
                return true;

            if (byteSize == 0 || other.byteSize == 0)
                return true;

            const std::int64_t lhsEnd = byteOffset + static_cast<std::int64_t>(byteSize);
            const std::int64_t rhsEnd =
                other.byteOffset + static_cast<std::int64_t>(other.byteSize);
            return byteOffset < rhsEnd && other.byteOffset < lhsEnd;
        }

        /// Composes a callee-relative region with the region its argument already points at.
        [[nodiscard]] MemoryRegion rebasedOn(const MemoryRegion& base) const noexcept
        {
            MemoryRegion composed = *this;
            composed.hasKnownOffset = hasKnownOffset && base.hasKnownOffset;
            composed.byteOffset = byteOffset + base.byteOffset;
            return composed;
        }

        [[nodiscard]] std::string suffix() const
        {
            if (!hasKnownOffset)
                return "[*]";

            return byteOffset == 0 ? std::string() : "+" + std::to_string(byteOffset);
        }
    };

    struct RootBinding
    {
        RootBindingKind kind = RootBindingKind::Global;
        std::string symbol;
        unsigned argumentIndex = 0;
        MemoryRegion region;

        [[nodiscard]] static RootBinding global(std::string globalSymbol, MemoryRegion region = {})
        {
            return RootBinding{
                .kind = RootBindingKind::Global,
                .symbol = std::move(globalSymbol),
                .region = region,
            };
        }

        [[nodiscard]] static RootBinding argument(unsigned index, MemoryRegion region = {})
        {
            return RootBinding{
                .kind = RootBindingKind::Argument,
                .argumentIndex = index,
                .region = region,
            };
        }
    };

    struct AccessFact
    {
        std::string symbol;
        /// Byte range touched inside `symbol`; two accesses only conflict when their ranges overlap.
        MemoryRegion region;
        std::string functionId;
        AccessKind kind = AccessKind::Read;
        AliasProvenance aliasProvenance = AliasProvenance::Direct;
        SourceLocation loweredLocation;
        SourceLocation userLocation;
        bool allowCallsiteProjection = false;
        /// True for atomic loads/stores and read-modify-write instructions: two atomic accesses to
        /// the same location never form a data race.
        bool isAtomic = false;
        /// True when the access executes on the initial thread rather than inside a spawned entry.
        bool inRootTask = false;
        /// Spawned entries already running when a root-task access executes.
        ThreadEntrySet liveEntries;
        std::set<std::string> heldLocks;
    };

    struct LockOrderFact
    {
        std::string functionId;
        std::string firstLockId;
        std::string secondLockId;
        SourceLocation location;
        /// Every lock held at the acquisition, including `firstLockId`; used to detect a common
        /// outer "gate" lock that serializes an otherwise inconsistent order.
        std::set<std::string> heldLocks;
        bool inRootTask = false;
        ThreadEntrySet liveEntries;
    };

    enum class ThreadHandleKind
    {
        PThread,
        StdThread,
    };

    enum class ThreadLifecycleAction
    {
        Create,
        Join,
        Detach,
        Move,
    };

    struct ThreadLifecycleFact
    {
        ThreadHandleKind handleKind = ThreadHandleKind::PThread;
        ThreadLifecycleAction action = ThreadLifecycleAction::Create;
        std::string handleGroupId;
        std::optional<std::string> sourceHandleGroupId;
        std::string functionId;
        SourceLocation location;
    };

    struct PendingAccess
    {
        const llvm::Function* function = nullptr;
        const llvm::Instruction* instruction = nullptr;
        RootBinding root;
        AccessFact fact;
    };

    /// Unordered pair of thread entries proven to never overlap, because one is joined before the
    /// other is spawned.
    using EntryPair = std::pair<std::string, std::string>;

    struct EntryPairHash
    {
        [[nodiscard]] std::size_t operator()(const EntryPair& pair) const noexcept
        {
            const std::size_t first = std::hash<std::string>{}(pair.first);
            const std::size_t second = std::hash<std::string>{}(pair.second);
            return first ^ (second + 0x9e3779b97f4a7c15ULL + (first << 6) + (first >> 2));
        }
    };

    [[nodiscard]] inline EntryPair makeEntryPair(std::string lhs, std::string rhs)
    {
        if (rhs < lhs)
            std::swap(lhs, rhs);
        return EntryPair{std::move(lhs), std::move(rhs)};
    }

    struct TUFacts
    {
        std::vector<SpawnFact> spawns;
        std::vector<AccessFact> accesses;
        std::vector<LockOrderFact> lockOrders;
        std::vector<ThreadLifecycleFact> threadLifecycles;
        std::unordered_map<std::string, EntryConcurrencyInfo> entryConcurrency;
        std::unordered_map<std::string, ThreadEntrySet> reachableThreadEntriesByFunction;
        /// Locks whose runtime type or attributes allow recursive acquisition.
        std::unordered_set<std::string> recursiveLockIds;
        /// Entry pairs separated by a join, hence never concurrent.
        std::unordered_set<EntryPair, EntryPairHash> sequencedEntryPairs;
    };
} // namespace ctrace::concurrency::internal::analysis
