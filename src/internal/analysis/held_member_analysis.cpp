// SPDX-License-Identifier: Apache-2.0
#include "held_member_analysis.hpp"

#include <llvm/ADT/DenseMap.h>
#include <llvm/ADT/DenseSet.h>
#include <llvm/ADT/STLExtras.h>
#include <llvm/ADT/SmallPtrSet.h>
#include <llvm/ADT/SmallVector.h>
#include <llvm/Analysis/CFG.h>
#include <llvm/Analysis/ConstantFolding.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/DataLayout.h>
#include <llvm/IR/Dominators.h>
#include <llvm/IR/InstIterator.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/IntrinsicInst.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Operator.h>
#include <llvm/TargetParser/Triple.h>

#include <algorithm>
#include <limits>
#include <map>
#include <utility>
#include <vector>

namespace ctrace::concurrency::internal::analysis
{
    namespace
    {
        /// The end of a write whose extent is unknown: it may cover any byte past its start.
        constexpr std::uint64_t kUnbounded = std::numeric_limits<std::uint64_t>::max();
        /// Past this many distinct writes through one parameter, the parameter counts as written
        /// whole: a recursion stepping its offset would otherwise grow the set without end.
        constexpr std::size_t kMaxWrites = 8;
        /// Bound on the instructions followed by any one walk.
        constexpr unsigned kMaxSteps = 256;

        /// How the target represents a pointer to member function, when it is one this analysis
        /// knows: two integers of a pointer's size, the function's address or v-table offset,
        /// then the adjustment of `this`. The Itanium C++ ABI (2.3) marks a virtual member with 1
        /// plus its v-table offset in the first field, the low bit of a non-virtual member
        /// function's address never being set; AArch64 follows ARM's variant, which marks it in
        /// the low bit of the adjustment, shifted left by one. Microsoft's representation differs
        /// altogether.
        struct Representation
        {
            unsigned wordBytes = 0;
            bool flagInAdjustment = false;
        };

        std::optional<Representation> representationOf(const llvm::Module& module)
        {
            const llvm::Triple triple(module.getTargetTriple());
            const unsigned wordBytes = module.getDataLayout().getPointerSize();
            if (triple.isWindowsMSVCEnvironment())
                return std::nullopt;
            if (triple.isAArch64())
                return Representation{.wordBytes = wordBytes, .flagInAdjustment = true};
            if (triple.getArch() == llvm::Triple::x86_64)
                return Representation{.wordBytes = wordBytes, .flagInAdjustment = false};
            return std::nullopt;
        }

        /// Where a pointer points within the function computing it: into one of its locals, into
        /// what one of its parameters points to, or somewhere this analysis cannot name.
        struct Place
        {
            enum class Kind
            {
                Local,
                Parameter,
                Unknown,
            };

            Kind kind = Kind::Unknown;
            const llvm::Value* base = nullptr;
            /// Empty when the offset into the object is not a constant.
            std::optional<std::int64_t> offset;
        };

        /// The one store into the local `slot`, when nothing but loads ever reads it: the slot
        /// unoptimized code keeps a value in.
        const llvm::StoreInst* soleStore(const llvm::Value& slot)
        {
            if (!llvm::isa<llvm::AllocaInst>(slot))
                return nullptr;
            const llvm::StoreInst* sole = nullptr;
            for (const llvm::User* user : slot.users())
            {
                if (const auto* store = llvm::dyn_cast<llvm::StoreInst>(user))
                {
                    if (store->getPointerOperand() != &slot || sole != nullptr)
                        return nullptr;
                    sole = store;
                }
                else if (!llvm::isa<llvm::LoadInst, llvm::DbgInfoIntrinsic>(user))
                    return nullptr;
            }
            return sole;
        }

        Place placeOf(const llvm::Value* pointer, const llvm::DataLayout& layout)
        {
            std::optional<std::int64_t> offset = 0;
            for (unsigned step = 0; step < kMaxSteps && pointer != nullptr; ++step)
            {
                pointer = pointer->stripPointerCasts();
                if (llvm::isa<llvm::AllocaInst>(pointer))
                    return Place{.kind = Place::Kind::Local, .base = pointer, .offset = offset};
                if (llvm::isa<llvm::Argument>(pointer))
                    return Place{.kind = Place::Kind::Parameter, .base = pointer, .offset = offset};
                if (const auto* gep = llvm::dyn_cast<llvm::GEPOperator>(pointer))
                {
                    llvm::APInt step(layout.getIndexTypeSizeInBits(gep->getType()), 0);
                    if (offset && gep->accumulateConstantOffset(layout, step))
                        offset = *offset + step.getSExtValue();
                    else
                        offset.reset();
                    pointer = gep->getPointerOperand();
                    continue;
                }
                const auto* load = llvm::dyn_cast<llvm::LoadInst>(pointer);
                const llvm::StoreInst* store =
                    load != nullptr ? soleStore(*load->getPointerOperand()) : nullptr;
                if (store == nullptr)
                    break;
                pointer = store->getValueOperand();
            }
            return Place{};
        }

        struct Written
        {
            std::uint64_t begin = 0;
            std::uint64_t end = 0;

            friend bool operator==(const Written&, const Written&) = default;
            friend bool operator<(const Written& left, const Written& right)
            {
                return std::pair{left.begin, left.end} < std::pair{right.begin, right.end};
            }
        };

