// ============================================================================
//  Library.h — the Library/Explorer/Store subsystem's asset model
//  (Milestone 16, PRODUCT_REQUIREMENTS.md §12; resolves ARCHITECTURE.md's
//  Architecture Decision Pending #9).
//
//  JUCE-side (juce::File/juce::SHA256/juce::Uuid/juce::var) -- not JUCE-free,
//  since this is an app/data-layer concern, matching Decision #5's own
//  established JUCE-free-engine / JUCE-side-persistence split. Session.h/
//  Deck.h/Mixer.h/OneShotVoice.h/Metronome.h remain completely untouched --
//  this subsystem only ever produces a juce::File for the existing,
//  unchanged loadLayer()/loadVoiceClip() functions to consume.
//
//  Asset identity (Architecture Decision Pending #9's resolved hybrid
//  model): a permanent AssetId (juce::Uuid, assigned once at import) is the
//  only thing ever stored to reference an asset. A content hash (SHA-256 of
//  the ORIGINAL FILE BYTES, not decoded PCM -- faster, and matches "stored
//  as uploaded, never re-encoded," PRODUCT_REQUIREMENTS.md §13) supports
//  duplicate detection and integrity verification. A relative path from the
//  owning Library root is the fast, everyday resolution path.
// ============================================================================
#pragma once
#include <JuceHeader.h>
#include <optional>
#include <vector>

namespace ezlibrary
{

// ============================================================================
//  SECURITY: true for a path that would make the OS talk to another machine
//  (Windows UNC "\\host\share", its forward-slash form, and the WebDAV
//  variants that tunnel the same thing over HTTP).
//
//  Why this matters even though we only ever READ these paths: on Windows,
//  simply calling existsAsFile() or opening a reader on \\attacker\share\x
//  performs an SMB (or WebDAV) handshake, which transmits the logged-in
//  user's NTLMv2 challenge/response to whoever controls that host --
//  harvestable for offline cracking or relay. A project or library file
//  received from someone else could therefore leak the user's credentials
//  the moment it was opened, with no further interaction. Paths like this
//  have no legitimate use in this app: samples live on the user's own disk.
// ============================================================================
inline bool isRemotePath (const juce::String& path)
{
    const auto p = path.trim();
    return p.startsWith ("\\\\") || p.startsWith ("//")
            || p.startsWithIgnoreCase ("http:") || p.startsWithIgnoreCase ("https:")
            || p.startsWithIgnoreCase ("ftp:")  || p.startsWithIgnoreCase ("file:");
}

struct LibraryEntry
{
    juce::String assetId;        // juce::Uuid::toString() -- permanent, never reused
    juce::String contentHash;    // SHA-256 hex string of the original file bytes
    juce::String relativePath;   // relative to the owning Library's root folder -- only meaningful when external == false
    juce::String name;           // display name (defaults to the imported filename)

    // Bug report: "the files imported should read from the default location
    // of the files and not copy into the app... that should be asked when
    // saving." external == true means this entry was never copied --
    // externalPath is the absolute path to the ORIGINAL file, read in place.
    // Importer.h's importFile() now defaults to leaving files external;
    // copyExternalEntryIntoLibrary() (also Importer.h) performs the deferred
    // copy, called from the project Save flow when the user opts in.
    bool         external { false };
    juce::String externalPath;
    juce::String tags;           // comma-separated -- simple, sufficient for v1 (no separate tag table)
    juce::String category;       // Phase 1.1 P2: Stems/Loops/Pads/FX/Projects/Templates/Packs -- still a
                                  // freeform string (v1's own established choice, see this struct's
                                  // header comment), just a wider vocabulary than Milestone 16's original.
    double  detectedBpm  { 0.0 };
    bool    favorite     { false };
    int64_t importedAtMs { 0 };

    // Phase 1.1 P2 "Metadata editing for every asset": Artist, Album, Key,
    // Time Signature, Genre, Notes, Creator, Rating. None of these are
    // detected by anything in this pipeline (matching detectedBpm's own
    // honest distinction from these -- see MySamplesTab's metaLine() header
    // comment on "no musical-key field... nothing in this pipeline detects
    // one") -- purely user-entered, empty/0 until a user sets them via the
    // new metadata dialog.
    juce::String artist;
    juce::String album;
    juce::String key;             // e.g. "C", "Am" -- freeform, no detector exists to validate against
    juce::String timeSignature;   // e.g. "4/4"
    juce::String genre;
    juce::String notes;
    juce::String creator;
    int rating { 0 };              // 0 = unrated, 1-5 stars otherwise

    // Phase 1.1 P2 "auto-detect/group a complete stem set into one logical
    // collection with a shared color": rather than fragile filename-pattern
    // guessing (which risks false positives/negatives across an arbitrary
    // real library), the heuristic used is "files the user selected
    // together in one multi-file import ARE one collection" -- a real,
    // unambiguous signal a user importing a stem pack (drums/bass/vocals/
    // other, picked together in one file-chooser action) already provides
    // for free, not an invented one. collectionId empty = not part of any
    // collection; collectionColorArgb is derived deterministically from
    // collectionId (see MySamplesTab::collectionColorFor()) so every entry
    // in the same collection always renders the same shared color without
    // needing to store the colour redundantly on every member.
    juce::String collectionId;

