// SPDX-License-Identifier: Apache-2.0
#include "compilation_database.hpp"

#include <llvm/ADT/SmallVector.h>
#include <llvm/Support/Allocator.h>
#include <llvm/Support/CommandLine.h>
#include <llvm/Support/JSON.h>
#include <llvm/Support/MemoryBuffer.h>
#include <llvm/Support/StringSaver.h>

#include <algorithm>
#include <cctype>
#include <array>
#include <map>
#include <string_view>

namespace ctrace::concurrency::internal
{
    namespace
    {
        /// Options that carry meaning for the analysis: they change which declarations the
        /// frontend sees, so an entry holding more of them describes the source better.
        constexpr std::array kSemanticOptionPrefixes = {
            std::string_view{"-I"},    std::string_view{"-isystem"},  std::string_view{"-D"},
            std::string_view{"-std="}, std::string_view{"--sysroot"}, std::string_view{"-f"},
            std::string_view{"-m"},    std::string_view{"-include"},
        };

        /// Options the analyzer supplies itself, or that would redirect its output.
        bool isOutputOrDependencyOption(std::string_view argument)
        {
            static constexpr std::array dropped = {
                std::string_view{"-o"},   std::string_view{"-c"},  std::string_view{"-MD"},
                std::string_view{"-MMD"}, std::string_view{"-MF"}, std::string_view{"-MT"},
                std::string_view{"-MQ"},  std::string_view{"-MP"},
            };

            return std::find(dropped.begin(), dropped.end(), argument) != dropped.end();
        }

        bool takesSeparateValue(std::string_view argument)
        {
            static constexpr std::array withValue = {
                std::string_view{"-o"},
                std::string_view{"-MF"},
                std::string_view{"-MT"},
                std::string_view{"-MQ"},
            };

            return std::find(withValue.begin(), withValue.end(), argument) != withValue.end();
        }

        std::size_t countSemanticOptions(const std::vector<std::string>& arguments)
        {
            return static_cast<std::size_t>(std::count_if(
                arguments.begin(), arguments.end(),
                [](const std::string& argument)
                {
                    return std::any_of(kSemanticOptionPrefixes.begin(),
                                       kSemanticOptionPrefixes.end(), [&](std::string_view prefix)
                                       { return argument.starts_with(prefix); });
                }));
        }

        std::filesystem::path absolutePath(const std::filesystem::path& file,
                                           const std::filesystem::path& directory)
        {
            std::filesystem::path resolved = file.is_absolute() ? file : directory / file;
            return resolved.lexically_normal();
        }

        /// True for an option that asks the compiler to optimize.
        ///
        /// The analysis reasons about the program as written: which accesses exist, which locks
        /// are held around them, which wait rechecks its condition. An optimizer is free to
        /// merge, inline and delete exactly those, and at -O3 a lock helper disappears into its
        /// caller and a `wait_for` collapses into an internal primitive. Replaying the build's
        /// optimization level would therefore report on a program the user never wrote.
        bool isOptimizationOption(std::string_view argument)
        {
            if (argument == "-flto" || argument.starts_with("-flto="))
                return true;

            if (!argument.starts_with("-O"))
                return false;

            // `-O`, `-O0`..`-O3`, `-Os`, `-Oz`, `-Og`, `-Ofast`. Anything longer is another
            // option that merely starts the same way, and must be kept.
            const std::string_view level = argument.substr(2);
            return level.empty() || level == "fast" ||
                   (level.size() == 1 && (std::isdigit(static_cast<unsigned char>(level[0])) != 0 ||
                                          level[0] == 's' || level[0] == 'z' || level[0] == 'g'));
        }

        /// Drops the compiler, the source and output selection, keeping what describes how the
        /// source is interpreted.
        std::vector<std::string> replayableArguments(const std::vector<std::string>& raw,
                                                     const std::filesystem::path& file,
                                                     const std::filesystem::path& directory)
        {
            std::vector<std::string> kept;
            kept.reserve(raw.size());

            for (std::size_t index = raw.empty() ? 0 : 1; index < raw.size(); ++index)
            {
                const std::string& argument = raw[index];

                if (isOutputOrDependencyOption(argument))
                {
                    if (takesSeparateValue(argument))
                        ++index;
                    continue;
                }

                if (argument.starts_with("-o") && argument.size() > 2)
                    continue;

                if (isOptimizationOption(argument))
                    continue;

                if (absolutePath(argument, directory) == file)
                    continue;

                kept.push_back(argument);
            }

            return kept;
        }

