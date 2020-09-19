/*
 * Header for the pre-compiler allowing programmatic access to the LLVM/SPIRV-LLVM binaries for converting OpenCL C
 * source code to LLVM-IR/SPIR-V
 *
 * Author: doe300
 *
 * See the file "LICENSE" for the full license governing this code.
 */

#ifndef PRECOMPILER_H
#define PRECOMPILER_H

#include "Optional.h"
#include "config.h"

#include <iostream>
#include <memory>
#include <unordered_map>
#include <vector>

namespace vc4c
{
    /*
     * The type of input-code determined for the input
     */
    enum class SourceType
    {
        /*
         * Type was not (yet) determined
         */
        UNKNOWN = 0,
        /*
         * OpenCL C source-code
         */
        OPENCL_C = 1,
        /*
         * LLVM IR in textual representation
         */
        LLVM_IR_TEXT = 2,
        /*
         * LLVM IR bit-code
         */
        LLVM_IR_BIN = 3,
        /*
         * SPIR-V in binary representation
         */
        SPIRV_BIN = 4,
        /*
         * SPIR-V in textual representation
         */
        SPIRV_TEXT = 5,
        /*
         * generated machine code in hexadecimal representation
         */
        QPUASM_HEX = 6,
        /*
         * generated machine code in binary representation
         */
        QPUASM_BIN = 7
    };

    bool isSupportedByFrontend(SourceType inputType, Frontend frontend);

    /*
     * RAII object to manage a temporary file.
     *
     * This class guarantees the temporary file to be deleted even if the compilation is cancelled by throwing an
     * exception.
     */
    class TemporaryFile : private NonCopyable
    {
    public:
        /*
         * Creates and manages a new empty temporary file
         */
        explicit TemporaryFile(const std::string& fileTemplate = "/tmp/vc4c-XXXXXX", bool hasStaticLifetime = false);
        /*
         * Creates and manages a new temporary file with fixed file-name and initial content
         */
        explicit TemporaryFile(const std::string& fileName, std::istream& data, bool hasStaticLifetime = false);
        /*
         * Creates and manages a new temporary file with fixed file-name and initial content
         */
        explicit TemporaryFile(const std::string& fileName, const std::vector<char>& data);
        TemporaryFile(const TemporaryFile&) = delete;
        TemporaryFile(TemporaryFile&& other) noexcept;
        ~TemporaryFile();

        TemporaryFile& operator=(const TemporaryFile&) = delete;
        TemporaryFile& operator=(TemporaryFile&&) noexcept = delete;

        void openOutputStream(std::unique_ptr<std::ostream>& ptr) const;
        void openInputStream(std::unique_ptr<std::istream>& ptr) const;

        const std::string fileName;

    private:
        // this temporary file lives as long as the program lives
        bool isStaticTemporary;
    };

    /*
     * Container for the paths used to look up the VC4CL OpenCL C standard-library implementation files
     */
    struct StdlibFiles
    {
        // The path to the defines.h header file, empty if not found. This is always required
        std::string configurationHeader;
        // The path to the pre-compiled header (PCH), empty if not found. Only required for SPIR-V front-end
        std::string precompiledHeader;
        // The path to the pre-compiled LLVM module, empty if not found. Only required for LLVM module front-end
        std::string llvmModule;
    };

    /*
     * The pre-compiler manages and executes the conversion of the input from a various of supported types to a type
     * which can be read by one of the configured compiler front-ends.
     */
    class Precompiler
    {
    public:
        Precompiler(Configuration& config, std::istream& input, SourceType inputType,
            const Optional<std::string>& inputFile = {});

        /*
         * Runs the pre-compilation from the source-type passed to the constructor to the output-type specified.
         */
        void run(std::unique_ptr<std::istream>& output, SourceType outputType, const std::string& options = "",
            Optional<std::string> outputFile = {});

        /*
         * Helper-function to easily pre-compile a single input with the given configuration into the given output.
         *
         * \param input The input stream
         * \param output The output-stream
         * \param config The configuration to use for compilation
         * \param options Specify additional compiler-options to pass onto the pre-compiler
         * \param inputFile Can be used by the compiler to speed-up compilation (e.g. by running the pre-compiler with
         * these files instead of needing to write input to a temporary file) \param outputFile The optional output-file
         * to write the pre-compiled code into. If this is specified, the code is compiled into the file, otherwise the
         * output stream is filled with the compiled code
         */
        static void precompile(std::istream& input, std::unique_ptr<std::istream>& output, Configuration config = {},
            const std::string& options = "", const Optional<std::string>& inputFile = {},
            const Optional<std::string>& outputFile = {});

        /*
         * Determines the type of code stored in the given stream.
         *
         * NOTE: This function reads from the stream but resets the cursor back to the beginning.
         */
        static SourceType getSourceType(std::istream& stream);

        /*
         * Links multiple source-code files using a linker provided by the pre-compilers.
         *
         * Returns the SourceType of the linked module
         */
        static SourceType linkSourceCode(const std::unordered_map<std::istream*, Optional<std::string>>& inputs,
            std::ostream& output, bool includeStandardLibrary = false);

        /*
         * Returns whether there is a linker available that can link the given input modules
         */
        static bool isLinkerAvailable(const std::unordered_map<std::istream*, Optional<std::string>>& inputs);
        /*
         * Returns whether a linker is available at all in the compiler
         */
        static bool isLinkerAvailable();

        /*
         * Determines and returns the paths to the VC4CL OpenCL C standard library files to be used for compilations
         *
         * The optional parameter specifies additional folder to look up the required files. If it is not given, only
         * the default locations will be searched.
         *
         * NOTE: The locations of the files are cached, therefore only the first call has any effect of specifying the
         * locations.
         */
        static const StdlibFiles& findStandardLibraryFiles(const std::vector<std::string>& additionalFolders = {});

        /*
         * Pre-compiles the given VC4CL OpenCL C standard-library file (the VC4CLStdLib.h header) into a PCH and an LLVM
         * module and stores them in the given output folder.
         */
        static void precompileStandardLibraryFiles(const std::string& sourceFile, const std::string& destinationFolder);

        const SourceType inputType;
        const Optional<std::string> inputFile;
        const Configuration config;

    private:
        std::istream& input;
    };
} // namespace vc4c

#endif /* PRECOMPILER_H */
