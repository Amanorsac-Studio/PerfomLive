// ============================================================================
//  arrangetest.cpp -- standalone test for Arrangement.h, the song/section
//  model behind the PLAYBACK view. Pure C++, no JUCE.
//
//  The model is small but every rule in it is one a performer would notice
//  breaking mid-song: which section a bar belongs to, where a section ends,
//  what "next" skips, what a boundary drag may not cross, and that bars and
//  samples convert through the same arithmetic the engine uses.
//
//  Build & run:
//    cl /nologo /EHsc /std:c++17 /Fe:arrangetest.exe arrangetest.cpp
//    ./arrangetest.exe
// ============================================================================
#include "Arrangement.h"
#include <cmath>
#include <cstdio>

static int gPass = 0, gTotal = 0;
#define CHECK(cond, desc) do {                                    \
    ++gTotal;                                                     \
    if (cond) { ++gPass; std::printf ("  [PASS] %s\n", desc); }    \
    else      {          std::printf ("  [FAIL] %s\n", desc); }    \
} while (0)

static bool nearly (double a, double b, double eps = 1e-9) { return std::fabs (a - b) <= eps; }

using namespace ezarr;

static Arrangement typicalSong()
{
    Arrangement a;
    a.lengthBars = 32;
    Section s;
    s.name = "Intro";    s.startBar = 0;  a.addSection (s);
    s.name = "Verse 1";  s.startBar = 4;  a.addSection (s);
    s.name = "Chorus";   s.startBar = 12; a.addSection (s);
    s.name = "Bridge";   s.startBar = 20; a.addSection (s);
    s.name = "Chorus 2"; s.startBar = 24; a.addSection (s);
    return a;
}

int main()
{
    std::printf ("Arrangement.h tests\n");

    std::printf ("\n-- geometry --\n");
    {
        auto a = typicalSong();
        CHECK (a.sections.size() == 5, "five sections added");
        CHECK (a.sectionEndBar (0) == 4,  "a section ends where the next begins");
        CHECK (a.sectionEndBar (4) == 32, "the last section ends at the song's end");
        CHECK (a.sectionLengthBars (1) == 8, "length is end minus start");
        CHECK (a.sectionAtBar (0.0) == 0,   "bar 0 is the intro");
        CHECK (a.sectionAtBar (3.999) == 0, "just before a boundary is still the earlier section");
        CHECK (a.sectionAtBar (4.0) == 1,   "exactly on a boundary is the later section");
        CHECK (a.sectionAtBar (31.5) == 4,  "the tail of the song is the last section");
        CHECK (a.sectionAtBar (-1.0) == -1, "before the first section is nothing");
    }

    std::printf ("\n-- adding replaces, and stays sorted --\n");
    {
        auto a = typicalSong();
        Section s; s.name = "Tag"; s.startBar = 28;
        const int idx = a.addSection (s);
        CHECK (idx == 5 && a.sections.size() == 6, "a new start bar appends and reports its index");
        Section r; r.name = "Chorus (renamed)"; r.startBar = 12;
        const int same = a.addSection (r);
        CHECK (same == 2 && a.sections.size() == 6 && a.sections[2].name == "Chorus (renamed)",
               "adding at an existing start bar replaces that section rather than duplicating it");
        Section early; early.name = "Pickup"; early.startBar = 2;
        a.addSection (early);
        CHECK (a.sections[1].name == "Pickup" && a.sections[2].name == "Verse 1", "sections are kept in bar order");
    }

    std::printf ("\n-- moving a boundary --\n");
    {
        auto a = typicalSong();
        CHECK (a.moveSection (1, 6) && a.sectionStartBar (1) == 6, "a boundary moves within its neighbours");
        CHECK (a.moveSection (1, 0) && a.sectionStartBar (1) == 1, "...and clamps so it cannot cross the previous section");
        CHECK (a.moveSection (1, 99) && a.sectionStartBar (1) == 11, "...or the next one");
        CHECK (a.moveSection (4, 40) && a.sectionStartBar (4) == 31, "the last section cannot move past the song's end");
        CHECK (! a.moveSection (9, 3), "moving a section that does not exist is refused");
    }

    std::printf ("\n-- navigation honours skip and optional --\n");
    {
        auto a = typicalSong();
        a.sections[1].skip = true;       // Verse 1 skipped
        a.sections[3].optional = true;   // Bridge only if called
        CHECK (a.nextPlayableAfter (0) == 2, "next from Intro skips a +SKIP section and lands on Chorus");
        CHECK (a.nextPlayableAfter (2) == 4, "next from Chorus passes over an optional Bridge");
        CHECK (a.nextPlayableAfter (4) == -1, "next from the last section is the end of the song");
        CHECK (a.prevPlayableBefore (4) == 2, "previous from Chorus 2 also passes over the optional Bridge");
        CHECK (a.prevPlayableBefore (0) == -1, "previous from the first section is nothing");
        CHECK (a.firstPlayable() == 0, "the first playable section is the intro");
        a.sections[0].skip = true;
        CHECK (a.firstPlayable() == 2, "...unless it is skipped, in which case the first unskipped one");
    }

    std::printf ("\n-- auto-section --\n");
    {
        Arrangement a; a.lengthBars = 20;
        a.autoSection (8, worshipSectionNames());
        CHECK (a.sections.size() == 3, "20 bars every 8 = three sections (the last one short)");
        CHECK (a.sections[0].name == "Intro" && a.sections[1].name == "Verse 1" && a.sections[2].name == "Chorus",
               "worship names are applied in order");
        CHECK (a.sectionEndBar (2) == 20, "the short last section still ends at the song's end");
        Arrangement b; b.lengthBars = 0;
        b.autoSection (8);
        CHECK (b.sections.empty(), "a song with no length gets no sections");
        Arrangement c; c.lengthBars = 100;
        c.autoSection (8);
        CHECK (c.sections.size() == 13 && c.sections[12].name == "Section 13", "past the named list, sections are numbered");
    }

    std::printf ("\n-- bars <-> deck samples --\n");
    {
        // 120 BPM, 4/4, 48k device, file at 44.1k (ratio 0.91875): one bar
        // of device time is 96000 samples, which is 88200 layer samples.
        const double spb = samplesPerBarInDeckUnits (120.0, 4, 48000.0, 44100.0 / 48000.0);
        CHECK (nearly (spb, 88200.0, 1e-6), "a bar in deck units = device samples per bar x rateRatio");
        CHECK (nearly (barToDeckSamples (12, spb), 12 * 88200.0, 1e-6), "bar 12 is 12 bars of samples");
        CHECK (nearly (deckSamplesToBar (88200.0 * 5.5, spb), 5.5, 1e-9), "...and back, including the fractional bar");
        CHECK (samplesPerBarInDeckUnits (0.0, 4, 48000.0, 1.0) == 0.0, "a zero tempo yields 0 rather than dividing by it");
        CHECK (deckSamplesToBar (1234.0, 0.0) == 0.0, "a zero bar length yields bar 0 rather than infinity");
        CHECK (nearly (samplesPerBarInDeckUnits (120.0, 4, 48000.0, 0.0), 96000.0, 1e-9), "a zero rateRatio is treated as 1");
    }

    std::printf ("\n-- colours --\n");
    {
        CHECK (defaultSectionColour (0) == defaultSectionColour (8), "the colour wheel wraps every 8");
        CHECK (defaultSectionColour (-3) == defaultSectionColour (0), "a negative index is clamped, not undefined");
    }

    std::printf ("\n%d / %d passed\n", gPass, gTotal);
    return gPass == gTotal ? 0 : 1;
}
