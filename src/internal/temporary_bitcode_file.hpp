#pragma once

#include "coretrace_concurrency_error.hpp"

#include <filesystem>
#include <optional>
#include <utility>

namespace ctrace::concurrency::internal
{
    class TemporaryBitcodeFile
    {
      public:
        [[nodiscard]] static std::optional<TemporaryBitcodeFile> create(CompileError& error);

        TemporaryBitcodeFile(TemporaryBitcodeFile&& other) noexcept;
        TemporaryBitcodeFile& operator=(TemporaryBitcodeFile&& other) noexcept;

        ~TemporaryBitcodeFile();

        TemporaryBitcodeFile(const TemporaryBitcodeFile&) = delete;
        TemporaryBitcodeFile& operator=(const TemporaryBitcodeFile&) = delete;

        [[nodiscard]] const std::filesystem::path& path() const noexcept
        {
            return path_;
        }

      private:
        explicit TemporaryBitcodeFile(std::filesystem::path path) : path_(std::move(path)) {}
        void cleanup() noexcept;

        std::filesystem::path path_;
    };
} // namespace ctrace::concurrency::internal
