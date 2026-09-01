// SPDX-License-Identifier: Apache-2.0
#include "ir_cache.hpp"

#include <llvm/ADT/ArrayRef.h>
#include <llvm/ADT/StringRef.h>
#include <llvm/Support/SHA256.h>

#include <array>
#include <fstream>
#include <system_error>
#include <utility>

namespace ctrace::concurrency::internal
{
    namespace
    {
        std::string toHex(const std::array<std::uint8_t, 32>& digest)
        {
            constexpr std::string_view kDigits = "0123456789abcdef";

            std::string hex;
            hex.reserve(digest.size() * 2);
            for (const std::uint8_t byte : digest)
            {
                hex.push_back(kDigits[byte >> 4]);
                hex.push_back(kDigits[byte & 0x0F]);
            }

            return hex;
        }

        std::optional<std::string> readFile(const std::filesystem::path& path)
        {
            std::ifstream in(path, std::ios::in | std::ios::binary);
            if (!in)
                return std::nullopt;

            return std::string(std::istreambuf_iterator<char>(in),
                               std::istreambuf_iterator<char>());
        }

        /// Writes through a temporary sibling so a reader never sees a half-written entry, and so
        /// two runs racing on the same key leave one intact file rather than a mixture.
        bool writeFileAtomically(const std::filesystem::path& path, std::string_view contents)
        {
            const std::filesystem::path temporary = path.string() + ".tmp";
            {
                std::ofstream out(temporary, std::ios::out | std::ios::binary | std::ios::trunc);
                if (!out)
                    return false;

                out.write(contents.data(), static_cast<std::streamsize>(contents.size()));
                if (!out)
                    return false;
            }

            std::error_code ec;
            std::filesystem::rename(temporary, path, ec);
            if (ec)
            {
                std::filesystem::remove(temporary, ec);
                return false;
            }

            return true;
        }
    } // namespace

    std::vector<std::string> parseDependencyFile(std::string_view contents)
    {
        // The fragment reads `target: prereq prereq ...`, with `\` before a newline continuing the
        // list and `\ ` standing for a space inside a path.
        const std::size_t colon = contents.find(':');
        if (colon == std::string_view::npos)
            return {};

        std::vector<std::string> prerequisites;
        std::string current;
        const auto flush = [&]
        {
            if (!current.empty())
            {
                prerequisites.push_back(current);
                current.clear();
            }
        };

        for (std::size_t index = colon + 1; index < contents.size(); ++index)
        {
            const char character = contents[index];
            if (character == '\\' && index + 1 < contents.size())
            {
                const char escaped = contents[index + 1];
                if (escaped == '\n')
                {
                    flush();
                    ++index;
                    continue;
                }

                if (escaped == ' ' || escaped == '\\' || escaped == '#')
                {
                    current.push_back(escaped);
                    ++index;
                    continue;
                }
            }

            if (character == ' ' || character == '\t' || character == '\n' || character == '\r')
            {
                flush();
                continue;
            }

            current.push_back(character);
        }

        flush();
        return prerequisites;
    }

    std::optional<IRCache> IRCache::open(const std::filesystem::path& directory)
    {
        std::error_code ec;
        std::filesystem::create_directories(directory, ec);
        if (ec && !std::filesystem::is_directory(directory))
            return std::nullopt;

        return IRCache(directory);
    }

    std::string IRCache::keyFor(const CompileCommand& command) const
    {
        // Separated by a byte that cannot appear in a path or a flag, so that a different
        // splitting of the same characters cannot produce the same key.
        std::string material = command.file;
        material.push_back('\0');
        for (const std::string& argument : command.arguments)
        {
            material += argument;
            material.push_back('\0');
        }

        llvm::SHA256 hash;
        hash.update(llvm::StringRef(material));

        return toHex(hash.final());
    }

    std::filesystem::path IRCache::depfilePathFor(const CompileCommand& command) const
    {
        return directory_ / (keyFor(command) + ".d");
    }

    std::optional<std::string> IRCache::lookup(const CompileCommand& command) const
    {
        const std::string key = keyFor(command);
        const std::optional<std::string> dependencyList = readFile(directory_ / (key + ".deps"));
        const std::optional<std::string> storedStamp = readFile(directory_ / (key + ".stamp"));
        if (!dependencyList.has_value() || !storedStamp.has_value())
            return std::nullopt;

        llvm::SHA256 stamp;
        std::size_t begin = 0;
        while (begin < dependencyList->size())
        {
            const std::size_t end = dependencyList->find('\n', begin);
            const std::string path =
                dependencyList->substr(begin, end == std::string::npos ? end : end - begin);
            begin = end == std::string::npos ? dependencyList->size() : end + 1;
            if (path.empty())
                continue;

            // A dependency that disappeared invalidates the entry: the unit it described no
            // longer exists as it was compiled.
            const std::optional<std::string> contents = readFile(path);
            if (!contents.has_value())
                return std::nullopt;

            stamp.update(llvm::StringRef(*contents));
        }

        if (toHex(stamp.final()) != *storedStamp)
            return std::nullopt;

        return readFile(directory_ / (key + ".bc"));
    }

    void IRCache::store(const CompileCommand& command, const std::string& bitcode,
                        const std::filesystem::path& depfile) const
    {
        const std::optional<std::string> fragment = readFile(depfile);
        if (!fragment.has_value())
            return;

        const std::vector<std::string> dependencies = parseDependencyFile(*fragment);
        if (dependencies.empty())
            return;

        llvm::SHA256 stamp;
        std::string dependencyList;
        for (const std::string& path : dependencies)
        {
            const std::optional<std::string> contents = readFile(path);
            if (!contents.has_value())
                return;

            stamp.update(llvm::StringRef(*contents));
            dependencyList += path;
            dependencyList += '\n';
        }

        const std::string key = keyFor(command);
        // The stamp is written last: an entry is only usable once its bitcode and dependency list
        // are both on disk.
        if (!writeFileAtomically(directory_ / (key + ".bc"), bitcode))
            return;

        if (!writeFileAtomically(directory_ / (key + ".deps"), dependencyList))
            return;

        writeFileAtomically(directory_ / (key + ".stamp"), toHex(stamp.final()));
    }
} // namespace ctrace::concurrency::internal
