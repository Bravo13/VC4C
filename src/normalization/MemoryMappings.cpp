/*
 * Author: doe300
 *
 * See the file "LICENSE" for the full license governing this code.
 */

#include "MemoryMappings.h"

#include "../intermediate/Helper.h"
#include "../intermediate/IntermediateInstruction.h"
#include "../intermediate/VectorHelper.h"
#include "../intermediate/operators.h"
#include "../periphery/TMU.h"
#include "../periphery/VPM.h"
#include "log.h"

using namespace vc4c;
using namespace vc4c::normalization;
using namespace vc4c::intermediate;
using namespace vc4c::operators;

using MemoryMapper = InstructionWalker (*)(Method&, InstructionWalker, MemoryInstruction*,
    const tools::SmallSortedPointerSet<const MemoryInfo*>&, const tools::SmallSortedPointerSet<const MemoryInfo*>&);

static InstructionWalker invalidMapping(Method& method, InstructionWalker it, MemoryInstruction* mem,
    const tools::SmallSortedPointerSet<const MemoryInfo*>& srcInfos,
    const tools::SmallSortedPointerSet<const MemoryInfo*>& destInfos)
{
    throw CompilationError(CompilationStep::NORMALIZER, "Invalid memory access", mem->to_string());
}

static InstructionWalker lowerMemoryReadOnlyToRegister(Method& method, InstructionWalker it, MemoryInstruction* mem,
    const tools::SmallSortedPointerSet<const MemoryInfo*>& srcInfos,
    const tools::SmallSortedPointerSet<const MemoryInfo*>& destInfos);
static InstructionWalker lowerMemoryReadWriteToRegister(Method& method, InstructionWalker it, MemoryInstruction* mem,
    const tools::SmallSortedPointerSet<const MemoryInfo*>& srcInfos,
    const tools::SmallSortedPointerSet<const MemoryInfo*>& destInfos);
static InstructionWalker lowerMemoryCopyToRegister(Method& method, InstructionWalker it, MemoryInstruction* mem,
    const tools::SmallSortedPointerSet<const MemoryInfo*>& srcInfos,
    const tools::SmallSortedPointerSet<const MemoryInfo*>& destInfos);
static InstructionWalker lowerMemoryReadToVPM(Method& method, InstructionWalker it, MemoryInstruction* mem,
    const tools::SmallSortedPointerSet<const MemoryInfo*>& srcInfos,
    const tools::SmallSortedPointerSet<const MemoryInfo*>& destInfos);
static InstructionWalker lowerMemoryWriteToVPM(Method& method, InstructionWalker it, MemoryInstruction* mem,
    const tools::SmallSortedPointerSet<const MemoryInfo*>& srcInfos,
    const tools::SmallSortedPointerSet<const MemoryInfo*>& destInfos);
static InstructionWalker loadMemoryViaTMU(Method& method, InstructionWalker it, MemoryInstruction* mem,
    const tools::SmallSortedPointerSet<const MemoryInfo*>& srcInfos,
    const tools::SmallSortedPointerSet<const MemoryInfo*>& destInfos);
static InstructionWalker accessMemoryInRAMViaVPM(Method& method, InstructionWalker it, MemoryInstruction* mem,
    const tools::SmallSortedPointerSet<const MemoryInfo*>& srcInfos,
    const tools::SmallSortedPointerSet<const MemoryInfo*>& destInfos);
static InstructionWalker mapMemoryCopy(Method& method, InstructionWalker it, MemoryInstruction* mem,
    const tools::SmallSortedPointerSet<const MemoryInfo*>& srcInfos,
    const tools::SmallSortedPointerSet<const MemoryInfo*>& destInfos);

/* clang-format off */
static constexpr MemoryMapper MAPPERS[6][4] = {
    /* READ,                         WRITE,                          COPY (from),                   FILL */
    {lowerMemoryReadOnlyToRegister,  invalidMapping,                 lowerMemoryReadOnlyToRegister, invalidMapping},                 /* QPU_REGISTER_READONLY */
    {lowerMemoryReadWriteToRegister, lowerMemoryReadWriteToRegister, lowerMemoryCopyToRegister,     lowerMemoryReadWriteToRegister}, /* QPU_REGISTER_READWRITE */
    {lowerMemoryReadToVPM,           lowerMemoryWriteToVPM,          mapMemoryCopy,                 lowerMemoryWriteToVPM},          /* VPM_PER_QPU */
    {lowerMemoryReadToVPM,           lowerMemoryWriteToVPM,          mapMemoryCopy,                 lowerMemoryWriteToVPM},          /* VPM_SHARED_ACCESS */
    {loadMemoryViaTMU,               invalidMapping,                 mapMemoryCopy,                 invalidMapping},                 /* RAM_LOAD_TMU */
    {accessMemoryInRAMViaVPM,        accessMemoryInRAMViaVPM,        mapMemoryCopy,                 accessMemoryInRAMViaVPM},        /* RAM_READ_WRITE_VPM */
};
/* clang-format on */

LCOV_EXCL_START
std::string MemoryInfo::to_string() const
{
    switch(type)
    {
    case MemoryAccessType::QPU_REGISTER_READONLY:
        return "read-only register " + mappedRegisterOrConstant.to_string();
    case MemoryAccessType::QPU_REGISTER_READWRITE:
        return "register " + mappedRegisterOrConstant.to_string();
    case MemoryAccessType::VPM_PER_QPU:
        return "private VPM area " + (area ? area->to_string() : "(null)");
    case MemoryAccessType::VPM_SHARED_ACCESS:
        return "shared VPM area " + (area ? area->to_string() : "(null)");
    case MemoryAccessType::RAM_LOAD_TMU:
        return "read-only memory access via TMU" + std::string(tmuFlag ? "1" : "0");
    case MemoryAccessType::RAM_READ_WRITE_VPM:
        return "read-write memory access via VPM" + (area ? " (cached in" + area->to_string() + ")" : "");
    }
    throw CompilationError(
        CompilationStep::NORMALIZER, "Unhandled memory info type", std::to_string(static_cast<uint32_t>(type)));
}
LCOV_EXCL_STOP

