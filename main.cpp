#include <cstdlib>
#include <cstdint>
#include <iostream>
#include <string>
#include <vector>

#include <curl/curl.h>
#include <portaudio.h>

constexpr double SAMPLE_RATE = 24000.0;
constexpr int CHANNELS = 1;

struct AudioSink {
    PaStream* stream = nullptr;
    bool started = false;

    // A curl chunk is not guaranteed to end on a 16-bit sample boundary.
    bool haveCarry = false;
    uint8_t carry = 0;

    PaError error = paNoError;
};

static bool writeSamples(AudioSink* sink,
                         const int16_t* samples,
                         size_t sampleCount)
{
    if (sampleCount == 0)
        return true;

    // Don't start PortAudio until the first actual PCM bytes arrive.
    if (!sink->started) {
        sink->error = Pa_StartStream(sink->stream);

        if (sink->error != paNoError) {
            std::cerr
                << "Pa_StartStream failed: "
                << Pa_GetErrorText(sink->error)
                << '\n';

            return false;
        }

        sink->started = true;
    }

    // Mono:
    //     one sample == one PortAudio frame.
    sink->error = Pa_WriteStream(
        sink->stream,
        samples,
        static_cast<unsigned long>(sampleCount)
    );

    // An underflow isn't fatal. It means the network/TTS server
    // temporarily wasn't producing audio fast enough.
    if (sink->error != paNoError &&
        sink->error != paOutputUnderflowed)
    {
        std::cerr
            << "Pa_WriteStream failed: "
            << Pa_GetErrorText(sink->error)
            << '\n';

        return false;
    }

    return true;
}


// Called by libcurl every time another chunk of the HTTP response arrives.
static size_t curlAudioCallback(
    char* ptr,
    size_t size,
    size_t nmemb,
    void* userdata)
{
    auto* sink = static_cast<AudioSink*>(userdata);

    const size_t byteCount = size * nmemb;

    if (byteCount == 0)
        return 0;

    const auto* bytes =
        reinterpret_cast<const uint8_t*>(ptr);

    std::vector<int16_t> samples;
    samples.reserve(
        (byteCount + (sink->haveCarry ? 1 : 0)) / 2
    );

    size_t i = 0;

    // Complete a sample left over from the previous curl chunk.
    if (sink->haveCarry) {
        uint16_t value =
            static_cast<uint16_t>(sink->carry) |
            (static_cast<uint16_t>(bytes[0]) << 8);

        samples.push_back(static_cast<int16_t>(value));

        sink->haveCarry = false;
        i = 1;
    }

    // PCM is signed 16-bit little endian:
    //
    // byte 0 = low byte
    // byte 1 = high byte
    while (i + 1 < byteCount) {
        uint16_t value =
            static_cast<uint16_t>(bytes[i]) |
            (static_cast<uint16_t>(bytes[i + 1]) << 8);

        samples.push_back(static_cast<int16_t>(value));

        i += 2;
    }

    // Keep an odd trailing byte for the next curl invocation.
    if (i < byteCount) {
        sink->carry = bytes[i];
        sink->haveCarry = true;
    }

    if (!writeSamples(
            sink,
            samples.data(),
            samples.size()))
    {
        // Returning a value other than byteCount tells libcurl
        // that writing failed and aborts the HTTP request.
        return 0;
    }

    return byteCount;
}


// Enough escaping for putting arbitrary prompt text in a JSON string.
static std::string jsonEscape(const std::string& input)
{
    std::string result;

    for (unsigned char c : input) {
        switch (c) {
            case '"':
                result += "\\\"";
                break;

            case '\\':
                result += "\\\\";
                break;

            case '\n':
                result += "\\n";
                break;

            case '\r':
                result += "\\r";
                break;

            case '\t':
                result += "\\t";
                break;

            default:
                result += static_cast<char>(c);
                break;
        }
    }

    return result;
}


