#pragma once

#include <stdint.h>

#if defined(_WIN32)
  #if defined(GRPCAUDIOBRIDGE_EXPORTS)
    #define GAB_API __declspec(dllexport)
  #else
    #define GAB_API __declspec(dllimport)
  #endif
#else
  #define GAB_API
#endif

#ifdef __cplusplus
extern "C" {
#endif

typedef void* GabSession;

// All callbacks fire on the bridge's internal worker thread, not the caller's thread.
// Marshal to your own UI/main thread as needed before touching UI state.
typedef void (*GabLogFn)(void* user, const char* message);
typedef void (*GabWordFn)(void* user, const char* word, const char* fSecondsStart, const char* fSecondsEnd);
typedef void (*GabReceiveProgressFn)(void* user, int64_t progress);
typedef void (*GabTransmitProgressFn)(void* user, int64_t progress);
typedef void (*GabSimpleFn)(void* user);

typedef struct GabCallbacks
{
    void* user;
    GabLogFn onLog;             // optional, may be null
    GabWordFn onWord;           // required for meaningful output
    GabReceiveProgressFn onReceiveProgress;
    GabTransmitProgressFn onTransmitProgress;
    GabSimpleFn onStarted;              // optional: server signaled transcription start ("[..._BEG...]" marker)
    GabSimpleFn onTranscriptionFinished; // optional: server signaled transcription end mid-stream (e.g. "[..._END...]" marker)
    GabSimpleFn onFinished;             // required: Gab_StreamFile call itself has returned/the RPC closed
} GabCallbacks;

// serverAddress: "host:port", optionally prefixed with "http://" or "https://".
// https:// selects TLS (SslCredentials); anything else uses an insecure channel.
GAB_API GabSession Gab_Create(const char* serverAddress, const GabCallbacks* callbacks);

// Runs synchronously on the calling thread: streams audioFilePath to the server in
// 4096-byte chunks and reads back word responses until the server closes the stream
// or Gab_Cancel is called. Call this from your own worker thread, not the UI thread.
GAB_API void Gab_StreamFile(GabSession session, const char* audioFilePath);

// Thread-safe. Requests cancellation of an in-progress Gab_StreamFile call.
GAB_API void Gab_Cancel(GabSession session);

GAB_API void Gab_Destroy(GabSession session);

#ifdef __cplusplus
}
#endif
