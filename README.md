# PerformLive

A stage instrument for running stems, loops, pads and a click live, by
[Amanorsac Studio](https://amanorsac.studio).

This repository builds **PERFORMLIVE BETA (Testing)** for Mac and iPad. The
beta was released on 13 September 2026 and can be used until 12 October 2026.
It opens empty: no sounds are included. It goes online only when you use the WEB tab's browser, or open a link on the STORE page.

## Builds

| Platform | How | Output |
|---|---|---|
| Mac | GitHub Actions, `.github/workflows/build.yml` | `PerformLive-<version>-macOS.pkg`, signed and notarised |
| iPad | GitHub Actions, same workflow | TestFlight, plus simulator screenshots |
| Windows | `package_release.ps1` on the studio machine (needs Steinberg's ASIO SDK) | `PerformLive-<version>-Windows.exe` |

Clone with `git clone --recursive` so the `JUCE/` submodule (JUCE 9.0.0) comes too.

## Creators

Want to sell your loops and stems in PerformLive? Open the STORE page in the app.

## Licence

Copyright © 2026 Amanorsac Studio. All rights reserved. The source is public so
the Mac and iPad versions can be built here; it is not licensed for reuse. See
[amanorsac.studio/legal](https://amanorsac.studio/legal). The bundled fonts
(Inter, Space Grotesk, JetBrains Mono) are under the SIL Open Font Licence 1.1.