InstructionWalker normalization::mapMemoryAccess(Method& method, InstructionWalker it,
    intermediate::MemoryInstruction* mem, const tools::SmallSortedPointerSet<const MemoryInfo*>& srcInfos,
    const tools::SmallSortedPointerSet<const MemoryInfo*>& destInfos)
{
    auto& typeInfos = mem->op == MemoryOperation::READ || mem->op == MemoryOperation::COPY ? srcInfos : destInfos;
    auto type = (*typeInfos.begin())->type;
    for(const auto& info : typeInfos)
    {
        if(info->type != type)
            throw CompilationError(CompilationStep::NORMALIZER,
                "Can't map conditional memory accesses of different memory access types together",
                mem ? mem->to_string() : it->to_string());
    }
    return MAPPERS[static_cast<unsigned>(type)][static_cast<unsigned>(mem->op)](method, it, mem, srcInfos, destInfos);
}

// FIXME remove once all mappers support multiple sources/destinations
#define ASSERT_SINGLE_SOURCE(type)                                                                                     \
    if(srcInfos.size() != 1)                                                                                           \
    {                                                                                                                  \
        logging::error() << "Accessed memory locations: " << to_string<const MemoryInfo*>(srcInfos) << logging::endl;  \
        throw CompilationError(                                                                                        \
            CompilationStep::NORMALIZER, "This type of memory mapping does not yet support multiple sources", type);   \
    }                                                                                                                  \
    const MemoryInfo& srcInfo = **srcInfos.begin()

#define ASSERT_SINGLE_DESTINATION(type)                                                                                \
    if(destInfos.size() != 1)                                                                                          \
    {                                                                                                                  \
        logging::error() << "Accessed memory locations: " << to_string<const MemoryInfo*>(srcInfos) << logging::endl;  \
        throw CompilationError(CompilationStep::NORMALIZER,                                                            \
            "This type of memory mapping does not yet support multiple destinations", type);                           \
    }                                                                                                                  \
    const MemoryInfo& destInfo = **destInfos.begin()

static bool copiesWholeRegister(const Value& numEntries, const DataType& elementType, const DataType& registerType)
{
    // for copying of byte* where actually the whole vector is copied
    return numEntries.getLiteralValue() &&
        numEntries.getLiteralValue()->unsignedInt() * elementType.getLogicalWidth() == registerType.getLogicalWidth();
}

/*
 * There are several cases of memory lowered into registers:
 * - constant memory with constant index (direct value determinable) -> map to direct value
 * - constant memory which fits into register but dynamic index -> map to register, index by vector rotation
 * - private memory which fits into register -> map to register
 * - private memory where the type can be converted to fit into register -> map to register + index by vector
 * rotation
 */

/*
 * Lowers access to a constant memory location into a register.
 *
 * This can be done for constant memory locations.
 *
 * NOTE: This is the best optimization for memory access and should be preferred, where applicable.
 */
