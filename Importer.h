// ============================================================================
//  Importer.h — imports a source audio file into a Library (Milestone 16-T2,
//  PRODUCT_REQUIREMENTS.md §12; MILESTONE_16_ARCHITECTURE.md's Implementation
//  Plan, task T2).
//
//  JUCE-side, depends on Library.h + EzDSP.h. Reuses ezdsp::analyze()
//  (proven since Milestone 1, already used by loadLayer()/loadVoiceClip() in
//  Main.cpp) for BPM detection rather than reinventing analysis here --
//  metadata extraction is "read the file once, run the same analysis every
//  clip load already runs," not a separate subsystem.
//
//  Threading: importFile() itself is synchronous (hash + copy + decode +
//  analyze, all ordinary blocking file/CPU work -- never called from
//  render()). MILESTONE_16_ARCHITECTURE.md's resolved Threading Model calls
//  for background import via juce::ThreadPool for multi-file/batch imports;
//  this function's shape (no shared mutable state beyond the Library it's
//  given, all locals) is exactly what a ThreadPool::Job would call per file --
//  SessionComponent (Main.cpp) is the only place that owns a ThreadPool and
//  marshals completion back to the message thread, matching the
//  MidiActionRouter precedent (Milestone 14) of never touching UI/session
//  state off the message thread.
// ============================================================================
#pragma once
#include <JuceHeader.h>
#include "Library.h"
#include "EzDSP.h"

