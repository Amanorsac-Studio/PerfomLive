// ============================================================================
//  Arrangement.h -- the song model for section playback.
//
//  A SONG is a stem-mode deck. Its ARRANGEMENT is an ordered list of named
//  SECTIONS laid along the deck in bars: Intro, Verse 1, Chorus... Each
//  section ends where the next begins; the last ends at the stem's end.
//
//  This lives OUTSIDE the engine on purpose. Deck.h and Session.h only ever
//  see sample positions; bars, names, colours and flags are a performer's
//  vocabulary, converted to samples at the moment a jump or loop is armed.
//  That keeps the engine's tests in plain numbers and keeps this file free
//  to change shape as the product does.
//
//  Design borrowed from AbleSet, translated out of Ableton's locator syntax
//  into plain fields:
//    skip        "+SKIP"  -- passed over when stepping to the next section
//    optional    "+END"   -- not played in sequence, reachable by jumping
//                           (the extra chorus that exists if it's called for)
//    loopOnEntry "+LOOP"  -- arms a loop the moment this section starts
//    pauseAfter  "+PAUSE" -- playback stops at the end of this section
//
//  Pure JUCE-free C++ so it can be unit-tested standalone.
// ============================================================================
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

namespace ezarr
{

struct Section
{
    std::string name;
    int         startBar { 0 };        // 0-based bar index where this section begins
    uint32_t    colourArgb { 0 };      // 0 = derive from position
    bool        skip { false };
    bool        optional { false };
    bool        loopOnEntry { false };
    bool        pauseAfter { false };
    // Native cue for this section (Guide.h): "" = spoken from the name,
    // "-" = silent, else a cue stem such as "Chorus-2" or "Last-Time".
    std::string cue;
};

// What happens when a song ends. Appended only: projects store the number.
// Owner: "let's have options for what happens when one song ends -- next,
// cue next, fade in, etc."
enum class EndBehaviour { stop, cueNext, autoAdvance, advanceWithCountIn, fadeIntoNext, repeat };

struct Arrangement
{
    std::vector<Section> sections;     // kept sorted by startBar, unique startBar
    int          lengthBars { 0 };     // the song's length; sections beyond it are clamped on use
    EndBehaviour atEnd { EndBehaviour::autoAdvance };
    int          endFadeSeconds { 2 };   // fadeIntoNext: how long the next song takes to come up (1, 2 or 4)
    int          countInBars { 0 };    // 0 = none, else 1/2/4

    // Native guide tracks (Guide.h). Off by default: a stem pack that ships
    // its own click and cue stems should not get a second set on top.
    bool guideClick { false };
    bool guideCues { false };
    int  cueLeadBars { 2 };            // the name is spoken this many bars early
    bool cueCounts { true };           // "1, 2, 3, 4" into each section

    bool empty() const { return sections.empty(); }

    // ---- editing ---------------------------------------------------------

    /** Inserts or replaces the section starting at `bar`. Returns its index. */
    int addSection (Section s)
    {
        s.startBar = (std::max) (0, s.startBar);
        for (size_t i = 0; i < sections.size(); ++i)
            if (sections[i].startBar == s.startBar) { sections[i] = s; return (int) i; }
        sections.push_back (s);
        sortSections();
        for (size_t i = 0; i < sections.size(); ++i)
            if (sections[i].startBar == s.startBar) return (int) i;
        return -1;
    }

    void removeSection (int index)
    {
        if (index >= 0 && index < (int) sections.size())
            sections.erase (sections.begin() + index);
    }

    /** Moves a section's start, refusing to cross its neighbours. */
    bool moveSection (int index, int newStartBar)
    {
        if (index < 0 || index >= (int) sections.size()) return false;
        const int lo = index > 0 ? sections[(size_t) index - 1].startBar + 1 : 0;
        const int hi = index + 1 < (int) sections.size() ? sections[(size_t) index + 1].startBar - 1
                                                        : (std::max) (lo, lengthBars - 1);
        if (hi < lo) return false;
        sections[(size_t) index].startBar = std::clamp (newStartBar, lo, hi);
        return true;
    }

