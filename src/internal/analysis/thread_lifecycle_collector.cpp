// SPDX-License-Identifier: Apache-2.0
#include "thread_lifecycle_collector.hpp"

#include "concurrency_symbol_classifier.hpp"
#include "ir_utils.hpp"

#include <llvm/IR/DebugInfoMetadata.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/InstrTypes.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/Module.h>

#include <filesystem>
#include <unordered_set>

namespace ctrace::concurrency::internal::analysis
{
    namespace
    {
        struct LifecycleDescriptor
        {
            ThreadHandleKind handleKind = ThreadHandleKind::PThread;
            ThreadLifecycleAction action = ThreadLifecycleAction::Create;
            unsigned operandIndex = 0;
        };

        std::optional<LifecycleDescriptor> classifyLifecycle(CallKind kind)
        {
            switch (kind)
            {
            case CallKind::PThreadCreate:
                return LifecycleDescriptor{
                    .handleKind = ThreadHandleKind::PThread,
                    .action = ThreadLifecycleAction::Create,
                    .operandIndex = 0,
                };
            case CallKind::PThreadJoin:
                return LifecycleDescriptor{
                    .handleKind = ThreadHandleKind::PThread,
                    .action = ThreadLifecycleAction::Join,
                    .operandIndex = 0,
                };
            case CallKind::PThreadDetach:
                return LifecycleDescriptor{
                    .handleKind = ThreadHandleKind::PThread,
                    .action = ThreadLifecycleAction::Detach,
                    .operandIndex = 0,
                };
            case CallKind::StdThreadCtor:
                return LifecycleDescriptor{
                    .handleKind = ThreadHandleKind::StdThread,
                    .action = ThreadLifecycleAction::Create,
                    .operandIndex = 0,
                };
            case CallKind::StdThreadJoin:
                return LifecycleDescriptor{
                    .handleKind = ThreadHandleKind::StdThread,
                    .action = ThreadLifecycleAction::Join,
                    .operandIndex = 0,
                };
            case CallKind::StdThreadDetach:
                return LifecycleDescriptor{
                    .handleKind = ThreadHandleKind::StdThread,
                    .action = ThreadLifecycleAction::Detach,
                    .operandIndex = 0,
                };
            default:
                return std::nullopt;
            }
        }

        std::string lifecycleKey(const ThreadLifecycleFact& fact)
        {
            return std::to_string(static_cast<int>(fact.handleKind)) + "|" +
                   std::to_string(static_cast<int>(fact.action)) + "|" + fact.handleGroupId + "|" +
                   fact.functionId + "|" + fact.location.file + "|" +
                   std::to_string(fact.location.line) + "|" + std::to_string(fact.location.column);
        }

        std::optional<std::filesystem::path> primarySourceRoot(const llvm::Module& module)
        {
            for (llvm::DICompileUnit* compileUnit : module.debug_compile_units())
            {
                if (compileUnit == nullptr || compileUnit->getFile() == nullptr)
                    continue;

                std::filesystem::path filePath(compileUnit->getFile()->getFilename().str());
                const std::string directory = compileUnit->getFile()->getDirectory().str();
                if (!directory.empty() && filePath.is_relative())
                    filePath = std::filesystem::path(directory) / filePath;
                filePath = filePath.lexically_normal();
                if (!filePath.empty())
                    return filePath.parent_path();
            }

            return std::nullopt;
        }

        bool isLikelyUserLocation(const SourceLocation& location,
                                  const std::optional<std::filesystem::path>& sourceRoot)
        {
            if (location.file.empty() || !sourceRoot.has_value())
                return false;

            const std::filesystem::path filePath =
                std::filesystem::path(location.file).lexically_normal();
            const std::filesystem::path relativePath = filePath.lexically_relative(*sourceRoot);
            return !relativePath.empty() && *relativePath.begin() != "..";
        }

        bool isRuntimeOwnedLifecycle(const ThreadLifecycleFact& fact,
                                     const SourceLocation& userLocation,
                                     const std::optional<std::filesystem::path>& sourceRoot)
        {
            if (!fact.handleGroupId.starts_with("arg:"))
                return false;

            return !isLikelyUserLocation(userLocation, sourceRoot);
        }
    } // namespace

    ThreadLifecycleCollector::ThreadLifecycleCollector(
        const ConcurrencySymbolClassifier& classifier)
        : classifier_(classifier)
    {
    }

    std::vector<ThreadLifecycleFact>
    ThreadLifecycleCollector::collect(const llvm::Module& module) const
    {
        std::vector<ThreadLifecycleFact> facts;
        std::unordered_set<std::string> factKeys;
        const std::optional<std::filesystem::path> sourceRoot = primarySourceRoot(module);

        for (const llvm::Function& function : module)
        {
            if (function.isDeclaration())
                continue;

            for (const llvm::BasicBlock& block : function)
            {
                for (const llvm::Instruction& instruction : block)
                {
                    const auto* call = llvm::dyn_cast<llvm::CallBase>(&instruction);
                    if (call == nullptr)
                        continue;

                    const CallKind kind = classifier_.classify(*call);
                    const std::optional<LifecycleDescriptor> descriptor = classifyLifecycle(kind);
                    if (!descriptor.has_value() || descriptor->operandIndex >= call->arg_size())
                        continue;

                    const std::optional<std::string> handleGroupId =
                        canonicalStorageGroupId(*call->getArgOperand(descriptor->operandIndex));
                    if (!handleGroupId.has_value())
                        continue;

                    const ResolvedSourceLocations locations = resolveSourceLocations(instruction);
                    ThreadLifecycleFact fact{
                        .handleKind = descriptor->handleKind,
                        .action = descriptor->action,
                        .handleGroupId = *handleGroupId,
                        .functionId = functionId(function),
                        .location = locations.userLocation,
                    };

                    if (isRuntimeOwnedLifecycle(fact, locations.userLocation, sourceRoot))
                    {
                        continue;
                    }

                    const std::string key = lifecycleKey(fact);
                    if (!factKeys.insert(key).second)
                        continue;

                    facts.push_back(std::move(fact));
                }
            }
        }

        return facts;
    }
} // namespace ctrace::concurrency::internal::analysis
