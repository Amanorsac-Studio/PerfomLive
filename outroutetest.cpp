// ============================================================================
//  outroutetest.cpp — standalone console test for Mixer.h's output-pair
//  routing (SPEC_OUTPUT_ROUTING.md).
//
//  Same idea as Deck.h's own decktest.cpp: pure C++, no JUCE, no audio
//  device. Proves routing by numbers, not by ear:
//    - a channel assigned to output pair N writes ONLY into pair N's
//      buffers -- silence (untouched, still-zeroed) everywhere else
//    - the solo-safe metronome exemption holds regardless of which pair the
//      soloed channel and the metronome are each routed to
//    - Master gain scales pair 0 (Main) only -- a channel routed to any
//      other pair is deliberately NOT affected by the Master fader (see
//      Mixer::mixDown's own header comment for why)
//    - an output-pair assignment beyond however many pairs are actually
//      available clamps safely to a valid pair, never an out-of-bounds
//      write
//    - with every channel at its default routing (pair 0) and a
//      single-pair device, mixDown() behaves identically to its
//      pre-routing single-output-pair behavior (the "step 2 should sound
//      identical to today" requirement) -- proven against a manually
//      computed expected sum, not just "didn't crash"
//
//  Build & run (no CMake needed — this file has zero JUCE dependency):
//    g++ -std=c++17 -O2 outroutetest.cpp -o outroutetest.exe
//    ./outroutetest.exe
// ============================================================================
#include "Mixer.h"
#include <cmath>
#include <cstdio>
#include <vector>

static int gPass = 0, gTotal = 0;

#define CHECK(cond, desc) do {                                    \
    ++gTotal;                                                     \
    if (cond) { ++gPass; std::printf ("  [PASS] %s\n", desc); }    \
    else      {          std::printf ("  [FAIL] %s\n", desc); }    \
} while (0)

static bool nearlyEqual (float a, float b, float eps = 1e-4f)
{
    return std::fabs (a - b) <= eps;
}

// Every channel gets a distinct constant-amplitude source so a mismixed
// channel is unambiguous, not just "some nonzero value."
static std::vector<float> constantSource (int n, float value)
{
    return std::vector<float> ((size_t) n, value);
}

