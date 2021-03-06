////////////////////////////////////////////////////////////////////////////////////////////////////
//
//  Project:  Embedded Learning Library (ELL)
//  File:     IRSwigInterfaceWriter.cpp (emitters)
//  Authors:  Lisa Ong
//
////////////////////////////////////////////////////////////////////////////////////////////////////

#include "IRSwigInterfaceWriter.h"
#include "EmitterException.h"
#include "IRHeaderWriter.h"
#include "IRMetadata.h"

// utilities
#include "Debug.h"
#include "StringUtil.h"

// stl
#include <regex>
#include <sstream>
#include <string>

namespace ell
{
namespace emitters
{
    namespace
    {
        // Utilities
        void ReplaceDelimiter(std::string& text, const std::string& delimiter, const std::string& replacement)
        {
            std::string actualDelimiter = "@@" + delimiter + "@@";
            text = std::regex_replace(text, std::regex(actualDelimiter), replacement);
        }

        std::string AsVectorType(const std::string& type)
        {
            return type == "float" ? "FloatVector" : "DoubleVector";
        }

        std::string AsNumpyType(const std::string& type)
        {
            return type == "float" ? "float32" : "float";
        }

        void WriteCommaSeparatedList(std::ostream& os, const std::vector<std::string>& list)
        {
            bool first = true;
            for (const auto& item : list)
            {
                if (!first)
                {
                    os << ", ";
                }
                os << item;
                first = false;
            }
        }

        // Writes a scoped #ifdef SWIG declaration
        struct DeclareIfndefSwig : private DeclareIfDefGuard
        {
            DeclareIfndefSwig(std::ostream& os)
                : DeclareIfDefGuard(os, "SWIG", DeclareIfDefGuard::Type::Negative)
            {
            }
        };

        // Writes SWIG interfaces for predict
        class PredictInterfaceWriter
        {
        public:
            PredictInterfaceWriter(IRModuleEmitter& moduleEmitter, llvm::Function& predictFunction)
                : _function(&predictFunction)
            {
                InitPredictFunctionInfo();
            }

            void WriteHeaderCode(std::ostream& os) const
            {
                std::string inputType = _inputType;
                std::string inputArgument = "input";
                if (!_inputIsScalar)
                {
                    std::ostringstream osType;
                    osType << "const std::vector<" << _inputType << ">&";
                    inputType = osType.str();

                    std::ostringstream osArgument;
                    osArgument << "const_cast<" << _inputType << "*>(&input[0])";
                    inputArgument = osArgument.str();
                }

                // Write header for SWIG to generate a wrapper
                // (Note: newlines are part of the syntax for #include)
                std::string predictFunctionCode(
#include "SwigPredictFunction.in"
                    );

                ReplaceDelimiter(predictFunctionCode, "FUNCTION", _functionName);
                ReplaceDelimiter(predictFunctionCode, "INPUT_TYPE", inputType);
                ReplaceDelimiter(predictFunctionCode, "INPUT_ARGUMENT", inputArgument);
                ReplaceDelimiter(predictFunctionCode, "OUTPUT_TYPE", _outputType);

                os << predictFunctionCode << "\n";
            }

            void WriteSwigCode(std::ostream& os) const
            {
                DeclareIfDefGuard guard(os, "SWIGPYTHON", DeclareIfDefGuard::Type::Positive);

                std::string predictPythonCode(
#include "SwigRawPredictPython.in"
                    );

                ReplaceDelimiter(predictPythonCode, "PREDICT_FUNCTION", _functionName);
                ReplaceDelimiter(predictPythonCode, "OUTPUT_VECTOR_TYPE", AsVectorType(_outputType));

                os << "%pythoncode %{\n"
                   << predictPythonCode
                   << "\n%}\n";
            }

