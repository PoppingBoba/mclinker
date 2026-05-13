#include "mcld/LTO/LLVMBitcodeReader.h"
#include "mcld/LTO/LTOInput.h"
#include "mcld/LTO/LTOContext.h"
#include "mcld/Support/MemoryArea.h"
#include "mcld/IRBuilder.h"

#include "llvm/ADT/StringRef.h"
#include "llvm/LTO/LTO.h"
#include "llvm/Support/Error.h"
#include "llvm/Support/MemoryBuffer.h"

namespace mcld 
{

LLVMBitcodeReader::LLVMBitcodeReader(
    IRBuilder& builder,
    LTOContext& ltoContext
) : m_Builder(builder),
    m_LTOContext(ltoContext)
{ }

bool LLVMBitcodeReader::isMyFormat(
    Input& input, 
    bool& doContinue)
{
    doContinue = true;

    if (!input.hasMemArea())
        return false;

    auto* area = input.memArea();

    auto data = area->request(0, area->size());

    auto ref = llvm::MemoryBufferRef(
        llvm::StringRef(data.data(), data.size()),
        input.name()
    );

    auto bitcodeFileOrErr = llvm::lto::InputFile::create(ref);
    if (!bitcodeFileOrErr)
    {
        llvm::consumeError(bitcodeFileOrErr.takeError());
        return false;
    }

    return true;
}

bool LLVMBitcodeReader::readSymbols(Input& input)
{
    return true;
}

};