// ============================================================================
//  ProductPaths.h -- where PerformLive reads its bundled files from, and the
//  only two places it writes.
//
//  From the studio's File & Data Conventions (doc 04, sections 1.1 and 1.2):
//
//    user content   Documents/Amanorsac Studio/PerformLive/
//                   projects, the library -- what a person would back up.
//                   On iPad this is inside the app's own Documents folder,
//                   which the Files app shows under "On My iPad > PerformLive".
//
//    machine state  %LOCALAPPDATA%/Amanorsac Studio/PerformLive/   (Windows)
//                   ~/Library/Application Support/Amanorsac Studio/PerformLive/
//                                                                (Mac, iPad)
//                   settings, logs -- things nobody would miss if deleted
//
//  Never beside the executable, never under Program Files or inside the app
//  bundle (Build Standard B48/B49). Both folders are created on first use, so
//  a missing folder is never an error.
// ============================================================================
#pragma once

#include <JuceHeader.h>

namespace productpaths
{

inline constexpr const char* kCompany = "Amanorsac Studio";
inline constexpr const char* kProduct = "PerformLive";

inline juce::File userContent()
{
    auto dir = juce::File::getSpecialLocation (juce::File::userDocumentsDirectory)
                   .getChildFile (kCompany).getChildFile (kProduct);
    dir.createDirectory();
    return dir;
}

inline juce::File machineState()
{
   #if JUCE_WINDOWS
    auto base = juce::File::getSpecialLocation (juce::File::windowsLocalAppData);
   #elif JUCE_MAC || JUCE_IOS
    // JUCE's userApplicationDataDirectory is ~/Library on Apple platforms; the
    // convention (and Apple's) is the Application Support folder inside it.
    auto base = juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory)
                    .getChildFile ("Application Support");
   #else
    auto base = juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory);
   #endif
    auto dir = base.getChildFile (kCompany).getChildFile (kProduct);
    dir.createDirectory();
    return dir;
}

/** The read-only folder the installed fonts/, art/ and logo.png live in.
    Windows: beside PerformLive.exe. Mac: PerformLive.app/Contents/Resources,
    because a signed bundle may only hold code in Contents/MacOS. iPad: the
    top of the (flat) app bundle. Callers still fall back to the working
    directory, which is what a development run from the repo uses. */
inline juce::File bundledResources()
{
   #if JUCE_MAC
    return juce::File::getSpecialLocation (juce::File::currentApplicationFile)
               .getChildFile ("Contents").getChildFile ("Resources");
   #elif JUCE_IOS
    // Flat bundle; if a build tool nests them under Resources/ after all,
    // use that instead of failing to find the fonts.
    const auto app = juce::File::getSpecialLocation (juce::File::currentApplicationFile);
    const auto nested = app.getChildFile ("Resources");
    return nested.getChildFile ("fonts").isDirectory() ? nested : app;
   #else
    return juce::File::getSpecialLocation (juce::File::currentExecutableFile).getParentDirectory();
   #endif
}

} // namespace productpaths
