// ============================================================================
//  BrowserTab.h -- the in-app browser, its download manager, and "Import
//  audio from URL".
//
//  THREE THINGS, KEPT HONEST:
//
//  1. THE BROWSER is Edge WebView2 -- a real Chromium -- so YouTube, Drive,
//     Dropbox and any vendor's site work as they do in Edge. It opens on
//     YouTube because that is where this app's users look for reference
//     tracks. Its audio plays through WINDOWS' default output device, not
//     through this app's mixer or ASIO routing: a WebView runs in its own
//     process and never enters our audio callback. The tab says so rather
//     than letting anyone discover it on stage.
//
//  2. THE DOWNLOAD MANAGER catches any direct link to an audio file or pack
//     (.wav .mp3 .flac .zip ...) that the page tries to open, downloads it
//     with resume and progress, and offers to put it in the Library. Nothing
//     downloaded is ever executed.
//
//  3. "IMPORT AUDIO FROM URL" hands a page address to yt-dlp, the
//     open-source extractor most desktop tools shell out to, and converts the
//     result to WAV so the deck can load it. Read this carefully, because it
//     is the part with consequences:
//
//       * yt-dlp, ffmpeg and deno are NOT shipped with this application. The
//         user enables the feature and the app fetches them from their own
//         GitHub release pages, verifying yt-dlp against its published
//         SHA-256 sums. That separation is deliberate and legal in nature:
//         this product contains no extraction code, and the tools are
//         obtained by the user, at their request, from their authors.
//       * Downloading from YouTube is against YouTube's terms of service,
//         and in the US and UK there is no personal-use exception in
//         copyright law. The feature requires the user to confirm, once, that
//         they hold the rights to what they import. That is an attestation,
//         not a licence, and the handover document says so plainly.
//       * The output is always re-encoded to WAV because JUCE on Windows
//         cannot decode the AAC/Opus streams yt-dlp would otherwise produce.
// ============================================================================
#pragma once

#include <JuceHeader.h>
#include "DownloadManager.h"

#include <functional>
#include <memory>

namespace ezweb
{

//==============================================================================
//  Where the helper tools live and how they are fetched.
//==============================================================================
class ToolsManager : private juce::Thread
{
public:
    ToolsManager() : juce::Thread ("PerformLive tools") {}
    ~ToolsManager() override { stopThread (8000); masterReference.clear(); }

    static juce::File toolsDir()
    {
       #if JUCE_WINDOWS
        auto base = juce::File::getSpecialLocation (juce::File::windowsLocalAppData);
       #else
        auto base = juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory);
       #endif
        auto d = base.getChildFile ("PerformLive").getChildFile ("tools");
        d.createDirectory();
        return d;
    }

    static juce::File ytdlpExe()  { return toolsDir().getChildFile ("yt-dlp.exe"); }
    static juce::File ffmpegDir() { return toolsDir().getChildFile ("ffmpeg"); }
    static juce::File ffmpegExe() { return ffmpegDir().getChildFile ("ffmpeg.exe"); }
    static juce::File denoExe()   { return toolsDir().getChildFile ("deno.exe"); }

    static bool isReady()
    {
        return ytdlpExe().existsAsFile() && ffmpegExe().existsAsFile()
            && ffmpegDir().getChildFile ("ffprobe.exe").existsAsFile();
    }

    static bool hasDeno() { return denoExe().existsAsFile(); }

    std::function<void (juce::String)> onStatus;       // progress text, message thread
    std::function<void (bool ok, juce::String)> onFinished;

    bool isInstalling() const { return isThreadRunning(); }

    /** Fetches whatever is missing. Safe to call when everything is present. */
    void install()
    {
        if (isThreadRunning()) return;
        startThread();
    }

private:
    void status (const juce::String& s)
    {
        juce::WeakReference<ToolsManager> safe (this);
        juce::MessageManager::callAsync ([safe, s] { if (auto* t = safe.get()) if (t->onStatus) t->onStatus (s); });
    }

    void finished (bool ok, const juce::String& s)
    {
        juce::WeakReference<ToolsManager> safe (this);
        juce::MessageManager::callAsync ([safe, ok, s] { if (auto* t = safe.get()) if (t->onFinished) t->onFinished (ok, s); });
    }

    static bool fetch (const juce::String& url, const juce::File& dest, juce::Thread& thread,
                       std::function<void (juce::int64, juce::int64)> progress)
    {
        int status = 0;
        auto opts = juce::URL::InputStreamOptions (juce::URL::ParameterHandling::inAddress)
                        .withConnectionTimeoutMs (30000).withStatusCode (&status).withNumRedirectsToFollow (8);
        std::unique_ptr<juce::InputStream> in (juce::URL (url).createInputStream (opts));
        if (in == nullptr || status >= 400) return false;

        auto tmp = dest.getSiblingFile (dest.getFileName() + ".part");
        tmp.deleteFile();
        std::unique_ptr<juce::FileOutputStream> out (tmp.createOutputStream());
        if (out == nullptr) return false;

        const juce::int64 total = in->getTotalLength();
        juce::HeapBlock<char> buf (256 * 1024);
        juce::int64 got = 0;
        for (;;)
        {
            if (thread.threadShouldExit()) { out.reset(); tmp.deleteFile(); return false; }
            const int n = in->read (buf, 256 * 1024);
            if (n <= 0) break;
            if (! out->write (buf, (size_t) n)) { out.reset(); tmp.deleteFile(); return false; }
            got += n;
            if (progress) progress (got, total);
        }
        out->flush();
        out.reset();
        dest.deleteFile();
        return tmp.moveFileTo (dest);
    }

