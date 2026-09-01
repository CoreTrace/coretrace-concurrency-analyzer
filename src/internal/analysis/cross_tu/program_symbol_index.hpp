// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "internal/analysis/ir_utils.hpp"
#include "internal/analysis/lock_wrapper_summaries.hpp"

#include <string>
#include <unordered_map>
#include <unordered_set>

namespace llvm
{
    class Function;
    class GlobalValue;
    class GlobalVariable;
    class Module;
} // namespace llvm

namespace ctrace::concurrency::internal::analysis
{
    struct TUFacts;

    /// Cross-translation-unit identity of a global value.
    ///
    /// `GlobalValue::getGlobalIdentifier` qualifies local-linkage symbols with the module that
    /// defines them, so two `static int counter` in different files stay distinct. Joining them
    /// by plain name would merge unrelated objects, which for a race detector invents a conflict
    /// between threads that share nothing.
    [[nodiscard]] std::string programSymbol(const llvm::GlobalValue& value);

    /// What one translation unit contributes to the whole-program view, and what it needs back.
    ///
    /// A translation unit sees only part of the program: it spawns a thread whose entry is
    /// defined elsewhere, or reads a global declared `extern` here and defined there. Both are
    /// dropped by the single-unit analysis, which is right on its own and wrong for a project.
    /// This index carries just enough across the boundary to repair those two blind spots.
    class ProgramSymbolIndex
    {
      public:
        /// Records what `module` defines and what it spawns, reading the spawn counts from the
        /// facts its own single-unit analysis produced. Safe to call per module in any order;
        /// the result does not depend on it.
        void addModule(const llvm::Module& module, const TUFacts& facts);

        /// True when some translation unit passes this function to a thread creation API, even
        /// if the spawn site is in another unit.
        [[nodiscard]] bool isThreadEntry(const llvm::Function& function) const;

        /// Instances that can run at once program-wide, and whether any spawn sits in a loop.
        /// Two threads running the same entry race with each other, and that fact may only be
        /// visible from the unit holding `main`.
        [[nodiscard]] std::size_t spawnCount(const llvm::Function& function) const;
        [[nodiscard]] bool spawnedInLoop(const llvm::Function& function) const;

        /// True when a global declared here has a definition in another unit, so accesses to it
        /// describe real shared state rather than an unresolved symbol.
        [[nodiscard]] bool isDefinedSomewhere(const llvm::GlobalVariable& global) const;

        /// Lock effects of a helper defined in some other unit, or null when no unit
        /// summarises it.
        [[nodiscard]] const std::vector<ParameterLockEffect>*
        lockSummaryFor(const llvm::Function& function) const;

        /// True when some unit joins or detaches the thread handle held in this global.
        [[nodiscard]] bool resolvesHandleElsewhere(const llvm::GlobalVariable& handle) const;

        [[nodiscard]] const ProgramDefinedGlobals& definedGlobals() const noexcept
        {
            return definedGlobals_;
        }

        [[nodiscard]] bool empty() const noexcept
        {
            return spawnSitesByEntry_.empty() && definedGlobals_.empty() &&
                   lockSummariesBySymbol_.empty();
        }

      private:
        struct EntrySpawns
        {
            std::size_t instanceCount = 0;
            bool insideLoop = false;
        };

        std::unordered_map<std::string, EntrySpawns> spawnSitesByEntry_;
        ProgramDefinedGlobals definedGlobals_;
        std::unordered_map<std::string, std::vector<ParameterLockEffect>> lockSummariesBySymbol_;
        std::unordered_set<std::string> resolvedHandleSymbols_;
    };
} // namespace ctrace::concurrency::internal::analysis
