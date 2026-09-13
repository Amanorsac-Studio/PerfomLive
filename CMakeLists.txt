cmake_minimum_required(VERSION 3.22)

project(EzPlay VERSION 0.4.0)

# ---- JUCE --------------------------------------------------------------------
# A clone of the public repo (and CI) gets JUCE 9.0.0 as the JUCE/ submodule.
# The studio's Windows machine builds against its shared copy at C:/JUCE.
set(PERFORMLIVE_JUCE_DIR "C:/JUCE" CACHE PATH "JUCE checkout to use when there is no JUCE/ submodule")
if (EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/JUCE/CMakeLists.txt")
    add_subdirectory(JUCE)
else()
    add_subdirectory(${PERFORMLIVE_JUCE_DIR} ${CMAKE_BINARY_DIR}/JUCE)
endif()

# CFBundleVersion on Mac and iPad. TestFlight refuses a second upload with a
# number it has already seen, so CI passes its run number here.
set(PERFORMLIVE_BUILD_NUMBER "1" CACHE STRING "Apple build number (CFBundleVersion)")

# ---- per-platform app settings -----------------------------------------------
set(PERFORMLIVE_PLATFORM_ARGS)
if (IOS)
    # iPad only, landscape, full screen: the performance layout needs the width,
    # and a stage app must not rotate into portrait mid-song or share the screen.
    #
    # Background audio keeps a set playing when the screen locks. File sharing
    # plus "open in place" makes the app's Documents folder -- where projects
    # and the library live (ProductPaths.h) -- visible in the Files app.
    #
    # The microphone text is required by App Store validation because JUCE's
    # audio code links the input APIs. PerformLive opens no inputs, so iPadOS
    # never shows it in practice.
    #
    # ITSAppUsesNonExemptEncryption=false: the app uses no encryption, which
    # also stops TestFlight holding every build for an export-compliance answer.
    list(APPEND PERFORMLIVE_PLATFORM_ARGS
        ICON_BIG "${CMAKE_CURRENT_SOURCE_DIR}/logo_icon_1024.png"
        BUILD_VERSION "${PERFORMLIVE_BUILD_NUMBER}"
        TARGETED_DEVICE_FAMILY 2
        IPAD_SCREEN_ORIENTATIONS UIInterfaceOrientationLandscapeLeft UIInterfaceOrientationLandscapeRight
        IPHONE_SCREEN_ORIENTATIONS UIInterfaceOrientationLandscapeLeft UIInterfaceOrientationLandscapeRight
        REQUIRES_FULL_SCREEN TRUE
        STATUS_BAR_HIDDEN TRUE
        FILE_SHARING_ENABLED TRUE
        BACKGROUND_AUDIO_ENABLED TRUE
        MICROPHONE_PERMISSION_ENABLED TRUE
        MICROPHONE_PERMISSION_TEXT "PerformLive only plays audio. iPadOS asks this for apps that can use audio interfaces with inputs."
        PLIST_TO_MERGE "<plist><dict><key>LSSupportsOpeningDocumentsInPlace</key><true/><key>ITSAppUsesNonExemptEncryption</key><false/></dict></plist>")
elseif (APPLE)
    # Hardened runtime is required for notarisation. No audio-input entitlement:
    # the app opens outputs only (setAudioChannels (0, 2) in Main.cpp).
    list(APPEND PERFORMLIVE_PLATFORM_ARGS
        ICON_BIG "${CMAKE_CURRENT_SOURCE_DIR}/logo_icon_1024.png"
        BUILD_VERSION "${PERFORMLIVE_BUILD_NUMBER}"
        HARDENED_RUNTIME_ENABLED TRUE
        PLIST_TO_MERGE "<plist><dict><key>LSApplicationCategoryType</key><string>public.app-category.music</string><key>ITSAppUsesNonExemptEncryption</key><false/></dict></plist>")
else()
    list(APPEND PERFORMLIVE_PLATFORM_ARGS
        ICON_BIG "${CMAKE_CURRENT_SOURCE_DIR}/logo_icon.png")
endif()

# Standalone app target. The TARGET stays EzPlay (internal names keep the
# working title) -- PRODUCT_NAME is the user-facing identity, so the built app
# is PerformLive(.exe/.app) with the PerformLive "P" emblem as its icon.
#
# No WebView2: the in-app browser was removed for the public beta, which makes
# no network requests at all (Build Standard B46). COMPANY_NAME and BUNDLE_ID
# put the manufacturer into the app's metadata (B29). The bundle id is the one
# registered with Apple for the Mac and iPad apps.
juce_add_gui_app(EzPlay
    PRODUCT_NAME "PerformLive"
    COMPANY_NAME "Amanorsac Studio"
    BUNDLE_ID "com.amanorsac.performlive"
    ${PERFORMLIVE_PLATFORM_ARGS})

# Our source files
target_sources(EzPlay PRIVATE
    Main.cpp
    Library.cpp
    ProjectFile.cpp)

# EzDSP.h lives in this folder, so the app can #include it
target_include_directories(EzPlay PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR})

# Generate the JuceHeader.h include
juce_generate_juce_header(EzPlay)