    static juce::String sha256Hex (const juce::File& f)
    {
        juce::FileInputStream in (f);
        if (! in.openedOk()) return {};
        return juce::SHA256 (in).toHexString();
    }

    /** Pulls one named file out of a zip into `dest`, refusing anything odd. */
    static bool extractOne (const juce::File& zipFile, const juce::String& entrySuffix, const juce::File& dest)
    {
        juce::ZipFile zip (zipFile);
        for (int i = 0; i < zip.getNumEntries(); ++i)
        {
            const auto* e = zip.getEntry (i);
            if (e == nullptr || e->isSymbolicLink) continue;
            auto name = e->filename.replaceCharacter ('\\', '/');
            if (! name.endsWithIgnoreCase (entrySuffix)) continue;
            if (name.contains ("..")) return false;

            std::unique_ptr<juce::InputStream> in (zip.createStreamForEntry (i));
            if (in == nullptr) return false;
            dest.getParentDirectory().createDirectory();
            auto tmp = dest.getSiblingFile (dest.getFileName() + ".part");
            tmp.deleteFile();
            std::unique_ptr<juce::FileOutputStream> out (tmp.createOutputStream());
            if (out == nullptr) return false;
            out->writeFromInputStream (*in, -1);
            out->flush();
            out.reset();
            dest.deleteFile();
            return tmp.moveFileTo (dest);
        }
        return false;
    }

    void run() override
    {
        auto dir = toolsDir();
        auto pct = [] (juce::int64 a, juce::int64 b) { return b > 0 ? juce::String ((int) (100 * a / b)) + "%" : juce::String (a / (1024 * 1024)) + " MB"; };

        // ---- yt-dlp, verified against its published checksums ----------------
        if (! ytdlpExe().existsAsFile())
        {
            status ("Fetching yt-dlp checksums...");
            auto sums = dir.getChildFile ("SHA2-256SUMS");
            if (! fetch ("https://github.com/yt-dlp/yt-dlp/releases/latest/download/SHA2-256SUMS", sums, *this, {}))
            { finished (false, "Couldn't fetch yt-dlp's checksum list from GitHub."); return; }

            juce::String expected;
            for (auto line : juce::StringArray::fromLines (sums.loadFileAsString()))
                if (line.trim().endsWithIgnoreCase ("yt-dlp.exe") && ! line.contains ("_x86") && ! line.contains ("_min"))
                    expected = line.upToFirstOccurrenceOf (" ", false, false).trim().toLowerCase();

            if (expected.length() != 64) { finished (false, "yt-dlp's checksum list didn't contain yt-dlp.exe."); return; }

            status ("Downloading yt-dlp...");
            auto exe = ytdlpExe();
            if (! fetch ("https://github.com/yt-dlp/yt-dlp/releases/latest/download/yt-dlp.exe", exe, *this,
                         [this, pct] (juce::int64 a, juce::int64 b) { status ("Downloading yt-dlp... " + pct (a, b)); }))
            { finished (false, "Couldn't download yt-dlp."); return; }

            if (sha256Hex (exe) != expected)
            {
                exe.deleteFile();
                finished (false, "yt-dlp did not match its published checksum and was discarded.");
                return;
            }
        }

        // ---- ffmpeg + ffprobe (yt-dlp's own Windows builds) -------------------
        if (! ffmpegExe().existsAsFile() || ! ffmpegDir().getChildFile ("ffprobe.exe").existsAsFile())
        {
            status ("Downloading ffmpeg (about 100 MB)...");
            auto zip = dir.getChildFile ("ffmpeg.zip");
            if (! fetch ("https://github.com/yt-dlp/FFmpeg-Builds/releases/latest/download/ffmpeg-master-latest-win64-gpl.zip",
                         zip, *this, [this, pct] (juce::int64 a, juce::int64 b) { status ("Downloading ffmpeg... " + pct (a, b)); }))
            { finished (false, "Couldn't download ffmpeg."); return; }

            status ("Unpacking ffmpeg...");
            const bool ok = extractOne (zip, "/bin/ffmpeg.exe",  ffmpegDir().getChildFile ("ffmpeg.exe"))
                         && extractOne (zip, "/bin/ffprobe.exe", ffmpegDir().getChildFile ("ffprobe.exe"));
            zip.deleteFile();
            if (! ok) { finished (false, "The ffmpeg archive didn't contain ffmpeg.exe and ffprobe.exe."); return; }
        }

        // ---- deno: the JavaScript runtime yt-dlp needs for YouTube since
        //      late 2025. Optional in the sense that yt-dlp runs without it,
        //      but format availability degrades -- so it is fetched too.
        if (! denoExe().existsAsFile())
        {
            status ("Downloading deno (about 40 MB)...");
            auto zip = dir.getChildFile ("deno.zip");
            auto sum = dir.getChildFile ("deno.sha256sum");
            const juce::String base = "https://github.com/denoland/deno/releases/latest/download/deno-x86_64-pc-windows-msvc.zip";
            if (! fetch (base, zip, *this, [this, pct] (juce::int64 a, juce::int64 b) { status ("Downloading deno... " + pct (a, b)); }))
            { finished (false, "Couldn't download deno."); return; }

            if (fetch (base + ".sha256sum", sum, *this, {}))
            {
                const auto expected = sum.loadFileAsString().upToFirstOccurrenceOf (" ", false, false).trim().toLowerCase();
                if (expected.length() == 64 && sha256Hex (zip) != expected)
                {
                    zip.deleteFile();
                    finished (false, "deno did not match its published checksum and was discarded.");
                    return;
                }
            }

            status ("Unpacking deno...");
            const bool ok = extractOne (zip, "deno.exe", denoExe());
            zip.deleteFile();
            sum.deleteFile();
            if (! ok) { finished (false, "The deno archive didn't contain deno.exe."); return; }
        }

        finished (true, "Import tools are ready.");
    }