        bool overlaps(const Written& write, std::uint64_t begin, std::uint64_t end)
        {
            return write.begin < end && begin < write.end;
        }

        /// What a function may write through one of its pointer parameters, and whether it lets
        /// that pointer escape, after which anything may write through it.
        struct ParameterEffects
        {
            std::vector<Written> written;
            bool escapes = false;

            friend bool operator==(const ParameterEffects&, const ParameterEffects&) = default;
        };

        /// What a function may write besides its own locals: through each parameter, and
        /// elsewhere — a global, memory reached through a pointer read from memory, or anything a
        /// call it cannot see may write.
        struct FunctionEffects
        {
            std::vector<ParameterEffects> parameters;
            bool elsewhere = false;

            friend bool operator==(const FunctionEffects&, const FunctionEffects&) = default;
        };

        bool marksOnly(const llvm::Instruction& instruction)
        {
            const auto* intrinsic = llvm::dyn_cast<llvm::IntrinsicInst>(&instruction);
            return intrinsic != nullptr && (intrinsic->isLifetimeStartOrEnd() ||
                                            llvm::isa<llvm::DbgInfoIntrinsic>(intrinsic));
        }

        /// The bytes a write through `pointer` covers, `size` long from where it points.
        Written extentOf(const Place& place, std::uint64_t size)
        {
            if (!place.offset || *place.offset < 0)
                return Written{.begin = 0, .end = kUnbounded};
            const auto begin = static_cast<std::uint64_t>(*place.offset);
            return Written{.begin = begin,
                           .end = size == kUnbounded || begin > kUnbounded - size ? kUnbounded
                                                                                  : begin + size};
        }
    } // namespace

    struct HeldMemberAnalysis::Impl
    {
        const llvm::Module& module;
        const llvm::DataLayout& layout;
        std::optional<Representation> representation;
        llvm::DenseMap<const llvm::Function*, FunctionEffects> effects;
        mutable std::map<const llvm::Function*, std::unique_ptr<llvm::DominatorTree>> dominators;
        mutable std::map<std::pair<const llvm::Function*, std::uint64_t>, std::optional<WordSource>>
            returned;
        mutable std::map<std::tuple<const llvm::Function*, unsigned, std::uint64_t>,
                         std::optional<WordSource>>
            stored;
        mutable llvm::DenseSet<const llvm::Function*> resolving;

        explicit Impl(const llvm::Module& analysed)
            : module(analysed), layout(analysed.getDataLayout()),
              representation(representationOf(analysed))
        {
            for (const llvm::Function& function : module)
                if (!function.isDeclaration())
                    effects[&function].parameters.resize(function.arg_size());
            // What a function may write only grows with what its callees may: from no write at
            // all, the iteration reaches the least effects every function has, recursion included.
            // Nothing below reads them before they are stable.
            bool changed = true;
            while (changed)
            {
                changed = false;
                for (const llvm::Function& function : module)
                {
                    if (function.isDeclaration())
                        continue;
                    FunctionEffects next = effectsOf(function);
                    if (!(next == effects[&function]))
                    {
                        effects[&function] = std::move(next);
                        changed = true;
                    }
                }
            }
        }

        // --- memory effects ---------------------------------------------------------------

        const FunctionEffects* effectsOfCallee(const llvm::CallBase& call) const
        {
            const llvm::Function* callee = call.getCalledFunction();
            const auto found = callee != nullptr ? effects.find(callee) : effects.end();
            return found != effects.end() ? &found->second : nullptr;
        }

        static void record(FunctionEffects& into, const Place& place, std::uint64_t size)
        {
            if (place.kind == Place::Kind::Local)
                return;
            if (place.kind == Place::Kind::Unknown)
            {
                into.elsewhere = true;
                return;
            }
            std::vector<Written>& written =
                into.parameters[llvm::cast<llvm::Argument>(place.base)->getArgNo()].written;
            // A write that may cover the whole object absorbs every other: the set only ever
            // grows, to at most kMaxWrites writes or that one, so the iteration ends.
            const Written whole{.begin = 0, .end = kUnbounded};
            const Written write = extentOf(place, size);
            if (llvm::is_contained(written, whole) || llvm::is_contained(written, write))
                return;
            if (write == whole || written.size() == kMaxWrites)
            {
                written = {whole};
                return;
            }
            written.insert(std::upper_bound(written.begin(), written.end(), write), write);
        }

        std::uint64_t storeSize(const llvm::Type& type) const
        {
            return layout.getTypeStoreSize(const_cast<llvm::Type*>(&type)).getFixedValue();
        }

        static std::uint64_t lengthOf(const llvm::MemIntrinsic& intrinsic)
        {
            const auto* length = llvm::dyn_cast<llvm::ConstantInt>(intrinsic.getLength());
            return length != nullptr ? length->getZExtValue() : kUnbounded;
        }

