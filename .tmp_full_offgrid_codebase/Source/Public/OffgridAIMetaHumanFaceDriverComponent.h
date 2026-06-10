#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
class FOffgridAIEmotionExpression;

#include "OffgridAIMetaHumanFaceDriverComponent.generated.h"

UENUM(BlueprintType)
enum class EOffgridAIFacePoseLayer : uint8
{
    Viseme,
    Emotion
};

USTRUCT(BlueprintType)
struct OFFGRIDAI_API FOffgridAIFaceDriverControlValue
{
    GENERATED_BODY()

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="OffgridAI|MetaHuman Face")
    FName ControlName = NAME_None;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="OffgridAI|MetaHuman Face")
    bool bIsVector2D = false;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="OffgridAI|MetaHuman Face")
    float FloatValue = 0.0f;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="OffgridAI|MetaHuman Face")
    FVector2D Vector2DValue = FVector2D::ZeroVector;
};


USTRUCT(BlueprintType)
struct OFFGRIDAI_API FOffgridAIMetaHumanFacePose
{
    GENERATED_BODY()

    // AnimBP-facing MetaHuman control surface projected from the JSON pose libraries.
    // Declaration order intentionally mirrors the Face_ControlBoard_CtrlRig node pin order
    // so Break Struct nodes sit close to the ControlRig pins and Blueprint wiring is auditable.
    // CTRL_C_tongue is exposed as "CTRL C Tongue Move" because that is the ControlRig node pin name.
    // CTRL_C_tongue_roll is intentionally omitted: the current Face_ControlBoard_CtrlRig node exposes it inconsistently / not as a Vector2D pin.

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="OffgridAI|MetaHuman Face|Jaw", meta=(DisplayName="CTRL C jaw")) FVector2D CTRL_C_jaw = FVector2D::ZeroVector;
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="OffgridAI|MetaHuman Face|Eyes", meta=(DisplayName="CTRL L eye blink")) float CTRL_L_eye_blink = 0.0f;
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="OffgridAI|MetaHuman Face|Eyes", meta=(DisplayName="CTRL R eye blink")) float CTRL_R_eye_blink = 0.0f;
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="OffgridAI|MetaHuman Face|Brows", meta=(DisplayName="CTRL L brow lateral")) float CTRL_L_brow_lateral = 0.0f;
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="OffgridAI|MetaHuman Face|Brows", meta=(DisplayName="CTRL R brow lateral")) float CTRL_R_brow_lateral = 0.0f;
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="OffgridAI|MetaHuman Face|Brows", meta=(DisplayName="CTRL L brow down")) float CTRL_L_brow_down = 0.0f;
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="OffgridAI|MetaHuman Face|Brows", meta=(DisplayName="CTRL R brow down")) float CTRL_R_brow_down = 0.0f;
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="OffgridAI|MetaHuman Face|Brows", meta=(DisplayName="CTRL L brow raiseOut")) float CTRL_L_brow_raiseOut = 0.0f;
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="OffgridAI|MetaHuman Face|Brows", meta=(DisplayName="CTRL R brow raiseOut")) float CTRL_R_brow_raiseOut = 0.0f;
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="OffgridAI|MetaHuman Face|Brows", meta=(DisplayName="CTRL L brow raiseIn")) float CTRL_L_brow_raiseIn = 0.0f;
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="OffgridAI|MetaHuman Face|Brows", meta=(DisplayName="CTRL R brow raiseIn")) float CTRL_R_brow_raiseIn = 0.0f;
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="OffgridAI|MetaHuman Face|Eyes", meta=(DisplayName="CTRL L eye cheekRaise")) float CTRL_L_eye_cheekRaise = 0.0f;
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="OffgridAI|MetaHuman Face|Eyes", meta=(DisplayName="CTRL R eye cheekRaise")) float CTRL_R_eye_cheekRaise = 0.0f;
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="OffgridAI|MetaHuman Face|Eyes", meta=(DisplayName="CTRL L eye squintInner")) float CTRL_L_eye_squintInner = 0.0f;
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="OffgridAI|MetaHuman Face|Eyes", meta=(DisplayName="CTRL R eye squintInner")) float CTRL_R_eye_squintInner = 0.0f;
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="OffgridAI|MetaHuman Face|Nose", meta=(DisplayName="CTRL L nose wrinkleUpper")) float CTRL_L_nose_wrinkleUpper = 0.0f;
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="OffgridAI|MetaHuman Face|Nose", meta=(DisplayName="CTRL R nose wrinkleUpper")) float CTRL_R_nose_wrinkleUpper = 0.0f;
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="OffgridAI|MetaHuman Face|Nose", meta=(DisplayName="CTRL L nose")) FVector2D CTRL_L_nose = FVector2D::ZeroVector;
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="OffgridAI|MetaHuman Face|Nose", meta=(DisplayName="CTRL R nose")) FVector2D CTRL_R_nose = FVector2D::ZeroVector;
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="OffgridAI|MetaHuman Face|Mouth", meta=(DisplayName="CTRL L mouth upperLipRaise")) float CTRL_L_mouth_upperLipRaise = 0.0f;
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="OffgridAI|MetaHuman Face|Mouth", meta=(DisplayName="CTRL R mouth upperLipRaise")) float CTRL_R_mouth_upperLipRaise = 0.0f;
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="OffgridAI|MetaHuman Face|Mouth", meta=(DisplayName="CTRL L mouth sharpCornerPull")) float CTRL_L_mouth_sharpCornerPull = 0.0f;
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="OffgridAI|MetaHuman Face|Mouth", meta=(DisplayName="CTRL R mouth sharpCornerPull")) float CTRL_R_mouth_sharpCornerPull = 0.0f;
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="OffgridAI|MetaHuman Face|Mouth", meta=(DisplayName="CTRL L mouth cornerPull")) float CTRL_L_mouth_cornerPull = 0.0f;
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="OffgridAI|MetaHuman Face|Mouth", meta=(DisplayName="CTRL R mouth cornerPull")) float CTRL_R_mouth_cornerPull = 0.0f;
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="OffgridAI|MetaHuman Face|Mouth", meta=(DisplayName="CTRL L mouth dimple")) float CTRL_L_mouth_dimple = 0.0f;
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="OffgridAI|MetaHuman Face|Mouth", meta=(DisplayName="CTRL R mouth dimple")) float CTRL_R_mouth_dimple = 0.0f;
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="OffgridAI|MetaHuman Face|Mouth", meta=(DisplayName="CTRL L mouth cornerDepress")) float CTRL_L_mouth_cornerDepress = 0.0f;
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="OffgridAI|MetaHuman Face|Mouth", meta=(DisplayName="CTRL R mouth cornerDepress")) float CTRL_R_mouth_cornerDepress = 0.0f;
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="OffgridAI|MetaHuman Face|Mouth", meta=(DisplayName="CTRL L mouth stretch")) float CTRL_L_mouth_stretch = 0.0f;
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="OffgridAI|MetaHuman Face|Mouth", meta=(DisplayName="CTRL R mouth stretch")) float CTRL_R_mouth_stretch = 0.0f;
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="OffgridAI|MetaHuman Face|Mouth", meta=(DisplayName="CTRL L mouth lowerLipDepress")) float CTRL_L_mouth_lowerLipDepress = 0.0f;
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="OffgridAI|MetaHuman Face|Mouth", meta=(DisplayName="CTRL R mouth lowerLipDepress")) float CTRL_R_mouth_lowerLipDepress = 0.0f;
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="OffgridAI|MetaHuman Face|Mouth", meta=(DisplayName="CTRL L mouth towardsD")) float CTRL_L_mouth_towardsD = 0.0f;
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="OffgridAI|MetaHuman Face|Mouth", meta=(DisplayName="CTRL R mouth towardsD")) float CTRL_R_mouth_towardsD = 0.0f;
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="OffgridAI|MetaHuman Face|Jaw", meta=(DisplayName="CTRL L jaw chinCompress")) float CTRL_L_jaw_chinCompress = 0.0f;
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="OffgridAI|MetaHuman Face|Jaw", meta=(DisplayName="CTRL R jaw chinCompress")) float CTRL_R_jaw_chinCompress = 0.0f;
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="OffgridAI|MetaHuman Face|Neck", meta=(DisplayName="CTRL L neck mastoidContract")) float CTRL_L_neck_mastoidContract = 0.0f;
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="OffgridAI|MetaHuman Face|Neck", meta=(DisplayName="CTRL R neck mastoidContract")) float CTRL_R_neck_mastoidContract = 0.0f;
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="OffgridAI|MetaHuman Face|Mouth", meta=(DisplayName="CTRL L mouth towardsU")) float CTRL_L_mouth_towardsU = 0.0f;
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="OffgridAI|MetaHuman Face|Mouth", meta=(DisplayName="CTRL R mouth towardsU")) float CTRL_R_mouth_towardsU = 0.0f;
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="OffgridAI|MetaHuman Face|Mouth", meta=(DisplayName="CTRL L mouth lipBiteD")) float CTRL_L_mouth_lipBiteD = 0.0f;
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="OffgridAI|MetaHuman Face|Mouth", meta=(DisplayName="CTRL R mouth lipBiteD")) float CTRL_R_mouth_lipBiteD = 0.0f;
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="OffgridAI|MetaHuman Face|Mouth", meta=(DisplayName="CTRL L mouth lipsTogetherU")) float CTRL_L_mouth_lipsTogetherU = 0.0f;
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="OffgridAI|MetaHuman Face|Mouth", meta=(DisplayName="CTRL R mouth lipsTogetherU")) float CTRL_R_mouth_lipsTogetherU = 0.0f;
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="OffgridAI|MetaHuman Face|Tongue", meta=(DisplayName="CTRL C tongue press")) float CTRL_C_tongue_press = 0.0f;
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="OffgridAI|MetaHuman Face|Mouth", meta=(DisplayName="CTRL L mouth tightenD")) float CTRL_L_mouth_tightenD = 0.0f;
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="OffgridAI|MetaHuman Face|Mouth", meta=(DisplayName="CTRL R mouth tightenD")) float CTRL_R_mouth_tightenD = 0.0f;
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="OffgridAI|MetaHuman Face|Mouth", meta=(DisplayName="CTRL L mouth pressU")) float CTRL_L_mouth_pressU = 0.0f;
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="OffgridAI|MetaHuman Face|Mouth", meta=(DisplayName="CTRL R mouth pressU")) float CTRL_R_mouth_pressU = 0.0f;
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="OffgridAI|MetaHuman Face|Mouth", meta=(DisplayName="CTRL L mouth funnelD")) float CTRL_L_mouth_funnelD = 0.0f;
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="OffgridAI|MetaHuman Face|Mouth", meta=(DisplayName="CTRL R mouth funnelD")) float CTRL_R_mouth_funnelD = 0.0f;
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="OffgridAI|MetaHuman Face|Neck", meta=(DisplayName="CTRL L neck stretch")) float CTRL_L_neck_stretch = 0.0f;
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="OffgridAI|MetaHuman Face|Neck", meta=(DisplayName="CTRL R neck stretch")) float CTRL_R_neck_stretch = 0.0f;
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="OffgridAI|MetaHuman Face|Mouth", meta=(DisplayName="CTRL L mouth tightenU")) float CTRL_L_mouth_tightenU = 0.0f;
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="OffgridAI|MetaHuman Face|Mouth", meta=(DisplayName="CTRL R mouth tightenU")) float CTRL_R_mouth_tightenU = 0.0f;
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="OffgridAI|MetaHuman Face|Mouth", meta=(DisplayName="CTRL L mouth pressD")) float CTRL_L_mouth_pressD = 0.0f;
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="OffgridAI|MetaHuman Face|Mouth", meta=(DisplayName="CTRL R mouth pressD")) float CTRL_R_mouth_pressD = 0.0f;
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="OffgridAI|MetaHuman Face|Mouth", meta=(DisplayName="CTRL L mouth lipsTogetherD")) float CTRL_L_mouth_lipsTogetherD = 0.0f;
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="OffgridAI|MetaHuman Face|Mouth", meta=(DisplayName="CTRL R mouth lipsTogetherD")) float CTRL_R_mouth_lipsTogetherD = 0.0f;
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="OffgridAI|MetaHuman Face|Mouth", meta=(DisplayName="CTRL L mouth funnelU")) float CTRL_L_mouth_funnelU = 0.0f;
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="OffgridAI|MetaHuman Face|Mouth", meta=(DisplayName="CTRL R mouth funnelU")) float CTRL_R_mouth_funnelU = 0.0f;
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="OffgridAI|MetaHuman Face|Mouth", meta=(DisplayName="CTRL L mouth purseD")) float CTRL_L_mouth_purseD = 0.0f;
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="OffgridAI|MetaHuman Face|Mouth", meta=(DisplayName="CTRL R mouth purseD")) float CTRL_R_mouth_purseD = 0.0f;
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="OffgridAI|MetaHuman Face|Mouth", meta=(DisplayName="CTRL L mouth purseU")) float CTRL_L_mouth_purseU = 0.0f;
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="OffgridAI|MetaHuman Face|Mouth", meta=(DisplayName="CTRL R mouth purseU")) float CTRL_R_mouth_purseU = 0.0f;
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="OffgridAI|MetaHuman Face|Jaw", meta=(DisplayName="CTRL R jaw clench")) float CTRL_R_jaw_clench = 0.0f;
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="OffgridAI|MetaHuman Face|Jaw", meta=(DisplayName="CTRL L jaw clench")) float CTRL_L_jaw_clench = 0.0f;
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="OffgridAI|MetaHuman Face|Tongue", meta=(DisplayName="CTRL C Tongue Move")) FVector2D CTRL_C_tongue = FVector2D::ZeroVector;
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="OffgridAI|MetaHuman Face|Jaw", meta=(DisplayName="CTRL L jaw ChinRaiseD")) float CTRL_L_jaw_ChinRaiseD = 0.0f;
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="OffgridAI|MetaHuman Face|Jaw", meta=(DisplayName="CTRL R jaw ChinRaiseD")) float CTRL_R_jaw_ChinRaiseD = 0.0f;
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="OffgridAI|MetaHuman Face|Jaw", meta=(DisplayName="CTRL L jaw ChinRaiseU")) float CTRL_L_jaw_ChinRaiseU = 0.0f;
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="OffgridAI|MetaHuman Face|Jaw", meta=(DisplayName="CTRL R jaw ChinRaiseU")) float CTRL_R_jaw_ChinRaiseU = 0.0f;
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="OffgridAI|MetaHuman Face|Jaw", meta=(DisplayName="CTRL C jaw fwdBack")) float CTRL_C_jaw_fwdBack = 0.0f;
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="OffgridAI|MetaHuman Face|Mouth", meta=(DisplayName="CTRL L mouth lipsPressU")) float CTRL_L_mouth_lipsPressU = 0.0f;
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="OffgridAI|MetaHuman Face|Mouth", meta=(DisplayName="CTRL R mouth lipsPressU")) float CTRL_R_mouth_lipsPressU = 0.0f;
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="OffgridAI|MetaHuman Face|Tongue", meta=(DisplayName="CTRL C tongue inOut")) float CTRL_C_tongue_inOut = 0.0f;
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="OffgridAI|MetaHuman Face|Mouth", meta=(DisplayName="CTRL L mouth pushPullU")) float CTRL_L_mouth_pushPullU = 0.0f;
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="OffgridAI|MetaHuman Face|Mouth", meta=(DisplayName="CTRL R mouth pushPullU")) float CTRL_R_mouth_pushPullU = 0.0f;
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="OffgridAI|MetaHuman Face|Mouth", meta=(DisplayName="CTRL L mouth pushPullD")) float CTRL_L_mouth_pushPullD = 0.0f;
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="OffgridAI|MetaHuman Face|Mouth", meta=(DisplayName="CTRL R mouth pushPullD")) float CTRL_R_mouth_pushPullD = 0.0f;
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="OffgridAI|MetaHuman Face|Mouth", meta=(DisplayName="CTRL L mouth thicknessU")) float CTRL_L_mouth_thicknessU = 0.0f;
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="OffgridAI|MetaHuman Face|Mouth", meta=(DisplayName="CTRL R mouth thicknessU")) float CTRL_R_mouth_thicknessU = 0.0f;
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="OffgridAI|MetaHuman Face|Mouth", meta=(DisplayName="CTRL L mouth thicknessD")) float CTRL_L_mouth_thicknessD = 0.0f;
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="OffgridAI|MetaHuman Face|Mouth", meta=(DisplayName="CTRL R mouth thicknessD")) float CTRL_R_mouth_thicknessD = 0.0f;
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="OffgridAI|MetaHuman Face|Mouth", meta=(DisplayName="CTRL L mouth lipsRollD")) float CTRL_L_mouth_lipsRollD = 0.0f;
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="OffgridAI|MetaHuman Face|Mouth", meta=(DisplayName="CTRL R mouth lipsRollD")) float CTRL_R_mouth_lipsRollD = 0.0f;
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="OffgridAI|MetaHuman Face|Mouth", meta=(DisplayName="CTRL L mouth cornerSharpnessD")) float CTRL_L_mouth_cornerSharpnessD = 0.0f;
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="OffgridAI|MetaHuman Face|Mouth", meta=(DisplayName="CTRL R mouth cornerSharpnessD")) float CTRL_R_mouth_cornerSharpnessD = 0.0f;
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="OffgridAI|MetaHuman Face|Mouth", meta=(DisplayName="CTRL L mouth lipsRollU")) float CTRL_L_mouth_lipsRollU = 0.0f;
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="OffgridAI|MetaHuman Face|Mouth", meta=(DisplayName="CTRL R mouth lipsRollU")) float CTRL_R_mouth_lipsRollU = 0.0f;
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="OffgridAI|MetaHuman Face|Mouth", meta=(DisplayName="CTRL L mouth cornerSharpnessU")) float CTRL_L_mouth_cornerSharpnessU = 0.0f;
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="OffgridAI|MetaHuman Face|Mouth", meta=(DisplayName="CTRL R mouth cornerSharpnessU")) float CTRL_R_mouth_cornerSharpnessU = 0.0f;
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="OffgridAI|MetaHuman Face|Nose", meta=(DisplayName="CTRL L nose nasolabialDeepen")) float CTRL_L_nose_nasolabialDeepen = 0.0f;
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="OffgridAI|MetaHuman Face|Nose", meta=(DisplayName="CTRL R nose nasolabialDeepen")) float CTRL_R_nose_nasolabialDeepen = 0.0f;
};