    JUCE_DECLARE_WEAK_REFERENCEABLE (ToolsManager)
};

//==============================================================================
//  One "import from URL" job: yt-dlp as a child process, progress parsed
//  from its own machine-readable template, output converted to WAV.
//==============================================================================
class UrlImportJob : private juce::Thread
{
public:
    UrlImportJob() : juce::Thread ("PerformLive url import") {}
    ~UrlImportJob() override { cancel(); stopThread (6000); masterReference.clear(); }

    std::function<void (juce::String)> onStatus;
    std::function<void (double)>       onProgress;   // 0..1
    std::function<void (bool ok, juce::File result, juce::String message)> onFinished;

    bool isRunning() const { return isThreadRunning(); }

    void start (const juce::String& url, const juce::File& outDir)
    {
        if (isThreadRunning()) return;
        pageUrl = url.trim();
        outputDir = outDir;
        cancelled = false;
        startThread();
    }

    void cancel()
    {
        cancelled = true;
        if (child.isRunning()) child.kill();
    }

private:
    void status (const juce::String& s)
    {
        juce::WeakReference<UrlImportJob> safe (this);
        juce::MessageManager::callAsync ([safe, s] { if (auto* j = safe.get()) if (j->onStatus) j->onStatus (s); });
    }
    void progress (double p)
    {
        juce::WeakReference<UrlImportJob> safe (this);
        juce::MessageManager::callAsync ([safe, p] { if (auto* j = safe.get()) if (j->onProgress) j->onProgress (p); });
    }
    void finished (bool ok, juce::File f, const juce::String& s)
    {
        juce::WeakReference<UrlImportJob> safe (this);
        juce::MessageManager::callAsync ([safe, ok, f, s] { if (auto* j = safe.get()) if (j->onFinished) j->onFinished (ok, f, s); });
    }

    void run() override
    {
        if (! ToolsManager::isReady()) { finished (false, {}, "Import tools aren't installed yet."); return; }
        if (! ezdl::DownloadManager::isAcceptableUrl (pageUrl)) { finished (false, {}, "That isn't a web address."); return; }

        outputDir.createDirectory();

        // Every argument is passed as its own array element, never joined
        // into a shell string -- the URL comes from a text box and must not
        // be able to smuggle in extra arguments.
        juce::StringArray args;
        args.add (ToolsManager::ytdlpExe().getFullPathName());
        args.add ("--no-playlist");
        args.add ("--no-warnings");
        args.add ("--no-colors");
        args.add ("--newline");
        args.add ("--progress");
        args.add ("--progress-template");
        args.add ("download:PL|%(progress.status)s|%(progress.downloaded_bytes)s|%(progress.total_bytes_estimate)s|%(progress.speed)s");
        args.add ("--ffmpeg-location");
        args.add (ToolsManager::ffmpegDir().getFullPathName());
        if (ToolsManager::hasDeno())
        {
            args.add ("--js-runtimes");
            args.add ("deno:" + ToolsManager::denoExe().getFullPathName());
        }
        args.add ("-f"); args.add ("bestaudio");
        args.add ("-x");
        args.add ("--audio-format"); args.add ("wav");
        // pin the WAV to a rate the deck plays natively at the most common
        // interface setting -- the source is lossy anyway, so nothing is lost
        args.add ("--postprocessor-args"); args.add ("ffmpeg:-ar 48000");
        args.add ("--restrict-filenames");
        args.add ("-o"); args.add (outputDir.getChildFile ("%(title).80s [%(id)s].%(ext)s").getFullPathName());
        args.add ("--print"); args.add ("after_move:filepath");
        args.add ("--"); args.add (pageUrl);

        status ("Starting import...");
        if (! child.start (args, juce::ChildProcess::wantStdOut | juce::ChildProcess::wantStdErr))
        { finished (false, {}, "Couldn't start yt-dlp."); return; }

        juce::String buffer, lastPath, lastError;
        char chunk[4096];
        while (child.isRunning() || true)
        {
            if (threadShouldExit() || cancelled) { child.kill(); finished (false, {}, "Import cancelled."); return; }
            const int n = child.readProcessOutput (chunk, sizeof (chunk));
            if (n > 0)
            {
                buffer += juce::String::fromUTF8 (chunk, n);
                int nl;
                while ((nl = buffer.indexOfChar ('\n')) >= 0)
                {
                    auto line = buffer.substring (0, nl).trim();
                    buffer = buffer.substring (nl + 1);
                    handleLine (line, lastPath, lastError);
                }
            }
            else if (! child.isRunning())
            {
                break;
            }
            else
            {
                wait (50);
            }
        }
        if (buffer.trim().isNotEmpty()) handleLine (buffer.trim(), lastPath, lastError);

        const auto exit = child.getExitCode();
        juce::File result (lastPath);
        if (exit == 0 && result.existsAsFile())
            finished (true, result, "Imported " + result.getFileName());
        else
            finished (false, {}, lastError.isNotEmpty() ? lastError : "yt-dlp exited with code " + juce::String ((int) exit));
    }

