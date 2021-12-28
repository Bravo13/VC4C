/*
 * General header for the public C++ interface, containing all required headers
 *
 * Author: doe300
 *
 * See the file "LICENSE" for the full license governing this code.
 */

#ifndef VC4C_H
#define VC4C_H

#include "CompilationError.h"
#include "Compiler.h"
#include "Precompiler.h"
#include "config.h"
#include "tools.h"

#include <iostream>

namespace vc4c
{
    /*
     * Disassembles the given machine-code module and writes the converted code into output
     */
    std::size_t disassembleModule(
        const CompilationData& binaryData, std::ostream& output, OutputMode outputMode = OutputMode::HEX);
    /*
     * Disassembles the given machine code (containing only instructions) and writes the converted code into output
     */
    std::size_t disassembleCodeOnly(std::istream& binary, std::ostream& output, std::size_t numInstructions,
        OutputMode outputMode = OutputMode::HEX);
} /* namespace vc4c */

#endif /* VC4C_H */
