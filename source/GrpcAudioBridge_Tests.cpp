#include "GrpcAudioBridge.h"

#include <grpcpp/grpcpp.h>
#include "whisper.grpc.pb.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

// Acceptance tests: drive the public C API (Gab_Create / Gab_StreamFile / Gab_Cancel / Gab_Destroy)
// against an in-process fake Whisper gRPC server listening on localhost.
namespace
{
    using namespace std::chrono_literals;

    constexpr auto kTimeout = 5s;
    constexpr auto kServerHoldLimit = 10s; // safety net so a broken cancel cannot hang the test run forever

    Whisper::StreamingWordsResponse response(const std::string& word, const std::string& start = "", const std::string& end = "")
    {
        Whisper::StreamingWordsResponse r;
        r.set_word(word);
        r.set_start_time(start);
        r.set_end_time(end);
        return r;
    }

    // Collects every callback as a readable event string, in call order.
    struct CallbackRecorder
    {
        std::mutex mutex;
        std::vector<std::string> events;
        std::vector<std::string> logs;

        // Optional hooks, run after the event has been recorded (outside the lock).
        std::function<void()> onStartedHook;
        std::function<void()> onTranscriptionStartedHook;
        std::function<void()> onFinishedHook;

        void add(const std::string& event)
        {
            std::lock_guard<std::mutex> lock(mutex);
            events.push_back(event);
        }

        std::vector<std::string> snapshot()
        {
            std::lock_guard<std::mutex> lock(mutex);
            return events;
        }

        bool hasLogContaining(const std::string& text)
        {
            std::lock_guard<std::mutex> lock(mutex);
            return std::any_of(logs.begin(), logs.end(), [&](const std::string& l) { return l.find(text) != std::string::npos; });
        }

        static CallbackRecorder& self(void* user) { return *static_cast<CallbackRecorder*>(user); }

        GabCallbacks callbacks()
        {
            GabCallbacks cb{};
            cb.user = this;
            cb.onLog = [](void* user, const char* message)
            {
                auto& r = self(user);
                std::lock_guard<std::mutex> lock(r.mutex);
                r.logs.emplace_back(message);
            };
            cb.onWord = [](void* user, const char* word, const char* start, const char* end)
            {
                self(user).add(std::string("word:") + word + "@" + start + "-" + end);
            };
            cb.onReceiveProgress = [](void* user, int64_t progress)
            {
                self(user).add("receive:" + std::to_string(progress));
            };
            cb.onTransmitProgress = [](void* user, int64_t progress)
            {
                self(user).add("transmit:" + std::to_string(progress));
            };
            cb.onStarted = [](void* user)
            {
                auto& r = self(user);
                r.add("started");
                if (r.onStartedHook)
                    r.onStartedHook();
            };
            cb.onTranscriptionStarted = [](void* user)
            {
                auto& r = self(user);
                r.add("transcriptionStarted");
                if (r.onTranscriptionStartedHook)
                    r.onTranscriptionStartedHook();
            };
            cb.onTranscriptionFinished = [](void* user)
            {
                self(user).add("transcriptionFinished");
            };
            cb.onFinished = [](void* user)
            {
                auto& r = self(user);
                r.add("finished");
                if (r.onFinishedHook)
                    r.onFinishedHook();
            };
            return cb;
        }
    };

    // Fake server: reads the whole audio upload, then replays a scripted list of responses.
    // With holdUntilCancelled it keeps the stream open afterwards until the client cancels.
    class FakeWhisperService final : public Whisper::AudioStream::Service
    {
    public:
        std::vector<Whisper::StreamingWordsResponse> responses;
        bool holdUntilCancelled = false;

        grpc::Status StreamAudio(grpc::ServerContext* context,
                                 grpc::ServerReaderWriter<Whisper::StreamingWordsResponse, Whisper::AudioChunk>* stream) override
        {
            std::string received;
            Whisper::AudioChunk chunk;
            while (stream->Read(&chunk))
                received += chunk.data();

            {
                std::lock_guard<std::mutex> lock(m_mutex);
                m_received = received;
            }

            for (const auto& r : responses)
            {
                if (!stream->Write(r))
                    break;
            }

            if (!holdUntilCancelled)
                return grpc::Status::OK;

            const auto deadline = std::chrono::steady_clock::now() + kServerHoldLimit;
            while (!context->IsCancelled() && std::chrono::steady_clock::now() < deadline)
                std::this_thread::sleep_for(5ms);

            std::lock_guard<std::mutex> lock(m_mutex);
            m_sawCancel = context->IsCancelled();
            return grpc::Status::CANCELLED;
        }

        std::string received()
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            return m_received;
        }

