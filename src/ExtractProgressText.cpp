#include "ExtractProgressText.h"

namespace ProgressCalculation
{
    std::string extractProgressText(const std::string& word)
    {
        const size_t first = word.find('|');
        const size_t second = first == std::string::npos ? std::string::npos : word.find('|', first + 1);
        if (second == std::string::npos)
            return {};
        std::string text = word.substr(second + 1);
        if (text.size() >= 2 && text.compare(text.size() - 2, 2, "_]") == 0)
            text.resize(text.size() - 2);
        return text;
    }
} // namespace ProgressCalculation