USTRUCT(BlueprintType)
struct OFFGRIDAI_API FOffgridAIFacePoseSample
{
    GENERATED_BODY()

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="OffgridAI|MetaHuman Face")
    FName PoseID = NAME_None;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="OffgridAI|MetaHuman Face")
    float Weight = 0.0f;
};

DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FOffgridAIFaceDriverControlsUpdatedSignature, const TArray<FOffgridAIFaceDriverControlValue>&, ControlValues);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FOffgridAIFaceDriverPoseUpdatedSignature, const FOffgridAIMetaHumanFacePose&, FacePose);

UCLASS(ClassGroup=(OffgridAI), meta=(BlueprintSpawnableComponent))
class OFFGRIDAI_API UOffgridAIMetaHumanFaceDriverComponent : public UActorComponent
{
    GENERATED_BODY()

public:
    UOffgridAIMetaHumanFaceDriverComponent();
    virtual ~UOffgridAIMetaHumanFaceDriverComponent() override;

    virtual void BeginPlay() override;
    virtual void TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction) override;

    UFUNCTION(BlueprintCallable, Category="OffgridAI|MetaHuman Face")
    bool ReloadPoseLibraries();

    UFUNCTION(BlueprintCallable, Category="OffgridAI|MetaHuman Face")
    void SubmitVisemePose(FName PoseIDOrAlias, float Weight);

    UFUNCTION(BlueprintCallable, Category="OffgridAI|MetaHuman Face")
    // Layer 4 boundary. Input is an abstract performed-viseme frame from LineCoach.
    // This component may translate visemes to MetaHuman controls, blend viseme vs
    // emotion controls, and publish rig values. It must not alter text planning,
    // audio timing, event order, or viseme selection.
    void SubmitVisemePoseWeights(const TMap<FName, float>& PoseIDOrAliasWeights);

    UFUNCTION(BlueprintCallable, Category="OffgridAI|MetaHuman Face")
    void SubmitEmotion(FName EmotionName, float Magnitude, float OverallWeight = 1.0f);

    UFUNCTION(BlueprintCallable, Category="OffgridAI|MetaHuman Face")
    void ClearVisemes();

    // Immediate viseme reset used by LineCoach rest/pre-speech/end gates.
    // Unlike ClearVisemes(), this clears both target and current viseme weights
    // so stale mouth poses cannot decay after lipsync has explicitly gone idle.
    UFUNCTION(BlueprintCallable, Category="OffgridAI|MetaHuman Face")
    void ClearVisemesImmediate();


    UFUNCTION(BlueprintCallable, Category="OffgridAI|MetaHuman Face|Emotion")
    void ConfigureEmotionMouthAllowance(float InSpeechCriticalScaleDuringSpeech, float InMouthCornerScaleDuringSpeech, float InMouthCornerScaleDuringBilabial, float InSharedMouthScaleDuringSpeech, float InSpeechHoldSeconds, float InMouthFadeInSeconds, float InMouthFadeOutSeconds, float InFullMouthAfterSilenceSeconds);

    // Line-lifecycle speech suppression. When true, lower-mouth emotion remains
    // suppressed even if audio/viseme activity drops during sentence gaps.
    // This prevents emotional mouth poses from reasserting mid-line.
    UFUNCTION(BlueprintCallable, Category="OffgridAI|MetaHuman Face|Emotion")
    void SetLineSpeechMouthSuppressionActive(bool bActive);

    UFUNCTION(BlueprintCallable, Category="OffgridAI|MetaHuman Face")
    void ClearEmotion();

    UFUNCTION(BlueprintCallable, Category="OffgridAI|MetaHuman Face")
    void ForceNeutral();

    UFUNCTION(BlueprintCallable, BlueprintPure, Category="OffgridAI|MetaHuman Face")
    TArray<FOffgridAIFaceDriverControlValue> GetLatestControlValues() const { return LatestControlValues; }

    UFUNCTION(BlueprintCallable, BlueprintPure, Category="OffgridAI|MetaHuman Face")
    FOffgridAIMetaHumanFacePose GetLatestFacePose() const { return LatestFacePose; }

    UFUNCTION(BlueprintCallable, Category="OffgridAI|MetaHuman Face")
    TMap<FName, float> GetDebugActivePoseWeights() const { return DebugActivePoseWeights; }

    /** Blueprint event dispatcher for Actor/AnimBP bridge code. Bind this in BP_Ettore or a BP subclass. */
    UPROPERTY(BlueprintAssignable, Category="OffgridAI|MetaHuman Face")
    FOffgridAIFaceDriverControlsUpdatedSignature OnControlsUpdated;

    UPROPERTY(BlueprintAssignable, Category="OffgridAI|MetaHuman Face")
    FOffgridAIFaceDriverPoseUpdatedSignature OnFacePoseUpdated;

    /** Optional subclass hooks. Useful if you make a BP child of this component. */
    UFUNCTION(BlueprintImplementableEvent, Category="OffgridAI|MetaHuman Face")
    void ReceiveControlsUpdated(const TArray<FOffgridAIFaceDriverControlValue>& ControlValues);

    UFUNCTION(BlueprintImplementableEvent, Category="OffgridAI|MetaHuman Face")
    void ReceiveFacePoseUpdated(const FOffgridAIMetaHumanFacePose& FacePose);

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="OffgridAI|MetaHuman Face")
    FString VisemeLibraryRelativePath = TEXT("Face/MetaHumanVisemeLibrary.json");

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="OffgridAI|MetaHuman Face")
    FString EmotionLibraryRelativePath = TEXT("Face/MetaHumanEmotionLibrary.json");

    /**
     * Temporary lipsync-isolation switch. When false, the driver ignores all
     * submitted emotion expression poses and publishes viseme-only facial output.
     * Keep this disabled while evaluating v47 mouth timing/readability so upper
     * and lower face emotion controls cannot mask or bias speech shapes.
     */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="OffgridAI|MetaHuman Face|Debug")
    bool bEnableEmotionExpression = true;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="OffgridAI|MetaHuman Face", meta=(ClampMin="0.001", ClampMax="1.0"))
    float VisemeBlendInSeconds = 0.035f;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="OffgridAI|MetaHuman Face", meta=(ClampMin="0.001", ClampMax="1.0"))
    float VisemeBlendOutSeconds = 0.070f;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="OffgridAI|MetaHuman Face", meta=(ClampMin="0.001", ClampMax="3.0"))
    float EmotionBlendInSeconds = 0.180f;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="OffgridAI|MetaHuman Face", meta=(ClampMin="0.001", ClampMax="3.0"))
    float EmotionBlendOutSeconds = 0.450f;

    // Used only for direct non-neutral emotion-family switches, e.g. anger -> joy.
    // This crossfades the expression control targets slowly enough to avoid pose pops
    // while keeping neutral->emotion and emotion->neutral timing governed by the
    // regular EmotionBlendInSeconds / EmotionBlendOutSeconds values.
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="OffgridAI|MetaHuman Face", meta=(ClampMin="0.001", ClampMax="10.0"))
    float EmotionFamilyTransitionBlendSeconds = 3.0f;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="OffgridAI|MetaHuman Face", meta=(ClampMin="0.0", ClampMax="1.0"))
    float EmotionSpeechControlScaleDuringSpeech = 0.0f;

    // Version A: allow expression to involve mouth corners/stretch during speech,
    // but keep it subordinate to the current viseme. Speech-critical jaw/lip/tongue
    // controls remain heavily reduced; mouth-corner expression survives at a limited
    // scale unless a strong MBP/closed pose is active.
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="OffgridAI|MetaHuman Face", meta=(ClampMin="0.0", ClampMax="1.0"))
    float EmotionMouthCornerScaleDuringSpeech = 0.0f;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="OffgridAI|MetaHuman Face", meta=(ClampMin="0.0", ClampMax="1.0"))
    float EmotionMouthCornerScaleDuringBilabial = 0.0f;

    // Version B: mouth emotion is not binary-gated by speech. It remains
    // submissive while visemes are active, then ramps back in only after a
    // sustained silence hold so expression does not pop between syllables.
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="OffgridAI|MetaHuman Face", meta=(ClampMin="0.0", ClampMax="1.0"))
    float EmotionSharedMouthScaleDuringSpeech = 0.0f;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="OffgridAI|MetaHuman Face", meta=(ClampMin="0.0", ClampMax="2.0"))
    float EmotionSpeechHoldSeconds = 1.5f;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="OffgridAI|MetaHuman Face", meta=(ClampMin="0.001", ClampMax="10.0"))
    float EmotionMouthFadeInSeconds = 3.0f;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="OffgridAI|MetaHuman Face", meta=(ClampMin="0.001", ClampMax="2.0"))
    float EmotionMouthFadeOutSeconds = 0.01f;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="OffgridAI|MetaHuman Face", meta=(ClampMin="0.01", ClampMax="3.0"))
    float EmotionFullMouthAfterSilenceSeconds = 1.5f;

    // Runtime-sampled from MetaHumanEmotionLibrary.json speech_modifiers. These are
    // deliberately not designer constants here; the JSON owns how much mouth emotion
    // survives while speech/lipsync is active for each emotion stage.
    float CurrentEmotionSpeechCriticalMouthScale = 0.04f;
    float CurrentEmotionSharedMouthScale = 0.08f;
    float CurrentEmotionMouthCornerScale = 0.18f;
    float CurrentEmotionMouthCornerBilabialScale = 0.04f;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, AdvancedDisplay, Category="OffgridAI|MetaHuman Face", meta=(ClampMin="0.0", ClampMax="2.0"))
    float VisemeStrength = 1.0f;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, AdvancedDisplay, Category="OffgridAI|MetaHuman Face", meta=(ClampMin="0.0", ClampMax="2.0"))
    float EmotionStrength = 1.0f;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="OffgridAI|MetaHuman Face|Debug")
    bool bLogDiagnostics = false;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="OffgridAI|MetaHuman Face|Debug", meta=(ClampMin="0.0", ClampMax="5.0"))
    float DiagnosticLogIntervalSeconds = 0.5f;