static InstructionWalker lowerMemoryReadOnlyToRegister(Method& method, InstructionWalker it, MemoryInstruction* mem,
    const tools::SmallSortedPointerSet<const MemoryInfo*>& srcInfos,
    const tools::SmallSortedPointerSet<const MemoryInfo*>& destInfos)
{
    ASSERT_SINGLE_SOURCE("lowerMemoryReadOnlyToRegister");
    if(mem->op != MemoryOperation::READ && mem->op != MemoryOperation::COPY)
        throw CompilationError(
            CompilationStep::NORMALIZER, "Cannot perform a non-read operation on constant memory", mem->to_string());

    Value tmpIndex = UNDEFINED_VALUE;
    it = insertAddressToElementOffset(
        it, method, tmpIndex, srcInfo.local, *srcInfo.mappedRegisterOrConstant, mem, mem->getSource());
    // TODO check whether index is guaranteed to be in range [0, 16[
    auto elementType = srcInfo.convertedRegisterType ? *srcInfo.convertedRegisterType :
                                                       srcInfo.mappedRegisterOrConstant->type.getElementType();

    auto wholeRegister = srcInfo.convertedRegisterType &&
        copiesWholeRegister(mem->getNumEntries(), mem->getDestinationElementType(), *srcInfo.convertedRegisterType);
    Value tmpVal(UNDEFINED_VALUE);
    if(mem->op == MemoryOperation::COPY && wholeRegister)
        // there is no need to calculate the index, if we copy the whole object
        tmpVal = *srcInfo.convertedRegisterType;
    else
    {
        tmpVal = method.addNewLocal(elementType, "%lowered_constant");
        it = insertVectorExtraction(it, method, *srcInfo.mappedRegisterOrConstant, tmpIndex, tmpVal);
    }

    if(srcInfo.mappedRegisterOrConstant && !srcInfo.mappedRegisterOrConstant->checkLocal())
    {
        if(mem->op == MemoryOperation::COPY)
        {
            if(!wholeRegister && mem->getNumEntries() != INT_ONE)
            {
                logging::error() << mem->getSource().to_string() << " - " << srcInfo.to_string() << " -> "
                                 << mem->getDestination().to_string() << " - "
                                 << to_string<const MemoryInfo*>(destInfos) << logging::endl;
                throw CompilationError(CompilationStep::NORMALIZER,
                    "Lowering copy with more than 1 entry is not yet implemented", mem->to_string());
            }
            it.reset(new MemoryInstruction(MemoryOperation::WRITE, Value(mem->getDestination()), std::move(tmpVal)));
            CPPLOG_LAZY(logging::Level::DEBUG,
                log << "Replaced memory copy from constant memory to memory write of constant value: "
                    << it->to_string() << logging::endl);
            return mapMemoryAccess(method, it, it.get<MemoryInstruction>(), srcInfos, destInfos);
        }
        if(mem->op == MemoryOperation::READ)
        {
            it.reset(new MoveOperation(mem->getDestination(), tmpVal));
            CPPLOG_LAZY(logging::Level::DEBUG,
                log << "Replaced loading of constant memory with constant literal: " << it->to_string()
                    << logging::endl);
            return it;
        }
    }
    if(srcInfo.convertedRegisterType)
    {
        if(mem->op == MemoryOperation::READ)
        {
            it.reset(new MoveOperation(mem->getDestination(), tmpVal));
            CPPLOG_LAZY(logging::Level::DEBUG,
                log << "Replaced loading of constant memory with vector rotation of register: " << it->to_string()
                    << logging::endl);
            return it;
        }
        if(mem->op == MemoryOperation::COPY)
        {
            if(!wholeRegister && mem->getNumEntries() != INT_ONE)
            {
                logging::error() << mem->getSource().to_string() << " - " << srcInfo.to_string() << " -> "
                                 << mem->getDestination().to_string() << " - "
                                 << to_string<const MemoryInfo*>(destInfos) << logging::endl;
                throw CompilationError(CompilationStep::NORMALIZER,
                    "Lowering copy with more than 1 entry is not yet implemented", mem->to_string());
            }
            it.reset(new MemoryInstruction(MemoryOperation::WRITE, Value(mem->getDestination()), std::move(tmpVal)));
            it = mapMemoryAccess(method, it, it.get<MemoryInstruction>(), srcInfos, destInfos);
            CPPLOG_LAZY(logging::Level::DEBUG,
                log << "Replaced copying from constant memory with vector rotation and writing of memory: "
                    << it->to_string() << logging::endl);
            return it;
        }
    }
    auto constant = getConstantValue(mem->getSource());
    if(constant)
    {
        if(mem->op == MemoryOperation::COPY)
        {
            if(mem->getNumEntries() != INT_ONE)
            {
                logging::error() << mem->getSource().to_string() << " - " << srcInfo.to_string() << " -> "
                                 << mem->getDestination().to_string() << " - "
                                 << to_string<const MemoryInfo*>(destInfos) << logging::endl;
                throw CompilationError(CompilationStep::NORMALIZER,
                    "Lowering copy with more than 1 entry is not yet implemented", mem->to_string());
            }
            // since a copy always involves another memory object, this rewrite is picked up when the other
            // object is processed
            it.reset(new MemoryInstruction(MemoryOperation::WRITE, Value(mem->getDestination()), *std::move(constant)));
            it = mapMemoryAccess(method, it, it.get<MemoryInstruction>(), srcInfos, destInfos);
            CPPLOG_LAZY(logging::Level::DEBUG,
                log << "Replaced memory copy from constant memory to memory write of constant value: "
                    << it->to_string() << logging::endl);
            return it;
        }
        else
        {
            it.reset(new MoveOperation(mem->getOutput().value(), *constant));
            CPPLOG_LAZY(logging::Level::DEBUG,
                log << "Replaced loading of constant memory with constant literal: " << it->to_string()
                    << logging::endl);
            return it;
        }
    }
    throw CompilationError(
        CompilationStep::NORMALIZER, "Unhandled case of lowering constant memory to register", mem->to_string());
}

/*
 * Maps memory access to the given local into moves from/to the given register
 *
 * NOTE: This is the best optimization for memory access and should always be preferred.
 * NOTE: This optimization cannot be applied if changes made to the lowered register need to be reflected to other QPUs.
 */
static InstructionWalker lowerMemoryReadWriteToRegister(Method& method, InstructionWalker it, MemoryInstruction* mem,
    const tools::SmallSortedPointerSet<const MemoryInfo*>& srcInfos,
    const tools::SmallSortedPointerSet<const MemoryInfo*>& destInfos)
{
    const MemoryInfo* loweredInfo = nullptr;
    if(mem->op == MemoryOperation::READ)
    {
        ASSERT_SINGLE_SOURCE("lowerMemoryReadWriteToRegister");
        loweredInfo = &srcInfo;
    }
    else
    {
        ASSERT_SINGLE_DESTINATION("lowerMemoryReadWriteToRegister");
        loweredInfo = &destInfo;
    }
    if(!loweredInfo->mappedRegisterOrConstant)
        throw CompilationError(CompilationStep::NORMALIZER,
            "Cannot map memory location to register without mapping register specified", mem->to_string());
    const auto& loweredRegister = loweredInfo->mappedRegisterOrConstant.value();
    const auto local = loweredInfo->local;
    // TODO check whether index is guaranteed to be in range [0, 16[
    if(mem->op == MemoryOperation::READ)
    {
        Value tmpIndex = UNDEFINED_VALUE;
        it = insertAddressToElementOffset(it, method, tmpIndex, local, loweredRegister, mem, mem->getSource());
        it = insertVectorExtraction(it, method, loweredRegister, tmpIndex, mem->getDestination());
    }
    else if(mem->op == MemoryOperation::WRITE)
    {
        Value tmpIndex = UNDEFINED_VALUE;
        it = insertAddressToElementOffset(it, method, tmpIndex, local, loweredRegister, mem, mem->getDestination());
        it = insertVectorInsertion(it, method, loweredRegister, tmpIndex, mem->getSource());
    }
    else if(mem->op == MemoryOperation::FILL && mem->getSource().type.isScalarType())
    {
        it = insertReplication(it, mem->getSource(), loweredRegister);
    }
    else
        throw CompilationError(
            CompilationStep::NORMALIZER, "Unhandled case of lowering memory access to register", mem->to_string());
    CPPLOG_LAZY(logging::Level::DEBUG,
        log << "Replaced access to stack allocation '" << it->to_string()
            << "' with: " << it.copy().previousInBlock()->to_string() << logging::endl);
    return it.erase();
}