int main(int argc, char** argv)
{
    //----------------------------------------------------------------------
    // Prompt
    //----------------------------------------------------------------------

    std::string prompt =
        argc > 1
            ? argv[1]
            : "You must pass text as a parameter to the program.";


    //----------------------------------------------------------------------
    // API key
    //----------------------------------------------------------------------

    const char* apiKey = std::getenv("NARI_API_KEY");

    if (!apiKey) {
        std::cerr
            << "Set NARI_API_KEY first.\n"
            << "Example:\n"
            << "  export NARI_API_KEY='sk-...'\n";

        return 1;
    }


    //----------------------------------------------------------------------
    // PortAudio
    //----------------------------------------------------------------------

    PaError paError = Pa_Initialize();

    if (paError != paNoError) {
        std::cerr
            << "Pa_Initialize failed: "
            << Pa_GetErrorText(paError)
            << '\n';

        return 1;
    }

    AudioSink sink;

    paError = Pa_OpenDefaultStream(
        &sink.stream,
        0,                         // input channels
        CHANNELS,                  // output channels
        paInt16,                   // raw PCM = signed int16
        SAMPLE_RATE,
        paFramesPerBufferUnspecified,
        nullptr,                   // no PortAudio callback
        nullptr
    );

    if (paError != paNoError) {
        std::cerr
            << "Pa_OpenDefaultStream failed: "
            << Pa_GetErrorText(paError)
            << '\n';

        Pa_Terminate();
        return 1;
    }


    //----------------------------------------------------------------------
    // libcurl
    //----------------------------------------------------------------------

    CURLcode globalResult =
        curl_global_init(CURL_GLOBAL_DEFAULT);

    if (globalResult != CURLE_OK) {
        std::cerr
            << "curl_global_init failed\n";

        Pa_CloseStream(sink.stream);
        Pa_Terminate();

        return 1;
    }

    CURL* curl = curl_easy_init();

    if (!curl) {
        std::cerr << "curl_easy_init failed\n";

        curl_global_cleanup();
        Pa_CloseStream(sink.stream);
        Pa_Terminate();

        return 1;
    }


    //----------------------------------------------------------------------
    // HTTP headers
    //----------------------------------------------------------------------

    struct curl_slist* headers = nullptr;

    std::string authHeader =
        "Authorization: Bearer " +
        std::string(apiKey);

    headers =
        curl_slist_append(
            headers,
            authHeader.c_str()
        );

    headers =
        curl_slist_append(
            headers,
            "Content-Type: application/json"
        );

    headers =
        curl_slist_append(
            headers,
            "Accept: audio/pcm"
        );


    //----------------------------------------------------------------------
    // Request body
    //----------------------------------------------------------------------

    std::string json =
        "{"
        "\"model\":\"qwen3-tts-fast:free\","
        "\"input\":\"" + jsonEscape(prompt) + "\","
        "\"voice\":\"graham\","
        "\"stream\":true,"
        "\"response_format\":\"pcm\""
        "}";


    //----------------------------------------------------------------------
    // Configure curl
    //----------------------------------------------------------------------

    curl_easy_setopt(
        curl,
        CURLOPT_URL,
        "https://api.narilabs.com/v1/audio/speech"
    );

    curl_easy_setopt(
        curl,
        CURLOPT_HTTPHEADER,
        headers
    );

    curl_easy_setopt(
        curl,
        CURLOPT_POSTFIELDS,
        json.c_str()
    );

    curl_easy_setopt(
        curl,
        CURLOPT_POSTFIELDSIZE,
        static_cast<long>(json.size())
    );

    // Don't feed an HTTP error response (JSON, HTML, etc.)
    // into the speakers as though it were PCM.
    curl_easy_setopt(
        curl,
        CURLOPT_FAILONERROR,
        1L
    );

    curl_easy_setopt(
        curl,
        CURLOPT_WRITEFUNCTION,
        curlAudioCallback
    );

    curl_easy_setopt(
        curl,
        CURLOPT_WRITEDATA,
        &sink
    );

    curl_easy_setopt(
        curl,
        CURLOPT_TCP_KEEPALIVE,
        1L
    );

    curl_easy_setopt(
        curl,
        CURLOPT_NOSIGNAL,
        1L
    );


    //----------------------------------------------------------------------
    // Start TTS.
    //
    // curlAudioCallback() starts receiving data before curl_easy_perform()
    // returns, so playback starts while the response is being generated.
    //----------------------------------------------------------------------

    std::cout << "Speaking...\n";

    CURLcode result =
        curl_easy_perform(curl);

    long httpStatus = 0;

    curl_easy_getinfo(
        curl,
        CURLINFO_RESPONSE_CODE,
        &httpStatus
    );


    //----------------------------------------------------------------------
    // Cleanup curl
    //----------------------------------------------------------------------

    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    curl_global_cleanup();


    //----------------------------------------------------------------------
    // Let PortAudio finish anything remaining in its output buffer.
    //----------------------------------------------------------------------

    if (sink.started) {
        PaError stopResult =
            Pa_StopStream(sink.stream);

        if (stopResult != paNoError) {
            std::cerr
                << "Pa_StopStream failed: "
                << Pa_GetErrorText(stopResult)
                << '\n';
        }
    }

    Pa_CloseStream(sink.stream);
    Pa_Terminate();


    //----------------------------------------------------------------------
    // Errors
    //----------------------------------------------------------------------

    if (result != CURLE_OK) {
        std::cerr
            << "HTTP/TTS request failed: "
            << curl_easy_strerror(result)
            << " (HTTP "
            << httpStatus
            << ")\n";

        return 1;
    }

    if (sink.haveCarry) {
        std::cerr
            << "Warning: response ended with an incomplete PCM sample\n";
    }

    std::cout << "Done.\n";

    return 0;
}
