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
    allFolders.clear();
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

    // SECURITY: library.json can come from someone else, so folder fields are
    // bounded here; imagePath is only ever used through folderImageFile(),
    // which refuses anything outside root/Folders.
    if (auto* folderArr = root2->getProperty ("folders").getArray())
        for (auto& v : *folderArr)
        {
            auto* o = v.getDynamicObject();
            if (o == nullptr) continue;
            LibraryFolder f;
            f.id          = o->getProperty ("id").toString().trim().substring (0, 64);
            f.name        = o->getProperty ("name").toString().trim().substring (0, kMaxFolderNameLength);
            f.imagePath   = o->getProperty ("imagePath").toString().substring (0, 260);
            f.createdAtMs = (int64_t) (double) o->getProperty ("createdAtMs");
            if (f.id.isEmpty() || findFolder (f.id).has_value()) continue;
            if (f.name.isEmpty()) f.name = "Folder";
            allFolders.push_back (f);
        }

    adoptLooseCollections();
    return true;
}

bool Library::save() const
{
    auto* rootObj = new juce::DynamicObject();
    juce::Array<juce::var> arr;
    for (auto& e : allEntries) arr.add (libraryEntryToVar (e));
    rootObj->setProperty ("entries", arr);

    juce::Array<juce::var> folderArr;
    for (auto& f : allFolders)
    {
        auto* o = new juce::DynamicObject();
        o->setProperty ("id", f.id);
        o->setProperty ("name", f.name);
        o->setProperty ("imagePath", f.imagePath);
        o->setProperty ("createdAtMs", (double) f.createdAtMs);
        folderArr.add (juce::var (o));
    }
    rootObj->setProperty ("folders", folderArr);

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

// ---- Folders ------------------------------------------------------------

// Imports made before folders existed grouped each multi-file pick under a
// shared collectionId with no name. Each such group becomes a folder named
// after its files, so nothing already imported looks ungrouped.
void Library::adoptLooseCollections()
{
    for (auto& e : allEntries)
    {
        if (e.collectionId.isEmpty() || findFolder (e.collectionId).has_value()) continue;

        juce::StringArray names;
        int64_t earliest = 0;
        for (auto& member : allEntries)
            if (member.collectionId == e.collectionId)
            {
                names.add (member.name);
                if (earliest == 0 || member.importedAtMs < earliest) earliest = member.importedAtMs;
            }

        LibraryFolder f;
        f.id = e.collectionId.substring (0, 64);
        f.name = suggestFolderName (names);
        f.createdAtMs = earliest;
        allFolders.push_back (f);
    }
}

std::optional<LibraryFolder> Library::findFolder (const juce::String& folderId) const
{
    if (folderId.isEmpty()) return std::nullopt;
    for (auto& f : allFolders) if (f.id == folderId) return f;
    return std::nullopt;
}

LibraryFolder Library::createFolder (const juce::String& name, const juce::StringArray& assetIds)
{
    LibraryFolder f;
    f.id = juce::Uuid().toString();
    f.name = name.trim().substring (0, kMaxFolderNameLength);
    if (f.name.isEmpty()) f.name = "New folder";
    f.createdAtMs = juce::Time::currentTimeMillis();
    allFolders.push_back (f);

    for (auto& id : assetIds) setEntryFolder (id, f.id);
    return f;
}

void Library::upsertFolder (const LibraryFolder& folder)
{
    for (auto& f : allFolders)
        if (f.id == folder.id) { f = folder; return; }
    allFolders.push_back (folder);
}

void Library::removeFolder (const juce::String& folderId)
{
    if (folderId.isEmpty()) return;
    clearFolderImage (folderId);
    allFolders.erase (std::remove_if (allFolders.begin(), allFolders.end(),
                                       [&] (const LibraryFolder& f) { return f.id == folderId; }),
                       allFolders.end());
    for (auto& e : allEntries)
        if (e.collectionId == folderId) e.collectionId = {};
}

bool Library::setEntryFolder (const juce::String& assetId, const juce::String& folderId)
{
    if (folderId.isNotEmpty() && ! findFolder (folderId).has_value()) return false;
    for (auto& e : allEntries)
        if (e.assetId == assetId) { e.collectionId = folderId; return true; }
    return false;
}

int Library::folderSize (const juce::String& folderId) const
{
    if (folderId.isEmpty()) return 0;
    int n = 0;
    for (auto& e : allEntries) if (e.collectionId == folderId) ++n;
    return n;
}

juce::File Library::folderImageFile (const juce::String& folderId) const
{
    auto folder = findFolder (folderId);
    if (! folder.has_value() || folder->imagePath.isEmpty()) return {};
    // SECURITY: same rules as resolve() -- never a remote path, and never a
    // path ("..", absolute, drive-relative) that lands outside root/Folders.
    if (isRemotePath (folder->imagePath)) return {};
    const auto file = root.getChildFile (folder->imagePath);
    if (! file.isAChildOf (root.getChildFile ("Folders"))) return {};
    return file.existsAsFile() ? file : juce::File();
}

bool Library::setFolderImage (const juce::String& folderId, const juce::File& source, juce::String& error)
{
    auto folder = findFolder (folderId);
    if (! folder.has_value()) { error = "That folder no longer exists"; return false; }
    if (isRemotePath (source.getFullPathName())) { error = "Choose a picture saved on this computer"; return false; }
    if (! source.existsAsFile()) { error = "That picture could not be found"; return false; }
    if (source.getSize() > 25 * 1024 * 1024) { error = "That picture is over 25 MB - choose a smaller one"; return false; }

    auto image = juce::ImageFileFormat::loadFrom (source);
    if (! image.isValid()) { error = "That file isn't a picture PerformLive can read - use a PNG or JPG"; return false; }

    constexpr int kMaxSide = 512;
    const int longest = juce::jmax (image.getWidth(), image.getHeight());
    if (longest > kMaxSide)
    {
        const double scale = (double) kMaxSide / (double) longest;
        image = image.rescaled (juce::jmax (1, juce::roundToInt (image.getWidth() * scale)),
                                juce::jmax (1, juce::roundToInt (image.getHeight() * scale)),
                                juce::Graphics::highResamplingQuality);
    }

    auto dir = root.getChildFile ("Folders");
    if (! dir.createDirectory()) { error = "Could not create the library's Folders directory"; return false; }

    const juce::String fileName = juce::Uuid().toString() + ".png";
    const auto dest = dir.getChildFile (fileName);
    {
        juce::FileOutputStream out (dest);
        juce::PNGImageFormat png;
        if (! out.openedOk() || ! png.writeImageToStream (image, out))
        {
            error = "Could not save the folder picture";
            dest.deleteFile();
            return false;
        }
    }

    clearFolderImage (folderId);   // the previous picture, if any
    auto updated = *findFolder (folderId);
    updated.imagePath = "Folders/" + fileName;
    upsertFolder (updated);
    return true;
}

void Library::clearFolderImage (const juce::String& folderId)
{
    auto folder = findFolder (folderId);
    if (! folder.has_value() || folder->imagePath.isEmpty()) return;
    const auto file = folderImageFile (folderId);   // only ever a file inside root/Folders
    if (file.existsAsFile()) file.deleteFile();
    auto updated = *folder;
    updated.imagePath = {};
    upsertFolder (updated);
}

juce::String Library::suggestFolderName (const juce::StringArray& sampleNames)
{
    if (sampleNames.isEmpty()) return "New folder";
    if (sampleNames.size() == 1) return sampleNames[0].trim().substring (0, kMaxFolderNameLength);

    auto isBreak = [] (juce::juce_wchar ch) { return ch == ' ' || ch == '-' || ch == '_' || ch == '.' || ch == '(' || ch == '['; };

    juce::String prefix = sampleNames[0];
    for (auto& n : sampleNames)
    {
        int i = 0;
        const int len = juce::jmin (prefix.length(), n.length());
        while (i < len && juce::CharacterFunctions::toLowerCase (prefix[i]) == juce::CharacterFunctions::toLowerCase (n[i])) ++i;
        prefix = prefix.substring (0, i);
    }

    // don't stop mid-word: "Grace Kick" + "Grace Keys" -> "Grace", not "Grace K"
    if (prefix.length() < sampleNames[0].length() && ! isBreak (sampleNames[0][prefix.length()]))
    {
        int cut = prefix.length();
        while (cut > 0 && ! isBreak (prefix[cut - 1])) --cut;
        prefix = prefix.substring (0, cut);
    }
    while (prefix.isNotEmpty() && isBreak (prefix.getLastCharacter())) prefix = prefix.dropLastCharacters (1);

    prefix = prefix.trim();
    return prefix.length() >= 2 ? prefix.substring (0, kMaxFolderNameLength) : juce::String ("Stem set");
}

} // namespace ezlibrary