static InstructionWalker lowerMemoryCopyToRegister(Method& method, InstructionWalker it, MemoryInstruction* mem,
    const tools::SmallSortedPointerSet<const MemoryInfo*>& srcInfos,
    const tools::SmallSortedPointerSet<const MemoryInfo*>& destInfos)
{
    ASSERT_SINGLE_SOURCE("lowerMemoryCopyToRegister");
    ASSERT_SINGLE_DESTINATION("lowerMemoryCopyToRegister");
    if(srcInfo.local == destInfo.local)
        throw CompilationError(CompilationStep::NORMALIZER,
            "Copy from and to same register lowered memory area is not supported", mem->to_string());
    if(mem->op != MemoryOperation::COPY)
        throw CompilationError(
            CompilationStep::NORMALIZER, "Unhandled case of lowering memory access to register", mem->to_string());
    if(destInfo.type == MemoryAccessType::QPU_REGISTER_READONLY)
        throw CompilationError(
            CompilationStep::NORMALIZER, "Copy into read-only registers is not supported", mem->to_string());

    auto wholeRegister =
        copiesWholeRegister(mem->getNumEntries(), mem->getDestinationElementType(), *srcInfo.convertedRegisterType);

    CPPLOG_LAZY(logging::Level::DEBUG,
        log << "Lowering copy with register-mapped memory: " << mem->to_string() << logging::endl);

    Value tmpIndex = UNDEFINED_VALUE;
    if(srcInfo.mappedRegisterOrConstant)
    {
        // TODO check whether index is guaranteed to be in range [0, 16[
        Value tmp(UNDEFINED_VALUE);
        Value numEntries = INT_ONE;
        if(wholeRegister)
            tmp = *srcInfo.mappedRegisterOrConstant;
        else
        {
            if(mem->getNumEntries() != INT_ONE)
            {
                if(auto lit = mem->getNumEntries().getLiteralValue())
                {
                    // TODO is this correct?
                    // NOTE: copied entry type could be byte, which actual type is half-word or word!
                    auto typeFactor = static_cast<uint32_t>(
                        srcInfo.mappedRegisterOrConstant->type.getElementType().getScalarBitCount() /
                        mem->getSourceElementType().getScalarBitCount());
                    if((lit->unsignedInt() % typeFactor) != 0)
                        throw CompilationError(CompilationStep::NORMALIZER,
                            "Copied number of bytes is not a multiple of the actual register type", mem->to_string());
                    auto numElements = lit->unsignedInt() / typeFactor;
                    if(numElements == 0 || numElements > NATIVE_VECTOR_SIZE)
                        throw CompilationError(
                            CompilationStep::NORMALIZER, "Invalid copied number of elements", mem->to_string());
                    tmp = method.addNewLocal(
                        srcInfo.mappedRegisterOrConstant->type.toVectorType(static_cast<uint8_t>(numElements)));
                }
                // TODO only add if above is tested and works!
                // else
                throw CompilationError(CompilationStep::NORMALIZER,
                    "Lowering copy with a dynamic number of entries is not yet implemented", mem->to_string());
            }
            else
                tmp = method.addNewLocal(mem->getSourceElementType());
            it = insertVectorExtraction(it, method, *srcInfo.mappedRegisterOrConstant, tmpIndex, tmp);
        }
        it.reset(new MemoryInstruction(
            MemoryOperation::WRITE, Value(mem->getDestination()), std::move(tmp), std::move(numEntries)));
        return mapMemoryAccess(method, it, it.get<MemoryInstruction>(), srcInfos, destInfos);
    }
    if(destInfo.mappedRegisterOrConstant)
    {
        // TODO is this ever called?? copying into register (from anywhere should be handled smewhere else)
        throw CompilationError(CompilationStep::NORMALIZER,
            "lowerMemoryCopyToRegister should not be called to copy into register", mem->to_string());
        auto tmp = method.addNewLocal(mem->getDestinationElementType());
        it.emplace(new MemoryInstruction(MemoryOperation::READ, std::move(tmp), Value(mem->getSource())));
        it = mapMemoryAccess(method, it, it.get<MemoryInstruction>(), srcInfos, destInfos);
        it = insertVectorInsertion(it, method, *destInfo.mappedRegisterOrConstant, tmpIndex, mem->getSource());
        return it.erase();
    }
    throw CompilationError(
        CompilationStep::NORMALIZER, "Unhandled case of lowering memory access to register", mem->to_string());
}

static InstructionWalker insertToInVPMAreaOffset(Method& method, InstructionWalker it, Value& out,
    const MemoryInfo& info, const MemoryInstruction* mem, const Value& ptrValue)
{
    if(info.ranges)
    {
        auto range = std::find_if(info.ranges->begin(), info.ranges->end(),
            [&](const MemoryAccessRange& range) -> bool { return range.addressWrite == it; });
        if(range == info.ranges->end())
            throw CompilationError(CompilationStep::NORMALIZER,
                "Failed to find memory access range for VPM cached memory access", mem->to_string());
        return insertAddressToWorkItemSpecificOffset(it, method, out, const_cast<MemoryAccessRange&>(*range));
    }
    return insertAddressToStackOffset(it, method, out, info.local, info.type, mem, ptrValue);
}

/*
 * Tries to map the given memory location into VPM
 *
 * This is applicable for private (stack) or local memory.
 *
 * NOTE: A memory location can only be lowered into VPM if all access to it can be lowered to VPM
 * NOTE: This is to be preferred over keeping the memory location in RAM
 */
