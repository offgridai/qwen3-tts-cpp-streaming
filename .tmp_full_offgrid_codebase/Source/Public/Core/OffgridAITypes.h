#pragma once

#include "CoreMinimal.h"
#include "OffgridAITypes.generated.h"

/*
================================================================================
Orchestrator State (Lifecycle / Availability)
================================================================================
*/

UENUM(BlueprintType)
enum class EOffgridAIOrchestratorState : uint8
{
    Inactive        UMETA(DisplayName = "Inactive"),
    Booting         UMETA(DisplayName = "Booting Services"),
    Ready           UMETA(DisplayName = "Ready"),
    Error           UMETA(DisplayName = "Error")
};

/*
================================================================================
Conversation State (Turn Flow / UI)
================================================================================
*/

UENUM(BlueprintType)
enum class EOffgridAIConversationState : uint8
{
    Inactive        UMETA(DisplayName = "Inactive"),
    AwaitingInput   UMETA(DisplayName = "Awaiting Input"),
    Recording       UMETA(DisplayName = "Recording"),
    ProcessingASR   UMETA(DisplayName = "Processing ASR"),
    ProcessingLLM   UMETA(DisplayName = "Processing LLM"),
    Retrying        UMETA(DisplayName = "Retrying"),
    Speaking        UMETA(DisplayName = "Speaking NPC Line"),
    Error           UMETA(DisplayName = "Conversation Error")
};


USTRUCT(BlueprintType)
struct OFFGRIDAI_API FOffgridAIPADSState
{
    GENERATED_BODY()

    // Scholarly PAD axes use [-1,+1]. Neutral is exactly (0,0,0).
    // Stability is OffgridAI-specific and remains [0,1], where 0 is volatile
    // and 1 is highly resistant to emotional change.
    UPROPERTY(EditAnywhere, BlueprintReadWrite, meta = (ClampMin = "-1.0", ClampMax = "1.0"))
    float Pleasure = 0.0f;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, meta = (ClampMin = "-1.0", ClampMax = "1.0"))
    float Activation = 0.0f;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, meta = (ClampMin = "-1.0", ClampMax = "1.0"))
    float Dominance = 0.0f;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, meta = (ClampMin = "0.0", ClampMax = "1.0"))
    float Stability = 0.5f;

    void Clamp01()
    {
        Pleasure = FMath::Clamp(Pleasure, -1.0f, 1.0f);
        Activation = FMath::Clamp(Activation, -1.0f, 1.0f);
        Dominance = FMath::Clamp(Dominance, -1.0f, 1.0f);
        Stability = FMath::Clamp(Stability, 0.0f, 1.0f);
    }
};

/*
================================================================================
Service Runtime Types
================================================================================
*/

UENUM(BlueprintType)
enum class EOffgridAIServiceKind : uint8
{
    ASR UMETA(DisplayName = "ASR"),
    LLM UMETA(DisplayName = "LLM"),
    TTS UMETA(DisplayName = "TTS")
};

UENUM(BlueprintType)
enum class EOffgridAIServiceState : uint8
{
    Stopped     UMETA(DisplayName = "Stopped"),
    Starting    UMETA(DisplayName = "Starting"),
    Ready       UMETA(DisplayName = "Ready"),
    Busy        UMETA(DisplayName = "Busy"),
    Unhealthy   UMETA(DisplayName = "Unhealthy"),
    Restarting  UMETA(DisplayName = "Restarting"),
    Fatal       UMETA(DisplayName = "Fatal")
};

UENUM(BlueprintType)
enum class EOffgridAIServiceImplementation : uint8
{
    Stub UMETA(DisplayName = "Stub"),
    Real UMETA(DisplayName = "Real")
};

UENUM(BlueprintType)
enum class EOffgridAIServiceEventSeverity : uint8
{
    Verbose UMETA(DisplayName = "Verbose"),
    Log     UMETA(DisplayName = "Log"),
    Warning UMETA(DisplayName = "Warning"),
    Error   UMETA(DisplayName = "Error")
};

