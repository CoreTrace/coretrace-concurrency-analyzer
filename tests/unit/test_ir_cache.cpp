// SPDX-License-Identifier: Apache-2.0
#include "internal/ir_cache.hpp"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace
{
    using ctrace::concurrency::internal::CompileCommand;
    using ctrace::concurrency::internal::IRCache;
    using ctrace::concurrency::internal::parseDependencyFile;

    bool assertTrue(bool condition, const std::string& message)
    {
        if (condition)
            return true;

        std::cerr << "[FAIL] " << message << "\n";
        return false;
    }

    /// A scratch directory that removes itself, named from the clock so parallel runs of this
    /// binary never share one.
    class TemporaryDirectory
    {
      public:
        TemporaryDirectory()
        {
            const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
            path_ = std::filesystem::temp_directory_path() /
                    ("coretrace-ir-cache-test-" + std::to_string(stamp));
            std::error_code ec;
            std::filesystem::create_directories(path_, ec);
        }

        ~TemporaryDirectory()
        {
            std::error_code ec;
            std::filesystem::remove_all(path_, ec);
        }

        TemporaryDirectory(const TemporaryDirectory&) = delete;
        TemporaryDirectory& operator=(const TemporaryDirectory&) = delete;

        [[nodiscard]] const std::filesystem::path& path() const noexcept
        {
            return path_;
        }

        [[nodiscard]] std::filesystem::path write(std::string_view name,
                                                  std::string_view contents) const
        {
            const std::filesystem::path file = path_ / name;
            std::ofstream out(file, std::ios::out | std::ios::binary | std::ios::trunc);
            out.write(contents.data(), static_cast<std::streamsize>(contents.size()));
            return file;
        }

      private:
        std::filesystem::path path_;
    };

    bool testParsesPrerequisitesAcrossContinuedLines()
    {
        const std::vector<std::string> parsed =
            parseDependencyFile("out.bc: main.c \\\n  /usr/include/stdio.h \\\n  local.h\n");

        return assertTrue(parsed.size() == 3, "three prerequisites are listed") &&
               assertTrue(parsed.at(0) == "main.c", "the source itself is a prerequisite") &&
               assertTrue(parsed.at(2) == "local.h", "the last line is not dropped");
    }

    bool testParsesEscapedSpacesInsidePaths()
    {
        const std::vector<std::string> parsed =
            parseDependencyFile("out.bc: /a\\ path/with\\ spaces.h other.h\n");

        return assertTrue(parsed.size() == 2, "an escaped space does not split a path") &&
               assertTrue(parsed.at(0) == "/a path/with spaces.h", "the escape is removed");
    }

    /// The target is what is being built, not something it depends on.
    bool testTargetIsNotAPrerequisite()
    {
        const std::vector<std::string> parsed = parseDependencyFile("build/out.bc: main.c\n");

        return assertTrue(parsed.size() == 1, "only the prerequisite is returned") &&
               assertTrue(parsed.at(0) == "main.c", "the target is excluded");
    }

    bool testFragmentWithoutATargetYieldsNothing()
    {
        return assertTrue(parseDependencyFile("not a dependency file").empty(),
                          "a fragment with no target yields no prerequisite");
    }

    bool testStoredBitcodeIsReturnedWhenNothingChanged()
    {
        const TemporaryDirectory scratch;
        const std::filesystem::path header = scratch.write("header.h", "int shared;\n");
        const std::filesystem::path source = scratch.write("unit.c", "#include \"header.h\"\n");
        const std::filesystem::path depfile = scratch.write(
            "unit.d", "unit.bc: " + source.string() + " \\\n  " + header.string() + "\n");

        const std::optional<IRCache> cache = IRCache::open(scratch.path() / "cache");
        if (!assertTrue(cache.has_value(), "the cache directory can be created"))
            return false;

        const CompileCommand command{.file = source.string(), .arguments = {"-std=c17"}};
        cache->store(command, "BITCODE", depfile);

        const std::optional<std::string> found = cache->lookup(command);
        return assertTrue(found.has_value() && *found == "BITCODE",
                          "unchanged inputs return the stored bitcode");
    }

    /// The point of recording the compiler's own dependency list: a header the source never names
    /// directly still invalidates the entry.
    bool testChangedHeaderInvalidatesTheEntry()
    {
        const TemporaryDirectory scratch;
        const std::filesystem::path header = scratch.write("header.h", "int shared;\n");
        const std::filesystem::path source = scratch.write("unit.c", "#include \"header.h\"\n");
        const std::filesystem::path depfile = scratch.write(
            "unit.d", "unit.bc: " + source.string() + " \\\n  " + header.string() + "\n");

        const std::optional<IRCache> cache = IRCache::open(scratch.path() / "cache");
        if (!cache.has_value())
            return false;

        const CompileCommand command{.file = source.string(), .arguments = {}};
        cache->store(command, "BITCODE", depfile);

        std::ofstream(header, std::ios::out | std::ios::trunc) << "int shared; int added;\n";

        return assertTrue(!cache->lookup(command).has_value(),
                          "editing a recorded header must invalidate the entry");
    }

    bool testRemovedDependencyInvalidatesTheEntry()
    {
        const TemporaryDirectory scratch;
        const std::filesystem::path header = scratch.write("header.h", "int shared;\n");
        const std::filesystem::path source = scratch.write("unit.c", "#include \"header.h\"\n");
        const std::filesystem::path depfile = scratch.write(
            "unit.d", "unit.bc: " + source.string() + " \\\n  " + header.string() + "\n");

        const std::optional<IRCache> cache = IRCache::open(scratch.path() / "cache");
        if (!cache.has_value())
            return false;

        const CompileCommand command{.file = source.string(), .arguments = {}};
        cache->store(command, "BITCODE", depfile);

        std::error_code ec;
        std::filesystem::remove(header, ec);

        return assertTrue(!cache->lookup(command).has_value(),
                          "a dependency that no longer exists must invalidate the entry");
    }

    /// Flags change the bitcode, so they belong in the key.
    bool testDifferentFlagsDoNotShareAnEntry()
    {
        const TemporaryDirectory scratch;
        const std::filesystem::path source = scratch.write("unit.c", "int shared;\n");
        const std::filesystem::path depfile =
            scratch.write("unit.d", "unit.bc: " + source.string() + "\n");

        const std::optional<IRCache> cache = IRCache::open(scratch.path() / "cache");
        if (!cache.has_value())
            return false;

        const CompileCommand stored{.file = source.string(), .arguments = {"-DFEATURE=1"}};
        const CompileCommand other{.file = source.string(), .arguments = {"-DFEATURE=2"}};
        cache->store(stored, "BITCODE", depfile);

        return assertTrue(cache->lookup(stored).has_value(), "the stored command hits") &&
               assertTrue(!cache->lookup(other).has_value(),
                          "a command built with different flags must not reuse it");
    }

    bool testMissingEntryIsAMiss()
    {
        const TemporaryDirectory scratch;
        const std::optional<IRCache> cache = IRCache::open(scratch.path() / "cache");
        if (!cache.has_value())
            return false;

        const CompileCommand command{.file = "never-stored.c", .arguments = {}};
        return assertTrue(!cache->lookup(command).has_value(), "an unknown command misses");
    }
} // namespace

int main()
{
    bool ok = true;
    ok = testParsesPrerequisitesAcrossContinuedLines() && ok;
    ok = testParsesEscapedSpacesInsidePaths() && ok;
    ok = testTargetIsNotAPrerequisite() && ok;
    ok = testFragmentWithoutATargetYieldsNothing() && ok;
    ok = testStoredBitcodeIsReturnedWhenNothingChanged() && ok;
    ok = testChangedHeaderInvalidatesTheEntry() && ok;
    ok = testRemovedDependencyInvalidatesTheEntry() && ok;
    ok = testDifferentFlagsDoNotShareAnEntry() && ok;
    ok = testMissingEntryIsAMiss() && ok;

    if (!ok)
        return 1;

    std::cout << "[PASS] IR cache tests\n";
    return 0;
}
