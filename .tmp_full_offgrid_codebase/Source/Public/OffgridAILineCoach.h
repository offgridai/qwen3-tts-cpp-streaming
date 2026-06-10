#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "Core/OffgridAITypes.h"
#include "Data/OffgridAILipsyncSettingsDataAsset.h"
#include "Data/OffgridAIEmotionSettingsDataAsset.h"
#include "Lipsync/OffgridAITextVisemePlanner.h"
#include "OffgridAILineCoach.generated.h"

class UAudioComponent;
class USoundAttenuation;
class USoundWaveProcedural;
class UOffgridAILineCoachAudioSettingsDataAsset;
class UOffgridAIEmotionSettingsDataAsset;
class UOffgridAIOrchestrator;
class UOffgridAIMetaHumanFaceDriverComponent;
class FOffgridAILipsyncRuntimeSession;

DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FOffgridAIFacialFrameUpdated, const FOffgridAIFacialFrame&, FacialFrame);

struct FOffgridAILipsyncFeatureFrame
{
    float TimeSeconds = 0.0f;
    float RMSNorm = 0.0f;
    float PreviousRMSNorm = 0.0f;
    float LowRatio = 0.0f;
    float MidRatio = 0.0f;
    float HighRatio = 0.0f;
    float CentroidNorm = 0.0f;
    float Voicedness = 0.0f;
    bool bIsTransient = false;
};

// Architectural boundary types for lipsync.
//
// Layer 1: FOffgridAITextVisemePlan (from OffgridAITextVisemePlanner) chooses symbolic visemes.
// Layer 2: Runtime/audio timing assists anchor those symbolic events to observed speech time.
// Layer 3: LineCoach's performance synthesizer samples the fixed layer-2 timeline into
//          abstract per-frame viseme weights.
// Layer 4: MetaHuman FaceDriver translates abstract viseme weights into rig controls.
//
// Guardrail for future changes: work on layer 3 must not mutate layer 1/2 event
// identity, order, strengths, or timing. It may only change how fixed events are
// evaluated into abstract weights for the current playback time.
struct FOffgridAIAlignedVisemeEvent
{
    int32 EventIndex = INDEX_NONE;
    FName PoseID = NAME_None;
    float Strength = 0.0f;
    FString SourceWord;
    int32 WordIndex = INDEX_NONE;
    int32 PhraseIndex = 0;
    bool bIsLandmark = false;

    // Text-plan normalized center corresponding to this event.
    float TextCenterNorm = 0.0f;

    // Diagnostic only: the original text-plan center before audio correction.
    // Layer 3 must never use this for runtime sampling.
    float TextDiagnosticCenterSeconds = 0.0f;

    // SINGLE AUTHORITATIVE RUNTIME CLOCK.
    // Layer 2 owns this value. It is audible playback seconds from
    // OutputAudioComponent->Play()/GetCurrentOutputPlaybackSeconds()==0 after
    // text timing has been warped/corrected to the observed TTS audio.
    // Layer 3 may shape anticipation/release around this center, but must not
    // recompute, replace, or fall back to another event clock.
    float FinalRenderCenterSeconds = 0.0f;

    // Optional debug/perf windows. Current gameplay may leave these equal to
    // the center until future parity work ports the newer runtime performer.
    float RenderStartSeconds = 0.0f;
    float RenderEndSeconds = 0.0f;

    // Layer2 streaming-v2 diagnostics / runtime safety metadata. These mirror
    // the lipsync-lab committed-track fields and are intentionally optional for
    // older paths.
    float AlignmentConfidence = 0.0f;
    bool bUsedLayer1Fallback = false;
    float RawShiftSeconds = 0.0f;
    float AppliedShiftSeconds = 0.0f;
    float ShiftCapSeconds = 0.0f;

    // V22 diagnostics: when this event was committed relative to playback, and
    // which timing policy produced it.  These are scoring/debug fields only;
    // Layer 3 must continue to sample FinalRenderCenterSeconds.
    float CommitPlaybackSeconds = 0.0f;
    float CommitLeadSeconds = 0.0f;
    FName CommitReason = FName(TEXT("unknown"));
    float EffectiveSegmentScale = 1.0f;
    float EffectiveCommitHorizonSeconds = 0.0f;

    // N04 diagnostic-only active-clock accounting. These fields explain why
    // each event was placed by audio occupancy or tail drain: how much
    // speech-active audio the event required from the text plan, how much
    // observed active audio existed, and the resulting deficit. Runtime
    // sampling continues to use FinalRenderCenterSeconds as authoritative.
    float RequiredActiveElapsedSeconds = 0.0f;
    float ObservedActiveElapsedSeconds = 0.0f;
    float ActiveProgressDeficitSeconds = 0.0f;
    float RequiredProgressNorm = 0.0f;
    float ObservedProgressNorm = 0.0f;
    float ActiveProgressRatio = 1.0f;
    bool bMappedToObservedSpeech = false;

