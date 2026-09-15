// ============================================================================
//  Guide.h -- the native CLICK and CUES tracks.
//
//  A worship band on stems expects two guide tracks beside the music: a click
//  in the drummer's ears, and a voice that says "Chorus" a couple of bars
//  before the chorus and counts "1, 2, 3, 4" into it. Stem packs usually ship
//  both as extra audio tracks; a song that arrives without them (or whose
//  cues say the wrong thing) gets these instead, generated from the song's
//  own arrangement and tempo, and sent to their own mixer strips so they can
//  go to the drummer and the MD and never to the house.
//
//  Pure C++, no JUCE, so guidetest.cpp can pin the scheduling. The spoken cue
//  audio itself is loaded by the app (GuideBank in Main.cpp) and handed here
//  as plain float buffers.
//
//  Clock: everything is driven by the CALLER-supplied position, in the units
//  the caller chooses (deck samples while a song plays, master samples during
//  a count-in) -- neither class owns a clock, so a section jump moves the
//  click and the cues with the music instead of drifting from it.
// ============================================================================
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace ezguide
{

//==============================================================================
//  The click: a short sine burst on every beat, brighter and louder on beat
//  one, so the drummer hears the bar and not just the pulse.
//==============================================================================
class Click
{
public:
    void prepare (double sampleRate)
    {
        deviceSampleRate = sampleRate;
        clickSamples     = std::max (1, (int) std::llround (kClickSeconds * sampleRate));
        decayPerSample   = std::pow (kClickFloor, 1.0 / (double) clickSamples);
        phase = -1;
        lastBeat = INT64_MIN;
    }

    /** Forget which beat was last struck, so the next render re-strikes
        whatever beat `pos` lands in (call on transport start and on jumps). */
    void reset() { lastBeat = INT64_MIN; phase = -1; }

    /** Renders `n` samples of click, OVERWRITING outL/outR.
        posAtStart is the position of the first sample, in the same units as
        samplesPerBeat (deck samples or device samples -- the caller's choice,
        as long as both agree). beatsPerBar picks the accented beat. */
    void render (float* outL, float* outR, int n, double posAtStart, double samplesPerBeat, int beatsPerBar, double gain = 1.0)
    {
        for (int i = 0; i < n; ++i) { outL[i] = 0.0f; outR[i] = 0.0f; }
        if (samplesPerBeat <= 0.0 || beatsPerBar <= 0) return;

        for (int i = 0; i < n; ++i)
        {
            const double  pos  = posAtStart + (double) i;
            const int64_t beat = (int64_t) std::floor (pos / samplesPerBeat);
            if (beat != lastBeat)
            {
                lastBeat = beat;
                const int64_t inBar = ((beat % beatsPerBar) + beatsPerBar) % beatsPerBar;
                accent   = inBar == 0;
                phase    = 0;
                envelope = accent ? 1.0f : 0.62f;
            }
            if (phase >= 0)
            {
                const float hz = accent ? kAccentHz : kBeatHz;
                const float s  = std::sin (hz * 2.0f * kPi * (float) phase / (float) deviceSampleRate) * envelope * (float) gain;
                outL[i] = s; outR[i] = s;
                envelope = (float) ((double) envelope * decayPerSample);
                if (++phase >= clickSamples) phase = -1;
            }
        }
    }

private:
    static constexpr float  kPi           = 3.14159265358979323846f;
    static constexpr float  kAccentHz     = 1760.0f;
    static constexpr float  kBeatHz       = 1175.0f;
    static constexpr double kClickFloor   = 0.001;
    static constexpr double kClickSeconds = 0.028;

    double  deviceSampleRate { 44100.0 };
    int     clickSamples { 1 };
    double  decayPerSample { 1.0 };
    int     phase { -1 };
    bool    accent { false };
    float   envelope { 1.0f };
    int64_t lastBeat { INT64_MIN };
};

//==============================================================================
//  Cue vocabulary. Ids index the bank the app loaded; kNone means "no such
//  recording", which the scheduler simply leaves out.
//==============================================================================
constexpr int kNoCue = -1;

struct CueEvent
{
    double pos { 0.0 };   // in the caller's clock units (deck samples)
    int    cue { kNoCue };
};

/** How a section's cue is chosen. Stored on the section: "" = automatic
    (from the section name), "-" = no cue, anything else = a cue file stem
    such as "Chorus-2" or "Last-Time". */
inline bool cueIsOff (const std::string& cue) { return cue == "-"; }

/** Turns a section name into the cue file stem the Motion Worship library
    uses: "Verse 1" -> "Verse-1", "Pre-Chorus" -> "Prechorus", "Chorus (big)"
    -> "Chorus". `exists` says which stems the bank actually has, so "Verse 7"
    falls back to "Verse" rather than silence. */
inline std::string cueStemForSectionName (const std::string& name, const std::function<bool (const std::string&)>& exists)
{
    std::string base, digits;
    for (char ch : name)
    {
        if (ch >= 'a' && ch <= 'z') base += ch;
        else if (ch >= 'A' && ch <= 'Z') base += (char) (ch - 'A' + 'a');
        else if (ch >= '0' && ch <= '9') digits += ch;
    }
    if (base.empty()) return {};

    struct Alias { const char* word; const char* stem; };
    static const Alias aliases[] = {
        { "intro", "Intro" }, { "verse", "Verse" }, { "prechorus", "Prechorus" }, { "chorus", "Chorus" },
        { "postchorus", "Post-Chorus" }, { "bridge", "Bridge" }, { "instrumental", "Instrumental" },
        { "interlude", "Interlude" }, { "breakdown", "Breakdown" }, { "break", "Break" }, { "build", "Build" },
        { "tag", "Tag" }, { "vamp", "Vamp" }, { "outro", "Outro" }, { "ending", "Ending" }, { "end", "End" },
        { "refrain", "Refrain" }, { "solo", "Solo" }, { "turnaround", "Turnaround" }, { "hits", "Hits" },
        { "hold", "Hold" }, { "rap", "Rap" }, { "exhortation", "Exhortation" }, { "softly", "Softly" },
        { "worshipfreely", "Worship-Freely" }, { "freeworship", "Worship-Freely" }, { "spontaneous", "Worship-Freely" },
        { "keychange", "Key-Change" }, { "lasttime", "Last-Time" }, { "again", "Again" }, { "repeat", "Repeat" },
        { "allin", "All-In" }, { "bigending", "Big-Ending" }, { "longtag", "Long-Tag" }, { "shorttag", "Short-Tag" },
        { "wait", "Wait" },
    };

    std::string stem;
    // longest alias that the name contains wins ("postchorus" before "chorus")
    size_t bestLen = 0;
    for (const auto& a : aliases)
    {
        const std::string w (a.word);
        if (base.find (w) != std::string::npos && w.size() > bestLen) { bestLen = w.size(); stem = a.stem; }
    }
    if (stem.empty()) return {};

    if (! digits.empty())
    {
        const std::string numbered = stem + "-" + digits.substr (0, 1);
        if (exists (numbered)) return numbered;
    }
    return exists (stem) ? stem : std::string();
}

//==============================================================================
//  Scheduling: from an arrangement to a sorted list of cue events.
//==============================================================================
struct GuideSection
{
    std::string name;
    std::string cue;        // see cueIsOff / "" = automatic
    int         startBar { 0 };
};

struct ScheduleSettings
{
    double samplesPerBar { 0.0 };   // deck-sample units
    int    beatsPerBar { 4 };
    int    leadBars { 2 };          // the name is spoken this many bars before the section
    bool   counts { true };         // "1, 2, 3, 4" on the beats of the bar before
    bool   fast { true };           // which count recordings to use
};

/** cueIdFor(stem) -> bank id or kNoCue. countIdFor(beat 1..8, fast) -> id. */
inline std::vector<CueEvent> buildSchedule (const std::vector<GuideSection>& sections,
                                            const ScheduleSettings& s,
                                            const std::function<int (const std::string&)>& cueIdFor,
                                            const std::function<int (int, bool)>& countIdFor)
{
    std::vector<CueEvent> out;
    if (s.samplesPerBar <= 0.0 || s.beatsPerBar <= 0) return out;
    const double spb = s.samplesPerBar / (double) s.beatsPerBar;

    auto exists = [&cueIdFor] (const std::string& stem) { return cueIdFor (stem) != kNoCue; };

    for (size_t i = 0; i < sections.size(); ++i)
    {
        const auto& sec = sections[i];
        if (cueIsOff (sec.cue)) continue;
        if (sec.startBar <= 0) continue;   // the first bar is covered by the count-in

        // ---- the name ----
        std::string stem = sec.cue.empty() ? cueStemForSectionName (sec.name, exists) : sec.cue;
        const int nameBar = std::max (0, sec.startBar - std::max (1, s.leadBars));
        if (! stem.empty())
        {
            const int id = cueIdFor (stem);
            if (id != kNoCue) out.push_back ({ (double) nameBar * s.samplesPerBar, id });
        }

        // ---- "Last time" on the final repeat of a section name ----
        if (sec.cue.empty() && ! stem.empty())
        {
            bool seenBefore = false, seenAfter = false;
            for (size_t j = 0; j < sections.size(); ++j)
            {
                if (j == i) continue;
                const bool same = cueStemForSectionName (sections[j].name, exists) == stem;
                if (same && j < i) seenBefore = true;
                if (same && j > i) seenAfter = true;
            }
            if (seenBefore && ! seenAfter)
            {
                const int id = cueIdFor ("Last-Time");
                if (id != kNoCue) out.push_back ({ ((double) nameBar + 0.75) * s.samplesPerBar, id });
            }
        }

        // ---- the count into it ----
        if (s.counts && sec.startBar >= 1)
        {
            const double barStart = (double) (sec.startBar - 1) * s.samplesPerBar;
            for (int b = 0; b < s.beatsPerBar && b < 8; ++b)
            {
                const int id = countIdFor (b + 1, s.fast);
                if (id != kNoCue) out.push_back ({ barStart + (double) b * spb, id });
            }
        }
    }

    std::sort (out.begin(), out.end(), [] (const CueEvent& a, const CueEvent& b) { return a.pos < b.pos; });
    return out;
}

//==============================================================================
//  Playback: fires events as the position sweeps past them and mixes the
//  recordings. Audio-thread only once `setBank`/`setSchedule` have been
//  called from a stopped state (the app swaps schedules with a lock-free
//  pointer exchange, see Main.cpp).
//==============================================================================
struct CueSample
{
    const float* data { nullptr };
    int          length { 0 };
};

class CuePlayer
{
public:
    static constexpr int kVoices = 4;

    void setBank (const std::vector<CueSample>* b) { bank = b; }
    void setSchedule (const std::vector<CueEvent>* s) { schedule = s; next = 0; }

    /** Re-finds the first event at or after `pos` (after a jump or a start). */
    void locate (double pos)
    {
        next = 0;
        if (schedule == nullptr) return;
        while (next < schedule->size() && (*schedule)[next].pos < pos) ++next;
        lastPos = pos;
    }

    /** Starts a recording now (a manual cue, or a count-in count). */
    void trigger (int cue)
    {
        if (bank == nullptr || cue < 0 || cue >= (int) bank->size()) return;
        const auto& s = (*bank)[(size_t) cue];
        if (s.data == nullptr || s.length <= 0) return;
        int slot = 0;
        for (int v = 0; v < kVoices; ++v) if (voices[v].sample == nullptr) { slot = v; break; }
        voices[slot].sample = &s;
        voices[slot].pos = 0;
    }

    void stopAll()
    {
        for (auto& v : voices) v.sample = nullptr;
    }

    /** Fires every event in [posBefore, posAfter) -- the playhead before and
        after one block. If posBefore is not where the last block ended, the
        playhead jumped between blocks: the schedule is re-located there and
        nothing fires for the skipped span. A wrap inside the block (a
        section loop) fires the tail, then re-locates at the loop start. */
    void advance (double posBefore, double posAfter)
    {
        if (schedule == nullptr) return;
        if (std::fabs (posBefore - lastPos) > 1.0) locate (posBefore);
        if (posAfter < posBefore)
        {
            fireUpTo (posBefore, 1.0e18);
            locate (0.0);
            fireUpTo (0.0, posAfter);
        }
        else
        {
            fireUpTo (posBefore, posAfter);
        }
        lastPos = posAfter;
    }

    /** ADDS the playing recordings into outL/outR. */
    void render (float* outL, float* outR, int n, float gain = 1.0f)
    {
        for (auto& v : voices)
        {
            if (v.sample == nullptr) continue;
            const int avail = std::min (n, v.sample->length - v.pos);
            for (int i = 0; i < avail; ++i)
            {
                const float s = v.sample->data[v.pos + i] * gain;
                outL[i] += s; outR[i] += s;
            }
            v.pos += avail;
            if (v.pos >= v.sample->length) v.sample = nullptr;
        }
    }

    bool isSpeaking() const
    {
        for (const auto& v : voices) if (v.sample != nullptr) return true;
        return false;
    }

private:
    void fireUpTo (double from, double to)
    {
        while (next < schedule->size() && (*schedule)[next].pos < to)
        {
            if ((*schedule)[next].pos >= from) trigger ((*schedule)[next].cue);
            ++next;
        }
    }

    struct Voice { const CueSample* sample { nullptr }; int pos { 0 }; };
    const std::vector<CueSample>* bank { nullptr };
    const std::vector<CueEvent>*  schedule { nullptr };
    size_t next { 0 };
    double lastPos { 0.0 };
    Voice  voices[kVoices];
};

} // namespace ezguide
