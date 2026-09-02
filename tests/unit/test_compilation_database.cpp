// SPDX-License-Identifier: Apache-2.0
#include "internal/compilation_database.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <string_view>

namespace
{
    using ctrace::concurrency::internal::CompilationDatabase;
    using ctrace::concurrency::internal::CompileCommand;

    bool assertTrue(bool condition, const std::string& message)
    {
        if (condition)
            return true;

        std::cerr << "[FAIL] " << message << "\n";
        return false;
    }

    /// Writes a database next to a unique directory so parallel runs cannot collide.
    class TemporaryDatabase
    {
      public:
        explicit TemporaryDatabase(std::string_view contents)
        {
            root_ = std::filesystem::temp_directory_path() /
                    ("coretrace-cdb-" +
                     std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) +
                     "-" + std::to_string(counter_++));
            std::filesystem::create_directories(root_);
            path_ = root_ / "compile_commands.json";

            std::ofstream stream(path_);
            stream << contents;
        }

        ~TemporaryDatabase()
        {
            std::error_code ignored;
            std::filesystem::remove_all(root_, ignored);
        }

        TemporaryDatabase(const TemporaryDatabase&) = delete;
        TemporaryDatabase& operator=(const TemporaryDatabase&) = delete;

        [[nodiscard]] const std::filesystem::path& path() const noexcept
        {
            return path_;
        }