    // Island-clock diagnostics. These make the runtime timing model explicit:
    // every committed event belongs to one text island and one matching audio
    // island. The event center is computed from the island-local mapping, not
    // from a whole-line/global speech clock. Seconds here are playback seconds
    // for audio fields and text-plan seconds for text fields.
    int32 TextIslandIndex = INDEX_NONE;
    int32 AudioIslandIndex = INDEX_NONE;
    float IslandTextStartSeconds = 0.0f;
    float IslandTextEndSeconds = 0.0f;
    float IslandAudioStartSeconds = 0.0f;
    float IslandAudioEndSeconds = 0.0f;
    float IslandLocalNorm = 0.0f;
    float IslandAudioSpanSeconds = 0.0f;
    bool bReusedIslandClock = false;

    // Explicit planner-owned timing diagnostics. These are authored by the
    // text/prosody planner and remain separate from runtime playback seconds.
    float PlannerIslandPredictedDurationSeconds = 0.0f;
    float PlannerIslandSpeechMaterialSeconds = 0.0f;
    float PlannerIslandPunctuationSeconds = 0.0f;
    float PlannerIslandShortUtteranceFloorSeconds = 0.0f;

    // AU18b duration calibration diagnostics. These are planner-owned metadata
    // used to audit event-count bucket scaling; runtime sampling still uses
    // FinalRenderCenterSeconds.
    FName PlannerDurationBucket = NAME_None;
    float PlannerDurationScaleApplied = 1.0f;
    float PlannerIslandUnscaledDurationSeconds = 0.0f;

    // B05 punctuation-restart diagnostics. These anchors are not sub-islands:
    // a text soft-boundary only permits the audio detector to snap the next
    // phrase's restart when a nearby silence->speech resume is observed.
    bool bPunctuationRestartCandidate = false;
    bool bPunctuationRestartAccepted = false;
    FName PunctuationRestartReason = NAME_None;
    float PunctuationRestartExpectedSeconds = 0.0f;
    float PunctuationRestartObservedSeconds = 0.0f;
    float PunctuationRestartRawShiftSeconds = 0.0f;
    float PunctuationRestartAppliedShiftSeconds = 0.0f;

    // F11 diagnostic-only boundary gate trace. These fields name the exact
    // punctuation boundary that attempted to own an audio silence gap. They are
    // intentionally copied onto affected downstream events so debug CSVs can
    // recover one row per boundary without introducing a separate runtime side channel.
    int32 PunctuationGateIndex = INDEX_NONE;
    FString PunctuationGateBoundary = TEXT("");
    int32 PunctuationGatePrevPhraseIndex = INDEX_NONE;
    int32 PunctuationGateNextPhraseIndex = INDEX_NONE;
    float PunctuationGateBoundarySeconds = 0.0f;
    float PunctuationGateExpectedResumeSeconds = 0.0f;
    float PunctuationGateCandidatePauseStartSeconds = 0.0f;
    float PunctuationGateCandidateResumeSeconds = 0.0f;
    float PunctuationGateCandidateGapSeconds = 0.0f;
    int32 PunctuationGateCandidatePrevAudioIsland = INDEX_NONE;
    int32 PunctuationGateCandidateNextAudioIsland = INDEX_NONE;
    float PunctuationGatePreviousPhraseLastCenterSeconds = 0.0f;
    float PunctuationGateNextPhraseFirstCenterSeconds = 0.0f;

    // F12 punctuation-segment scheduler diagnostics. Unlike F10, which shifted
    // all downstream events, F12 records segment-local launch/scale decisions.
    FName PunctuationSegmentScheduler = NAME_None;
    float PunctuationSegmentScale = 1.0f;

    // E20 diagnostic-only punctuation/audio ownership audit. These fields do
    // not drive timing; they explain why a punctuation boundary did or did not
    // consume an audio pause/resume gap inside the one-line runtime island.
    int32 PunctuationOwnershipBoundaryPhraseIndex = INDEX_NONE;
    int32 PunctuationOwnershipNextPhraseIndex = INDEX_NONE;
    FName PunctuationOwnershipKind = NAME_None;
    int32 PunctuationOwnershipPreviousAudioIslandIndex = INDEX_NONE;
    int32 PunctuationOwnershipNextAudioIslandIndex = INDEX_NONE;
    int32 PunctuationOwnershipCandidatePreviousAudioIslandIndex = INDEX_NONE;
    int32 PunctuationOwnershipCandidateNextAudioIslandIndex = INDEX_NONE;
    bool bPunctuationOwnershipSameAudioIsland = false;
    bool bPunctuationOwnershipGapMatches = false;
    float PunctuationOwnershipPreviousPhraseStartSeconds = 0.0f;
    float PunctuationOwnershipPreviousPhraseEndSeconds = 0.0f;
    float PunctuationOwnershipNextPhraseStartSeconds = 0.0f;
    float PunctuationOwnershipNextPhraseEndSeconds = 0.0f;
    float PunctuationOwnershipCandidateGapStartSeconds = 0.0f;
    float PunctuationOwnershipCandidateResumeSeconds = 0.0f;

    // V8 duration diagnostics: syllable floor used by the planner-owned island
    // duration model. These are diagnostics only; runtime sampling still uses
    // FinalRenderCenterSeconds.
    int32 PlannerIslandSyllableCount = 0;
    float PlannerIslandSyllableFloorSeconds = 0.0f;
    float PlannerEventPredictedOffsetSeconds = 0.0f;
    float PlannerEventNormCenter = 0.0f;
    int32 PlannerProsodyGroupIndex = INDEX_NONE;
    FName PlannerProsodyRole = NAME_None;
    float PlannerProsodyGroupWeight = 0.0f;
    float PlannerProsodyGroupAllocatedSeconds = 0.0f;

