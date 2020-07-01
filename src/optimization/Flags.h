/*
 * Author: doe300
 *
 * See the file "LICENSE" for the full license governing this code.
 */
#ifndef VC4C_OPTIMIZATION_FLAGS
#define VC4C_OPTIMIZATION_FLAGS

namespace vc4c
{
    class Method;
    class Module;
    class InstructionWalker;
    struct Configuration;

    namespace optimizations
    {
        /*
         * Removes setting of flags where it is guaranteed that the flags are set always or never.
         * Also removes the setting of flags, if they are never used.
         *
         * All succeeding conditional instructions based on these flags are either also removed or made unconditional,
         * depending on whether they require the flags to be set or cleared.
         *
         * Example:
         *   - = xor 0, 1 (setf)
         *   %1 = %2 (ifz)
         *   %1 = %3 (ifzc)
         *
         * becomes:
         *   %1 = %3
         *
         * Also:
         *   %1 = xor 0, %2 (setf)
         *   [...]
         *   - = xor 0, %4 (setf)
         *
         * becomes:
         *   %1 = xor 0, %2
         *   [...]
         *   - = xor 0, %4 (setf)
         *
         * And:
         *   - = xor 0, 1 (setf)
         *   [...]
         *   - = xor 0, %4 (setf)
         *
         * becomes:
         *   [...]
         *   - = xor 0, %4 (setf)
         */
        bool removeUselessFlags(const Module& module, Method& method, const Configuration& config);

        /*
         * Combines successive setting of the same flag (e.g. introduced by PHI-nodes)
         *
         * Example:
         *   - = %3 (setf)
         *   ...
         *   - = %3 (setf)
         *
         * is converted to:
         *   - = %3 (setf)
         *   ...
         *
         * NOTE: Currently, only moves into nop-register are combined, but in an extended optimization-step any two
         * instructions setting flags for the same value and with at most one output could be combined.
         */
        InstructionWalker combineSameFlags(
            const Module& module, Method& method, InstructionWalker it, const Configuration& config);

        /*
         * Combines moves setting flags with move of the same value into output registers.
         *
         * Example:
         *   - = %b (setf)
         *   ...
         *   %a = %b
         *
         * becomes:
         *   ...
         *   %a = %b (setf)
         *
         * Also:
         *   %a = %b
         *   ...
         *   - = %b (setf)
         *
         * becomes:
         *   %a = %b (setf)
         *   ...
         */
        InstructionWalker combineFlagWithOutput(
            const Module& module, Method& method, InstructionWalker it, const Configuration& config);

        /**
         * Simplifies settings of flags to facilitate further optimizations.
         *
         * Example:
         *   %a = uniform
         *   - = or elem_num, %a (setf)
         *   br %A (anyz)
         *   br %B (allnz)
         *
         * can be simplified to:
         *   %a = uniform
         *   - = %a (setf)
         *   br %A (anyz)
         *   br %B (allnz)
         *
         * which then allows for further optimization.
         */
        InstructionWalker simplifyFlag(
            const Module& module, Method& method, InstructionWalker it, const Configuration& config);

        /**
         * Tries to rewrite flags (and their conditional operations) depending on conditional values (most often bool
         * depending on another flags) to directly depend on the originating flags, removing the need for intermediate
         * conditional writes.
         *
         * Example:
         *   - = max %iterator, %limit (setf)
         *   %comp = 1 (ifc)
         *   %comp = 0 (ifcc)
         *   - = %comp (setf)
         *   %out = %in (ifzc)
         *
         * can be simplified to:
         *   - = max %iterator, %limit (setf)
         *   %out = %in (ifc)
         */
        bool removeConditionalFlags(const Module& module, Method& method, const Configuration& config);
    } /* namespace optimizations */
} /* namespace vc4c */

#endif /* VC4C_OPTIMIZATION_FLAGS */