    void handleLine (const juce::String& line, juce::String& lastPath, juce::String& lastError)
    {
        if (line.startsWith ("PL|"))
        {
            auto parts = juce::StringArray::fromTokens (line, "|", "");
            if (parts.size() >= 4)
            {
                const double done  = parts[2].getDoubleValue();
                const double total = parts[3].getDoubleValue();
                if (total > 0) progress (juce::jlimit (0.0, 1.0, done / total));
                status (parts[1] == "finished" ? "Converting to WAV..." : "Downloading... "
                        + juce::String ((int) (total > 0 ? 100.0 * done / total : 0)) + "%");
            }
            return;
        }
        if (line.startsWithIgnoreCase ("ERROR"))   { lastError = line.fromFirstOccurrenceOf (":", false, false).trim(); return; }
        if (line.startsWith ("[ExtractAudio]"))    { status ("Converting to WAV..."); return; }
        // --print after_move:filepath emits a bare absolute path
        if (line.length() > 3 && line[1] == ':' && juce::File::isAbsolutePath (line)) lastPath = line;
    }

    juce::ChildProcess child;
    juce::String pageUrl;
    juce::File outputDir;
    std::atomic<bool> cancelled { false };

    JUCE_DECLARE_WEAK_REFERENCEABLE (UrlImportJob)
};

//==============================================================================
//  The browser itself: intercepts file links, tracks URL/title.
//==============================================================================
class Browser : public juce::WebBrowserComponent
{
public:
    explicit Browser (const juce::WebBrowserComponent::Options& opts) : juce::WebBrowserComponent (opts) {}

    std::function<bool (juce::String)> onInterceptDownload;   // return true = handled, don't navigate
    std::function<void (juce::String)> onUrlChanged;
    std::function<void (juce::String)> onTitleChanged;
    std::function<void (juce::String)> onError;

    bool pageAboutToLoad (const juce::String& url) override
    {
        if (onInterceptDownload && ezdl::DownloadManager::looksLikeMediaFile (url) && onInterceptDownload (url))
            return false;
        return true;
    }

    void newWindowAttemptingToLoad (const juce::String& url) override
    {
        // target=_blank links: a file link goes to the downloader, anything
        // else opens in this same tab -- there is only one tab.
        if (onInterceptDownload && ezdl::DownloadManager::looksLikeMediaFile (url) && onInterceptDownload (url))
            return;
        goToURL (url);
    }

    void pageFinishedLoading (const juce::String& url) override
    {
        if (onUrlChanged) onUrlChanged (url);
        juce::Component::SafePointer<Browser> safe (this);
        evaluateJavascript ("document.title", [safe] (juce::WebBrowserComponent::EvaluationResult r)
        {
            if (safe == nullptr) return;
            if (auto* v = r.getResult())
                if (safe->onTitleChanged) safe->onTitleChanged (v->toString());
        });
    }

    bool pageLoadHadNetworkError (const juce::String& error) override
    {
        if (onError) onError (error);
        return true;   // we showed our own message; suppress the stock page
    }
};

//==============================================================================
//  The tab.
//==============================================================================
class BrowserTab : public juce::Component, private juce::Timer
{
public:
    static constexpr const char* kHomeUrl = "https://www.youtube.com/@amanorsac/videos";   // owner: the studio's channel is WEB's default page

    BrowserTab()
    {
        // ---- toolbar ----------------------------------------------------
        for (auto* b : { &backButton, &forwardButton, &reloadButton, &homeButton })
            addAndMakeVisible (b);
        backButton.setButtonText (juce::String (juce::CharPointer_UTF8 ("\xe2\x86\x90")));
        forwardButton.setButtonText (juce::String (juce::CharPointer_UTF8 ("\xe2\x86\x92")));
        reloadButton.setButtonText (juce::String (juce::CharPointer_UTF8 ("\xe2\x86\xbb")));
        homeButton.setButtonText ("YouTube");

        backButton.onClick    = [this] { if (browser) browser->goBack(); };
        forwardButton.onClick = [this] { if (browser) browser->goForward(); };
        reloadButton.onClick  = [this] { if (browser) browser->refresh(); };
        homeButton.onClick    = [this] { navigate (kHomeUrl); };

        urlBox.setTextToShowWhenEmpty ("Enter a web address or search", juce::Colour (0xff6f7099u));
        urlBox.setColour (juce::TextEditor::backgroundColourId, juce::Colour (0xff14162au));
        urlBox.setColour (juce::TextEditor::outlineColourId, juce::Colour (0xff2b2b4du));
        urlBox.setColour (juce::TextEditor::textColourId, juce::Colour (0xfff2f0ffu));
        urlBox.setSelectAllWhenFocused (true);
        urlBox.onReturnKey = [this] { navigate (urlBox.getText()); };
        addAndMakeVisible (urlBox);

        importButton.setButtonText ("Import audio from this page");
        importButton.onClick = [this] { importFromUrl (currentUrl); };
        addAndMakeVisible (importButton);

        downloadsButton.setButtonText ("Downloads");
        downloadsButton.setClickingTogglesState (true);
        downloadsButton.onClick = [this] { panelOpen = downloadsButton.getToggleState(); resized(); };
        addAndMakeVisible (downloadsButton);

        // ---- the note about audio -----------------------------------------
        audioNote.setText ("Browser audio plays through Windows' default output; the mixer's WEB strip sets the page's level and mute.",
                           juce::dontSendNotification);
        audioNote.setColour (juce::Label::textColourId, juce::Colour (0xff6f7099u));
        audioNote.setFont (juce::Font (juce::FontOptions (11.0f)));
        audioNote.setJustificationType (juce::Justification::centredRight);
        addAndMakeVisible (audioNote);

        // ---- downloads panel ----------------------------------------------
        addChildComponent (panel);
        panel.onAction = [this] (int id, juce::String action) { panelAction (id, action); };
        panel.onInstallTools = [this] { installTools(); };
        panel.onImportUrl = [this] (juce::String url) { importFromUrl (url); };

        downloads.onListChanged = [this] { panel.refresh (downloads, importJob != nullptr && importJob->isRunning(), importStatus, importProgress); };
        downloads.onMessage     = [this] (juce::String m) { if (onMessage) onMessage (m); };
        downloads.onCompleted   = [this] (const ezdl::DownloadItem& item) { completed (item.target); };

        // ---- the web view --------------------------------------------------
        createBrowser();
        startTimerHz (4);
    }

