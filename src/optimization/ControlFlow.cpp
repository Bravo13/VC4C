/*
 * Author: doe300
 *
 * See the file "LICENSE" for the full license governing this code.
 */

#include "ControlFlow.h"

#include "../InstructionWalker.h"
#include "../Profiler.h"
#include "../analysis/ControlFlowGraph.h"
#include "../analysis/DataDependencyGraph.h"
#include "../intermediate/Helper.h"
#include "../intermediate/TypeConversions.h"
#include "../intermediate/operators.h"
#include "../normalization/LiteralValues.h"
#include "../periphery/VPM.h"
#include "./Combiner.h"
#include "log.h"

#include <algorithm>
#include <cmath>
#include <numeric>

using namespace vc4c;
using namespace vc4c::optimizations;
using namespace vc4c::intermediate;
using namespace vc4c::operators;

static FastSet<Local*> findLoopIterations(const ControlFlowLoop& loop, const DataDependencyGraph& dependencyGraph)
{
    FastSet<Local*> innerDependencies;
    FastSet<Local*> outerDependencies;
    for(auto& node : loop)
    {
        // not all basic blocks have an entry in the dependency graph (e.g. if they have no dependency)
        if(auto dependencyNode = dependencyGraph.findNode(node->key))
        {
            // TODO is checking for only incoming edges correct?
            dependencyNode->forAllIncomingEdges(
                [&](const DataDependencyNode& neighbor, const DataDependencyEdge& edge) -> bool {
                    // check if this basic block has a local dependent on at least two phi-nodes
                    for(auto& dependency : edge.data)
                    {
                        if(has_flag(dependency.second, add_flag(DataDependencyType::PHI, DataDependencyType::FLOW)))
                        {
                            if(std::find_if(loop.begin(), loop.end(), [&neighbor](const CFGNode* node) -> bool {
                                   return node->key == neighbor.key;
                               }) != loop.end())
                                //... one of which lies within the loop
                                innerDependencies.emplace(dependency.first);
                            else
                                //... and the other outside of it
                                outerDependencies.emplace(dependency.first);
                        }
                    }

                    return true;
                });
        }
    }

    FastSet<Local*> intersection;
    std::set_intersection(innerDependencies.begin(), innerDependencies.end(), outerDependencies.begin(),
        outerDependencies.end(), std::inserter(intersection, intersection.begin()));

    if(intersection.empty())
    {
        CPPLOG_LAZY(logging::Level::DEBUG, log << "Failed to find loop iteration variable for loop" << logging::endl);
    }

    return intersection;
}

enum class StepKind : unsigned char
{
    // step-kind is not known
    UNKNOWN,
    // integer addition with constant factor, e.g. step of +1. Default for more for-range loops
    ADD_CONSTANT,
    // integer subtraction with constant factor e.g. step of -1. Default for loops counting backwards
    SUB_CONSTANT,
    // integer multiplication with constant factor
    MUL_CONSTANT
};

struct LoopControl
{
    // the initial value for the loop iteration variable
    intermediate::IntermediateInstruction* initialization = nullptr;
    // the value compared with to terminate the loop
    Value terminatingValue = UNDEFINED_VALUE;
    // the local containing the current iteration-variable
    Local* iterationVariable = nullptr;
    // the operation to change the iteration-variable
    Optional<InstructionWalker> iterationStep{};
    // the kind of step performed
    StepKind stepKind = StepKind::UNKNOWN;
    // the comparison to check for continue/end loop
    Optional<InstructionWalker> comparisonInstruction{};
    // the branch-instruction to continue the loop
    Optional<InstructionWalker> repetitionJump{};
    // the comparison function to abort the loop
    std::string comparison{};
    // the vectorization-factor used
    unsigned vectorizationFactor = 0;

    void determineStepKind(const OpCode& code)
    {
        if(code == OP_ADD)
            stepKind = StepKind::ADD_CONSTANT;
        else if(code == OP_SUB)
            stepKind = StepKind::SUB_CONSTANT;
        else if(code == OP_MUL24)
            stepKind = StepKind::MUL_CONSTANT;
    }

    OpCode getStepOperation() const
    {
        switch(stepKind)
        {
        case StepKind::ADD_CONSTANT:
            return OP_ADD;
        case StepKind::SUB_CONSTANT:
            return OP_SUB;
        case StepKind::MUL_CONSTANT:
            return OP_MUL24;
        default:
            throw CompilationError(CompilationStep::OPTIMIZER, "Operation for this step-kind is not yet mapped!");
        }
    }

    Optional<Literal> getStep() const
    {
        if(!iterationStep.ifPresent(
               [](const InstructionWalker& it) -> bool { return it.get<const intermediate::Operation>(); }))
            return {};
        const intermediate::Operation* op = iterationStep->get<const intermediate::Operation>();
        if(op->getArguments().size() != 2)
            return {};
        if(op->getArgument(0) && op->assertArgument(0).isLiteralValue())
            return op->assertArgument(0).getLiteralValue();
        return op->assertArgument(1).getLiteralValue();
    }

    int32_t countIterations(int32_t initial, int32_t limit, int32_t step, const std::string& comparisonType) const
    {
        // TODO this is not always true (e.g. true for test_vectorization.cl#test5, not true for #test11
        if(comparisonType == intermediate::COMP_EQ)
            // we compare up to including the limit
            limit += 1;
        else if(comparisonType == "lt")
            // we compare up to excluding the limit
            limit += 0;
        else
            throw CompilationError(CompilationStep::OPTIMIZER, "Unhandled comparison type", comparisonType);
        switch(stepKind)
        {
        case StepKind::ADD_CONSTANT:
            // iterations = (end - start) / step
            return (limit - initial) / step;
        case StepKind::SUB_CONSTANT:
            // iterations = (start - end) / step
            return (initial - limit) / step;
        case StepKind::MUL_CONSTANT:
            // limit = (start * step) ^ iterations -> iterations = log(start * step) / log(limit)
            return static_cast<int32_t>(std::log(initial * step) / std::log(limit));
        default:
            throw CompilationError(CompilationStep::OPTIMIZER, "Invalid step type!");
        }
    }

    bool operator==(const LoopControl& other) const
    {
        return iterationVariable == other.iterationVariable;
    }
};

struct LoopControlHash : public std::hash<Local*>
{
    size_t operator()(const LoopControl& val) const noexcept
    {
        return std::hash<Local*>::operator()(val.iterationVariable);
    }
};

