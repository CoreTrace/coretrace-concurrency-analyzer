// SPDX-License-Identifier: Apache-2.0
#include "program_symbol_index.hpp"

#include "internal/analysis/facts.hpp"

#include <llvm/IR/GlobalValue.h>

namespace ctrace::concurrency::internal::analysis
{
    std::string programSymbol(const llvm::GlobalValue& value)
    {
        return value.getGlobalIdentifier();
    }

    void ProgramSymbolIndex::addUnit(const TUFacts& facts)
    {
        const ProgramSymbolFacts& program = facts.program;
        definedGlobals_.insert(program.definedGlobals.begin(), program.definedGlobals.end());

        // `entryConcurrency` is already corrected by this unit's own may-happen-in-parallel
        // analysis: two spawns on mutually exclusive branches count as one instance. Re-counting
        // spawn calls here would discard that verdict and report races the unit had ruled out.
        for (const std::string& handleGroupId : facts.resolvedGlobalHandleIds)
        {
            if (const auto it = program.handleSymbolsByGroupId.find(handleGroupId);
                it != program.handleSymbolsByGroupId.end())
            {
                resolvedHandleSymbols_.insert(it->second);
            }
        }

        for (const auto& [helperFunctionId, parameterEffects] : facts.lockWrapperSummaries)
        {
            const auto it = program.functionSymbolsById.find(helperFunctionId);
            if (it != program.functionSymbolsById.end() &&
                !program.declaredFunctions.contains(it->second))
            {
                lockSummariesBySymbol_.emplace(it->second, parameterEffects);
            }
        }

        for (const auto& [entryFunctionId, concurrency] : facts.entryConcurrency)
        {
            const auto it = program.functionSymbolsById.find(entryFunctionId);
            if (it == program.functionSymbolsById.end())
                continue;

            EntrySpawns& spawns = spawnSitesByEntry_[it->second];
            // Instances created by different units have no dominance relation to sequence them,
            // so their counts add up.
            spawns.instanceCount += concurrency.staticSpawnCount;
            spawns.insideLoop = spawns.insideLoop || concurrency.hasSpawnInLoop;
        }
    }

    const std::vector<ParameterLockEffect>*
    ProgramSymbolIndex::lockSummaryFor(const std::string& symbol) const
    {
        const auto it = lockSummariesBySymbol_.find(symbol);
        return it == lockSummariesBySymbol_.end() ? nullptr : &it->second;
    }

    bool ProgramSymbolIndex::resolvesHandleElsewhere(const std::string& symbol) const
    {
        return resolvedHandleSymbols_.contains(symbol);
    }

    bool ProgramSymbolIndex::isThreadEntry(const std::string& symbol) const
    {
        return spawnSitesByEntry_.contains(symbol);
    }

    std::size_t ProgramSymbolIndex::spawnCount(const std::string& symbol) const
    {
        const auto it = spawnSitesByEntry_.find(symbol);
        return it == spawnSitesByEntry_.end() ? 0 : it->second.instanceCount;
    }

    bool ProgramSymbolIndex::spawnedInLoop(const std::string& symbol) const
    {
        const auto it = spawnSitesByEntry_.find(symbol);
        return it != spawnSitesByEntry_.end() && it->second.insideLoop;
    }

    bool ProgramSymbolIndex::isDefinedSomewhere(const std::string& symbol) const
    {
        return definedGlobals_.contains(symbol);
    }
} // namespace ctrace::concurrency::internal::analysis
