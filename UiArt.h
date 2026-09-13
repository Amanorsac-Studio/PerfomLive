// ============================================================================
//  UiArt.h -- baked UI art, loaded the same way the fonts are.
//
//  WHAT BELONGS IN A PNG, AND WHAT DOES NOT.
//
//  Only surfaces that never change belong here. A bitmap carries gradients,
//  glass, bevels and inner shadow far more cheaply than redrawing them every
//  frame, and it lets a designer's texture reach the screen exactly as drawn
//  rather than being approximated in paint() calls.
//
//  But a PNG can only ever show ONE state. Anything that moves -- an LED ring
//  lit to a live fraction, a meter, a readout, a pad in the user's own colour
//  -- stays drawn in code, on top of the art. That split is the whole design:
//  the bitmap is the instrument's housing, the code is what lights up inside
//  it.
//
//  Assets are looked up next to the exe first, exactly like ./fonts, so a
//  packaged build finds them without a working-directory assumption. A missing
//  art folder DEGRADES the app rather than breaking it: every caller checks
//  isValid() and falls back to drawing the surface itself.
// ============================================================================
#pragma once

#include <JuceHeader.h>
#include "ProductPaths.h"

namespace performart
{

inline juce::Image loadImage (const char* filename)
{
    const juce::File candidates[] = {
        productpaths::bundledResources().getChildFile ("art").getChildFile (filename),
        juce::File::getCurrentWorkingDirectory().getChildFile ("art").getChildFile (filename),
    };

    for (const auto& c : candidates)
    {
        if (! c.existsAsFile()) continue;
        auto img = juce::ImageFileFormat::loadFrom (c);
        if (img.isValid()) return img;
    }
    return {};
}

// Function-local statics: loaded on first use and cached for the app's life,
// the same convention performfonts uses for typefaces.

/** The PLAYBACK dial's housing: bezel, recessed LED channel, face gradient,
    glass highlight, inner shadow. Deliberately contains NO LED segments, no
    index pointer and no numbers -- those are drawn over it so the ring can
    animate. */
inline const juce::Image& dialFace() { static juce::Image i = loadImage ("dial-face.png"); return i; }

/** The Amanorsac Studio lockup, on black -- the only background the Master
    Standard allows it on. STORE page and end-of-beta notice. */
inline const juce::Image& amanorsacLogo() { static juce::Image i = loadImage ("amanorsac-logo.jpg"); return i; }

/** QR code for the Perform Live Creators WhatsApp group (STORE page). */
inline const juce::Image& creatorsQr() { static juce::Image i = loadImage ("creators-qr.jpg"); return i; }

} // namespace performart
