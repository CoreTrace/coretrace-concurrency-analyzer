#include "ir_loader.hpp"

#include <llvm/Bitcode/BitcodeReader.h>
#include <llvm/IR/Module.h>
#include <llvm/IRReader/IRReader.h>
#include <llvm/Support/Error.h>
#include <llvm/Support/MemoryBuffer.h>
#include <llvm/Support/SourceMgr.h>
#include <llvm/Support/raw_ostream.h>

namespace ctrace::concurrency::internal
{
    namespace
    {
        void setError(CompileError& error, CompileErrc code, std::string message)
        {
            error.code = make_error_code(code);
            error.phase = CompilePhase::IRParse;
            error.message = std::move(message);
        }
    } // namespace

    std::unique_ptr<llvm::Module> LLVMIRLoader::parseLL(std::string_view llvmIR,
                                                        llvm::LLVMContext& context,
                                                        CompileError& error) const
    {
        const llvm::StringRef irRef(llvmIR.data(), llvmIR.size());
        auto buffer = llvm::MemoryBuffer::getMemBuffer(
            llvm::MemoryBufferRef(irRef, "in_memory_ll"), /*RequiresNullTerminator=*/false);

        llvm::SMDiagnostic diag;
        std::unique_ptr<llvm::Module> module =
            llvm::parseIR(buffer->getMemBufferRef(), diag, context);
        if (module)
            return module;

        std::string message;
        llvm::raw_string_ostream os(message);
        diag.print("in_memory_ll", os);
        setError(error, CompileErrc::IRParseFailed, os.str());
        return nullptr;
    }

    std::unique_ptr<llvm::Module> LLVMIRLoader::parseBC(std::string_view llvmBitcode,
                                                        llvm::LLVMContext& context,
                                                        CompileError& error) const
    {
        const llvm::MemoryBufferRef bufferRef(
            llvm::StringRef(llvmBitcode.data(), llvmBitcode.size()), "in_memory_bc");

        auto parsedBitcode = llvm::parseBitcodeFile(bufferRef, context);
        if (parsedBitcode)
            return std::move(*parsedBitcode);

        setError(error, CompileErrc::BitcodeParseFailed, llvm::toString(parsedBitcode.takeError()));
        return nullptr;
    }
} // namespace ctrace::concurrency::internal
