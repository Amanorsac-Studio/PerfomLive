// ============================================================================
//  CreatorsTab.h -- the STORE page for the public beta, and the window that
//  replaces the app once the beta has ended.
//
//  The online pack store is not in this build: STORE opens on a preview
//  catalogue (StoreShowcase.h) and this page, which tells people who make
//  loops and stems how to become a creator: the studio lockup, the contacts
//  the owner gave for it, and the QR code for the creators' WhatsApp group.
//
//  The links hand off to the operating system's own mail app and browser, and
//  every address they hand over is a compiled-in constant -- never text read
//  from a file or a server.
//
//  Colours are the PerformLive tokens written out, because this header is
//  included before the performlive namespace is declared in Main.cpp. Body
//  and link colours were picked for contrast on the card colour (B24): dim
//  text is about 7:1, link text about 6:1.
// ============================================================================
#pragma once

#include <JuceHeader.h>
#include "Beta.h"
#include "UiArt.h"

namespace creators
{

inline constexpr const char* kEmail        = "amanorsac@gmail.com";
inline constexpr const char* kInstagram    = "@studioamanorsac";
inline constexpr const char* kInstagramUrl = "https://www.instagram.com/studioamanorsac/";
inline constexpr const char* kLegalUrl     = "https://amanorsac.studio/legal";
inline constexpr const char* kPrivacyUrl   = "https://amanorsac.studio/privacy";

namespace tone
{
    inline const juce::Colour ground { 0xff07070fu };
    inline const juce::Colour card   { 0xff151527u };
    inline const juce::Colour border { 0xff2b2b4du };
    inline const juce::Colour link   { 0xffa08bffu };   // focus violet: legible on the card, unlike the darker accent
    inline const juce::Colour text   { 0xfff2f0ffu };
    inline const juce::Colour dim    { 0xffa3a6ccu };
    inline const juce::Colour beta   { 0xffffc933u };
}

//==============================================================================
/** A text link that hands a compiled-in URL to the operating system. Reachable
    by keyboard, with a visible focus ring (B21, B22) and an accessible name
    (B25). */
class LinkButton : public juce::Button
{
public:
    LinkButton (juce::String text, juce::String urlIn, juce::String accessibleName, float height = 15.0f)
        : juce::Button (text), url (std::move (urlIn)), font (juce::FontOptions (height, juce::Font::bold))
    {
        setTitle (accessibleName);
        setTooltip (url.startsWith ("mailto:") ? "Opens your email app" : "Opens in your web browser");
        setWantsKeyboardFocus (true);
        setMouseCursor (juce::MouseCursor::PointingHandCursor);
        onClick = [this] { juce::URL (url).launchInDefaultBrowser(); };
    }

    int idealWidth() const
    {
        return juce::GlyphArrangement::getStringWidthInt (font, getButtonText()) + 12;
    }

    void paintButton (juce::Graphics& g, bool over, bool down) override
    {
        const auto r = getLocalBounds().toFloat();
        if (hasKeyboardFocus (true))
        {
            g.setColour (tone::link);
            g.drawRoundedRectangle (r.reduced (1.0f), 4.0f, 2.0f);
        }

        g.setColour (down ? tone::text : tone::link);
        g.setFont (font);
        const auto textArea = getLocalBounds().reduced (6, 0);
        g.drawText (getButtonText(), textArea, juce::Justification::centredLeft, false);

        if (over || down)
        {
            const float w = (float) juce::GlyphArrangement::getStringWidthInt (font, getButtonText());
            g.fillRect ((float) textArea.getX(), r.getCentreY() + font.getHeight() * 0.5f + 2.0f, w, 1.0f);
        }
    }

private:
    juce::String url;
    juce::Font   font;
};

//==============================================================================
class CreatorsTab : public juce::Component
{
public:
    explicit CreatorsTab (juce::String versionIn)
        : version (std::move (versionIn)), endsAt (performbeta::expiry())
    {
        setTitle ("Store");
        for (auto* link : { &emailLink, &instagramLink, &legalLink, &privacyLink })
            addAndMakeVisible (link);
    }

