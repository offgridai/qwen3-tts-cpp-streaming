#pragma once

#include "CoreMinimal.h"

// Text-first lipsync viseme groups used by OffgridAILineCoach.
// These names are intentionally articulation-group names rather than literal phonemes.
enum class EOffgridAITextViseme : uint8
{
    Rest,
    MBP, // M / B / P bilabial closure
    AAA, // Open vowel / jaw-open family
    EEE, // Wide vowel family
    OOO, // Rounded vowel family
    WUH, // W / rounded onset funnel
    FVS  // F / V / S teeth/fricative family
};

struct FOffgridAILipsyncLayer1StageCounts
{
    int32 CandidateCount = 0;
    int32 TimedCandidateCount = 0;
    int32 FinalEventCount = 0;
};

struct FOffgridAILipsyncLayer1Diagnostics
{
    FOffgridAILipsyncLayer1StageCounts StageCounts;
    int32 SuppressionCount = 0;
    int32 PhraseFinalAdjustmentCount = 0;
    float CompressionRatio = 1.0f;
};

struct FOffgridAITextVisemeEvent
{
    float StartNorm = 0.0f;
    float EndNorm = 0.0f;
    EOffgridAITextViseme Viseme = EOffgridAITextViseme::Rest;

    // Direct MetaHuman viseme pose id from MetaHumanVisemeLibrary.json.
    // When set, LineCoach submits this exact pose id to FaceDriver rather than
    // collapsing the event through the legacy six-bucket mouth abstraction.
    FName PoseID = NAME_None;

    float Strength = 0.0f;
    FString SourceText;
    int32 WordIndex = INDEX_NONE;
    int32 PhraseIndex = 0;

    // Sentence-level launch island assigned by the text planner.
    // PhraseIndex remains an intra-island timing feature; SentenceIslandIndex
    // is the only planner-provided structural island boundary. It advances on
    // hard sentence breaks only (. ! ? and newlines), never on commas.
    int32 SentenceIslandIndex = 0;
    bool bIsLandmark = false;
    bool bIsDominant = false;
    bool bIsFunctionWord = false;
    FName Generator = NAME_None;
};

struct FOffgridAITextVisemePlan
{
    TArray<FOffgridAITextVisemeEvent> Events;
    float EstimatedDurationSeconds = 0.0f;
    FOffgridAILipsyncLayer1Diagnostics Layer1Diagnostics;

    // Duration-planner metadata for every tokenized dialogue word, including
    // words that later produce no visible viseme event. The streaming aligner
    // uses this to make island duration syllable-owned without accidentally
    // ignoring reduced/function words that were visually suppressed.
    TArray<int32> WordSentenceIslandIndices;
    TArray<int32> WordPhraseIndices;
    TArray<int32> WordSyllableCounts;

    // Soft/hard punctuation immediately following each tokenized word. This
    // lets the runtime duration planner budget corpus-derived punctuation
    // pockets by punctuation type without reparsing dialogue text in the
    // streaming aligner. Zero means no punctuation boundary after the word.
    TArray<TCHAR> WordBoundaryPunctuationAfter;
};




class OFFGRIDAI_API FOffgridAITextVisemePlanner
{
public:
    static FOffgridAITextVisemePlan BuildPlan(const FText& Dialogue, float CharactersPerSecond = 14.0f, float MinDurationSeconds = 0.45f);
    static const TCHAR* ToPoseKey(EOffgridAITextViseme Viseme);
    static FString ToDebugString(EOffgridAITextViseme Viseme);
};