static LoopControl extractLoopControl(const ControlFlowLoop& loop, const DataDependencyGraph& dependencyGraph)
{
    FastSet<LoopControl, LoopControlHash> availableLoopControls;

    for(Local* local : findLoopIterations(loop, dependencyGraph))
    {
        if(local == nullptr)
            continue;

        CPPLOG_LAZY(logging::Level::DEBUG,
            log << "Loop iteration variable candidate: " << local->to_string(false) << logging::endl);

        LoopControl loopControl;
        loopControl.iterationVariable = local;

        for(const auto& pair : local->getUsers())
        {
            const intermediate::IntermediateInstruction* inst = pair.first;
            Optional<InstructionWalker> it = loop.findInLoop(inst);
            //"lower" bound: the initial setting of the value outside of the loop
            if(pair.second.writesLocal() && inst->hasDecoration(intermediate::InstructionDecorations::PHI_NODE) && !it)
            {
                auto tmp = inst->precalculate(4).first;
                if(tmp && tmp->isLiteralValue())
                {
                    CPPLOG_LAZY(
                        logging::Level::DEBUG, log << "Found lower bound: " << tmp->to_string() << logging::endl);
                    loopControl.initialization = const_cast<intermediate::IntermediateInstruction*>(inst);
                }
            }
            // iteration step: the instruction inside the loop where the iteration variable is changed
            // XXX this currently only looks for single operations with immediate values (e.g. +1,-1)
            else if(pair.second.readsLocal() && it)
            {
                if(it->get<intermediate::Operation>() && it.value()->getArguments().size() == 2 &&
                    it.value()->readsLiteral() &&
                    // TODO could here more simply check against output being the local the iteration variable is set to
                    // (in the phi-node inside the loop)
                    it.value()->getOutput().ifPresent([](const Value& val) -> bool {
                        return val.checkLocal() &&
                            std::any_of(val.local()->getUsers().begin(), val.local()->getUsers().end(),
                                [](const auto& pair) -> bool {
                                    return pair.first->hasDecoration(intermediate::InstructionDecorations::PHI_NODE);
                                });
                    }))
                {
                    CPPLOG_LAZY(logging::Level::DEBUG,
                        log << "Found iteration instruction: " << it.value()->to_string() << logging::endl);
                    loopControl.iterationStep = it;
                    loopControl.determineStepKind(it->get<intermediate::Operation>()->op);
                }
                // for use-with immediate local, TODO need better checking
                else if(it->get<intermediate::MoveOperation>() && it.value()->hasValueType(ValueType::LOCAL))
                {
                    // second-level checking for loop iteration step (e.g. if loop variable is copied for
                    // use-with-immediate)
                    const Local* stepLocal = it.value()->getOutput()->local();
                    for(const auto& pair : stepLocal->getUsers())
                    {
                        const intermediate::IntermediateInstruction* inst = pair.first;
                        Optional<InstructionWalker> it = loop.findInLoop(inst);
                        // iteration step: the instruction inside the loop where the iteration variable is changed
                        if(pair.second.readsLocal() && it)
                        {
                            if(it->get<intermediate::Operation>() && it.value()->getArguments().size() == 2 &&
                                it.value()->readsLiteral() &&
                                it.value()->getOutput().ifPresent([](const Value& val) -> bool {
                                    return val.checkLocal() &&
                                        std::any_of(val.local()->getUsers().begin(), val.local()->getUsers().end(),
                                            [](const auto& pair) -> bool {
                                                return pair.first->hasDecoration(
                                                    intermediate::InstructionDecorations::PHI_NODE);
                                            });
                                }))
                            {
                                CPPLOG_LAZY(logging::Level::DEBUG,
                                    log << "Found iteration instruction: " << it.value()->to_string() << logging::endl);
                                loopControl.iterationStep = it;
                                loopControl.determineStepKind(it->get<intermediate::Operation>()->op);
                            }
                        }
                    };
                }
            }
        };

        loop.front()->forAllOutgoingEdges([&](const CFGNode& neighbor, const CFGEdge& edge) -> bool {
            if(!edge.data.isImplicit(loop.front()->key))
            {
                if(std::find(loop.begin(), loop.end(), &neighbor) != loop.end())
                {
                    // FIXME is this correct?
                    loopControl.repetitionJump = edge.data.getPredecessor(loop.front()->key);
                    CPPLOG_LAZY(logging::Level::DEBUG,
                        log << "Found loop repetition branch: " << loopControl.repetitionJump.value()->to_string()
                            << logging::endl);
                }
            }
            return true;
        });

        //"upper" bound: the value being checked against inside the loop
        if(loopControl.repetitionJump && loopControl.iterationStep)
        {
            const Value repeatCond = loopControl.repetitionJump->get<intermediate::Branch>()->getCondition();
            const Value iterationStep = loopControl.iterationStep.value()->getOutput().value();

            // check for either local (iteration-variable or iteration-step result) whether they are used in the
            // condition on which the loop is repeated  and select the literal used together with in this condition

            // simple case, there exists an instruction, directly mapping the values
            auto userIt =
                std::find_if(iterationStep.local()->getUsers().begin(), iterationStep.local()->getUsers().end(),
                    [&repeatCond](const auto& pair) -> bool { return pair.first->writesLocal(repeatCond.local()); });
            if(userIt == iterationStep.local()->getUsers().end())
            {
                //"default" case, the iteration-variable is compared to something and the result of this comparison is
                // used to branch  e.g. "- = xor <iteration-variable>, <upper-bound> (setf)"
                userIt =
                    std::find_if(iterationStep.local()->getUsers().begin(), iterationStep.local()->getUsers().end(),
                        [](const auto& pair) -> bool { return pair.first->setFlags == SetFlag::SET_FLAGS; });
                if(userIt != iterationStep.local()->getUsers().end())
                {
                    // TODO need to check, whether the comparison result is the one used for branching
                    // if not, set userIt to loop.end()
                    auto instIt = loop.findInLoop(userIt->first);
                    loopControl.comparisonInstruction = instIt;
                    CPPLOG_LAZY(logging::Level::DEBUG,
                        log << "Found loop continue condition: "
                            << loopControl.comparisonInstruction.value()->to_string() << logging::endl);
                }
                else
                {
                    // TODO more complex case, the iteration-variable is used in an operation, whose result is compared
                    // to something and that result is used to branch, e.g:
                    // <tmp> = max <iteration-variable>, <upper-bound>
                    // - = xor <tmp>, <upper-bound> (setf)
                    // this also applies for unsigned less than for 32-bit integers
                }
            }

            if(userIt != iterationStep.local()->getUsers().end())
            {
                // userIt converts the loop-variable to the condition. The comparison value is the upper bound
                const intermediate::IntermediateInstruction* inst = userIt->first;
                if(inst->getArguments().size() != 2)
                {
                    // TODO error
                }
                if(inst->assertArgument(0).hasLocal(iterationStep.local()))
                    loopControl.terminatingValue = inst->assertArgument(1);
                else
                    loopControl.terminatingValue = inst->assertArgument(0);
                if(loopControl.terminatingValue.getSingleWriter() != nullptr)
                {
                    if(auto tmp = loopControl.terminatingValue.getSingleWriter()->precalculate(4).first)
                        loopControl.terminatingValue = tmp.value();
                    else
                    {
                        auto writer = loopControl.terminatingValue.getSingleWriter();
                        if(writer->readsLocal(iterationStep.local()))
                        {
                            for(const auto& arg : writer->getArguments())
                            {
                                if(!arg.hasLocal(iterationStep.local()))
                                {
                                    auto precalc =
                                        arg.getSingleWriter() ? arg.getSingleWriter()->precalculate(4).first : NO_VALUE;
                                    loopControl.terminatingValue = precalc ? precalc.value() : arg;
                                    break;
                                }
                            }
                        }
                    }
                }
                CPPLOG_LAZY(logging::Level::DEBUG,
                    log << "Found upper bound: " << loopControl.terminatingValue.to_string() << logging::endl);

                // determine type of comparison
                if(auto comparison = dynamic_cast<const intermediate::Operation*>(inst))
                {
                    bool isEqualityComparison = comparison->op == OP_XOR;
                    bool isLessThenComparison = comparison->op == OP_SUB || comparison->op == OP_FSUB;
                    // TODO distinguish ==/!=, </>/<=/>= !! The setting of flags as well as the reading (for branch) can
                    // be for positive/negative flags
                    // XXX need to distinguish between continuation condition and cancel condition
                    if(isEqualityComparison)
                        loopControl.comparison = intermediate::COMP_EQ;
                    if(isLessThenComparison)
                        loopControl.comparison = "lt";
                    if(!loopControl.comparison.empty())
                        CPPLOG_LAZY(logging::Level::DEBUG,
                            log << "Found comparison type: " << loopControl.comparison << logging::endl);
                }
            }
        }

        if(loopControl.initialization && !loopControl.terminatingValue.isUndefined() && loopControl.iterationStep &&
            loopControl.repetitionJump)
        {
            availableLoopControls.emplace(loopControl);
        }
        else
            CPPLOG_LAZY(logging::Level::DEBUG,
                log << "Failed to find all bounds and step for iteration variable, skipping: "
                    << loopControl.iterationVariable->name << logging::endl);
    }

    if(availableLoopControls.empty())
        return LoopControl{};
    else if(availableLoopControls.size() == 1)
        return *availableLoopControls.begin();

    throw CompilationError(
        CompilationStep::OPTIMIZER, "Selecting from multiple iteration variables is not supported yet!");
}

/*
 * For now uses a very simple algorithm:
 * - checks the maximum vector-width used inside the loop
 * - tries to find an optimal factor, which never exceeds 16 elements and divides the number of iterations equally
 */