USTRUCT(BlueprintType)
struct OFFGRIDAI_API FOffgridAIServiceStatus
{
    GENERATED_BODY()

    UPROPERTY(EditAnywhere, BlueprintReadWrite)
    EOffgridAIServiceKind ServiceKind = EOffgridAIServiceKind::ASR;

    UPROPERTY(EditAnywhere, BlueprintReadWrite)
    EOffgridAIServiceState State = EOffgridAIServiceState::Stopped;

    UPROPERTY(EditAnywhere, BlueprintReadWrite)
    EOffgridAIServiceImplementation Implementation = EOffgridAIServiceImplementation::Stub;

    UPROPERTY(EditAnywhere, BlueprintReadWrite)
    bool bIsRequired = true;

    UPROPERTY(EditAnywhere, BlueprintReadWrite)
    int32 RestartCount = 0;

    UPROPERTY(EditAnywhere, BlueprintReadWrite)
    bool bHasPendingRequest = false;

    UPROPERTY(EditAnywhere, BlueprintReadWrite)
    float SecondsSinceLastHeartbeat = 0.0f;

    UPROPERTY(EditAnywhere, BlueprintReadWrite)
    FString LastError;
};

USTRUCT(BlueprintType)
struct OFFGRIDAI_API FOffgridAIServiceEvent
{
    GENERATED_BODY()

    UPROPERTY(EditAnywhere, BlueprintReadWrite)
    EOffgridAIServiceKind ServiceKind = EOffgridAIServiceKind::ASR;

    UPROPERTY(EditAnywhere, BlueprintReadWrite)
    EOffgridAIServiceEventSeverity Severity = EOffgridAIServiceEventSeverity::Log;

    UPROPERTY(EditAnywhere, BlueprintReadWrite)
    FString Category;

    UPROPERTY(EditAnywhere, BlueprintReadWrite)
    FString Message;

    UPROPERTY(EditAnywhere, BlueprintReadWrite)
    FString RequestID;

    UPROPERTY(EditAnywhere, BlueprintReadWrite)
    FGuid ConversationID;

    UPROPERTY(EditAnywhere, BlueprintReadWrite)
    int32 RestartCount = 0;
};

USTRUCT(BlueprintType)
struct OFFGRIDAI_API FOffgridAIServiceSelection
{
    GENERATED_BODY()

    UPROPERTY(EditAnywhere, BlueprintReadWrite)
    EOffgridAIServiceImplementation ASR = EOffgridAIServiceImplementation::Stub;

    UPROPERTY(EditAnywhere, BlueprintReadWrite)
    EOffgridAIServiceImplementation LLM = EOffgridAIServiceImplementation::Stub;

    UPROPERTY(EditAnywhere, BlueprintReadWrite)
    EOffgridAIServiceImplementation TTS = EOffgridAIServiceImplementation::Stub;
};

USTRUCT(BlueprintType)
struct OFFGRIDAI_API FOffgridAIServiceSupervisorSettings
{
    GENERATED_BODY()

    UPROPERTY(EditAnywhere, BlueprintReadWrite)
    bool bRequired = true;

    UPROPERTY(EditAnywhere, BlueprintReadWrite)
    float StartupDelaySeconds = 0.05f;

    UPROPERTY(EditAnywhere, BlueprintReadWrite)
    float StartupTimeoutSeconds = 30.0f;

    UPROPERTY(EditAnywhere, BlueprintReadWrite)
    float HeartbeatIntervalSeconds = 1.0f;

    UPROPERTY(EditAnywhere, BlueprintReadWrite)
    float HeartbeatTimeoutSeconds = 5.0f;

    UPROPERTY(EditAnywhere, BlueprintReadWrite)
    float RequestTimeoutSeconds = 15.0f;

    UPROPERTY(EditAnywhere, BlueprintReadWrite)
    float RestartCooldownSeconds = 1.0f;

