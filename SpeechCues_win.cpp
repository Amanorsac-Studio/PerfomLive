// SpeechCues_win.cpp -- SpeechCues.h on Windows (SAPI 5, in-process recognizer).
// Raw COM on purpose: no ATL, no sphelper.h, and never JuceHeader.h in this file.
#include "SpeechCues.h"

#ifdef _WIN32

#ifndef WIN32_LEAN_AND_MEAN
 #define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
 #define NOMINMAX
#endif
#include <windows.h>
#include <mmreg.h>
#include <shlwapi.h>
#include <sapi.h>

#include <algorithm>
#include <climits>
#include <cmath>
#include <cstdint>
#include <map>
#include <set>

#pragma comment (lib, "sapi.lib")
#pragma comment (lib, "ole32.lib")
#pragma comment (lib, "shlwapi.lib")

namespace
{
    struct ComScope
    {
        HRESULT hr;
        ComScope() : hr (CoInitializeEx (nullptr, COINIT_MULTITHREADED)) {}
        ~ComScope() { if (SUCCEEDED (hr)) CoUninitialize(); }
        bool ok() const { return SUCCEEDED (hr) || hr == RPC_E_CHANGED_MODE; }
    };

    template <typename T>
    struct Com
    {
        T* p { nullptr };
        Com() = default;
        Com (const Com&) = delete;
        Com& operator= (const Com&) = delete;
        ~Com() { reset(); }
        void reset() { if (p != nullptr) { p->Release(); p = nullptr; } }
        T** put() { reset(); return &p; }
        T* operator->() const { return p; }
        explicit operator bool() const { return p != nullptr; }
    };

    void clearEvent (SPEVENT& e)
    {
        if (e.lParam != 0)
        {
            if (e.elParamType == SPET_LPARAM_IS_OBJECT)
                ((IUnknown*) e.lParam)->Release();
            else if (e.elParamType == SPET_LPARAM_IS_POINTER || e.elParamType == SPET_LPARAM_IS_STRING)
                CoTaskMemFree ((void*) e.lParam);
        }
        e.lParam = 0;
    }

    std::wstring widen (const std::string& s) { return std::wstring (s.begin(), s.end()); }   // phrases are ASCII

    std::string narrow (const wchar_t* w)
    {
        std::string out;
        for (; w != nullptr && *w != 0; ++w) out.push_back (*w < 128 ? (char) *w : '?');
        return out;
    }

    std::string lower (std::string s)
    {
        std::transform (s.begin(), s.end(), s.begin(), [] (unsigned char c) { return (char) std::tolower (c); });
        return s;
    }

    /** The default English recognizer's token, so the in-process recognizer
        doesn't depend on whatever the control panel last selected. */
    bool setRecognizerToken (ISpRecognizer* recognizer)
    {
        Com<ISpObjectTokenCategory> category;
        if (FAILED (CoCreateInstance (CLSID_SpObjectTokenCategory, nullptr, CLSCTX_ALL, IID_ISpObjectTokenCategory, (void**) category.put())))
            return false;
        if (FAILED (category->SetId (SPCAT_RECOGNIZERS, FALSE))) return false;
        LPWSTR id = nullptr;
        if (FAILED (category->GetDefaultTokenId (&id)) || id == nullptr) return false;
        Com<ISpObjectToken> token;
        HRESULT hr = CoCreateInstance (CLSID_SpObjectToken, nullptr, CLSCTX_ALL, IID_ISpObjectToken, (void**) token.put());
        if (SUCCEEDED (hr)) hr = token->SetId (nullptr, id, FALSE);
        CoTaskMemFree (id);
        if (FAILED (hr)) return false;
        return SUCCEEDED (recognizer->SetRecognizer (token.p));
    }

