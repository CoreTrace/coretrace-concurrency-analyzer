// SPDX-License-Identifier: Apache-2.0
#include "compile_command_builder.hpp"

#include <algorithm>
#include <utility>

namespace ctrace::concurrency::internal
{
    bool CompileCommandBuilder::hasExactToken(const std::vector<std::string>& args,
                                              const std::string& token)
    {
        return std::find(args.begin(), args.end(), token) != args.end();
    }

    void CompileCommandBuilder::appendIfMissing(std::vector<std::string>& args,
                                                const std::string& flag)
    {
        if (!hasExactToken(args, flag))
            args.push_back(flag);
    }

    void CompileCommandBuilder::removeOutputPathArgs(std::vector<std::string>& args)
    {
        std::size_t writeIndex = 0;
        for (std::size_t readIndex = 0; readIndex < args.size(); ++readIndex)
        {
            std::string& arg = args[readIndex];

            if (arg == "-o")
            {
                if (readIndex + 1 < args.size())
                    ++readIndex;
                continue;
            }
            if (arg.rfind("-o=", 0) == 0)
                continue;
            if (arg.size() > 2 && arg.rfind("-o", 0) == 0)
                continue;

            if (writeIndex != readIndex)
                args[writeIndex] = std::move(arg);
            ++writeIndex;
        }

        args.resize(writeIndex);
    }

    void CompileCommandBuilder::forceUnoptimizedCodegen(std::vector<std::string>& args)
    {
        // Appended after everything the caller supplied, because the last `-O` wins. The analysis
        // reasons about the program as written, and an optimizer is free to inline, merge and
        // delete exactly what the rules read: at -O2 a wait reached through a helper vanishes
        // with the helper, and the finding disappears with no sign that anything was lost.
        //
        // Nothing further is added. At -O0 clang already marks every function `optnone` and
        // `noinline`; what remains in the pipeline is lowering, which the analysis needs.
        args.emplace_back("-O0");
    }

    std::vector<std::string> CompileCommandBuilder::buildLL(const CompileRequest& request)
    {
        std::vector<std::string> args = request.extraCompileArgs;
        forceUnoptimizedCodegen(args);
        removeOutputPathArgs(args);
        appendIfMissing(args, "-emit-llvm");
        appendIfMissing(args, "-S");
        appendIfMissing(args, "-g");
        if (!hasExactToken(args, request.inputFile))
            args.push_back(request.inputFile);
        return args;
    }

    std::vector<std::string> CompileCommandBuilder::buildBC(const CompileRequest& request,
                                                            const std::filesystem::path& outputPath)
    {
        std::vector<std::string> args = request.extraCompileArgs;
        forceUnoptimizedCodegen(args);
        args.erase(std::remove(args.begin(), args.end(), "-S"), args.end());
        removeOutputPathArgs(args);
        appendIfMissing(args, "-emit-llvm");
        appendIfMissing(args, "-c");
        appendIfMissing(args, "-g");
        args.push_back("-o");
        args.push_back(outputPath.string());
        if (!hasExactToken(args, request.inputFile))
            args.push_back(request.inputFile);
        return args;
    }
} // namespace ctrace::concurrency::internal