    void paint (juce::Graphics& g) override
    {
        g.fillAll (tone::ground);

        // ---- header band, matching the other full views -----------------------
        auto header = getLocalBounds().removeFromTop (kHeaderHeight);
        g.setColour (tone::card);
        g.fillRect (header);
        g.setColour (tone::border);
        g.fillRect (header.removeFromBottom (1));

        auto h = getLocalBounds().removeFromTop (kHeaderHeight).reduced (24, 0);
        g.setColour (tone::text);
        g.setFont (juce::FontOptions (22.0f, juce::Font::bold));
        g.drawText ("STORE", h.removeFromLeft (120), juce::Justification::centredLeft, false);

        const juce::String chip (performbeta::kLabel);
        const juce::Font chipFont (juce::FontOptions (12.0f, juce::Font::bold));
        const int chipW = juce::GlyphArrangement::getStringWidthInt (chipFont, chip) + 28;
        const auto chipR = h.removeFromRight (chipW).withSizeKeepingCentre (chipW, 28).toFloat();
        g.setColour (tone::beta.withAlpha (0.14f));
        g.fillRoundedRectangle (chipR, 14.0f);
        g.setColour (tone::beta);
        g.drawRoundedRectangle (chipR.reduced (0.5f), 14.0f, 1.0f);
        g.setFont (chipFont);
        g.drawText (chip, chipR, juce::Justification::centred, false);

        // ---- cards ------------------------------------------------------------
        for (const auto& card : { pitchCard, qrCard })
        {
            g.setColour (tone::card);
            g.fillRoundedRectangle (card.toFloat(), 10.0f);
            g.setColour (tone::border);
            g.drawRoundedRectangle (card.toFloat().reduced (0.5f), 10.0f, 1.0f);
        }

        // ---- the pitch --------------------------------------------------------
        auto p = pitchCard.reduced (kPad, kPad - 4);

        // The lockup sits on black: the only background the Master Standard
        // allows it on.
        auto logoBox = p.removeFromTop (kLogoHeight);
        g.setColour (juce::Colours::black);
        g.fillRoundedRectangle (logoBox.toFloat(), 8.0f);
        if (const auto& logo = performart::amanorsacLogo(); logo.isValid())
            g.drawImage (logo, logoBox.reduced (8).toFloat(),
                         juce::RectanglePlacement (juce::RectanglePlacement::centred));

        p.removeFromTop (kGapAfterLogo);
        g.setColour (tone::text);
        g.setFont (juce::FontOptions (24.0f, juce::Font::bold));
        g.drawFittedText ("Sell your loops and stems in PerformLive",
                          p.removeFromTop (kHeadingHeight), juce::Justification::centredLeft, 1);

        p.removeFromTop (kGapAfterHeading);
        g.setColour (tone::dim);
        g.setFont (juce::FontOptions (15.0f));
        g.drawFittedText ("If you make loops, stems or pads, you can become a PerformLive creator "
                          "and sell them to the people who use the app. Get in touch by email or "
                          "Instagram, or scan the code to join the Perform Live Creators group on WhatsApp.",
                          p.removeFromTop (kBodyHeight), juce::Justification::topLeft, 4, 1.0f);

        g.setColour (tone::dim);
        g.setFont (juce::FontOptions (12.0f, juce::Font::bold));
        g.drawText ("EMAIL",     emailRow.withWidth (kContactLabelWidth),     juce::Justification::centredLeft, false);
        g.drawText ("INSTAGRAM", instagramRow.withWidth (kContactLabelWidth), juce::Justification::centredLeft, false);

        // ---- the QR code ------------------------------------------------------
        auto q = qrCard.reduced (24);
        const auto caption = q.removeFromBottom (48);
        if (const auto& qr = performart::creatorsQr(); qr.isValid())
            g.drawImage (qr, q.toFloat(), juce::RectanglePlacement (juce::RectanglePlacement::centred));
        g.setColour (tone::dim);
        g.setFont (juce::FontOptions (14.0f));
        g.drawFittedText ("Scan with WhatsApp to join the Perform Live Creators group",
                          caption, juce::Justification::centred, 2);

        // ---- footer: what this build is, and when it stops --------------------
        auto f = footer;
        g.setColour (tone::border);
        g.fillRect (f.removeFromTop (1));
        g.setColour (tone::dim);
        g.setFont (juce::FontOptions (13.0f));
        const juce::String dot (juce::CharPointer_UTF8 ("  \xc2\xb7  "));
        g.drawText (juce::String (performbeta::kLabel) + dot + "Version " + version + dot
                        + "You can use this beta until " + performbeta::dateText (performbeta::lastDay (endsAt)) + ".",
                    f.reduced (24, 0).withTrimmedRight (220), juce::Justification::centredLeft, true);
    }

