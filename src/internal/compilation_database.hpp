// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace ctrace::concurrency::internal
{
    /// One entry of a JSON compilation database: the source, where the compiler ran, and the
    /// arguments to replay. Arguments are already stripped of what would fight the analyzer:
    /// the compiler name, the source itself, and output selection.
    struct CompileCommand
    {
        std::filesystem::path file;
        std::filesystem::path directory;
        std::vector<std::string> arguments;
    };

    /// Reads `compile_commands.json` as emitted by CMake.
    ///
    /// Only what the analyzer replays is kept. A file may legitimately appear several times, once
    /// per target it is compiled into; the entry carrying the most semantic options wins, so the
    /// richest include and macro set is the one analyzed.
    class CompilationDatabase
    {
      public:
        [[nodiscard]] static std::optional<CompilationDatabase>
        loadFromFile(const std::filesystem::path& path, std::string& error);

        [[nodiscard]] const std::vector<CompileCommand>& commands() const noexcept
        {
            return commands_;
        }

        /// Sources the analyzer can handle, in a deterministic order. Entries under a `_deps`
        /// directory are dependencies fetched by the build, not code under analysis.
        [[nodiscard]] std::vector<CompileCommand> analyzableSources(bool includeDependencies) const;

        [[nodiscard]] const CompileCommand* find(const std::filesystem::path& file) const;

      private:
        std::vector<CompileCommand> commands_;
    };
} // namespace ctrace::concurrency::internal