    UPROPERTY(EditAnywhere, BlueprintReadWrite)
    float GracefulShutdownTimeoutSeconds = 1.0f;

    UPROPERTY(EditAnywhere, BlueprintReadWrite)
    int32 MaxRestartsInWindow = 3;

    UPROPERTY(EditAnywhere, BlueprintReadWrite)
    float RestartWindowSeconds = 300.0f;
};

/*
================================================================================
Conversation Record (Canonical Memory)
================================================================================
*/

USTRUCT(BlueprintType)
struct OFFGRIDAI_API FOffgridAIConversationRecordLine
{
    GENERATED_BODY()

    UPROPERTY(EditAnywhere, BlueprintReadWrite)
    bool bIsPlayer = false;

    UPROPERTY(EditAnywhere, BlueprintReadWrite)
    FName SpeakerID = NAME_None;

    UPROPERTY(EditAnywhere, BlueprintReadWrite)
    FName VoiceID = NAME_None;

    UPROPERTY(EditAnywhere, BlueprintReadWrite)
    FName Emotion = NAME_None;

    UPROPERTY(EditAnywhere, BlueprintReadWrite)
    FText Message;

    UPROPERTY(EditAnywhere, BlueprintReadWrite)
    FGuid ConversationID;

    UPROPERTY(EditAnywhere, BlueprintReadWrite)
    FName LineID = NAME_None;
};

/*
================================================================================
Transcript Line (UI / HUD)
================================================================================
*/

USTRUCT(BlueprintType)
struct OFFGRIDAI_API FOffgridAITranscriptLine
{
    GENERATED_BODY()

    UPROPERTY(EditAnywhere, BlueprintReadWrite)
    bool bIsPlayer = false;

    UPROPERTY(EditAnywhere, BlueprintReadWrite)
    FName SpeakerID = NAME_None;

    // NPC transcript metadata for multi-speaker UI. Player lines usually leave this unset.
    UPROPERTY(EditAnywhere, BlueprintReadWrite)
    FName Emotion = NAME_None;

    // Raw dialogue without UI formatting. Existing widgets can keep using Message.
    UPROPERTY(EditAnywhere, BlueprintReadWrite)
    FText Dialogue;

    UPROPERTY(EditAnywhere, BlueprintReadWrite)
    FText Message;
};

/*
================================================================================
TTS Voice Mode
================================================================================
*/

UENUM(BlueprintType)
enum class EOffgridAITTSVoiceMode : uint8
{
    Base        UMETA(DisplayName = "Base"),
    CustomVoice UMETA(DisplayName = "Custom Voice"),
    VoiceDesign UMETA(DisplayName = "VoiceDesign")
};

/*
================================================================================
Line Performance Request (Orchestrator -> LineCoach)
================================================================================
*/

USTRUCT(BlueprintType)
struct OFFGRIDAI_API FOffgridAILinePerformanceRequest
{
    GENERATED_BODY()

    UPROPERTY(EditAnywhere, BlueprintReadWrite)
    FGuid ConversationID;

    UPROPERTY(EditAnywhere, BlueprintReadWrite)
    FName LineID = NAME_None;

    UPROPERTY(EditAnywhere, BlueprintReadWrite)
    FName NPCID = NAME_None;

    UPROPERTY(EditAnywhere, BlueprintReadWrite)
    FName VoiceID = NAME_None;

    UPROPERTY(EditAnywhere, BlueprintReadWrite)
    EOffgridAITTSVoiceMode TTSVoiceMode = EOffgridAITTSVoiceMode::CustomVoice;

    UPROPERTY(EditAnywhere, BlueprintReadWrite)
    bool bTTSVoiceDesign = false;

    // Final per-line instruction sent to VoiceDesign-capable TTS services.
    // This is built by the LineCoach from stable voice identity + current delivery.
    UPROPERTY(EditAnywhere, BlueprintReadWrite)
    FString TTSInstruction;

