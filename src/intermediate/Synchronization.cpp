/*
 * Author: doe300
 *
 * See the file "LICENSE" for the full license governing this code.
 */

#include "../asm/SemaphoreInstruction.h"
#include "IntermediateInstruction.h"

using namespace vc4c;
using namespace vc4c::intermediate;

SemaphoreAdjustment::SemaphoreAdjustment(Semaphore semaphore, bool increase) :
    ExtendedInstruction(SIGNAL_SEMAPHORE, COND_ALWAYS, SetFlag::DONT_SET, PACK_NOP), semaphore(semaphore),
    increase(increase)
{
}

LCOV_EXCL_START
std::string SemaphoreAdjustment::to_string() const
{
    return std::string("semaphore ") + (std::to_string(static_cast<unsigned>(semaphore)) + " ") +
        (increase ? "increase" : "decrease") + createAdditionalInfoString();
}
LCOV_EXCL_STOP

qpu_asm::DecoratedInstruction SemaphoreAdjustment::convertToAsm(const FastMap<const Local*, Register>& registerMapping,
    const FastMap<const Local*, std::size_t>& labelMapping, std::size_t instructionIndex) const
{
    if(conditional != COND_ALWAYS)
        throw CompilationError(CompilationStep::CODE_GENERATION,
            "Condition codes have no effect on whether the semaphore is adjusted", to_string());

    const Register outReg = getOutput() ?
        (getOutput()->checkLocal() ? registerMapping.at(getOutput()->local()) : getOutput()->reg()) :
        REG_NOP;
    return qpu_asm::SemaphoreInstruction(PACK_NOP, COND_ALWAYS, COND_ALWAYS, setFlags,
        outReg.file == RegisterFile::PHYSICAL_B ? WriteSwap::SWAP : WriteSwap::DONT_SWAP, outReg.num, outReg.num,
        increase, semaphore);
}

bool SemaphoreAdjustment::isNormalized() const
{
    return true;
}

SideEffectType SemaphoreAdjustment::getSideEffects() const
{
    return add_flag(IntermediateInstruction::getSideEffects(), SideEffectType::SEMAPHORE);
}

IntermediateInstruction* SemaphoreAdjustment::copyFor(
    Method& method, const std::string& localPrefix, InlineMapping& localMapping) const
{
    return (new SemaphoreAdjustment(semaphore, increase))->copyExtrasFrom(this)->setOutput(getOutput());
}

bool SemaphoreAdjustment::innerEquals(const IntermediateInstruction& other) const
{
    if(auto otherSema = dynamic_cast<const SemaphoreAdjustment*>(&other))
        return semaphore == otherSema->semaphore && increase == otherSema->increase;
    return false;
}

MemoryBarrier::MemoryBarrier(MemoryScope scope, MemorySemantics semantics) :
    IntermediateInstruction(Optional<Value>{}), scope(scope), semantics(semantics)
{
}

LCOV_EXCL_START
static std::string toString(MemoryScope scope)
{
    switch(scope)
    {
    case MemoryScope::CROSS_DEVICE:
        return "global";
    case MemoryScope::DEVICE:
        return "device";
    case MemoryScope::SUB_GROUP:
        return "sub-group";
    case MemoryScope::WORK_GROUP:
        return "work-group";
    case MemoryScope::INVOCATION:
        return "invocation";
    }
    throw CompilationError(
        CompilationStep::GENERAL, "Unsupported memory scope value", std::to_string(static_cast<int>(scope)));
}

static std::string toString(MemorySemantics semantics)
{
    std::vector<std::string> result;
    if(has_flag(semantics, MemorySemantics::ACQUIRE) || has_flag(semantics, MemorySemantics::ACQUIRE_RELEASE))
        result.emplace_back("acquire");
    if(has_flag(semantics, MemorySemantics::RELEASE) || has_flag(semantics, MemorySemantics::ACQUIRE_RELEASE))
        result.emplace_back("release");
    if(has_flag(semantics, MemorySemantics::SEQUENTIALLY_CONSISTENT))
        result.emplace_back("sequentially consistent");
    if(has_flag(semantics, MemorySemantics::SUBGROUP_MEMORY))
        result.emplace_back("sub-group");
    if(has_flag(semantics, MemorySemantics::WORK_GROUP_MEMORY))
        result.emplace_back("work-group");
    if(has_flag(semantics, MemorySemantics::CROSS_WORK_GROUP_MEMORY))
        result.emplace_back("global");
    if(has_flag(semantics, MemorySemantics::ATOMIC_COUNTER_MEMORY))
        result.emplace_back("atomic counter");
    if(has_flag(semantics, MemorySemantics::IMAGE_MEMORY))
        result.emplace_back("image");
    return vc4c::to_string<std::string>(result, "|");
}

std::string MemoryBarrier::to_string() const
{
    return std::string("mem-fence ") + (::toString(scope) + ", ") + ::toString(semantics) +
        createAdditionalInfoString();
}