static Optional<unsigned> determineVectorizationFactor(const ControlFlowLoop& loop, const LoopControl& loopControl)
{
    unsigned char maxTypeWidth = 1;
    InstructionWalker it = loop.front()->key->walk();
    while(!it.isEndOfMethod() && it != loop.back()->key->walkEnd())
    {
        if(it->getOutput())
        {
            // TODO is this check enough?
            maxTypeWidth = std::max(maxTypeWidth, it->getOutput()->type.getVectorWidth());
        }
        it.nextInMethod();
    }

    CPPLOG_LAZY(logging::Level::DEBUG,
        log << "Found maximum used vector-width of " << static_cast<unsigned>(maxTypeWidth) << " elements"
            << logging::endl);

    const Literal initial = loopControl.initialization->precalculate(4).first->getLiteralValue().value();
    // TODO for test_vectorization.cl#test5 this calculates an iteration count of 1023 (instead of 1024)
    const Literal end = loopControl.terminatingValue.getLiteralValue().value();
    // the number of iterations from the bounds depends on the iteration operation
    auto iterations = loopControl.countIterations(
        initial.signedInt(), end.signedInt(), loopControl.getStep()->signedInt(), loopControl.comparison);
    CPPLOG_LAZY(logging::Level::DEBUG, log << "Determined iteration count of " << iterations << logging::endl);

    // find the biggest factor fitting into 16 SIMD-elements
    unsigned factor = 16 / maxTypeWidth;
    while(factor > 0)
    {
        // TODO factors not in [1,2,3,4,8,16] possible?? Should be from hardware-specification side
        if((iterations % static_cast<int32_t>(factor)) == 0)
            break;
        --factor;
    }
    CPPLOG_LAZY(
        logging::Level::DEBUG, log << "Determined possible vectorization-factor of " << factor << logging::endl);
    return factor;
}

/*
 * On the cost-side, we have (as increments):
 * - instructions inserted to construct vectors from scalars
 * - additional delay for writing larger vectors through VPM
 * - memory address is read and written from within loop -> abort
 * - vector rotations -> for now abort
 *
 * On the benefit-side, we have (as factors):
 * - the iterations saved (times the number of instructions in an iteration)
 */
static int calculateCostsVsBenefits(
    const ControlFlowLoop& loop, const LoopControl& loopControl, const DataDependencyGraph& dependencyGraph)
{
    int costs = 0;

    FastSet<const Local*> readAddresses;
    FastSet<const Local*> writtenAddresses;

    InstructionWalker it = loop.front()->key->walk();
    while(!it.isEndOfMethod() && it != loop.back()->key->walkEnd())
    {
        if(it.has())
        {
            if(it->getOutput().ifPresent([](const Value& out) -> bool {
                   return out.hasRegister(REG_VPM_DMA_LOAD_ADDR) || out.hasRegister(REG_TMU0_ADDRESS) ||
                       out.hasRegister(REG_TMU1_ADDRESS);
               }))
            {
                for(const Value& arg : it->getArguments())
                {
                    if(auto loc = arg.checkLocal())
                    {
                        readAddresses.emplace(loc);
                        readAddresses.emplace(loc->reference.first);
                    }
                }
            }
            else if(it->getOutput() && it->getOutput()->hasRegister(REG_VPM_DMA_STORE_ADDR))
            {
                for(const Value& arg : it->getArguments())
                {
                    if(auto loc = arg.checkLocal())
                    {
                        writtenAddresses.emplace(loc);
                        writtenAddresses.emplace(loc->reference.first);
                    }
                }
            }
            else if(it.get<intermediate::VectorRotation>())
            {
                // abort
                CPPLOG_LAZY(logging::Level::DEBUG,
                    log << "Cannot vectorize loops containing vector rotations: " << it->to_string() << logging::endl);
                return std::numeric_limits<int>::min();
            }
            else if(it.get<intermediate::MemoryBarrier>())
            {
                // abort
                CPPLOG_LAZY(logging::Level::DEBUG,
                    log << "Cannot vectorize loops containing memory barriers: " << it->to_string() << logging::endl);
                return std::numeric_limits<int>::min();
            }
            else if(it.get<intermediate::SemaphoreAdjustment>())
            {
                // abort
                CPPLOG_LAZY(logging::Level::DEBUG,
                    log << "Cannot vectorize loops containing semaphore calls: " << it->to_string() << logging::endl);
                return std::numeric_limits<int>::min();
            }
        }

        // TODO check and increase costs
        it.nextInMethod();
    }

    // constant cost - loading immediate for iteration-step for vector-width > 15 (no longer fitting into small
    // immediate)
    if(loopControl.iterationStep.value()->getOutput()->type.getVectorWidth() * loopControl.vectorizationFactor > 15)
        ++costs;

    FastSet<const Local*> readAndWrittenAddresses;
    std::set_intersection(readAddresses.begin(), readAddresses.end(), writtenAddresses.begin(), writtenAddresses.end(),
        std::inserter(readAndWrittenAddresses, readAndWrittenAddresses.begin()));
    // the references could be null-pointers
    readAndWrittenAddresses.erase(nullptr);
    if(!readAndWrittenAddresses.empty())
    {
        for(const Local* local : readAndWrittenAddresses)
        {
            CPPLOG_LAZY(logging::Level::DEBUG,
                log << "Cannot vectorize loops reading and writing the same memory addresses: " << local->to_string()
                    << logging::endl);
        }
        // abort
        return std::numeric_limits<int>::min();
    }

    int numInstructions = 0;
    for(const CFGNode* node : loop)
    {
        // XXX to be exact, would need to include delays here too
        numInstructions += static_cast<int>(node->key->size());
    }
    // the number of instructions/cycles saved
    int benefits = numInstructions * static_cast<int>(loopControl.vectorizationFactor);

    CPPLOG_LAZY(logging::Level::DEBUG,
        log << "Calculated an cost-vs-benefit rating of " << (benefits - costs)
            << " (estimated number of clock cycles saved, larger is better)" << logging::endl);
    return benefits - costs;
}

static void scheduleForVectorization(
    const Local* local, FastSet<const intermediate::IntermediateInstruction*>& openInstructions, ControlFlowLoop& loop)
{
    local->forUsers(LocalUse::Type::READER, [&openInstructions, &loop](const LocalUser* user) -> void {
        if(!user->hasDecoration(intermediate::InstructionDecorations::AUTO_VECTORIZED))
            openInstructions.emplace(user);
        if(user->getOutput().ifPresent([](const Value& out) -> bool {
               return out.checkRegister() && (out.reg().isSpecialFunctionsUnit() || out.reg().isTextureMemoryUnit());
           }))
        {
            // need to add the reading of SFU/TMU too
            if(auto optIt = loop.findInLoop(user))
            {
                InstructionWalker it = optIt.value().nextInBlock();
                while(!it.isEndOfBlock())
                {
                    if(it->readsRegister(REG_SFU_OUT) &&
                        !it->hasDecoration(intermediate::InstructionDecorations::AUTO_VECTORIZED))
                    {
                        openInstructions.emplace(it.get());
                        break;
                    }

                    it.nextInBlock();
                }
            }
        }
    });
}

static void vectorizeInstruction(InstructionWalker it,
    FastSet<const intermediate::IntermediateInstruction*>& openInstructions, unsigned vectorizationFactor,
    ControlFlowLoop& loop)
{
    CPPLOG_LAZY(logging::Level::DEBUG, log << "Vectorizing instruction: " << it->to_string() << logging::endl);

    // 1. update types of values matching the types of their locals
    unsigned char vectorWidth = 1;
    for(auto& arg : it->getArguments())
    {
        if(arg.checkLocal() && arg.type != arg.local()->type)
        {
            scheduleForVectorization(arg.local(), openInstructions, loop);
            const_cast<DataType&>(arg.type) = arg.type.toVectorType(arg.local()->type.getVectorWidth());
            vectorWidth = std::max(vectorWidth, arg.type.getVectorWidth());
        }
        else if(arg.checkRegister())
        {
            // TODO correct?? This is at least required for reading from TMU
            vectorWidth = static_cast<unsigned char>(vectorizationFactor);
        }
    }

    // 2. depending on operation performed, update type of output
    if(it->getOutput() && (it.get<intermediate::Operation>() || it.get<intermediate::MoveOperation>()))
    {
        // TODO vector-rotations need special handling?!
        Value& out = const_cast<Value&>(it->getOutput().value());
        if(auto ptrType = out.type.getPointerType())
            // TODO this is only correct if the elements are located in one block (base+0, base+1, base+2...). Is this
            // guaranteed?
            out.type = it.getBasicBlock()->getMethod().createPointerType(
                ptrType->elementType.toVectorType(vectorWidth), ptrType->addressSpace);
        else
            out.type = out.type.toVectorType(vectorWidth);
        if(auto loc = out.checkLocal())
        {
            if(auto ptrType = loc->type.getPointerType())
                // TODO see above
                const_cast<DataType&>(loc->type) = it.getBasicBlock()->getMethod().createPointerType(
                    loc->type.getPointerType()->elementType.toVectorType(out.type.getVectorWidth()),
                    ptrType->addressSpace);
            else
                const_cast<DataType&>(loc->type) = loc->type.toVectorType(out.type.getVectorWidth());
            scheduleForVectorization(loc, openInstructions, loop);
        }
    }

    // TODO need to adapt types of some registers/output of load, etc.?
    // TODO cosmetic errors: depending on the order of vectorization, some locals are written as vectors, but read as
    // scalars, if the read-instruction was vectorized before the write-instruction

    // mark as already processed and remove from open-set
    it->addDecorations(intermediate::InstructionDecorations::AUTO_VECTORIZED);
    openInstructions.erase(it.get());
}