namespace eximport
{

// ============================================================================
//  SECURITY: audio files are UNTRUSTED input -- a user drags in whatever they
//  were sent, and a container's HEADER fields are attacker-controlled even
//  when the payload is short. A WAV/RF64 header can legally declare a `data`
//  chunk far larger than the file (JUCE takes lengthInSamples from the
//  declared size, it does not cross-check the real file size), and a fmt
//  chunk can declare an absurd channel count or sample rate.
//
//  Unchecked, those three fields caused, from ONE drag-and-drop:
//    - (int) lengthInSamples truncating to a NEGATIVE size -> AudioBuffer's
//      byte-size arithmetic wraps -> either a 44-byte allocation reported
//      as huge, or a
//      failed allocation leaving channels[] null -> null deref / a
//      std::vector::assign with last < first -> length_error -> terminate.
//    - a legitimate-looking 2e9 samples * 2ch * 4 bytes -> a 16 GB malloc.
//    - numChannels sign-extending (JUCE reads it via a SIGNED readShort) to
//      a huge unsigned -> channel-list size wrap -> null deref.
//
//  So: every reader is validated HERE, once, before any cast or allocation,
//  and every call site uses this one function. Limits are far above any real
//  musical material and far below anything that can overflow an int when
//  multiplied by a channel count or a sample rate.
// ============================================================================
constexpr juce::int64 kMaxDecodeSamples = 192000LL * 60 * 30;   // 30 min @ 192kHz
constexpr int         kMaxChannels      = 8;
constexpr double      kMinSampleRate    = 1000.0;
constexpr double      kMaxSampleRate    = 384000.0;

// Returns false (with a human-readable reason) for any file whose declared
// geometry we refuse to allocate for. Callers must bail out -- never cast
// lengthInSamples/numChannels before this passes.
inline bool readerGeometryIsSane (const juce::AudioFormatReader& reader, juce::String& reasonOut)
{
    if (reader.lengthInSamples <= 0)
    {
        reasonOut = "declares no audio data";
        return false;
    }
    if (reader.lengthInSamples > kMaxDecodeSamples)
    {
        reasonOut = "declares an implausible length (" + juce::String (reader.lengthInSamples) + " samples)";
        return false;
    }
    if (reader.numChannels < 1 || reader.numChannels > (unsigned int) kMaxChannels)
    {
        reasonOut = "declares an unsupported channel count (" + juce::String ((juce::int64) reader.numChannels) + ")";
        return false;
    }
    if (! (reader.sampleRate >= kMinSampleRate && reader.sampleRate <= kMaxSampleRate))
    {
        reasonOut = "declares an out-of-range sample rate (" + juce::String (reader.sampleRate) + " Hz)";
        return false;
    }
    return true;
}

// Owner: "when I import files into the app u change the name... keep the
// file names." Copies into Samples/ keep the ORIGINAL filename now; a
// same-name collision gets a " (2)"-style suffix instead of the old
// assetId-prefix scheme ("<uuid>_<name>.wav"), which surfaced as the
// visible clip name everywhere the file's own name is displayed. The
// AssetId remains the permanent identity -- it lives in the library index,
// not the filename, so nothing else changes.
inline juce::File uniqueLibraryFileFor (const juce::File& samplesDir, const juce::File& sourceFile)
{
    auto dest = samplesDir.getChildFile (sourceFile.getFileName());
    int n = 2;
    while (dest.exists())
        dest = samplesDir.getChildFile (sourceFile.getFileNameWithoutExtension()
                                          + " (" + juce::String (n++) + ")" + sourceFile.getFileExtension());
    return dest;
}

struct ImportResult
{
    bool success       { false };
    bool wasDuplicate  { false };   // true: an entry with this content hash already existed -- nothing new was copied
    ezlibrary::LibraryEntry entry;
    juce::String errorMessage;      // set only when success == false
};

// Imports one file into `library`:
//   1. Hash the source file's raw bytes (Library::hashFile).
//   2. If an entry with that hash already exists, return it (wasDuplicate =
//      true) -- no copy, no new entry. This is the whole of "dedup on
//      import" (MILESTONE_16_ARCHITECTURE.md's importer design): identical
//      bytes are recognized before ever touching the filesystem again.
//   3. Otherwise decode the file (juce::AudioFormatManager, the same one
//      loadLayer()/loadVoiceClip() already use) to confirm it's real,
//      readable audio, then either:
//        - copyIntoLibrary == false (the default -- bug report: "the files
//          imported should read from the default location... not copy into
//          the app... that should be asked when saving"): register the
//          entry as EXTERNAL, pointing straight at sourceFile. Nothing is
//          copied; the original file must stay where it is to keep playing.
//        - copyIntoLibrary == true: the original pre-existing behavior --
//          copy the ORIGINAL bytes (never re-encoded, per PRD §13) into
//          library.getRoot()/Samples/, entry is NOT external. Used by the
//          project Save flow (copyExternalEntryIntoLibrary() below performs
//          the same copy for an entry that was already registered external).
// Never called from render() -- ordinary blocking file I/O, exactly like
// loadLayer()'s own existing file-loading code.
inline ImportResult importFile (ezlibrary::Library& library, const juce::File& sourceFile,
                                 juce::AudioFormatManager& formatManager,
                                 const juce::String& category = {}, const juce::String& tags = {},
                                 bool copyIntoLibrary = false)
{
    ImportResult result;

    if (! sourceFile.existsAsFile())
    {
        result.errorMessage = "File does not exist: " + sourceFile.getFullPathName();
        return result;
    }

    const juce::String hash = ezlibrary::Library::hashFile (sourceFile);
    if (hash.isEmpty())
    {
        result.errorMessage = "Could not read file: " + sourceFile.getFullPathName();
        return result;
    }

    if (auto existing = library.findByHash (hash))
    {
        result.success      = true;
        result.wasDuplicate = true;
        result.entry         = *existing;
        return result;
    }

    std::unique_ptr<juce::AudioFormatReader> reader (formatManager.createReaderFor (sourceFile));
    if (reader == nullptr)
    {
        result.errorMessage = "Unrecognized or undecodable audio format: " + sourceFile.getFileName();
        return result;
    }
    // SECURITY: refuse absurd declared geometry BEFORE any cast/allocation
    // (see readerGeometryIsSane's own comment).
    {
        juce::String reason;
        if (! readerGeometryIsSane (*reader, reason))
        {
            result.errorMessage = sourceFile.getFileName() + ": " + reason;
            return result;
        }
    }

    // Keyed by a fresh AssetId so two different source files that happen to
    // share a filename can never collide -- the AssetId, not the filename,
    // is the permanent identity, whether copied or referenced in place.
    const juce::String assetId = juce::Uuid().toString();

    ezlibrary::LibraryEntry entry;
    entry.assetId      = assetId;
    entry.contentHash  = hash;
    entry.name         = sourceFile.getFileNameWithoutExtension();
    entry.category     = category;
    entry.tags         = tags;
    entry.importedAtMs = (int64_t) juce::Time::getCurrentTime().toMilliseconds();

    if (copyIntoLibrary)
    {
        auto samplesDir = library.getRoot().getChildFile ("Samples");
        samplesDir.createDirectory();
        auto destFile = uniqueLibraryFileFor (samplesDir, sourceFile);   // owner: keep the original file name

        if (! sourceFile.copyFileTo (destFile))
        {
            result.errorMessage = "Could not copy file into the library: " + sourceFile.getFileName();
            return result;
        }

        entry.relativePath = destFile.getRelativePathFrom (library.getRoot());
        entry.external      = false;
    }
    else
    {
        entry.external     = true;
        entry.externalPath = sourceFile.getFullPathName();
    }

    // Metadata extraction: mono downmix of channel 0, matching loadLayer()'s
    // own existing analysis convention (Main.cpp) -- not a new convention.
    //
    // Owner bug report ("can you increase the speed when uploading files?
    // since it's referencing and not copying it should be faster"): the
    // slow part was never a copy (referencing has been the default since
    // the import-in-place change) -- it was decoding the ENTIRE file just
    // to detect its BPM, which on a several-minute mp3 dominates the whole
    // import. Tempo is stable across a track, so analysing the first 30
    // seconds gives the same detection at a fraction of the decode cost;
    // playback later still decodes the full file exactly as before.
    // Embedded (DAW-authored) tempo beats detection when the file carries
    // one -- WAV ACID chunks and bpm/TBPM tags, mirroring Main.cpp's own
    // embeddedTempoFrom(). Also skips the decode+analysis entirely in that
    // case, which makes those imports near-instant.
    double embeddedBpm = 0.0;
    for (const char* key : { "acid tempo", "bpm", "BPM", "TBPM", "tempo" })
    {
        const auto v = reader->metadataValues.getValue (key, {});
        if (v.isNotEmpty())
        {
            const double bpm = v.getDoubleValue();
            if (bpm > 20.0 && bpm < 999.0) { embeddedBpm = bpm; break; }
        }
    }

    const int lengthSamples = (int) reader->lengthInSamples;
    if (embeddedBpm > 0.0)
    {
        entry.detectedBpm = embeddedBpm;
    }
    else if (lengthSamples > 0)
    {
        // sampleRate is validated in range by readerGeometryIsSane above, so
        // this 30s cap can no longer be defeated by an absurd declared rate.
        const int analysisSamples = (int) juce::jmin ((juce::int64) lengthSamples,
                                                       (juce::int64) (reader->sampleRate * 30.0));
        juce::AudioBuffer<float> temp ((int) reader->numChannels, analysisSamples);
        reader->read (&temp, 0, analysisSamples, 0, true, true);
        auto analysis = ezdsp::analyze (temp.getReadPointer (0), analysisSamples, reader->sampleRate);
        entry.detectedBpm = analysis.bpm;
    }

    library.upsert (entry);
    library.save();

    result.success = true;
    result.entry   = entry;
    return result;
}

// ============================================================================
//  Background-import split (owner bug: "app freezes when trying to upload
//  multiple files into library"). importFile() above is synchronous and
//  touches the Library, so a multi-file loop over it must either freeze the
//  message thread (the reported bug) or race the Library from a worker.
//  These two halves split it at exactly the thread boundary the header
//  comment above always called for:
//
//    analyzeImportFile()    -- ALL the expensive work (hash the bytes, open
//                              a reader, embedded-tempo check, capped decode
//                              + BPM analysis) with NO Library access at
//                              all: safe on any thread. AudioFormatManager::
//                              createReaderFor is safe for this (it's what
//                              JUCE's own thumbnail/background threads do).
//    registerAnalyzedImport() -- the Library half (dedup by hash, build the
//                              entry, upsert + save): cheap, message thread
//                              only, same rules as every other Library call.
//
//  Result of running both back-to-back on the message thread is behaviorally
//  identical to importFile(copyIntoLibrary=false) -- same hash, same dedup,
//  same external-reference entry, same tempo logic.
// ============================================================================
struct AnalyzedImport
{
    juce::File   sourceFile;
    bool         readable { false };   // false: missing/unreadable/undecodable -- errorMessage says which
    juce::String errorMessage;
    juce::String contentHash;
    double       detectedBpm { 0.0 };  // embedded tempo when present, else capped-decode analysis
};

inline AnalyzedImport analyzeImportFile (const juce::File& sourceFile, juce::AudioFormatManager& formatManager)
{
    AnalyzedImport out;
    out.sourceFile = sourceFile;

    if (! sourceFile.existsAsFile())
    {
        out.errorMessage = "File does not exist: " + sourceFile.getFullPathName();
        return out;
    }

    out.contentHash = ezlibrary::Library::hashFile (sourceFile);
    if (out.contentHash.isEmpty())
    {
        out.errorMessage = "Could not read file: " + sourceFile.getFullPathName();
        return out;
    }

    std::unique_ptr<juce::AudioFormatReader> reader (formatManager.createReaderFor (sourceFile));
    if (reader == nullptr)
    {
        out.errorMessage = "Unrecognized or undecodable audio format: " + sourceFile.getFileName();
        return out;
    }
    // SECURITY: same geometry gate as importFile() -- this one runs on a
    // background thread, where an unchecked 16 GB allocation would take the
    // whole process down with it.
    {
        juce::String reason;
        if (! readerGeometryIsSane (*reader, reason))
        {
            out.errorMessage = sourceFile.getFileName() + ": " + reason;
            return out;
        }
    }

    // same embedded-tempo-beats-detection logic as importFile() above
    double embeddedBpm = 0.0;
    for (const char* key : { "acid tempo", "bpm", "BPM", "TBPM", "tempo" })
    {
        const auto v = reader->metadataValues.getValue (key, {});
        if (v.isNotEmpty())
        {
            const double bpm = v.getDoubleValue();
            if (bpm > 20.0 && bpm < 999.0) { embeddedBpm = bpm; break; }
        }
    }

    const int lengthSamples = (int) reader->lengthInSamples;
    if (embeddedBpm > 0.0)
    {
        out.detectedBpm = embeddedBpm;
    }
    else if (lengthSamples > 0)
    {
        // sampleRate is validated in range by readerGeometryIsSane above, so
        // this 30s cap can no longer be defeated by an absurd declared rate.
        const int analysisSamples = (int) juce::jmin ((juce::int64) lengthSamples,
                                                       (juce::int64) (reader->sampleRate * 30.0));
        juce::AudioBuffer<float> temp ((int) reader->numChannels, analysisSamples);
        reader->read (&temp, 0, analysisSamples, 0, true, true);
        auto analysis = ezdsp::analyze (temp.getReadPointer (0), analysisSamples, reader->sampleRate);
        out.detectedBpm = analysis.bpm;
    }

    out.readable = true;
    return out;
}

// Message thread only -- see the split comment above.
inline ImportResult registerAnalyzedImport (ezlibrary::Library& library, const AnalyzedImport& analyzed,
                                             const juce::String& category = {}, const juce::String& tags = {})
{
    ImportResult result;

    if (! analyzed.readable)
    {
        result.errorMessage = analyzed.errorMessage;
        return result;
    }

    if (auto existing = library.findByHash (analyzed.contentHash))
    {
        result.success      = true;
        result.wasDuplicate = true;
        result.entry         = *existing;
        return result;
    }

    ezlibrary::LibraryEntry entry;
    entry.assetId      = juce::Uuid().toString();
    entry.contentHash  = analyzed.contentHash;
    entry.name         = analyzed.sourceFile.getFileNameWithoutExtension();
    entry.category     = category;
    entry.tags         = tags;
    entry.importedAtMs = (int64_t) juce::Time::getCurrentTime().toMilliseconds();
    entry.external      = true;
    entry.externalPath  = analyzed.sourceFile.getFullPathName();
    entry.detectedBpm   = analyzed.detectedBpm;

    library.upsert (entry);
    library.save();

    result.success = true;
    result.entry   = entry;
    return result;
}

// The deferred half of importFile()'s copyIntoLibrary path: given an entry
// that was imported EXTERNAL (external == true), copies its original file
// (externalPath) into library.getRoot()/Samples/ now, and flips it to a
// normal internal entry -- same copy code importFile() itself would have
// run had copyIntoLibrary been true at import time. Called from the project
// Save flow when the user opts to make referenced-in-place assets portable.
// Returns false (entry left untouched) if the original file is missing --
// same "fail cleanly, never silently substitute" precedent Library::resolve()
// already follows.
inline bool copyExternalEntryIntoLibrary (ezlibrary::Library& library, ezlibrary::LibraryEntry& entry)
{
    if (! entry.external) return true;   // already internal -- nothing to do

    juce::File sourceFile (entry.externalPath);
    if (! sourceFile.existsAsFile()) return false;

    auto samplesDir = library.getRoot().getChildFile ("Samples");
    samplesDir.createDirectory();
    auto destFile = uniqueLibraryFileFor (samplesDir, sourceFile);   // owner: keep the original file name

    if (! sourceFile.copyFileTo (destFile)) return false;

    entry.relativePath = destFile.getRelativePathFrom (library.getRoot());
    entry.external      = false;
    entry.externalPath  = {};

    library.upsert (entry);
    library.save();
    return true;
}

} // namespace eximport
