#pragma once

#include "coretrace_concurrency_analyzer.hpp"

#include <filesystem>
#include <string>
#include <vector>

namespace ctrace::concurrency::internal
{
    class CompileCommandBuilder
    {
      public:
        [[nodiscard]] static std::vector<std::string> buildLL(const CompileRequest& request);

        [[nodiscard]] static std::vector<std::string>
        buildBC(const CompileRequest& request, const std::filesystem::path& outputPath);

      private:
        [[nodiscard]] static bool hasExactToken(const std::vector<std::string>& args,
                                                const std::string& token);
        static void appendIfMissing(std::vector<std::string>& args, const std::string& flag);
        static void removeOutputPathArgs(std::vector<std::string>& args);
    };
} // namespace ctrace::concurrency::internal