    // Alpha17 prosody/island budget diagnostics. These mirror liplab committed
    // track fields and are authored by the streaming aligner. They are diagnostic
    // metadata only; Layer 3 must continue to sample FinalRenderCenterSeconds.
    int32 PlannerProsodyGroupEventCount = 0;
    float PlannerProsodyGroupEventCountModelSeconds = 0.0f;
    float PlannerProsodyIslandEventCountModelSeconds = 0.0f;
    float PlannerProsodyIslandSpeechBudgetSeconds = 0.0f;
    float PlannerProsodyAllocationScale = 1.0f;

    int32 ProsodyGroupIndex = INDEX_NONE;
    FName ProsodyGroupRole = NAME_None;
    FName ProsodyGroupReleaseReason = NAME_None;

    float ProsodyGroupExpectedReleaseSeconds = 0.0f;
    float ProsodyGroupEarliestReleaseSeconds = 0.0f;
    float ProsodyGroupLatestReleaseSeconds = 0.0f;
    float ProsodyGroupActualReleaseSeconds = 0.0f;
    float ProsodyGroupReleaseLagSeconds = 0.0f;

    // Audio-nudge diagnostics. These mirror liplab's Step 9 instrumentation and
    // are diagnostic/runtime-safety metadata only. Layer 3 must continue to use
    // FinalRenderCenterSeconds as the authoritative committed event center.
    bool bAudioNudgeEligible = false;
    bool bAudioNudgeSearchPerformed = false;
    bool bAudioNudgeAccepted = false;
    FName AudioNudgeRejectReason = NAME_None;
    float AudioNudgeSearchStartSeconds = 0.0f;
    float AudioNudgeSearchEndSeconds = 0.0f;
    float AudioNudgeObservedEndSeconds = 0.0f;
    float AudioNudgeScheduledCenterSeconds = 0.0f;
    float AudioNudgeCandidateRawCenterSeconds = 0.0f;
    float AudioNudgeCandidateAppliedCenterSeconds = 0.0f;
    float AudioNudgeCandidateRawShiftSeconds = 0.0f;
    float AudioNudgeCandidateAppliedShiftSeconds = 0.0f;
    float AudioNudgeCandidateConfidence = 0.0f;
    float AudioNudgeRequiredConfidence = 0.0f;
    float AudioNudgeMaxCorrectionSeconds = 0.0f;

    // G06 passive streaming correspondence diagnostics. These deliberately
    // separate acoustic correspondence from 150 ms preroll actionability:
    // a landmark can be a real match even when it arrived too late to safely
    // change the already-authored runtime track.
    bool bAudioNudgeCorrespondenceMatched = false;
    bool bAudioNudgeActionableWithinPreroll = false;
    float AudioNudgeCandidateAudioTimeSeconds = 0.0f;
    float AudioNudgeCandidateAvailableAtSeconds = 0.0f;

    // G07 passive compatibility diagnostics.  These fields record the actual
    // streaming detector class that was paired with this planned viseme and
    // whether the match was exact or family-compatible.  They do not alter
    // timing; they exist so future runtime retiming can be based on measured
    // detector/planner vocabulary overlap rather than hidden class mismatches.
    FName AudioNudgeCandidateLandmarkClass = NAME_None;
    FName AudioNudgeCompatibilityKind = NAME_None;
    float AudioNudgeCompatibilityScore = 0.0f;

    // Monotonic-center repair diagnostics. The streaming aligner may make a tiny
    // forward-only repair when two committed centers would otherwise violate event
    // ordering after island-clock mapping. Runtime sampling still uses
    // FinalRenderCenterSeconds.
    bool bCenterOrderRepaired = false;
    float CenterOrderRepairSeconds = 0.0f;

    // V22/V23 search-window diagnostics. These explain cases where Step 9 could
    // not search the predicted timing window and whether the available-audio
    // fallback path was used.
    FName AudioNudgeEmptyWindowCause = NAME_None;
    float AudioNudgeAvailableAudioStartSeconds = 0.0f;
    float AudioNudgeAvailableAudioEndSeconds = 0.0f;
    float AudioNudgeAvailableBeforeSearchSeconds = 0.0f;
    float AudioNudgePredictedLeadSeconds = 0.0f;
    FName AudioNudgeSearchMode = NAME_None;
};

struct FOffgridAIAlignedVisemeTrack
{
    FName NPCID = NAME_None;
    FName LineID = NAME_None;
    float SpeechStartSeconds = 0.0f;
    float SpeechEndSeconds = 0.0f;
    TArray<FOffgridAIAlignedVisemeEvent> Events;
};

struct FOffgridAIPerformedVisemeFrame
{
    float PlaybackSeconds = 0.0f;
    FName NPCID = NAME_None;
    FName LineID = NAME_None;
    TMap<FName, float> AbstractVisemeWeights;
};


