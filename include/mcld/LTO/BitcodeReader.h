#ifndef MCLD_LTO_BITCODEREADER_H_
#define MCLD_LTO_BITCODEREADER_H_

#include "mcld/LD/LDReader.h"

namespace mcld
{

class Input;

class IRBuilder;
class LTOContext;

class BitcodeReader : public LDReader
{
protected:
    BitcodeReader() {}
public:
    virtual bool readSymbols(Input& pFile) = 0;
};

};

#endif