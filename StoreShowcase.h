// ============================================================================
//  StoreShowcase.h -- the STORE page as it will look, before it opens.
//
//  Owner: "build me a store that looks like this (a MultiTracks-style
//  catalogue) ... all buttons inactive, say coming soon when clicked." Then:
//  "for the artists and songs I want real Ghanaian, Nigerian, South African
//  and other African gospel, and a few foreign ones -- real names and song
//  titles. Only the creators can be imagined names." And: "have the QR code
//  and the sign-up to become a creator up there, prominent."
//
//  So the page opens on the creator sign-up (the real QR code, email and
//  Instagram from CreatorsTab.h -- those links work). Below it, a preview
//  catalogue: real songs, albums and artists (each pairing checked), and
//  invented creators with a badge. Catalogue controls report "coming soon"
//  through onComingSoon; nothing is sold, streamed or licensed here. All
//  artwork is drawn, so no album art or photos are used.
// ============================================================================
#pragma once

#include <JuceHeader.h>
#include "CreatorsTab.h"   // creators::kEmail, kInstagram, kInstagramUrl -- one source for the contacts
#include "UiArt.h"         // the studio logo and the creators' WhatsApp QR code

namespace ezstore
{

namespace tokens
{
    const juce::Colour ground   { 0xff07070f };
    const juce::Colour card     { 0xff151527 };
    const juce::Colour cardHi   { 0xff1e1e36 };
    const juce::Colour border   { 0xff2b2b4d };
    const juce::Colour bright   { 0xfff2f0ff };
    const juce::Colour dim      { 0xffa3a6cc };
    const juce::Colour faint    { 0xff6f7099 };
    const juce::Colour indigo   { 0xff7c5cff };
    const juce::Colour gold     { 0xffffc933 };
}

inline juce::String u8 (const char* s) { return juce::String (juce::CharPointer_UTF8 (s)); }

// ---- the preview catalogue -------------------------------------------------------
// Real songs, albums and artists. Every song-artist and album-artist pairing
// was checked against the streaming services before it went in.
struct Song    { const char* title; const char* artist; };
struct Album   { const char* title; const char* artist; };
struct Artist  { const char* name; const char* country; };
struct Creator { const char* name; const char* craft; const char* country; int products; };   // invented

inline const std::vector<Song>& topSongs()
{
    static const std::vector<Song> s = {
        { "Imela",                  "Nathaniel Bassey ft. Enitan Adaba" },
        { "Adom",                   "Diana Hamilton" },
        { "Lion of Judah (Live)",   "Lebo Sekgobela" },
        { "Way Maker",              "Sinach" },
        { "Agbebolo",               "Celestine Donkor ft. Nhyiraba Gideon" },
        { "Uyalalelwa",             "Joyous Celebration" },
        { "Excess Love",            "Mercy Chinwo" },
        { "Bo Noo Ni",              "Joe Mettle ft. Luigi Maclean" },
        { "Mwema",                  "Mercy Masika" },
        { "Too Faithful",           "Moses Bliss" },
        { "Waye Me Yie",            "Piesie Esther" },
        { "Nipe Uvumilivu",         "Rose Muhando" },
        { "Fragrance to Fire",      "Dunsin Oyekan" },
        { "Jireh",                  "Elevation Worship & Maverick City Music" },
        { "Goodness of God",        "CeCe Winans" },
    };
    return s;
}

inline const std::vector<Album>& africanAlbums()
{
    static const std::vector<Album> a = {
        { "The Son of God (& Imela)",   "Nathaniel Bassey" },
        { "Waymaker \xe2\x80\x93 Live", "Sinach" },
        { "Grace",                      "Diana Hamilton" },
        { "God of Miracles",            "Joe Mettle" },
        { "Restored",                   "Lebo Sekgobela" },
        { "Too Faithful",               "Moses Bliss" },
        { "The Gospel of the Kingdom",  "Dunsin Oyekan" },
        { "The Cross: My Gaze",         "Mercy Chinwo" },
    };
    return a;
}

inline const std::vector<Album>& worldAlbums()
{
    static const std::vector<Album> a = {
        { "Old Church Basement", "Elevation Worship & Maverick City Music" },
        { "House of Miracles",   "Brandon Lake" },
        { "Believe For It",      "CeCe Winans" },
    };
    return a;
}

inline const std::vector<Artist>& topArtists()
{
    static const std::vector<Artist> a = {
        { "Nathaniel Bassey",   "Nigeria" },       { "Diana Hamilton",     "Ghana" },
        { "Lebo Sekgobela",     "South Africa" },  { "Sinach",             "Nigeria" },
        { "Joe Mettle",         "Ghana" },         { "Joyous Celebration", "South Africa" },
        { "Mercy Chinwo",       "Nigeria" },       { "Celestine Donkor",   "Ghana" },
        { "Mercy Masika",       "Kenya" },         { "Rose Muhando",       "Tanzania" },
        { "Benjamin Dube",      "South Africa" },  { "Moses Bliss",        "Nigeria" },
        { "CeCe Winans",        "United States" }, { "Brandon Lake",       "United States" },
    };
    return a;
}

inline const std::vector<Creator>& creators()
{
    static const std::vector<Creator> c = {
        { "Amara Eze",       "Afrobeat drum loops",    "Nigeria",       52 },
        { "Kofi Mensah",     "Keys & pad patches",     "Ghana",         38 },
        { "Thabo Nkosi",     "Choir stems",            "South Africa",  17 },
        { "Wanjiru Kamau",   "Click & guide packs",    "Kenya",         24 },
        { "Ifeoma Nwosu",    "Ambient worship pads",   "Nigeria",       19 },
        { "Lucas Ferreira",  "Transitions & swells",   "Brazil",        12 },
        { "Emily Carter",    "Multitrack templates",   "Canada",         9 },
    };
    return c;
}

inline const juce::StringArray& themes()
{
    static const juce::StringArray t { "Praise", "Worship", "Communion", "Healing", "Afro Gospel", "Highlife Praise", "Amapiano Worship", "Christmas" };
    return t;
}

// ---- drawn artwork ----------------------------------------------------------------
inline int seedOf (const juce::String& s) { return (int) (s.hashCode() & 0x7fffffff); }

inline juce::Colour seedColour (int seed, float saturation, float brightness)
{
    return juce::Colour::fromHSV ((float) (seed % 360) / 360.0f, saturation, brightness, 1.0f);
}

/** An uppercase that also raises accented letters (JUCE's leaves "ç" as it is). */
inline juce::String upper (const juce::String& s)
{
    juce::String out;
    for (auto p = s.getCharPointer(); ! p.isEmpty(); ++p)
    {
        juce::juce_wchar c = *p;
        if (c >= 0xe0 && c <= 0xfe && c != 0xf7) c -= 0x20;   // Latin-1 lower -> upper
        else c = juce::CharacterFunctions::toUpperCase (c);
        out += juce::String::charToString (c);
    }
    return out;
}

/** A cover in the catalogue's style: a dark, lit ground and the title set
    big, condensed and white. Drawn -- no real album art is used. */
inline void drawCover (juce::Graphics& g, juce::Rectangle<float> r, const juce::String& title, const juce::String& label)
{
    const int seed = seedOf (title);
    juce::Graphics::ScopedSaveState saved (g);
    juce::Path clip;
    clip.addRoundedRectangle (r, 6.0f);
    g.reduceClipRegion (clip);

    const auto base = seedColour (seed, 0.35f, 0.30f);
    g.setGradientFill (juce::ColourGradient (base.brighter (0.25f), r.getX(), r.getY(), juce::Colour (0xff050508), r.getRight(), r.getBottom(), false));
    g.fillRect (r);
    juce::Random rng (seed);
    for (int i = 0; i < 3; ++i)
    {
        const float d = r.getWidth() * (0.5f + rng.nextFloat() * 0.7f);
        const float cx = r.getX() + rng.nextFloat() * r.getWidth(), cy = r.getY() + rng.nextFloat() * r.getHeight();
        g.setGradientFill (juce::ColourGradient (base.brighter (0.9f).withAlpha (0.18f), cx, cy, base.withAlpha (0.0f), cx + d * 0.5f, cy, true));
        g.fillEllipse (cx - d * 0.5f, cy - d * 0.5f, d, d);
    }
    g.setColour (juce::Colours::black.withAlpha (0.25f));
    g.fillRect (r);

    auto textArea = r.reduced (r.getWidth() * 0.08f);
    const float h = r.getHeight() * 0.17f;
    g.setColour (juce::Colours::white);
    g.setFont (juce::Font (juce::FontOptions (h, juce::Font::bold)).withHorizontalScale (0.78f));
    g.drawFittedText (upper (title), textArea.toNearestInt(), juce::Justification::centredLeft, 3, 0.7f);

    g.setColour (juce::Colours::white.withAlpha (0.55f));
    g.setFont (juce::Font (juce::FontOptions (juce::jmax (7.0f, r.getHeight() * 0.045f), juce::Font::bold)).withExtraKerningFactor (0.12f));
    g.drawText (upper (label), textArea.removeFromBottom (r.getHeight() * 0.08f).toNearestInt(), juce::Justification::bottomLeft, true);
    g.setColour (juce::Colours::white.withAlpha (0.8f));
    const float dot = r.getWidth() * 0.06f;
    g.drawEllipse (r.getRight() - dot * 2.0f, r.getY() + dot, dot, dot, 1.2f);
}

/** A small square thumbnail for a song row. */
inline void drawThumb (juce::Graphics& g, juce::Rectangle<float> r, const juce::String& title)
{
    const int seed = seedOf (title);
    juce::Graphics::ScopedSaveState saved (g);
    juce::Path clip;
    clip.addRoundedRectangle (r, 5.0f);
    g.reduceClipRegion (clip);
    const auto c = seedColour (seed, 0.65f, 0.75f);
    g.setGradientFill (juce::ColourGradient (c, r.getX(), r.getY(), c.darker (1.6f), r.getRight(), r.getBottom(), false));
    g.fillRect (r);
    juce::Random rng (seed + 7);
    g.setColour (juce::Colours::white.withAlpha (0.22f));
    for (int i = 0; i < 4; ++i)
        g.fillRect (r.getX(), r.getY() + rng.nextFloat() * r.getHeight(), r.getWidth(), 1.5f);
    g.setColour (juce::Colours::white.withAlpha (0.85f));
    const float d = r.getWidth() * 0.34f;
    g.fillEllipse (r.getCentreX() - d * 0.5f, r.getCentreY() - d * 0.5f, d, d);
    g.setColour (c.darker (0.8f));
    g.fillEllipse (r.getCentreX() - d * 0.12f, r.getCentreY() - d * 0.12f, d * 0.24f, d * 0.24f);
}

/** A round portrait placeholder: a colour, the initials, a ring. No photos. */
inline void drawAvatar (juce::Graphics& g, juce::Rectangle<float> r, const juce::String& name, bool hovered)
{
    const int seed = seedOf (name);
    const auto c = seedColour (seed, 0.55f, 0.62f);
    g.setGradientFill (juce::ColourGradient (c.brighter (0.35f), r.getCentreX(), r.getY(), c.darker (1.2f), r.getCentreX(), r.getBottom(), false));
    g.fillEllipse (r);
    {
        juce::Graphics::ScopedSaveState saved (g);
        juce::Path clip;
        clip.addEllipse (r);
        g.reduceClipRegion (clip);
        g.setColour (juce::Colours::black.withAlpha (0.22f));
        const float w = r.getWidth();
        g.fillEllipse (r.getCentreX() - w * 0.18f, r.getY() + w * 0.2f, w * 0.36f, w * 0.36f);
        g.fillEllipse (r.getCentreX() - w * 0.42f, r.getY() + w * 0.62f, w * 0.84f, w * 0.7f);
    }
    juce::String initials;
    for (const auto& word : juce::StringArray::fromTokens (name, " ", ""))
        if (word.isNotEmpty() && juce::CharacterFunctions::isLetter (word[0]) && initials.length() < 2) initials += word.substring (0, 1).toUpperCase();
    g.setColour (juce::Colours::white);
    g.setFont (juce::Font (juce::FontOptions (r.getHeight() * 0.3f, juce::Font::bold)));
    g.drawText (initials, r.toNearestInt(), juce::Justification::centred, false);
    g.setColour (hovered ? tokens::indigo : juce::Colours::white.withAlpha (0.15f));
    g.drawEllipse (r.reduced (1.0f), hovered ? 3.0f : 1.5f);
}

/** The creator badge: an indigo pill with a tick. */
inline void drawCreatorBadge (juce::Graphics& g, juce::Rectangle<float> r)
{
    g.setColour (tokens::indigo);
    g.fillRoundedRectangle (r, r.getHeight() * 0.5f);
    const float s = r.getHeight();
    juce::Path tick;
    tick.startNewSubPath (r.getX() + s * 0.32f, r.getCentreY());
    tick.lineTo (r.getX() + s * 0.48f, r.getCentreY() + s * 0.16f);
    tick.lineTo (r.getX() + s * 0.74f, r.getCentreY() - s * 0.16f);
    g.setColour (juce::Colours::white);
    g.strokePath (tick, juce::PathStrokeType (1.6f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
    g.setFont (juce::Font (juce::FontOptions (s * 0.55f, juce::Font::bold)).withExtraKerningFactor (0.08f));
    g.drawText ("CREATOR", r.withTrimmedLeft (s * 0.9f).toNearestInt(), juce::Justification::centredLeft, false);
}

//==============================================================================
class StoreShowcase : public juce::Component
{
public:
    std::function<void (const juce::String&)> onComingSoon;   // what was clicked
    std::function<void()> onOpenCreators;                     // the full creators page

    StoreShowcase()
    {
        page.owner = this;
        viewport.setViewedComponent (&page, false);
        viewport.setScrollBarsShown (true, false);
        viewport.setScrollBarThickness (10);
        addAndMakeVisible (viewport);
    }

    void paint (juce::Graphics& g) override { g.fillAll (tokens::ground); }

    void resized() override
    {
        viewport.setBounds (getLocalBounds());
        page.layoutFor (viewport.getMaximumVisibleWidth());
    }

private:
    enum class Kind { search, signUp, email, instagram, details, pill, songRow, songAdd, cover, artist, creator, follow, theme, link };

    struct Hot
    {
        juce::Rectangle<int> area;
        Kind kind;
        juce::String label;     // what "coming soon" names
        int index { -1 };       // item index; for covers, which row (0 African, 1 world) * 100 + item
    };

    class Page : public juce::Component
    {
    public:
        StoreShowcase* owner { nullptr };

        void layoutFor (int width)
        {
            hots.clear();
            sectionTitles.clear();
            const int margin = juce::jmax (20, width / 30);
            const int w = juce::jmax (360, width) - margin * 2;
            int y = 20;

            // ---- become a creator: the QR code and the sign-up, the very top of the page ----
            // (owner: "the become a creator and the QR code should be prominent at the top")
            {
                const bool wide = w >= 900;
                heroCard = { margin, y, w, wide ? 320 : 400 };
                auto inner = heroCard.reduced (32, 26);
                auto qrColumn = inner.removeFromRight (wide ? 236 : 190);
                heroQr = qrColumn.withHeight (qrColumn.getWidth()).withY (inner.getY());
                heroQrCaption = qrColumn.withTrimmedTop (qrColumn.getWidth() + 8);
                inner.removeFromRight (28);
                heroLogo = w >= 1100 ? inner.removeFromLeft (230) : juce::Rectangle<int>();
                if (! heroLogo.isEmpty()) { heroLogo = heroLogo.withSizeKeepingCentre (230, juce::jmin (150, inner.getHeight())); inner.removeFromLeft (28); }
                heroText = inner;

                // the buttons along the bottom of the text column, wrapping if narrow
                juce::Font bf (juce::FontOptions (13.5f, juce::Font::bold));
                const juce::String emailText = u8 ("Email  \xc2\xb7  ") + creators::kEmail;
                const juce::String instaText = u8 ("Instagram  \xc2\xb7  ") + creators::kInstagram;
                struct B { Kind kind; juce::String text; int width; };
                const B buttons[] = {
                    { Kind::signUp,    "Sign up as a creator", juce::GlyphArrangement::getStringWidthInt (bf, "Sign up as a creator") + 44 },
                    { Kind::email,     emailText,              juce::GlyphArrangement::getStringWidthInt (bf, emailText) + 36 },
                    { Kind::instagram, instaText,              juce::GlyphArrangement::getStringWidthInt (bf, instaText) + 36 },
                    { Kind::details,   u8 ("Creator details \xe2\x80\xba"), juce::GlyphArrangement::getStringWidthInt (bf, u8 ("Creator details \xe2\x80\xba")) + 16 },
                };
                int bx = heroText.getX();
                int by = heroText.getBottom() - 40;
                // a second row when they don't fit on one
                int total = 0;
                for (const auto& b : buttons) total += b.width + 10;
                if (total > heroText.getWidth()) by -= 50;
                for (const auto& b : buttons)
                {
                    if (bx + b.width > heroText.getRight()) { bx = heroText.getX(); by += 50; }
                    hots.push_back ({ { bx, by, b.width, 40 }, b.kind, b.text, 0 });
                    bx += b.width + 10;
                }
                y += heroCard.getHeight() + 28;
            }

            // ---- then the catalogue header: title and search ------------------------------
            header = { margin, y, w, 64 };
            {
                const int sw = juce::jmin (380, w / 3);
                hots.push_back ({ header.withLeft (header.getRight() - sw).withSizeKeepingCentre (sw, 38), Kind::search, "Search", 0 });
            }
            y += 80;

            // ---- filter pills -----------------------------------------------------
            {
                static const char* const pills[] = { "All", "Songs", "Albums", "Artists", "Creators", "Themes", "Genres", "Sync License" };
                int x = margin;
                juce::Font f (juce::FontOptions (13.0f));
                for (int i = 0; i < (int) juce::numElementsInArray (pills); ++i)
                {
                    const int pw = juce::GlyphArrangement::getStringWidthInt (f, pills[i]) + 34;
                    if (x + pw > margin + w) { x = margin; y += 40; }
                    hots.push_back ({ { x, y, pw, 32 }, Kind::pill, pills[i], i });
                    x += pw + 8;
                }
                y += 32 + 30;
            }

            // ---- Top Songs: columns of rows ----------------------------------------------
            addSection ("Top Songs", {}, margin, w, y);
            {
                const int cols = w >= 900 ? 3 : (w >= 600 ? 2 : 1);
                const int gap = 24;
                const int colW = (w - gap * (cols - 1)) / cols;
                const int rowH = 72;
                const int count = (int) topSongs().size();
                const int rows = (count + cols - 1) / cols;
                for (int i = 0; i < count; ++i)
                {
                    // fill down each column, like the catalogue it's modelled on
                    const int c = i % cols, r = i / cols;
                    juce::Rectangle<int> row (margin + c * (colW + gap), y + r * rowH, colW, rowH);
                    hots.push_back ({ row.withTrimmedRight (48), Kind::songRow, u8 (topSongs()[(size_t) i].title), i });
                    hots.push_back ({ row.removeFromRight (44).withSizeKeepingCentre (36, 36), Kind::songAdd, "Add \"" + u8 (topSongs()[(size_t) i].title) + "\"", i });
                }
                y += rows * rowH + 36;
            }

            // ---- rows of covers / portraits (as many slots as fit; fewer items leave space) --
            auto cardRow = [&] (int count, int minW, int extraH, Kind kind, int rowTag, std::function<juce::String (int)> label)
            {
                const int gap = 20;
                const int slots = juce::jmax (2, (w + gap) / (minW + gap));
                const int n = juce::jmin (count, slots);
                const int cw = (w - gap * (slots - 1)) / slots;
                for (int i = 0; i < n; ++i)
                    hots.push_back ({ { margin + i * (cw + gap), y, cw, cw + extraH }, kind, label (i), rowTag * 100 + i });
                y += cw + extraH + 36;
            };

            addSection ("Top African Gospel Albums", {}, margin, w, y);
            cardRow ((int) africanAlbums().size(), 150, 50, Kind::cover, 0, [] (int i) { return u8 (africanAlbums()[(size_t) i].title); });

            addSection ("Top Artists", {}, margin, w, y);
            cardRow ((int) topArtists().size(), 140, 56, Kind::artist, 0, [] (int i) { return u8 (topArtists()[(size_t) i].name); });

            // ---- creators (invented): cards with the badge ------------------------------------
            addSection ("Featured Creators", "SELL ON PERFORMLIVE", margin, w, y);
            {
                const int gap = 20;
                const int n = juce::jlimit (2, (int) creators().size(), (w + gap) / (180 + gap));
                const int cw = (w - gap * (n - 1)) / n;
                const int ch = 250;
                for (int i = 0; i < n; ++i)
                {
                    juce::Rectangle<int> card (margin + i * (cw + gap), y, cw, ch);
                    hots.push_back ({ card, Kind::creator, u8 (creators()[(size_t) i].name), i });
                    hots.push_back ({ juce::Rectangle<int> (card.getX() + 18, card.getBottom() - 50, cw - 36, 34), Kind::follow, "Follow " + u8 (creators()[(size_t) i].name), i });
                }
                y += ch + 36;
            }

            addSection ("From Around the World", {}, margin, w, y);
            cardRow ((int) worldAlbums().size(), 150, 50, Kind::cover, 1, [] (int i) { return u8 (worldAlbums()[(size_t) i].title); });

            // ---- themes -----------------------------------------------------------------------
            addSection ("Browse by Theme", {}, margin, w, y);
            {
                const int gap = 16;
                const int n = juce::jlimit (2, 4, (w + gap) / (200 + gap));
                const int tw = (w - gap * (n - 1)) / n;
                const int th = 84;
                for (int i = 0; i < themes().size(); ++i)
                    hots.push_back ({ { margin + (i % n) * (tw + gap), y + (i / n) * (th + gap), tw, th }, Kind::theme, themes()[i], i });
                y += ((themes().size() + n - 1) / n) * (th + gap) + 20;
            }

            footerY = y + 10;
            setSize (width, footerY + 80);
            repaint();
        }

        void paint (juce::Graphics& g) override
        {
            g.fillAll (tokens::ground);

            // ---- header ----
            g.setColour (tokens::bright);
            g.setFont (juce::Font (juce::FontOptions (30.0f, juce::Font::bold)));
            g.drawText ("Store", header.withHeight (38), juce::Justification::centredLeft, false);
            g.setColour (tokens::dim);
            g.setFont (juce::Font (juce::FontOptions (13.5f)));
            g.drawText ("Multitracks, loops and sounds for worship teams - African gospel and the world's worship",
                        header.withTrimmedTop (40).withHeight (22).withTrimmedRight (juce::jmin (400, header.getWidth() / 3) + 20), juce::Justification::centredLeft, true);

            paintHero (g);

            // ---- section headings ----
            for (const auto& s : sectionTitles)
            {
                g.setColour (tokens::bright);
                g.setFont (juce::Font (juce::FontOptions (19.0f, juce::Font::bold)));
                g.drawText (s.title, s.area, juce::Justification::centredLeft, false);
                if (s.badge.isNotEmpty())
                {
                    juce::Font bf (juce::FontOptions (10.0f, juce::Font::bold));
                    const int tw = juce::GlyphArrangement::getStringWidthInt (juce::Font (juce::FontOptions (19.0f, juce::Font::bold)), s.title);
                    const int bw = juce::GlyphArrangement::getStringWidthInt (bf, s.badge) + 20;
                    juce::Rectangle<float> pill ((float) (s.area.getX() + tw + 12), (float) s.area.getCentreY() - 10.0f, (float) bw, 20.0f);
                    g.setColour (tokens::indigo.withAlpha (0.22f));
                    g.fillRoundedRectangle (pill, 10.0f);
                    g.setColour (tokens::indigo.brighter (0.6f));
                    g.setFont (bf.withExtraKerningFactor (0.08f));
                    g.drawText (s.badge, pill.toNearestInt(), juce::Justification::centred, false);
                }
                g.setColour (s.viewAllHot == hovered ? tokens::bright : tokens::faint);
                g.setFont (juce::Font (juce::FontOptions (13.0f)));
                g.drawText ("View all", s.area, juce::Justification::centredRight, false);
            }

            for (size_t h = 0; h < hots.size(); ++h)
                paintHot (g, hots[h], (int) h == hovered);

            g.setColour (tokens::faint);
            g.setFont (juce::Font (juce::FontOptions (12.0f)));
            g.drawFittedText ("A preview of the PerformLive store. Artist and song names show the kind of catalogue planned; nothing here is "
                              "for sale or licensed yet, and no artist is affiliated. Creator names and all artwork are placeholders.",
                              juce::Rectangle<int> (40, footerY, getWidth() - 80, 40), juce::Justification::centred, 2);
        }

        void mouseMove (const juce::MouseEvent& e) override
        {
            const int h = hotAt (e.getPosition());
            if (h != hovered)
            {
                hovered = h;
                setMouseCursor (h >= 0 ? juce::MouseCursor::PointingHandCursor : juce::MouseCursor::NormalCursor);
                repaint();
            }
        }

        void mouseExit (const juce::MouseEvent&) override { if (hovered != -1) { hovered = -1; repaint(); } }

        void mouseUp (const juce::MouseEvent& e) override
        {
            if (e.mouseWasDraggedSinceMouseDown() || owner == nullptr) return;
            const int h = hotAt (e.getPosition());
            if (h < 0) return;
            const auto& hot = hots[(size_t) h];
            switch (hot.kind)
            {
                // the creator sign-up is real: these hand off to the mail app and browser
                case Kind::signUp:    juce::URL (juce::String ("mailto:") + creators::kEmail + "?subject=PerformLive%20creator%20sign-up").launchInDefaultBrowser(); return;
                case Kind::email:     juce::URL (juce::String ("mailto:") + creators::kEmail).launchInDefaultBrowser(); return;
                case Kind::instagram: juce::URL (creators::kInstagramUrl).launchInDefaultBrowser(); return;
                case Kind::details:   if (owner->onOpenCreators) owner->onOpenCreators(); return;
                default:              if (owner->onComingSoon) owner->onComingSoon (hot.label); return;
            }
        }

    private:
        struct SectionTitle { juce::String title, badge; juce::Rectangle<int> area; int viewAllHot { -1 }; };

        void paintHero (juce::Graphics& g)
        {
            const auto card = heroCard.toFloat();
            g.setColour (tokens::card);
            g.fillRoundedRectangle (card, 14.0f);
            g.setGradientFill (juce::ColourGradient (tokens::indigo.withAlpha (0.26f), card.getX(), card.getY(),
                                                     tokens::indigo.withAlpha (0.0f), card.getX() + card.getWidth() * 0.7f, card.getBottom(), false));
            g.fillRoundedRectangle (card, 14.0f);
            g.setColour (tokens::indigo.withAlpha (0.55f));
            g.drawRoundedRectangle (card.reduced (0.5f), 14.0f, 1.2f);

            if (! heroLogo.isEmpty())
            {
                g.setColour (juce::Colours::black);   // the lockup only ever sits on black
                g.fillRoundedRectangle (heroLogo.toFloat(), 10.0f);
                if (const auto& logo = performart::amanorsacLogo(); logo.isValid())
                    g.drawImage (logo, heroLogo.reduced (10).toFloat(), juce::RectanglePlacement (juce::RectanglePlacement::centred));
            }

            auto t = heroText;
            g.setColour (tokens::gold);
            g.setFont (juce::Font (juce::FontOptions (11.5f, juce::Font::bold)).withExtraKerningFactor (0.14f));
            g.drawText ("BECOME A CREATOR", t.removeFromTop (18), juce::Justification::centredLeft, false);
            t.removeFromTop (6);
            g.setColour (tokens::bright);
            g.setFont (juce::Font (juce::FontOptions (34.0f, juce::Font::bold)));
            g.drawFittedText ("Sell your loops, stems and pads in PerformLive", t.removeFromTop (46), juce::Justification::centredLeft, 1, 0.75f);
            t.removeFromTop (8);
            g.setColour (tokens::dim);
            g.setFont (juce::Font (juce::FontOptions (14.5f)));
            g.drawFittedText ("Worship teams across Africa and the world use PerformLive on stage. If you make multitracks, loops, "
                              "pads or patches, sign up and sell them here. Scan the code to join the Perform Live Creators group on WhatsApp.",
                              t.removeFromTop (64), juce::Justification::topLeft, 3, 1.0f);

            // the QR code, on white so any phone reads it
            if (! heroQr.isEmpty())
            {
                g.setColour (juce::Colours::white);
                g.fillRoundedRectangle (heroQr.toFloat(), 10.0f);
                if (const auto& qr = performart::creatorsQr(); qr.isValid())
                    g.drawImage (qr, heroQr.reduced (10).toFloat(), juce::RectanglePlacement (juce::RectanglePlacement::centred));
                g.setColour (tokens::dim);
                g.setFont (juce::Font (juce::FontOptions (12.5f)));
                g.drawFittedText ("Scan with WhatsApp to join Perform Live Creators", heroQrCaption, juce::Justification::centredTop, 2);
            }
        }

        void paintHot (juce::Graphics& g, const Hot& hot, bool isHover)
        {
            const auto a = hot.area.toFloat();
            switch (hot.kind)
            {
                case Kind::search:
                {
                    g.setColour (isHover ? tokens::cardHi : tokens::card);
                    g.fillRoundedRectangle (a, a.getHeight() * 0.5f);
                    g.setColour (tokens::border);
                    g.drawRoundedRectangle (a.reduced (0.5f), a.getHeight() * 0.5f, 1.0f);
                    g.setColour (tokens::faint);
                    const float cx = a.getX() + 22.0f, cy = a.getCentreY() - 1.5f;
                    g.drawEllipse (cx - 5.0f, cy - 5.0f, 9.0f, 9.0f, 1.5f);
                    g.drawLine (cx + 2.5f, cy + 2.5f, cx + 7.0f, cy + 7.0f, 1.6f);
                    g.setFont (juce::Font (juce::FontOptions (13.0f)));
                    g.drawText ("Search songs, artists, creators", hot.area.withTrimmedLeft (40), juce::Justification::centredLeft, true);
                    break;
                }
                case Kind::signUp:
                {
                    g.setColour (isHover ? tokens::indigo.brighter (0.15f) : tokens::indigo);
                    g.fillRoundedRectangle (a, a.getHeight() * 0.5f);
                    g.setColour (juce::Colours::white);
                    g.setFont (juce::Font (juce::FontOptions (13.5f, juce::Font::bold)));
                    g.drawText (hot.label, hot.area, juce::Justification::centred, false);
                    break;
                }
                case Kind::email:
                case Kind::instagram:
                {
                    g.setColour (isHover ? tokens::cardHi : tokens::ground.withAlpha (0.5f));
                    g.fillRoundedRectangle (a, a.getHeight() * 0.5f);
                    g.setColour (isHover ? tokens::bright : tokens::border.brighter (0.4f));
                    g.drawRoundedRectangle (a.reduced (0.5f), a.getHeight() * 0.5f, 1.0f);
                    g.setColour (tokens::bright);
                    g.setFont (juce::Font (juce::FontOptions (13.5f, juce::Font::bold)));
                    g.drawText (hot.label, hot.area, juce::Justification::centred, false);
                    break;
                }
                case Kind::details:
                {
                    g.setColour (isHover ? tokens::bright : juce::Colour (0xffa08bff));
                    g.setFont (juce::Font (juce::FontOptions (13.5f, juce::Font::bold)));
                    g.drawText (hot.label, hot.area, juce::Justification::centred, false);
                    break;
                }
                case Kind::pill:
                {
                    const bool active = hot.index == 0;
                    g.setColour (active ? tokens::bright : (isHover ? tokens::cardHi : tokens::ground));
                    g.fillRoundedRectangle (a, a.getHeight() * 0.5f);
                    if (! active) { g.setColour (tokens::border.brighter (0.2f)); g.drawRoundedRectangle (a.reduced (0.5f), a.getHeight() * 0.5f, 1.0f); }
                    g.setColour (active ? tokens::ground : tokens::dim);
                    g.setFont (juce::Font (juce::FontOptions (13.0f, active ? juce::Font::bold : juce::Font::plain)));
                    g.drawText (hot.label, hot.area, juce::Justification::centred, false);
                    break;
                }
                case Kind::songRow:
                {
                    const auto& s = topSongs()[(size_t) hot.index];
                    if (isHover) { g.setColour (tokens::card); g.fillRoundedRectangle (a.reduced (0.0f, 4.0f).withTrimmedLeft (-8.0f), 8.0f); }
                    auto r = hot.area.reduced (0, 12);
                    drawThumb (g, r.removeFromLeft (r.getHeight()).toFloat(), u8 (s.title));
                    r.removeFromLeft (16);
                    g.setColour (tokens::bright);
                    g.setFont (juce::Font (juce::FontOptions (14.5f, juce::Font::bold)));
                    g.drawText (u8 (s.title), r.removeFromTop (r.getHeight() / 2), juce::Justification::bottomLeft, true);
                    g.setColour (tokens::dim);
                    g.setFont (juce::Font (juce::FontOptions (12.5f)));
                    g.drawText (u8 (s.artist), r, juce::Justification::topLeft, true);
                    g.setColour (tokens::border.withAlpha (0.6f));
                    g.fillRect (hot.area.getX(), hot.area.getBottom() - 1, hot.area.getWidth() + 48, 1);
                    break;
                }
                case Kind::songAdd:
                {
                    g.setColour (isHover ? tokens::bright : tokens::faint);
                    const float c = a.getCentreX(), m = a.getCentreY(), s = 8.0f;
                    g.drawLine (c - s, m, c + s, m, 2.0f);
                    g.drawLine (c, m - s, c, m + s, 2.0f);
                    break;
                }
                case Kind::cover:
                {
                    const bool world = hot.index >= 100;
                    const auto& album = world ? worldAlbums()[(size_t) (hot.index - 100)] : africanAlbums()[(size_t) hot.index];
                    auto coverArea = a.withHeight (a.getWidth());
                    if (isHover) coverArea = coverArea.reduced (-3.0f);
                    drawCover (g, coverArea, u8 (album.title), u8 (album.artist));
                    auto text = hot.area.withTrimmedTop (hot.area.getWidth() + 10);
                    g.setColour (tokens::bright);
                    g.setFont (juce::Font (juce::FontOptions (14.0f, juce::Font::bold)));
                    g.drawText (u8 (album.title), text.removeFromTop (18), juce::Justification::centredLeft, true);
                    g.setColour (tokens::faint);
                    g.setFont (juce::Font (juce::FontOptions (12.5f)));
                    g.drawText (u8 (album.artist), text.removeFromTop (18), juce::Justification::centredLeft, true);
                    break;
                }
                case Kind::artist:
                {
                    const auto& ar = topArtists()[(size_t) hot.index];
                    drawAvatar (g, a.withHeight (a.getWidth()).reduced (6.0f), u8 (ar.name), isHover);
                    auto text = hot.area.withTrimmedTop (hot.area.getWidth() + 8);
                    g.setColour (tokens::bright);
                    g.setFont (juce::Font (juce::FontOptions (15.0f, juce::Font::bold)));
                    g.drawText (u8 (ar.name), text.removeFromTop (20), juce::Justification::centred, true);
                    g.setColour (tokens::faint);
                    g.setFont (juce::Font (juce::FontOptions (12.5f)));
                    g.drawText (u8 (ar.country), text.removeFromTop (18), juce::Justification::centred, true);
                    break;
                }
                case Kind::creator:
                {
                    const auto& c = creators()[(size_t) hot.index];
                    g.setColour (isHover ? tokens::cardHi : tokens::card);
                    g.fillRoundedRectangle (a, 12.0f);
                    g.setColour (isHover ? tokens::indigo.withAlpha (0.7f) : tokens::border);
                    g.drawRoundedRectangle (a.reduced (0.5f), 12.0f, 1.0f);
                    const float av = juce::jmin (78.0f, a.getWidth() * 0.45f);
                    drawAvatar (g, juce::Rectangle<float> (a.getCentreX() - av * 0.5f, a.getY() + 18.0f, av, av), u8 (c.name), false);
                    auto text = hot.area.withTrimmedTop ((int) (18 + av + 10));
                    g.setColour (tokens::bright);
                    g.setFont (juce::Font (juce::FontOptions (15.0f, juce::Font::bold)));
                    g.drawText (u8 (c.name), text.removeFromTop (20), juce::Justification::centred, true);
                    text.removeFromTop (5);
                    drawCreatorBadge (g, text.removeFromTop (18).toFloat().withSizeKeepingCentre (86.0f, 18.0f));
                    text.removeFromTop (6);
                    g.setColour (tokens::dim);
                    g.setFont (juce::Font (juce::FontOptions (12.5f)));
                    g.drawText (u8 (c.craft), text.removeFromTop (17), juce::Justification::centred, true);
                    g.setColour (tokens::faint);
                    g.drawText (juce::String (c.products) + " products  " + u8 ("\xc2\xb7") + "  " + u8 (c.country), text.removeFromTop (17), juce::Justification::centred, true);
                    break;
                }
                case Kind::follow:
                {
                    g.setColour (isHover ? tokens::bright : juce::Colours::transparentBlack);
                    g.fillRoundedRectangle (a, a.getHeight() * 0.5f);
                    if (! isHover) { g.setColour (tokens::dim); g.drawRoundedRectangle (a.reduced (0.5f), a.getHeight() * 0.5f, 1.0f); }
                    g.setColour (isHover ? tokens::ground : tokens::bright);
                    g.setFont (juce::Font (juce::FontOptions (13.0f, juce::Font::bold)));
                    g.drawText ("Follow", hot.area, juce::Justification::centred, false);
                    break;
                }
                case Kind::theme:
                {
                    const auto c = seedColour (seedOf (hot.label) + 40, 0.6f, 0.55f);
                    g.setGradientFill (juce::ColourGradient (c, a.getX(), a.getY(), c.darker (1.4f), a.getRight(), a.getBottom(), false));
                    g.fillRoundedRectangle (isHover ? a.reduced (-2.0f) : a, 10.0f);
                    g.setColour (juce::Colours::white);
                    g.setFont (juce::Font (juce::FontOptions (17.0f, juce::Font::bold)));
                    g.drawText (hot.label, hot.area.reduced (18, 0), juce::Justification::centredLeft, true);
                    break;
                }
                case Kind::link:
                    break;
            }
        }

        void addSection (const juce::String& title, const juce::String& badge, int x, int w, int& y)
        {
            juce::Rectangle<int> area (x, y, w, 28);
            juce::Font f (juce::FontOptions (13.0f));
            const int vw = juce::GlyphArrangement::getStringWidthInt (f, "View all") + 8;
            hots.push_back ({ area.withLeft (area.getRight() - vw), Kind::link, title + ": view all", -1 });
            sectionTitles.push_back ({ title, badge, area, (int) hots.size() - 1 });
            y += 28 + 14;
        }

        int hotAt (juce::Point<int> p) const
        {
            // smaller controls sit inside bigger ones (Follow in a creator card), so the later one wins
            for (int i = (int) hots.size() - 1; i >= 0; --i)
                if (hots[(size_t) i].area.contains (p)) return i;
            return -1;
        }

        std::vector<Hot> hots;
        std::vector<SectionTitle> sectionTitles;
        juce::Rectangle<int> header, heroCard, heroText, heroLogo, heroQr, heroQrCaption;
        int footerY { 0 };
        int hovered { -1 };
    };

    juce::Viewport viewport;
    Page page;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (StoreShowcase)
};

} // namespace ezstore
