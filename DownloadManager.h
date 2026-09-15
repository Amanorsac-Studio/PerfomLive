// ============================================================================
//  DownloadManager.h -- a queue of file downloads with resume, progress,
//  cancel, and a hand-off to the Library when each one lands.
//
//  Built on juce::URL/WebInputStream directly rather than
//  juce::URL::DownloadTask, because on Windows that task is a plain
//  stream-copy with no Range support: a dropped connection on a 300 MB
//  multitrack would start again from byte zero. Here a partial file is kept
//  as  <name>.part  and resumed with  Range: bytes=N-  -- the same discipline
//  the Store client uses.
//
//  THREADING. One worker per active download (at most kMaxParallel) on a
//  juce::ThreadPool. Every callback to the outside world is marshalled to the
//  message thread, and the manager is WeakReferenceable so a completion that
//  lands after the UI is gone is dropped, not dereferenced.
//
//  SAFETY. A URL arrives from a web page the user was browsing, which is
//  untrusted by definition (SECURITY.md). So: only http/https; the filename
//  is derived from the URL or Content-Disposition and then SANITISED to a
//  plain basename (no separators, no reserved names, no traversal); and the
//  file is written under the user's own Downloads folder and nowhere else.
//  Downloaded bytes are data, never executed -- this app never launches
//  anything it downloads.
// ============================================================================
#pragma once

#include <JuceHeader.h>
#include <atomic>
#include <functional>
#include <map>
#include <memory>
#include <vector>

namespace ezdl
{

enum class DownloadState { queued, running, paused, done, failed, cancelled };

struct DownloadItem
{
    int           id { 0 };
    juce::String  url;
    juce::String  displayName;      // what the list shows
    juce::File    target;           // final file
    juce::File    partFile;         // in-progress file
    DownloadState state { DownloadState::queued };
    std::atomic<juce::int64> bytesDone { 0 };
    std::atomic<juce::int64> bytesTotal { -1 };   // -1 = unknown
    std::atomic<double>      bytesPerSecond { 0.0 };
    juce::String  error;
    juce::uint32  startedAtMs { 0 };
    bool          importWhenDone { true };

    double fraction() const
    {
        const auto t = bytesTotal.load();
        return t > 0 ? (double) bytesDone.load() / (double) t : 0.0;
    }
};

//==============================================================================
class DownloadManager
{
public:
    static constexpr int kMaxParallel = 3;

    DownloadManager() : pool (kMaxParallel) {}

    ~DownloadManager()
    {
        cancelAll();
        pool.removeAllJobs (true, 4000);
        masterReference.clear();
    }

    /** Where files land. Created on demand. */
    static juce::File downloadsFolder()
    {
        auto dir = juce::File::getSpecialLocation (juce::File::userHomeDirectory)
                       .getChildFile ("Downloads").getChildFile ("PerformLive");
        dir.createDirectory();
        return dir;
    }

    //==========================================================================
    //  observers (message thread)
    //==========================================================================
    std::function<void()>                         onListChanged;   // any add/remove/state change
    std::function<void (const DownloadItem&)>     onCompleted;     // a file finished on disk
    std::function<void (juce::String)>            onMessage;       // toast-worthy text

    //==========================================================================
    //  queue
    //==========================================================================

    /** Returns the new item's id, or -1 if the URL was refused. */
    int enqueue (const juce::String& urlText, const juce::String& suggestedName = {})
    {
        const juce::String url = urlText.trim();
        if (! isAcceptableUrl (url))
        {
            if (onMessage) onMessage ("Only http and https links can be downloaded.");
            return -1;
        }

        // Same URL already queued or running: don't start a second copy.
        for (auto& it : items)
            if (it->url == url && (it->state == DownloadState::queued || it->state == DownloadState::running))
            {
                if (onMessage) onMessage ("That file is already downloading.");
                return it->id;
            }

        auto item = std::make_shared<DownloadItem>();
        item->id = nextId++;
        item->url = url;

        juce::String name = suggestedName.isNotEmpty() ? suggestedName : nameFromUrl (url);
        name = sanitiseFileName (name);
        if (name.isEmpty()) name = "download";

        item->displayName = name;
        item->target   = uniqueTarget (downloadsFolder().getChildFile (name));
        item->partFile = partFileFor (item->target);

        items.push_back (item);
        notifyList();
        pump();
        return item->id;
    }

