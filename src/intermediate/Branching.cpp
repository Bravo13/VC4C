/*
 * Author: doe300
 *
 * See the file "LICENSE" for the full license governing this code.
 */

#include "../asm/BranchInstruction.h"
#include "IntermediateInstruction.h"
#include "log.h"

using namespace vc4c;
using namespace vc4c::intermediate;

BranchLabel::BranchLabel(const Local& label) : IntermediateInstruction(label.createReference())
{
    setArgument(0, label.createReference());
}

LCOV_EXCL_START
std::string BranchLabel::to_string() const
{
    return std::string("label: ") + getLabel()->name + createAdditionalInfoString();
}
LCOV_EXCL_STOP

IntermediateInstruction* BranchLabel::copyFor(
    Method& method, const std::string& localPrefix, InlineMapping& localMapping) const
{
    return new BranchLabel(*renameValue(method, assertArgument(0), localPrefix, localMapping).local());
}

qpu_asm::DecoratedInstruction BranchLabel::convertToAsm(const FastMap<const Local*, Register>& registerMapping,
    const FastMap<const Local*, std::size_t>& labelMapping, const std::size_t instructionIndex) const
{
    throw CompilationError(
        CompilationStep::CODE_GENERATION, "There should be no more labels at this point", to_string());
}

bool BranchLabel::mapsToASMInstruction() const
{
    return false;
}

bool BranchLabel::isNormalized() const
{
    return true;
}

const Local* BranchLabel::getLabel() const
{
    return assertArgument(0).local();
}

Local* BranchLabel::getLabel()
{
    return assertArgument(0).local();
}

bool BranchLabel::innerEquals(const IntermediateInstruction& other) const
{
    // no extra fields to check
    return dynamic_cast<const BranchLabel*>(&other);
}

Branch::Branch(const Local* target) : Branch(target, COND_ALWAYS, BOOL_TRUE)
{
}

Branch::Branch(const Local* target, const ConditionCode condCode, const Value& cond) :
    IntermediateInstruction({}, condCode)
{
    if(condCode != COND_ALWAYS && condCode != COND_ZERO_CLEAR && condCode != COND_ZERO_SET)
        // only allow always and comparison for zero, since branches only work on boolean values (0, 1)
        throw CompilationError(CompilationStep::GENERAL, "Invalid condition for branches", condCode.to_string());
    setArgument(0, target->createReference());
    setArgument(1, cond);
}

LCOV_EXCL_START
std::string Branch::to_string() const
{
    if(getCondition() == BOOL_TRUE)
    {
        return std::string("br ") + getTarget()->name + createAdditionalInfoString();
    }
    return std::string("br.") + (conditional.to_string() + " ") + getTarget()->name +
        (getCondition() != BOOL_TRUE ? std::string(" (on ") + getCondition().to_string(false, false) + ")" : "") +
        createAdditionalInfoString();
}
LCOV_EXCL_STOP

IntermediateInstruction* Branch::copyFor(
    Method& method, const std::string& localPrefix, InlineMapping& localMapping) const
{
    return (new Branch(renameValue(method, assertArgument(0), localPrefix, localMapping).local(), conditional,
                renameValue(method, getCondition(), localPrefix, localMapping)))
        ->setOutput(getOutput())
        ->copyExtrasFrom(this);
}