        bool sawCancel()
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            return m_sawCancel;
        }

    private:
        std::mutex m_mutex;
        std::string m_received;
        bool m_sawCancel = false;
    };

    class GrpcAudioBridgeTest : public ::testing::Test
    {
    protected:
        // 4 full chunks -> transmit progress 25, 50, 75, 100.
        static constexpr size_t kAudioSize = 4 * 4096;

        void SetUp() override
        {
            grpc::ServerBuilder builder;
            builder.AddListeningPort("127.0.0.1:0", grpc::InsecureServerCredentials(), &m_port);
            builder.RegisterService(&m_service);
            m_server = builder.BuildAndStart();
            ASSERT_NE(nullptr, m_server);
            ASSERT_NE(0, m_port);

            m_audio.resize(kAudioSize);
            for (size_t i = 0; i < m_audio.size(); ++i)
                m_audio[i] = static_cast<char>(i % 251);

            const auto* info = ::testing::UnitTest::GetInstance()->current_test_info();
            m_audioPath = std::filesystem::temp_directory_path() / (std::string("gab_") + info->name() + ".raw");
            std::ofstream(m_audioPath, std::ios::binary).write(m_audio.data(), static_cast<std::streamsize>(m_audio.size()));
        }

        void TearDown() override
        {
            stopServer();
            std::error_code ignored;
            std::filesystem::remove(m_audioPath, ignored);
        }

        // Waits for running server handlers, so their recorded state can be inspected afterwards.
        void stopServer()
        {
            if (m_server)
            {
                m_server->Shutdown(std::chrono::system_clock::now() + 2s);
                m_server.reset();
            }
        }

        std::string address() const { return "127.0.0.1:" + std::to_string(m_port); }
        std::string audioPath() const { return m_audioPath.string(); }

        void useRegularTranscript()
        {
            m_service.responses = {
                response("[_BEG_]"),
                response("[_PROGRESS|20|Detected 1 speech segments_]", "20"),
                response("Hello", "0.00", "0.50"),
                response("[_PROGRESS|40|Aligning words for segment 1/1_]", "40"),
                response("world", "0.50", "1.00"),
                response("[_END_]"),
            };
        }

        static std::vector<std::string> regularTranscriptEvents()
        {
            return {
                "started",
                "transmit:25", "transmit:50", "transmit:75", "transmit:100",
                "transcriptionStarted",
                "receive:15",
                "word:Hello@0.00-0.50",
                "receive:72",
                "word:world@0.50-1.00",
                "receive:100",
                "transcriptionFinished",
                "finished",
            };
        }

        FakeWhisperService m_service;
        std::unique_ptr<grpc::Server> m_server;
        int m_port = 0;
        std::string m_audio;
        std::filesystem::path m_audioPath;
        CallbackRecorder m_recorder;
    };

    // --- StreamFile ------------------------------------------------------------------------------

    TEST_F(GrpcAudioBridgeTest, StreamFile_UploadsWholeFileAndDeliversWords)
    {
        useRegularTranscript();
        const GabCallbacks callbacks = m_recorder.callbacks();
        GabSession session = Gab_Create(address().c_str(), &callbacks);

        Gab_StreamFile(session, audioPath().c_str());
        Gab_Destroy(session);
        stopServer();

        EXPECT_EQ(m_audio, m_service.received());

        std::vector<std::string> words;
        for (const auto& e : m_recorder.snapshot())
            if (e.rfind("word:", 0) == 0)
                words.push_back(e);
        EXPECT_EQ((std::vector<std::string>{ "word:Hello@0.00-0.50", "word:world@0.50-1.00" }), words);
        EXPECT_FALSE(m_recorder.hasLogContaining("gRPC error"));
    }

    TEST_F(GrpcAudioBridgeTest, StreamFile_HttpPrefixedAddressIsAccepted)
    {
        useRegularTranscript();
        const GabCallbacks callbacks = m_recorder.callbacks();
        GabSession session = Gab_Create(("http://" + address()).c_str(), &callbacks);

        Gab_StreamFile(session, audioPath().c_str());
        Gab_Destroy(session);

        EXPECT_EQ(regularTranscriptEvents(), m_recorder.snapshot());
    }

    TEST_F(GrpcAudioBridgeTest, StreamFile_WithoutCallbacksStillStreams)
    {
        useRegularTranscript();
        GabSession session = Gab_Create(address().c_str(), nullptr);

        Gab_StreamFile(session, audioPath().c_str());
        Gab_Destroy(session);
        stopServer();

        EXPECT_EQ(m_audio, m_service.received());
    }

    TEST_F(GrpcAudioBridgeTest, StreamFile_UnreachableServerStillFinishes)
    {
        const GabCallbacks callbacks = m_recorder.callbacks();
        GabSession session = Gab_Create("127.0.0.1:1", &callbacks);

        Gab_StreamFile(session, audioPath().c_str());
        Gab_Destroy(session);

        const auto events = m_recorder.snapshot();
        ASSERT_FALSE(events.empty());
        EXPECT_EQ("started", events.front());
        EXPECT_EQ("finished", events.back());
        EXPECT_EQ(1, std::count(events.begin(), events.end(), "finished"));
        EXPECT_EQ(0, std::count(events.begin(), events.end(), "transcriptionFinished"));
        EXPECT_TRUE(m_recorder.hasLogContaining("gRPC error"));
    }

    // --- Callback order --------------------------------------------------------------------------

    TEST_F(GrpcAudioBridgeTest, Callbacks_FireInExpectedOrder)
    {
        // started -> transmit (upload) -> transcriptionStarted -> words/receive progress -> 100 -> transcriptionFinished -> finished
        useRegularTranscript();
        const GabCallbacks callbacks = m_recorder.callbacks();
        GabSession session = Gab_Create(address().c_str(), &callbacks);

        Gab_StreamFile(session, audioPath().c_str());
        Gab_Destroy(session);

        EXPECT_EQ(regularTranscriptEvents(), m_recorder.snapshot());
    }

    TEST_F(GrpcAudioBridgeTest, Callbacks_FinishedFiresOnceOnCallingThread)
    {
        // onFinished is the last callback and fires exactly once, on the thread that called Gab_StreamFile.
        useRegularTranscript();
        std::thread::id finishedThread;
        m_recorder.onFinishedHook = [&] { finishedThread = std::this_thread::get_id(); };
        const GabCallbacks callbacks = m_recorder.callbacks();
        GabSession session = Gab_Create(address().c_str(), &callbacks);

        Gab_StreamFile(session, audioPath().c_str());
        Gab_Destroy(session);

        const auto events = m_recorder.snapshot();
        EXPECT_EQ(1, std::count(events.begin(), events.end(), "finished"));
        EXPECT_EQ("finished", events.back());
        EXPECT_EQ(std::this_thread::get_id(), finishedThread);
    }

    // --- Cancel ----------------------------------------------------------------------------------

    TEST_F(GrpcAudioBridgeTest, Cancel_DuringTranscriptionStopsStreamFile)
    {
        // Server announces the transcription, then never ends the stream on its own.
        m_service.responses = { response("[_BEG_]") };
        m_service.holdUntilCancelled = true;

        std::promise<void> transcriptionStarted;
        m_recorder.onTranscriptionStartedHook = [&] { transcriptionStarted.set_value(); };
        const GabCallbacks callbacks = m_recorder.callbacks();
        GabSession session = Gab_Create(address().c_str(), &callbacks);

        const auto begin = std::chrono::steady_clock::now();
        std::thread worker([&] { Gab_StreamFile(session, audioPath().c_str()); });

        const bool started = transcriptionStarted.get_future().wait_for(kTimeout) == std::future_status::ready;
        Gab_Cancel(session);
        worker.join();
        const auto elapsed = std::chrono::steady_clock::now() - begin;
        Gab_Destroy(session);
        stopServer();

        ASSERT_TRUE(started);
        EXPECT_LT(elapsed, kTimeout) << "Gab_StreamFile did not return promptly after Gab_Cancel";
        EXPECT_TRUE(m_service.sawCancel());
        EXPECT_EQ((std::vector<std::string>{
                      "started",
                      "transmit:25", "transmit:50", "transmit:75", "transmit:100",
                      "transcriptionStarted",
                      "finished",
                  }),
                  m_recorder.snapshot());
        EXPECT_TRUE(m_recorder.hasLogContaining("gRPC error"));
    }

    TEST_F(GrpcAudioBridgeTest, Cancel_RightAfterStartSkipsTranscription)
    {
        // onStarted documents that the stream is cancelable from that point on.
        useRegularTranscript();
        GabSession session = nullptr;
        m_recorder.onStartedHook = [&] { Gab_Cancel(session); };
        const GabCallbacks callbacks = m_recorder.callbacks();
        session = Gab_Create(address().c_str(), &callbacks);

        Gab_StreamFile(session, audioPath().c_str());
        Gab_Destroy(session);
        stopServer();

        const auto events = m_recorder.snapshot();
        ASSERT_FALSE(events.empty());
        EXPECT_EQ("started", events.front());
        EXPECT_EQ("finished", events.back());
        EXPECT_EQ(1, std::count(events.begin(), events.end(), "finished"));
        for (const auto& e : events)
        {
            EXPECT_NE(0u, e.rfind("word:", 0)) << e;
            EXPECT_NE("transcriptionStarted", e);
            EXPECT_NE("transcriptionFinished", e);
        }
        EXPECT_LT(m_service.received().size(), m_audio.size());
    }

    TEST_F(GrpcAudioBridgeTest, Cancel_WhenIdleIsNoOp)
    {
        useRegularTranscript();
        const GabCallbacks callbacks = m_recorder.callbacks();
        GabSession session = Gab_Create(address().c_str(), &callbacks);

        Gab_Cancel(session);
        Gab_StreamFile(session, audioPath().c_str());
        Gab_Cancel(session);
        Gab_Destroy(session);

        EXPECT_EQ(regularTranscriptEvents(), m_recorder.snapshot());
    }

    TEST_F(GrpcAudioBridgeTest, Cancel_NullSessionIsNoOp)
    {
        Gab_Cancel(nullptr);
        Gab_StreamFile(nullptr, audioPath().c_str());
        Gab_Destroy(nullptr);
    }
} // namespace
