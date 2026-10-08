#pragma once

#include <string>

namespace ProgressCalculation
{
    // "[_PROGRESS|20|Detected 3 speech segments_]" -> "Detected 3 speech segments"
    std::string extractProgressText(const std::string& word);
} // namespace ProgressCalculation
