#pragma once

#include "CoreMinimal.h"

struct OFFGRIDAI_API FOffgridAILatencyMark
{
    FString Name;
    double TimeSeconds = 0.0;
    FString Detail;
};

struct OFFGRIDAI_API FOffgridAIMetricSample
{
    FString Name;
    double ValueMs = 0.0;
};

struct OFFGRIDAI_API FOffgridAIMetricWindow
{
    double LastMs = 0.0;
    double MedianMs = 0.0;
    double P90Ms = 0.0;
    int32 SampleCount = 0;
};

class OFFGRIDAI_API FOffgridAIMetricAccumulator
{
public:
    explicit FOffgridAIMetricAccumulator(int32 InMaxSamplesPerMetric = 100);

    void Reset();
    void SetMaxSamplesPerMetric(int32 InMaxSamplesPerMetric);
    void AddSample(const FString& MetricName, double ValueMs);
    bool GetWindow(const FString& MetricName, FOffgridAIMetricWindow& OutWindow) const;
    FString BuildLogSummary() const;

private:
    static double Percentile(TArray<double> Values, double Percentile01);

private:
    int32 MaxSamplesPerMetric = 100;
    TMap<FString, TArray<double>> SamplesByMetric;
};

class OFFGRIDAI_API FOffgridAITurnLatencyTrace
{
public:
    void Reset();
    void BeginTurn(int32 InTurnIndex, const FGuid& InConversationID, FName InPlayerID);

    bool IsActive() const { return bActive; }
    bool MatchesConversation(const FGuid& InConversationID) const { return bActive && ConversationID == InConversationID; }
    bool HasFirstMark(const FString& Name) const;
    bool TryGetDeltaMs(const FString& StartMark, const FString& EndMark, double& OutMs) const;
    void GetCanonicalMetricSamples(TArray<FOffgridAIMetricSample>& OutSamples) const;

    void Mark(const FString& Name, const FString& Detail = FString());
    void SetCurrentLine(FName InNPCID, FName InLineID);
    void EmitAndReset(const FString& Outcome);

private:
    const FOffgridAILatencyMark* FindFirstMark(const FString& Name) const;
    FString BuildSummary(const FString& Outcome) const;
    FString BuildTimelineJson(const FString& Outcome) const;
    static FString FormatMetric(const FString& Name, double ValueMs);

private:
    bool bActive = false;
    int32 TurnIndex = 0;
    FGuid ConversationID;
    FName PlayerID = NAME_None;
    FName CurrentNPCID = NAME_None;
    FName CurrentLineID = NAME_None;
    double StartedAtSeconds = 0.0;
    TArray<FOffgridAILatencyMark> Marks;
};
