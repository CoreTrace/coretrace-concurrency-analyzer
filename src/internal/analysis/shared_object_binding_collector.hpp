// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "facts.hpp"
#include "ir_utils.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace llvm
{
    class CallBase;
    class DataLayout;
    class DominatorTree;
    class Instruction;
    class Loop;
    class Module;
    class Value;
} // namespace llvm

namespace ctrace::concurrency::internal::analysis
{
    struct RoundJoin;

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

    /// A spawn that gives each of its threads an element of the object of its own.
    ///
    /// The spawn sits in a loop whose counter picks the element. The counter takes a new value
    /// every round, and the loop runs once in a function that runs once, so no two threads
    /// receive the same element. No other spawn hands out this object's elements.
    struct ElementPerThread
    {
        const llvm::CallBase* spawn = nullptr;
        const llvm::Loop* loop = nullptr;
        /// The variable holding the counter, and the store that advances it at the end of every
        /// round.
        const llvm::Value* counter = nullptr;
        const llvm::Instruction* increment = nullptr;
        /// The pointer the spawn hands over.
        VariablyIndexedPointer handed;
    };

    struct SharedObjectBinding
    {
        /// The object and where in it the spawn points, as a call binding would bind it: a
        /// global's name, or the storage `canonicalStorageGroupId` names.
        RootBinding object;
        SharedObjectKind kind = SharedObjectKind::BehindPointer;
        /// Parameter of the entry the object arrives on.
        unsigned argumentIndex = 0;
        /// Set when every thread of the entry receives an element of its own.
        std::optional<ElementPerThread> elementPerThread;
    };

    /// Whether `access`, made by the spawning function over `byteSize` bytes starting `offset`
    /// bytes past `pointer`, reaches the element the spawn of the current round is about to hand
    /// over, and does so before the spawn. No thread holds that element yet, and the threads
    /// already running hold the elements of earlier rounds.
    [[nodiscard]] bool reachesElementBeforeHandOver(const ElementPerThread& elements,
                                                    const llvm::Instruction& access,
                                                    const llvm::Value& pointer, std::int64_t offset,
                                                    std::uint64_t byteSize,
                                                    const llvm::DataLayout& layout);

    /// Whether `access`, made by the spawning function over `byteSize` bytes starting `offset`
    /// bytes past `pointer`, reaches the element of the thread the current round of `joins` has
    /// certainly joined: past that round's join success, at the join loop's counter read before
    /// its increment. That thread has ended, and the threads still running hold other elements.
    [[nodiscard]] bool reachesElementOfJoinedThread(
        const ElementPerThread& elements, const RoundJoin& joins, const llvm::Instruction& access,
        const llvm::Value& pointer, std::int64_t offset, std::uint64_t byteSize,
        const llvm::DataLayout& layout, const llvm::DominatorTree& dominators);

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
        /// `spawnSites` counts the spawn sites of each entry, including those reached through a
        /// helper that receives the entry: an element is only a thread's own when a single
        /// site hands them out.
        [[nodiscard]] SharedObjectBindings
        collect(const llvm::Module& module, const std::vector<DirectCallSite>& directCallSites,
                const ProgramDefinedGlobals* programDefined = nullptr,
                const std::unordered_map<std::string, EntryConcurrencyInfo>* spawnSites =
                    nullptr) const;

      private:
        const ConcurrencySymbolClassifier& classifier_;
        LlvmFunctionAnalysisProvider& analyses_;
    };
} // namespace ctrace::concurrency::internal::analysis