      private:
        static inline int counter_ = 0;
        std::filesystem::path root_;
        std::filesystem::path path_;
    };

    bool hasArgument(const CompileCommand& command, std::string_view argument)
    {
        return std::find(command.arguments.begin(), command.arguments.end(), argument) !=
               command.arguments.end();
    }

    /// CMake emits `command`, one shell-escaped string. Anything that cannot read that form is
    /// useless on the databases the project itself produces.
    bool testReadsCMakeCommandForm()
    {
        const TemporaryDatabase database(R"([
          {
            "directory": "/work/build",
            "file": "/work/src/worker.cpp",
            "command": "/usr/bin/c++ -I/work/include -DMODE=2 -std=gnu++20 -o CMakeFiles/x.o -c /work/src/worker.cpp"
          }
        ])");

        std::string error;
        const std::optional<CompilationDatabase> loaded =
            CompilationDatabase::loadFromFile(database.path(), error);
        if (!assertTrue(loaded.has_value(), "command form should load: " + error))
            return false;

        const std::vector<CompileCommand>& commands = loaded->commands();
        return assertTrue(commands.size() == 1, "one entry should be kept") &&
               assertTrue(commands.front().file == "/work/src/worker.cpp",
                          "the source path should be normalized and absolute") &&
               assertTrue(commands.front().directory == "/work/build",
                          "the working directory should be preserved") &&
               assertTrue(hasArgument(commands.front(), "-I/work/include"),
                          "include options carry meaning and must survive") &&
               assertTrue(hasArgument(commands.front(), "-DMODE=2"),
                          "macro definitions must survive") &&
               assertTrue(hasArgument(commands.front(), "-std=gnu++20"),
                          "the language standard must survive");
    }

    /// The compiler, the source and output selection are supplied by the analyzer itself;
    /// replaying them would fight it.
    bool testDropsCompilerSourceAndOutput()
    {
        const TemporaryDatabase database(R"([
          {
            "directory": "/work/build",
            "file": "/work/src/a.c",
            "command": "/usr/bin/cc -c /work/src/a.c -o obj/a.o -MD -MF obj/a.o.d -MT obj/a.o -I/work/inc"
          }
        ])");

        std::string error;
        const std::optional<CompilationDatabase> loaded =
            CompilationDatabase::loadFromFile(database.path(), error);
        if (!assertTrue(loaded.has_value(), "entry should load: " + error))
            return false;

        const CompileCommand& command = loaded->commands().front();
        return assertTrue(!hasArgument(command, "/usr/bin/cc"), "the compiler should be dropped") &&
               assertTrue(!hasArgument(command, "/work/src/a.c"), "the source should be dropped") &&
               assertTrue(!hasArgument(command, "-o"), "output selection should be dropped") &&
               assertTrue(!hasArgument(command, "obj/a.o"), "the output path should be dropped") &&
               assertTrue(!hasArgument(command, "-MF"), "dependency options should be dropped") &&
               assertTrue(!hasArgument(command, "obj/a.o.d"),
                          "the dependency file should be dropped") &&
               assertTrue(hasArgument(command, "-I/work/inc"), "include options should remain");
    }

    /// A header-only source compiled into several targets appears several times. Analyzing it
    /// once, through the entry that describes it best, keeps the richest declaration set.
    bool testKeepsRichestEntryPerSource()
    {
        const TemporaryDatabase database(R"([
          {
            "directory": "/work/build",
            "file": "/work/src/shared.cpp",
            "command": "/usr/bin/c++ -c /work/src/shared.cpp -o a.o"
          },
          {
            "directory": "/work/build",
            "file": "/work/src/shared.cpp",
            "command": "/usr/bin/c++ -I/work/include -DWITH_FEATURE -std=c++20 -c /work/src/shared.cpp -o b.o"
          }
        ])");

        std::string error;
        const std::optional<CompilationDatabase> loaded =
            CompilationDatabase::loadFromFile(database.path(), error);
        if (!assertTrue(loaded.has_value(), "entries should load: " + error))
            return false;

        return assertTrue(loaded->commands().size() == 1,
                          "a source compiled twice should be analyzed once") &&
               assertTrue(hasArgument(loaded->commands().front(), "-DWITH_FEATURE"),
                          "the entry carrying more semantic options should win");
    }

    /// Dependencies fetched by the build are compiled alongside the project but are not the code
    /// under analysis.
    bool testSeparatesDependenciesAndUnsupportedFiles()
    {
        const TemporaryDatabase database(R"([
          {
            "directory": "/work/build",
            "file": "/work/src/main.cpp",
            "command": "/usr/bin/c++ -c /work/src/main.cpp -o m.o"
          },
          {
            "directory": "/work/build",
            "file": "/work/build/_deps/lib-src/lib.cpp",
            "command": "/usr/bin/c++ -c /work/build/_deps/lib-src/lib.cpp -o l.o"
          },
          {
            "directory": "/work/build",
            "file": "/work/src/shader.metal",
            "command": "/usr/bin/cc -c /work/src/shader.metal -o s.o"
          }
        ])");

        std::string error;
        const std::optional<CompilationDatabase> loaded =
            CompilationDatabase::loadFromFile(database.path(), error);
        if (!assertTrue(loaded.has_value(), "entries should load: " + error))
            return false;

        const std::vector<CompileCommand> own = loaded->analyzableSources(false);
        const std::vector<CompileCommand> all = loaded->analyzableSources(true);

        return assertTrue(own.size() == 1, "only the project source should be analyzable") &&
               assertTrue(own.front().file == "/work/src/main.cpp",
                          "the project source should be the one kept") &&
               assertTrue(all.size() == 2,
                          "dependencies should be reachable when explicitly requested") &&
               assertTrue(loaded->find("/work/src/main.cpp") != nullptr,
                          "a known source should be found by path") &&
               assertTrue(loaded->find("/work/src/absent.cpp") == nullptr,
                          "an unknown source should not be found");
    }

    /// A database the analyzer cannot trust must say so rather than analyze a subset silently.
    bool testRejectsMalformedDatabases()
    {
        std::string error;

        const TemporaryDatabase notAnArray(R"({"file": "/work/a.c"})");
        const bool objectRejected =
            !CompilationDatabase::loadFromFile(notAnArray.path(), error).has_value();

        const TemporaryDatabase missingFile(R"([{"directory": "/work", "command": "cc -c a.c"}])");
        const bool missingFileRejected =
            !CompilationDatabase::loadFromFile(missingFile.path(), error).has_value();

        const TemporaryDatabase noCommand(R"([{"directory": "/work", "file": "/work/a.c"}])");
        const bool noCommandRejected =
            !CompilationDatabase::loadFromFile(noCommand.path(), error).has_value();

        const bool absentRejected =
            !CompilationDatabase::loadFromFile("/nonexistent/compile_commands.json", error)
                 .has_value();

        return assertTrue(objectRejected, "a non-array database should be rejected") &&
               assertTrue(missingFileRejected, "an entry without 'file' should be rejected") &&
               assertTrue(noCommandRejected,
                          "an entry without 'arguments' or 'command' should be rejected") &&
               assertTrue(absentRejected, "a missing database should be rejected");
    }

    /// The order decides the order diagnostics come out in, so it must not depend on the file.
    bool testOrderIsDeterministic()
    {
        const TemporaryDatabase database(R"([
          {"directory": "/work", "file": "/work/z.c", "command": "cc -c /work/z.c"},
          {"directory": "/work", "file": "/work/a.c", "command": "cc -c /work/a.c"},
          {"directory": "/work", "file": "/work/m.c", "command": "cc -c /work/m.c"}
        ])");

        std::string error;
        const std::optional<CompilationDatabase> loaded =
            CompilationDatabase::loadFromFile(database.path(), error);
        if (!assertTrue(loaded.has_value(), "entries should load: " + error))
            return false;

        const std::vector<CompileCommand> sources = loaded->analyzableSources(false);
        return assertTrue(sources.size() == 3, "every source should be listed") &&
               assertTrue(std::is_sorted(sources.begin(), sources.end(),
                                         [](const CompileCommand& lhs, const CompileCommand& rhs)
                                         { return lhs.file < rhs.file; }),
                          "sources should come out in a stable order");
    }
} // namespace

int main()
{
    bool ok = true;

    ok = testReadsCMakeCommandForm() && ok;
    ok = testDropsCompilerSourceAndOutput() && ok;
    ok = testKeepsRichestEntryPerSource() && ok;
    ok = testSeparatesDependenciesAndUnsupportedFiles() && ok;
    ok = testRejectsMalformedDatabases() && ok;
    ok = testOrderIsDeterministic() && ok;

    if (!ok)
        return 1;

    std::cout << "[PASS] compilation database tests\n";
    return 0;
}
