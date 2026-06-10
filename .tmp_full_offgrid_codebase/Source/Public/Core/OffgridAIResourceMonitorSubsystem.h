#pragma once

#include "CoreMinimal.h"
#include "Subsystems/GameInstanceSubsystem.h"
#include "Tickable.h"
#include "Core/OffgridAILatencyTelemetry.h"
#include "OffgridAIResourceMonitorSubsystem.generated.h"

USTRUCT(BlueprintType)
struct OFFGRIDAI_API FOffgridAIProcessResourceStats
{
    GENERATED_BODY()

    UPROPERTY(BlueprintReadOnly, Category = "OffgridAI|Resources")
    bool bFound = false;

    UPROPERTY(BlueprintReadOnly, Category = "OffgridAI|Resources")
    int32 ProcessId = 0;

    UPROPERTY(BlueprintReadOnly, Category = "OffgridAI|Resources")
    float CPUPercent = 0.0f;

    UPROPERTY(BlueprintReadOnly, Category = "OffgridAI|Resources")
    float RAMUsedMB = 0.0f;
};


USTRUCT(BlueprintType)
struct OFFGRIDAI_API FOffgridAIPerformanceMetricStats
{
    GENERATED_BODY()

    UPROPERTY(BlueprintReadOnly, Category = "OffgridAI|Performance")
    bool bHasSamples = false;

    UPROPERTY(BlueprintReadOnly, Category = "OffgridAI|Performance")
    float LastMs = 0.0f;

    UPROPERTY(BlueprintReadOnly, Category = "OffgridAI|Performance")
    float MedianMs = 0.0f;

    UPROPERTY(BlueprintReadOnly, Category = "OffgridAI|Performance")
    float P90Ms = 0.0f;

    UPROPERTY(BlueprintReadOnly, Category = "OffgridAI|Performance")
    int32 SampleCount = 0;
};

USTRUCT(BlueprintType)
struct OFFGRIDAI_API FOffgridAIResourceStats
{
    GENERATED_BODY()

    UPROPERTY(BlueprintReadOnly, Category = "OffgridAI|Resources")
    float SystemCPUPercent = 0.0f;

    UPROPERTY(BlueprintReadOnly, Category = "OffgridAI|Resources")
    float SystemRAMUsedGB = 0.0f;

    UPROPERTY(BlueprintReadOnly, Category = "OffgridAI|Resources")
    float SystemRAMTotalGB = 0.0f;

    UPROPERTY(BlueprintReadOnly, Category = "OffgridAI|Resources")
    float GPUPercent = 0.0f;

    UPROPERTY(BlueprintReadOnly, Category = "OffgridAI|Resources")
    float VRAMUsedGB = 0.0f;

    UPROPERTY(BlueprintReadOnly, Category = "OffgridAI|Resources")
    float VRAMBudgetGB = 0.0f;

    UPROPERTY(BlueprintReadOnly, Category = "OffgridAI|Resources")
    float VRAMTotalGB = 0.0f;

    UPROPERTY(BlueprintReadOnly, Category = "OffgridAI|Resources")
    FOffgridAIProcessResourceStats UnrealProcess;

    UPROPERTY(BlueprintReadOnly, Category = "OffgridAI|Resources")
    FOffgridAIProcessResourceStats ASRService;

    UPROPERTY(BlueprintReadOnly, Category = "OffgridAI|Resources")
    FOffgridAIProcessResourceStats LLMService;

    UPROPERTY(BlueprintReadOnly, Category = "OffgridAI|Resources")
    FOffgridAIProcessResourceStats TTSService;

    UPROPERTY(BlueprintReadOnly, Category = "OffgridAI|Resources")
    float ASRServiceGPUPercent = 0.0f;

    UPROPERTY(BlueprintReadOnly, Category = "OffgridAI|Resources")
    float LLMServiceGPUPercent = 0.0f;

    UPROPERTY(BlueprintReadOnly, Category = "OffgridAI|Resources")
    float TTSServiceGPUPercent = 0.0f;

    UPROPERTY(BlueprintReadOnly, Category = "OffgridAI|Resources")
    float ASRServiceVRAMMB = 0.0f;