    bool isValid() const { return assetId.isNotEmpty(); }
};

juce::var libraryEntryToVar (const LibraryEntry& entry);
LibraryEntry libraryEntryFromVar (const juce::var& v);

// One library root's own metadata database (root/library.json) + asset
// resolution. A root moved or copied to another machine carries its own
// complete database with it -- "moved libraries" and "multiple library
// roots" both fall directly out of this, per-root design (not one global
// database), per MILESTONE_16_ARCHITECTURE.md's own storage-layout decision.
class Library
{
public:
    explicit Library (juce::File rootFolder) : root (std::move (rootFolder)) {}

    // False (a fresh/empty library, not an error) if root/library.json
    // doesn't exist yet or fails to parse.
    bool load();
    bool save() const;

    const juce::File& getRoot() const { return root; }

    // Registers a new entry (after import) or updates an existing one (same
    // assetId). Pure bookkeeping -- does not itself copy/hash a file, that's
    // the Importer's job (Milestone 16-T2).
    void upsert (const LibraryEntry& entry);
    void remove (const juce::String& assetId);

    std::optional<LibraryEntry> findById (const juce::String& assetId) const;
    std::optional<LibraryEntry> findByHash (const juce::String& contentHash) const;

    // Step 1 of the resolution chain (Architecture Decision Pending #9): an
    // external entry resolves straight to its externalPath (the original
    // file, never copied); otherwise the stored relative path, if it still
    // exists on disk. Returns a non-existent juce::File otherwise -- callers
    // should then either call rescanAndRepair() or surface "Missing asset"
    // (PRODUCT_REQUIREMENTS.md §11's own established precedent: fail
    // cleanly, never silently substitute or guess).
    juce::File resolve (const juce::String& assetId) const;

    // Step 2: walks the root folder, hashing files not already accounted
    // for, and repairs any entry whose stored path is missing but whose
    // content hash is found elsewhere (a file renamed/moved outside the
    // app). Deliberately NOT called implicitly from resolve() -- this is a
    // real filesystem walk + hash pass, expensive for a large library, and
    // is meant to be triggered explicitly (a "Rescan" action or a
    // background job), never as a hidden cost of an ordinary resolve().
    // Returns the number of entries repaired.
    int rescanAndRepair();

    const std::vector<LibraryEntry>& entries() const { return allEntries; }

    // SHA-256 of the file's raw bytes, as a lowercase hex string. Streamed
    // (juce::FileInputStream), not loaded fully into a second buffer first.
    static juce::String hashFile (const juce::File& file);

private:
    juce::File libraryJsonFile() const { return root.getChildFile ("library.json"); }

    juce::File root;
    std::vector<LibraryEntry> allEntries;
};

// Aggregates several Library roots. AssetIds are globally unique (UUIDs),
// so no cross-root collision is possible when resolving -- "multiple
// library roots" is satisfied by construction, not a special case.
class LibraryManager
{
public:
    // Returns a stable reference: roots is a vector of unique_ptr
    // specifically so this reference stays valid even if a LATER addRoot()
    // call reallocates the vector itself (only the pointer moves, not the
    // Library object it points to) -- callers are not required to re-fetch
    // a Library& after every addRoot() elsewhere.
    Library& addRoot (const juce::File& rootFolder)
    {
        for (auto& lib : roots) if (lib->getRoot() == rootFolder) return *lib;
        roots.push_back (std::make_unique<Library> (rootFolder));
        roots.back()->load();
        return *roots.back();
    }

    juce::File resolve (const juce::String& assetId) const
    {
        for (auto& lib : roots)
        {
            auto file = lib->resolve (assetId);
            if (file.existsAsFile()) return file;
        }
        return {};
    }

    std::optional<LibraryEntry> findById (const juce::String& assetId) const
    {
        for (auto& lib : roots)
            if (auto e = lib->findById (assetId)) return e;
        return std::nullopt;
    }

    std::vector<std::unique_ptr<Library>>& libraries() { return roots; }
    const std::vector<std::unique_ptr<Library>>& libraries() const { return roots; }

private:
    std::vector<std::unique_ptr<Library>> roots;
};

// Milestone 16: resolves an (assetId, filePath) pair the way every project
// load boundary (Deck layers, Pads, FX) should -- prefer the Library
// resolution (survives a moved/renamed file) when an assetId is present and
// resolves successfully; fall back to the raw path otherwise. An empty
// assetId (every project saved before this milestone) always falls
// straight through to the raw path -- exact pre-Milestone-16 behavior,
// unchanged.
inline juce::File resolveAssetOrPath (const LibraryManager& libraryManager, const juce::String& assetId, const juce::String& filePath)
{
    if (assetId.isNotEmpty())
    {
        auto resolved = libraryManager.resolve (assetId);
        if (resolved.existsAsFile()) return resolved;
    }
    // SECURITY: filePath comes from a project file, which may have been sent
    // by someone else -- see isRemotePath's own comment for why a UNC/WebDAV
    // path must never be touched. Local paths are still honoured: a project
    // legitimately references the user's own files by absolute path.
    if (isRemotePath (filePath)) return {};
    return juce::File (filePath);
}

} // namespace ezlibrary