    void cancel (int id)
    {
        auto it = find (id);
        if (it == nullptr) return;

        if (it->state == DownloadState::running)
        {
            flag (cancelFlags, id).store (true);
        }
        else if (it->state == DownloadState::queued || it->state == DownloadState::paused
              || it->state == DownloadState::failed)
        {
            it->state = DownloadState::cancelled;
            it->partFile.deleteFile();
            notifyList();
        }
    }

    void pause (int id)
    {
        if (auto it = find (id))
            if (it->state == DownloadState::running)
                flag (pauseFlags, id).store (true);
    }

    void resume (int id)
    {
        if (auto it = find (id))
            if (it->state == DownloadState::paused || it->state == DownloadState::failed)
            {
                it->state = DownloadState::queued;
                it->error.clear();
                notifyList();
                pump();
            }
    }

    void remove (int id)
    {
        cancel (id);
        items.erase (std::remove_if (items.begin(), items.end(),
                                     [id] (auto& p) { return p->id == id && p->state != DownloadState::running; }),
                     items.end());
        notifyList();
    }

    void clearFinished()
    {
        items.erase (std::remove_if (items.begin(), items.end(), [] (auto& p)
        {
            return p->state == DownloadState::done || p->state == DownloadState::cancelled
                || p->state == DownloadState::failed;
        }), items.end());
        notifyList();
    }

    void cancelAll()
    {
        for (auto& it : items)
            if (it->state == DownloadState::running || it->state == DownloadState::queued)
                cancel (it->id);
    }

    const std::vector<std::shared_ptr<DownloadItem>>& list() const { return items; }

    int numActive() const
    {
        int n = 0;
        for (auto& it : items)
            if (it->state == DownloadState::running || it->state == DownloadState::queued) ++n;
        return n;
    }

    //==========================================================================
    //  what counts as a downloadable link -- used by the browser to decide
    //  whether to intercept a navigation instead of displaying it
    //==========================================================================
    static bool looksLikeMediaFile (const juce::String& url)
    {
        const auto path = juce::URL (url).getSubPath().toLowerCase().upToFirstOccurrenceOf ("?", false, false);
        static const char* exts[] = { ".wav", ".mp3", ".flac", ".aiff", ".aif", ".ogg", ".m4a",
                                      ".zip", ".plpack" };
        for (auto* e : exts)
            if (path.endsWith (e)) return true;
        return false;
    }

    static bool isAcceptableUrl (const juce::String& url)
    {
        if (url.length() < 8 || url.length() > 4096) return false;
        if (! (url.startsWithIgnoreCase ("http://") || url.startsWithIgnoreCase ("https://"))) return false;
        if (url.containsChar ('\n') || url.containsChar ('\r') || url.containsChar (' ')) return false;
        return true;
    }

    /**
     * Reduces any string to a safe basename. Refuses, rather than repairs,
     * anything that could be a path: this mirrors PackInstaller's rules, for
     * the same reason -- a name that needs fixing is hostile, not a typo.
     */
    static juce::String sanitiseFileName (juce::String name)
    {
        name = name.fromLastOccurrenceOf ("/", false, false)
                   .fromLastOccurrenceOf ("\\", false, false)
                   .trim();

        juce::String out;
        for (auto c : name)
        {
            if (c < 32 || c == '<' || c == '>' || c == ':' || c == '"' || c == '|' || c == '?' || c == '*'
                || c == '/' || c == '\\')
                continue;
            out += juce::String::charToString (c);
        }
        out = out.trimCharactersAtEnd (". ").trim();
        if (out == "." || out == "..") return {};

        const auto stem = out.upToFirstOccurrenceOf (".", false, false).toUpperCase();
        static const char* reserved[] = { "CON", "PRN", "AUX", "NUL",
                                          "COM1","COM2","COM3","COM4","COM5","COM6","COM7","COM8","COM9",
                                          "LPT1","LPT2","LPT3","LPT4","LPT5","LPT6","LPT7","LPT8","LPT9" };
        for (auto* r : reserved) if (stem == r) return {};

        return out.substring (0, 180);
    }

private:
    //==========================================================================
    std::shared_ptr<DownloadItem> find (int id)
    {
        for (auto& it : items) if (it->id == id) return it;
        return nullptr;
    }

