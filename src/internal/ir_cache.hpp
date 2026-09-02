// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "compilation_database.hpp"

#include <filesystem>
#include <optional>
#include <string>

namespace ctrace::concurrency::internal
{
    /// Reuses bitcode across runs instead of compiling a unit that has not changed.
    ///
    /// Compiling is about half the cost of analysing a project, and almost none of it is useful
    /// work on a second run. What makes reuse safe is knowing exactly what the bitcode depends
    /// on, so an entry is keyed on the source, on every header the compiler actually opened, and
    /// on the flags: any of them changing is a miss. The dependency list comes from the compiler
    /// itself rather than from a guess about which headers matter.
    class IRCache
    {
      public:
        /// Opens, creating the directory if needed. Returns nothing when the directory cannot be
        /// used, which is a reason to compile normally rather than to fail.
        [[nodiscard]] static std::optional<IRCache> open(const std::filesystem::path& directory);

        /// Bitcode stored for this command, if every input still hashes to what it did then.
        [[nodiscard]] std::optional<std::string> lookup(const CompileCommand& command) const;

        /// Records bitcode alongside the dependency list the compiler wrote to `depfile`.
        /// Failure to store is silent: a cache that cannot be written is a slow run, not a wrong
        /// one.
        void store(const CompileCommand& command, const std::string& bitcode,
                   const std::filesystem::path& depfile) const;

        /// Where the compiler should write the dependency list for this command.
        [[nodiscard]] std::filesystem::path depfilePathFor(const CompileCommand& command) const;

      private:
        explicit IRCache(std::filesystem::path directory) : directory_(std::move(directory)) {}

        [[nodiscard]] std::string keyFor(const CompileCommand& command) const;

        std::filesystem::path directory_;
    };

    /// Parses the Makefile fragment a compiler writes for `-MD`, returning the prerequisites.
    /// Exposed for testing: the escaping rules are the part worth pinning.
    [[nodiscard]] std::vector<std::string> parseDependencyFile(std::string_view contents);
} // namespace ctrace::concurrency::internal