        void recordCall(FunctionEffects& into, const llvm::CallBase& call) const
        {
            if (marksOnly(call))
                return;
            if (const auto* intrinsic = llvm::dyn_cast<llvm::MemIntrinsic>(&call))
            {
                record(into, placeOf(intrinsic->getRawDest(), layout), lengthOf(*intrinsic));
                return;
            }
            if (llvm::isa<llvm::IntrinsicInst>(call) && !call.mayWriteToMemory())
                return;
            if (const FunctionEffects* callee = effectsOfCallee(call))
            {
                into.elsewhere = into.elsewhere || callee->elsewhere;
                for (unsigned index = 0;
                     index < call.arg_size() && index < callee->parameters.size(); ++index)
                {
                    const Place place = placeOf(call.getArgOperand(index), layout);
                    for (const Written& write : callee->parameters[index].written)
                    {
                        Place shifted = place;
                        if (shifted.offset && write.end != kUnbounded)
                            shifted.offset =
                                *shifted.offset + static_cast<std::int64_t>(write.begin);
                        else
                            shifted.offset.reset();
                        record(into, shifted,
                               write.end == kUnbounded ? kUnbounded : write.end - write.begin);
                    }
                }
                return;
            }
            // A declaration, an indirect call or inline assembly: only LLVM's statement that it
            // reads memory at most rules a write out.
            if (call.onlyReadsMemory())
                return;
            into.elsewhere = true;
            for (const llvm::Value* argument : call.args())
                if (argument->getType()->isPointerTy())
                    record(into, placeOf(argument, layout), kUnbounded);
        }

        FunctionEffects effectsOf(const llvm::Function& function) const
        {
            FunctionEffects result;
            result.parameters.resize(function.arg_size());
            for (const llvm::Instruction& instruction : llvm::instructions(function))
            {
                if (const auto* store = llvm::dyn_cast<llvm::StoreInst>(&instruction))
                    record(result, placeOf(store->getPointerOperand(), layout),
                           storeSize(*store->getValueOperand()->getType()));
                else if (const auto* call = llvm::dyn_cast<llvm::CallBase>(&instruction))
                    recordCall(result, *call);
                else if (instruction.mayWriteToMemory())
                    result.elsewhere = true;
            }
            for (const llvm::Argument& parameter : function.args())
                if (parameter.getType()->isPointerTy() && escapes(parameter))
                    result.parameters[parameter.getArgNo()].escapes = true;
            return result;
        }

        /// Whether `call` may keep the pointer it is handed as its argument `index`.
        bool keeps(const llvm::CallBase& call, unsigned index) const
        {
            if (marksOnly(call) || llvm::isa<llvm::MemIntrinsic>(call))
                return false;
            if (const FunctionEffects* callee = effectsOfCallee(call))
                return index >= callee->parameters.size() || callee->parameters[index].escapes;
            return !call.doesNotCapture(index);
        }

        /// Whether the pointer `parameter` holds may be kept anywhere past the function's frame:
        /// stored into memory other than the slot unoptimized code keeps it in, handed to a call
        /// that may keep it, or turned into anything but a pointer.
        bool escapes(const llvm::Argument& parameter) const
        {
            llvm::SmallVector<const llvm::Value*, 16> pending{&parameter};
            llvm::SmallPtrSet<const llvm::Value*, 16> seen;
            while (!pending.empty())
            {
                const llvm::Value* value = pending.pop_back_val();
                if (!seen.insert(value).second)
                    continue;
                if (seen.size() > kMaxSteps)
                    return true;
                for (const llvm::Use& use : value->uses())
                {
                    const llvm::User* user = use.getUser();
                    if (llvm::isa<llvm::GEPOperator, llvm::BitCastOperator, llvm::PHINode,
                                  llvm::SelectInst, llvm::AddrSpaceCastInst>(user))
                    {
                        pending.push_back(user);
                        continue;
                    }
                    if (llvm::isa<llvm::LoadInst, llvm::ICmpInst, llvm::ReturnInst>(user))
                        continue;
                    if (const auto* store = llvm::dyn_cast<llvm::StoreInst>(user))
                    {
                        if (use.getOperandNo() == store->getPointerOperandIndex())
                            continue;
                        if (soleStore(*store->getPointerOperand()) != store)
                            return true;
                        for (const llvm::User* reader : store->getPointerOperand()->users())
                            if (llvm::isa<llvm::LoadInst>(reader))
                                pending.push_back(reader);
                        continue;
                    }
                    const auto* call = llvm::dyn_cast<llvm::CallBase>(user);
                    if (call == nullptr || !call->isArgOperand(&use) ||
                        keeps(*call, call->getArgOperandNo(&use)))
                        return true;
                    if (call->getType()->isPointerTy())
                        pending.push_back(call);
                }
            }
            return false;
        }

        // --- writes to one object -----------------------------------------------------------

