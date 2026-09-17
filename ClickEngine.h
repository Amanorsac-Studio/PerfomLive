// ============================================================================
//  ClickEngine.h -- the one click: loop mode, songs and the count-in.
//
//  Owner: "let's work on the metronome settings... we should have different
//  metronome settings." Before this, loop mode had a plain beep with no
//  accent or level (Metronome.h) and songs had their own sine click with a
//  fixed accent (Guide.h's Click); the only setting was on/off.
//
//  Now one engine plays every click, from shared settings (ClickParams):
//    sound         Classic, Woodblock, Hi-hat, Cowbell, Rim, Clave, Beep
//    level         the click's own volume (the Click strip still sets the mix)
//    accent        beat one louder and in its own voice, or every beat alike
//    beat levels   beat one, the other beats, and subdivisions separately
//    subdivision   none, eighths, triplets or sixteenths between the beats
//
//  Real-time: every sound is rendered once in prepare() (message thread, when
//  the device starts); render() only copies samples and reads atomics, so a
//  settings change never allocates on the audio thread. Pure C++, tested by
//  clicktest.cpp.
// ============================================================================
#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <random>
#include <vector>

namespace ezclick
{

enum class Sound { classic, woodblock, hihat, cowbell, rim, clave, beep, count };
constexpr int kNumSounds = (int) Sound::count;

inline const char* soundName (Sound s)
{
    switch (s)
    {
        case Sound::classic:   return "Classic";
        case Sound::woodblock: return "Woodblock";
        case Sound::hihat:     return "Hi-hat";
        case Sound::cowbell:   return "Cowbell";
        case Sound::rim:       return "Rim";
        case Sound::clave:     return "Clave";
        case Sound::beep:      return "Beep";
        default:               return "";
    }
}

enum class Hit { accent, beat, sub, count };
constexpr int kNumHits = (int) Hit::count;

/** What the person chose. Written by the UI, read by the audio thread. */
struct ClickParams
{
    std::atomic<int>   sound       { (int) Sound::classic };
    std::atomic<float> level       { 1.0f };    // 0..1.5
    std::atomic<bool>  accent      { true };    // beat one in its own, louder voice
    std::atomic<float> accentLevel { 1.0f };    // 0..1
    std::atomic<float> beatLevel   { 0.62f };   // 0..1
    std::atomic<int>   subdivision { 1 };       // clicks per beat: 1, 2, 3 or 4
    std::atomic<float> subLevel    { 0.35f };   // 0..1
};

//==============================================================================
/** Every sound, as three short recordings (beat one, beat, subdivision). */
class SoundBank
{
public:
    void build (double sampleRate)
    {
        sr = sampleRate > 0.0 ? sampleRate : 44100.0;
        for (int s = 0; s < kNumSounds; ++s)
            for (int h = 0; h < kNumHits; ++h)
                hits[(size_t) s][(size_t) h] = render ((Sound) s, (Hit) h);
    }

    const std::vector<float>& get (Sound s, Hit h) const
    {
        const int si = std::clamp ((int) s, 0, kNumSounds - 1);
        return hits[(size_t) si][(size_t) h];
    }

private:
    static constexpr double kTwoPi = 2.0 * 3.14159265358979323846;

    std::vector<float> render (Sound s, Hit h) const
    {
        const bool accent = h == Hit::accent;
        const bool sub    = h == Hit::sub;
        std::mt19937 rng (1234u + (unsigned) s * 7u + (unsigned) h);
        std::uniform_real_distribution<double> noise (-1.0, 1.0);

        auto tone = [&] (double seconds, double hz, double decayTo, double partial2 = 0.0, double mix2 = 0.0)
        {
            const size_t n = (size_t) std::max (1.0, seconds * sr);
            std::vector<float> out (n);
            const double decay = std::pow (decayTo, 1.0 / (double) n);
            double env = 1.0;
            for (size_t i = 0; i < n; ++i)
            {
                const double t = (double) i / sr;
                double v = std::sin (kTwoPi * hz * t);
                if (mix2 > 0.0) v = (1.0 - mix2) * v + mix2 * std::sin (kTwoPi * partial2 * t);
                out[i] = (float) (v * env);
                env *= decay;
            }
            return out;
        };

        std::vector<float> out;
        switch (s)
        {
            case Sound::classic:   // the click PerformLive always had
                out = tone (sub ? 0.018 : 0.028, accent ? 1760.0 : (sub ? 880.0 : 1175.0), 0.001);
                break;

            case Sound::woodblock:
                out = tone (0.045, accent ? 1250.0 : (sub ? 700.0 : 900.0), 0.0005,
                            accent ? 2600.0 : (sub ? 1500.0 : 1900.0), 0.35);
                for (size_t i = 0; i < std::min<size_t> (out.size(), (size_t) (0.002 * sr)); ++i)
                    out[i] += (float) (0.3 * noise (rng));   // the stick's knock
                break;

            case Sound::hihat:
            {
                const double seconds = accent ? 0.09 : (sub ? 0.03 : 0.05);
                const size_t n = (size_t) (seconds * sr);
                out.resize (n);
                double x1 = 0.0, x2 = 0.0;
                const double decay = std::pow (0.001, 1.0 / (double) n);
                double env = 1.0;
                for (size_t i = 0; i < n; ++i)
                {
                    const double x = noise (rng);
                    const double hp = x - 2.0 * x1 + x2;   // steep high-pass: all shimmer, no thump
                    x2 = x1; x1 = x;
                    out[i] = (float) (0.45 * hp * env);
                    env *= decay;
                }
                break;
            }

            case Sound::cowbell:
            {
                const double seconds = sub ? 0.06 : 0.14;
                const size_t n = (size_t) (seconds * sr);
                out.resize (n);
                const double f1 = accent ? 587.0 : (sub ? 440.0 : 540.0);
                const double f2 = f1 * 1.48;
                const double decay = std::pow (0.001, 1.0 / (double) n);
                double env = 1.0;
                for (size_t i = 0; i < n; ++i)
                {
                    const double t = (double) i / sr;
                    // two slightly clipped tones: the metal ring of a bell
                    double v = std::sin (kTwoPi * f1 * t) + std::sin (kTwoPi * f2 * t);
                    v = std::tanh (1.8 * v) * 0.55;
                    out[i] = (float) (v * env);
                    env *= decay;
                }
                break;
            }

            case Sound::rim:
            {
                out = tone (0.02, accent ? 1900.0 : (sub ? 1300.0 : 1600.0), 0.001);
                for (size_t i = 0; i < out.size(); ++i)
                    out[i] = (float) (0.7 * out[i] + 0.4 * noise (rng) * std::exp (-(double) i / sr * 300.0));
                break;
            }

            case Sound::clave:
                out = tone (0.03, accent ? 2900.0 : (sub ? 2000.0 : 2500.0), 0.0008);
                break;

            case Sound::beep:
            {
                const double seconds = sub ? 0.03 : 0.06;
                const size_t n = (size_t) (seconds * sr);
                out.resize (n);
                const double hz = accent ? 1500.0 : (sub ? 750.0 : 1000.0);
                const size_t ramp = std::max<size_t> (1, (size_t) (0.003 * sr));
                for (size_t i = 0; i < n; ++i)
                {
                    const double t = (double) i / sr;
                    double v = std::sin (kTwoPi * hz * t) + std::sin (kTwoPi * 3.0 * hz * t) / 3.0;   // a soft square
                    const double edge = std::min ({ 1.0, (double) i / ramp, (double) (n - 1 - i) / ramp });
                    out[i] = (float) (0.7 * v * edge);
                }
                break;
            }

            default: out.assign (1, 0.0f); break;
        }

        // same loudness for every sound: peak at 0.8
        float peak = 0.0f;
        for (auto v : out) peak = std::max (peak, std::fabs (v));
        if (peak > 0.0f) for (auto& v : out) v *= 0.8f / peak;
        return out;
    }