    UPROPERTY(BlueprintReadOnly, Category = "OffgridAI|Resources")
    float LLMServiceVRAMMB = 0.0f;

    UPROPERTY(BlueprintReadOnly, Category = "OffgridAI|Resources")
    float TTSServiceVRAMMB = 0.0f;

    UPROPERTY(BlueprintReadOnly, Category = "OffgridAI|Performance")
    bool bHasRoundTripTime = false;

    UPROPERTY(BlueprintReadOnly, Category = "OffgridAI|Performance")
    float RoundTripTimeMs = 0.0f;

    UPROPERTY(BlueprintReadOnly, Category = "OffgridAI|Performance")
    FOffgridAIPerformanceMetricStats RoundTripLatency;

    UPROPERTY(BlueprintReadOnly, Category = "OffgridAI|Performance")
    FOffgridAIPerformanceMetricStats PerceivedLatency;

    UPROPERTY(BlueprintReadOnly, Category = "OffgridAI|Performance")
    FOffgridAIPerformanceMetricStats ASRLatency;

    UPROPERTY(BlueprintReadOnly, Category = "OffgridAI|Performance")
    FOffgridAIPerformanceMetricStats LLMTotalLatency;

    UPROPERTY(BlueprintReadOnly, Category = "OffgridAI|Performance")
    FOffgridAIPerformanceMetricStats TTSFirstAudioLatency;

    UPROPERTY(BlueprintReadOnly, Category = "OffgridAI|Performance")
    FOffgridAIPerformanceMetricStats PlaybackDelay;
};

UCLASS()
class OFFGRIDAI_API UOffgridAIResourceMonitorSubsystem : public UGameInstanceSubsystem, public FTickableGameObject
{
    GENERATED_BODY()

public:
    virtual void Initialize(FSubsystemCollectionBase& Collection) override;
    virtual void Deinitialize() override;

    virtual void Tick(float DeltaTime) override;
    virtual bool IsTickable() const override;
    virtual TStatId GetStatId() const override;

    UFUNCTION(BlueprintCallable, Category = "OffgridAI|Resources")
    void SetResourceSampleInterval(float InSampleIntervalSeconds);

    UFUNCTION(BlueprintPure, Category = "OffgridAI|Resources")
    FOffgridAIResourceStats GetLatestResourceStats() const;

    UFUNCTION(BlueprintCallable, Category = "OffgridAI|Resources")
    FOffgridAIResourceStats SampleResourceStatsNow();

    UFUNCTION(BlueprintCallable, Category = "OffgridAI|Performance")
    void SetLatestRoundTripTimeMs(float InRoundTripTimeMs);

    void SetLatestPerformanceMetricStats(const FOffgridAIMetricAccumulator& MetricAccumulator);

private:
    void SampleResourceStats();

    void LatchAndResetTurnResourcePeaksIfNeeded(int32 InPerceivedLatencySampleCount);
    void ApplyTurnResourcePeakPresentation();

    float CurrentTurnASRServiceCPUPeakPercent = 0.0f;
    float CurrentTurnLLMServiceCPUPeakPercent = 0.0f;
    float CurrentTurnTTSServiceCPUPeakPercent = 0.0f;

    float LastTurnASRServiceCPUPeakPercent = 0.0f;
    float LastTurnLLMServiceCPUPeakPercent = 0.0f;
    float LastTurnTTSServiceCPUPeakPercent = 0.0f;

    float CurrentTurnASRServiceGPUPeakPercent = 0.0f;
    float CurrentTurnLLMServiceGPUPeakPercent = 0.0f;
    float CurrentTurnTTSServiceGPUPeakPercent = 0.0f;

    float LastTurnASRServiceGPUPeakPercent = 0.0f;
    float LastTurnLLMServiceGPUPeakPercent = 0.0f;
    float LastTurnTTSServiceGPUPeakPercent = 0.0f;

    int32 LastLatchedPerceivedLatencySampleCount = 0;

    FOffgridAIResourceStats LatestStats;
    float SampleIntervalSeconds = 0.5f;
    double SecondsUntilNextSample = 0.0;
    bool bInitialized = false;
};