    ~BrowserTab() override { stopTimer(); }

    /** Wired by the host: a file ready to go into the Library. */
    std::function<void (juce::Array<juce::File>)> onImportFiles;
    std::function<void (juce::String)> onMessage;
    /** Wired by the host: persistent "has the user accepted the rights note". */
    std::function<bool()> getRightsAccepted;
    std::function<void()> setRightsAccepted;

    void navigate (juce::String text)
    {
        text = text.trim();
        if (text.isEmpty()) return;
        if (! text.containsChar ('.') || text.containsChar (' '))
            text = "https://www.youtube.com/results?search_query=" + juce::URL::addEscapeChars (text, true);
        else if (! text.startsWithIgnoreCase ("http://") && ! text.startsWithIgnoreCase ("https://"))
            text = "https://" + text;
        if (browser) browser->goToURL (text);
        currentUrl = text;
        urlBox.setText (text, juce::dontSendNotification);
    }

    // ---- the page's sound, from the mixer's WEB strip ------------------------
    // WebView2 plays through Windows itself, not through the app's mixer, so
    // the strip drives the page's own players: every <video>/<audio> gets the
    // level and mute. Re-applied on each page load and every couple of
    // seconds, because sites like YouTube swap players without a page load.

    /** Owner: "anytime I close the browser, stop playing anything." */
    void pauseMedia()
    {
        if (browser) browser->evaluateJavascript ("document.querySelectorAll('video,audio').forEach(function(e){e.pause();});", nullptr);
    }

    void setMediaLevel (float level01, bool muted)
    {
        mediaLevel = juce::jlimit (0.0f, 1.0f, level01);
        mediaMuted = muted;
        applyMediaLevel();
    }

    void applyMediaLevel()
    {
        if (! browser) return;
        browser->evaluateJavascript ("(function(){var v=" + juce::String (mediaLevel, 3) + ",m=" + (mediaMuted ? "true" : "false")
                                     + ";document.querySelectorAll('video,audio').forEach(function(e){e.volume=v;e.muted=m;});})();", nullptr);
    }

    void paint (juce::Graphics& g) override
    {
        g.fillAll (juce::Colour (0xff07070fu));
        g.setColour (juce::Colour (0xff14162au));
        g.fillRect (getLocalBounds().removeFromTop (kBarHeight));
        g.setColour (juce::Colour (0xff2b2b4du));
        g.fillRect (0, kBarHeight - 1, getWidth(), 1);

        if (browser == nullptr)
        {
            g.setColour (juce::Colour (0xffa3a6ccu));
            g.setFont (juce::Font (juce::FontOptions (14.0f)));
            g.drawFittedText (unavailableReason, getLocalBounds().withTrimmedTop (kBarHeight).reduced (40),
                              juce::Justification::centred, 6);
        }
    }

    void resized() override
    {
        auto area = getLocalBounds();
        auto bar = area.removeFromTop (kBarHeight).reduced (10, 7);

        backButton.setBounds (bar.removeFromLeft (34));    bar.removeFromLeft (4);
        forwardButton.setBounds (bar.removeFromLeft (34)); bar.removeFromLeft (4);
        reloadButton.setBounds (bar.removeFromLeft (34));  bar.removeFromLeft (4);
        homeButton.setBounds (bar.removeFromLeft (76));    bar.removeFromLeft (10);

        downloadsButton.setBounds (bar.removeFromRight (110)); bar.removeFromRight (8);
        importButton.setBounds (bar.removeFromRight (200));    bar.removeFromRight (10);
        urlBox.setBounds (bar);

        auto note = area.removeFromBottom (18);
        audioNote.setBounds (note.reduced (10, 0));

        if (panelOpen)
        {
            panel.setVisible (true);
            panel.setBounds (area.removeFromRight (juce::jmin (380, getWidth() / 2)));
        }
        else panel.setVisible (false);

        if (browser) browser->setBounds (area);
    }

private:
    static constexpr int kBarHeight = 46;

    //==========================================================================
    void createBrowser()
    {
       #if JUCE_WINDOWS
        auto dataDir = juce::File::getSpecialLocation (juce::File::windowsLocalAppData)
                           .getChildFile ("PerformLive").getChildFile ("WebView2");
       #else
        auto dataDir = juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory)
                           .getChildFile ("PerformLive").getChildFile ("WebView2");
       #endif
        dataDir.createDirectory();

        auto opts = juce::WebBrowserComponent::Options()
                        .withBackend (juce::WebBrowserComponent::Options::Backend::webview2)
                        .withKeepPageLoadedWhenBrowserIsHidden()   // switching tabs must not kill a playing video
                        .withWinWebView2Options (juce::WebBrowserComponent::Options::WinWebView2()
                                                     .withUserDataFolder (dataDir)
                                                     .withStatusBarDisabled()
                                                     .withBackgroundColour (juce::Colour (0xff07070fu)));

