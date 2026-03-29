// SPDX-License-Identifier: Apache-2.0
#include "temporary_bitcode_file.hpp"

#include <llvm/ADT/SmallString.h>
#include <llvm/Support/FileSystem.h>

#include <string>
#include <utility>

namespace ctrace::concurrency::internal
{
    std::optional<TemporaryBitcodeFile> TemporaryBitcodeFile::create(CompileError& error)
    {
        llvm::SmallString<256> tempPath;
        if (std::error_code ec =
                llvm::sys::fs::createTemporaryFile("coretrace-concurrency-ir", "bc", tempPath))
        {
            error.code = make_error_code(CompileErrc::TemporaryBitcodeFileCreationFailed);
            error.phase = CompilePhase::BuildCommand;
            error.message = ec.message();
            return std::nullopt;
        }

        return TemporaryBitcodeFile(std::filesystem::path(std::string(tempPath.str())));
    }

    TemporaryBitcodeFile::TemporaryBitcodeFile(TemporaryBitcodeFile&& other) noexcept
        : path_(std::exchange(other.path_, {}))
    {
    }

    TemporaryBitcodeFile& TemporaryBitcodeFile::operator=(TemporaryBitcodeFile&& other) noexcept
    {
        if (this == &other)
            return *this;

        cleanup();
        path_ = std::exchange(other.path_, {});
        return *this;
    }

    TemporaryBitcodeFile::~TemporaryBitcodeFile()
    {
        cleanup();
    }

    void TemporaryBitcodeFile::cleanup() noexcept
    {
        if (path_.empty())
            return;

        std::error_code ec;
        std::filesystem::remove(path_, ec);
        path_.clear();
    }
} // namespace ctrace::concurrency::internal
