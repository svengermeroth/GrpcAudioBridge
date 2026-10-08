#include "GrpcAudioBridge.h"

#include <grpcpp/grpcpp.h>
#include "whisper.grpc.pb.h"

#include <fstream>
#include <mutex>
#include <string>

#include "ReceiveProgressCalculator.h"
#include "ExtractProgressText.h"

namespace
{
    bool startsWithIgnoreCase(const std::string& value, const char* prefix)
    {
        const size_t prefixLen = strlen(prefix);
        if (value.size() < prefixLen)
            return false;
        for (size_t i = 0; i < prefixLen; ++i)
        {
            if (tolower(static_cast<unsigned char>(value[i])) != tolower(static_cast<unsigned char>(prefix[i])))
                return false;
        }
        return true;
    }
}

struct GabSessionImpl
{
    std::string serverAddress;
    GabCallbacks callbacks{};

    std::mutex contextMutex;
    grpc::ClientContext* context = nullptr;

    void log(const std::string& message) const
    {
        if (callbacks.onLog)
            callbacks.onLog(callbacks.user, message.c_str());
    }
};

GAB_API GabSession Gab_Create(const char* serverAddress, const GabCallbacks* callbacks)
{
    auto* session = new GabSessionImpl();
    session->serverAddress = serverAddress != nullptr ? serverAddress : "";
    if (callbacks != nullptr)
        session->callbacks = *callbacks;
    return session;
}

GAB_API void Gab_StreamFile(GabSession sessionHandle, const char* audioFilePath)
{
    auto* session = static_cast<GabSessionImpl*>(sessionHandle);
    if (session == nullptr)
        return;

    session->log("STT gRPC client startup");

    std::string serverAddress = session->serverAddress;
    const bool isHttps = startsWithIgnoreCase(serverAddress, "https://");
    const bool isHttp = startsWithIgnoreCase(serverAddress, "http://");
    if (!isHttp && !isHttps)
        serverAddress = "http://" + serverAddress;

    session->log("Connecting to server " + serverAddress + "...");

    std::string target = serverAddress;
    if (startsWithIgnoreCase(target, "https://"))
        target = target.substr(8);
    else if (startsWithIgnoreCase(target, "http://"))
        target = target.substr(7);

    std::shared_ptr<grpc::ChannelCredentials> credentials = isHttps
        ? grpc::SslCredentials(grpc::SslCredentialsOptions())
        : grpc::InsecureChannelCredentials();

    std::shared_ptr<grpc::Channel> channel = grpc::CreateChannel(target, credentials);
    std::unique_ptr<Whisper::AudioStream::Stub> client = Whisper::AudioStream::NewStub(channel);

    grpc::ClientContext context;
    {
        std::lock_guard<std::mutex> lock(session->contextMutex);
        session->context = &context;
    }

    std::unique_ptr<grpc::ClientReaderWriter<Whisper::AudioChunk, Whisper::StreamingWordsResponse>> stream(client->StreamAudio(&context));

    session->log(std::string("Sending audio file: ") + (audioFilePath != nullptr ? audioFilePath : ""));

    ProgressCalculation::ReceiveProgressCalculator receiveProgressCalculator;
    auto reportReceiveProgress = [session](int value)
    {
        auto isValidProgress = value >= 0;
        if (not isValidProgress)
            return;

        if (session->callbacks.onReceiveProgress)
            session->callbacks.onReceiveProgress(session->callbacks.user, value);
    };

    if (session->callbacks.onStarted)
        session->callbacks.onStarted(session->callbacks.user);

    // Send phase: read the file in 4096-byte chunks and write each as an AudioChunk.
    {
        std::ifstream file(audioFilePath, std::ios::binary | std::ios::ate);
        if (!file)
        {
            session->log("Error opening audio file for reading.");
        }
        else
        {
            const int64_t fileSize = static_cast<int64_t>(file.tellg());
            file.seekg(0, std::ios::beg);

            const std::streamsize bufferSize = 4096;
            std::string buffer(static_cast<size_t>(bufferSize), '\0');
            int64_t total = 0;
            int sentProgress = 0;
            while (file.read(&buffer[0], bufferSize) || file.gcount() > 0)
            {
                const std::streamsize bytesRead = file.gcount();
                Whisper::AudioChunk chunk;
                chunk.set_data(buffer.data(), static_cast<size_t>(bytesRead));
                if (!stream->Write(chunk))
                    break;
                total += bytesRead;
                session->log("Sent " + std::to_string(bytesRead) + " bytes (total " + std::to_string(total) + ")");

                if (fileSize > 0)
                {
                    auto fValue = 100.0 * static_cast<double>(total) / static_cast<double>(fileSize);
                    const int value = static_cast<int>(std::min(fValue, 100.0));

                    if (value > sentProgress)
                    {
                        if (session->callbacks.onTransmitProgress)
                            session->callbacks.onTransmitProgress(session->callbacks.user, value);
                        sentProgress = value;
                    }
                }
            }

            if(sentProgress < 100 && session->callbacks.onTransmitProgress)
                session->callbacks.onTransmitProgress(session->callbacks.user, 100);

            session->log("Finished sending audio file.");
        }

        stream->WritesDone();
    }

    session->log("Receiving transcription...");

    // Receive phase: read all StreamingWordsResponse messages until the server closes the stream.
    Whisper::StreamingWordsResponse response;
    while (stream->Read(&response))
    {
        const std::string& word = response.word();
        const std::string& fSecondsStartTime = response.start_time();
        const std::string& fSecondsEndTime = response.end_time();

        session->log("Word: " + word + ", Start: " + fSecondsStartTime + ", End: " + fSecondsEndTime);

        if (word.find('[') != std::string::npos && word.find(']') != std::string::npos)
        {
            if (word.find("PROGRESS") != std::string::npos)
            {
                const int serverPercent = static_cast<int>(std::strtol(fSecondsStartTime.c_str(), nullptr, 10));
                reportReceiveProgress(receiveProgressCalculator.onProgress(serverPercent, ProgressCalculation::extractProgressText(word)));
            }
            else if (word.find("_BEG") != std::string::npos)
            {
                if (session->callbacks.onTranscriptionStarted)
                    session->callbacks.onTranscriptionStarted(session->callbacks.user);
            }
            else
            {
                // if (word == "[_END_]") possible to end up here without end?
                    reportReceiveProgress(receiveProgressCalculator.complete());
                if (session->callbacks.onTranscriptionFinished)
                    session->callbacks.onTranscriptionFinished(session->callbacks.user);
            }
        }
        else if (session->callbacks.onWord)
        {
            session->callbacks.onWord(session->callbacks.user, word.c_str(), fSecondsStartTime.c_str(), fSecondsEndTime.c_str());
        }
    }

    grpc::Status status = stream->Finish();
    if (!status.ok())
        session->log("gRPC error: " + status.error_message());

    {
        std::lock_guard<std::mutex> lock(session->contextMutex);
        session->context = nullptr;
    }

    session->log("Disconnected");
    if (session->callbacks.onFinished)
        session->callbacks.onFinished(session->callbacks.user);
}

GAB_API void Gab_Cancel(GabSession sessionHandle)
{
    auto* session = static_cast<GabSessionImpl*>(sessionHandle);
    if (session == nullptr)
        return;

    std::lock_guard<std::mutex> lock(session->contextMutex);
    if (session->context != nullptr)
        session->context->TryCancel();
}

GAB_API void Gab_Destroy(GabSession sessionHandle)
{
    delete static_cast<GabSessionImpl*>(sessionHandle);
}