        /// The instructions of the local `local`'s function that may write its bytes from `begin`
        /// to `end`; empty when its address may be kept where those writes cannot be seen.
        std::optional<std::vector<const llvm::Instruction*>>
        writersOf(const llvm::AllocaInst& local, std::uint64_t begin, std::uint64_t end) const
        {
            std::vector<const llvm::Instruction*> writers;
            auto writes = [&](const llvm::Instruction& writer, std::optional<std::int64_t> at,
                              const Written& extent)
            {
                const bool known = at && *at >= 0 && extent.end != kUnbounded;
                const Written shifted =
                    known ? Written{.begin = static_cast<std::uint64_t>(*at) + extent.begin,
                                    .end = static_cast<std::uint64_t>(*at) + extent.end}
                          : Written{.begin = 0, .end = kUnbounded};
                if (overlaps(shifted, begin, end) && !llvm::is_contained(writers, &writer))
                    writers.push_back(&writer);
            };
            llvm::SmallVector<std::pair<const llvm::Value*, std::optional<std::int64_t>>, 16>
                pending{{&local, 0}};
            llvm::SmallPtrSet<const llvm::Value*, 16> seen;
            while (!pending.empty())
            {
                const auto [value, offset] = pending.pop_back_val();
                if (!seen.insert(value).second)
                    continue;
                if (seen.size() > kMaxSteps)
                    return std::nullopt;
                for (const llvm::Use& use : value->uses())
                {
                    const llvm::User* user = use.getUser();
                    if (const auto* gep = llvm::dyn_cast<llvm::GEPOperator>(user))
                    {
                        llvm::APInt step(layout.getIndexTypeSizeInBits(gep->getType()), 0);
                        pending.emplace_back(gep,
                                             offset && gep->accumulateConstantOffset(layout, step)
                                                 ? std::optional{*offset + step.getSExtValue()}
                                                 : std::nullopt);
                        continue;
                    }
                    if (llvm::isa<llvm::BitCastOperator, llvm::AddrSpaceCastInst>(user))
                    {
                        pending.emplace_back(user, offset);
                        continue;
                    }
                    if (llvm::isa<llvm::PHINode, llvm::SelectInst>(user))
                    {
                        pending.emplace_back(user, std::nullopt);
                        continue;
                    }
                    if (llvm::isa<llvm::LoadInst, llvm::ICmpInst>(user))
                        continue;
                    if (const auto* store = llvm::dyn_cast<llvm::StoreInst>(user))
                    {
                        if (use.getOperandNo() != store->getPointerOperandIndex())
                            return std::nullopt;
                        writes(*store, offset,
                               Written{.begin = 0,
                                       .end = storeSize(*store->getValueOperand()->getType())});
                        continue;
                    }
                    const auto* call = llvm::dyn_cast<llvm::CallBase>(user);
                    if (call == nullptr || !call->isArgOperand(&use))
                        return std::nullopt;
                    if (marksOnly(*call))
                        continue;
                    const unsigned index = call->getArgOperandNo(&use);
                    if (const auto* intrinsic = llvm::dyn_cast<llvm::MemIntrinsic>(call))
                    {
                        if (intrinsic->getRawDest() == value)
                            writes(*call, offset, Written{.begin = 0, .end = lengthOf(*intrinsic)});
                        continue;
                    }
                    if (keeps(*call, index))
                        return std::nullopt;
                    if (const FunctionEffects* callee = effectsOfCallee(*call))
                    {
                        if (index < callee->parameters.size())
                            for (const Written& write : callee->parameters[index].written)
                                writes(*call, offset, write);
                    }
                    else if (!call->onlyReadsMemory())
                        writes(*call, offset, Written{.begin = 0, .end = kUnbounded});
                    if (call->getType()->isPointerTy())
                        pending.emplace_back(call, std::nullopt);
                }
            }
            return writers;
        }

        const llvm::DominatorTree& dominatorsOf(const llvm::Function& function) const
        {
            std::unique_ptr<llvm::DominatorTree>& tree = dominators[&function];
            if (tree == nullptr)
                tree = std::make_unique<llvm::DominatorTree>(const_cast<llvm::Function&>(function));
            return *tree;
        }

        /// Whether `instruction` may write the bytes from `begin` to `end` of what the parameter
        /// `parameter` points to: any write that cannot be told to land elsewhere may, through
        /// another parameter or a pointer read from memory as well, since either may alias it.
        bool mayWriteParameter(const llvm::Instruction& instruction,
                               const llvm::Argument& parameter, std::uint64_t begin,
                               std::uint64_t end) const
        {
            auto lands = [&](const Place& place, std::uint64_t size)
            {
                if (place.kind == Place::Kind::Local)
                    return false;
                return place.kind == Place::Kind::Unknown || place.base != &parameter ||
                       overlaps(extentOf(place, size), begin, end);
            };
            if (const auto* store = llvm::dyn_cast<llvm::StoreInst>(&instruction))
                return lands(placeOf(store->getPointerOperand(), layout),
                             storeSize(*store->getValueOperand()->getType()));
            const auto* call = llvm::dyn_cast<llvm::CallBase>(&instruction);
            if (call == nullptr)
                return instruction.mayWriteToMemory();
            if (marksOnly(*call))
                return false;
            if (const auto* intrinsic = llvm::dyn_cast<llvm::MemIntrinsic>(call))
                return lands(placeOf(intrinsic->getRawDest(), layout), lengthOf(*intrinsic));
            if (llvm::isa<llvm::IntrinsicInst>(call) && !call->mayWriteToMemory())
                return false;
            const FunctionEffects* callee = effectsOfCallee(*call);
            if (callee == nullptr)
                return !call->onlyReadsMemory();
            if (callee->elsewhere)
                return true;
            for (unsigned index = 0; index < call->arg_size() && index < callee->parameters.size();
                 ++index)
            {
                const ParameterEffects& handed = callee->parameters[index];
                const Place place = placeOf(call->getArgOperand(index), layout);
                if (place.kind == Place::Kind::Local)
                    continue;
                if (handed.escapes && place.base == &parameter)
                    return true;
                for (const Written& write : handed.written)
                {
                    Place shifted = place;
                    if (shifted.offset && write.end != kUnbounded)
                        shifted.offset = *shifted.offset + static_cast<std::int64_t>(write.begin);
                    else
                        shifted.offset.reset();
                    if (lands(shifted,
                              write.end == kUnbounded ? kUnbounded : write.end - write.begin))
                        return true;
                }
            }
            return false;
        }