// Event-instance sample at the Layer3 -> Layer4 boundary.
// This preserves committed timing provenance until the final rig collapse.
// Repeated PoseIDs (07_Aa, 09_Oh, 11_Oo, 22_MBP, etc.) must remain distinct here.
struct FOffgridAISubmittedVisemeSample
{
    int32 EventIndex = INDEX_NONE;
    FName PoseID = NAME_None;
    FString SourceWord;
    float PlaybackSeconds = 0.0f;
    float CommittedRenderStartSeconds = 0.0f;
    float CommittedRenderCenterSeconds = 0.0f;
    float CommittedRenderEndSeconds = 0.0f;
    float SubmittedWeight = 0.0f;
    float SourceStrength = 0.0f;
};

struct FOffgridAILipsyncPoseFrame
{
    float TimeSeconds = 0.0f;
    FName NPCID = NAME_None;
    FName LineID = NAME_None;
    TMap<FName, float> MouthPoseWeights;
    bool bIsTransient = false;
};

struct FOffgridAILipsyncPoseRuntimeState
{
    float Open = 0.0f;
    float Closed = 0.0f;
    float Wide = 0.0f;
    float Round = 0.0f;
    float Funnel = 0.0f;
    float Teeth = 0.0f;
};

struct FOffgridAIPrecomputedLipsyncSample
{
    float PlaybackSeconds = 0.0f;
    FOffgridAILipsyncPoseRuntimeState PoseState;
    TMap<FName, float> DriverPoseWeights;
};




enum class EOffgridAILipsyncSpeechState : uint8
{
    Silence,
    Speaking
};


UCLASS(ClassGroup = (OffgridAI), meta = (BlueprintSpawnableComponent))
class OFFGRIDAI_API UOffgridAILineCoach : public UActorComponent
{
    GENERATED_BODY()

public:
    UOffgridAILineCoach();


