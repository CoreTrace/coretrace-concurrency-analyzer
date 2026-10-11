// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <array>
#include <cstdint>
#include <memory>
#include <optional>

namespace llvm
{
    class CallBase;
    class Constant;
    class Function;
    class LoadInst;
    class Module;
    class Value;
} // namespace llvm

namespace ctrace::concurrency::internal::analysis
{
    /// Where a word of a value comes from, in the terms of the function computing it: a constant,
    /// a word of one of its parameters, or a word of the memory a pointer parameter points to as
    /// it was when the function was entered.
    struct WordSource
    {
        enum class Kind
        {
            Constant,
            Parameter,
            Memory,
        };

        Kind kind = Kind::Constant;
        const llvm::Constant* constant = nullptr;
        unsigned parameter = 0;
        /// Bytes into the parameter's value, or past the address it holds.
        std::uint64_t offset = 0;

        friend bool operator==(const WordSource&, const WordSource&) = default;
    };

    /// A pointer-to-member function as the function invoking it receives it: its two fields, the
    /// function's address or v-table offset, then the adjustment of `this`.
    using HeldPair = std::array<WordSource, 2>;

    /// A call through a pointer-to-member, as clang lowers `(object.*member)(...)`: the load reading
    /// the pair, from which the call's target and its `this` are computed, and the object that
    /// `this` is adjusted from.
    struct MemberCall
    {
        const llvm::CallBase* call = nullptr;
        const llvm::LoadInst* pair = nullptr;
        const llvm::Value* object = nullptr;

        friend bool operator==(const MemberCall&, const MemberCall&) = default;
    };

    /// Which member function a call through a pointer-to-member invokes, on which object, in each
    /// context it is called from. It names targets and objects only: whether an invocation joins a
    /// thread is for the thread completion analysis to decide.
    ///
    /// Memory effects are computed for the whole module before any value is resolved: what each
    /// function may write through each pointer parameter, whether it lets one escape, and whether
    /// it may write any other memory. A declaration or an indirect call writes everything it may
    /// reach unless LLVM states it only reads memory. A value is then resolved on demand, word by
    /// word, through locals written once, aggregates, calls returning a value and calls storing one
    /// through a parameter; a word is known only when exactly one write can have produced it and
    /// no other write, by any path, can change it before it is read. A recursion is cut as an
    /// unknown word, so no conclusion rests on an assumption made before it was checked.
    class HeldMemberAnalysis
    {
      public:
        explicit HeldMemberAnalysis(const llvm::Module& module);
        ~HeldMemberAnalysis();
        HeldMemberAnalysis(const HeldMemberAnalysis&) = delete;
        HeldMemberAnalysis& operator=(const HeldMemberAnalysis&) = delete;

        /// `call` as a call through a pointer-to-member of the representation the module's target
        /// uses; empty for any other call, and for any target this analysis does not know.
        [[nodiscard]] std::optional<MemberCall> memberCall(const llvm::CallBase& call) const;
        /// The pair `member` reads, in the terms of the function making the call; empty when a word
        /// of it is unknown.
        [[nodiscard]] std::optional<HeldPair> pairOf(const MemberCall& member) const;
        /// `pair`, given in the terms of the function `call` calls, in those of its caller.
        [[nodiscard]] std::optional<HeldPair> atCall(const HeldPair& pair,
                                                     const llvm::CallBase& call) const;
        /// The parameter of its function the pointer `value` is, through the locals unoptimized
        /// code keeps it in and the functions returning one of their arguments.
        [[nodiscard]] std::optional<unsigned> parameterOf(const llvm::Value& value) const;
        /// The function `member` calls, on `member.object` itself, when the pair it reads holds the
        /// constants `pair` gives: the call's selection of its target and its adjustment of `this`
        /// are evaluated from that pair. Null when either cannot be evaluated, the target is not a
        /// function, or `this` is not the object.
        [[nodiscard]] const llvm::Function* calledWith(const MemberCall& member,
                                                       const HeldPair& pair) const;

      private:
        struct Impl;
        std::unique_ptr<Impl> impl_;
    };
} // namespace ctrace::concurrency::internal::analysis