        private:
            void InitPredictFunctionInfo()
            {
                _functionName = _function->getName();

                // Two arguments (input may be a scalar or pointer)
                auto it = _function->args().begin();
                {
                    std::ostringstream os;
                    auto& argument = *it;
                    auto type = argument.getType();
                    if (type->isPointerTy())
                    {
                        _inputIsScalar = false;
                        WriteLLVMType(os, argument.getType()->getPointerElementType());
                    }
                    else
                    {
                        _inputIsScalar = true;
                        WriteLLVMType(os, argument.getType());
                    }
                    _inputType = os.str();
                }

                {
                    std::ostringstream os;
                    WriteLLVMType(os, (*(++it)).getType()->getPointerElementType());
                    _outputType = os.str();
                }
            }

            std::string _functionName;
            std::string _inputType;
            std::string _outputType;
            bool _inputIsScalar;

            llvm::Function* _function;
        };

        struct CallbackSignature
        {
            CallbackSignature(llvm::Function& f)
            {
                functionName = f.getName();

                // Callbacks have one input parameter and a return (which can be void)
                {
                    std::ostringstream os;
                    auto& argument = *(f.args().begin());
                    auto type = argument.getType();
                    if (type->isPointerTy())
                    {
                        inputIsScalar = false;
                        WriteLLVMType(os, argument.getType()->getPointerElementType());
                    }
                    else
                    {
                        inputIsScalar = true;
                        WriteLLVMType(os, argument.getType());
                    }
                    inputType = os.str();
                }

                {
                    std::ostringstream os;
                    WriteLLVMType(os, f.getReturnType());
                    returnType = os.str();
                }

                className = inputType + "CallbackBase";
                className[0] = toupper(className[0]); // pascal case
            }

            std::string functionName;
            std::string className;
            std::string inputType;
            std::string returnType;
            bool inputIsScalar;
        };

        // Writes SWIG interfaces for predictors with step support
        class SteppablePredictorInterfaceWriter
        {
        public:
            SteppablePredictorInterfaceWriter(IRModuleEmitter& moduleEmitter, const std::string& predictFunctionName, const std::vector<FunctionTagValues>& callbacks)
                : _predictFunctionName(predictFunctionName)
            {
                _moduleName = moduleEmitter.GetLLVMModule()->getName();
                _className = _moduleName + "_Predictor";

                for (const auto& c : callbacks)
                {
                    if (!c.values.empty())
                    {
                        auto nodeType = c.values[0];
                        if (nodeType == "SourceNode")
                        {
                            _inputCallbacks.push_back(CallbackSignature(*c.function));
                        }
                        else if (nodeType == "SinkNode")
                        {
                            _outputCallbacks.push_back(CallbackSignature(*c.function));
                        }
                        else if (nodeType == "ClockNode")
                        {
                            _lagCallbacks.push_back(CallbackSignature(*c.function));
                        }
                    }
                }

                // Eventually we'd support multiple sources and sinks.
                // For now, assert that we're only going to look at the first ones of each.
                DEBUG_THROW(_inputCallbacks.size() != 1, EmitterException(EmitterError::badFunctionDefinition, "Only one input callback function will be generated"));
                DEBUG_THROW(_outputCallbacks.size() != 1, EmitterException(EmitterError::badFunctionDefinition, "Only one output callback function will be generated"));
                DEBUG_THROW(_lagCallbacks.size() != 1, EmitterException(EmitterError::badFunctionDefinition, "Only one lag callback function will be generated"));
            }

            virtual ~SteppablePredictorInterfaceWriter() = default;

            void WriteSwigCode(std::ostream& os) const
            {
                WriteCallbackSwigCode(os);
                WritePythonCode(os);
            }