        if (! juce::WebBrowserComponent::areOptionsSupported (opts))
        {
            unavailableReason = "The browser needs Microsoft Edge WebView2, which this PC doesn't have.\n\n"
                                "It comes with Windows 11 and with Edge on Windows 10. Install Edge, or the "
                                "WebView2 Runtime from microsoft.com, then reopen this tab.";
            importButton.setEnabled (false);
            return;
        }

        browser = std::make_unique<Browser> (opts);
        browser->onInterceptDownload = [this] (juce::String url) -> bool
        {
            downloads.enqueue (url);
            panelOpen = true; downloadsButton.setToggleState (true, juce::dontSendNotification); resized();
            return true;
        };
        browser->onUrlChanged   = [this] (juce::String url)
        {
            currentUrl = url;
            if (! urlBox.hasKeyboardFocus (true)) urlBox.setText (url, juce::dontSendNotification);
            applyMediaLevel();   // the new page's players take the WEB strip's level
        };
        browser->onTitleChanged = [this] (juce::String t) { pageTitle = t; };
        browser->onError        = [this] (juce::String e) { if (onMessage) onMessage ("Page failed to load: " + e); };
        addAndMakeVisible (*browser);
        navigate (kHomeUrl);
    }

    //==========================================================================
    void panelAction (int id, const juce::String& action)
    {
        if (action == "cancel") downloads.cancel (id);
        else if (action == "pause") downloads.pause (id);
        else if (action == "resume") downloads.resume (id);
        else if (action == "remove") downloads.remove (id);
        else if (action == "clear") downloads.clearFinished();
        else if (action == "open") ezdl::DownloadManager::downloadsFolder().revealToUser();
        else if (action == "cancelImport") { if (importJob) importJob->cancel(); }
    }

    void completed (const juce::File& file)
    {
        static const char* audio[] = { ".wav", ".mp3", ".flac", ".aiff", ".aif", ".ogg" };
        for (auto* e : audio)
            if (file.hasFileExtension (e))
            {
                if (onImportFiles) onImportFiles ({ file });
                return;
            }
        if (onMessage) onMessage (file.getFileName() + " saved to Downloads\\PerformLive");
    }

    //==========================================================================
    void installTools()
    {
        if (tools.isInstalling()) return;
        tools.onStatus = [this] (juce::String s) { importStatus = s; panel.refresh (downloads, true, importStatus, -1.0); };
        tools.onFinished = [this] (bool ok, juce::String s)
        {
            importStatus = s;
            if (onMessage) onMessage (s);
            panel.refresh (downloads, false, importStatus, -1.0);
            if (ok && pendingImportUrl.isNotEmpty()) { auto u = pendingImportUrl; pendingImportUrl.clear(); importFromUrl (u); }
        };
        importStatus = "Setting up import tools...";
        panel.refresh (downloads, true, importStatus, -1.0);
        tools.install();
    }

    void importFromUrl (juce::String url)
    {
        url = url.trim();
        if (! ezdl::DownloadManager::isAcceptableUrl (url)) { if (onMessage) onMessage ("Open a page first, then import from it."); return; }

        panelOpen = true; downloadsButton.setToggleState (true, juce::dontSendNotification); resized();

        // A direct file link does not need yt-dlp at all.
        if (ezdl::DownloadManager::looksLikeMediaFile (url)) { downloads.enqueue (url); return; }

        if (getRightsAccepted && ! getRightsAccepted())
        {
            juce::AlertWindow::showOkCancelBox (juce::MessageBoxIconType::WarningIcon,
                "Before you import",
                "Only import audio you have the right to use.\n\n"
                "Many sites, including YouTube, don't permit downloading, and copyright law in most "
                "countries has no personal-use exception. Backing tracks from MultiTracks, Loop Community "
                "or PraiseCharts are licensed for this; a video someone uploaded usually isn't.\n\n"
                "By continuing you confirm you hold the rights to what you import.",
                "I hold the rights", "Cancel", this,
                juce::ModalCallbackFunction::create ([this, url] (int result)
                {
                    if (result != 1) return;
                    if (setRightsAccepted) setRightsAccepted();
                    importFromUrl (url);
                }));
            return;
        }

        if (! ToolsManager::isReady())
        {
            pendingImportUrl = url;
            juce::AlertWindow::showOkCancelBox (juce::MessageBoxIconType::InfoIcon,
                "Set up import tools",
                "Importing audio from web pages uses yt-dlp and ffmpeg, which aren't part of PerformLive.\n\n"
                "They'll be downloaded from their own GitHub release pages (about 160 MB) into your "
                "local app data, checked against their published checksums, and used only by this feature.",
                "Download tools", "Cancel", this,
                juce::ModalCallbackFunction::create ([this] (int result)
                {
                    if (result == 1) installTools(); else pendingImportUrl.clear();
                }));
            return;
        }

        if (importJob && importJob->isRunning()) { if (onMessage) onMessage ("One import at a time -- this one is still running."); return; }

        importJob = std::make_unique<UrlImportJob>();
        importProgress = 0.0;
        importJob->onStatus   = [this] (juce::String s) { importStatus = s; panel.refresh (downloads, true, importStatus, importProgress); };
        importJob->onProgress = [this] (double p) { importProgress = p; panel.refresh (downloads, true, importStatus, importProgress); };
        importJob->onFinished = [this] (bool ok, juce::File f, juce::String s)
        {
            importStatus = s;
            if (onMessage) onMessage (s);
            panel.refresh (downloads, false, importStatus, -1.0);
            if (ok) completed (f);
        };
        importStatus = "Starting...";
        panel.refresh (downloads, true, importStatus, 0.0);
        importJob->start (url, ezdl::DownloadManager::downloadsFolder());
    }

    void timerCallback() override
    {
        // keep the panel's progress numbers moving even when no event fired
        if (panelOpen && downloads.numActive() > 0)
            panel.refresh (downloads, importJob != nullptr && importJob->isRunning(), importStatus, importProgress);
        // players a site created since the last page load (YouTube's next video) get the level too
        if (++mediaTicks % 8 == 0 && (mediaMuted || mediaLevel < 0.999f)) applyMediaLevel();
    }

    //==========================================================================
    //  The downloads / import side panel.
    //==========================================================================
    class Panel : public juce::Component
    {
    public:
        Panel()
        {
            title.setText ("DOWNLOADS", juce::dontSendNotification);
            title.setColour (juce::Label::textColourId, juce::Colour (0xfff2f0ffu));
            title.setFont (juce::Font (juce::FontOptions (13.0f, juce::Font::bold)));
            addAndMakeVisible (title);

            urlBox.setTextToShowWhenEmpty ("Paste a page or file address", juce::Colour (0xff6f7099u));
            urlBox.setColour (juce::TextEditor::backgroundColourId, juce::Colour (0xff14162au));
            urlBox.setColour (juce::TextEditor::outlineColourId, juce::Colour (0xff2b2b4du));
            urlBox.setColour (juce::TextEditor::textColourId, juce::Colour (0xfff2f0ffu));
            urlBox.onReturnKey = [this] { if (onImportUrl) { onImportUrl (urlBox.getText()); urlBox.clear(); } };
            addAndMakeVisible (urlBox);

            importButton.setButtonText ("Import");
            importButton.onClick = [this] { if (onImportUrl) { onImportUrl (urlBox.getText()); urlBox.clear(); } };
            addAndMakeVisible (importButton);

            importStatusLabel.setColour (juce::Label::textColourId, juce::Colour (0xffa3a6ccu));
            importStatusLabel.setFont (juce::Font (juce::FontOptions (11.5f)));
            addAndMakeVisible (importStatusLabel);

            importBar.setColour (juce::ProgressBar::backgroundColourId, juce::Colour (0xff14162au));
            importBar.setColour (juce::ProgressBar::foregroundColourId, juce::Colour (0xff7c5cffu));
            addChildComponent (importBar);

            cancelImportButton.setButtonText ("Cancel");
            cancelImportButton.onClick = [this] { if (onAction) onAction (-1, "cancelImport"); };
            addChildComponent (cancelImportButton);

            toolsButton.setButtonText ("Set up import tools");
            toolsButton.onClick = [this] { if (onInstallTools) onInstallTools(); };
            addAndMakeVisible (toolsButton);

            openFolderButton.setButtonText ("Open folder");
            openFolderButton.onClick = [this] { if (onAction) onAction (-1, "open"); };
            addAndMakeVisible (openFolderButton);

            clearButton.setButtonText ("Clear finished");
            clearButton.onClick = [this] { if (onAction) onAction (-1, "clear"); };
            addAndMakeVisible (clearButton);

            listViewport.setViewedComponent (&listHolder, false);
            listViewport.setScrollBarsShown (true, false);
            addAndMakeVisible (listViewport);
        }

        std::function<void (int, juce::String)> onAction;
        std::function<void()> onInstallTools;
        std::function<void (juce::String)> onImportUrl;

        void refresh (const ezdl::DownloadManager& dm, bool importing, const juce::String& status, double progress)
        {
            importStatusLabel.setText (status, juce::dontSendNotification);
            importBar.setVisible (importing && progress >= 0.0);
            importProgressValue = juce::jlimit (0.0, 1.0, progress);
            cancelImportButton.setVisible (importing);
            toolsButton.setButtonText (ToolsManager::isReady() ? "Import tools ready" : "Set up import tools");
            toolsButton.setEnabled (! ToolsManager::isReady() && ! importing);

            rows.clear();
            int y = 0;
            for (auto& item : dm.list())
            {
                auto* r = rows.add (new Row (*item));
                r->onAction = [this] (int id, juce::String a) { if (onAction) onAction (id, a); };
                r->setBounds (0, y, listHolder.getWidth(), 56);
                listHolder.addAndMakeVisible (r);
                y += 58;
            }
            listHolder.setSize (juce::jmax (10, listViewport.getWidth() - 14), juce::jmax (y, 10));
            resized();
        }

        void paint (juce::Graphics& g) override
        {
            g.fillAll (juce::Colour (0xff14162au));
            g.setColour (juce::Colour (0xff2b2b4du));
            g.fillRect (0, 0, 1, getHeight());
        }

        void resized() override
        {
            auto area = getLocalBounds().reduced (12, 10);
            title.setBounds (area.removeFromTop (22));
            area.removeFromTop (6);

            auto row = area.removeFromTop (30);
            importButton.setBounds (row.removeFromRight (70)); row.removeFromRight (6);
            urlBox.setBounds (row);
            area.removeFromTop (6);

            importStatusLabel.setBounds (area.removeFromTop (18));
            if (importBar.isVisible() || cancelImportButton.isVisible())
            {
                auto pr = area.removeFromTop (22);
                if (cancelImportButton.isVisible()) { cancelImportButton.setBounds (pr.removeFromRight (70)); pr.removeFromRight (6); }
                importBar.setBounds (pr);
                area.removeFromTop (4);
            }
            toolsButton.setBounds (area.removeFromTop (26));
            area.removeFromTop (10);

            auto footer = area.removeFromBottom (26);
            openFolderButton.setBounds (footer.removeFromLeft (100));
            clearButton.setBounds (footer.removeFromRight (110));
            area.removeFromBottom (6);

            listViewport.setBounds (area);
            listHolder.setSize (juce::jmax (10, listViewport.getWidth() - 14), listHolder.getHeight());
            for (auto* r : rows) r->setSize (listHolder.getWidth(), 56);
        }

    private:
        class Row : public juce::Component
        {
        public:
            explicit Row (const ezdl::DownloadItem& it) : item (it)
            {
                addAndMakeVisible (primary);
                addAndMakeVisible (remove);
                remove.setButtonText (juce::String (juce::CharPointer_UTF8 ("\xc3\x97")));
                remove.onClick = [this] { if (onAction) onAction (item.id, "remove"); };
                using S = ezdl::DownloadState;
                switch (item.state)
                {
                    case S::running: primary.setButtonText ("Pause");  primary.onClick = [this] { if (onAction) onAction (item.id, "pause"); }; break;
                    case S::queued:  primary.setButtonText ("Cancel"); primary.onClick = [this] { if (onAction) onAction (item.id, "cancel"); }; break;
                    case S::paused:
                    case S::failed:  primary.setButtonText ("Resume"); primary.onClick = [this] { if (onAction) onAction (item.id, "resume"); }; break;
                    default:         primary.setVisible (false); break;
                }
            }
            std::function<void (int, juce::String)> onAction;

            void resized() override
            {
                auto r = getLocalBounds().reduced (8, 6);
                remove.setBounds (r.removeFromRight (24).removeFromTop (22));
                r.removeFromRight (4);
                primary.setBounds (r.removeFromRight (64).removeFromTop (22));
            }

            void paint (juce::Graphics& g) override
            {
                using S = ezdl::DownloadState;
                auto b = getLocalBounds().toFloat().reduced (0.5f);
                g.setColour (juce::Colour (0xff151527u));
                g.fillRoundedRectangle (b, 8.0f);

                auto r = getLocalBounds().reduced (10, 6).withTrimmedRight (100);
                g.setColour (juce::Colour (0xfff2f0ffu));
                g.setFont (juce::Font (juce::FontOptions (12.5f, juce::Font::bold)));
                g.drawText (item.displayName, r.removeFromTop (18), juce::Justification::centredLeft, true);

                juce::String line;
                const auto done = item.bytesDone.load(), total = item.bytesTotal.load();
                switch (item.state)
                {
                    case S::queued:    line = "Waiting"; break;
                    case S::running:   line = juce::File::descriptionOfSizeInBytes (done)
                                            + (total > 0 ? " of " + juce::File::descriptionOfSizeInBytes (total) : juce::String())
                                            + "  " + juce::File::descriptionOfSizeInBytes ((juce::int64) item.bytesPerSecond.load()) + "/s"; break;
                    case S::paused:    line = "Paused at " + juce::File::descriptionOfSizeInBytes (done); break;
                    case S::done:      line = "Done  " + juce::File::descriptionOfSizeInBytes (done); break;
                    case S::failed:    line = "Failed: " + item.error; break;
                    case S::cancelled: line = "Cancelled"; break;
                }
                g.setColour (item.state == S::failed ? juce::Colour (0xffff3b5cu) : juce::Colour (0xffa3a6ccu));
                g.setFont (juce::Font (juce::FontOptions (11.0f)));
                g.drawText (line, r.removeFromTop (16), juce::Justification::centredLeft, true);

                if (item.state == S::running || item.state == S::paused)
                {
                    auto bar = r.removeFromTop (6).toFloat();
                    g.setColour (juce::Colour (0xff14162au));
                    g.fillRoundedRectangle (bar, 3.0f);
                    g.setColour (juce::Colour (0xff7c5cffu));
                    g.fillRoundedRectangle (bar.withWidth ((float) bar.getWidth() * (float) item.fraction()), 3.0f);
                }
            }

        private:
            const ezdl::DownloadItem& item;
            juce::TextButton primary, remove;
        };

        juce::Label title, importStatusLabel;
        juce::TextEditor urlBox;
        juce::TextButton importButton, toolsButton, openFolderButton, clearButton, cancelImportButton;
        double importProgressValue { 0.0 };
        juce::ProgressBar importBar { importProgressValue };
        juce::Viewport listViewport;
        juce::Component listHolder;
        juce::OwnedArray<Row> rows;
    };

    //==========================================================================
    std::unique_ptr<Browser> browser;
    juce::String unavailableReason, currentUrl, pageTitle;
    float mediaLevel { 1.0f };
    bool  mediaMuted { false };
    int   mediaTicks { 0 };

    juce::TextButton backButton, forwardButton, reloadButton, homeButton, importButton, downloadsButton;
    juce::TextEditor urlBox;
    juce::Label audioNote;

    Panel panel;
    bool panelOpen { false };

    ezdl::DownloadManager downloads;
    ToolsManager tools;
    std::unique_ptr<UrlImportJob> importJob;
    juce::String importStatus, pendingImportUrl;
    double importProgress { -1.0 };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (BrowserTab)
};

} // namespace ezweb