static InstructionWalker lowerMemoryReadToVPM(Method& method, InstructionWalker it, MemoryInstruction* mem,
    const tools::SmallSortedPointerSet<const MemoryInfo*>& srcInfos,
    const tools::SmallSortedPointerSet<const MemoryInfo*>& destInfos)
{
    ASSERT_SINGLE_SOURCE("lowerMemoryReadToVPM");
    // Need to make sure addressing is still correct!
    if(srcInfo.type == MemoryAccessType::VPM_PER_QPU && !srcInfo.local->is<StackAllocation>())
        throw CompilationError(
            CompilationStep::NORMALIZER, "Unhandled case of per-QPU memory buffer", srcInfo.local->to_string());
    if(!srcInfo.area)
        throw CompilationError(CompilationStep::NORMALIZER, "Cannot lower into VPM without VPM area", mem->to_string());

    if(srcInfo.type == MemoryAccessType::VPM_PER_QPU)
        CPPLOG_LAZY(logging::Level::DEBUG,
            log << "Lowering read of stack allocation into VPM: " << mem->to_string() << logging::endl);
    else
        CPPLOG_LAZY(logging::Level::DEBUG,
            log << "Lowering read of shared local memory into VPM: " << mem->to_string() << logging::endl);

    Value inAreaOffset = UNDEFINED_VALUE;
    it = insertToInVPMAreaOffset(method, it, inAreaOffset, srcInfo, mem, mem->getSource());
    if(mem->op == MemoryOperation::READ)
    {
        it = method.vpm->insertReadVPM(method, it, mem->getDestination(), srcInfo.area, mem->guardAccess, inAreaOffset);
        return it.erase();
    }
    throw CompilationError(
        CompilationStep::NORMALIZER, "Unhandled case to lower reading of memory into VPM", mem->to_string());
}

static InstructionWalker lowerMemoryWriteToVPM(Method& method, InstructionWalker it, MemoryInstruction* mem,
    const tools::SmallSortedPointerSet<const MemoryInfo*>& srcInfos,
    const tools::SmallSortedPointerSet<const MemoryInfo*>& destInfos)
{
    ASSERT_SINGLE_DESTINATION("lowerMemoryWriteToVPM");
    if(destInfo.type == MemoryAccessType::VPM_PER_QPU && !destInfo.local->is<StackAllocation>())
        throw CompilationError(
            CompilationStep::NORMALIZER, "Unhandled case of per-QPU memory buffer", destInfo.local->to_string());
    if(!destInfo.area)
        throw CompilationError(CompilationStep::NORMALIZER, "Cannot lower into VPM without VPM area", mem->to_string());

    if(destInfo.type == MemoryAccessType::VPM_PER_QPU)
        CPPLOG_LAZY(logging::Level::DEBUG,
            log << "Lowering write to stack allocation into VPM: " << mem->to_string() << logging::endl);
    else
        CPPLOG_LAZY(logging::Level::DEBUG,
            log << "Lowering write to shared local memory into VPM: " << mem->to_string() << logging::endl);

    Value inAreaOffset = UNDEFINED_VALUE;
    it = insertToInVPMAreaOffset(method, it, inAreaOffset, destInfo, mem, mem->getDestination());
    if(mem->op == MemoryOperation::WRITE)
    {
        it = method.vpm->insertWriteVPM(method, it, mem->getSource(), destInfo.area, mem->guardAccess, inAreaOffset);
        return it.erase();
    }
    if(mem->op == MemoryOperation::FILL)
    {
        if(!mem->getNumEntries().getLiteralValue())
        {
            throw CompilationError(CompilationStep::NORMALIZER,
                "Filling dynamically sized VPM area is not yet implemented", mem->to_string());
        }
        if(mem->getSource().type == TYPE_INT8)
        {
            // if we fill single bytes, combine them to some vector type to not have to write so many single bytes
            auto vpmType = periphery::getBestVectorSize(mem->getNumEntries().getLiteralValue()->unsignedInt());
            // 1. replicate byte across word
            auto fillWord = assign(it, TYPE_INT32) = (mem->getSource(), UNPACK_8A_32);
            // 2. replicate word across all vector elements
            auto fillVector = method.addNewLocal(TYPE_INT32.toVectorType(16), "%memory_fill");
            it = insertReplication(it, fillWord, fillVector);
            // 3. calculate base offset for per-QPU memory area
            Value inAreaOffset = UNDEFINED_VALUE;
            it = insertToInVPMAreaOffset(method, it, inAreaOffset, destInfo, mem, mem->getDestination());
            // 3. write vector to VPM
            auto vpmTypeSize = Literal(vpmType.first.getInMemoryWidth());
            if(mem->guardAccess)
            {
                it.emplace(new MutexLock(MutexAccess::LOCK));
                it.nextInBlock();
            }
            for(unsigned i = 0; i < vpmType.second; ++i)
            {
                auto byteOffset = assign(it, TYPE_INT32) = Value(Literal(i), TYPE_INT32) * vpmTypeSize;
                byteOffset = assign(it, TYPE_INT32) = inAreaOffset + byteOffset;
                it = method.vpm->insertWriteVPM(method, it, fillVector, destInfo.area, false, byteOffset);
            }
            if(mem->guardAccess)
            {
                it.emplace(new MutexLock(MutexAccess::RELEASE));
                it.nextInBlock();
            }
            return it.erase();
        }
        logging::error() << "Destination: " << destInfo.local->to_string() << " - " << mem->getNumEntries().to_string()
                         << " - " << mem->getSource().to_string() << " - "
                         << (destInfo.area ? destInfo.area->to_string() : "") << logging::endl;
    }
    throw CompilationError(
        CompilationStep::NORMALIZER, "Unhandled case to lower writing of memory into VPM", mem->to_string());
}

/*
 * Maps a single memory read to a TMU load
 *
 * NOTE: Memory locations loaded via TMU MUST NOT be written to by the same kernel (even on a different QPU)!
 */
