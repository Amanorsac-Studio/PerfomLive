// ============================================================================
//  SpeechCues.h -- hearing the words on a guide track.
//
//  Owner: "when it says bridge I see rap, or chorus I see chorus 3 -- work on
//  the speech to text so you don't write something wrong."
//
//  Matching a word's loudness shape against the Motion Worship recordings only
//  works when the guide was made with those recordings; a MultiTracks guide
//  is a different voice, and "Bridge" came back as "Rap". This asks the
//  operating system's own speech recognizer instead, restricted to the words
//  a guide actually says (CueDetect.h's cuePhrases()), so it can only ever
//  answer with a real section name -- or with nothing, which the review
//  window then shows as "not sure".
//
//  Windows: SAPI's in-process recognizer (installed with Windows, English).
//  Elsewhere available() is false and the caller falls back to matching.
//  Plain C++ interface on purpose: the implementation needs <windows.h> and
//  <sapi.h>, which must not meet JuceHeader.h.
// ============================================================================
#pragma once

#include <string>
#include <vector>

namespace ezspeech
{

struct Heard
{
    std::string text;            // the phrase recognised, exactly as given; empty = nothing recognised
    float confidence { 0.0f };   // the engine's own, 0..1
    bool rejected { false };     // the engine's best guess, which it itself did not trust
};

/** True when this computer can recognise speech. */
bool available();

/** Listens to each clip (mono, 16 kHz, -1..1) for one of `phrases` (lower-
    case, words separated by single spaces). One Heard per clip, in order.
    Blocking -- call it off the message thread. Sets `error` and returns an
    empty vector when the recognizer can't start. An empty `phrases` list
    means free dictation (diagnostics only). */
std::vector<Heard> recognise (const std::vector<std::vector<float>>& clips,
                              const std::vector<std::string>& phrases,
                              std::string& error,
                              std::vector<std::string>* trace = nullptr);

} // namespace ezspeech