static std::size_t fixVPMSetups(ControlFlowLoop& loop, LoopControl& loopControl)
{
    InstructionWalker it = loop.front()->key->walk();
    std::size_t numVectorized = 0;

    while(!it.isEndOfMethod() && it != loop.back()->key->walkEnd())
    {
        if(it->writesRegister(REG_VPM_OUT_SETUP))
        {
            periphery::VPWSetupWrapper vpwSetup(it.get<intermediate::LoadImmediate>());
            auto vpmWrite = periphery::findRelatedVPMInstructions(it, false).vpmAccess;
            if(vpwSetup.isDMASetup() && vpmWrite &&
                (*vpmWrite)->hasDecoration(intermediate::InstructionDecorations::AUTO_VECTORIZED))
            {
                // Since this is only true for values actually vectorized, the corresponding VPM-write is checked
                vpwSetup.dmaSetup.setDepth(
                    static_cast<uint8_t>(vpwSetup.dmaSetup.getDepth() * loopControl.vectorizationFactor));
                ++numVectorized;
                it->addDecorations(intermediate::InstructionDecorations::AUTO_VECTORIZED);
            }
        }
        else if(it->writesRegister(REG_VPM_IN_SETUP))
        {
            periphery::VPRSetupWrapper vprSetup(it.get<intermediate::LoadImmediate>());
            auto vpmRead = periphery::findRelatedVPMInstructions(it, true).vpmAccess;
            if(vprSetup.isDMASetup() && vpmRead &&
                (*vpmRead)->hasDecoration(intermediate::InstructionDecorations::AUTO_VECTORIZED))
            {
                // See VPM write
                vprSetup.dmaSetup.setRowLength(
                    (vprSetup.dmaSetup.getRowLength() * loopControl.vectorizationFactor) % 16 /* 0 => 16 */);
                ++numVectorized;
                it->addDecorations(intermediate::InstructionDecorations::AUTO_VECTORIZED);
            }
        }

        it.nextInMethod();
    }

    return numVectorized;
}

/*
 * Makes sure, the predecessor-node and the instruction-walker are found in correct order
 */
static Optional<InstructionWalker> findWalker(const CFGNode* node, const intermediate::IntermediateInstruction* inst)
{
    return node != nullptr ? node->key->findWalkerForInstruction(inst, node->key->walkEnd()) :
                             Optional<InstructionWalker>{};
}

static void fixInitialValueAndStep(ControlFlowLoop& loop, LoopControl& loopControl)
{
    intermediate::Operation* stepOp = loopControl.iterationStep->get<intermediate::Operation>();
    if(stepOp == nullptr)
        throw CompilationError(CompilationStep::OPTIMIZER, "Unhandled iteration step operation");

    const_cast<DataType&>(loopControl.initialization->getOutput()->type) =
        loopControl.initialization->getOutput()->type.toVectorType(
            loopControl.iterationVariable->type.getVectorWidth());
    intermediate::MoveOperation* move = dynamic_cast<intermediate::MoveOperation*>(loopControl.initialization);
    Optional<InstructionWalker> initialValueWalker;
    if(move != nullptr && move->getSource().hasLiteral(INT_ZERO.literal()) &&
        loopControl.stepKind == StepKind::ADD_CONSTANT && loopControl.getStep() == INT_ONE.literal())
    {
        // special/default case: initial value is zero and step is +1
        move->setSource(Value(ELEMENT_NUMBER_REGISTER));
        move->addDecorations(intermediate::InstructionDecorations::AUTO_VECTORIZED);
        CPPLOG_LAZY(logging::Level::DEBUG,
            log << "Changed initial value: " << loopControl.initialization->to_string() << logging::endl);
    }
    else if(move != nullptr && move->getSource().getLiteralValue() && loopControl.stepKind == StepKind::ADD_CONSTANT &&
        loopControl.getStep() == INT_ONE.literal() &&
        (initialValueWalker = findWalker(loop.findPredecessor(), move)).has_value())
    {
        // more general case: initial value is a literal and step is +1
        initialValueWalker->reset(
            (new intermediate::Operation(OP_ADD, move->getOutput().value(), move->getSource(), ELEMENT_NUMBER_REGISTER))
                ->copyExtrasFrom(move));
        initialValueWalker.value()->addDecorations(intermediate::InstructionDecorations::AUTO_VECTORIZED);
        loopControl.initialization = initialValueWalker->get();
        CPPLOG_LAZY(logging::Level::DEBUG,
            log << "Changed initial value: " << loopControl.initialization->to_string() << logging::endl);
    }
    else
        throw CompilationError(
            CompilationStep::OPTIMIZER, "Unhandled initial value", loopControl.initialization->to_string());

    bool stepChanged = false;
    switch(stepOp->op.opAdd)
    {
    case OP_ADD.opAdd:
    case OP_SUB.opAdd:
        if(stepOp->getFirstArg().checkLocal())
        {
            const Value& offset = stepOp->assertArgument(1);
            if(offset.getLiteralValue())
                stepOp->setArgument(1,
                    Value(Literal(offset.getLiteralValue()->signedInt() *
                              static_cast<int32_t>(loopControl.vectorizationFactor)),
                        offset.type.toVectorType(static_cast<unsigned char>(
                            offset.type.getVectorWidth() * loopControl.vectorizationFactor))));
            else
                throw CompilationError(CompilationStep::OPTIMIZER, "Unhandled iteration step", stepOp->to_string());
        }
        else
        {
            const Value& offset = stepOp->getFirstArg();
            if(offset.getLiteralValue())
                stepOp->setArgument(0,
                    Value(Literal(offset.getLiteralValue()->signedInt() *
                              static_cast<int32_t>(loopControl.vectorizationFactor)),
                        offset.type.toVectorType(static_cast<unsigned char>(
                            offset.type.getVectorWidth() * loopControl.vectorizationFactor))));
            else
                throw CompilationError(CompilationStep::OPTIMIZER, "Unhandled iteration step", stepOp->to_string());
        }
        CPPLOG_LAZY(logging::Level::DEBUG, log << "Changed iteration step: " << stepOp->to_string() << logging::endl);
        stepChanged = true;
    }

    if(!stepChanged)
        throw CompilationError(CompilationStep::OPTIMIZER, "Unhandled iteration step operation", stepOp->to_string());
}

/*
 * Approach:
 * - set the iteration variable (local) to vector
 * - iterative (until no more values changed), modify all value (and local)-types so argument/result-types match again
 * - add new instruction-decoration (vectorized) to facilitate
 * - in final iteration, fix TMU/VPM configuration and address calculation and loop condition
 * - fix initial iteration value and step
 */
static void vectorize(ControlFlowLoop& loop, LoopControl& loopControl, const DataDependencyGraph& dependencyGraph)
{
    FastSet<const intermediate::IntermediateInstruction*> openInstructions;

    const_cast<DataType&>(loopControl.iterationVariable->type) =
        loopControl.iterationVariable->type.toVectorType(loopControl.iterationVariable->type.getVectorWidth() *
            static_cast<unsigned char>(loopControl.vectorizationFactor));
    scheduleForVectorization(loopControl.iterationVariable, openInstructions, loop);
    std::size_t numVectorized = 0;

    // iteratively change all instructions
    while(!openInstructions.empty())
    {
        auto it = loop.findInLoop(*openInstructions.begin());
        if(!it)
        {
            // TODO what to do?? These are e.g. for accumulation-variables (like sum, maximum)
            // FIXME depending on the operation performed on this locals, the vector-elements need to be folded into a
            // scalar/previous vector width
            CPPLOG_LAZY(logging::Level::DEBUG,
                log << "Local is accessed outside of loop: " << (*openInstructions.begin())->to_string()
                    << logging::endl);

            const intermediate::IntermediateInstruction* inst = *openInstructions.begin();
            const Value& arg = inst->assertArgument(0);
            const intermediate::Operation* op = dynamic_cast<const intermediate::Operation*>(arg.getSingleWriter());
            if(std::all_of(inst->getArguments().begin(), inst->getArguments().end(),
                   [&arg](const Value& otherArg) -> bool { return otherArg == arg; }) &&
                op != nullptr && op->hasDecoration(intermediate::InstructionDecorations::AUTO_VECTORIZED) &&
                !op->hasSideEffects())
            {
                /*
                 * There is a single writer to this local, which is vectorized and calculates the local via some
                 * operation (also has no side-effects)
                 * -> TODO we can accept the instruction by folding the vector-elements with the operation last applied
                 */
            }
            throw CompilationError(CompilationStep::OPTIMIZER,
                "Accessing vectorized locals outside of the loop is not yet implemented",
                (*openInstructions.begin())->to_string());
        }
        else
        {
            vectorizeInstruction(it.value(), openInstructions, loopControl.vectorizationFactor, loop);
            ++numVectorized;
        }
    }

    numVectorized += fixVPMSetups(loop, loopControl);

    fixInitialValueAndStep(loop, loopControl);
    numVectorized += 2;

    CPPLOG_LAZY(logging::Level::DEBUG,
        log << "Vectorization done, changed " << numVectorized << " instructions!" << logging::endl);
}

