//===- Attribute.h --------------------------------------------------------===//
//
//                     The MCLinker Project
//
// This file is distributed under the Nakada Tokumei
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
#ifndef MCLD_LTO_LTOINPUT_H_
#define MCLK_LTO_LTOINPUT_H_

#include <memory>
#include <vector>

// ===================== Forward Declaring =====================
namespace llvm
{
namespace lto
{
class InputFile;
};

class MemoryBuffer;
};

namespace mcld
{
class Input;
class LDSymbol;
}
// ===================== Forward Declaring =====================

namespace mcld
{

class LTOInput
{
    Input& m_Input;
    std::unique_ptr<llvm::lto::InputFile> m_LTOFile;
    std::unique_ptr<llvm::MemoryBuffer> m_BackingBuffer;

    std::vector<LDSymbol*> m_Symbols;

public:
    LTOInput(
        Input& input,
        std::unique_ptr<llvm::lto::InputFile> ltoFile,
        std::unique_ptr<llvm::MemoryBuffer> backingBuffer
    )   :   m_Input(input),
            m_LTOFile(std::move(ltoFile)),
            m_BackingBuffer(std::move(backingBuffer))
    { }

    Input& input() { return m_Input; }
    llvm::lto::InputFile& ltoFile() { return *m_LTOFile; }
    
    std::vector<LDSymbol*>& symbols() { return m_Symbols; }
};

};

#endif