    static std::atomic<bool>& flag (std::map<int, std::unique_ptr<std::atomic<bool>>>& m, int id)
    {
        auto& p = m[id];
        if (p == nullptr) p = std::make_unique<std::atomic<bool>> (false);
        return *p;
    }

    static juce::String nameFromUrl (const juce::String& url)
    {
        auto path = juce::URL (url).getSubPath().upToFirstOccurrenceOf ("?", false, false);
        return juce::URL::removeEscapeChars (path.fromLastOccurrenceOf ("/", false, false));
    }

    static juce::File partFileFor (const juce::File& target)
    {
        return target.getSiblingFile (target.getFileName() + ".part");
    }

    static juce::File uniqueTarget (juce::File f)
    {
        if (! f.exists() && ! partFileFor (f).exists()) return f;
        const auto base = f.getFileNameWithoutExtension();
        const auto ext  = f.getFileExtension();
        for (int i = 2; i < 1000; ++i)
        {
            auto candidate = f.getSiblingFile (base + " (" + juce::String (i) + ")" + ext);
            if (! candidate.exists() && ! partFileFor (candidate).exists()) return candidate;
        }
        return f;
    }

    void notifyList() { if (onListChanged) onListChanged(); }

    /** Starts as many queued items as the parallel limit allows. */
    void pump()
    {
        int running = 0;
        for (auto& it : items) if (it->state == DownloadState::running) ++running;

        for (auto& it : items)
        {
            if (running >= kMaxParallel) break;
            if (it->state != DownloadState::queued) continue;
            it->state = DownloadState::running;
            it->startedAtMs = juce::Time::getMillisecondCounter();
            flag (cancelFlags, it->id).store (false);
            flag (pauseFlags,  it->id).store (false);
            ++running;
            startWorker (it);
        }
        notifyList();
    }

    enum class Outcome { done, paused, cancelled, failed };

    void startWorker (std::shared_ptr<DownloadItem> item)
    {
        juce::WeakReference<DownloadManager> safe (this);
        const int id = item->id;

        // The flags outlive the worker: they are owned by the manager and only
        // ever read here through raw pointers captured before the job starts.
        std::atomic<bool>* cancelFlag = &flag (cancelFlags, id);
        std::atomic<bool>* pauseFlag  = &flag (pauseFlags,  id);

        pool.addJob ([safe, item, cancelFlag, pauseFlag]
        {
            const auto result = runDownload (item,
                [cancelFlag, pauseFlag]() -> int
                {
                    if (cancelFlag->load()) return 2;
                    if (pauseFlag->load())  return 1;
                    return 0;
                },
                [safe]
                {
                    juce::MessageManager::callAsync ([safe] { if (auto* s = safe.get()) s->notifyList(); });
                });

            juce::MessageManager::callAsync ([safe, item, result]
            {
                auto* s = safe.get();
                if (s == nullptr) return;

                switch (result)
                {
                    case Outcome::done:
                        item->state = DownloadState::done;
                        item->bytesDone = item->target.getSize();
                        if (s->onMessage) s->onMessage ("Downloaded " + item->displayName);
                        if (s->onCompleted) s->onCompleted (*item);
                        break;

                    case Outcome::paused:
                        item->state = DownloadState::paused;
                        break;

                    case Outcome::cancelled:
                        item->state = DownloadState::cancelled;
                        item->partFile.deleteFile();
                        break;

                    case Outcome::failed:
                        item->state = DownloadState::failed;
                        if (s->onMessage)
                            s->onMessage ("Download failed: " + item->displayName
                                          + (item->error.isNotEmpty() ? " -- " + item->error : juce::String()));
                        break;
                }
                s->pump();
            });
        });
    }