bool optimizations::vectorizeLoops(const Module& module, Method& method, const Configuration& config)
{
    // 1. find loops
    auto& cfg = method.getCFG();
    auto loops = cfg.findLoops();
    bool hasChanged = false;

    // 2. determine data dependencies of loop bodies
    auto dependencyGraph = DataDependencyGraph::createDependencyGraph(method);

    for(auto& loop : loops)
    {
        // 3. determine operation on iteration variable and bounds
        LoopControl loopControl = extractLoopControl(loop, *dependencyGraph);
        PROFILE_COUNTER(vc4c::profiler::COUNTER_OPTIMIZATION + 333, "Loops found", 1);
        if(loopControl.iterationVariable == nullptr)
            // we could not find the iteration variable, skip this loop
            continue;

        if(!loopControl.initialization || loopControl.terminatingValue.isUndefined() ||
            !loopControl.terminatingValue.isLiteralValue() || !loopControl.iterationStep || !loopControl.repetitionJump)
        {
            // we need to know both bounds and the iteration step (for now)
            CPPLOG_LAZY(logging::Level::DEBUG,
                log << "Failed to find all bounds and step for loop, aborting vectorization!" << logging::endl);
            continue;
        }

        // 4. determine vectorization factor
        Optional<unsigned> vectorizationFactor = determineVectorizationFactor(loop, loopControl);
        if(!vectorizationFactor)
        {
            CPPLOG_LAZY(logging::Level::DEBUG,
                log << "Failed to determine a vectorization factor for the loop, aborting!" << logging::endl);
            continue;
        }
        if(vectorizationFactor.value() == 1)
            // nothing to do
            continue;
        loopControl.vectorizationFactor = vectorizationFactor.value();

        // 5. cost-benefit calculation
        int rating = calculateCostsVsBenefits(loop, loopControl, *dependencyGraph);
        if(rating < 0 /* TODO some positive factor to be required before vectorizing loops? */)
            // vectorization (probably) doesn't pay off
            continue;

        // 6. run vectorization
        vectorize(loop, loopControl, *dependencyGraph);
        // increasing the iteration step might create a value not fitting into small immediate
        normalization::handleImmediate(module, method, loopControl.iterationStep.value(), config);
        hasChanged = true;

        PROFILE_COUNTER(
            vc4c::profiler::COUNTER_OPTIMIZATION + 334, "Vectorization factors", loopControl.vectorizationFactor);
    }

    return hasChanged;
}

void optimizations::extendBranches(const Module& module, Method& method, const Configuration& config)
{
    auto it = method.walkAllInstructions();
    // we only need to set the same flag once
    std::pair<Value, intermediate::InstructionDecorations> lastSetFlags =
        std::make_pair(UNDEFINED_VALUE, intermediate::InstructionDecorations::NONE);
    while(!it.isEndOfMethod())
    {
        if(auto branch = it.get<intermediate::Branch>())
        {
            CPPLOG_LAZY(logging::Level::DEBUG, log << "Extending branch: " << branch->to_string() << logging::endl);
            if(branch->hasConditionalExecution() || !branch->getCondition().hasLiteral(BOOL_TRUE.literal()))
            {
                /*
                 * branch can only depend on scalar value
                 * -> set any not used vector-element (all except element 0) to a value where it doesn't influence the
                 * condition
                 *
                 * Using ELEMENT_NUMBER sets the vector-elements 1 to 15 to a non-zero value and 0 to either 0 (if
                 * condition was false) or 1 (if condition was true)
                 */
                // TODO can be skipped, if it is checked/guaranteed, that the last instruction setting flags is the
                // boolean-selection for the given condition  but we need to check more than the last instructions,
                // since there could be moves inserted by phi

                // skip setting of flags, if the previous setting wrote the same flags
                if(lastSetFlags.first != branch->getCondition() ||
                    branch->hasDecoration(intermediate::InstructionDecorations::BRANCH_ON_ALL_ELEMENTS) !=
                        has_flag(lastSetFlags.second, intermediate::InstructionDecorations::BRANCH_ON_ALL_ELEMENTS))
                {
                    if(branch->hasDecoration(intermediate::InstructionDecorations::BRANCH_ON_ALL_ELEMENTS))
                        assign(it, NOP_REGISTER) =
                            (branch->getCondition() | branch->getCondition(), SetFlag::SET_FLAGS);
                    else
                        assign(it, NOP_REGISTER) =
                            (ELEMENT_NUMBER_REGISTER | branch->getCondition(), SetFlag::SET_FLAGS);
                }
                lastSetFlags.first = branch->getCondition();
                lastSetFlags.second = branch->decoration;
            }
            // go to next instruction
            it.nextInBlock();
            // insert 3 NOPs before
            it.emplace(new intermediate::Nop(intermediate::DelayType::BRANCH_DELAY));
            it.emplace(new intermediate::Nop(intermediate::DelayType::BRANCH_DELAY));
            it.emplace(new intermediate::Nop(intermediate::DelayType::BRANCH_DELAY));
        }
        else if(it.get() != nullptr && it->setFlags == SetFlag::SET_FLAGS)
        {
            // any other instruction setting flags, need to re-set the branch-condition
            lastSetFlags = std::make_pair(UNDEFINED_VALUE, intermediate::InstructionDecorations::NONE);
        }
        it.nextInMethod();
    }
}

static NODISCARD InstructionWalker loadVectorParameter(Parameter& param, Method& method, InstructionWalker it)
{
    // we need to load a UNIFORM per vector element into the particular vector element
    for(uint8_t i = 0; i < param.type.getVectorWidth(); ++i)
    {
        // the first write to the parameter needs to unconditional, so the register allocator can find it
        if(i > 0)
        {
            assign(it, NOP_REGISTER) =
                (ELEMENT_NUMBER_REGISTER ^ Value(SmallImmediate(i), TYPE_INT8), SetFlag::SET_FLAGS);
        }
        if(has_flag(param.decorations, ParameterDecorations::SIGN_EXTEND))
        {
            it = intermediate::insertSignExtension(it, method, Value(REG_UNIFORM, param.type),
                Value(&param, TYPE_INT32), false, i == 0 ? COND_ALWAYS : COND_ZERO_SET);
            it.copy().previousInBlock()->addDecorations(intermediate::InstructionDecorations::ELEMENT_INSERTION);
        }
        else if(has_flag(param.decorations, ParameterDecorations::ZERO_EXTEND))
        {
            it = intermediate::insertZeroExtension(it, method, Value(REG_UNIFORM, param.type),
                Value(&param, TYPE_INT32), false, i == 0 ? COND_ALWAYS : COND_ZERO_SET);
            it.copy().previousInBlock()->addDecorations(intermediate::InstructionDecorations::ELEMENT_INSERTION);
        }
        else
        {
            assign(it, param.createReference()) =
                (UNIFORM_REGISTER, i == 0 ? COND_ALWAYS : COND_ZERO_SET, InstructionDecorations::ELEMENT_INSERTION);
        }
        // TODO improve performance by first putting together the vector, then zero/sign extending all elements?
    }
    return it;
}

