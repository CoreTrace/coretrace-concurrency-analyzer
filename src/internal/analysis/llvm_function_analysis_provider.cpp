// SPDX-License-Identifier: Apache-2.0
#include "llvm_function_analysis_provider.hpp"

#include <llvm/Analysis/AliasAnalysis.h>
#include <llvm/IR/PassManager.h>
#include <llvm/Passes/PassBuilder.h>

#include <memory>

namespace ctrace::concurrency::internal::analysis
{
    struct LlvmFunctionAnalysisProvider::Impl
    {
        llvm::LoopAnalysisManager loopAnalysisManager;
        llvm::FunctionAnalysisManager functionAnalysisManager;
        llvm::CGSCCAnalysisManager cgsccAnalysisManager;
        llvm::ModuleAnalysisManager moduleAnalysisManager;
        llvm::PassBuilder passBuilder;

        Impl()
        {
            passBuilder.registerModuleAnalyses(moduleAnalysisManager);
            passBuilder.registerCGSCCAnalyses(cgsccAnalysisManager);
            passBuilder.registerFunctionAnalyses(functionAnalysisManager);
            passBuilder.registerLoopAnalyses(loopAnalysisManager);
            passBuilder.crossRegisterProxies(loopAnalysisManager, functionAnalysisManager,
                                             cgsccAnalysisManager, moduleAnalysisManager);
        }
    };

    LlvmFunctionAnalysisProvider::LlvmFunctionAnalysisProvider()
        : impl_(std::make_unique<Impl>())
    {
    }

    LlvmFunctionAnalysisProvider::~LlvmFunctionAnalysisProvider() = default;

    llvm::AAResults&
    LlvmFunctionAnalysisProvider::getAAResults(const llvm::Function& function)
    {
        return impl_->functionAnalysisManager.getResult<llvm::AAManager>(
            const_cast<llvm::Function&>(function));
    }
} // namespace ctrace::concurrency::internal::analysis