private:
    struct FRuntimePose
    {
        FName PoseID = NAME_None;
        TArray<FOffgridAIFaceDriverControlValue> Controls;
    };

    bool LoadVisemeLibrary(const FString& AbsolutePath);
    bool ParseControlsArray(const TArray<TSharedPtr<FJsonValue>>& Values, TArray<FOffgridAIFaceDriverControlValue>& OutControls) const;
    void AddWeightedControls(const TArray<FOffgridAIFaceDriverControlValue>& Controls, float Weight, bool bEmotionLayer, TMap<FName, FOffgridAIFaceDriverControlValue>& InOutValues, FName SourcePoseID = NAME_None) const;
    void ResolveVisemeTargets(TArray<FOffgridAIFacePoseSample>& OutSamples) const;
    void SmoothControlMapToward(const TMap<FName, FOffgridAIFaceDriverControlValue>& TargetControls, float DeltaTimeSeconds, float BlendInSeconds, float BlendOutSeconds, TMap<FName, FOffgridAIFaceDriverControlValue>& InOutSmoothedControls) const;
    void ClampLowerFaceEmotionSmoothingForMagnitudeRetarget(FName EmotionName, float NewMagnitude);
    void ScaleControlMap(TMap<FName, FOffgridAIFaceDriverControlValue>& InOutControls, float Scale) const;
    void PublishControlValues(const TMap<FName, FOffgridAIFaceDriverControlValue>& TargetControls, float DeltaTimeSeconds, bool bImmediate = false);
    void ProjectControlValueToFacePose(const FOffgridAIFaceDriverControlValue& Value, FOffgridAIMetaHumanFacePose& InOutPose) const;
    void PublishNeutralZeros();
    float StepBlend(float Current, float Target, float DeltaTime, float InSeconds, float OutSeconds) const;
    bool IsSpeechCriticalControl(FName ControlName) const;
    bool IsMouthCornerExpressionControl(FName ControlName) const;
    bool IsSharedMouthExpressionControl(FName ControlName) const;
    bool IsBilabialVisemePose(FName PoseID) const;
    void UpdateEmotionMouthAllowance(float DeltaTimeSeconds, float SpeechActivity);
    float ComputeEmotionControlScaleDuringSpeech(FName ControlName, float SpeechActivity, float BilabialActivity) const;
    bool ControlValuesChanged(const TArray<FOffgridAIFaceDriverControlValue>& A, const TArray<FOffgridAIFaceDriverControlValue>& B) const;

    TMap<FName, FRuntimePose> VisemePoses;
    TMap<FName, TArray<FName>> VisemeAliases;

    TMap<FName, float> TargetVisemeWeights;
    TMap<FName, float> CurrentVisemeWeights;
    TMap<FName, FOffgridAIFaceDriverControlValue> SmoothedEmotionControlValues;

    // Lower-face emotional controls are smoothed separately so a newly selected
    // emotion can register quickly in the brows/eyes/cheeks while the
    // lipsync-owned mouth area eases toward the new expression more gradually.
    TMap<FName, FOffgridAIFaceDriverControlValue> SmoothedLowerFaceEmotionControlValues;

    // Last externally submitted emotion target. Used only to keep the slower
    // lower-face smoother from continuing toward an obsolete peak after CM
    // retargets the same emotion down to its relaxed 0.2 state.
    FName LastSubmittedEmotionName = NAME_None;
    float LastSubmittedEmotionMagnitude = 0.0f;

    TMap<FName, FOffgridAIFaceDriverControlValue> SmoothedControlValues;

    float EmotionSecondsSinceSpeechActivity = 999.0f;
    bool bLineSpeechMouthSuppressionActive = false;
    float EmotionMouthSilenceBlend = 1.0f;

    FOffgridAIEmotionExpression* EmotionExpression = nullptr;

    TArray<FOffgridAIFaceDriverControlValue> LatestControlValues;
    FOffgridAIMetaHumanFacePose LatestFacePose;
    TMap<FName, float> DebugActivePoseWeights;
    float TimeSinceLastDiagnosticLog = 0.0f;
};
