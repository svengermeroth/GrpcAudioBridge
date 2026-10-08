#include "ExtractProgressText.h"
#include "ExtractProgressText_TestData.h"

#include <gtest/gtest.h>

#include <cctype>

namespace ProgressCalculation
{
    class ExtractProgressTextTest : public testing::TestWithParam<ExtractProgressTextTestData> {};

    // Add new cases to extractProgressTextTestData in ExtractProgressText_TestData.h.
    INSTANTIATE_TEST_SUITE_P(ProgressCalculation, ExtractProgressTextTest,
        testing::ValuesIn(extractProgressTextTestData),
        [](const testing::TestParamInfo<ExtractProgressTextTestData>& info)
        {
            // GoogleTest only allows [A-Za-z0-9_] in test names.
            std::string name = info.param.output;
            for (auto& c : name)
            {
                if (!isalnum(static_cast<unsigned char>(c)))
                    c = '_';
            }

            return name.empty() ? "Empty_" + std::to_string(info.index) : name;
        }
    );

    // --- Preparation phase (server 0..20 mapped onto 0..15) ---------------------------------------

    TEST_P(ExtractProgressTextTest, extractCorrectStringPart)
    {
        const auto& testData = GetParam();
        
        auto result = extractProgressText(testData.input);

        EXPECT_EQ(result, testData.output);
    }
} // namespace ProgressCalculation
