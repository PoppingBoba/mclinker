add_llvm_library(MCLDLTO
    LTOReader.cpp
    LLVMBitcodeReader.cpp
    LINK_LIBS
    MCLDSupport
)