    /** Pool thread. `shouldStop` returns 0 continue, 1 pause, 2 cancel. */
    static Outcome runDownload (std::shared_ptr<DownloadItem> item,
                                std::function<int()> shouldStop,
                                std::function<void()> progressTick)
    {
        juce::int64 have = item->partFile.existsAsFile() ? item->partFile.getSize() : 0;

        juce::String headers;
        if (have > 0) headers << "Range: bytes=" << have << "-\r\n";

        int status = 0;
        juce::StringPairArray responseHeaders;
        auto options = juce::URL::InputStreamOptions (juce::URL::ParameterHandling::inAddress)
                           .withExtraHeaders (headers)
                           .withConnectionTimeoutMs (20000)
                           .withStatusCode (&status)
                           .withResponseHeaders (&responseHeaders)
                           .withNumRedirectsToFollow (8);

        std::unique_ptr<juce::InputStream> in (juce::URL (item->url).createInputStream (options));
        if (in == nullptr) { item->error = "couldn't connect"; return Outcome::failed; }
        if (status >= 400)  { item->error = "server said " + juce::String (status); return Outcome::failed; }

        // A server that ignores Range answers 200 with the whole body; anything
        // already on disk must be discarded or the result is a corrupt splice.
        const bool resuming = (status == 206 && have > 0);
        if (! resuming) { have = 0; item->partFile.deleteFile(); }

        // Size: Content-Range on resume, Content-Length otherwise.
        juce::int64 total = -1;
        {
            const auto cr = responseHeaders.getValue ("Content-Range", {});
            const auto cl = responseHeaders.getValue ("Content-Length", {});
            if (cr.containsChar ('/'))  total = cr.fromLastOccurrenceOf ("/", false, false).trim().getLargeIntValue();
            else if (cl.isNotEmpty())   total = cl.trim().getLargeIntValue() + have;
        }
        item->bytesTotal = total > 0 ? total : -1;
        item->bytesDone  = have;

        // Honour a Content-Disposition filename when the URL gave nothing usable.
        if (! item->target.getFileName().containsChar ('.'))
        {
            const auto cd = responseHeaders.getValue ("Content-Disposition", {});
            auto fn = cd.fromFirstOccurrenceOf ("filename=", false, true)
                        .upToFirstOccurrenceOf (";", false, false).trim().unquoted();
            const auto safeName = sanitiseFileName (fn);
            if (safeName.isNotEmpty())
            {
                item->displayName = safeName;
                item->target   = uniqueTarget (item->target.getSiblingFile (safeName));
                item->partFile = partFileFor (item->target);
            }
        }

        std::unique_ptr<juce::FileOutputStream> out (item->partFile.createOutputStream());
        if (out == nullptr) { item->error = "couldn't write to Downloads"; return Outcome::failed; }
        if (resuming) out->setPosition (have);
        else          { out->setPosition (0); out->truncate(); }

        juce::HeapBlock<char> buffer (256 * 1024);
        juce::int64 windowBytes = 0;
        auto windowStart = juce::Time::getMillisecondCounter();
        auto lastTick = windowStart;

        for (;;)
        {
            const int stop = shouldStop();
            if (stop == 2) { out->flush(); return Outcome::cancelled; }
            if (stop == 1) { out->flush(); return Outcome::paused; }

            const int got = in->read (buffer, 256 * 1024);
            if (got <= 0) break;

            if (! out->write (buffer, (size_t) got)) { item->error = "disk full?"; return Outcome::failed; }

            item->bytesDone += got;
            windowBytes += got;

            const auto now = juce::Time::getMillisecondCounter();
            if (now - lastTick > 200)
            {
                const auto dt = juce::jmax ((juce::uint32) 1, now - windowStart);
                item->bytesPerSecond = (double) windowBytes * 1000.0 / (double) dt;
                if (dt > 3000) { windowStart = now; windowBytes = 0; }
                lastTick = now;
                progressTick();
            }

            // Never write past a declared total; a server that streams forever
            // must not fill the disk.
            if (item->bytesTotal > 0 && item->bytesDone > item->bytesTotal + (1 << 20))
            { item->error = "server sent more than it promised"; return Outcome::failed; }
        }

        out->flush();
        out.reset();

        if (item->bytesTotal > 0 && item->bytesDone < item->bytesTotal)
        { item->error = "connection dropped -- resume to continue"; return Outcome::paused; }

        if (item->partFile.getSize() == 0)
        { item->error = "empty file"; item->partFile.deleteFile(); return Outcome::failed; }

        item->target.deleteFile();
        if (! item->partFile.moveFileTo (item->target)) { item->error = "couldn't finalise file"; return Outcome::failed; }
        return Outcome::done;
    }

    //==========================================================================
    juce::ThreadPool pool;
    std::vector<std::shared_ptr<DownloadItem>> items;
    std::map<int, std::unique_ptr<std::atomic<bool>>> cancelFlags, pauseFlags;
    int nextId { 1 };

    JUCE_DECLARE_WEAK_REFERENCEABLE (DownloadManager)
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (DownloadManager)
};

} // namespace ezdl
