#include "ReceiveProgressCalculator.h"

namespace ProgressCalculation
{
    namespace
    {
        constexpr double kPrepEnd = 15.0;
        constexpr double kSegmentsEnd = 97.0;
        constexpr double kFinalizing = 99.0;
        constexpr double kWhisperShare = 0.7; // part of a segment before "Aligning"
    }

    int ReceiveProgressCalculator::onProgress(int serverPercent, const std::string& text)
    {
        static const std::regex detected(R"(^Detected (\d+) speech segments$)");
        static const std::regex processing(R"(^Processing segment (\d+)/(\d+)$)");
        static const std::regex aligning(R"(^Aligning words for segment (\d+)/(\d+)$)");
        static const std::regex processed(R"(^Processed (\d+)/(\d+) segments$)");

        std::smatch m;
        if (std::regex_match(text, m, detected))
        {
            m_total = std::stoi(m[1].str());
            return advance(kPrepEnd);
        }
        if (std::regex_match(text, m, processing))
            return advance(segmentAt(m, 0.0));
        if (std::regex_match(text, m, aligning))
            return advance(segmentAt(m, kWhisperShare));
        if (std::regex_match(text, m, processed))
            return advance(segmentAt(m, 1.0));
        if (text == "Finalizing results")
            return advance(kFinalizing);

        // Preparation phase only; unknown texts after it are ignored.
        if (m_total == 0)
            return advance(kPrepEnd * std::clamp(serverPercent, 0, 20) / 20.0);

        return -1;
    }

    int ReceiveProgressCalculator::complete()
    {
        if (m_current >= 100)
            return -1;
        m_current = 100;
        return m_current;
    }

    double ReceiveProgressCalculator::segmentAt(const std::smatch& m, double within)
    {
        const int index = std::stoi(m[1].str());
        m_total = std::stoi(m[2].str());
        return kPrepEnd + (kSegmentsEnd - kPrepEnd) * ((index - 1 + within) / std::max(m_total, 1));
    }

    int ReceiveProgressCalculator::advance(double value)
    {
        const int v = static_cast<int>(std::min(value, 99.0));
        if (v <= m_current)
            return -1;
        m_current = v;
        return v;
    }
} // namespace ProgressCalculation