        // --- values, word by word -----------------------------------------------------------

        llvm::IntegerType* wordType() const
        {
            return llvm::IntegerType::get(module.getContext(), representation->wordBytes * 8);
        }

        std::uint64_t wordBytes() const
        {
            return representation->wordBytes;
        }

        /// The byte offset of the member `indices` select in a value of type `type`.
        std::optional<std::uint64_t> offsetOf(const llvm::Type* type,
                                              llvm::ArrayRef<unsigned> indices) const
        {
            std::uint64_t offset = 0;
            for (const unsigned index : indices)
            {
                if (const auto* structure = llvm::dyn_cast<llvm::StructType>(type))
                {
                    offset += layout.getStructLayout(const_cast<llvm::StructType*>(structure))
                                  ->getElementOffset(index);
                    type = structure->getElementType(index);
                }
                else if (const auto* array = llvm::dyn_cast<llvm::ArrayType>(type))
                {
                    type = array->getElementType();
                    offset += index * layout.getTypeAllocSize(const_cast<llvm::Type*>(type));
                }
                else
                    return std::nullopt;
            }
            return offset;
        }

        std::optional<WordSource> wordOf(const llvm::Value& value, std::uint64_t offset) const
        {
            if (offset + wordBytes() > storeSize(*value.getType()))
                return std::nullopt;
            if (const auto* constant = llvm::dyn_cast<llvm::Constant>(&value))
            {
                const llvm::Constant* word =
                    llvm::ConstantFoldLoadFromConst(const_cast<llvm::Constant*>(constant),
                                                    wordType(), llvm::APInt(64, offset), layout);
                if (word == nullptr)
                    return std::nullopt;
                return WordSource{.kind = WordSource::Kind::Constant, .constant = word};
            }
            if (const auto* parameter = llvm::dyn_cast<llvm::Argument>(&value))
                return WordSource{.kind = WordSource::Kind::Parameter,
                                  .parameter = parameter->getArgNo(),
                                  .offset = offset};
            if (const auto* load = llvm::dyn_cast<llvm::LoadInst>(&value))
                return bytesAt(*load->getPointerOperand(), offset, *load);
            if (const auto* extract = llvm::dyn_cast<llvm::ExtractValueInst>(&value))
            {
                const std::optional<std::uint64_t> start =
                    offsetOf(extract->getAggregateOperand()->getType(), extract->getIndices());
                return start ? wordOf(*extract->getAggregateOperand(), *start + offset)
                             : std::nullopt;
            }
            if (const auto* insert = llvm::dyn_cast<llvm::InsertValueInst>(&value))
            {
                const std::optional<std::uint64_t> start =
                    offsetOf(insert->getType(), insert->getIndices());
                if (!start)
                    return std::nullopt;
                const std::uint64_t size = storeSize(*insert->getInsertedValueOperand()->getType());
                if (offset >= *start && offset + wordBytes() <= *start + size)
                    return wordOf(*insert->getInsertedValueOperand(), offset - *start);
                if (offset + wordBytes() <= *start || offset >= *start + size)
                    return wordOf(*insert->getAggregateOperand(), offset);
                return std::nullopt;
            }
            if (const auto* cast = llvm::dyn_cast<llvm::CastInst>(&value))
            {
                const bool keepsBits =
                    llvm::isa<llvm::BitCastInst, llvm::PtrToIntInst, llvm::IntToPtrInst,
                              llvm::AddrSpaceCastInst>(cast) &&
                    storeSize(*cast->getSrcTy()) == storeSize(*cast->getDestTy());
                return keepsBits ? wordOf(*cast->getOperand(0), offset) : std::nullopt;
            }
            const auto* call = llvm::dyn_cast<llvm::CallBase>(&value);
            const llvm::Function* callee = call != nullptr ? call->getCalledFunction() : nullptr;
            if (callee == nullptr || callee->isDeclaration())
                return std::nullopt;
            const std::optional<WordSource> word = returnedWord(*callee, offset);
            return word ? instantiate(*word, *call) : std::nullopt;
        }

        std::optional<WordSource> instantiate(const WordSource& word,
                                              const llvm::CallBase& call) const
        {
            if (word.kind == WordSource::Kind::Constant)
                return word;
            if (word.parameter >= call.arg_size())
                return std::nullopt;
            const llvm::Value& argument = *call.getArgOperand(word.parameter);
            return word.kind == WordSource::Kind::Parameter ? wordOf(argument, word.offset)
                                                            : bytesAt(argument, word.offset, call);
        }

