// ============================================================================
//  CueDetect.h -- reading a song's own cue track to draft its sections.
//
//  Owner: "if they import a song that already has cues, is it possible to
//  listen to the cues and use it to build the sections? Usually the cues come
//  four bars before the section." Yes, in two halves:
//
//    WHEN is reliable. A cue track is silence with short bursts of speech
//    in it, so the bursts are found by energy alone, and a burst that lands
//    N bars before a bar line marks a section start there. A run of three
//    or four short bursts a beat apart is a count ("1, 2, 3, 4"): the
//    section starts on the bar after it.
//
//    WHAT is best-effort. The app has no speech recognition, but it does
//    have the Motion Worship recordings (Guide.h). Each burst's loudness
//    envelope is compared with every recording's; a close enough match
//    names the section ("Chorus", "Bridge", "Last time"). Cues made with
//    those recordings name themselves; anything else becomes "Section N"
//    for the performer to rename with one click.
//
//  Pure C++, no JUCE (cuedetecttest.cpp). Works on a mono signal at any
//  rate; the caller says which. Envelopes are 10 ms frames.
// ============================================================================
#pragma once

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <functional>
#include <string>
#include <vector>

namespace ezcue
{

constexpr double kFrameSeconds = 0.010;

/** RMS per 10 ms frame, in dBFS (silence floors at -100). */
inline std::vector<float> envelopeDb (const float* samples, size_t n, double sampleRate)
{
    const size_t frame = (size_t) std::max (1.0, sampleRate * kFrameSeconds);
    std::vector<float> out;
    out.reserve (n / frame + 1);
    for (size_t i = 0; i < n; i += frame)
    {
        double sq = 0.0;
        const size_t end = std::min (n, i + frame);
        for (size_t k = i; k < end; ++k) sq += (double) samples[k] * (double) samples[k];
        const double rms = std::sqrt (sq / (double) std::max<size_t> (1, end - i));
        out.push_back ((float) (rms > 1.0e-5 ? 20.0 * std::log10 (rms) : -100.0));
    }
    return out;
}

struct Burst
{
    double startSeconds { 0.0 };
    double lengthSeconds { 0.0 };
    size_t firstFrame { 0 }, lastFrame { 0 };
};

/** Bursts of sound above the track's own noise floor. Two bursts closer
    than `gapSeconds` are one burst (a spoken word has tiny gaps in it). */
inline std::vector<Burst> findBursts (const std::vector<float>& envDb, double gapSeconds = 0.25,
                                      double minLengthSeconds = 0.08, float aboveFloorDb = 18.0f, float absoluteFloorDb = -55.0f)
{
    std::vector<Burst> out;
    if (envDb.empty()) return out;

    // the noise floor is where most frames sit
    std::vector<float> sorted (envDb);
    std::sort (sorted.begin(), sorted.end());
    const float floorDb = sorted[sorted.size() / 2];
    const float threshold = std::max (absoluteFloorDb, floorDb + aboveFloorDb);

    const size_t gapFrames = (size_t) std::max (1.0, gapSeconds / kFrameSeconds);
    size_t i = 0;
    while (i < envDb.size())
    {
        if (envDb[i] < threshold) { ++i; continue; }
        Burst b;
        b.firstFrame = i;
        size_t last = i;
        size_t j = i;
        while (j < envDb.size())
        {
            if (envDb[j] >= threshold) last = j;
            else if (j - last > gapFrames) break;
            ++j;
        }
        b.lastFrame = last;
        b.startSeconds  = (double) b.firstFrame * kFrameSeconds;
        b.lengthSeconds = (double) (b.lastFrame - b.firstFrame + 1) * kFrameSeconds;
        if (b.lengthSeconds >= minLengthSeconds) out.push_back (b);
        i = last + 1;
    }
    return out;
}

/** The clicks of a click track: short bursts, no gap-merging (two clicks a
    beat apart must stay two bursts), a low bar so quiet clicks count. */
inline std::vector<Burst> findClicks (const std::vector<float>& envDb)
{
    return findBursts (envDb, 0.03, kFrameSeconds, 12.0f, -60.0f);
}

/** Tempo from the spacing of a click track's clicks, in BPM: the gaps are
    read, the typical (median) one is kept, and the gaps near it are averaged
    for precision, so a few missed or doubled clicks don't move the answer.
    Anything faster than 200 BPM is read as eighth notes and halved. 0 when
    there are fewer than 8 clicks. */
inline double tempoFromClicks (const std::vector<Burst>& clicks)
{
    if (clicks.size() < 8) return 0.0;
    std::vector<double> gaps;
    for (size_t i = 1; i < clicks.size(); ++i)
    {
        const double g = clicks[i].startSeconds - clicks[i - 1].startSeconds;
        if (g > 0.1 && g < 4.0) gaps.push_back (g);   // 15..600 BPM
    }
    if (gaps.size() < 7) return 0.0;
    std::vector<double> sorted (gaps);
    std::sort (sorted.begin(), sorted.end());
    const double median = sorted[sorted.size() / 2];

    double sum = 0.0; int n = 0;
    for (double g : gaps)
        if (std::fabs (g - median) <= median * 0.08) { sum += g; ++n; }
    if (n == 0) return 0.0;
    double bpm = 60.0 / (sum / (double) n);
    while (bpm > 200.0) bpm *= 0.5;
    return std::round (bpm * 10.0) / 10.0;
}

/** The typical gap between a click track's clicks, in seconds: the median
    gap, refined by averaging the gaps near it. 0 with fewer than 8 clicks. */
inline double clickIntervalSeconds (const std::vector<Burst>& clicks)
{
    if (clicks.size() < 8) return 0.0;
    std::vector<double> gaps;
    for (size_t i = 1; i < clicks.size(); ++i)
    {
        const double g = clicks[i].startSeconds - clicks[i - 1].startSeconds;
        if (g > 0.1 && g < 4.0) gaps.push_back (g);
    }
    if (gaps.size() < 7) return 0.0;
    std::vector<double> sorted (gaps);
    std::sort (sorted.begin(), sorted.end());
    const double median = sorted[sorted.size() / 2];
    double sum = 0.0; int n = 0;
    for (double g : gaps)
        if (std::fabs (g - median) <= median * 0.08) { sum += g; ++n; }
    return n > 0 ? sum / (double) n : 0.0;
}

/** The accent period in any per-click measure (loudness, pitch): the period
    whose clicks stand out most from the rest, if by at least `minSeparation`.
    Clicks are put on a grid by their time, so a few missing ones (a click
    that drops out for a breakdown) don't shift the count. */
inline int accentPeriodOf (const std::vector<Burst>& clicks, const std::vector<float>& values, double interval, float minSeparation,
                           int onlyPeriod = 0, int* phaseOut = nullptr)
{
    if (clicks.size() < 16 || interval <= 0.0 || values.size() != clicks.size()) return 0;
    struct Hit { long slot; float value; };
    std::vector<Hit> hits;
    // slots are counted from a fixed origin (never from the first click of a
    // subset), so a phase means the same clicks in every subset
    for (size_t i = 0; i < clicks.size() && hits.size() < 400; ++i)
        hits.push_back ({ (long) std::llround (clicks[i].startSeconds / interval), values[i] });

    int best = 0, bestPhase = 0;
    float bestSeparation = 0.0f;
    for (int period : { 2, 3, 4, 5, 6, 7, 8, 9, 12 })
    {
        if (onlyPeriod > 0 && period != onlyPeriod) continue;
        if ((long) hits.size() < period * 4) continue;
        for (int phase = 0; phase < period; ++phase)
        {
            double accent = 0.0, rest = 0.0;
            int na = 0, nr = 0;
            for (const auto& h : hits)
            {
                if ((int) (((h.slot % period) + period) % period) == phase) { accent += h.value; ++na; }
                else                                                        { rest   += h.value; ++nr; }
            }
            if (na == 0 || nr == 0) continue;
            const float separation = (float) (accent / na - rest / nr);
            // a longer period has to separate clearly better than a shorter one
            if (separation > bestSeparation + minSeparation * 0.15f) { bestSeparation = separation; best = period; bestPhase = phase; }
        }
    }
    if (bestSeparation < minSeparation) return 0;
    if (phaseOut != nullptr) *phaseOut = bestPhase;
    return best;
}

/** From loudness: the period whose clicks are loudest. 0 when nothing stands out. */
inline int accentPeriod (const std::vector<Burst>& clicks, const std::vector<float>& envDb, double interval)
{
    std::vector<float> level;
    for (const auto& c : clicks)
    {
        float peak = -100.0f;
        for (size_t f = c.firstFrame; f <= c.lastFrame && f < envDb.size(); ++f) peak = std::max (peak, envDb[f]);
        level.push_back (peak);
    }
    return accentPeriodOf (clicks, level, interval, 1.5f);
}

/** How high each click is pitched, in Hz: the strongest autocorrelation lag
    over its first 20 ms (200 Hz - 4 kHz). Click tracks mark beat one -- or a
    beat against its off-beat (MultiTracks) -- with a higher or lower click at
    the same loudness, which loudness alone can't see. (Counting zero
    crossings was tried first and is too noisy on a real click's room tail.) */
inline std::vector<float> clickPitches (const std::vector<Burst>& clicks, const float* samples, size_t n, double rate)
{
    std::vector<float> out;
    const size_t window = (size_t) std::max (32.0, rate * 0.025);
    const int maxLag = std::max (4, (int) (rate / 200.0));
    std::vector<double> corr ((size_t) maxLag + 1);
    for (const auto& c : clicks)
    {
        // a few ms in, past the attack, where the click's tone is
        const size_t a = (size_t) std::max (0.0, c.startSeconds * rate + rate * 0.003);
        // normalised autocorrelation: the sum per overlapping pair, so short
        // lags aren't favoured just for having more pairs
        for (int lag = 1; lag <= maxLag; ++lag)
        {
            double s = 0.0;
            size_t pairs = 0;
            for (size_t k = 0; k + (size_t) lag < window && a + k + (size_t) lag < n; ++k, ++pairs)
                s += (double) samples[a + k] * (double) samples[a + k + (size_t) lag];
            corr[(size_t) lag] = pairs > 0 ? s / (double) pairs : 0.0;
        }
        // skip the lags still inside the first lobe (until the correlation
        // first turns negative), then the strongest peak is one period
        int lag = 1;
        while (lag <= maxLag && corr[(size_t) lag] > 0.0) ++lag;
        int bestLag = 0;
        double bestCorr = 0.0;
        for (; lag <= maxLag; ++lag)
            if (corr[(size_t) lag] > bestCorr) { bestCorr = corr[(size_t) lag]; bestLag = lag; }
        out.push_back (bestLag > 0 && (double) bestLag >= rate / 4000.0 ? (float) (rate / bestLag) : 0.0f);
    }
    return out;
}

/** A tempo written in a file or folder name ("...-G-66.00bpm\Click.wav",
    "Grace 72BPM - Drums.wav"): the number just before "bpm", 40-250. The
    name nearest the file wins. 0 when there is none. */
inline double tempoFromName (const std::string& path)
{
    std::string s (path);
    std::transform (s.begin(), s.end(), s.begin(), [] (unsigned char ch) { return (char) std::tolower (ch); });
    double found = 0.0;
    for (size_t pos = s.find ("bpm"); pos != std::string::npos; pos = s.find ("bpm", pos + 3))
    {
        size_t end = pos;
        while (end > 0 && (s[end - 1] == ' ' || s[end - 1] == '_' || s[end - 1] == '-')) --end;
        size_t start = end;
        while (start > 0 && (std::isdigit ((unsigned char) s[start - 1]) || s[start - 1] == '.')) --start;
        if (start < end)
        {
            const double v = std::atof (s.substr (start, end - start).c_str());
            if (v >= 40.0 && v <= 250.0) found = v;
        }
    }
    return found;
}

struct ClickReading
{
    double bpm { 0.0 };            // 0 = no steady clicks
    int beatsPerBar { 0 };         // 0 = no beat one heard
    // The clicks alternate two sounds with no beat one above that: beats and
    // their off-beats (bpm / 2 is the song's tempo), or a bar of two. Only
    // the performer -- or a tempo in the file name -- can say which.
    bool alternating { false };
    double clicksPerMinute { 0.0 };
};

/** Tempo and time signature from a click track. Clicks faster than 200 a
    minute are read as subdivisions (eighth notes), for the tempo and for the
    beats in a bar alike. Beat one is found by loudness, else -- when
    `samples` are given -- by pitch. */
inline ClickReading readClickTrack (const std::vector<float>& envDb, const float* samples = nullptr, size_t n = 0, double rate = 0.0)
{
    ClickReading r;
    const auto clicks = findClicks (envDb);
    const double interval = clickIntervalSeconds (clicks);
    if (interval <= 0.0) return r;
    r.clicksPerMinute = std::round (600.0 / interval) / 10.0;

    // What each click sounds like: its loudness and, from the audio, its pitch.
    // From the audio, loudness is each click's own sample peak: the 10 ms
    // envelope frames drift against the click spacing and invent a slow
    // "accent" pattern (on a real 132-a-minute click, a 22-click cycle) that
    // isn't in the sound.
    std::vector<float> level;
    const bool haveAudio = samples != nullptr && n > 0 && rate > 0.0;
    for (const auto& c : clicks)
    {
        float peak = haveAudio ? 0.0f : -100.0f;
        if (haveAudio)
        {
            const size_t a = (size_t) std::max (0.0, c.startSeconds * rate - rate * 0.005);
            const size_t b = std::min (n, a + (size_t) (rate * 0.035));
            for (size_t i = a; i < b; ++i) peak = std::max (peak, std::fabs (samples[i]));
            peak = peak > 1.0e-5f ? 20.0f * std::log10 (peak) : -100.0f;
        }
        else
        {
            for (size_t f = c.firstFrame; f <= c.lastFrame && f < envDb.size(); ++f) peak = std::max (peak, envDb[f]);
        }
        level.push_back (peak);
    }
    std::vector<float> pitch;
    float pitchStep = 0.0f;
    if (haveAudio)
    {
        pitch = clickPitches (clicks, samples, n, rate);
        double mean = 0.0;
        for (float p : pitch) mean += p;
        pitchStep = (float) (0.08 * mean / (double) std::max<size_t> (1, pitch.size()));   // a clearly different click: 8% of the typical pitch
    }

    // the accent period of a set of clicks: louder, else higher, else lower
    auto periodOf = [&] (const std::vector<Burst>& cs, const std::vector<float>& lv, const std::vector<float>& pt, double iv,
                         int only, int* phase)
    {
        int p = accentPeriodOf (cs, lv, iv, 1.5f, only, phase);
        if (p == 0 && ! pt.empty()) p = accentPeriodOf (cs, pt, iv, pitchStep, only, phase);
        if (p == 0 && ! pt.empty())
        {
            std::vector<float> lower (pt);
            for (auto& v : lower) v = -v;
            p = accentPeriodOf (cs, lower, iv, pitchStep, only, phase);
        }
        return p;
    };

    // the clicks whose slot, in a pattern of `period`, is (or isn't) `phase`
    const double iv = interval;
    auto subset = [&] (int period, int phase, bool keepPhase, std::vector<Burst>& cs, std::vector<float>& lv, std::vector<float>& pt)
    {
        for (size_t i = 0; i < clicks.size(); ++i)
        {
            const long slot = (long) std::llround (clicks[i].startSeconds / iv);
            const bool inPhase = (int) (((slot % period) + period) % period) == phase;
            if (inPhase != keepPhase) continue;
            cs.push_back (clicks[i]);
            lv.push_back (level[i]);
            if (! pitch.empty()) pt.push_back (pitch[i]);
        }
    };

    // The most distinct accent first -- which may be beat one of the bar. A
    // shorter pattern dividing it is the beat against its off-beat only if it
    // still stands out once beat one's clicks are set aside; otherwise it was
    // just beat one showing through (6/8 accented every six clicks).
    int strongestPhase = 0;
    const int strongest = periodOf (clicks, level, pitch, interval, 0, &strongestPhase);
    int first = strongest;
    if (strongest > 2)
    {
        std::vector<Burst> others;
        std::vector<float> otherLevel, otherPitch;
        subset (strongest, strongestPhase, false, others, otherLevel, otherPitch);
        for (int d = 2; d < strongest; ++d)
            if (strongest % d == 0 && periodOf (others, otherLevel, otherPitch, interval, d, nullptr) == d) { first = d; break; }
    }

    int clicksPerBeat = 1, beatsInClicks = first;
    if (first > 1)
    {
        // A second level: among the clicks of one phase of that pattern, is
        // there a beat one? Then the first pattern was the beat and its
        // subdivision, and the second is the bar.
        for (int phase = 0; phase < first; ++phase)
        {
            std::vector<Burst> sub;
            std::vector<float> subLevel, subPitch;
            subset (first, phase, true, sub, subLevel, subPitch);
            const int second = periodOf (sub, subLevel, subPitch, interval * first, 0, nullptr);
            if (second > 1) { clicksPerBeat = first; beatsInClicks = second * first; break; }
        }
        if (clicksPerBeat == 1 && first == 2) { r.alternating = true; beatsInClicks = 0; }
    }

    double bpm = 60.0 / interval / clicksPerBeat;
    while (bpm > 200.0) { bpm *= 0.5; clicksPerBeat *= 2; }
    r.bpm = std::round (bpm * 10.0) / 10.0;
    if (beatsInClicks > 0 && beatsInClicks % clicksPerBeat == 0) r.beatsPerBar = beatsInClicks / clicksPerBeat;
    return r;
}

/** The same, straight from the audio. */
inline ClickReading readClickTrack (const float* samples, size_t n, double rate)
{
    return readClickTrack (envelopeDb (samples, n, rate), samples, n, rate);
}

/** How alike two envelopes are, 0..1: normalised cross-correlation of the
    above-floor parts, allowing a small time slip. */
inline float envelopeSimilarity (const std::vector<float>& a, const std::vector<float>& b, int maxSlipFrames = 8)
{
    if (a.size() < 4 || b.size() < 4) return 0.0f;
    // lengths must roughly agree: "Chorus" is not "Instrumental"
    const double ratio = (double) a.size() / (double) b.size();
    if (ratio < 0.6 || ratio > 1.6) return 0.0f;

    auto shape = [] (const std::vector<float>& v)
    {
        std::vector<double> s (v.size());
        double mean = 0.0;
        for (size_t i = 0; i < v.size(); ++i) { s[i] = std::max (-60.0, (double) v[i]); mean += s[i]; }
        mean /= (double) std::max<size_t> (1, s.size());
        double norm = 0.0;
        for (auto& x : s) { x -= mean; norm += x * x; }
        norm = std::sqrt (std::max (1.0e-9, norm));
        for (auto& x : s) x /= norm;
        return s;
    };
    const auto sa = shape (a), sb = shape (b);

    float best = 0.0f;
    for (int slip = -maxSlipFrames; slip <= maxSlipFrames; ++slip)
    {
        double dot = 0.0;
        for (size_t i = 0; i < sa.size(); ++i)
        {
            const long j = (long) i + slip;
            if (j >= 0 && j < (long) sb.size()) dot += sa[i] * sb[(size_t) j];
        }
        best = std::max (best, (float) dot);
    }
    return std::min (1.0f, best);
}

struct Reference
{
    std::string stem;            // "Chorus-2"
    std::vector<float> envDb;    // envelope of the recording, same frame size
};

struct Match
{
    std::string stem;            // matching: a recording's stem; speech: the section's name ("Chorus 3")
    float score { 0.0f };
    bool isCount { false };      // speech: a count word -- never a section name
    std::string heard;           // speech: the phrase that was heard ("chorus three")
};

/** Words a guide says that direct the band rather than name a section
    ("Drums in", "Build"). Said in the same bar as a section's name, the
    name wins; said alone, they still mark a section. */
inline bool isDirectionName (const std::string& name)
{
    static const char* const words[] = { "Drums In", "All In", "Build", "Slowly Build", "Softly", "Hits", "Hold", "Breakdown",
                                         "Last Time", "Again", "Repeat", "Wait", "Big Ending", "Key Change", "Key Change Up", "Key Change Down" };
    for (auto* w : words) if (name == w) return true;
    return false;
}

//==============================================================================
//  The words a guide track says, for a speech recognizer (SpeechCues.h).
//==============================================================================
struct CuePhrase
{
    std::string phrase;   // what is spoken, lower case: "chorus three"
    std::string name;     // the section it names: "Chorus 3"; "#count" for a count word
};

/** Every phrase a guide says: each recording's name spoken the natural way
    ("Chorus-3" -> "chorus three" -> Chorus 3), common spellings and extra
    words MultiTracks and Loop Community guides use, and the count words. */
inline std::vector<CuePhrase> cuePhrases (const std::vector<std::string>& stems)
{
    static const char* const numberWords[] = { "zero", "one", "two", "three", "four", "five", "six", "seven", "eight", "nine" };
    auto lower = [] (std::string s) { std::transform (s.begin(), s.end(), s.begin(), [] (unsigned char c) { return (char) std::tolower (c); }); return s; };
    auto titleWord = [] (std::string w) { if (! w.empty()) w[0] = (char) std::toupper ((unsigned char) w[0]); return w; };

    std::vector<CuePhrase> out;
    auto add = [&out] (const std::string& phrase, const std::string& name)
    {
        for (const auto& e : out) if (e.phrase == phrase) return;
        out.push_back ({ phrase, name });
    };

    auto addStem = [&] (const std::string& stem)
    {
        // split "Post-Chorus-2" into words; a trailing digit is spoken as a number
        std::vector<std::string> words;
        std::string cur;
        for (char ch : stem)
        {
            if (ch == '-' || ch == '_' || ch == ' ') { if (! cur.empty()) words.push_back (cur); cur.clear(); }
            else cur.push_back (ch);
        }
        if (! cur.empty()) words.push_back (cur);
        if (words.empty()) return;

        std::string number;
        if (words.back().size() == 1 && std::isdigit ((unsigned char) words.back()[0])) { number = words.back(); words.pop_back(); }
        if (words.empty()) return;

        std::vector<std::string> spoken;        // phrase words
        std::vector<std::string> shown;         // name words
        for (auto w : words)
        {
            const auto lw = lower (w);
            if (lw == "prechorus")  { spoken.push_back ("pre"); spoken.push_back ("chorus"); shown.push_back ("Pre-Chorus"); continue; }
            if (lw == "postchorus") { spoken.push_back ("post"); spoken.push_back ("chorus"); shown.push_back ("Post-Chorus"); continue; }
            spoken.push_back (lw);
            shown.push_back (titleWord (lw));
        }
        // "Post" + "Chorus" as separate stem words also reads Post-Chorus
        for (size_t i = 0; i + 1 < shown.size(); ++i)
            if ((shown[i] == "Pre" || shown[i] == "Post") && shown[i + 1] == "Chorus") { shown[i] += "-Chorus"; shown.erase (shown.begin() + (long) i + 1); }

        std::string phrase, name;
        for (const auto& w : spoken) phrase += (phrase.empty() ? "" : " ") + w;
        for (const auto& w : shown)  name   += (name.empty() ? "" : " ") + w;
        if (! number.empty())
        {
            phrase += " " + std::string (numberWords[number[0] - '0']);
            name   += " " + number;
        }
        add (phrase, name);
    };

    for (const auto& s : stems) addStem (s);

    // other words guides use, beyond the Motion Worship recordings
    static const char* const extra[][2] = {
        { "verse", "Verse" }, { "chorus", "Chorus" }, { "bridge", "Bridge" }, { "intro", "Intro" }, { "outro", "Outro" },
        { "pre chorus", "Pre-Chorus" }, { "post chorus", "Post-Chorus" }, { "tag", "Tag" }, { "ending", "Ending" },
        { "instrumental", "Instrumental" }, { "interlude", "Interlude" }, { "turnaround", "Turnaround" }, { "vamp", "Vamp" },
        { "breakdown", "Breakdown" }, { "build", "Build" }, { "refrain", "Refrain" }, { "channel", "Channel" },
        { "acapella", "Acapella" }, { "a cappella", "Acapella" }, { "drums in", "Drums In" }, { "all in", "All In" },
        { "last time", "Last Time" }, { "one more time", "Last Time" }, { "again", "Again" }, { "hits", "Hits" },
        { "hold", "Hold" }, { "big ending", "Big Ending" }, { "key change", "Key Change" }, { "solo", "Solo" },
        { "rap", "Rap" }, { "spontaneous", "Spontaneous" }, { "worship freely", "Worship Freely" },
    };
    for (const auto& e : extra) add (e[0], e[1]);
    for (const char* base : { "verse", "chorus", "bridge", "pre chorus", "post chorus", "tag", "interlude", "instrumental" })
        for (int n = 1; n <= 6; ++n)
        {
            std::string name = base == std::string ("pre chorus") ? "Pre-Chorus" : base == std::string ("post chorus") ? "Post-Chorus" : "";
            if (name.empty()) { name = base; name[0] = (char) std::toupper ((unsigned char) name[0]); }
            add (std::string (base) + " " + numberWords[n], name + " " + std::to_string (n));
        }

    // counts: named "#count" so they are never taken for a section name
    for (int n = 1; n <= 8; ++n) add (numberWords[n], "#count");
    add ("one two three four", "#count");
    add ("one two three", "#count");
    add ("one two", "#count");
    return out;
}

/** The section name a recognised phrase stands for; empty when unknown. */
inline std::string nameForPhrase (const std::vector<CuePhrase>& table, const std::string& phrase)
{
    for (const auto& e : table) if (e.phrase == phrase) return e.name;
    return {};
}

/** The part of an envelope within `dropDb` of its loudest frame -- a word
    without the silence either side of it, whatever the recording level. */
inline std::vector<float> trimToLoud (const std::vector<float>& env, float dropDb = 30.0f)
{
    if (env.empty()) return {};
    float peak = -200.0f;
    for (float v : env) peak = std::max (peak, v);
    size_t first = 0, last = env.size() - 1;
    while (first < env.size() && env[first] < peak - dropDb) ++first;
    while (last > first && env[last] < peak - dropDb) --last;
    return std::vector<float> (env.begin() + (long) first, env.begin() + (long) last + 1);
}

inline Match bestMatch (const std::vector<float>& burstEnv, const std::vector<Reference>& refs)
{
    Match m;
    const auto burst = trimToLoud (burstEnv);
    for (const auto& r : refs)
    {
        const auto trimmed = trimToLoud (r.envDb);
        const float s = envelopeSimilarity (burst, trimmed);
        if (s > m.score) { m.score = s; m.stem = r.stem; }
    }
    return m;
}

//==============================================================================
//  From bursts to sections.
//==============================================================================
struct DraftSection
{
    std::string name;
    int   startBar { 0 };
    float confidence { 0.0f };   // 1 = certain; below 0.5 the review window marks it "not sure"
    bool  fromCount { false };
    std::string heard;           // what the speech recognizer heard, if anything ("bridge")
    double cueSeconds { -1.0 };  // where the spoken cue is in the track, for "Hear" (-1 = none)
    double cueLength { 0.0 };
};

struct DetectSettings
{
    double secondsPerBar { 2.0 };
    int    beatsPerBar { 4 };
    int    leadBars { 2 };           // a spoken name is this many bars before its section
    float  nameThreshold { 0.55f };  // similarity needed to trust a name
};

/** Turns a cue track's bursts into a section list. `refs` may be empty
    (everything becomes "Section N"). Two bursts pointing at the same bar
    merge; a count wins over a name for placement, a section name wins over
    a direction ("Drums in") and over a guess for naming.

    `nameBurst`, when given, names each spoken burst instead of matching
    recordings -- the speech recognizer's answer (SpeechCues.h). A burst it
    calls a count is not a section name. */
inline std::vector<DraftSection> draftSections (const std::vector<Burst>& bursts,
                                                const std::vector<float>& envDb,
                                                const std::vector<Reference>& refs,
                                                const DetectSettings& s,
                                                int songLengthBars,
                                                const std::function<Match (size_t, const Burst&)>& nameBurst = {})
{
    std::vector<DraftSection> out;
    if (s.secondsPerBar <= 0.0 || bursts.empty()) return out;
    const double beat = s.secondsPerBar / (double) std::max (1, s.beatsPerBar);

    auto barOf = [&] (double seconds) { return (int) std::floor (seconds / s.secondsPerBar + 1.0e-6); };
    // a real section name outranks a direction, which outranks a guess
    auto rank = [] (const DraftSection& d) { return d.name.rfind ("Section ", 0) == 0 ? 0 : isDirectionName (d.name) ? 1 : 2; };
    auto addOrMerge = [&] (DraftSection d)
    {
        if (d.startBar <= 0 || (songLengthBars > 0 && d.startBar >= songLengthBars)) return;
        for (auto& e : out)
            if (std::abs (e.startBar - d.startBar) <= 1)
            {
                const bool takeName = rank (d) > rank (e) || (rank (d) == rank (e) && d.confidence > e.confidence);
                if (takeName)
                {
                    e.name = d.name; e.confidence = d.confidence; e.heard = d.heard;
                    e.cueSeconds = d.cueSeconds; e.cueLength = d.cueLength;
                }
                if (d.fromCount) { e.startBar = d.startBar; e.fromCount = true; }
                return;
            }
        out.push_back (d);
    };

    for (size_t i = 0; i < bursts.size(); ++i)
    {
        const auto& b = bursts[i];

        // ---- a count: short bursts a beat apart ----
        size_t j = i;
        int counted = 1;
        while (j + 1 < bursts.size())
        {
            const double gap = bursts[j + 1].startSeconds - bursts[j].startSeconds;
            if (bursts[j + 1].lengthSeconds > 0.6 || std::fabs (gap - beat) > beat * 0.25) break;
            ++counted;
            ++j;
        }
        if (counted >= 3 && b.lengthSeconds <= 0.6)
        {
            DraftSection d;
            d.startBar   = barOf (bursts[j].startSeconds + beat * 0.5) + 1;   // the bar after the last count
            d.name       = "Section " + std::to_string (out.size() + 1);
            d.confidence = 0.3f;
            d.fromCount  = true;
            addOrMerge (d);
            i = j;
            continue;
        }

        // ---- a spoken name ----
        DraftSection d;
        d.cueSeconds = b.startSeconds;
        d.cueLength  = b.lengthSeconds;
        d.startBar = barOf (b.startSeconds) + 1 + std::max (0, s.leadBars - 1);
        if (nameBurst)
        {
            const auto heard = nameBurst (i, b);
            if (heard.isCount) continue;
            d.heard = heard.heard;
            if (! heard.stem.empty()) { d.name = heard.stem; d.confidence = heard.score; }
            else                      { d.name = "Section " + std::to_string (out.size() + 1); d.confidence = 0.1f; }
            addOrMerge (d);
            continue;
        }
        std::vector<float> env (envDb.begin() + (long) b.firstFrame, envDb.begin() + (long) b.lastFrame + 1);
        const auto m = bestMatch (env, refs);
        // "Chorus" spoken in bar 11 (0-based 10) with a lead of 2 means the
        // chorus starts at bar 13: the next bar line after the burst, plus lead.
        d.startBar = barOf (b.startSeconds) + 1 + std::max (0, s.leadBars - 1);
        if (m.score >= s.nameThreshold)
        {
            std::string pretty = m.stem;
            std::replace (pretty.begin(), pretty.end(), '-', ' ');
            d.name = pretty;
            d.confidence = m.score;
        }
        else
        {
            d.name = "Section " + std::to_string (out.size() + 1);
            d.confidence = 0.2f;
        }
        addOrMerge (d);
    }

    std::sort (out.begin(), out.end(), [] (const DraftSection& a, const DraftSection& b) { return a.startBar < b.startBar; });
    // renumber the guesses in order
    int n = 1;
    for (auto& d : out)
        if (d.name.rfind ("Section ", 0) == 0) d.name = "Section " + std::to_string (n++);
    return out;
}

} // namespace ezcue
