// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "facts.hpp"
#include "ir_utils.hpp"

#include <vector>

namespace llvm
{
    class Module;
} // namespace llvm

namespace ctrace::concurrency::internal::analysis
{
    class SharedAccessCollector
    {
      public:
        /// `programDefined` names the globals the whole program defines, so an `extern`
        /// declared here and defined in another unit is followed instead of dropped. Null when
        /// only this unit is under analysis.
        /// `sharedObjectIds` names the storage slots the spawn sites proved hold an object more
        /// than one thread reaches. An access through such a slot is shared state as surely as a
        /// global is, and is followed on the same terms.
        [[nodiscard]] std::vector<PendingAccess>
        collect(const llvm::Module& module, const ProgramDefinedGlobals* programDefined = nullptr,
                const std::unordered_set<std::string>* sharedObjectIds = nullptr) const;
    };
} // namespace ctrace::concurrency::internal::analysis