    /** The same audio as a real WAV file bound with ISpStream::BindToFile --
        SAPI's own documented way to recognise from a file. */
    bool makeFileStream (const std::vector<int16_t>& pcm, const std::wstring& path, Com<ISpStream>& out)
    {
        {
            HANDLE h = CreateFileW (path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_TEMPORARY, nullptr);
            if (h == INVALID_HANDLE_VALUE) return false;
            const DWORD bytes = (DWORD) (pcm.size() * 2);
            auto put = [h] (const void* p, DWORD n) { DWORD w = 0; WriteFile (h, p, n, &w, nullptr); };
            auto u32 = [&put] (DWORD v) { put (&v, 4); };
            auto u16 = [&put] (WORD v)  { put (&v, 2); };
            put ("RIFF", 4); u32 (36 + bytes); put ("WAVEfmt ", 8); u32 (16); u16 (1); u16 (1); u32 (16000); u32 (32000); u16 (2); u16 (16);
            put ("data", 4); u32 (bytes); put (pcm.data(), bytes);
            CloseHandle (h);
        }
        HRESULT hr = CoCreateInstance (CLSID_SpStream, nullptr, CLSCTX_ALL, IID_ISpStream, (void**) out.put());
        WAVEFORMATEX wfx {};
        wfx.wFormatTag = WAVE_FORMAT_PCM; wfx.nChannels = 1; wfx.nSamplesPerSec = 16000;
        wfx.wBitsPerSample = 16; wfx.nBlockAlign = 2; wfx.nAvgBytesPerSec = 32000;
        if (SUCCEEDED (hr)) hr = out->BindToFile (path.c_str(), SPFM_OPEN_READONLY, &SPDFID_WaveFormatEx, &wfx, SPFEI_ALL_EVENTS);
        return SUCCEEDED (hr);
    }

    /** A 16 kHz 16-bit PCM stream over `pcm` (which must outlive the stream's use). */
    bool makeStream (const std::vector<int16_t>& pcm, Com<ISpStream>& out)
    {
        IStream* mem = SHCreateMemStream ((const BYTE*) pcm.data(), (UINT) (pcm.size() * sizeof (int16_t)));
        if (mem == nullptr) return false;
        HRESULT hr = CoCreateInstance (CLSID_SpStream, nullptr, CLSCTX_ALL, IID_ISpStream, (void**) out.put());
        WAVEFORMATEX wfx {};
        wfx.wFormatTag      = WAVE_FORMAT_PCM;
        wfx.nChannels       = 1;
        wfx.nSamplesPerSec  = 16000;
        wfx.wBitsPerSample  = 16;
        wfx.nBlockAlign     = 2;
        wfx.nAvgBytesPerSec = 32000;
        if (SUCCEEDED (hr)) hr = out->SetBaseStream (mem, SPDFID_WaveFormatEx, &wfx);
        mem->Release();
        return SUCCEEDED (hr);
    }

    /** Silence, the clip levelled to a steady peak, then silence: the engine
        needs a moment of quiet either side to find the start and end of speech. */
    std::vector<int16_t> preparePcm (const std::vector<float>& clip)
    {
        float peak = 0.0f;
        for (float v : clip) peak = std::max (peak, std::fabs (v));
        const float gain = peak > 1.0e-4f ? 0.7f / peak : 1.0f;
        std::vector<int16_t> pcm;
        pcm.reserve (clip.size() + 16000);
        pcm.insert (pcm.end(), 4800, 0);   // 0.3 s
        for (float v : clip)
            pcm.push_back ((int16_t) std::lround (std::max (-1.0f, std::min (1.0f, v * gain)) * 32000.0f));
        pcm.insert (pcm.end(), 9600, 0);   // 0.6 s
        return pcm;
    }
}

