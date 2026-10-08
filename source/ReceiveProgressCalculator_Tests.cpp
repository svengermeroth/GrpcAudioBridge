#include "ReceiveProgressCalculator.h"

#include <gtest/gtest.h>

#include <string>
#include <vector>

namespace ProgressCalculation
{
    // Fixture: every TEST_F gets a fresh calculator.
    // Friendship is not inherited by the TEST_F classes, so private state is reached through the helpers below.
    class ReceiveProgressCalculatorTest : public ::testing::Test
    {
    protected:
        static constexpr int kInvalidProgress = -1;

        struct Step
        {
            int serverPercent;
            std::string text;
            int expected;
        };

        void SetUp() override
        {
        }

        void TearDown() override
        {
        }

        // Shortcut for one server marker "[_PROGRESS|<serverPercent>|<text>_]".
        int progress(int serverPercent, const std::string& text)
        {
            return m_calculator.onProgress(serverPercent, text);
        }

        // Feeds the steps in order and checks each reported value plus monotonic growth.
        void replay(const std::vector<Step>& steps)
        {
            int last = current();
            for (const Step& step : steps)
            {
                SCOPED_TRACE("[" + std::to_string(step.serverPercent) + "] " + step.text);
                const int reported = progress(step.serverPercent, step.text);
                EXPECT_EQ(step.expected, reported);
                EXPECT_GE(current(), last);
                last = current();
            }
        }

        int current() const { return m_calculator.m_current; }
        int total() const { return m_calculator.m_total; }
        void setCurrent(int value) { m_calculator.m_current = value; }
        void setTotal(int value) { m_calculator.m_total = value; }
        int advance(double value) { return m_calculator.advance(value); }

        ReceiveProgressCalculator m_calculator;
    };

    // --- Preparation phase (server 0..20 mapped onto 0..15) ---------------------------------------

    TEST_F(ReceiveProgressCalculatorTest, Preparation_ZeroPercent_ReportsNothing)
    {
        auto value = progress(0, "Starting audio processing");

        EXPECT_EQ(kInvalidProgress, value);
        EXPECT_EQ(0, current());
    }

    TEST_F(ReceiveProgressCalculatorTest, Preparation_ServerPercentIsScaledToFifteen)
    {
        EXPECT_EQ(7, progress(10, "Running voice activity detection"));
        EXPECT_EQ(9, progress(12, "Downloading VAD model..."));
    }

    TEST_F(ReceiveProgressCalculatorTest, Preparation_BackwardServerValueIsIgnored)
    {
        progress(10, "Running voice activity detection");

        EXPECT_EQ(kInvalidProgress, progress(8, "Validating audio format..."));
        EXPECT_EQ(7, current());
    }

    TEST_F(ReceiveProgressCalculatorTest, Preparation_ServerPercentOutOfRangeIsClamped)
    {
        EXPECT_EQ(kInvalidProgress, progress(-5, "Starting audio processing"));
        EXPECT_EQ(15, progress(50, "Some unknown preparation text"));
    }

    TEST_F(ReceiveProgressCalculatorTest, DetectedSegments_JumpsToEndOfPreparationAndStoresTotal)
    {
        EXPECT_EQ(15, progress(20, "Detected 3 speech segments"));
        EXPECT_EQ(3, total());
    }

    // --- Segment phase (15..97, each segment 82/N) -----------------------------------------------

    TEST_F(ReceiveProgressCalculatorTest, SingleSegment_WhisperThenAlignmentThenDone)
    {
        replay({
            {20, "Detected 1 speech segments", 15},
            {90, "Processing segment 1/1", kInvalidProgress}, // start of segment 1 == 15
            {60, "Aligning words for segment 1/1", 72}, // 15 + 0.7 * 82
            {100, "Processed 1/1 segments", 97},
        });
    }

    TEST_F(ReceiveProgressCalculatorTest, ThreeSegments_EachSegmentGetsEqualShare)
    {
        replay({
            {20, "Detected 3 speech segments", 15},
            {50, "Processing segment 1/3", kInvalidProgress},
            {40, "Aligning words for segment 1/3", 34},   // 15 + 0.7/3 * 82
            {33, "Processed 1/3 segments", 42},           // 15 + 1/3 * 82
            {70, "Processing segment 2/3", kInvalidProgress},    // == end of segment 1
            {60, "Aligning words for segment 2/3", 61},   // 15 + 1.7/3 * 82
            {66, "Processed 2/3 segments", 69},           // 15 + 2/3 * 82
            {90, "Processing segment 3/3", kInvalidProgress},
            {80, "Aligning words for segment 3/3", 88},   // 15 + 2.7/3 * 82
            {100, "Processed 3/3 segments", 97},
        });
    }

