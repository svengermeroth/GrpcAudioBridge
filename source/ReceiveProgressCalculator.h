#pragma once

#include <cstdint>
#include <string>
#include <regex>

namespace ProgressCalculation
{
    // Derives a monotonic 0..100 progress from the server's "[_PROGRESS|<pct>|<text>_]" markers.
    // The server's own percentages jump backwards, so the segment texts are used instead:
    //   0..15  preparation / VAD (server 0..20 mapped onto it)
    //   15..97 segments, each 82/N; within a segment Whisper 0..70 %, alignment 70..100 %
    //   99     "Finalizing results"
    //   100    "[_END_]"

    struct ReceiveProgressCalculator
    {
        auto onProgress(int serverPercent, const std::string& text) -> int;
        auto complete() -> int;

    private:
        // Segment index is 1-based; 'within' is the fraction of that segment's share.
        auto segmentAt(const std::smatch& m, double within) -> double;

        // Caps at 99 until complete(), so 100 is reported exactly once.
        auto advance(double value) -> int;

        int m_total = 0;
        int m_current = 0;

        friend class ReceiveProgressCalculatorTest;
    };
} // namespace ProgressCalculation
