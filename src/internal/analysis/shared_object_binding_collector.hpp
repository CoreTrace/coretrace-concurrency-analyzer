// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "facts.hpp"
#include "ir_utils.hpp"

#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace llvm
{
    class Module;
} // namespace llvm

namespace ctrace::concurrency::internal::analysis
{
    class ConcurrencySymbolClassifier;
    class LlvmFunctionAnalysisProvider;
    struct DirectCallSite;

    /// The object a thread entry receives, when the program proves that object is shared.
    ///
    /// A field reached through a parameter has no name: its address is decided at run time, so
    /// two accesses to it cannot be compared the way two accesses to a global can. What the
    /// spawn sites do give is the object itself — and when the same one is handed to two threads,
    /// or to one thread started in a loop, its storage becomes an identity both accesses can be
    /// judged against.
    ///
    /// Sharing must be shown, never assumed. An entry handed a different object each time is
    /// exactly the shape that made the earlier attempt at this report races between threads that
    /// had nothing in common.
    /// How the object the thread receives is named, which decides who else can meet it.
    enum class SharedObjectKind
    {
        /// A global, or part of one: its owner's accesses already use the global's name.
        Global,
        /// A local variable of the function that owns it, named after its storage.
        Local,
        /// Whatever a variable points at; the variable is all that names it.
        BehindPointer,
    };

    struct SharedObjectBinding
    {
        /// The object and where in it the spawn points, as a call binding would bind it: a
        /// global's name, or the storage `canonicalStorageGroupId` names.
        RootBinding object;
        SharedObjectKind kind = SharedObjectKind::BehindPointer;
        /// Parameter of the entry the object arrives on.
        unsigned argumentIndex = 0;
    };

    /// Entry function id to the shared object it is handed. Absent when nothing is proven.
    using SharedObjectBindings = std::unordered_map<std::string, SharedObjectBinding>;

    /// Names of the objects of `kind` the bindings hand to threads.
    [[nodiscard]] std::unordered_set<std::string>
    sharedObjectNames(const SharedObjectBindings& bindings, SharedObjectKind kind);

    class SharedObjectBindingCollector
    {
      public:
        explicit SharedObjectBindingCollector(const ConcurrencySymbolClassifier& classifier,
                                              LlvmFunctionAnalysisProvider& analyses);

        /// `directCallSites` lets an object known only as a parameter be traced back to the
        /// caller that owns it: an object whose constructor starts its own thread hands `this`
        /// to the spawn, and `this` names nothing until the construction site is known.
        [[nodiscard]] SharedObjectBindings
        collect(const llvm::Module& module, const std::vector<DirectCallSite>& directCallSites,
                const ProgramDefinedGlobals* programDefined = nullptr) const;

      private:
        const ConcurrencySymbolClassifier& classifier_;
        LlvmFunctionAnalysisProvider& analyses_;
    };
} // namespace ctrace::concurrency::internal::analysis