static void generateStopSegment(Method& method)
{
    // write interrupt for host
    // write QPU number finished (value must be NON-NULL, so we invert it -> the first 28 bits are always 1)
    method.appendToEnd(
        (new intermediate::Operation(OP_NOT, Value(REG_HOST_INTERRUPT, TYPE_INT8), Value(REG_QPU_NUMBER, TYPE_INT8)))
            ->addDecorations(InstructionDecorations::WORK_GROUP_UNIFORM_VALUE));
    intermediate::IntermediateInstruction* nop = new intermediate::Nop(intermediate::DelayType::THREAD_END);
    // set signals to stop thread/program
    nop->setSignaling(SIGNAL_END_PROGRAM);
    method.appendToEnd(nop);
    method.appendToEnd(new intermediate::Nop(intermediate::DelayType::THREAD_END));
    method.appendToEnd(new intermediate::Nop(intermediate::DelayType::THREAD_END));
}

static bool isLocalUsed(Method& method, const std::string& name)
{
    auto loc = method.findLocal(name);
    return loc != nullptr && !loc->getUsers(LocalUse::Type::READER).empty();
}

void optimizations::addStartStopSegment(const Module& module, Method& method, const Configuration& config)
{
    auto it = method.walkAllInstructions();
    if(!it.get<intermediate::BranchLabel>() ||
        BasicBlock::DEFAULT_BLOCK != it.get<intermediate::BranchLabel>()->getLabel()->name)
    {
        it = method.emplaceLabel(
            it, new intermediate::BranchLabel(*method.findOrCreateLocal(TYPE_LABEL, BasicBlock::DEFAULT_BLOCK)));
    }
    it.nextInBlock();

    // if the second TMU was used explicitly at some point, we disable TMU_SWAP
    {
        bool tmu1Used = false;
        auto checkIt = method.walkAllInstructions();
        while(!checkIt.isEndOfMethod())
        {
            if(checkIt->writesRegister(REG_TMU1_ADDRESS))
            {
                tmu1Used = true;
                break;
            }
            checkIt.nextInMethod();
        }
        if(tmu1Used)
        {
            CPPLOG_LAZY(logging::Level::DEBUG,
                log << "Using both TMUs explicitly, disable automatic swapping!" << logging::endl);
            assign(it, Value(REG_TMU_NOSWAP, TYPE_BOOL)) = BOOL_TRUE;
        }
    }

    /*
     * The first UNIFORMs are reserved for relaying information about the work-item and work-group
     * - work_dim: number of dimensions
     * - local_sizes: local number of work-items in its work-group per dimension
     * - local_ids: local id of this work-item within its work-group
     * - num_groups (x,y,z): global number of work-groups per dimension
     * - group_id (x, y, z): id of this work-group
     * - global_offset (x, y, z): global initial offset per dimension
     * - address of global data / to load the global data from
     *
     */
    // initially set all implicit UNIFORMs to unused
    method.metaData.uniformsUsed.value = 0;
    auto workInfoDecorations =
        add_flag(InstructionDecorations::UNSIGNED_RESULT, InstructionDecorations::WORK_GROUP_UNIFORM_VALUE);
    if(isLocalUsed(method, Method::WORK_DIMENSIONS))
    {
        method.metaData.uniformsUsed.setWorkDimensionsUsed(true);
        assign(it, method.findOrCreateLocal(TYPE_INT8, Method::WORK_DIMENSIONS)->createReference()) =
            (Value(REG_UNIFORM, TYPE_INT8), workInfoDecorations);
    }
    if(isLocalUsed(method, Method::LOCAL_SIZES))
    {
        method.metaData.uniformsUsed.setLocalSizesUsed(true);
        assign(it, method.findOrCreateLocal(TYPE_INT32, Method::LOCAL_SIZES)->createReference()) =
            (Value(REG_UNIFORM, TYPE_INT32), workInfoDecorations);
    }
    if(isLocalUsed(method, Method::LOCAL_IDS))
    {
        method.metaData.uniformsUsed.setLocalIDsUsed(true);
        assign(it, method.findOrCreateLocal(TYPE_INT32, Method::LOCAL_IDS)->createReference()) =
            (Value(REG_UNIFORM, TYPE_INT32),
                remove_flag(workInfoDecorations, InstructionDecorations::WORK_GROUP_UNIFORM_VALUE));
    }
    if(isLocalUsed(method, Method::NUM_GROUPS_X))
    {
        method.metaData.uniformsUsed.setNumGroupsXUsed(true);
        assign(it, method.findOrCreateLocal(TYPE_INT32, Method::NUM_GROUPS_X)->createReference()) =
            (Value(REG_UNIFORM, TYPE_INT32), workInfoDecorations);
    }
    if(isLocalUsed(method, Method::NUM_GROUPS_Y))
    {
        method.metaData.uniformsUsed.setNumGroupsYUsed(true);
        assign(it, method.findOrCreateLocal(TYPE_INT32, Method::NUM_GROUPS_Y)->createReference()) =
            (Value(REG_UNIFORM, TYPE_INT32), workInfoDecorations);
    }
    if(isLocalUsed(method, Method::NUM_GROUPS_Z))
    {
        method.metaData.uniformsUsed.setNumGroupsZUsed(true);
        assign(it, method.findOrCreateLocal(TYPE_INT32, Method::NUM_GROUPS_Z)->createReference()) =
            (Value(REG_UNIFORM, TYPE_INT32), workInfoDecorations);
    }
    if(isLocalUsed(method, Method::GROUP_ID_X))
    {
        method.metaData.uniformsUsed.setGroupIDXUsed(true);
        assign(it, method.findOrCreateLocal(TYPE_INT32, Method::GROUP_ID_X)->createReference()) =
            (Value(REG_UNIFORM, TYPE_INT32), workInfoDecorations);
    }
    if(isLocalUsed(method, Method::GROUP_ID_Y))
    {
        method.metaData.uniformsUsed.setGroupIDYUsed(true);
        assign(it, method.findOrCreateLocal(TYPE_INT32, Method::GROUP_ID_Y)->createReference()) =
            (Value(REG_UNIFORM, TYPE_INT32), workInfoDecorations);
    }
    if(isLocalUsed(method, Method::GROUP_ID_Z))
    {
        method.metaData.uniformsUsed.setGroupIDZUsed(true);
        assign(it, method.findOrCreateLocal(TYPE_INT32, Method::GROUP_ID_Z)->createReference()) =
            (Value(REG_UNIFORM, TYPE_INT32), workInfoDecorations);
    }
    if(isLocalUsed(method, Method::GLOBAL_OFFSET_X))
    {
        method.metaData.uniformsUsed.setGlobalOffsetXUsed(true);
        assign(it, method.findOrCreateLocal(TYPE_INT32, Method::GLOBAL_OFFSET_X)->createReference()) =
            (Value(REG_UNIFORM, TYPE_INT32), workInfoDecorations);
    }
    if(isLocalUsed(method, Method::GLOBAL_OFFSET_Y))
    {
        method.metaData.uniformsUsed.setGlobalOffsetYUsed(true);
        assign(it, method.findOrCreateLocal(TYPE_INT32, Method::GLOBAL_OFFSET_Y)->createReference()) =
            (Value(REG_UNIFORM, TYPE_INT32), workInfoDecorations);
    }
    if(isLocalUsed(method, Method::GLOBAL_OFFSET_Z))
    {
        method.metaData.uniformsUsed.setGlobalOffsetZUsed(true);
        assign(it, method.findOrCreateLocal(TYPE_INT32, Method::GLOBAL_OFFSET_Z)->createReference()) =
            (Value(REG_UNIFORM, TYPE_INT32), workInfoDecorations);
    }
    if(isLocalUsed(method, Method::GLOBAL_DATA_ADDRESS))
    {
        method.metaData.uniformsUsed.setGlobalDataAddressUsed(true);
        assign(it, method.findOrCreateLocal(TYPE_INT32, Method::GLOBAL_DATA_ADDRESS)->createReference()) =
            (Value(REG_UNIFORM, TYPE_INT32), workInfoDecorations);
    }

    // load arguments to locals (via reading from uniform)
    for(Parameter& param : method.parameters)
    {
        // do the loading
        // we need special treatment for non-scalar parameter (e.g. vectors), since they can't be read with just 1
        // UNIFORM
        if(!param.type.getPointerType() && param.type.getVectorWidth() != 1)
        {
            it = loadVectorParameter(param, method, it);
        }
        else if(has_flag(param.decorations, ParameterDecorations::SIGN_EXTEND))
        {
            it = intermediate::insertSignExtension(
                it, method, Value(REG_UNIFORM, param.type), Value(&param, TYPE_INT32), false);
        }
        else if(has_flag(param.decorations, ParameterDecorations::ZERO_EXTEND))
        {
            it = intermediate::insertZeroExtension(
                it, method, Value(REG_UNIFORM, param.type), Value(&param, TYPE_INT32), false);
        }
        else
        {
            /*
             * NOTE: Pointers with the byval decoration are treated as simple pointers, saving us from having to
             * re-write all instructions accessing them. In return, the VC4CL run-time needs to convert the direct
             * kernel argument (e.g. a struct) to a pointer-to-data argument by allocating a buffer (similar to local
             * arguments).
             *
             * Alternative ways of solving this:
             * - Read parameter from UNIFORMs and write to VPM, where it can be accessed like "normal" pointed-to data
             * - Read directly from UNIFORM storage, needs pointer to UNIFORM and re-set UNIFORM pointer for successive
             * parameter
             * - Load the single parts separately via UNIFORMs like any other vector/scalar, replace index-chain and
             * access functions.
             */
            assign(it, param.createReference()) = (Value(REG_UNIFORM, param.type),
                InstructionDecorations::WORK_GROUP_UNIFORM_VALUE,
                // all pointers are unsigned
                param.type.getPointerType() ? InstructionDecorations::UNSIGNED_RESULT : InstructionDecorations::NONE);
        }
    }

    generateStopSegment(method);
}