    double sr { 44100.0 };
    std::array<std::array<std::vector<float>, kNumHits>, kNumSounds> hits;
};

//==============================================================================
/** One click voice following a position. Several can share a SoundBank and a
    ClickParams (the loop metronome and the song click do). */
class Clicker
{
public:
    Clicker (const SoundBank& b, const ClickParams& p) : bank (b), params (p) {}

    /** Forget which tick was last struck, so the next render strikes whatever
        tick `pos` lands in (call on transport start and on jumps). */
    void reset() { lastTick = INT64_MIN; playing = nullptr; }

    /** Renders `n` samples of click, OVERWRITING outL/outR. posAtStart is the
        first sample's position, in the same units as samplesPerBeat. */
    void render (float* outL, float* outR, int n, double posAtStart, double samplesPerBeat, int beatsPerBar, double gain = 1.0)
    {
        for (int i = 0; i < n; ++i) { outL[i] = 0.0f; outR[i] = 0.0f; }
        if (samplesPerBeat <= 0.0 || beatsPerBar <= 0) return;

        // the settings, once per block
        const auto sound    = (Sound) std::clamp (params.sound.load (std::memory_order_relaxed), 0, kNumSounds - 1);
        const float level   = std::clamp (params.level.load (std::memory_order_relaxed), 0.0f, 1.5f) * (float) gain;
        const bool accentOn = params.accent.load (std::memory_order_relaxed);
        const float lvAcc   = std::clamp (params.accentLevel.load (std::memory_order_relaxed), 0.0f, 1.0f);
        const float lvBeat  = std::clamp (params.beatLevel.load (std::memory_order_relaxed), 0.0f, 1.0f);
        const float lvSub   = std::clamp (params.subLevel.load (std::memory_order_relaxed), 0.0f, 1.0f);
        const int   sub     = std::clamp (params.subdivision.load (std::memory_order_relaxed), 1, 4);
        const double tickLen = samplesPerBeat / (double) sub;

        for (int i = 0; i < n; ++i)
        {
            const double pos = posAtStart + (double) i;
            const int64_t tick = (int64_t) std::floor (pos / tickLen);
            if (tick != lastTick)
            {
                lastTick = tick;
                const int64_t beat   = floorDiv (tick, sub);
                const int64_t inBeat = tick - beat * sub;
                const int64_t inBar  = ((beat % beatsPerBar) + beatsPerBar) % beatsPerBar;
                const Hit hit = inBeat != 0 ? Hit::sub : (accentOn && inBar == 0 ? Hit::accent : Hit::beat);
                const float lv = hit == Hit::accent ? lvAcc : (hit == Hit::beat ? lvBeat : lvSub);
                if (lv > 0.0f)
                {
                    playing = &bank.get (sound, hit);
                    index = 0;
                    voiceGain = lv;
                }
                else
                {
                    playing = nullptr;
                }
            }
            if (playing != nullptr)
            {
                const float s = (*playing)[index] * voiceGain * level;
                outL[i] = s;
                outR[i] = s;
                if (++index >= playing->size()) playing = nullptr;
            }
        }
    }

private:
    static int64_t floorDiv (int64_t a, int64_t b) { const int64_t q = a / b; return (a % b != 0 && ((a < 0) != (b < 0))) ? q - 1 : q; }

    const SoundBank& bank;
    const ClickParams& params;
    int64_t lastTick { INT64_MIN };
    const std::vector<float>* playing { nullptr };
    size_t index { 0 };
    float voiceGain { 1.0f };
};

} // namespace ezclick