    virtual void BeginPlay() override;
    virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;
    virtual void TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction) override;

    UFUNCTION(BlueprintCallable, Category = "OffgridAI")
    void PerformLine(const FOffgridAILinePerformanceRequest& LineRequest);

    void BeginOutputAudioStream(FName LineID, int32 SampleRate, int32 NumChannels);
    void SubmitOutputAudioChunk(FName LineID, const TArray<uint8>& PCMChunk, int32 SampleRate, int32 NumChannels);
    void SubmitOutputAudioChunk(FName LineID, const TArray<uint8>& PCMChunk, int32 SampleRate, int32 NumChannels, int64 ChunkStartSample, int32 ChunkSampleCount);
    void EndOutputAudioStream(FName LineID);
    void SubmitDrivenEmotionExpression(FName Emotion, float EmotionMagnitude);

    // Backwards-compatible wrappers for existing Blueprint/C++ callers.
    void BeginOutputAudioStream(int32 SampleRate, int32 NumChannels);
    void SubmitOutputAudioChunk(const TArray<uint8>& PCMChunk, int32 SampleRate, int32 NumChannels);
    void EndOutputAudioStream();
    bool IsPerformingLine(FName LineID) const;
    bool IsOutputBusy() const;

    UFUNCTION(BlueprintCallable, Category = "OffgridAI|Facial")
    void ForceNeutral();

    UFUNCTION(BlueprintCallable, Category = "OffgridAI|Facial")
    FOffgridAIFacialFrame GetCurrentFacialFrame() const { return CurrentFacialFrame; }

    UPROPERTY(BlueprintAssignable, Category = "OffgridAI|Facial")
    FOffgridAIFacialFrameUpdated OnFacialFrameUpdated;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "OffgridAI")
    FName NPCID = TEXT("Alfie");

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "OffgridAI")
    FName VoiceID = TEXT("male01");

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "OffgridAI|TTS|VoiceDesign", meta = (DisplayName = "Use VoiceDesign"))
    bool bUseVoiceDesign = false;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "OffgridAI|TTS|VoiceDesign", meta = (MultiLine = true))
    FString VoiceDesignIdentity = TEXT("natural adult conversational voice");

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "OffgridAI|TTS|VoiceDesign", meta = (MultiLine = true))
    FString VoiceDesignNeutralDelivery = TEXT("natural, conversational, emotionally balanced");

    bool ShouldUseVoiceDesignForTTS() const { return bUseVoiceDesign; }
    FString BuildVoiceDesignInstructionForLine(const FOffgridAILinePerformanceRequest& LineRequest) const;

    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "OffgridAI|Audio")
    TObjectPtr<UOffgridAILineCoachAudioSettingsDataAsset> LineCoachAudioSettingsAsset = nullptr;

    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "OffgridAI|Emotion")
    TObjectPtr<UOffgridAIEmotionSettingsDataAsset> EmotionSettingsAsset = nullptr;

    // Designer/debug toggle for facial emotional expression on this NPC.
    // When disabled, ConversationManager still owns and updates PADS state,
    // chat labels still print, and TTS still receives the resolved emotional
    // label. This only prevents LineCoach from submitting emotional face
    // targets or post-line emotional visual-rest targets to the FaceDriver, so
    // lipsync can be evaluated in isolation.
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "OffgridAI|Emotion", meta = (DisplayName = "Enable Facial Emotion Expression"))
    bool bEnableFacialEmotionExpression = true;

    // Designer-authored starting affect for this NPC. ConversationManager copies
    // these values when a conversation begins and owns runtime emotional state after that.
    // Pleasure/Activation/Dominance use scholarly PAD range [-1,+1].
    // Stability is OffgridAI-specific [0,1].
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "OffgridAI|Emotion|Starting PADS", meta = (ClampMin = "-1.0", ClampMax = "1.0"))
    float StartingPleasure = 0.0f;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "OffgridAI|Emotion|Starting PADS", meta = (ClampMin = "-1.0", ClampMax = "1.0"))
    float StartingActivation = 0.0f;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "OffgridAI|Emotion|Starting PADS", meta = (ClampMin = "-1.0", ClampMax = "1.0"))
    float StartingDominance = 0.0f;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "OffgridAI|Emotion|Starting PADS", meta = (ClampMin = "0.0", ClampMax = "1.0"))
    float StartingStability = 0.5f;

    UFUNCTION(BlueprintCallable, Category = "OffgridAI|Emotion")
    FOffgridAIPADSState GetStartingPADSState() const;


    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "OffgridAI|Facial")
    TObjectPtr<UOffgridAILipsyncSettingsDataAsset> LipsyncSettingsAsset = nullptr;

    // Emotion/lipsync blending controls forwarded to the MetaHuman FaceDriver.
    // These are LineCoach-facing defaults so designers can tune the interaction
    // between emotion expression and speech without editing the FaceDriver component.
    // They do not affect audio playback, lipsync timing, viseme selection, or LineCoach timing.
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "OffgridAI|Emotion|Lipsync Blend")
    bool bOverrideFaceDriverEmotionMouthAllowance = true;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "OffgridAI|Emotion|Lipsync Blend", meta = (ClampMin = "0.0", ClampMax = "1.0"))
    float EmotionSpeechControlScaleDuringSpeech = 0.04f;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "OffgridAI|Emotion|Lipsync Blend", meta = (ClampMin = "0.0", ClampMax = "1.0"))
    float EmotionMouthCornerScaleDuringSpeech = 0.18f;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "OffgridAI|Emotion|Lipsync Blend", meta = (ClampMin = "0.0", ClampMax = "1.0"))
    float EmotionMouthCornerScaleDuringBilabial = 0.04f;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "OffgridAI|Emotion|Lipsync Blend", meta = (ClampMin = "0.0", ClampMax = "1.0"))
    float EmotionSharedMouthScaleDuringSpeech = 0.08f;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "OffgridAI|Emotion|Lipsync Blend", meta = (ClampMin = "0.0", ClampMax = "10.0"))
    float EmotionSpeechHoldSeconds = 1.5f;

    // Default intentionally slow: after speech stops, full emotional mouth influence
    // eases back over several seconds so expression does not pop between syllables/lines.
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "OffgridAI|Emotion|Lipsync Blend", meta = (ClampMin = "0.001", ClampMax = "10.0"))
    float EmotionMouthFadeInSeconds = 3.0f;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "OffgridAI|Emotion|Lipsync Blend", meta = (ClampMin = "0.001", ClampMax = "10.0"))
    float EmotionMouthFadeOutSeconds = 0.10f;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "OffgridAI|Emotion|Lipsync Blend", meta = (ClampMin = "0.01", ClampMax = "10.0"))
    float EmotionFullMouthAfterSilenceSeconds = 1.501f;

    UFUNCTION(BlueprintCallable, Category = "OffgridAI|Emotion")
    TArray<FName> GetConfiguredSupportedEmotionNames() const;

    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "OffgridAI|Facial", meta = (ClampMin = "0.0", ClampMax = "1.0"))
    float PlaybackCompletionTailSeconds = 0.12f;