        /// The word `offset` bytes past where `pointer` points, as `at` reads it.
        std::optional<WordSource> bytesAt(const llvm::Value& pointer, std::uint64_t offset,
                                          const llvm::Instruction& at) const
        {
            const Place place = placeOf(&pointer, layout);
            if (!place.offset || *place.offset < 0)
                return std::nullopt;
            const std::uint64_t begin = static_cast<std::uint64_t>(*place.offset) + offset;
            const std::uint64_t end = begin + wordBytes();
            if (place.kind == Place::Kind::Local)
            {
                const auto& local = *llvm::cast<llvm::AllocaInst>(place.base);
                const std::optional<std::vector<const llvm::Instruction*>> writers =
                    writersOf(local, begin, end);
                if (!writers || writers->size() != 1 || writers->front() == &at ||
                    !dominatorsOf(*at.getFunction()).dominates(writers->front(), &at))
                    return std::nullopt;
                return writtenWord(*writers->front(), local, begin);
            }
            if (place.kind != Place::Kind::Parameter)
                return std::nullopt;
            const auto& parameter = *llvm::cast<llvm::Argument>(place.base);
            const llvm::DominatorTree& tree = dominatorsOf(*at.getFunction());
            for (const llvm::Instruction& instruction : llvm::instructions(*at.getFunction()))
                if (&instruction != &at &&
                    llvm::isPotentiallyReachable(&instruction, &at, nullptr, &tree) &&
                    mayWriteParameter(instruction, parameter, begin, end))
                    return std::nullopt;
            return WordSource{.kind = WordSource::Kind::Memory,
                              .parameter = parameter.getArgNo(),
                              .offset = begin};
        }

        /// The word from `begin` of the object `base` that `writer`, its only writer, leaves.
        std::optional<WordSource> writtenWord(const llvm::Instruction& writer,
                                              const llvm::Value& base, std::uint64_t begin) const
        {
            auto within = [&](const llvm::Value* pointer,
                              std::uint64_t size) -> std::optional<std::uint64_t>
            {
                const Place place = placeOf(pointer, layout);
                if (place.base != &base || !place.offset || *place.offset < 0)
                    return std::nullopt;
                const auto start = static_cast<std::uint64_t>(*place.offset);
                if (begin < start || size == kUnbounded || begin + wordBytes() > start + size)
                    return std::nullopt;
                return begin - start;
            };
            if (const auto* store = llvm::dyn_cast<llvm::StoreInst>(&writer))
            {
                const std::optional<std::uint64_t> at = within(
                    store->getPointerOperand(), storeSize(*store->getValueOperand()->getType()));
                return at ? wordOf(*store->getValueOperand(), *at) : std::nullopt;
            }
            if (const auto* transfer = llvm::dyn_cast<llvm::MemTransferInst>(&writer))
            {
                const std::optional<std::uint64_t> at =
                    within(transfer->getRawDest(), lengthOf(*transfer));
                return at ? bytesAt(*transfer->getRawSource(), *at, writer) : std::nullopt;
            }
            const auto* call = llvm::dyn_cast<llvm::CallBase>(&writer);
            const llvm::Function* callee = call != nullptr ? call->getCalledFunction() : nullptr;
            if (callee == nullptr || callee->isDeclaration())
                return std::nullopt;
            // The call writes the object through exactly one of its arguments.
            std::optional<WordSource> word;
            bool handed = false;
            for (unsigned index = 0; index < call->arg_size(); ++index)
            {
                const Place place = placeOf(call->getArgOperand(index), layout);
                if (place.base != &base)
                    continue;
                if (handed || !place.offset || *place.offset < 0 ||
                    begin < static_cast<std::uint64_t>(*place.offset))
                    return std::nullopt;
                handed = true;
                const std::optional<WordSource> left =
                    storedWord(*callee, index, begin - static_cast<std::uint64_t>(*place.offset));
                word = left ? instantiate(*left, *call) : std::nullopt;
            }
            return word;
        }

        /// The word `offset` bytes into what `function`'s parameter `index` points to, as every
        /// return of `function` leaves it: one write, ahead of every return, and no other write
        /// that may reach it, through that parameter, another one or anywhere else.
        std::optional<WordSource> storedWord(const llvm::Function& function, unsigned index,
                                             std::uint64_t offset) const
        {
            const auto key = std::tuple{&function, index, offset};
            if (const auto found = stored.find(key); found != stored.end())
                return found->second;
            if (!resolving.insert(&function).second)
                return std::nullopt;
            std::optional<WordSource> word;
            const llvm::Argument& parameter = *function.getArg(index);
            const llvm::Instruction* writer = nullptr;
            bool single = true;
            for (const llvm::Instruction& instruction : llvm::instructions(function))
                if (mayWriteParameter(instruction, parameter, offset, offset + wordBytes()))
                {
                    single = writer == nullptr;
                    writer = &instruction;
                    if (!single)
                        break;
                }
            if (single && writer != nullptr)
            {
                const llvm::DominatorTree& tree = dominatorsOf(function);
                bool ahead = true;
                for (const llvm::BasicBlock& block : function)
                    if (const auto* exit = llvm::dyn_cast<llvm::ReturnInst>(block.getTerminator()))
                        ahead = ahead && tree.dominates(writer, exit);
                if (ahead)
                    word = writtenWord(*writer, parameter, offset);
            }
            resolving.erase(&function);
            stored.emplace(key, word);
            return word;
        }

