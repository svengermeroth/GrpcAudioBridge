#pragma once

#include <string>
#include <vector>

namespace ProgressCalculation
{
    struct ExtractProgressTextTestData {
        std::string input;
        std::string output;
    };

    std::vector<ExtractProgressTextTestData> extractProgressTextTestData = {
        {"[_PROGRESS|0|Starting audio processing_]", "Starting audio processing"},
        {"[_PROGRESS|10|Running voice activity detection_]", "Running voice activity detection"},
        {"[_PROGRESS|8|validating audio format..._]", "validating audio format..."},
        {"[_PROGRESS|20|Detected 3 speech segments_]", "Detected 3 speech segments"},
        {"[_PROGRESS|30|Processing 3 segments_]", "Processing 3 segments"},
        {"[_PROGRESS|50|Processing segment 1/3_]", "Processing segment 1/3"},
        {"[_PROGRESS|40|Aligning words for segment 1/3_]", "Aligning words for segment 1/3"},
        {"[_PROGRESS|70|Processing segment 2/3_]", "Processing segment 2/3"},
        {"[_PROGRESS|90|Processing segment 3/3_]", "Processing segment 3/3"},
        {"[_PROGRESS|80|Aligning words for segment 3/3_]", "Aligning words for segment 3/3"},
        {"[_PROGRESS|100|Processed 3/3 segments_]", "Processed 3/3 segments"},
        {"[_PROGRESS|95|Finalizing results_]", "Finalizing results"},
        {"[_PROGRESS|100|Processing completed_]", "Processing completed"},
    };
} // namespace ProgressCalculation