qpu_asm::DecoratedInstruction MemoryBarrier::convertToAsm(const FastMap<const Local*, Register>& registerMapping,
    const FastMap<const Local*, std::size_t>& labelMapping, std::size_t instructionIndex) const
{
    throw CompilationError(
        CompilationStep::CODE_GENERATION, "There should be no more memory barriers at this point", to_string());
}
LCOV_EXCL_STOP

bool MemoryBarrier::isNormalized() const
{
    return true;
}

IntermediateInstruction* MemoryBarrier::copyFor(
    Method& method, const std::string& localPrefix, InlineMapping& localMapping) const
{
    return (new MemoryBarrier(scope, semantics))->copyExtrasFrom(this);
}

bool MemoryBarrier::mapsToASMInstruction() const
{
    return false;
}

bool MemoryBarrier::innerEquals(const IntermediateInstruction& other) const
{
    if(auto otherBarrier = dynamic_cast<const MemoryBarrier*>(&other))
        return scope == otherBarrier->scope && semantics == otherBarrier->semantics;
    return false;
}

LifetimeBoundary::LifetimeBoundary(const Value& allocation, bool lifetimeEnd) :
    IntermediateInstruction(Optional<Value>{}), isLifetimeEnd(lifetimeEnd)
{
    if(!allocation.checkLocal() || !allocation.local()->is<StackAllocation>())
        throw CompilationError(CompilationStep::LLVM_2_IR, "Cannot control life-time of object not located on stack",
            allocation.to_string());

    setArgument(0, allocation);
}

LCOV_EXCL_START
std::string LifetimeBoundary::to_string() const
{
    return std::string("life-time for ") + getStackAllocation().to_string() + (isLifetimeEnd ? " ends" : " starts");
}

qpu_asm::DecoratedInstruction LifetimeBoundary::convertToAsm(const FastMap<const Local*, Register>& registerMapping,
    const FastMap<const Local*, std::size_t>& labelMapping, std::size_t instructionIndex) const
{
    throw CompilationError(
        CompilationStep::CODE_GENERATION, "There should be no more lifetime instructions at this point", to_string());
}
LCOV_EXCL_STOP

bool LifetimeBoundary::isNormalized() const
{
    return true;
}

IntermediateInstruction* LifetimeBoundary::copyFor(
    Method& method, const std::string& localPrefix, InlineMapping& localMapping) const
{
    return (new LifetimeBoundary(renameValue(method, getStackAllocation(), localPrefix, localMapping), isLifetimeEnd))
        ->copyExtrasFrom(this);
}

bool LifetimeBoundary::mapsToASMInstruction() const
{
    return false;
}

const Value& LifetimeBoundary::getStackAllocation() const
{
    return assertArgument(0);
}

bool LifetimeBoundary::innerEquals(const IntermediateInstruction& other) const
{
    if(auto otherBound = dynamic_cast<const LifetimeBoundary*>(&other))
        return isLifetimeEnd == otherBound->isLifetimeEnd;
    return false;
}

static const Value MUTEX_REGISTER(REG_MUTEX, TYPE_BOOL);

MutexLock::MutexLock(MutexAccess accessType) : SignalingInstruction(SIGNAL_NONE), accessType(accessType)
{
    if(locksMutex())
        setArgument(0, MUTEX_REGISTER);
    else
        setOutput(MUTEX_REGISTER);
}

LCOV_EXCL_START
std::string MutexLock::to_string() const
{
    return REG_MUTEX.to_string(true, locksMutex());
}
LCOV_EXCL_STOP

qpu_asm::DecoratedInstruction MutexLock::convertToAsm(const FastMap<const Local*, Register>& registerMapping,
    const FastMap<const Local*, std::size_t>& labelMapping, std::size_t instructionIndex) const
{
    MoveOperation move(locksMutex() ? NOP_REGISTER : MUTEX_REGISTER,
        locksMutex() ? MUTEX_REGISTER : Value(SmallImmediate(1), TYPE_BOOL));
    return move.convertToAsm(registerMapping, labelMapping, instructionIndex);
}

bool MutexLock::isNormalized() const
{
    return true;
}

SideEffectType MutexLock::getSideEffects() const
{
    return add_flag(IntermediateInstruction::getSideEffects(),
        locksMutex() ? SideEffectType::REGISTER_READ : SideEffectType::REGISTER_WRITE);
}

IntermediateInstruction* MutexLock::copyFor(
    Method& method, const std::string& localPrefix, InlineMapping& localMapping) const
{
    return new MutexLock(accessType);
}

bool MutexLock::locksMutex() const
{
    return accessType == MutexAccess::LOCK;
}

bool MutexLock::releasesMutex() const
{
    return accessType == MutexAccess::RELEASE;
}

bool MutexLock::innerEquals(const IntermediateInstruction& other) const
{
    if(auto otherLock = dynamic_cast<const MutexLock*>(&other))
        return accessType == otherLock->accessType;
    return false;
}