protected:
    void UpdateLipsyncFromPlayback(float DeltaTime);
    float GetTextPlanSpeechDurationSeconds() const;
    // Layer 3 entry point: samples the immutable text/audio-aligned viseme timeline
    // into abstract viseme weights for this playback time. This function must not
    // mutate ActiveTextVisemePlan, event timing, event order, event IDs, or event
    // strengths. It must not know about MetaHuman controls.
    TArray<FOffgridAISubmittedVisemeSample> SampleCommittedVisemeSamples(float PlaybackSeconds) const;
    TMap<FName, float> CollapseSubmittedVisemeSamplesToPoseWeights(const TArray<FOffgridAISubmittedVisemeSample>& Samples) const;
    TMap<FName, float> SampleDirectVisemePoseWeights(float PlaybackSeconds) const;
    void BuildPrecomputedLipsyncPerformanceFromAccumulatedAudio();
    bool SamplePrecomputedLipsyncPerformance(float PlaybackSeconds, FOffgridAILipsyncPoseRuntimeState& OutPoseState, TMap<FName, float>& OutDriverPoseWeights) const;
    void WriteLipsyncDebugPrecomputedPerformanceCSV() const;
    void FinalizeSharedLipsyncRuntimeTrack();
    const FOffgridAIAlignedVisemeEvent* FindActiveAlignedVisemeEvent(int32 EventIndex) const;
    float ComputePCMDrivenSpeechActivityAtPlaybackTime(float PlaybackSeconds) const;
    void RefreshTextPlanSpeechBoundsFromAccumulatedPCM();
    void UpdateStreamingAlignedVisemeTrack();
    void BeginSharedLipsyncRuntimeSession();
    void SyncSharedLipsyncRuntimeMirrors();
    void ApplyAU38StreamingProsodyGroupReflow();
    FOffgridAILipsyncRuntimeSession& GetOrCreateSharedLipsyncRuntimeSession();
    bool IsStreamingLipsyncRuntimeEnabled() const;
    void UpdateLipsyncEnergyFromPCM(const TArray<uint8>& PCMChunk, int32 BytesToUse, int32 SampleRate, int32 NumChannels);
    void BroadcastFacialFrameIfChanged();
    void BeginLineFacialState(FName Emotion, float EmotionMagnitude = 0.0f);
    void EndLineFacialState();

    float BlendScalar(float CurrentValue, float TargetValue, float DeltaTime, float AttackSpeed, float ReleaseSpeed) const;
    void AppendPCMToLipsyncAnalysisBuffer(const TArray<uint8>& PCMChunk, int32 BytesToUse, int32 SampleRate, int32 NumChannels);
    void AppendPCMToLipsyncAnalysisBuffer(const TArray<uint8>& PCMChunk, int32 BytesToUse, int32 SampleRate, int32 NumChannels, int64 ChunkStartSample, int32 ChunkSampleCount);
    void ApplyLipsyncSmoothing(float& SmoothedValue, float RawValue, float AttackMs, float ReleaseMs, float DeltaTimeSeconds) const;
    void UpdateDisplayedLipsyncPose(float DeltaTime);
    // Layer 3 -> Layer 4 handoff. The LineCoach submits abstract viseme weights;
    // the FaceDriver is the only layer that may translate them to rig controls.
    void SetCurrentFacialMouthPoseFromState(const FOffgridAILipsyncPoseRuntimeState& PoseState);
    UOffgridAIMetaHumanFaceDriverComponent* GetFaceDriver() const;
    void ApplyEmotionMouthAllowanceSettingsToFaceDriver(UOffgridAIMetaHumanFaceDriverComponent* FaceDriver) const;
    FOffgridAILipsyncPoseRuntimeState GetPoseRuntimeStateFromMap(const TMap<FName, float>& PoseMap) const;
    void ResetLipsyncRuntimeState();
    bool IsLipsyncDebugLoggingEnabled() const;
    FString GetLipsyncDebugLogPath() const;
    FString GetLipsyncDebugLineDirectory() const;
    void ResetLipsyncDebugLog();
    void ResetLipsyncDebugInputAudio();
    void WriteLipsyncDebugLineMetadata() const;
    void AppendLipsyncDebugInputSample(float MonoSample, int32 SampleRate);
    FString GetLipsyncDebugInputWavPath() const;
    void WriteLipsyncDebugInputAudio();
    void AppendLipsyncDebugAudioSummaryCSV(const FString& WavPath, int32 SampleRate, int32 NumSamples, float DurationSec, float PeakAbs, float RMS, float LeadingSilenceMs, float TrailingSilenceMs) const;
    void WriteLipsyncDebugPlannedEventsCSV() const;
    void WriteLipsyncDebugDurationScalingDiagnosticsCSV() const;
    void WriteLipsyncDebugTimingCoverageDiagnosticsCSV() const;
    void WriteLipsyncDebugMotionQualityCSV() const;
    void WriteLipsyncDebugScorecards() const;
    void AppendLipsyncDebugSubmittedPosesCSV(float PlaybackSeconds, const TMap<FName, float>& SubmittedWeights, const TMap<FName, float>& FaceDriverWeights) const;
    void AppendLipsyncDebugCSV(const TCHAR* Stage, float FrameTimeSec, const FOffgridAILipsyncFeatureFrame* Features, const FOffgridAILipsyncPoseRuntimeState& Raw, const FOffgridAILipsyncPoseRuntimeState& Smoothed, const FOffgridAILipsyncPoseRuntimeState& Final) const;
    float GetCurrentOutputPlaybackSeconds() const;
    float GetLipsyncSettingFloat(float UOffgridAILipsyncSettingsDataAsset::*Member, float DefaultValue) const;
    int32 GetLipsyncSettingInt(int32 UOffgridAILipsyncSettingsDataAsset::*Member, int32 DefaultValue) const;