        /// The word `offset` bytes into the value every return of `function` returns.
        std::optional<WordSource> returnedWord(const llvm::Function& function,
                                               std::uint64_t offset) const
        {
            const auto key = std::pair{&function, offset};
            if (const auto found = returned.find(key); found != returned.end())
                return found->second;
            if (!resolving.insert(&function).second)
                return std::nullopt;
            std::optional<WordSource> word;
            bool agreed = true;
            for (const llvm::BasicBlock& block : function)
            {
                const auto* exit = llvm::dyn_cast<llvm::ReturnInst>(block.getTerminator());
                if (exit == nullptr || !agreed)
                    continue;
                const std::optional<WordSource> next = exit->getReturnValue() != nullptr
                                                           ? wordOf(*exit->getReturnValue(), offset)
                                                           : std::nullopt;
                agreed = next && (!word || *word == *next);
                word = next;
            }
            if (!agreed)
                word.reset();
            resolving.erase(&function);
            returned.emplace(key, word);
            return word;
        }

        // --- the call through the pair ------------------------------------------------------

        bool isPair(const llvm::Type& type) const
        {
            auto word = [&](const llvm::Type* field)
            { return field->isIntegerTy(representation->wordBytes * 8); };
            if (const auto* structure = llvm::dyn_cast<llvm::StructType>(&type))
                return structure->getNumElements() == 2 && word(structure->getElementType(0)) &&
                       word(structure->getElementType(1));
            const auto* array = llvm::dyn_cast<llvm::ArrayType>(&type);
            return array != nullptr && array->getNumElements() == 2 &&
                   word(array->getElementType());
        }

        /// The one load of a pair the value `target` is computed from.
        const llvm::LoadInst* pairLoadBehind(const llvm::Value& target) const
        {
            const llvm::LoadInst* found = nullptr;
            llvm::SmallVector<const llvm::Value*, 16> pending{&target};
            llvm::SmallPtrSet<const llvm::Value*, 16> seen;
            while (!pending.empty())
            {
                const llvm::Value* value = pending.pop_back_val();
                if (!seen.insert(value).second || seen.size() > kMaxSteps)
                    continue;
                const auto* instruction = llvm::dyn_cast<llvm::Instruction>(value);
                if (instruction == nullptr)
                    continue;
                if (const auto* load = llvm::dyn_cast<llvm::LoadInst>(instruction);
                    load != nullptr && isPair(*load->getType()))
                {
                    if (found != nullptr && found != load)
                        return nullptr;
                    found = load;
                    continue;
                }
                if (llvm::isa<llvm::CallBase>(instruction))
                    continue;
                for (const llvm::Value* operand : instruction->operands())
                    pending.push_back(operand);
            }
            return found;
        }

        /// What `instruction` computes once the values in `known` are, when LLVM's folding can
        /// tell, with the one fact LLVM cannot: under the Itanium representation, the low bit of a
        /// member function's address is clear.
        llvm::Constant* fold(const llvm::Instruction& instruction,
                             const llvm::DenseMap<const llvm::Value*, llvm::Constant*>& known) const
        {
            llvm::SmallVector<llvm::Constant*, 4> operands;
            for (const llvm::Value* operand : instruction.operands())
            {
                llvm::Constant* constant =
                    llvm::dyn_cast<llvm::Constant>(const_cast<llvm::Value*>(operand));
                if (constant == nullptr)
                {
                    const auto found = known.find(operand);
                    if (found == known.end())
                        return nullptr;
                    constant = found->second;
                }
                operands.push_back(constant);
            }
            if (!representation->flagInAdjustment &&
                instruction.getOpcode() == llvm::Instruction::And && operands.size() == 2)
            {
                const auto* address = llvm::dyn_cast<llvm::ConstantExpr>(operands[0]);
                const auto* mask = llvm::dyn_cast<llvm::ConstantInt>(operands[1]);
                if (address != nullptr && address->getOpcode() == llvm::Instruction::PtrToInt &&
                    llvm::isa<llvm::Function>(address->getOperand(0)->stripPointerCasts()) &&
                    mask != nullptr && mask->isOne())
                    return llvm::ConstantInt::get(instruction.getType(), 0);
            }
            if (const auto* compare = llvm::dyn_cast<llvm::CmpInst>(&instruction))
                return llvm::ConstantFoldCompareInstOperands(compare->getPredicate(), operands[0],
                                                             operands[1], layout);
            return llvm::ConstantFoldInstOperands(const_cast<llvm::Instruction*>(&instruction),
                                                  operands, layout);
        }

        /// The function `member` calls on its object when its pair holds `pair`: following the
        /// path from the pair's load, each branch taken as its condition evaluates, to the call.
        const llvm::Function* evaluate(const MemberCall& member, llvm::Constant& pair) const
        {
            llvm::DenseMap<const llvm::Value*, llvm::Constant*> known{{member.pair, &pair}};
            const llvm::BasicBlock* block = member.pair->getParent();
            const llvm::BasicBlock* from = nullptr;
            auto next = std::next(member.pair->getIterator());
            for (unsigned step = 0; step < kMaxSteps; ++step)
            {
                const llvm::Instruction& instruction = *next;
                if (&instruction == member.call)
                    return calledAt(member, known);
                if (const auto* branch = llvm::dyn_cast<llvm::BranchInst>(&instruction))
                {
                    unsigned successor = 0;
                    if (branch->isConditional())
                    {
                        const auto found = known.find(branch->getCondition());
                        const auto* condition =
                            found != known.end()
                                ? llvm::dyn_cast<llvm::ConstantInt>(found->second)
                                : llvm::dyn_cast<llvm::ConstantInt>(branch->getCondition());
                        if (condition == nullptr)
                            return nullptr;
                        successor = condition->isZero() ? 1 : 0;
                    }
                    from = block;
                    block = branch->getSuccessor(successor);
                    next = block->begin();
                    continue;
                }
                if (instruction.isTerminator())
                    return nullptr;
                if (const auto* phi = llvm::dyn_cast<llvm::PHINode>(&instruction))
                {
                    const llvm::Value* incoming =
                        from != nullptr ? phi->getIncomingValueForBlock(from) : nullptr;
                    if (incoming != nullptr)
                    {
                        if (auto* constant =
                                llvm::dyn_cast<llvm::Constant>(const_cast<llvm::Value*>(incoming)))
                            known[phi] = constant;
                        else if (const auto found = known.find(incoming); found != known.end())
                            known[phi] = found->second;
                    }
                }
                else if (llvm::Constant* value = fold(instruction, known))
                    known[&instruction] = value;
                ++next;
            }
            return nullptr;
        }

