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

/** The Perform Live Creators WhatsApp QR code, zoomed in to the code itself.
    Owner: "zoom in the qr code". creators-qr.jpg is WhatsApp's full phone
    screen (green ground, card, caption) with the code a small square in the
    middle; this keeps that square and a thin white quiet zone so it still
    scans. The crop is proportional, so a re-export at another resolution of
    the same screen still lands on the code. */
inline const juce::Image& creatorsQr()
{
    static juce::Image i = []
    {
        auto full = loadImage ("creators-qr.jpg");
        if (! full.isValid()) return full;
        const int w = full.getWidth(), h = full.getHeight();
        const int side = juce::roundToInt (w * 0.49);
        const juce::Rectangle<int> code (juce::roundToInt (w * 0.4985) - side / 2, juce::roundToInt (h * 0.5) - side / 2, side, side);
        if (! full.getBounds().contains (code)) return full;   // not the expected screen: show it whole
        return full.getClippedImage (code).createCopy();
    }();
    return i;
}

/** The Amanorsac Studio lockup, on black -- the only background the Master
    Standard allows it on. STORE page and end-of-beta notice. */
inline const juce::Image& amanorsacLogo() { static juce::Image i = loadImage ("amanorsac-logo.jpg"); return i; }

} // namespace performart
