#ifndef MCLD_LTO_LTOCONTEXT_H_
#define MCLD_LTO_LTOCONTEXT_H_
#include <memory>
#include <vector>

namespace mcld
{
class LTOInput;

class LTOContext
{
    std::vector<std::unique_ptr<LTOInput>> m_Inputs;

public:
    void add(std::unique_ptr<LTOInput> input) {
        m_Inputs.push_back(std::move(input));
    }

    bool empty() const { return m_Inputs.empty(); }

    std::vector<std::unique_ptr<LTOInput>>& inputs() {
        return m_Inputs;
    }
};
};
#endif