bool optimizations::removeConstantLoadInLoops(const Module& module, Method& method, const Configuration& config)
{
    CPPLOG_LAZY(logging::Level::DEBUG,
        log << "moveConstantsDepth = " << config.additionalOptions.moveConstantsDepth << logging::endl);
    bool hasChanged = false;

    // 1. find loops
    auto& cfg = method.getCFG();
    auto loops = cfg.findLoops();

    // 2. generate inclusion relation of loops as trees
    LoopInclusionTree inclusionTree;
    for(auto& loop1 : loops)
    {
        for(auto& loop2 : loops)
        {
            if(loop1.includes(loop2))
            {
                auto& node1 = inclusionTree.getOrCreateNode(&loop1);
                auto& node2 = inclusionTree.getOrCreateNode(&loop2);
                node1.addEdge(&node2, {});
            }
        }
    }

    // logging::debug() << "inclusionTree" << logging::endl;
    // for(auto& loop : inclusionTree) {
    //     logging::debug() << "  " << loop.first << logging::endl;
    //     for(auto& node : loop.second.getNeighbors()) {
    //         logging::debug() << "    " << node.first->key << ": " << node.second.includes << logging::endl;
    //     }
    // }

    // 3. move constant load operations from root of trees
    FastSet<ControlFlowLoop*> processed;
    for(auto& loop : loops)
    {
        auto& node = inclusionTree.getOrCreateNode(&loop);
        auto root = reinterpret_cast<LoopInclusionTreeNode*>(node.findRoot());

        if(processed.find(root->key) != processed.end())
            continue;
        processed.insert(root->key);

        // to prevent multiple block creation
        BasicBlock* insertedBlock = nullptr;

        for(auto& cfgNode : *root->key)
        {
            auto block = cfgNode->key;
            for(auto it = block->walk(); it != block->walkEnd(); it = it.nextInBlock())
            {
                // TODO: Constants like `mul24 r1, 4, elem_num` should be also moved.
                if(auto loadInst = it.get<LoadImmediate>())
                {
                    // LoadImmediate must have output value
                    auto out = loadInst->getOutput().value();
                    if(loadInst->hasValueType(ValueType::LOCAL) && !loadInst->hasSideEffects() &&
                        !loadInst->hasConditionalExecution())
                    {
                        CPPLOG_LAZY(logging::Level::DEBUG,
                            log << "Moving constant load out of loop: " << it->to_string() << logging::endl);
                        if(insertedBlock != nullptr)
                        {
                            insertedBlock->walkEnd().emplace(it.release());
                        }
                        else
                        {
                            if(auto targetBlock = root->key->findPredecessor())
                            {
                                auto targetInst = targetBlock->key->walkEnd();
                                targetInst.emplace(it.release());
                            }
                            else
                            {
                                CPPLOG_LAZY(logging::Level::DEBUG,
                                    log << "Create a new basic block before the root of inclusion tree"
                                        << logging::endl);

                                auto headBlock = method.begin();

                                insertedBlock = &method.createAndInsertNewBlock(
                                    method.begin(), "%createdByRemoveConstantLoadInLoops");
                                insertedBlock->walkEnd().emplace(it.release());

                                if(headBlock->getLabel()->getLabel()->name == BasicBlock::DEFAULT_BLOCK)
                                {
                                    // swap labels because DEFAULT_BLOCK is treated as head block.
                                    headBlock->getLabel()->getLabel()->name.swap(
                                        insertedBlock->getLabel()->getLabel()->name);
                                }
                            }
                        }
                        it.erase();
                        hasChanged = true;
                    }
                }
            }
        }
    }

    if(hasChanged)
        // combine the newly reordered (and at one place accumulated) loading instructions
        combineLoadingConstants(module, method, config);

    return hasChanged;
}

static const Local* findSourceBlock(const Local* label, const FastMap<const Local*, const Local*>& blockMap)
{
    auto it = blockMap.find(label);
    if(it == blockMap.end())
        return label;
    return findSourceBlock(it->second, blockMap);
}

bool optimizations::mergeAdjacentBasicBlocks(const Module& module, Method& method, const Configuration& config)
{
    auto& graph = method.getCFG();

    std::vector<std::pair<const Local*, const Local*>> blocksToMerge;

    auto it = method.begin();
    ++it;
    while(it != method.end())
    {
        // XXX currently, this only merges adjacent (in list of blocks) blocks
        auto prevIt = it;
        --prevIt;

        const auto& prevNode = graph.assertNode(&(*prevIt));
        const auto& node = graph.assertNode(&(*it));
        if(node.getSinglePredecessor() == &prevNode && prevNode.getSingleSuccessor() == &node &&
            // TODO for now, we cannot merge the last block, otherwise work-group unrolling doesn't work anymore
            it->getLabel()->getLabel()->name != BasicBlock::LAST_BLOCK)
        {
            CPPLOG_LAZY(logging::Level::DEBUG,
                log << "Found basic block with single direct successor: " << prevIt->to_string() << " and "
                    << it->to_string() << logging::endl);
            blocksToMerge.emplace_back(prevIt->getLabel()->getLabel(), it->getLabel()->getLabel());
        }
        ++it;
    }

    // this is required to be able to merge more than 2 blocks together
    FastMap<const Local*, const Local*> blockMap;

    for(auto& pair : blocksToMerge)
    {
        BasicBlock* sourceBlock = method.findBasicBlock(findSourceBlock(pair.second, blockMap));
        BasicBlock* destBlock = method.findBasicBlock(findSourceBlock(pair.first, blockMap));

        // remove all instructions from source block and append to destination block (skipping the source label)
        auto sourceIt = sourceBlock->walk().nextInBlock();
        while(!sourceIt.isEndOfBlock())
        {
            destBlock->walkEnd().emplace(sourceIt.release());
            sourceIt.nextInBlock();
        }
        // then remove the source block
        if(method.removeBlock(*sourceBlock))
            CPPLOG_LAZY(logging::Level::DEBUG,
                log << "Merged block " << pair.second->to_string() << " into " << pair.first->to_string()
                    << logging::endl);
        else
        {
            LCOV_EXCL_START
            CPPLOG_LAZY_BLOCK(logging::Level::WARNING, {
                logging::warn() << "Failed to remove empty basic block: " << sourceBlock->to_string() << logging::endl;
                if(!sourceBlock->empty())
                {
                    logging::warn() << "Block was not empty: " << logging::endl;
                    sourceBlock->dumpInstructions();
                }
                sourceBlock->forPredecessors([](InstructionWalker it) {
                    if(it.get())
                        logging::warn() << "Block has explicit predecessor: " << it->to_string() << logging::endl;
                });
            });
            LCOV_EXCL_STOP
        }

        blockMap.emplace(pair.second, pair.first);
    }

    CPPLOG_LAZY(logging::Level::DEBUG, log << "Merged " << blocksToMerge.size() << " pair of blocks!" << logging::endl);
    return !blocksToMerge.empty();
}