    UPROPERTY(EditAnywhere, BlueprintReadWrite)
    FText Dialogue;

    // Performance emotion used to drive the current spoken line.
    // Canonical values are data-asset driven; common values include neutral, joy, anger, sadness, fear, surprise, disgust.
    UPROPERTY(EditAnywhere, BlueprintReadWrite)
    FName Emotion = NAME_None;

    // Text phrase passed to TTS when emotion forwarding is enabled. This is derived from
    // the selected line emotion plus the post-escalation persistent intensity for that
    // emotion, e.g. "slightly angry", "very afraid", "extremely disgusted".
    // Neutral lines use "neutral" and do not escalate persistent emotion state.
    UPROPERTY(EditAnywhere, BlueprintReadWrite)
    FString TTSEmotionInstruction;

    // Post-update persistent emotion magnitude owned by ConversationManager.
    // LineCoach consumes this only as a per-line expression intensity; it does not
    // update or store persistent emotion state.
    UPROPERTY(EditAnywhere, BlueprintReadWrite)
    float EmotionMagnitude = 0.0f;
};

/*
================================================================================
Audio Analysis Frame (Lightweight Feature Extraction)
================================================================================
*/

USTRUCT(BlueprintType)
struct OFFGRIDAI_API FOffgridAIAudioAnalysisFrame
{
    GENERATED_BODY()

    UPROPERTY(EditAnywhere, BlueprintReadWrite)
    float SpeechEnergy = 0.0f;

    UPROPERTY(EditAnywhere, BlueprintReadWrite)
    float LowBandEnergy = 0.0f;

    UPROPERTY(EditAnywhere, BlueprintReadWrite)
    float MidBandEnergy = 0.0f;

    UPROPERTY(EditAnywhere, BlueprintReadWrite)
    float HighBandEnergy = 0.0f;

    UPROPERTY(EditAnywhere, BlueprintReadWrite)
    float Brightness = 0.0f;

    UPROPERTY(EditAnywhere, BlueprintReadWrite)
    float Voicedness = 0.0f;
};

/*
================================================================================
Facial Frame (Final Output to Animation System)
================================================================================
*/

USTRUCT(BlueprintType)
struct OFFGRIDAI_API FOffgridAIFacialFrame
{
    GENERATED_BODY()

    UPROPERTY(EditAnywhere, BlueprintReadWrite)
    TMap<FName, float> EmotionWeights;

    UPROPERTY(EditAnywhere, BlueprintReadWrite)
    TMap<FName, float> MouthPoseWeights;

    UPROPERTY(EditAnywhere, BlueprintReadWrite)
    FName NPCID = NAME_None;

    UPROPERTY(EditAnywhere, BlueprintReadWrite)
    FName LineID = NAME_None;

    UPROPERTY(EditAnywhere, BlueprintReadWrite)
    float JawOpen = 0.0f;

    UPROPERTY(EditAnywhere, BlueprintReadWrite)
    float TeethShow = 0.0f;

    UPROPERTY(EditAnywhere, BlueprintReadWrite)
    float LipRound = 0.0f;

    // Optional expanded mouth controls for MetaHuman / custom AnimBP mappings.
    // These are also mirrored in MouthPoseWeights under the same names. Existing
    // users of JawOpen/TeethShow/LipRound remain compatible.
    UPROPERTY(EditAnywhere, BlueprintReadWrite)
    float LipCompression = 0.0f;

    UPROPERTY(EditAnywhere, BlueprintReadWrite)
    float CornerPull = 0.0f;

    UPROPERTY(EditAnywhere, BlueprintReadWrite)
    float LipFunnel = 0.0f;

    UPROPERTY(EditAnywhere, BlueprintReadWrite)
    float LipStretch = 0.0f;

    UPROPERTY(EditAnywhere, BlueprintReadWrite)
    float LowerLipDown = 0.0f;
};