    /** Splits the song into equal sections of `barsEach` -- a quick first draft. */
    void autoSection (int barsEach, const std::vector<std::string>& names = {})
    {
        sections.clear();
        if (barsEach <= 0 || lengthBars <= 0) return;
        int n = 0;
        for (int b = 0; b < lengthBars; b += barsEach, ++n)
        {
            Section s;
            s.startBar = b;
            s.name = n < (int) names.size() ? names[(size_t) n] : ("Section " + std::to_string (n + 1));
            sections.push_back (s);
        }
    }

    void sortSections()
    {
        std::sort (sections.begin(), sections.end(),
                   [] (const Section& a, const Section& b) { return a.startBar < b.startBar; });
    }

    // ---- geometry --------------------------------------------------------

    int sectionStartBar (int index) const
    {
        return (index >= 0 && index < (int) sections.size()) ? sections[(size_t) index].startBar : 0;
    }

    /** One past the last bar of the section: the next section's start, or the song's end. */
    int sectionEndBar (int index) const
    {
        if (index < 0 || index >= (int) sections.size()) return lengthBars;
        if (index + 1 < (int) sections.size())
            return (std::min) (lengthBars, sections[(size_t) index + 1].startBar);
        return lengthBars;
    }

    int sectionLengthBars (int index) const { return (std::max) (0, sectionEndBar (index) - sectionStartBar (index)); }

    /** Which section contains `bar` (fractional bars welcome). -1 before the first. */
    int sectionAtBar (double bar) const
    {
        int found = -1;
        for (size_t i = 0; i < sections.size(); ++i)
            if ((double) sections[i].startBar <= bar) found = (int) i;
        return found;
    }

    // ---- navigation ------------------------------------------------------

    /**
     * The section a performer stepping forward should land on: the next one
     * that is neither skipped nor optional. -1 = there is none (end of song).
     */
    int nextPlayableAfter (int index) const
    {
        for (int i = index + 1; i < (int) sections.size(); ++i)
        {
            const auto& s = sections[(size_t) i];
            if (! s.skip && ! s.optional) return i;
        }
        return -1;
    }

    int prevPlayableBefore (int index) const
    {
        for (int i = (std::min) (index, (int) sections.size()) - 1; i >= 0; --i)
        {
            const auto& s = sections[(size_t) i];
            if (! s.skip && ! s.optional) return i;
        }
        return -1;
    }

    /** First section that would actually be played from the top. */
    int firstPlayable() const { return nextPlayableAfter (-1); }
};

// ---- bars <-> deck samples ---------------------------------------------------
//
// The deck's playhead counts LAYER samples (the file's own rate), advanced by
// rateRatio per device sample. A bar in playhead units is therefore a bar of
// device samples times rateRatio. Both inputs come from the engine, so a jump
// armed from here lands on the same sample the engine's own bar boundaries do.

inline double samplesPerBarInDeckUnits (double bpm, int beatsPerBar, double deviceSampleRate, double rateRatio)
{
    if (bpm <= 0.0 || beatsPerBar <= 0 || deviceSampleRate <= 0.0) return 0.0;
    const double deviceSamplesPerBar = (60.0 / bpm) * (double) beatsPerBar * deviceSampleRate;
    return deviceSamplesPerBar * (rateRatio > 0.0 ? rateRatio : 1.0);
}

inline double barToDeckSamples (double bar, double samplesPerBar) { return bar * samplesPerBar; }
inline double deckSamplesToBar (double pos, double samplesPerBar) { return samplesPerBar > 0.0 ? pos / samplesPerBar : 0.0; }

/** Default section colours -- a warm-to-cool wheel that stays legible on a dark stage screen. */
inline uint32_t defaultSectionColour (int index)
{
    static const uint32_t wheel[] = {
        0xff00d9ff, 0xffa855f7, 0xffff2d95, 0xffffa62b, 0xff3dffc0, 0xffff5c3b, 0xff4d7cff, 0xffb6ff2e,
    };
    return wheel[(size_t) (index < 0 ? 0 : index) % 8];
}

/** Worship-shaped starter names for auto-sectioning, in the order most songs run. */
inline std::vector<std::string> worshipSectionNames()
{
    return { "Intro", "Verse 1", "Chorus", "Verse 2", "Chorus", "Bridge", "Chorus", "Outro" };
}

} // namespace ezarr