            void WriteHeaderCode(std::ostream& os) const
            {
                // Load the template, which declares a C++ predictor class
                // for forwarding actuator callbacks to the predictor.
                // (Note: newlines are part of the syntax for #include)
                std::string predictorCode(
#include "SwigPredictorClass.in"
                    );

                ReplaceDelimiter(predictorCode, "PREDICTOR_CLASS", _className);

                ReplaceDelimiter(predictorCode, "LAG_CALLBACK", _lagCallbacks[0].functionName);
                ReplaceDelimiter(predictorCode, "SINK_CALLBACK", _outputCallbacks[0].functionName);
                ReplaceDelimiter(predictorCode, "SOURCE_CALLBACK", _inputCallbacks[0].functionName);

                ReplaceDelimiter(predictorCode, "SINK_TYPE", _outputCallbacks[0].inputType);
                ReplaceDelimiter(predictorCode, "SOURCE_TYPE", _inputCallbacks[0].inputType);
                ReplaceDelimiter(predictorCode, "TIMETICK_TYPE", "double");

                os << predictorCode << "\n";
            }

        private:
            void WriteCallbackSwigCode(std::ostream& os) const
            {
                os << "WRAP_CALLABLES_AS_CALLBACKS(" << _className << ", ";
                WriteCommaSeparatedList(os,
                                        { _inputCallbacks[0].className,
                                          _inputCallbacks[0].inputType,
                                          _outputCallbacks[0].className,
                                          _outputCallbacks[0].inputType,
                                          _lagCallbacks[0].className });
                os << ")\n\n";
            }

            void WritePythonCode(std::ostream& os) const
            {
                // Load the template, which declares the Python base class derived
                // by actuator code to implement callbacks.
                // (Note: newlines are part of the syntax for #include)
                std::string pythonCode(
#include "SwigPredictorPython.in"
                    );

                ReplaceDelimiter(pythonCode, "MODULE", _moduleName);
                ReplaceDelimiter(pythonCode, "PREDICTOR_CLASS", _className);
                ReplaceDelimiter(pythonCode, "PREDICT_FUNCTION", _predictFunctionName);
                ReplaceDelimiter(pythonCode, "PREDICT_FUNCTION_PY", AsPythonMethod(_predictFunctionName));

                ReplaceDelimiter(pythonCode, "LAG_CALLBACK", AsPythonMethod(_lagCallbacks[0].functionName));
                ReplaceDelimiter(pythonCode, "SINK_CALLBACK", AsPythonMethod(_outputCallbacks[0].functionName));
                ReplaceDelimiter(pythonCode, "SOURCE_CALLBACK", AsPythonMethod(_inputCallbacks[0].functionName));

                ReplaceDelimiter(pythonCode, "SINK_VECTOR_TYPE", AsVectorType(_outputCallbacks[0].inputType));
                ReplaceDelimiter(pythonCode, "SOURCE_VECTOR_TYPE", AsVectorType(_inputCallbacks[0].inputType));
                ReplaceDelimiter(pythonCode, "SOURCE_NUMPY_TYPE", AsNumpyType(_inputCallbacks[0].inputType));

                os << "%pythoncode %{\n"
                   << pythonCode
                   << "\n%}\n";
            }

            //
            // Utilities
            //

            std::string AsPythonMethod(const std::string& functionName) const
            {
                // remove module name prefix because these are method names
                std::string result = std::regex_replace(functionName, std::regex(_moduleName + "_"), "");

                // convert to snake case
                // (i.e. insert _ before a capitalized letter that's not preceeded by a non-capitalized letter)
                result = std::regex_replace(result, std::regex("([^A-Z])([A-Z])"), "$1_$2");

                // convert to lower case
                return utilities::ToLowercase(result);
            }

            // Callbacks
            std::vector<CallbackSignature> _outputCallbacks;
            std::vector<CallbackSignature> _inputCallbacks;
            std::vector<CallbackSignature> _lagCallbacks;

            std::string _predictFunctionName;
            std::string _moduleName;
            std::string _className;
        };

        void WriteCommonSwigCode(std::ostream& os, IRModuleEmitter& moduleEmitter)
        {
            auto pModule = moduleEmitter.GetLLVMModule();
            std::string moduleName = pModule->getName();

            // Declare the module and enable the directors feature
            os << "%module(directors=\"1\") " << moduleName << "\n";

            // Generate docstrings from types and method signatures
            os << "%feature(\"autodoc\", \"3\");\n\n";

            // Common SWIG code
            os << "%include \"callback.i\"\n";
        }