bool optimizations::reorderBasicBlocks(const Module& module, Method& method, const Configuration& config)
{
    const auto& cfg = method.getCFG();
    auto blockIt = method.begin();
    auto prevIt = method.begin();
    ++blockIt;
    while(blockIt != method.end())
    {
        const auto& node = cfg.assertNode(&(*blockIt));
        const auto predecessor = node.getSinglePredecessor();
        // Never re-order end-of-block. Though it should work, there could be trouble anyway
        if(blockIt->getLabel()->getLabel()->name != BasicBlock::LAST_BLOCK && predecessor != nullptr &&
            predecessor->key != &(*prevIt) && !prevIt->fallsThroughToNextBlock())
        {
            CPPLOG_LAZY(logging::Level::DEBUG,
                log << "Reordering block with single predecessor not being the previous block: " << blockIt->to_string()
                    << logging::endl);

            auto predecessorIt = method.begin();
            while(predecessorIt != method.end())
            {
                if(&(*predecessorIt) == predecessor->key)
                    break;
                ++predecessorIt;
            }

            if(predecessorIt == method.end())
                throw CompilationError(CompilationStep::OPTIMIZER,
                    "Failed to find predecessor basic block: ", predecessor->key->to_string());

            // we insert before the iteration, so we need to set the iterator after the predecessor
            ++predecessorIt;
            method.moveBlock(blockIt, predecessorIt);
            // prevIt stays the same, since we removed the block and the next blockIt now follows prevIt
            blockIt = prevIt;
            ++blockIt;
        }
        else
        {
            ++blockIt;
            ++prevIt;
        }
    }

    return false;
}

struct IfElseBlock
{
    // The common predecessor block, the block whether the condition(s) are checked
    CFGNode* predecessor;
    // The blocks executed for the different cases (may be a single for if without else or several for switch-cases)
    FastAccessList<CFGNode*> conditionalBlocks;
    // The common successor block, i.e. the block after the if-else or switch-case block
    CFGNode* successor;
};

static FastAccessList<IfElseBlock> findIfElseBlocks(ControlFlowGraph& graph)
{
    FastAccessList<IfElseBlock> blocks;
    graph.forAllNodes([&](CFGNode& node) {
        IfElseBlock candidateBlock{&node, {}, nullptr};
        node.forAllOutgoingEdges([&](CFGNode& successor, CFGEdge& edge) -> bool {
            // edge is a candidate, if it has a single successor (the same as all other candidates) and a single
            // predecessor (the base node being checked)
            // TODO does not accept if-without-else blocks or switch-with-defaults!! Would need to allow one
            // of the direct successors to also be successor of all other direct successors
            // TODO to guarantee that we not only save instructions, but also execution cycles, we should check the
            // maximum length of the resulting block not exceeding the instructions we save executing one of the cases
            // (e.g. 2 branches + some conditionals/phi).
            if(auto succ = successor.getSingleSuccessor())
            {
                if((candidateBlock.successor == nullptr || succ == candidateBlock.successor) &&
                    successor.getSinglePredecessor() == &node)
                {
                    candidateBlock.conditionalBlocks.emplace_back(&successor);
                    candidateBlock.successor = succ;
                    return true;
                }
            }
            // first level successors have different/multiple second level successors (or multiple predecessors), abort
            candidateBlock.successor = nullptr;
            return false;
        });

        if(candidateBlock.successor != nullptr && candidateBlock.conditionalBlocks.size() > 1)
        {
            blocks.emplace_back(std::move(candidateBlock));
        }
    });
    return blocks;
}

bool optimizations::simplifyConditionalBlocks(const Module& module, Method& method, const Configuration& config)
{
    bool changedCode = false;
    for(const auto& block : findIfElseBlocks(method.getCFG()))
    {
        CPPLOG_LAZY_BLOCK(logging::Level::DEBUG, {
            logging::debug() << "Found conditional block candidate: " << block.predecessor->key->to_string()
                             << logging::endl;
            for(auto succ : block.conditionalBlocks)
                logging::debug() << "\t" << succ->key->to_string() << logging::endl;
            logging::debug() << "Successor: " << block.successor->key->to_string() << logging::endl;
        });

        bool hasSideEffects = false;
        FastSet<const Local*> nonlocalLocals;
        for(auto succ : block.conditionalBlocks)
        {
            auto it = succ->key->walk().nextInBlock(); // skip label
            while(it != succ->key->walkEnd())
            {
                if(it.get<intermediate::Branch>() == nullptr && (it->hasSideEffects() || it->hasConditionalExecution()))
                {
                    CPPLOG_LAZY(logging::Level::DEBUG,
                        log << "Side effect in " << succ->key->to_string() << " - " << it->to_string()
                            << logging::endl);
                    hasSideEffects = true;
                    break;
                }
                if(it->hasValueType(ValueType::LOCAL) && !succ->key->isLocallyLimited(it, it->getOutput()->local(), 8))
                    nonlocalLocals.emplace(it->getOutput()->local());
                it.nextInBlock();
            }
            if(hasSideEffects)
                break;
        }

        if(hasSideEffects)
        {
            CPPLOG_LAZY(logging::Level::DEBUG,
                log << "Aborting optimization, since conditional block has side effects" << logging::endl);
            continue;
        }

        CPPLOG_LAZY_BLOCK(logging::Level::DEBUG, {
            for(auto loc : nonlocalLocals)
                logging::debug() << "Non-local: " << loc->to_string() << logging::endl;
        });

        // need to reorder successive blocks, so that default branch (without any condition) is inserted top-most and
        // not at last!
        InstructionWalker beforeBranchesIt = block.predecessor->key->walk().nextInBlock();
        while(!beforeBranchesIt.get<intermediate::Branch>())
            beforeBranchesIt.nextInBlock();
        // go to last before the first branch
        beforeBranchesIt.previousInBlock();

        for(auto succ : block.conditionalBlocks)
        {
            succ->forAllIncomingEdges([&](CFGNode& predecessor, CFGEdge& edge) -> bool {
                // the predecessor instruction is the branch to this block (if not fall-through)
                auto lastIt = edge.data.getPredecessor(predecessor.key);

                // copy the whole block content before the branch to the block, modify writing all external locals to
                // only be applied for the same condition the branch is applied and remove the branch (if not
                // fall-through).
                // at the moment of this optimization, the writing of the conditional the branch depends on is already
                // generated, so we can just re-use the conditional.
                Optional<Value> condVal{};
                ConditionCode cond = COND_ALWAYS;
                {
                    auto branch = lastIt.get<intermediate::Branch>();
                    if(branch && branch->getTarget() == succ->key->getLabel()->getLabel() &&
                        branch->hasConditionalExecution())
                    {
                        condVal = branch->getCondition();
                        cond = branch->conditional;
                    }
                    else
                    {
                        // the last branch is unconditional (e.g. the default for switch-cases), but we need to insert
                        // the unconditional local assignment as first instruction.
                        // remove original unconditional branch
                        lastIt.erase();
                        // make sure the instructions are inserted before all other
                        lastIt = beforeBranchesIt;
                    }
                }

                // 1.) insert flag depending on the conditional of the branch
                if(condVal && cond != COND_ALWAYS)
                    assign(lastIt, NOP_REGISTER) = (*condVal, SetFlag::SET_FLAGS);

                // 2.) insert all instructions
                for(auto& inst : *succ->key)
                {
                    if(dynamic_cast<const intermediate::BranchLabel*>(inst.get()))
                        // neither move nor delete the label
                        continue;

                    if(dynamic_cast<const intermediate::Branch*>(inst.get()))
                    {
                        // do not copy branches to successor label
                        inst.reset();
                        continue;
                    }

                    lastIt.emplace(inst.release());

                    // 3.) modify all instructions writing non-locals to only write under same condition as the branch
                    // XXX do we win anything in making all the instructions conditional? Technically this would be
                    // possible
                    for(auto loc : nonlocalLocals)
                    {
                        if(lastIt->writesLocal(loc))
                        {
                            lastIt->setCondition(cond);
                            break;
                        }
                    }

                    lastIt.nextInBlock();
                }

                // 4.) remove branch to original block
                if(condVal)
                    lastIt.erase();

                // 5.) remove original block
                if(!method.removeBlock(*succ->key))
                {
                    CPPLOG_LAZY_BLOCK(logging::Level::WARNING, {
                        logging::warn() << "Failed to remove move-from basic block: " << succ->key->to_string()
                                        << logging::endl;
                        succ->key->dumpInstructions();
                    });
                    // XXX throw exception here or continue??
                }

                // there is only one incoming edge
                return false;
            });
        }

        // insert branch to successor block to guarantee we switch into that, independent of the block order
        block.predecessor->key->walkEnd().emplace(
            new intermediate::Branch(block.successor->key->getLabel()->getLabel(), COND_ALWAYS, BOOL_TRUE));

        changedCode = true;
    }

    return changedCode;
}