    TEST_F(ReceiveProgressCalculatorTest, SkippedSegment_NextProcessingClosesPreviousSegment)
    {
        // Segment 2 is [BLANK_AUDIO] on the server: no "Aligning" and no "Processed" for it.
        setCurrent(42);
        setTotal(3);

        replay({
            {70, "Processing segment 2/3", kInvalidProgress},
            {90, "Processing segment 3/3", 69},
        });
    }

    TEST_F(ReceiveProgressCalculatorTest, SegmentWithoutAlignment_ProcessedFollowsDirectly)
    {
        // No Wav2Vec2 for the language: server sends no "Aligning" message.
        replay({
            {20, "Detected 2 speech segments", 15},
            {60, "Processing segment 1/2", kInvalidProgress},
            {50, "Processed 1/2 segments", 56},           // 15 + 1/2 * 82
        });
    }

    TEST_F(ReceiveProgressCalculatorTest, SegmentMessage_SetsTotalWithoutDetectedMessage)
    {
        EXPECT_EQ(56, progress(50, "Processed 1/2 segments"));
        EXPECT_EQ(2, total());
    }

    TEST_F(ReceiveProgressCalculatorTest, ManySegments_SmallStepsStayMonotonic)
    {
        progress(20, "Detected 100 speech segments");

        int last = current();
        for (int i = 1; i <= 100; ++i)
        {
            const std::string index = std::to_string(i) + "/100";
            progress(0, "Processing segment " + index);
            progress(0, "Aligning words for segment " + index);
            progress(0, "Processed " + index + " segments");
            EXPECT_GE(current(), last) << "segment " << i;
            last = current();
        }
        EXPECT_EQ(97, current());
    }

    TEST_F(ReceiveProgressCalculatorTest, UnknownTextAfterPreparation_IsIgnored)
    {
        setCurrent(42);
        setTotal(3);

        EXPECT_EQ(kInvalidProgress, progress(30, "GPU inference failed; retrying segment on CPU"));
        EXPECT_EQ(kInvalidProgress, progress(30, "Processing 3 segments"));
        EXPECT_EQ(42, current());
    }

    // --- End phase ------------------------------------------------------------------------------

    TEST_F(ReceiveProgressCalculatorTest, Finalizing_ReportsNinetyNine)
    {
        setCurrent(97);
        setTotal(3);

        EXPECT_EQ(99, progress(95, "Finalizing results"));
    }

    TEST_F(ReceiveProgressCalculatorTest, Advance_IsCappedAtNinetyNine)
    {
        EXPECT_EQ(99, advance(150.0));
        EXPECT_EQ(kInvalidProgress, advance(100.0));
    }

    TEST_F(ReceiveProgressCalculatorTest, Complete_ReportsHundredOnce)
    {
        EXPECT_EQ(100, m_calculator.complete());
        EXPECT_EQ(kInvalidProgress, m_calculator.complete());
    }

    TEST_F(ReceiveProgressCalculatorTest, AfterComplete_FurtherProgressIsIgnored)
    {
        setTotal(3);
        m_calculator.complete();

        EXPECT_EQ(kInvalidProgress, progress(100, "Processing completed"));
        EXPECT_EQ(kInvalidProgress, progress(95, "Finalizing results"));
        EXPECT_EQ(100, current());
    }

    TEST_F(ReceiveProgressCalculatorTest, NoSpeech_JumpsFromPreparationToComplete)
    {
        EXPECT_EQ(15, progress(20, "Detected 0 speech segments"));
        EXPECT_EQ(100, m_calculator.complete());
    }

    // --- Full run, recorded from the client log --------------------------------------------------

    TEST_F(ReceiveProgressCalculatorTest, RecordedServerLog_ThreeSegments_IsMonotonicFromZeroToHundred)
    {
        replay({
            {0, "Starting audio processing", kInvalidProgress},
            {10, "Running voice activity detection", 7},
            {8, "Validating audio format...", kInvalidProgress},
            {20, "Detected 3 speech segments", 15},
            {30, "Processing 3 segments", kInvalidProgress},
            {50, "Processing segment 1/3", kInvalidProgress},
            {40, "Aligning words for segment 1/3", 34},
            {33, "Processed 1/3 segments", 42},
            {70, "Processing segment 2/3", kInvalidProgress},
            {90, "Processing segment 3/3", 69},
            {80, "Aligning words for segment 3/3", 88},
            {100, "Processed 3/3 segments", 97},
            {95, "Finalizing results", 99},
        });

        EXPECT_EQ(100, m_calculator.complete());
        EXPECT_EQ(kInvalidProgress, progress(100, "Processing completed"));
    }
} // namespace ProgressCalculation