# ---- bundled fonts and art (Mac, iPad) ----------------------------------------
# Windows installs these beside the exe (installer/PerformLive.iss). A signed
# Mac bundle may only hold code in Contents/MacOS, so there they go into
# Contents/Resources; iPad bundles are flat. productpaths::bundledResources()
# knows all three layouts.
if (APPLE)
    file(GLOB PERFORMLIVE_FONT_FILES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/fonts/*.ttf")
    file(GLOB PERFORMLIVE_ART_FILES  CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/art/*.png" "${CMAKE_CURRENT_SOURCE_DIR}/art/*.jpg")
    set(PERFORMLIVE_LOGO_FILE "${CMAKE_CURRENT_SOURCE_DIR}/logo.png")
    target_sources(EzPlay PRIVATE ${PERFORMLIVE_FONT_FILES} ${PERFORMLIVE_ART_FILES} ${PERFORMLIVE_LOGO_FILE})
    set_source_files_properties(${PERFORMLIVE_FONT_FILES} PROPERTIES MACOSX_PACKAGE_LOCATION "Resources/fonts")
    set_source_files_properties(${PERFORMLIVE_ART_FILES}  PROPERTIES MACOSX_PACKAGE_LOCATION "Resources/art")
    set_source_files_properties(${PERFORMLIVE_LOGO_FILE}  PROPERTIES MACOSX_PACKAGE_LOCATION "Resources")

    # Xcode "optimises" PNGs for iOS into Apple's private CgBI format, which
    # JUCE's PNG decoder cannot read -- the art would silently fail to load.
    set_target_properties(EzPlay PROPERTIES
        XCODE_ATTRIBUTE_COMPRESS_PNG_FILES NO
        XCODE_ATTRIBUTE_STRIP_PNG_TEXT NO)
endif()

# ---- ASIO (Windows) -------------------------------------------------------------
# Every serious audio interface on Windows -- Behringer UMC, Focusrite,
# Yamaha/Steinberg, RME -- exposes its low-latency, multi-output driver as
# ASIO. Without this the app only sees WASAPI/DirectSound, which typically
# shows a UMC as one stereo endpoint (or nothing), and the owner's "Out 3/4"
# routing has no hardware to reach. (Mac and iPad use Core Audio, which shows
# every output of a class-compliant interface with no SDK.)
#
# JUCE needs Steinberg's SDK headers (just headers, nothing to link). They
# are licensed by Steinberg and cannot be committed to this repo, so:
#   1. download from https://www.steinberg.net/asiosdk
#   2. unzip so that  <ASIO_SDK_DIR>/common/iasiodrv.h  exists
#   3. point ASIO_SDK_DIR at it (default: the studio's shared SDK folder,
#      C:/Amanorsac Studio/sdks/ASIOSDK, per the File & Data Conventions)
# If the header is not found, the build still succeeds -- just without ASIO,
# and it says so loudly. Do NOT ship a Windows build that printed that
# warning: it cannot see multi-output interfaces.
if (WIN32)
    set(ASIO_SDK_DIR "C:/Amanorsac Studio/sdks/ASIOSDK" CACHE PATH "Steinberg ASIO SDK root (contains common/iasiodrv.h)")
    if (EXISTS "${ASIO_SDK_DIR}/common/iasiodrv.h")
        message(STATUS "ASIO: enabled (${ASIO_SDK_DIR})")
        target_include_directories(EzPlay PRIVATE "${ASIO_SDK_DIR}/common")
        target_compile_definitions(EzPlay PRIVATE JUCE_ASIO=1)
    else()
        message(WARNING "ASIO SDK not found at ${ASIO_SDK_DIR} -- building WITHOUT ASIO. "
                        "Multi-output interfaces (UMC, Focusrite...) will not be visible. "
                        "See the ASIO section of CMakeLists.txt.")
    endif()
    target_compile_definitions(EzPlay PRIVATE
        JUCE_WASAPI=1
        JUCE_DIRECTSOUND=1)
endif()

# JUCE compile settings
target_compile_definitions(EzPlay PRIVATE
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JUCE_APPLICATION_NAME_STRING="$<TARGET_PROPERTY:EzPlay,JUCE_PRODUCT_NAME>"
    JUCE_APPLICATION_VERSION_STRING="$<TARGET_PROPERTY:EzPlay,JUCE_VERSION>")

# Security hardening (owner: "make the app and its code safe"): Control
# Flow Guard on top of MSVC's defaults (/GS stack cookies, /DYNAMICBASE
# ASLR + high-entropy VA on x64, /NXCOMPAT DEP are already on). CFG makes
# indirect-call hijacking after a memory-corruption bug substantially
# harder; measurable cost is negligible for this app.
if (MSVC)
    target_compile_options(EzPlay PRIVATE /guard:cf)
    target_link_options(EzPlay PRIVATE /guard:cf)

    # Link the C runtime STATICALLY. Costs about a megabyte of exe size and
    # buys the thing that matters when handing a build to someone else: it
    # runs on a clean Windows machine with nothing installed. Dynamically
    # linked, a tester without the matching Visual C++ redistributable gets
    # "VCRUNTIME140.dll was not found" and concludes the app is broken.
    set_property(TARGET EzPlay PROPERTY
                 MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

# Link the JUCE modules we need.
# juce_audio_formats brings the WAV/AIFF decoders for loading files.
target_link_libraries(EzPlay PRIVATE
    juce::juce_audio_utils
    juce::juce_audio_devices
    juce::juce_audio_formats
    juce::juce_cryptography
    juce::juce_dsp
    juce::juce_gui_extra
    PRIVATE
    juce::juce_recommended_config_flags
    juce::juce_recommended_lto_flags
    juce::juce_recommended_warning_flags)
