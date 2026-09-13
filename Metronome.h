// ============================================================================
//  Metronome.h — a synthesized click, driven by Session's own clock
//  (PRODUCT_REQUIREMENTS.md §10; ARCHITECTURE.md's resolved Architecture
//  Decision Pending #6).
//
//  Pure C++, no JUCE. A short, exponentially-decaying sine burst -- not a
//  sample-based Layer, avoiding a new file-loading dependency on top of the
//  already-documented gap (NEXT_STEPS.md #12) and shipping with no bundled
//  asset in the future VST target.
//
//  Driven entirely by the CALLER-supplied absolute masterPos (Session's own
//  monotonic sample counter, read via Session::masterPosition()) and bpm
//  (Session::getTempo().bpm) -- Metronome never owns an independent clock,
//  so it stays perfectly locked to the same clock deck-switching uses. Beat
//  length depends only on bpm (60/bpm seconds), never on beatsPerBar --
//  Milestone 6's per-deck beatsPerBar has no bearing on click timing at all.
//  Reuses Session.h's own nextBarBoundaryAtOrAfter() free function
//  (substituting beat length for bar length) rather than reinventing the
//  same block-size-independent boundary math a second time.
// ============================================================================
#pragma once
#include "Session.h"
#include <algorithm>
#include <cmath>
#include <cstdint>

namespace ezdeck
{

class Metronome
{
public:
    // Sizes the click envelope's timing constants from the real device
    // sample rate -- called once in prepare(), never inside render().
    void prepare (double sampleRate)
    {
        deviceSampleRate = sampleRate;
        clickSamples     = std::max (1, (int) std::llround (kClickSeconds * sampleRate));
        decayPerSample    = std::pow (kClickFloor, 1.0 / (double) clickSamples);
    }

    // Renders numSamples of synthesized click, overwriting outL/outR.
    // masterPosAtStart is the absolute sample position of the FIRST sample
    // in this call (Session::masterPosition() at the moment this callback
    // began) -- block-size independent: chunking this call differently never
    // shifts where a click lands, exactly like Session::render()'s own
    // boundary math.
    void render (float* outL, float* outR, int numSamples, int64_t masterPosAtStart, double bpm)
    {
        for (int i = 0; i < numSamples; ++i) { outL[i] = 0.0f; outR[i] = 0.0f; }
        if (bpm <= 0.0) return;

        const int64_t beatLen = std::max<int64_t> (1, (int64_t) std::llround ((60.0 / bpm) * deviceSampleRate));

        int     done = 0;
        int64_t pos  = masterPosAtStart;
        while (done < numSamples)
        {
            int chunk = numSamples - done;

            const int64_t boundary      = nextBarBoundaryAtOrAfter (pos, beatLen);
            const int64_t untilBoundary = boundary - pos;

            if (untilBoundary <= 0)
            {
                clickPhase = 0;
                envelope   = 1.0f;

                // `pos` is now exactly ON a boundary -- the NEXT one is
                // exactly beatLen away. Clamp chunk so the loop re-checks
                // there instead of skipping past every later beat inside a
                // single large render() call (the same class of bug M5-T3
                // fixed in Session::render() for stem endings -- without
                // this, one large enough call would trigger only the FIRST
                // beat in its span and silently miss every subsequent one).
                if (beatLen < (int64_t) chunk) chunk = (int) beatLen;
            }
            else if (untilBoundary < (int64_t) chunk)
            {
                chunk = (int) untilBoundary;
            }

            if (clickPhase >= 0)
            {
                const int clickChunk = std::min (chunk, clickSamples - clickPhase);
                for (int i = 0; i < clickChunk; ++i)
                {
                    const float sample = std::sin (kClickFreqHz * 2.0f * kPi
                                                     * (float) (clickPhase + i) / (float) deviceSampleRate)
                                        * envelope;
                    outL[done + i] += sample;
                    outR[done + i] += sample;
                    envelope = (float) ((double) envelope * decayPerSample);
                }
                clickPhase += clickChunk;
                if (clickPhase >= clickSamples) clickPhase = -1;
            }

            pos  += chunk;
            done += chunk;
        }
    }

private:
    static constexpr float  kPi           = 3.14159265358979323846f;
    static constexpr float  kClickFreqHz  = 1500.0f;   // an audible, percussive click tone
    static constexpr double kClickFloor   = 0.001;     // ~-60dB -- envelope decays to this by the click's own end
    static constexpr double kClickSeconds = 0.030;     // click envelope window

    double deviceSampleRate { 44100.0 };
    int    clickSamples     { 1 };      // resized by prepare()
    double decayPerSample   { 1.0 };    // resized by prepare()

    // Audio-thread-owned only. -1 = no click currently in progress.
    int   clickPhase { -1 };
    float envelope   { 1.0f };
};

} // namespace ezdeck