        const llvm::Function*
        calledAt(const MemberCall& member,
                 const llvm::DenseMap<const llvm::Value*, llvm::Constant*>& known) const
        {
            const auto target = known.find(member.call->getCalledOperand());
            const auto* function =
                target != known.end()
                    ? llvm::dyn_cast<llvm::Function>(target->second->stripPointerCasts())
                    : nullptr;
            if (function == nullptr || member.call->arg_empty())
                return nullptr;
            const llvm::Value* self = member.call->getArgOperand(0);
            if (self == member.object)
                return function;
            const auto* adjusted = llvm::dyn_cast<llvm::GEPOperator>(self);
            if (adjusted == nullptr || adjusted->getPointerOperand() != member.object)
                return nullptr;
            for (const llvm::Value* index : adjusted->indices())
            {
                const auto found = known.find(index);
                const auto* step = found != known.end()
                                       ? llvm::dyn_cast<llvm::ConstantInt>(found->second)
                                       : llvm::dyn_cast<llvm::ConstantInt>(index);
                if (step == nullptr || !step->isZero())
                    return nullptr;
            }
            return function;
        }
    };

    HeldMemberAnalysis::HeldMemberAnalysis(const llvm::Module& module)
        : impl_(std::make_unique<Impl>(module))
    {
    }

    HeldMemberAnalysis::~HeldMemberAnalysis() = default;

    std::optional<MemberCall> HeldMemberAnalysis::memberCall(const llvm::CallBase& call) const
    {
        if (!impl_->representation || call.getCalledFunction() != nullptr || call.isInlineAsm() ||
            call.arg_empty())
            return std::nullopt;
        const llvm::LoadInst* pair = impl_->pairLoadBehind(*call.getCalledOperand());
        if (pair == nullptr || pair->getFunction() != call.getFunction())
            return std::nullopt;
        const llvm::Value* self = call.getArgOperand(0);
        if (const auto* adjusted = llvm::dyn_cast<llvm::GEPOperator>(self))
            self = adjusted->getPointerOperand();
        return MemberCall{.call = &call, .pair = pair, .object = self};
    }

    std::optional<HeldPair> HeldMemberAnalysis::pairOf(const MemberCall& member) const
    {
        const llvm::Value& pointer = *member.pair->getPointerOperand();
        const std::optional<WordSource> first = impl_->bytesAt(pointer, 0, *member.pair);
        const std::optional<WordSource> second =
            impl_->bytesAt(pointer, impl_->wordBytes(), *member.pair);
        if (!first || !second)
            return std::nullopt;
        return HeldPair{*first, *second};
    }

    std::optional<HeldPair> HeldMemberAnalysis::atCall(const HeldPair& pair,
                                                       const llvm::CallBase& call) const
    {
        const std::optional<WordSource> first = impl_->instantiate(pair[0], call);
        const std::optional<WordSource> second = impl_->instantiate(pair[1], call);
        if (!first || !second)
            return std::nullopt;
        return HeldPair{*first, *second};
    }

    std::optional<unsigned> HeldMemberAnalysis::parameterOf(const llvm::Value& value) const
    {
        if (!impl_->representation || !value.getType()->isPointerTy())
            return std::nullopt;
        const std::optional<WordSource> word = impl_->wordOf(value, 0);
        if (!word || word->kind != WordSource::Kind::Parameter || word->offset != 0)
            return std::nullopt;
        return word->parameter;
    }

    const llvm::Function* HeldMemberAnalysis::calledWith(const MemberCall& member,
                                                         const HeldPair& pair) const
    {
        if (!impl_->representation || !impl_->isPair(*member.pair->getType()))
            return nullptr;
        llvm::SmallVector<llvm::Constant*, 2> fields;
        for (const WordSource& word : pair)
        {
            if (word.kind != WordSource::Kind::Constant)
                return nullptr;
            fields.push_back(const_cast<llvm::Constant*>(word.constant));
        }
        llvm::Type* type = member.pair->getType();
        llvm::Constant* constant =
            llvm::isa<llvm::StructType>(type)
                ? llvm::ConstantStruct::get(llvm::cast<llvm::StructType>(type), fields)
                : llvm::ConstantArray::get(llvm::cast<llvm::ArrayType>(type), fields);
        return impl_->evaluate(member, *constant);
    }
} // namespace ctrace::concurrency::internal::analysis