static InstructionWalker loadMemoryViaTMU(Method& method, InstructionWalker it, MemoryInstruction* mem,
    const tools::SmallSortedPointerSet<const MemoryInfo*>& srcInfos,
    const tools::SmallSortedPointerSet<const MemoryInfo*>& destInfos)
{
    CPPLOG_LAZY(
        logging::Level::DEBUG, log << "Loading from read-only memory via TMU: " << mem->to_string() << logging::endl);
    if(mem->op == MemoryOperation::READ)
    {
        // we have to use the same TMU for all loads. To minimize cache misses, select the TMU which is used by the most
        // sources selected
        uint32_t numTMU0 = 0;
        uint32_t numTMU1 = 0;
        for(auto srcInfo : srcInfos)
        {
            if(auto param = srcInfo->local->as<Parameter>())
                const_cast<Parameter*>(param)->decorations = add_flag(param->decorations, ParameterDecorations::INPUT);
            ++(srcInfo->tmuFlag ? numTMU0 : numTMU1);
        }
        // prefer TMU1 here, since statistically TMU0 will be used more often
        auto tmu = numTMU0 > numTMU1 ? periphery::TMU0 : periphery::TMU1;
        it = periphery::insertReadVectorFromTMU(method, it, mem->getDestination(), mem->getSource(), tmu);
        return it.erase();
    }
    throw CompilationError(CompilationStep::NORMALIZER, "Unhandled case to read from memory via TMU", mem->to_string());
}

/*
 * Maps a memory access instruction to an instruction accessing RAM through VPM.
 *
 * NOTE: At least one of the operands of the instruction to be mapped must be located in RAM
 * NOTE: this is the least optimal mapping possible and should avoided if possible.
 */
static InstructionWalker accessMemoryInRAMViaVPM(Method& method, InstructionWalker it, MemoryInstruction* mem,
    const tools::SmallSortedPointerSet<const MemoryInfo*>& srcInfos,
    const tools::SmallSortedPointerSet<const MemoryInfo*>& destInfos)
{
    CPPLOG_LAZY(
        logging::Level::DEBUG, log << "Mapping access to memory located in RAM: " << mem->to_string() << logging::endl);
    switch(mem->op)
    {
    case MemoryOperation::FILL:
    {
        if(mem->guardAccess)
        {
            it.emplace(new MutexLock(MutexAccess::LOCK));
            it.nextInBlock();
        }
        if(auto numCopies = mem->getNumEntries().getLiteralValue())
        {
            if(mem->getSource().type == TYPE_INT8)
            {
                // if we fill single bytes, combine them to some vector type to not have to write so many single bytes
                auto vpmType = periphery::getBestVectorSize(numCopies->unsignedInt());
                // 1. replicate byte across word
                auto fillWord = assign(it, TYPE_INT32) = (mem->getSource(), UNPACK_8A_32);
                // 2. replicate word across all vector elements
                auto fillVector = method.addNewLocal(TYPE_INT32.toVectorType(16), "%memory_fill");
                it = insertReplication(it, fillWord, fillVector);
                // 3. write vector to VPM
                it = method.vpm->insertWriteVPM(method, it, fillVector, nullptr, false);
                // 4. fill memory with vector
                it = method.vpm->insertFillRAM(
                    method, it, mem->getDestination(), vpmType.first, vpmType.second, nullptr, false);
            }
            else
            {
                it = method.vpm->insertWriteVPM(method, it, mem->getSource(), nullptr, false);
                it = method.vpm->insertFillRAM(method, it, mem->getDestination(), mem->getSourceElementType(),
                    numCopies->unsignedInt(), nullptr, false);
            }
        }
        else
        {
            // Fill dynamically sized memory
            // TODO This is usually the result of an (llvm.)memset(...) instruction, which always writes a certain
            // number of single bytes, which is very inefficient!
            it = method.vpm->insertWriteVPM(method, it, mem->getSource(), nullptr, false);
            it = method.vpm->insertFillRAMDynamic(
                method, it, mem->getDestination(), mem->getSourceElementType(), mem->getNumEntries(), nullptr, false);
        }
        if(mem->guardAccess)
        {
            it.emplace(new MutexLock(MutexAccess::RELEASE));
            it.nextInBlock();
        }
        for(auto destInfo : destInfos)
        {
            if(auto param = destInfo->local->as<Parameter>())
                const_cast<Parameter*>(param)->decorations = add_flag(param->decorations, ParameterDecorations::OUTPUT);
        }
        break;
    }
    case MemoryOperation::READ:
    {
        it = periphery::insertReadDMA(method, it, mem->getDestination(), mem->getSource(), mem->guardAccess);
        for(auto srcInfo : srcInfos)
        {
            if(auto param = srcInfo->local->as<Parameter>())
                const_cast<Parameter*>(param)->decorations = add_flag(param->decorations, ParameterDecorations::INPUT);
        }

        break;
    }
    case MemoryOperation::WRITE:
    {
        it = periphery::insertWriteDMA(method, it, mem->getSource(), mem->getDestination(), mem->guardAccess);
        for(auto destInfo : destInfos)
        {
            if(auto param = destInfo->local->as<Parameter>())
                const_cast<Parameter*>(param)->decorations = add_flag(param->decorations, ParameterDecorations::OUTPUT);
        }
        break;
    }
    default:
        throw CompilationError(CompilationStep::NORMALIZER, "Unhandled case of accessing RAM", mem->to_string());
    }
    // remove MemoryInstruction
    // since a copy may have another iterator to it, do not remove the element, just clear it
    // the empty instruction is cleaned up in #combineVPMAccess
    return it.erase();
}