        std::optional<std::vector<std::string>> readArguments(const llvm::json::Object& entry,
                                                              std::string& error)
        {
            if (const llvm::json::Array* arguments = entry.getArray("arguments"))
            {
                std::vector<std::string> parsed;
                parsed.reserve(arguments->size());
                for (const llvm::json::Value& argument : *arguments)
                {
                    const std::optional<llvm::StringRef> text = argument.getAsString();
                    if (!text.has_value())
                    {
                        error = "compilation database: 'arguments' must hold strings";
                        return std::nullopt;
                    }
                    parsed.emplace_back(text->str());
                }
                return parsed;
            }

            // CMake emits `command`: the whole invocation as one shell-escaped string. It is
            // split with the tokenizer LLVM's own tooling uses, so quoting and escapes are
            // handled the same way the compiler saw them.
            if (const std::optional<llvm::StringRef> command = entry.getString("command"))
            {
                llvm::BumpPtrAllocator allocator;
                llvm::StringSaver saver(allocator);
                llvm::SmallVector<const char*, 32> tokens;
                llvm::cl::TokenizeGNUCommandLine(*command, saver, tokens);

                std::vector<std::string> parsed;
                parsed.reserve(tokens.size());
                for (const char* token : tokens)
                    parsed.emplace_back(token);
                return parsed;
            }

            error = "compilation database: entry has neither 'arguments' nor 'command'";
            return std::nullopt;
        }
    } // namespace

    std::optional<CompilationDatabase>
    CompilationDatabase::loadFromFile(const std::filesystem::path& path, std::string& error)
    {
        const llvm::ErrorOr<std::unique_ptr<llvm::MemoryBuffer>> buffer =
            llvm::MemoryBuffer::getFile(path.string());
        if (!buffer)
        {
            error = "compilation database: cannot read " + path.string();
            return std::nullopt;
        }

        llvm::Expected<llvm::json::Value> parsed = llvm::json::parse((*buffer)->getBuffer());
        if (!parsed)
        {
            error = "compilation database: " + llvm::toString(parsed.takeError());
            return std::nullopt;
        }

        const llvm::json::Array* entries = parsed->getAsArray();
        if (entries == nullptr)
        {
            error = "compilation database: top level must be an array";
            return std::nullopt;
        }

        // Keyed by source so a file compiled into several targets is analyzed once, with the
        // entry that describes it most completely.
        std::map<std::filesystem::path, CompileCommand> best;

        for (const llvm::json::Value& value : *entries)
        {
            const llvm::json::Object* entry = value.getAsObject();
            if (entry == nullptr)
            {
                error = "compilation database: entries must be objects";
                return std::nullopt;
            }

            const std::optional<llvm::StringRef> file = entry->getString("file");
            const std::optional<llvm::StringRef> directory = entry->getString("directory");
            if (!file.has_value() || !directory.has_value())
            {
                error = "compilation database: entry is missing 'file' or 'directory'";
                return std::nullopt;
            }

            const std::optional<std::vector<std::string>> raw = readArguments(*entry, error);
            if (!raw.has_value())
                return std::nullopt;

            CompileCommand command;
            command.directory = std::filesystem::path(directory->str()).lexically_normal();
            command.file = absolutePath(std::filesystem::path(file->str()), command.directory);
            command.arguments = replayableArguments(*raw, command.file, command.directory);

            const auto existing = best.find(command.file);
            if (existing == best.end() || countSemanticOptions(command.arguments) >
                                              countSemanticOptions(existing->second.arguments))
            {
                best[command.file] = std::move(command);
            }
        }

        CompilationDatabase database;
        database.commands_.reserve(best.size());
        for (auto& [file, command] : best)
            database.commands_.push_back(std::move(command));

        return database;
    }

    std::vector<CompileCommand>
    CompilationDatabase::analyzableSources(bool includeDependencies) const
    {
        static constexpr std::array supported = {
            std::string_view{".c"},   std::string_view{".C"},   std::string_view{".cc"},
            std::string_view{".cpp"}, std::string_view{".cxx"}, std::string_view{".c++"},
            std::string_view{".cp"},
        };

        std::vector<CompileCommand> sources;
        for (const CompileCommand& command : commands_)
        {
            const std::string extension = command.file.extension().string();
            if (std::find(supported.begin(), supported.end(), extension) == supported.end())
                continue;

            if (!includeDependencies)
            {
                const std::string text = command.file.generic_string();
                if (text.find("/_deps/") != std::string::npos)
                    continue;
            }

            sources.push_back(command);
        }

        return sources;
    }

    const CompileCommand* CompilationDatabase::find(const std::filesystem::path& file) const
    {
        const std::filesystem::path target = std::filesystem::path(file).lexically_normal();
        const auto it =
            std::find_if(commands_.begin(), commands_.end(),
                         [&](const CompileCommand& command) { return command.file == target; });
        return it == commands_.end() ? nullptr : &*it;
    }
} // namespace ctrace::concurrency::internal
