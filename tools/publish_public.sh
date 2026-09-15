#!/usr/bin/env bash
# ============================================================================
#  tools/publish_public.sh -- push the app-only snapshot of HEAD to the public
#  GitHub repository that builds the Mac and iPad versions:
#      https://github.com/Amanorsac-Studio/PerfomLive
#
#  This local repository keeps everything: the store backend, licence tooling,
#  store keys, handover notes, marketing, and the full history (including the
#  samples that were removed from it). None of that goes public. The public
#  repository gets ONLY the files listed in INCLUDE below, as one new commit per
#  publish on top of the previous public commit, plus JUCE 9.0.0 as a submodule.
#
#  Usage, from the repo root, after committing:   bash tools/publish_public.sh
#  Adding a file to the app? Add it to INCLUDE, or CI will not see it.
# ============================================================================
set -euo pipefail

REMOTE_URL="https://github.com/Amanorsac-Studio/PerfomLive.git"
PUBLIC_REF="refs/heads/public"   # local record of what was last published
JUCE_URL="https://github.com/juce-framework/JUCE.git"
JUCE_COMMIT="f8f8864172464b9adf9eba6101e1f784838d1597"   # JUCE 9.0.0, the version in C:/JUCE

INCLUDE=(
  .github/workflows/build.yml
  .gitignore
  README.md
  CMakeLists.txt
  # app sources
  Main.cpp Library.cpp Library.h ProjectFile.cpp ProjectFile.h
  ActionRegistry.h Arrangement.h Beta.h CreatorsTab.h DeckCard.h Deck.h EzDSP.h
  Importer.h KeyBindingMap.h Metronome.h MidiActionRouter.h Mixer.h OneShotVoice.h
  PlaybackView.h ProductPaths.h Session.h SignatureManager.h UiArt.h WarpIntegration.h
  BrowserTab.h DownloadManager.h CueDetect.h CueReview.h Guide.h InstrumentHost.h
  SectionNameField.h SpeechCues.h SpeechCues_win.cpp StemImport.h StoreShowcase.h TouchSupport.h
  ChannelStrip/PerformProcessor.cpp ChannelStrip/PerformProcessor.h
  ChannelStrip/PerformEditor.cpp ChannelStrip/PerformEditor.h ChannelStrip/SOURCE.txt
  # engine test suites run by CI
  decktest.cpp switchtest.cpp stemlentest.cpp stemplaytest.cpp stemlocktest.cpp
  outroutetest.cpp sectiontest.cpp arrangetest.cpp
  # bundled, read-only assets
  fonts art logo.png logo_icon.png logo_icon_1024.png
  # Windows packaging (built locally; needs the ASIO SDK)
  installer package_release.ps1
  tools/ci_ipad_testflight.sh tools/publish_public.sh
)

# Same patterns verify_batch.sh refuses, plus Apple/GitHub key material.
SECRET_RE='(sk|pk|rk)_(live|test)_[A-Za-z0-9]{16,}|whsec_[A-Za-z0-9]{16,}|AKIA[0-9A-Z]{16}|gh[pousr]_[A-Za-z0-9]{20,}|-----BEGIN [A-Z ]*PRIVATE KEY-----'
FORBIDDEN_RE='\.(wav|mp3|aif|aiff|flac|ogg|m4a|perform|p12|p8|pem|key|cer|mobileprovision|env|zip)$|(^|/)store-backend/|Store(Client|Crypto|Keys)\.h$|PackInstaller\.h$'

cd "$(git rev-parse --show-toplevel)"

# Publish committed work only, so the public commit maps to a real local one.
if ! git diff --quiet HEAD -- "${INCLUDE[@]}"; then
  echo "Published files have uncommitted changes -- commit them first:" >&2
  git diff --stat HEAD -- "${INCLUDE[@]}" >&2
  exit 1
fi
for p in "${INCLUDE[@]}"; do
  git ls-tree -r --name-only HEAD -- "$p" | grep -q . || { echo "Not in HEAD: $p" >&2; exit 1; }
done

INDEX="$(mktemp)"
trap 'rm -f "$INDEX"' EXIT
export GIT_INDEX_FILE="$INDEX"
git read-tree --empty
git ls-tree -r HEAD -- "${INCLUDE[@]}" | git update-index --index-info

GITMODULES_BLOB="$(printf '[submodule "JUCE"]\n\tpath = JUCE\n\turl = %s\n' "$JUCE_URL" | git hash-object -w --stdin)"
git update-index --add --cacheinfo "100644,$GITMODULES_BLOB,.gitmodules"
git update-index --add --cacheinfo "160000,$JUCE_COMMIT,JUCE"
TREE="$(git write-tree)"
unset GIT_INDEX_FILE

BAD="$(git ls-tree -r --name-only "$TREE" | grep -E "$FORBIDDEN_RE" || true)"
if [ -n "$BAD" ]; then echo "Refusing to publish these files:" >&2; printf '  %s\n' $BAD >&2; exit 1; fi
HITS="$(git grep -nIE "$SECRET_RE" "$TREE" -- . ':(exclude)tools/publish_public.sh' || true)"
if [ -n "$HITS" ]; then echo "Refusing to publish -- credential-shaped strings:" >&2; printf '%s\n' "$HITS" >&2; exit 1; fi

PARENT="$(git rev-parse -q --verify "$PUBLIC_REF" || true)"

# Never force-push. If GitHub has commits this machine did not make (a file
# edited on github.com, say), the new snapshot goes on top of them, and any
# file only they added is listed, because the snapshot replaces the tree.
REMOTE_HEAD="$(git ls-remote "$REMOTE_URL" refs/heads/main | cut -f1)"
if [ -n "$REMOTE_HEAD" ]; then
  git fetch -q "$REMOTE_URL" main
  if [ -z "$PARENT" ] || ! git merge-base --is-ancestor "$REMOTE_HEAD" "$PARENT"; then
    DROPPED="$(git diff --name-only --diff-filter=D "$REMOTE_HEAD" "$TREE" || true)"
    [ -n "$DROPPED" ] && { echo "Replacing files that exist only on GitHub:"; printf '  %s\n' $DROPPED; }
    PARENT="$REMOTE_HEAD"
  fi
fi

if [ -n "$PARENT" ] && [ "$(git rev-parse "$PARENT^{tree}")" = "$TREE" ]; then
  echo "Nothing new since the last publish ($(git rev-parse --short "$PARENT")); pushing it again."
  COMMIT="$PARENT"
else
  VERSION="$(sed -n 's/^project(EzPlay VERSION \([0-9.]*\)).*/\1/p' CMakeLists.txt)"
  MESSAGE="PerformLive $VERSION ($(git rev-parse --short HEAD))

$(git log -1 --format=%s HEAD)"
  COMMIT="$(git commit-tree "$TREE" ${PARENT:+-p "$PARENT"} -m "$MESSAGE")"
  git update-ref "$PUBLIC_REF" "$COMMIT"
fi

echo "Files: $(git ls-tree -r --name-only "$TREE" | wc -l | tr -d ' ')"
git push "$REMOTE_URL" "$COMMIT:refs/heads/main"
echo "Published $(git rev-parse --short "$COMMIT") -> $REMOTE_URL (Actions: ${REMOTE_URL%.git}/actions)"