private:
    UOffgridAIOrchestrator* GetOrchestrator() const;
    void InitializeEmotionMapsIfNeeded();
    const TArray<FName>& GetSupportedEmotionNames() const;
    void EnsurePlaybackObjects(int32 SampleRate, int32 NumChannels);
    void TeardownPlaybackObjects();
    void HandlePlaybackFinished();
    void SchedulePlaybackDrainCheck(float DelaySeconds);
    bool HasPendingOrBufferedOutputAudio() const;
    void StartOutputPlaybackIfReady(bool bForceStart);
    void FlushPendingOutputPCM(bool bForceFlushAll);
    void UpdateOutputPlaybackRecovery();
    void PauseOutputPlaybackForUnderrun();
    float GetInitialPrerollSeconds() const;
    float GetMaintainBufferedAudioFloorSeconds() const;
    float GetPlaybackUnderrunFloorSeconds() const;
    float GetPlaybackResumePrerollSeconds() const;
    float GetCoalescedWriteSeconds() const;
    float GetMaxWriteBurstSeconds() const;
    float GetPlaybackPostDrainHoldSeconds() const;
    int32 GetBytesPerSecond() const;
    int64 GetEstimatedBufferedPlaybackBytes() const;


    FVector GetConfiguredAudioSourceOffset() const;
    bool IsSpatializationEnabled() const;
    float GetPlaybackVolumeMultiplier() const;
    USoundAttenuation* GetConfiguredAttenuationSettings() const;

    UPROPERTY(Transient)
    TObjectPtr<UAudioComponent> OutputAudioComponent = nullptr;

    UPROPERTY(Transient)
    TObjectPtr<USoundWaveProcedural> ProceduralSoundWave = nullptr;

    UPROPERTY()
    FOffgridAILinePerformanceRequest ActiveLineRequest;

    bool bHasActiveLineRequest = false;
    bool bOutputStreamOpen = false;
    bool bOutputPlaybackStarted = false;
    bool bOutputPlaybackPausedForUnderrun = false;
    bool bEmotionMapsInitialized = false;
    bool bHasReceivedAnalysis = false;

    // Shared lipsync runtime session. This is the single interface between LineCoach
    // and the lipsync core in streaming mode; LineCoach mirrors its outputs only
    // for legacy debug/export and FaceDriver sampling code below.
    FOffgridAILipsyncRuntimeSession* LipsyncRuntimeSession = nullptr;
    bool bSharedLipsyncRuntimeFinalized = false;
    bool bSharedLipsyncOfflineEvidenceRetimingApplied = false;
    int32 SharedLipsyncPreRetimingEventCount = 0;
    int32 SharedLipsyncPostRetimingEventCount = 0;

    // AU38 Unreal probe: LineCoach-local mirror of the LipLab AU38 streaming
    // prosody-group reflow simulation. The shared runtime still owns island
    // launch and monotonic commit; this pass only rewrites the mirrored
    // ActiveAlignedVisemeTrack for FaceDriver sampling so we can evaluate the
    // reflow hypothesis in Unreal before promoting it into shared core.
    bool bAU38StreamingGroupReflowApplied = false;
    int32 AU38StreamingGroupReflowGroupCount = 0;
    int32 AU38StreamingGroupReflowAffineGroupCount = 0;
    int32 AU38StreamingGroupReflowSingleAnchorGroupCount = 0;
    int32 AU38StreamingGroupReflowForwardShiftGroupCount = 0;
    int32 AU38StreamingGroupReflowAppliedEventCount = 0;
    int32 AU38StreamingGroupReflowAnchorCount = 0;
    float AU38StreamingGroupReflowMeanAbsEventDeltaMs = 0.0f;
    float AU38StreamingGroupReflowMaxAbsEventDeltaMs = 0.0f;

    // Layer 1 output mirror. Do not modify from layer-3 performance synthesis.
    FOffgridAITextVisemePlan ActiveTextVisemePlan;

    // Layer 2 authoritative schedule for the current line. Version H publishes
    // exactly one final LipsyncRuntime track for current offline/full-buffer
    // playback. Future streaming must update this via aligner-owned incremental
    // commit, never via LineCoach timing rebuilds. Layer 3 consumes only this
    // track, never text timing directly.
    //
    // TIMING CONTRACT:
    // - Text-plan timing is an input guess only.
    // - Layer 2 converts that guess into FinalRenderCenterSeconds.
    // - Layer 3 consumes FinalRenderCenterSeconds read-only.
    // - There is no layer-3 fallback to text duration, duration scale, or diagnostics.
    //
    // If this track is missing, layer 3 must output no timed viseme events rather
    // than silently using a competing clock. Playback startup is responsible for
    // creating the provisional track before audible audio begins.
    FOffgridAIAlignedVisemeTrack ActiveAlignedVisemeTrack;
    bool bActiveAlignedVisemeTrackBuilt = false;
    // Legacy mirror retained only for code that has not yet been moved to the
    // shared runtime accessors. RuntimeSession owns the authoritative detector.

    // V03 clock-domain contract. Streaming speech detection produces AudioBufferSec
    // values (seconds from first received synthesized PCM sample). FaceDriver only
    // receives PlaybackSec values (seconds from AudioComponent->Play()). This mapping
    // is frozen on first playback start and used exactly once when committing events.
    bool bStreamingPlaybackAudioBufferMapValid = false;
    float StreamingPlaybackAudioBufferStartSec = 0.0f;

    // Streaming lipsync duration predictor.
    // Provisional layer-2 tracks are created before the full WAV exists. The
    // predictor stores the recent final audio/text speech-duration ratio so long
    // or slow TTS lines do not spend their first half on a too-short speculative
    // timeline. This is only used to estimate future event centers during
    // streaming; final/full-audio alignment still remains the timing authority.
    bool bStreamingTimingDurationScaleEstimateInitialized = false;
    float StreamingTimingDurationScaleEstimate = 1.0f;

    float LipsyncObservedAudioDurationSeconds = 0.0f;
    float LipsyncEstimatedTextDurationSeconds = 0.0f;
    float LipsyncEnergyEnvelope = 0.0f;
    float LipsyncEnergyPeak = 0.0001f;
    float LipsyncRawRMS = 0.0f;
    bool bTextPlanSpeechOnsetDetected = false;
    float TextPlanSpeechOnsetAudioSeconds = 0.0f;
    bool bTextPlanSpeechOnsetCandidateActive = false;
    float TextPlanSpeechOnsetCandidateAudioSeconds = 0.0f;
    float TextPlanSpeechOnsetArmSeconds = 0.0f;
    float TextPlanPreSpeechNoisePeakRMS = 0.0001f;
    bool bTextPlanHasSpeechActivityBounds = false;
    float TextPlanLastSpeechActivityAudioSeconds = 0.0f;
    float TextPlanSpeechPeakRMS = 0.0001f;
    float TextPlanSpeechGateThresholdRMS = 0.010f;
    TArray<FOffgridAIPrecomputedLipsyncSample> PrecomputedLipsyncSamples;
    bool bHasPrecomputedLipsyncPerformance = false;
    float PrecomputedLipsyncFrameStepSeconds = 1.0f / 90.0f;
    float PrecomputedLipsyncSpeechStartSeconds = 0.0f;
    float PrecomputedLipsyncSpeechEndSeconds = 0.0f;
    float PrecomputedLipsyncSpeechDurationSeconds = 0.0f;
    float PrecomputedLipsyncLeadSeconds = 0.032f;
    int32 ActiveSampleRate = 0;
    int32 ActiveNumChannels = 0;
    int64 QueuedOutputBytes = 0;
    int64 SubmittedOutputBytes = 0;
    int32 ReceivedOutputChunkCount = 0;
    int32 SubmittedOutputChunkCount = 0;
    int32 PlaybackStartedAfterWindowIndex = 0;
    int64 LastSubmittedChunkStartSample = 0;
    int64 LastSubmittedChunkEndSample = 0;
    double OutputPlaybackStartTimeSeconds = 0.0;
    double OutputPlaybackResumeTimeSeconds = 0.0;
    double ConsumedPlaybackTimeSeconds = 0.0;
    double OutputDrainZeroSinceSeconds = 0.0;
    TArray<uint8> PendingOutputPCM;
    int32 PendingOutputPCMReadOffset = 0;

    FTimerHandle PlaybackCompletionTimerHandle;

    FOffgridAIFacialFrame CurrentFacialFrame;
    FOffgridAIFacialFrame LastBroadcastFacialFrame;

    TArray<float> LipsyncAnalysisSamples;
    int64 LipsyncAnalysisSampleBaseIndex = 0;
    int64 LipsyncNextFrameStartSample = 0;
    float LipsyncRMSPeak = 0.0001f;
    float LipsyncPreviousRMSNorm = 0.0f;
    FOffgridAILipsyncPoseRuntimeState LipsyncPoseState;
    FOffgridAILipsyncPoseRuntimeState TargetDisplayedLipsyncPoseState;
    FOffgridAILipsyncPoseRuntimeState CurrentDisplayedLipsyncPoseState;
    bool bHasDisplayedLipsyncTarget = false;
    bool bLipsyncDebugFileInitialized = false;
    FString LipsyncDebugLineDirectory;
    TArray<int16> LipsyncDebugInputPCM16;
    int32 LipsyncDebugInputSampleRate = 0;
    bool bLipsyncDebugInputAudioWritten = false;
    FOffgridAILipsyncPoseRuntimeState LastCurveSampledPoseState;
    TMap<FName, float> LastResolvedDriverVisemeWeights;
    mutable TArray<FOffgridAISubmittedVisemeSample> LastSubmittedVisemeSamples;
    double LastResolvedDriverVisemePlaybackSeconds = -1.0;
    bool bDriverVisemeSubmissionEnabled = false;
    double LastLipsyncFrameApplyTimeSeconds = 0.0;
    EOffgridAILipsyncSpeechState LipsyncSpeechState = EOffgridAILipsyncSpeechState::Silence;
    bool bLipsyncClosureHoldActive = false;
    bool bLipsyncClosureLerpActive = false;
    float LipsyncClosureHoldRemainingSeconds = 0.0f;
    float LipsyncClosureLerpAlpha = 0.0f;
    FOffgridAILipsyncPoseRuntimeState LipsyncClosureStartState;

    FName LipsyncHeldDominantPose = NAME_None;
    float LipsyncHeldDominantWeight = 0.0f;
    float LipsyncHeldDominantRemainingSeconds = 0.0f;

    float LipsyncBilabialClosureRemainingSeconds = 0.0f;
    float LipsyncBilabialClosureStrength = 0.0f;
    float LipsyncLowEnergyAccumSeconds = 0.0f;
    bool bLipsyncSpeechTailReleaseActive = false;

    UPROPERTY(Transient)
    TObjectPtr<UOffgridAIMetaHumanFaceDriverComponent> CachedFaceDriver = nullptr;

    // Transient expression sample submitted by ConversationManager through the active line request.
    // This is not persistent emotional state; ConversationManager is the only owner of per-NPC emotion levels.
    FName ActiveLineEmotion = NAME_None;
    float ActiveLineEmotionMagnitude = 0.0f;

    TArray<FName> CachedSupportedEmotions;

};
