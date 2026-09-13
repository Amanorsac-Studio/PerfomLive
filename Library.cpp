#include "Library.h"

namespace ezlibrary
{

juce::var libraryEntryToVar (const LibraryEntry& entry)
{
    auto* o = new juce::DynamicObject();
    o->setProperty ("assetId", entry.assetId);
    o->setProperty ("contentHash", entry.contentHash);
    o->setProperty ("relativePath", entry.relativePath);
    o->setProperty ("name", entry.name);
    o->setProperty ("tags", entry.tags);
    o->setProperty ("category", entry.category);
    o->setProperty ("detectedBpm", entry.detectedBpm);
    o->setProperty ("favorite", entry.favorite);
    o->setProperty ("importedAtMs", (double) entry.importedAtMs);
    // Phase 1.1 P2 "Metadata editing" fields -- additive, same "no version
    // bump for a purely-additive field" policy ProjectFile.h's own comment
    // documents (this library.json format has no version field at all yet,
    // so there's nothing to bump regardless).
    o->setProperty ("artist", entry.artist);
    o->setProperty ("album", entry.album);
    o->setProperty ("key", entry.key);
    o->setProperty ("timeSignature", entry.timeSignature);
    o->setProperty ("genre", entry.genre);
    o->setProperty ("notes", entry.notes);
    o->setProperty ("creator", entry.creator);
    o->setProperty ("rating", entry.rating);
    o->setProperty ("collectionId", entry.collectionId);
    o->setProperty ("external", entry.external);
    o->setProperty ("externalPath", entry.externalPath);
    return juce::var (o);
}

LibraryEntry libraryEntryFromVar (const juce::var& v)
{
    LibraryEntry e;
    if (auto* o = v.getDynamicObject())
    {
        e.assetId      = o->getProperty ("assetId").toString();
        e.contentHash  = o->getProperty ("contentHash").toString();
        e.relativePath = o->getProperty ("relativePath").toString();
        e.name         = o->getProperty ("name").toString();
        e.tags         = o->getProperty ("tags").toString();
        e.category     = o->getProperty ("category").toString();
        e.detectedBpm  = (double) o->getProperty ("detectedBpm");
        e.favorite     = (bool) o->getProperty ("favorite");
        e.importedAtMs = (int64_t) (double) o->getProperty ("importedAtMs");
        e.artist        = o->getProperty ("artist").toString();
        e.album         = o->getProperty ("album").toString();
        e.key           = o->getProperty ("key").toString();
        e.timeSignature = o->getProperty ("timeSignature").toString();
        e.genre         = o->getProperty ("genre").toString();
        e.notes         = o->getProperty ("notes").toString();
        e.creator       = o->getProperty ("creator").toString();
        e.rating        = (int) o->getProperty ("rating");
        e.collectionId  = o->getProperty ("collectionId").toString();
        e.external      = (bool) o->getProperty ("external");
        e.externalPath  = o->getProperty ("externalPath").toString();
    }
    return e;
}

juce::String Library::hashFile (const juce::File& file)
{
    juce::FileInputStream stream (file);
    if (! stream.openedOk()) return {};
    juce::SHA256 hash (stream);
    return hash.toHexString();
}

bool Library::load()
{
    allEntries.clear();
    auto file = libraryJsonFile();
    if (! file.existsAsFile()) return false;

    const juce::String text = file.loadFileAsString();
    if (text.isEmpty()) return false;

    juce::var parsed;
    if (juce::JSON::parse (text, parsed).failed()) return false;

    auto* root2 = parsed.getDynamicObject();
    if (root2 == nullptr) return false;

    if (auto* arr = root2->getProperty ("entries").getArray())
        for (auto& v : *arr) allEntries.push_back (libraryEntryFromVar (v));

    return true;
}

bool Library::save() const
{
    auto* rootObj = new juce::DynamicObject();
    juce::Array<juce::var> arr;
    for (auto& e : allEntries) arr.add (libraryEntryToVar (e));
    rootObj->setProperty ("entries", arr);

    if (! root.exists()) root.createDirectory();
    return libraryJsonFile().replaceWithText (juce::JSON::toString (juce::var (rootObj)));
}

void Library::upsert (const LibraryEntry& entry)
{
    for (auto& e : allEntries)
    {
        if (e.assetId == entry.assetId) { e = entry; return; }
    }
    allEntries.push_back (entry);
}

void Library::remove (const juce::String& assetId)
{
    allEntries.erase (std::remove_if (allEntries.begin(), allEntries.end(),
                                       [&] (const LibraryEntry& e) { return e.assetId == assetId; }),
                       allEntries.end());
}

std::optional<LibraryEntry> Library::findById (const juce::String& assetId) const
{
    for (auto& e : allEntries) if (e.assetId == assetId) return e;
    return std::nullopt;
}

std::optional<LibraryEntry> Library::findByHash (const juce::String& contentHash) const
{
    if (contentHash.isEmpty()) return std::nullopt;
    for (auto& e : allEntries) if (e.contentHash == contentHash) return e;
    return std::nullopt;
}

juce::File Library::resolve (const juce::String& assetId) const
{
    auto entry = findById (assetId);
    if (! entry.has_value()) return {};

    if (entry->external)
    {
        if (entry->externalPath.isEmpty()) return {};
        // SECURITY: a library index can arrive from someone else (a shared
        // library folder or pack), so its paths are untrusted. Reject remote
        // ones -- merely probing \\host\share on Windows performs an SMB/
        // WebDAV handshake that transmits the user's NTLM credentials to
        // whoever runs that host, with no interaction beyond opening a file.
        if (isRemotePath (entry->externalPath)) return {};
        juce::File file (entry->externalPath);
        return file.existsAsFile() ? file : juce::File();
    }

    if (entry->relativePath.isEmpty()) return {};
    if (isRemotePath (entry->relativePath)) return {};
    // SECURITY: getChildFile() deliberately honours absolute paths and
    // resolves embedded "..", so a crafted relativePath ("..\..\..\Windows\
    // ...", "C:\...", "\\host\share\...") escapes the library root entirely.
    // Requiring the result to still be INSIDE root closes that.
    auto file = root.getChildFile (entry->relativePath);
    if (! file.isAChildOf (root)) return {};
    return file.existsAsFile() ? file : juce::File();
}

int Library::rescanAndRepair()
{
    int repaired = 0;

    // Which relative paths are currently accounted for by a VALID entry
    // (file still exists there) -- everything else on disk is a candidate
    // for repairing a broken entry.
    std::vector<juce::String> knownGoodPaths;
    for (auto& e : allEntries)
        if (! e.external && root.getChildFile (e.relativePath).existsAsFile())
            knownGoodPaths.push_back (e.relativePath);

    juce::Array<juce::File> onDisk;
    root.findChildFiles (onDisk, juce::File::findFiles, true);

    for (auto& e : allEntries)
    {
        if (e.external) continue;   // resolved straight to externalPath, not this root -- nothing here to repair
        if (root.getChildFile (e.relativePath).existsAsFile()) continue;   // already fine
        if (e.contentHash.isEmpty()) continue;                             // nothing to match against

        for (auto& candidate : onDisk)
        {
            const juce::String relative = candidate.getRelativePathFrom (root);
            if (std::find (knownGoodPaths.begin(), knownGoodPaths.end(), relative) != knownGoodPaths.end())
                continue;   // already claimed by another valid entry

            if (hashFile (candidate) == e.contentHash)
            {
                e.relativePath = relative;
                knownGoodPaths.push_back (relative);
                ++repaired;
                break;
            }
        }
    }

    if (repaired > 0) save();
    return repaired;
}

} // namespace ezlibrary
