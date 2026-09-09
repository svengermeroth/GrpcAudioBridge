#include "GrpcAudioBridge.h"

#include <grpcpp/grpcpp.h>
#include "whisper.grpc.pb.h"

#include <fstream>
#include <mutex>
#include <string>

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
    if (isHttps)
        target = target.substr(8);
    else if (isHttp)
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

    std::unique_ptr<grpc::ClientReaderWriter<Whisper::AudioChunk, Whisper::StreamingWordsResponse>> stream(
        client->StreamAudio(&context));

    session->log(std::string("Sending audio file: ") + (audioFilePath != nullptr ? audioFilePath : ""));

    // Send phase: read the file in 4096-byte chunks and write each as an AudioChunk.
    {
        std::ifstream file(audioFilePath, std::ios::binary);
        if (!file)
        {
            session->log("Error opening audio file for reading.");
        }
        else
        {
            const std::streamsize bufferSize = 4096;
            std::string buffer(static_cast<size_t>(bufferSize), '\0');
            int64_t total = 0;
            while (file.read(&buffer[0], bufferSize) || file.gcount() > 0)
            {
                const std::streamsize bytesRead = file.gcount();
                Whisper::AudioChunk chunk;
                chunk.set_data(buffer.data(), static_cast<size_t>(bytesRead));
                if (!stream->Write(chunk))
                    break;
                total += bytesRead;
                session->log("Sent " + std::to_string(bytesRead) + " bytes (total " + std::to_string(total) + ")");
                if (session->callbacks.onTransmit)
                    session->callbacks.onTransmit(session->callbacks.user, static_cast<int64_t>(bytesRead), total);
            }

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
        const std::string& startTime = response.start_time();
        const std::string& endTime = response.end_time();

        session->log("Word: " + word + ", Start: " + startTime + ", End: " + endTime);

        if (word.find('[') != std::string::npos && word.find(']') != std::string::npos)
        {
            if (word.find("PROGRESS") != std::string::npos)
            {
                if (session->callbacks.onProgress)
                    session->callbacks.onProgress(session->callbacks.user, std::strtoll(startTime.c_str(), nullptr, 10));
            }
            else if (word.find("_BEG") != std::string::npos)
            {
                if (session->callbacks.onStarted)
                    session->callbacks.onStarted(session->callbacks.user);
            }
            else if (session->callbacks.onTranscriptionFinished)
            {
                session->callbacks.onTranscriptionFinished(session->callbacks.user);
            }
        }
        else if (session->callbacks.onWord)
        {
            session->callbacks.onWord(session->callbacks.user, word.c_str(), startTime.c_str(), endTime.c_str());
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