qpu_asm::DecoratedInstruction Branch::convertToAsm(const FastMap<const Local*, Register>& registerMapping,
    const FastMap<const Local*, std::size_t>& labelMapping, const std::size_t instructionIndex) const
{
    /*
     * We use relative targets, since the absolute address is completely absolute and not relative to the start of the
     * code-block. Since we do not know the start of the code-block (where the QPU code is loaded to), we use relative
     * addresses.
     *
     * Broadcom specification, page 34:
     * "branch target is relative to PC+4 (add PC+4 to target)"
     */
    auto labelPos = labelMapping.find(getTarget());
    if(labelPos == labelMapping.end())
        throw CompilationError(
            CompilationStep::CODE_GENERATION, "Target label not mapped to any position", to_string());
    const int64_t branchOffset = static_cast<int64_t>(labelPos->second) -
        static_cast<int64_t>(
            (instructionIndex + 4 /*  NOPs */) * sizeof(uint64_t)) /* convert instruction-index to byte-index */;
    BranchCond cond = BranchCond::ALWAYS;
    if(conditional != COND_ALWAYS && has_flag(decoration, InstructionDecorations::BRANCH_ON_ALL_ELEMENTS))
    {
        if(conditional == COND_ZERO_CLEAR)
            cond = BranchCond::ALL_Z_CLEAR;
        else if(conditional == COND_ZERO_SET)
            cond = BranchCond::ALL_Z_SET;
        else
            throw CompilationError(CompilationStep::CODE_GENERATION,
                "Unhandled branch condition depending on all elements", conditional.to_string());
    }
    else
        cond = conditional.toBranchCondition();
    if(branchOffset < static_cast<int64_t>(std::numeric_limits<int32_t>::min()) ||
        branchOffset > static_cast<int64_t>(std::numeric_limits<int32_t>::max()))
        throw CompilationError(CompilationStep::CODE_GENERATION,
            "Cannot jump a distance not fitting into 32-bit integer", std::to_string(branchOffset));
    return qpu_asm::DecoratedInstruction(
        qpu_asm::BranchInstruction(cond, BranchRel::BRANCH_RELATIVE, BranchReg::NONE,
            0 /* only 5 bits, so REG_NOP doesn't fit */, REG_NOP.num, REG_NOP.num, static_cast<int32_t>(branchOffset)),
        "to " + this->getTarget()->name);
}

bool Branch::isNormalized() const
{
    return true;
}

SideEffectType Branch::getSideEffects() const
{
    return add_flag(IntermediateInstruction::getSideEffects(), SideEffectType::BRANCH);
}

const Local* Branch::getTarget() const
{
    return assertArgument(0).local();
}

bool Branch::isUnconditional() const
{
    return conditional == COND_ALWAYS || getCondition() == BOOL_TRUE;
}

const Value& Branch::getCondition() const
{
    return assertArgument(1);
}

bool Branch::innerEquals(const IntermediateInstruction& other) const
{
    // no extra fields to check
    return dynamic_cast<const Branch*>(&other);
}

PhiNode::PhiNode(
    Value&& dest, std::vector<std::pair<Value, const Local*>>&& labelPairs, ConditionCode cond, SetFlag setFlags) :
    IntermediateInstruction(std::move(dest), cond, setFlags)
{
    for(std::size_t i = 0; i < labelPairs.size(); ++i)
    {
        setArgument(i * 2, labelPairs[i].second->createReference());
        setArgument(i * 2 + 1, std::move(labelPairs[i].first));
    }
}

LCOV_EXCL_START
std::string PhiNode::to_string() const
{
    std::string args;
    for(const auto& pair : getValuesForLabels())
        args.append(", ").append(pair.first->name).append(" -> ").append(pair.second.to_string());
    return (getOutput().to_string() + " = phi") + args.substr(1) + createAdditionalInfoString();
}

qpu_asm::DecoratedInstruction PhiNode::convertToAsm(const FastMap<const Local*, Register>& registerMapping,
    const FastMap<const Local*, std::size_t>& labelMapping, const std::size_t instructionIndex) const
{
    throw CompilationError(
        CompilationStep::CODE_GENERATION, "There should be no more phi-nodes at this point", to_string());
}
LCOV_EXCL_STOP

bool PhiNode::isNormalized() const
{
    return false;
}

IntermediateInstruction* PhiNode::copyFor(
    Method& method, const std::string& localPrefix, InlineMapping& localMapping) const
{
    IntermediateInstruction* tmp =
        (new PhiNode(renameValue(method, getOutput().value(), localPrefix, localMapping), {}, conditional, setFlags))
            ->copyExtrasFrom(this);
    for(std::size_t i = 0; i < getArguments().size(); ++i)
    {
        tmp->setArgument(i, renameValue(method, assertArgument(i), localPrefix, localMapping));
    }
    return tmp;
}

FastMap<const Local*, Value> PhiNode::getValuesForLabels() const
{
    FastMap<const Local*, Value> res;
    for(std::size_t i = 0; i < getArguments().size(); i += 2)
    {
        res.emplace(assertArgument(i).local(), assertArgument(i + 1));
    }
    return res;
}

bool PhiNode::innerEquals(const IntermediateInstruction& other) const
{
    // no extra fields to check
    return dynamic_cast<const PhiNode*>(&other);
}