    void resized() override
    {
        auto area = getLocalBounds();
        area.removeFromTop (kHeaderHeight);

        footer = area.removeFromBottom (52);
        auto f = footer.reduced (24, 0);
        privacyLink.setBounds (f.removeFromRight (privacyLink.idealWidth()).withSizeKeepingCentre (privacyLink.idealWidth(), 32));
        f.removeFromRight (12);
        legalLink.setBounds (f.removeFromRight (legalLink.idealWidth()).withSizeKeepingCentre (legalLink.idealWidth(), 32));

        auto body = area.reduced (24, 20);
        if (body.getWidth() > kMaxBodyWidth)
            body = body.withSizeKeepingCentre (kMaxBodyWidth, body.getHeight());

        qrCard = body.removeFromRight (juce::jlimit (260, 380, body.getWidth() * 34 / 100));
        body.removeFromRight (20);
        pitchCard = body;

        // The contact rows follow the text block drawn in paint(), so they use
        // the same measurements.
        auto p = pitchCard.reduced (kPad, kPad - 4);
        p.removeFromTop (kLogoHeight + kGapAfterLogo + kHeadingHeight + kGapAfterHeading + kBodyHeight + 12);
        emailRow = p.removeFromTop (36);
        p.removeFromTop (4);
        instagramRow = p.removeFromTop (36);

        emailLink.setBounds (emailRow.withTrimmedLeft (kContactLabelWidth).withWidth (emailLink.idealWidth()));
        instagramLink.setBounds (instagramRow.withTrimmedLeft (kContactLabelWidth).withWidth (instagramLink.idealWidth()));
    }

private:
    static constexpr int kHeaderHeight      = 84;
    static constexpr int kMaxBodyWidth      = 1080;
    static constexpr int kPad               = 32;
    static constexpr int kLogoHeight        = 150;
    static constexpr int kGapAfterLogo      = 22;
    static constexpr int kHeadingHeight     = 34;
    static constexpr int kGapAfterHeading   = 8;
    static constexpr int kBodyHeight        = 72;
    static constexpr int kContactLabelWidth = 110;

    juce::String version;
    juce::Time   endsAt;

    LinkButton emailLink     { kEmail,     juce::String ("mailto:") + kEmail, "Email amanorsac@gmail.com" };
    LinkButton instagramLink { kInstagram, kInstagramUrl,                     "Instagram, studioamanorsac" };
    LinkButton legalLink     { "Licence",  kLegalUrl,                         "Licence and terms", 13.0f };
    LinkButton privacyLink   { "Privacy",  kPrivacyUrl,                       "Privacy policy",    13.0f };

    juce::Rectangle<int> pitchCard, qrCard, footer, emailRow, instagramRow;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (CreatorsTab)
};

//==============================================================================
/** What opens instead of the app once the beta has ended. Says what happened
    and what to do, keeps nothing half-running behind it, and quits cleanly. */
class BetaEndedContent : public juce::Component
{
public:
    explicit BetaEndedContent (juce::Time endsAtIn) : endsAt (endsAtIn)
    {
        quitButton.setButtonText ("Quit");
        quitButton.setTitle ("Quit PerformLive");
        quitButton.onClick = []
        {
            if (auto* app = juce::JUCEApplicationBase::getInstance())
                app->systemRequestedQuit();
        };
        addAndMakeVisible (quitButton);
        addAndMakeVisible (emailLink);
        setSize (600, 430);
    }

    void paint (juce::Graphics& g) override
    {
        g.fillAll (tone::ground);
        auto r = getLocalBounds().reduced (32, 28);

        auto logoBox = r.removeFromTop (120);
        g.setColour (juce::Colours::black);
        g.fillRoundedRectangle (logoBox.toFloat(), 8.0f);
        if (const auto& logo = performart::amanorsacLogo(); logo.isValid())
            g.drawImage (logo, logoBox.reduced (6).toFloat(),
                         juce::RectanglePlacement (juce::RectanglePlacement::centred));

        r.removeFromTop (22);
        g.setColour (tone::text);
        g.setFont (juce::FontOptions (22.0f, juce::Font::bold));
        g.drawText ("This beta has ended", r.removeFromTop (32), juce::Justification::centredLeft, false);

        r.removeFromTop (8);
        g.setColour (tone::dim);
        g.setFont (juce::FontOptions (15.0f));
        g.drawFittedText (juce::String (performbeta::kLabel) + " stopped working on " + performbeta::dateText (endsAt)
                              + ". Your projects and library are still on this computer. "
                                "Email us for the next version.",
                          r.removeFromTop (72), juce::Justification::topLeft, 4, 1.0f);
    }

    void resized() override
    {
        auto bottom = getLocalBounds().reduced (32, 28).removeFromBottom (40);
        quitButton.setBounds (bottom.removeFromRight (110));
        emailLink.setBounds (bottom.removeFromLeft (emailLink.idealWidth()));
    }

private:
    juce::Time       endsAt;
    juce::TextButton quitButton;
    LinkButton       emailLink { kEmail, juce::String ("mailto:") + kEmail, "Email amanorsac@gmail.com" };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (BetaEndedContent)
};

class BetaEndedWindow : public juce::DocumentWindow
{
public:
    explicit BetaEndedWindow (juce::Time endsAt)
        : juce::DocumentWindow (performbeta::kLabel, tone::ground, juce::DocumentWindow::closeButton)
    {
        setUsingNativeTitleBar (true);
        setContentOwned (new BetaEndedContent (endsAt), true);
       #if JUCE_IOS || JUCE_ANDROID
        setFullScreen (true);
       #else
        setResizable (false, false);
        centreWithSize (getWidth(), getHeight());
       #endif
        setVisible (true);
    }

    void closeButtonPressed() override
    {
        if (auto* app = juce::JUCEApplicationBase::getInstance())
            app->systemRequestedQuit();
    }

private:
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (BetaEndedWindow)
};

} // namespace creators