namespace ezspeech
{

bool available()
{
    ComScope com;
    if (! com.ok()) return false;
    Com<ISpRecognizer> recognizer;
    if (FAILED (CoCreateInstance (CLSID_SpInprocRecognizer, nullptr, CLSCTX_ALL, IID_ISpRecognizer, (void**) recognizer.put())))
        return false;
    return setRecognizerToken (recognizer.p);
}

std::vector<Heard> recognise (const std::vector<std::vector<float>>& clips,
                              const std::vector<std::string>& phrases,
                              std::string& error,
                              std::vector<std::string>* trace)
{
    auto log = [trace] (const std::string& s) { if (trace != nullptr) trace->push_back (s); };
    wchar_t tmpDir[MAX_PATH] {};
    GetTempPathW (MAX_PATH, tmpDir);
    const bool useFiles = GetEnvironmentVariableW (L"PERFORMLIVE_SPEECH_FILES", nullptr, 0) > 0;
    std::vector<Heard> out;
    ComScope com;
    if (! com.ok()) { error = "COM could not start"; return out; }

    Com<ISpRecognizer> recognizer;
    if (FAILED (CoCreateInstance (CLSID_SpInprocRecognizer, nullptr, CLSCTX_ALL, IID_ISpRecognizer, (void**) recognizer.put())))
    { error = "Windows speech recognition is not available"; return out; }
    if (! setRecognizerToken (recognizer.p)) { error = "no speech recognizer is installed"; return out; }

    // the input has to exist before the context: start with a silent stream
    std::vector<int16_t> silence (1600, 0);
    {
        Com<ISpStream> first;
        if (! makeStream (silence, first) || FAILED (recognizer->SetInput (first.p, TRUE)))
        { error = "could not open an audio stream for the recognizer"; return out; }
    }

    Com<ISpRecoContext> context;
    if (FAILED (recognizer->CreateRecoContext (context.put()))) { error = "could not create a recognition context"; return out; }
    const ULONGLONG interest = SPFEI (SPEI_RECOGNITION) | SPFEI (SPEI_FALSE_RECOGNITION) | SPFEI (SPEI_END_SR_STREAM) | SPFEI (SPEI_START_SR_STREAM);
    if (FAILED (context->SetNotifyWin32Event()) || FAILED (context->SetInterest (interest, interest)))
    { error = "could not listen for recognition events"; return out; }

    Com<ISpRecoGrammar> grammar;
    if (FAILED (context->CreateGrammar (1, grammar.put()))) { error = "could not create a grammar"; return out; }
    if (phrases.empty())
    {
        if (FAILED (grammar->LoadDictation (nullptr, SPLO_STATIC)) || FAILED (grammar->SetDictationState (SPRS_ACTIVE)))
        { error = "dictation is not available"; return out; }
    }
    else
    {
        SPSTATEHANDLE rule = nullptr;
        if (FAILED (grammar->GetRule (L"cue", 0, SPRAF_TopLevel | SPRAF_Active, TRUE, &rule))) { error = "could not create the grammar rule"; return out; }
        for (const auto& phrase : phrases)
        {
            const auto w = widen (phrase);
            grammar->AddWordTransition (rule, nullptr, w.c_str(), L" ", SPWT_LEXICAL, 1.0f, nullptr);
        }
        if (FAILED (grammar->Commit (0)) || FAILED (grammar->SetRuleState (nullptr, nullptr, SPRS_ACTIVE)))
        { error = "the recognizer rejected the word list"; return out; }
    }

    const HANDLE notify = context->GetNotifyEventHandle();
    out.resize (clips.size());

    // Every event carries the number of the stream it belongs to. Events for
    // a stream can still be arriving after the next one has been set, so each
    // result is filed under the clip whose stream it came from, and a clip is
    // done only when ITS OWN stream reports its end -- an earlier stream's end
    // event must never be taken for this one's (that was every clip reading
    // "finished" in 0 ms, heard nothing, and one clip's word landing on the next).
    std::map<ULONG, size_t> clipOfStream;
    std::set<ULONG> endedStreams;
    ULONG newestStream = 0;

    auto pump = [&] (size_t currentClip, ULONG& currentStream, int& nReco, int& nFalse)
    {
        SPEVENT ev {};
        ULONG fetched = 0;
        while (context->GetEvents (1, &ev, &fetched) == S_OK && fetched == 1)
        {
            if (ev.eEventId == SPEI_START_SR_STREAM && ev.ulStreamNum > newestStream && currentStream == 0)
            {
                newestStream = ev.ulStreamNum;
                currentStream = ev.ulStreamNum;
                clipOfStream[ev.ulStreamNum] = currentClip;
            }
            else if (ev.eEventId == SPEI_END_SR_STREAM)
            {
                endedStreams.insert (ev.ulStreamNum);
            }

            const bool accepted = ev.eEventId == SPEI_RECOGNITION;
            if ((accepted || ev.eEventId == SPEI_FALSE_RECOGNITION) && ev.elParamType == SPET_LPARAM_IS_OBJECT && ev.lParam != 0)
            {
                const auto owner = clipOfStream.find (ev.ulStreamNum);
                if (owner != clipOfStream.end() && owner->second < out.size())
                {
                    if (accepted) ++nReco; else ++nFalse;
                    auto* result = (ISpRecoResult*) ev.lParam;
                    LPWSTR text = nullptr;
                    SPPHRASE* phrase = nullptr;
                    if (SUCCEEDED (result->GetText ((ULONG) SP_GETWHOLEPHRASE, (ULONG) SP_GETWHOLEPHRASE, TRUE, &text, nullptr)) && text != nullptr
                        && SUCCEEDED (result->GetPhrase (&phrase)) && phrase != nullptr)
                    {
                        float conf = phrase->Rule.SREngineConfidence;
                        if (conf > 1.0f || conf < 0.0f)   // some engines report outside 0..1; fall back to the coarse rating
                            conf = phrase->Rule.Confidence == SP_HIGH_CONFIDENCE ? 0.9f : phrase->Rule.Confidence == SP_NORMAL_CONFIDENCE ? 0.6f : 0.3f;
                        auto& h = out[owner->second];
                        // an accepted result always beats a rejected one; otherwise the more confident
                        const bool better = h.text.empty() || (accepted && h.rejected) || (accepted == ! h.rejected && conf > h.confidence);
                        if (better && std::wcslen (text) > 0)
                        {
                            h.text = lower (narrow (text));
                            h.confidence = conf;
                            h.rejected = ! accepted;
                        }
                    }
                    if (phrase != nullptr) CoTaskMemFree (phrase);
                    if (text != nullptr) CoTaskMemFree (text);
                }
            }
            clearEvent (ev);
        }
    };

    // the silent start-up stream: let it run out, so its events are behind us
    {
        ULONG ignored = 0; int a = 0, b = 0;
        recognizer->SetRecoState (SPRST_ACTIVE);
        const DWORD until = GetTickCount() + 1500;
        while (GetTickCount() < until)
        {
            WaitForSingleObject (notify, 100);
            pump (SIZE_MAX, ignored, a, b);
            if (ignored != 0 && endedStreams.count (ignored) > 0) break;
        }
        clipOfStream.clear();   // nothing from it is kept
        recognizer->SetRecoState (SPRST_INACTIVE);
    }

    for (size_t c = 0; c < clips.size(); ++c)
    {
        const auto pcm = preparePcm (clips[c]);
        Com<ISpStream> stream;
        const std::wstring clipPath = std::wstring (tmpDir) + L"performlive_cue_" + std::to_wstring (c) + L".wav";
        if (! (useFiles ? makeFileStream (pcm, clipPath, stream) : makeStream (pcm, stream))) { log ("clip " + std::to_string (c) + ": no stream"); continue; }
        if (FAILED (recognizer->SetInput (stream.p, TRUE))) { log ("clip " + std::to_string (c) + ": SetInput failed"); continue; }
        recognizer->SetRecoState (SPRST_ACTIVE);

        int nReco = 0, nFalse = 0;
        ULONG thisStream = 0;
        const DWORD started = GetTickCount();
        // the engine runs faster than real time; allow generously for a slow PC
        const DWORD deadline = started + 8000 + (DWORD) (pcm.size() / 16);
        while (GetTickCount() < deadline)
        {
            WaitForSingleObject (notify, 100);
            pump (c, thisStream, nReco, nFalse);
            if (thisStream != 0 && endedStreams.count (thisStream) > 0) break;
        }
        recognizer->SetRecoState (SPRST_INACTIVE);
        log ("clip " + std::to_string (c) + ": " + std::to_string (GetTickCount() - started) + " ms, stream " + std::to_string (thisStream)
             + ", reco " + std::to_string (nReco) + ", false " + std::to_string (nFalse)
             + (thisStream != 0 && endedStreams.count (thisStream) > 0 ? ", ended" : ", TIMED OUT"));
        if (useFiles) DeleteFileW (clipPath.c_str());
    }
    return out;
}

} // namespace ezspeech

#else   // not Windows: no recognizer yet; the caller falls back to matching

namespace ezspeech
{
bool available() { return false; }
std::vector<Heard> recognise (const std::vector<std::vector<float>>&, const std::vector<std::string>&, std::string& error,
                              std::vector<std::string>*)
{
    error = "speech recognition is not available on this platform yet";
    return {};
}
}

#endif
