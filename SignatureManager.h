// ============================================================================
//  SignatureManager.h — a thin, additive mapping/data layer above Session.
//
//  Pure C++, no JUCE, no dependency on Session.h/Deck.h beyond treating a
//  flat deck index as a plain int. Resolves ARCHITECTURE.md's Architecture
//  Decision Pending #1 (Option A): every signature bank shares ONE Session,
//  generalized to a flattened deck index space; SignatureManager owns which
//  NAMES exist and which contiguous RANGE of that flat index space belongs
//  to each one. It never touches playback state (active/queued/crossfade)
//  and Session/Deck never include or reference this header -- the
//  dependency runs one way only: Application -> SignatureManager -> Session.
//
//  Milestone 6 scope: the 7 signatures PRODUCT_REQUIREMENTS.md §1 names
//  explicitly (4/4, 2/4, 3/4, 6/8, REG, AFR, HIL), each with 8 decks, per
//  PRD §1's own "8 decks per signature" grid (side A/B x 4 rows). The "+ add
//  a custom signature" UI is out of scope here -- it depends on runtime
//  clip-loading, which doesn't exist anywhere in this app yet
//  (NEXT_STEPS.md #12) -- see MILESTONE_6_IMPLEMENTATION_PLAN.md's stated
//  scope boundary.
//
//  beatsPerBar values below are PRD §1's own stated SIGS-array example
//  (4/4 = 4 beats, 6/8 = 2 beats -- counted in dotted-quarter beats, not
//  eighth notes) extended to 2/4 and 3/4 by the same convention, with
//  REG/AFR/HIL sharing 4/4's timing exactly as PRD §1 states.
// ============================================================================
#pragma once
#include <array>
#include <string>

namespace ezdeck
{

constexpr int kDecksPerSignature = 8;

struct Signature
{
    std::string name;
    int         beatsPerBar;
    int         firstDeckIndex;   // first of this signature's 8 contiguous flat deck indices
};

class SignatureManager
{
public:
    static constexpr int kNumSignatures = 7;
    static constexpr int kTotalDecks    = kNumSignatures * kDecksPerSignature;

    SignatureManager()
    {
        struct Def { const char* name; int beatsPerBar; };
        static const Def kDefs[kNumSignatures] = {
            { "4/4", 4 }, { "2/4", 2 }, { "3/4", 3 }, { "6/8", 2 },
            { "REG", 4 }, { "AFR", 4 }, { "HIL", 4 }
        };
        for (int i = 0; i < kNumSignatures; ++i)
            signatures[(size_t) i] = { kDefs[(size_t) i].name, kDefs[(size_t) i].beatsPerBar,
                                        i * kDecksPerSignature };
    }

    int size() const { return kNumSignatures; }
    const Signature& signature (int sigIdx) const { return signatures[(size_t) sigIdx]; }

    int firstDeckIndex (int sigIdx) const { return signatures[(size_t) sigIdx].firstDeckIndex; }

    // Which signature owns a given flat deck index.
    int signatureIndexForDeck (int flatDeckIndex) const { return flatDeckIndex / kDecksPerSignature; }

private:
    std::array<Signature, kNumSignatures> signatures;
};

} // namespace ezdeck
