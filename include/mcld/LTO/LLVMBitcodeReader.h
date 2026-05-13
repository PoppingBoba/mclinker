#ifndef MCLD_LTO_LLVMBITCODEREADER_H_
#define MCLD_LTO_LLVMBITCODEREADER_H_

#include "mcld/LTO/BitcodeReader.h"

namespace mcld
{

class LLVMBitcodeReader : public BitcodeReader
{
    IRBuilder& m_Builder;
    LTOContext& m_LTOContext;

public:
    LLVMBitcodeReader(
        IRBuilder& builder,
        LTOContext& LTOContext
    );

    bool isMyFormat(Input& input, bool& doContinue);
    bool readSymbols(Input& input);
};

};


#endif