static InstructionWalker mapMemoryCopy(Method& method, InstructionWalker it, MemoryInstruction* mem,
    const tools::SmallSortedPointerSet<const MemoryInfo*>& srcInfos,
    const tools::SmallSortedPointerSet<const MemoryInfo*>& destInfos)
{
    /*
     * Handled cases:
     *
     * From\To |     VPM      |        RAM           |
     * VPM     | read + write |      DMA write       |
     * RAM     |   DMA read   | DMA read + DMA write |
     *
     */
    ASSERT_SINGLE_SOURCE("mapMemoryCopy");
    ASSERT_SINGLE_DESTINATION("mapMemoryCopy");

    // srcInRegister is handled by another function
    bool destInRegister = destInfo.type == MemoryAccessType::QPU_REGISTER_READWRITE;
    bool srcInVPM =
        srcInfo.type == MemoryAccessType::VPM_PER_QPU || srcInfo.type == MemoryAccessType::VPM_SHARED_ACCESS;
    bool srcInRAM =
        srcInfo.type == MemoryAccessType::RAM_LOAD_TMU || srcInfo.type == MemoryAccessType::RAM_READ_WRITE_VPM;
    bool destInVPM =
        destInfo.type == MemoryAccessType::VPM_PER_QPU || destInfo.type == MemoryAccessType::VPM_SHARED_ACCESS;
    bool destInRAM =
        destInfo.type == MemoryAccessType::RAM_LOAD_TMU || destInfo.type == MemoryAccessType::RAM_READ_WRITE_VPM;

    for(auto srcInfo : srcInfos)
    {
        if(auto param = srcInfo->local->as<Parameter>())
            const_cast<Parameter*>(param)->decorations = add_flag(param->decorations, ParameterDecorations::INPUT);
    }
    for(auto destInfo : destInfos)
    {
        if(auto param = destInfo->local->as<Parameter>())
            const_cast<Parameter*>(param)->decorations = add_flag(param->decorations, ParameterDecorations::OUTPUT);
    }

    // for some/all copies, LLVM generates memcpy of i8* to i8* with the number of bytes as number of elements. We need
    // to convert it back to the actual number of elements of the given type
    auto numEntries = mem->getNumEntries();
    Optional<DataType> vpmRowType{};
    if(numEntries.getLiteralValue() && srcInfo.area && mem->getSourceElementType() == TYPE_INT8)
    {
        auto origType = srcInfo.local->type.getElementType();
        auto numBytes = numEntries.getLiteralValue()->unsignedInt();
        if(numBytes != origType.getInMemoryWidth())
            throw CompilationError(
                CompilationStep::NORMALIZER, "Byte-wise partial copy from VPM is yet implemented", mem->to_string());
        if(auto array = origType.getArrayType())
        {
            numEntries = Value(Literal(array->size), TYPE_INT32);
            vpmRowType = array->elementType;
        }
        else if(origType.isVectorType())
        {
            numEntries = INT_ONE;
            vpmRowType = origType;
        }
        else
            throw CompilationError(
                CompilationStep::NORMALIZER, "Unsupported element type for memory copy into VPM", mem->to_string());
    }

    if(numEntries.getLiteralValue() && destInfo.area && mem->getDestinationElementType() == TYPE_INT8)
    {
        auto origType = destInfo.local->type.getElementType();
        auto numBytes = numEntries.getLiteralValue()->unsignedInt();
        if(numBytes != origType.getInMemoryWidth())
            throw CompilationError(
                CompilationStep::NORMALIZER, "Byte-wise partial copy to VPM is yet implemented", mem->to_string());
        if(auto array = origType.getArrayType())
        {
            numEntries = Value(Literal(array->size), TYPE_INT32);
            vpmRowType = array->elementType;
        }
        else if(origType.isVectorType())
        {
            numEntries = INT_ONE;
            vpmRowType = origType;
        }
        else
            throw CompilationError(
                CompilationStep::NORMALIZER, "Unsupported element type for memory copy into VPM", mem->to_string());
    }

    if(srcInVPM && destInVPM)
    {
        // copy from VPM into VPM -> VPM read + VPM write
        CPPLOG_LAZY(logging::Level::DEBUG,
            log << "Mapping copy from/to VPM to VPM read and VPM write: " << mem->to_string() << logging::endl);

        if(numEntries != INT_ONE)
            // TODO could for static count insert that number of reads/writes, for dynamic need a loop!
            throw CompilationError(CompilationStep::NORMALIZER,
                "Copying within VPM with more than 1 entries is not yet implemented", mem->to_string());
        if(mem->guardAccess)
        {
            it.emplace(new MutexLock(MutexAccess::LOCK));
            it.nextInBlock();
        }
        auto tmpVal = method.addNewLocal(mem->getSourceElementType(), "%vpm_copy_tmp");
        it.emplace(new MemoryInstruction(
            MemoryOperation::READ, Value(tmpVal), Value(mem->getSource()), Value(numEntries), false));
        it = mapMemoryAccess(method, it, it.get<MemoryInstruction>(), srcInfos, destInfos);
        it.reset(new MemoryInstruction(
            MemoryOperation::WRITE, Value(mem->getDestination()), std::move(tmpVal), Value(numEntries), false));
        it = mapMemoryAccess(method, it, it.get<MemoryInstruction>(), srcInfos, destInfos);
        if(mem->guardAccess)
        {
            it.emplace(new MutexLock(MutexAccess::RELEASE));
            it.nextInBlock();
        }
        return it;
    }
    else if(srcInVPM && destInRAM)
    {
        // copy from VPM into RAM -> DMA write
        CPPLOG_LAZY(logging::Level::DEBUG,
            log << "Mapping copy from VPM into RAM to DMA write: " << mem->to_string() << logging::endl);
        Value inAreaOffset = UNDEFINED_VALUE;
        it = insertToInVPMAreaOffset(method, it, inAreaOffset, srcInfo, mem, mem->getSource());
        it = method.vpm->insertWriteRAM(method, it,
            Value(mem->getDestination().local(), vpmRowType.value_or(mem->getDestinationElementType())),
            vpmRowType.value_or(mem->getSourceElementType()), srcInfo.area, mem->guardAccess, inAreaOffset, numEntries);
        return it.erase();
    }
    else if(srcInRAM && destInVPM)
    {
        // copy from RAM into VPM -> DMA read
        CPPLOG_LAZY(logging::Level::DEBUG,
            log << "Mapping copy from RAM into VPM to DMA read: " << mem->to_string() << logging::endl);
        Value inAreaOffset = UNDEFINED_VALUE;
        it = insertToInVPMAreaOffset(method, it, inAreaOffset, destInfo, mem, mem->getDestination());
        it = method.vpm->insertReadRAM(method, it,
            Value(mem->getSource().local(), vpmRowType.value_or(mem->getSourceElementType())),
            vpmRowType.value_or(mem->getDestinationElementType()), destInfo.area, mem->guardAccess, inAreaOffset,
            numEntries);
        return it.erase();
    }
    else if(srcInRAM && destInRAM)
    {
        // copy from RAM into RAM -> DMA read + DMA write
        CPPLOG_LAZY(logging::Level::DEBUG,
            log << "Mapping copy from RAM into RAM to DMA read and DMA write: " << mem->to_string() << logging::endl);
        if(!numEntries.isLiteralValue())
            it = method.vpm->insertCopyRAMDynamic(
                method, it, mem->getDestination(), mem->getSource(), numEntries, nullptr, mem->guardAccess);
        else
        {
            uint64_t numBytes = numEntries.getLiteralValue()->unsignedInt() *
                (mem->getSourceElementType().getScalarBitCount() * mem->getSourceElementType().getVectorWidth()) / 8;
            if(numBytes > std::numeric_limits<unsigned>::max())
                throw CompilationError(
                    CompilationStep::OPTIMIZER, "Cannot copy more than 4GB of data", mem->to_string());

            it = method.vpm->insertCopyRAM(method, it, mem->getDestination(), mem->getSource(),
                static_cast<unsigned>(numBytes), nullptr, mem->guardAccess);
        }
        return it.erase();
    }
    else if(destInRegister && destInfo.convertedRegisterType)
    {
        // copy from VPM/RAM into register -> read from VPM/RAM + write to register
        if(copiesWholeRegister(numEntries, mem->getSourceElementType(), *destInfo.convertedRegisterType))
        {
            // e.g. for copying 32 bytes into float[8] register -> just read 1 float16 vector
            CPPLOG_LAZY(logging::Level::DEBUG,
                log << "Mapping copy of whole register from VPM/RAM into register to read from VPM/RAM: "
                    << mem->to_string() << logging::endl);
            it.reset(new MemoryInstruction(MemoryOperation::READ, Value(*destInfo.mappedRegisterOrConstant),
                Value(mem->getSource().local(), method.createPointerType(*destInfo.convertedRegisterType)),
                Value(INT_ONE), mem->guardAccess));
            return mapMemoryAccess(method, it, it.get<MemoryInstruction>(), srcInfos, destInfos);
        }
        else if(numEntries.getLiteralValue() &&
            (numEntries.getLiteralValue()->unsignedInt() * mem->getSourceElementType().getLogicalWidth()) <=
                TYPE_INT32.toVectorType(NATIVE_VECTOR_SIZE).getLogicalWidth())
        {
            // general case, read whole row via TMU/VPM and insert only actually used elements
            CPPLOG_LAZY(logging::Level::DEBUG,
                log << "Mapping partial copy of read only RAM into register: " << mem->to_string() << logging::endl);
            // e.g. if we copy 2 entries of int2, we need to copy 4 SIMD elements
            // also if we copy 20 entries of i8, we need to copy 5 SIMD elements of i32!
            auto numElements =
                (numEntries.getLiteralValue()->unsignedInt() * mem->getSourceElementType().getLogicalWidth()) /
                destInfo.convertedRegisterType->getElementType().getLogicalWidth();
            if(numElements == 0 || numElements > NATIVE_VECTOR_SIZE)
                // TODO copy e.g. copying 1 byte into an int vector, need to combine the byte with the rest of the word
                // for the correct element
                throw CompilationError(
                    CompilationStep::NORMALIZER, "Invalid copied number of elements", mem->to_string());

            if(mem->guardAccess)
            {
                it.emplace(new MutexLock(MutexAccess::LOCK));
                it.nextInBlock();
            }
            auto tmp = method.addNewLocal(
                mem->getSourceElementType().toVectorType(static_cast<uint8_t>(numElements)), "%mem_read_tmp");
            it.emplace(new MemoryInstruction(
                MemoryOperation::READ, Value(tmp), Value(mem->getSource()), Value(INT_ONE), false));
            it = mapMemoryAccess(method, it, it.get<MemoryInstruction>(), srcInfos, destInfos);
            it.reset(new MemoryInstruction(
                MemoryOperation::WRITE, Value(mem->getDestination()), std::move(tmp), Value(INT_ONE), false));
            it = mapMemoryAccess(method, it, it.get<MemoryInstruction>(), srcInfos, destInfos);
            if(mem->guardAccess)
            {
                it.emplace(new MutexLock(MutexAccess::RELEASE));
                it.nextInBlock();
            }
            return it;
        }
        else
        {
            // copy an dynamic (or constant but too big fitting) area of VPM/RAM (via TMU or VPM) to register
            logging::error() << to_string<const MemoryInfo*>(srcInfos) << " - " << srcInfo.to_string() << logging::endl;
            logging::error() << to_string<const MemoryInfo*>(destInfos) << " - " << destInfo.to_string()
                             << logging::endl;
            if(auto writer = mem->getNumEntries().getSingleWriter())
                logging::error() << writer->to_string() << logging::endl;
            throw CompilationError(CompilationStep::NORMALIZER, "Needs to be re-written", mem->to_string());
        }
    }
    else
    {
        LCOV_EXCL_START
        logging::error() << "Source: " << (srcInfo.local ? srcInfo.local->to_string() : "?") << " - "
                         << static_cast<unsigned>(srcInfo.type) << " - "
                         << (srcInfo.area ? srcInfo.area->to_string() : "") << logging::endl;

        logging::error() << "Destination: " << (destInfo.local ? destInfo.local->to_string() : "?") << " - "
                         << static_cast<unsigned>(destInfo.type) << " - "
                         << (destInfo.area ? destInfo.area->to_string() : "") << logging::endl;

        throw CompilationError(
            CompilationStep::NORMALIZER, "Unhandled case for handling memory copy", mem->to_string());
        LCOV_EXCL_STOP
    }
}