        void WriteShapeWrappers(std::ostream& os, IRModuleEmitter& moduleEmitter)
        {
            auto pModule = moduleEmitter.GetLLVMModule();
            std::string moduleName = pModule->getName();

            /*
            * Wrap the module_GetInputShape "C" function so we return the TensorShape on the stack
            * as a conveniently wrapped Python type.

            %inline %{
                ell::api::math::TensorShape get_default_input_shape() {
                    TensorShape  s;
                    module_GetInputShape(index, &s);
                    return ell::api::math::TensorShape{s.rows, s.columns, s.channels};
                }
            %} */

            std::string shapeWrappers(
#include "SwigShapeWrappers.in"
                );

            ReplaceDelimiter(shapeWrappers, "MODULE", moduleName);

            os << shapeWrappers << "\n";
        }

        void WriteModuleSwigCode(std::ostream& os, IRModuleEmitter& moduleEmitter, const std::string& headerName)
        {
            // Module C++ #include
            os << "%{\n"
                  "#include \"" << headerName << "\"\n"
                  "%}\n\n";

            auto predicts = GetFunctionsWithTag(moduleEmitter, c_predictFunctionTagName);
            std::string predictFunctionName = (predicts[0].function)->getName();

            // Module callback macros
            auto callbacks = GetFunctionsWithTag(moduleEmitter, c_callbackFunctionTagName);
            if (!callbacks.empty())
            {
                SteppablePredictorInterfaceWriter writer(moduleEmitter, predictFunctionName, callbacks);
                writer.WriteSwigCode(os);
            }
            else
            {
                PredictInterfaceWriter writer(moduleEmitter, *(predicts[0].function));
                writer.WriteSwigCode(os);
            }

            os << "%include \"" << headerName << "\"\n";
            WriteShapeWrappers(os, moduleEmitter);
        }
    }

    void WriteModuleSwigHeader(std::ostream& os, IRModuleEmitter& moduleEmitter)
    {
        std::string moduleName = moduleEmitter.GetLLVMModule()->getName();

        os << "//\n// ELL SWIG header for module " << moduleName << "\n//\n\n";
        os << "#pragma once\n\n";

        auto predicts = GetFunctionsWithTag(moduleEmitter, c_predictFunctionTagName);
        auto callbacks = GetFunctionsWithTag(moduleEmitter, c_callbackFunctionTagName);
        auto swigFunctions = GetFunctionsWithTag(moduleEmitter, c_swigFunctionTagName);

        // Dependencies
        {
            DeclareIfndefSwig ifndefSwig(os);

            // Callbacks, if present
            if (!callbacks.empty())
            {
                os << "#include \"CallbackInterface.h\"\n";
            }

            // Module definitions (a.k.a. the C/C++ header)
            WriteModuleHeader(os, moduleEmitter);
        }

        // Predict functions
        for (const auto& p : predicts)
        {
            PredictInterfaceWriter writer(moduleEmitter, *(p.function));
            writer.WriteHeaderCode(os);
        }

        // Callbacks
        if (!callbacks.empty())
        {
            std::string predictFunctionName = (predicts[0].function)->getName();

            SteppablePredictorInterfaceWriter writer(moduleEmitter, predictFunctionName, callbacks);
            writer.WriteHeaderCode(os);
        }

        // General functions
        for (const auto& p : swigFunctions)
        {
            WriteFunctionDeclaration(os, moduleEmitter, *(p.function));
            os << "\n\n";
        }
    }

    void WriteModuleSwigInterface(std::ostream& os, IRModuleEmitter& moduleEmitter, const std::string& headerName)
    {
        auto pModule = moduleEmitter.GetLLVMModule();
        std::string moduleName = pModule->getName();

        os << "//\n// ELL SWIG interface for module " << moduleName << "\n//\n\n";

        WriteCommonSwigCode(os, moduleEmitter);

        WriteModuleSwigCode(os, moduleEmitter, headerName);
    }
}
}
