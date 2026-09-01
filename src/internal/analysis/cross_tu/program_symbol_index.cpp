// SPDX-License-Identifier: Apache-2.0
#include "program_symbol_index.hpp"

#include "internal/analysis/facts.hpp"
#include "internal/analysis/ir_utils.hpp"

#include <llvm/IR/Function.h>
#include <llvm/IR/GlobalVariable.h>
#include <llvm/IR/Module.h>

namespace ctrace::concurrency::internal::analysis
{
    std::string programSymbol(const llvm::GlobalValue& value)
    {
        return value.getGlobalIdentifier();
    }

    void ProgramSymbolIndex::addModule(const llvm::Module& module, const TUFacts& facts)
    {
        for (const llvm::GlobalVariable& global : module.globals())
        {
            if (!global.isDeclaration())
                definedGlobals_.insert(programSymbol(global));
        }

        // `entryConcurrency` is already corrected by this module's own may-happen-in-parallel
        // analysis: two spawns on mutually exclusive branches count as one instance. Re-counting
        // spawn calls here would discard that verdict and report races the unit had ruled out.
        for (const auto& [entryFunctionId, concurrency] : facts.entryConcurrency)
        {
            const llvm::Function* entry = module.getFunction(entryFunctionId);
            if (entry == nullptr)
                continue;

            EntrySpawns& spawns = spawnSitesByEntry_[programSymbol(*entry)];
            // Instances created by different units have no dominance relation to sequence them,
            // so their counts add up.
            spawns.instanceCount += concurrency.staticSpawnCount;
            spawns.insideLoop = spawns.insideLoop || concurrency.hasSpawnInLoop;
        }
    }

    bool ProgramSymbolIndex::isThreadEntry(const llvm::Function& function) const
    {
        return spawnSitesByEntry_.contains(programSymbol(function));
    }

    std::size_t ProgramSymbolIndex::spawnCount(const llvm::Function& function) const
    {
        const auto it = spawnSitesByEntry_.find(programSymbol(function));
        return it == spawnSitesByEntry_.end() ? 0 : it->second.instanceCount;
    }

    bool ProgramSymbolIndex::spawnedInLoop(const llvm::Function& function) const
    {
        const auto it = spawnSitesByEntry_.find(programSymbol(function));
        return it != spawnSitesByEntry_.end() && it->second.insideLoop;
    }

    bool ProgramSymbolIndex::isDefinedSomewhere(const llvm::GlobalVariable& global) const
    {
        return definedGlobals_.contains(programSymbol(global));
    }
} // namespace ctrace::concurrency::internal::analysis