int main()
{
    using namespace ezdeck;
    std::printf ("Mixer.h output-pair routing tests (SPEC_OUTPUT_ROUTING.md)\n");

    const int numSamples = 64;

    // ---- a channel routed to pair N writes ONLY pair N's buffers ----------
    {
        Mixer m;
        // Tab1 (index 0) -> pair 0 (default, untouched); Tab2 -> pair 1; Fx -> pair 2.
        m.setChannelOutputPair (MixerChannel::Tab2, 1);
        m.setChannelOutputPair (MixerChannel::Fx,   2);

        std::array<std::vector<float>, kNumMixerChannels> srcL, srcR;
        std::array<const float*, kNumMixerChannels> inL {}, inR {};
        for (int c = 0; c < kNumMixerChannels; ++c)
        {
            const float v = 0.1f * (float) (c + 1);   // distinct per channel
            srcL[(size_t) c] = constantSource (numSamples, v);
            srcR[(size_t) c] = constantSource (numSamples, v);
            inL[(size_t) c] = srcL[(size_t) c].data();
            inR[(size_t) c] = srcR[(size_t) c].data();
        }

        const int numPairs = 3;
        std::array<std::vector<float>, numPairs> outLBuf, outRBuf;
        std::vector<float*> outL (numPairs), outR (numPairs);
        for (int p = 0; p < numPairs; ++p)
        {
            outLBuf[(size_t) p].assign ((size_t) numSamples, -1.0f);   // poison value -- must be overwritten to 0 or a real sum
            outRBuf[(size_t) p].assign ((size_t) numSamples, -1.0f);
            outL[(size_t) p] = outLBuf[(size_t) p].data();
            outR[(size_t) p] = outRBuf[(size_t) p].data();
        }

        m.mixDown (inL, inR, outL, outR, numSamples);

        // Pair 0 takes every channel NOT explicitly routed elsewhere, scaled
        // by Master (1.0 default). Derived from the enum rather than written
        // out as literals: this assertion used to name seven channels by
        // hand and silently became wrong the moment the mixer grew Tab5-Tab8.
        // Summed in channel order so the float rounding matches mixDown().
        float expectedPair0 = 0.0f;
        for (int c = 0; c < kNumMixerChannels; ++c)
        {
            if (c == (int) MixerChannel::Tab2 || c == (int) MixerChannel::Fx) continue;
            expectedPair0 += 0.1f * (float) (c + 1);
        }
        const float expectedPair1 = 0.1f * (float) ((int) MixerChannel::Tab2 + 1);   // Tab2 only
        const float expectedPair2 = 0.1f * (float) ((int) MixerChannel::Fx   + 1);   // Fx only

        bool pair0Ok = true, pair1Ok = true, pair2Ok = true;
        for (int i = 0; i < numSamples; ++i)
        {
            if (! nearlyEqual (outLBuf[0][(size_t) i], expectedPair0) || ! nearlyEqual (outRBuf[0][(size_t) i], expectedPair0)) pair0Ok = false;
            if (! nearlyEqual (outLBuf[1][(size_t) i], expectedPair1) || ! nearlyEqual (outRBuf[1][(size_t) i], expectedPair1)) pair1Ok = false;
            if (! nearlyEqual (outLBuf[2][(size_t) i], expectedPair2) || ! nearlyEqual (outRBuf[2][(size_t) i], expectedPair2)) pair2Ok = false;
        }
        CHECK (pair0Ok, "pair 0 (Main) sums exactly the channels still routed to it, scaled by Master");
        CHECK (pair1Ok, "pair 1 receives ONLY the channel explicitly routed to it (Tab2), not silence-plus-leakage from others");
        CHECK (pair2Ok, "pair 2 receives ONLY the channel explicitly routed to it (Fx)");
    }

    // ---- Master gain scales pair 0 only, never a routed-away pair ---------
    {
        Mixer m;
        m.setChannelOutputPair (MixerChannel::Tab1, 1);   // routed away from Main
        m.setMasterGain (0.25f);   // if this leaked into pair 1, its value would change

        std::array<std::vector<float>, kNumMixerChannels> srcL, srcR;
        std::array<const float*, kNumMixerChannels> inL {}, inR {};
        for (int c = 0; c < kNumMixerChannels; ++c)
        {
            srcL[(size_t) c] = constantSource (numSamples, c == (int) MixerChannel::Tab1 ? 1.0f : 0.0f);
            srcR[(size_t) c] = srcL[(size_t) c];
            inL[(size_t) c] = srcL[(size_t) c].data();
            inR[(size_t) c] = srcR[(size_t) c].data();
        }

        std::vector<float> l0 ((size_t) numSamples), r0 ((size_t) numSamples), l1 ((size_t) numSamples), r1 ((size_t) numSamples);
        std::vector<float*> outL { l0.data(), l1.data() }, outR { r0.data(), r1.data() };
        m.mixDown (inL, inR, outL, outR, numSamples);

        CHECK (nearlyEqual (l1[0], 1.0f) && nearlyEqual (r1[0], 1.0f),
               "a channel routed away from Main is NOT scaled by Master gain (0.25 would shrink 1.0 to 0.25 if it leaked in)");
        CHECK (nearlyEqual (l0[0], 0.0f) && nearlyEqual (r0[0], 0.0f),
               "Main (pair 0) carries none of Tab1's signal once it's routed elsewhere -- no double-count");
    }

    // ---- solo-safe Metro exemption holds regardless of pair ---------------
    {
        Mixer m;
        m.setChannelOutputPair (MixerChannel::Metro, 1);   // click routed to its own pair (the spec's own headline use case)
        m.setChannelSolo (MixerChannel::Tab1, true);        // some other channel soloed

        std::array<std::vector<float>, kNumMixerChannels> srcL, srcR;
        std::array<const float*, kNumMixerChannels> inL {}, inR {};
        for (int c = 0; c < kNumMixerChannels; ++c)
        {
            // Cues is solo-exempt too (Guide.h) and stays on Main, so it is
            // silent here: this check is about Tab1 alone on pair 0.
            srcL[(size_t) c] = constantSource (numSamples, c == (int) MixerChannel::Cues ? 0.0f : 1.0f);
            srcR[(size_t) c] = srcL[(size_t) c];
            inL[(size_t) c] = srcL[(size_t) c].data();
            inR[(size_t) c] = srcR[(size_t) c].data();
        }

        std::vector<float> l0 ((size_t) numSamples), r0 ((size_t) numSamples), l1 ((size_t) numSamples), r1 ((size_t) numSamples);
        std::vector<float*> outL { l0.data(), l1.data() }, outR { r0.data(), r1.data() };
        m.mixDown (inL, inR, outL, outR, numSamples);

        CHECK (nearlyEqual (l1[0], 1.0f) && nearlyEqual (r1[0], 1.0f),
               "Metro's solo-exemption still holds when it's routed to a pair other than Main -- another channel's solo doesn't silence it");
        CHECK (nearlyEqual (l0[0], 1.0f) && nearlyEqual (r0[0], 1.0f),
               "the soloed channel (Tab1, still on pair 0/Main) plays normally alongside routing changes elsewhere");
    }

    // ---- an out-of-range pair assignment clamps safely, never crashes -----
    {
        Mixer m;
        m.setChannelOutputPair (MixerChannel::Tab1, 99);   // e.g. a saved "Output 4" restored onto a now-2-pair device

        std::array<std::vector<float>, kNumMixerChannels> srcL, srcR;
        std::array<const float*, kNumMixerChannels> inL {}, inR {};
        for (int c = 0; c < kNumMixerChannels; ++c)
        {
            srcL[(size_t) c] = constantSource (numSamples, c == (int) MixerChannel::Tab1 ? 1.0f : 0.0f);
            srcR[(size_t) c] = srcL[(size_t) c];
            inL[(size_t) c] = srcL[(size_t) c].data();
            inR[(size_t) c] = srcR[(size_t) c].data();
        }

        // Only 2 pairs actually exist -- the stored "99" must land somewhere
        // in [0,1], not index out of bounds.
        std::vector<float> l0 ((size_t) numSamples), r0 ((size_t) numSamples), l1 ((size_t) numSamples), r1 ((size_t) numSamples);
        std::vector<float*> outL { l0.data(), l1.data() }, outR { r0.data(), r1.data() };
        m.mixDown (inL, inR, outL, outR, numSamples);   // must not crash / read or write out of bounds

        const bool landedInPair0 = nearlyEqual (l0[0], 1.0f);
        const bool landedInPair1 = nearlyEqual (l1[0], 1.0f);
        CHECK (landedInPair0 || landedInPair1,
               "an output-pair index far beyond the available pairs clamps into a VALID pair -- the signal isn't silently dropped");
        CHECK (! (landedInPair0 && landedInPair1),
               "a channel's signal lands in exactly one pair, never duplicated across pairs by the clamp");
    }

    // ---- single-pair device: mixDown() behaves identically to its ---------
    // ---- pre-routing behavior (every channel defaults to pair 0) ----------
    {
        Mixer m;   // no setChannelOutputPair calls -- every channel at its default (pair 0)
        m.setMasterGain (0.8f);

        std::array<std::vector<float>, kNumMixerChannels> srcL, srcR;
        std::array<const float*, kNumMixerChannels> inL {}, inR {};
        float expectedSum = 0.0f;
        for (int c = 0; c < kNumMixerChannels; ++c)
        {
            const float v = 0.05f * (float) (c + 1);
            expectedSum += v;
            srcL[(size_t) c] = constantSource (numSamples, v);
            srcR[(size_t) c] = srcL[(size_t) c];
            inL[(size_t) c] = srcL[(size_t) c].data();
            inR[(size_t) c] = srcR[(size_t) c].data();
        }
        expectedSum *= 0.8f;   // Master

        std::vector<float> l0 ((size_t) numSamples), r0 ((size_t) numSamples);
        std::vector<float*> outL { l0.data() }, outR { r0.data() };   // exactly 1 pair -- the ordinary stereo-device case
        m.mixDown (inL, inR, outL, outR, numSamples);

        bool allMatch = true;
        for (int i = 0; i < numSamples; ++i)
            if (! nearlyEqual (l0[(size_t) i], expectedSum) || ! nearlyEqual (r0[(size_t) i], expectedSum)) { allMatch = false; break; }
        CHECK (allMatch,
               "with a single output pair and every channel at its default routing, mixDown() sums identically to its pre-routing single-output behavior");
    }

    std::printf ("\n%d/%d PASS\n", gPass, gTotal);
    return gPass == gTotal ? 0 : 1;
}
