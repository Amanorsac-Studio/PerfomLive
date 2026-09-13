// ============================================================================
//  Beta.h -- the public beta's label and its fixed end date.
//
//  The owner's decision: the beta stops opening 30 days after the RELEASE
//  DATE (13 September 2026), the same date for every tester on every platform.
//  Not 30 days from each person's first launch -- that resets the moment
//  someone deletes a settings file -- and not from the compile date, which
//  would give the Windows, Mac and iPad builds different end dates depending
//  on which day each one happened to be built.
//
//  It fails CLOSED. If the release date ever failed to parse, the beta counts
//  as ended rather than as never ending. A build that silently never expires
//  is the worse mistake; the self-test pins the parse.
//
//  No network and no time server. Setting the computer's clock back gets
//  around it, and that is accepted for a free beta: the point is that it ends,
//  not that it cannot be defeated.
// ============================================================================
#pragma once

#include <JuceHeader.h>

#include <cstdio>
#include <cstring>

namespace performbeta
{

/** Shown in the window title, on the STORE page and in the installer. */
inline constexpr const char* kLabel = "PERFORMLIVE BETA (Testing)";

/** The public release, in the "Mmm dd yyyy" format parseCompileDate() reads.
    package_release.ps1 and the CI workflow read this line for the installer's
    "use it until" text, so keep it on one line in exactly this form. */
inline constexpr const char* kReleaseDate = "Sep 13 2026";

/** How long the beta stays usable, counted from the release date. */
inline constexpr int kLifetimeDays = 30;

/** Parses the C++ __DATE__ format, "Mmm dd yyyy", where a single-digit day is
    padded with a space ("Sep  2 2026"). Returns local midnight of that day, or
    a null Time (0 ms) when the string is not in that format. */
inline juce::Time parseCompileDate (const char* date)
{
    static const char* const kMonths[] = { "Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                           "Jul", "Aug", "Sep", "Oct", "Nov", "Dec" };
    if (date == nullptr || std::strlen (date) != 11)
        return {};

    int month = -1;
    for (int m = 0; m < 12; ++m)
        if (std::strncmp (date, kMonths[m], 3) == 0) { month = m; break; }
    if (month < 0 || date[3] != ' ' || date[6] != ' ')
        return {};

    auto digit = [] (char c) { return c >= '0' && c <= '9'; };
    if (! (date[4] == ' ' || digit (date[4])) || ! digit (date[5]))
        return {};
    const int day = (date[4] == ' ' ? 0 : (date[4] - '0') * 10) + (date[5] - '0');

    for (int i = 7; i < 11; ++i)
        if (! digit (date[i])) return {};
    const int year = (date[7] - '0') * 1000 + (date[8] - '0') * 100
                   + (date[9] - '0') * 10   + (date[10] - '0');

    if (day < 1 || day > 31 || year < 2000)
        return {};
    return juce::Time (year, month, day, 0, 0, 0, 0, true);
}

/** Local midnight of the calendar day that contains `t` shifted by `offset`.
    Adding whole days across a daylight-saving change lands on 23:00 or 01:00;
    shifting by half a day before snapping absorbs that, so the date is right. */
inline juce::Time snapToLocalMidnight (juce::Time t)
{
    return juce::Time (t.getYear(), t.getMonth(), t.getDayOfMonth(), 0, 0, 0, 0, true);
}

/** The moment a build made on `built` stops opening. Null in, null out. */
inline juce::Time expiryFor (juce::Time built)
{
    if (built.toMilliseconds() == 0)
        return {};
    return snapToLocalMidnight (built + juce::RelativeTime::days ((double) kLifetimeDays)
                                      + juce::RelativeTime::hours (12.0));
}

/** The last calendar day the beta can be used: the day before it ends. */
inline juce::Time lastDay (juce::Time endsAt)
{
    if (endsAt.toMilliseconds() == 0)
        return {};
    return snapToLocalMidnight (endsAt - juce::RelativeTime::hours (12.0));
}

inline juce::Time releaseDate() { return parseCompileDate (kReleaseDate); }
inline juce::Time expiry()      { return expiryFor (releaseDate()); }

/** True once `now` has reached the end moment. An unknown end counts as ended. */
inline bool hasEnded (juce::Time now, juce::Time endsAt)
{
    return endsAt.toMilliseconds() == 0 || now >= endsAt;
}

/** "12 October 2026" -- day first, matching the studio's British-English
    documents. Built by hand: strftime's day-without-padding flag is not
    portable, and MSVC's strftime aborts on a flag it does not know. */
inline juce::String dateText (juce::Time t)
{
    if (t.toMilliseconds() == 0)
        return "an unknown date";
    return juce::String (t.getDayOfMonth()) + " " + t.getMonthName (false) + " " + juce::String (t.getYear());
}

//==============================================================================
/** Run with `PerformLive.exe --selftest-beta`. Hermetic: no clock changes, no
    files. Prints this codebase's "  [PASS] ..." / "  [FAIL] ..." convention. */
inline bool runSelfTest()
{
    bool ok = true;
    auto check = [&ok] (bool cond, const char* what)
    {
        std::printf ("  [%s] %s\n", cond ? "PASS" : "FAIL", what);
        if (! cond) ok = false;
    };
    auto ymd = [] (juce::Time t, int y, int month0, int d)
    {
        return t.getYear() == y && t.getMonth() == month0 && t.getDayOfMonth() == d;
    };

    const auto built = parseCompileDate ("Sep 12 2026");
    check (ymd (built, 2026, 8, 12),                              "parses a two-digit day");
    check (ymd (parseCompileDate ("Sep  2 2026"), 2026, 8, 2),    "parses a space-padded single-digit day");
    check (parseCompileDate ("Foo 12 2026").toMilliseconds() == 0, "rejects an unknown month");
    check (parseCompileDate ("Sep 12 26").toMilliseconds() == 0,   "rejects a malformed year");
    check (parseCompileDate ("Sep 1x 2026").toMilliseconds() == 0, "rejects a malformed day");
    check (parseCompileDate (nullptr).toMilliseconds() == 0,       "rejects a null string");

    const auto endsAt = expiryFor (built);
    check (ymd (endsAt, 2026, 9, 12),                             "ends 30 days after the build date");
    check (endsAt.getHours() == 0 && endsAt.getMinutes() == 0,    "ends at local midnight");
    check (ymd (expiryFor (parseCompileDate ("Dec 20 2026")), 2027, 0, 19),  "crosses a year boundary");
    check (ymd (expiryFor (parseCompileDate ("Oct 20 2026")), 2026, 10, 19), "keeps the right date across a daylight-saving change");
    check (expiryFor (juce::Time()).toMilliseconds() == 0,        "an unknown build date gives an unknown end");

    check (! hasEnded (built, endsAt),                                        "works on the day it was built");
    check (! hasEnded (endsAt - juce::RelativeTime::minutes (1.0), endsAt),   "still works the minute before it ends");
    check (hasEnded (endsAt, endsAt),                                         "has ended at the end moment");
    check (hasEnded (endsAt + juce::RelativeTime::days (1.0), endsAt),        "has ended the day after");
    check (hasEnded (juce::Time::getCurrentTime(), juce::Time()),             "fails closed when the end date is unknown");

    check (ymd (lastDay (endsAt), 2026, 9, 11),                   "reports the last usable day");
    check (dateText (endsAt) == "12 October 2026",                "writes the date day first");

    const auto liveEnd = expiry();
    check (liveEnd.toMilliseconds() != 0,                         "the release date parses");
    check (ymd (releaseDate(), 2026, 8, 13),                      "the release date is 13 September 2026");
    check (ymd (liveEnd, 2026, 9, 13),                            "the beta ends at the start of 13 October 2026");
    check (ymd (lastDay (liveEnd), 2026, 9, 12),                  "12 October 2026 is the last day it can be used");
    check (! hasEnded (releaseDate(), liveEnd),                   "usable on the release date");

    return ok;
}

} // namespace performbeta
