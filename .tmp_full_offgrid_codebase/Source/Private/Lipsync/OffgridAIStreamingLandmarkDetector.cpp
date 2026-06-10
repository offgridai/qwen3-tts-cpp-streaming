#include "Lipsync/OffgridAIStreamingLandmarkDetector.h"
#include <cstring>

namespace
{
    static float Clamp01(float V)
    {
        return FMath::Clamp(V, 0.0f, 1.0f);
    }

    struct FWindowFeatures
    {
        float Rms = 0.0f;
        float MeanAbs = 0.0f;
        float Zcr = 0.0f;
        float DiffMean = 0.0f;      // crude high-frequency / frication proxy
        float LowDominance = 0.0f;  // voiced/rounded proxy: energy without much fast variation
    };

    struct FClassProposal
    {
        EOffgridAIAudioLandmarkClass Class = EOffgridAIAudioLandmarkClass::Unknown;
        float Confidence = 0.0f;
        FName RejectReason = NAME_None;
    };

    static FWindowFeatures ComputeFeatures(const TArray<int16>& Samples, int32 Start, int32 Count)
    {
        FWindowFeatures Out;
        if (Count <= 1 || Start < 0 || Start + Count > Samples.Num())
        {
            return Out;
        }

        double SumSq = 0.0;
        double SumAbs = 0.0;
        double SumDiff = 0.0;
        int32 Crossings = 0;
        int16 Prev = Samples[Start];
        float PrevN = static_cast<float>(Prev) / 32768.0f;
        for (int32 I = 0; I < Count; ++I)
        {
            const int16 S = Samples[Start + I];
            const float N = static_cast<float>(S) / 32768.0f;
            SumSq += static_cast<double>(N) * static_cast<double>(N);
            SumAbs += FMath::Abs(N);
            if (I > 0)
            {
                SumDiff += FMath::Abs(N - PrevN);
                if ((S >= 0 && Prev < 0) || (S < 0 && Prev >= 0))
                {
                    ++Crossings;
                }
            }
            Prev = S;
            PrevN = N;
        }

        Out.Rms = FMath::Sqrt(static_cast<float>(SumSq / Count));
        Out.MeanAbs = static_cast<float>(SumAbs / Count);
        Out.Zcr = static_cast<float>(Crossings) / static_cast<float>(Count - 1);
        Out.DiffMean = static_cast<float>(SumDiff) / static_cast<float>(Count - 1);
        Out.LowDominance = Out.MeanAbs / FMath::Max(Out.DiffMean, 0.0025f);
        return Out;
    }

    static float RefractorySecondsForClass(EOffgridAIAudioLandmarkClass Class, float PrerollSec)
    {
        switch (Class)
        {
        case EOffgridAIAudioLandmarkClass::MBP:
            return FMath::Clamp(PrerollSec * 0.70f, 0.080f, 0.130f);
        case EOffgridAIAudioLandmarkClass::FV:
        case EOffgridAIAudioLandmarkClass::SHCH:
            return FMath::Clamp(PrerollSec * 0.75f, 0.090f, 0.140f);
        case EOffgridAIAudioLandmarkClass::WOO:
            return FMath::Clamp(PrerollSec * 1.20f, 0.150f, 0.240f);
        case EOffgridAIAudioLandmarkClass::VowelOpen:
        case EOffgridAIAudioLandmarkClass::VowelFront:
            return FMath::Clamp(PrerollSec * 1.05f, 0.130f, 0.220f);
        default:
            return FMath::Clamp(PrerollSec * 0.50f, 0.060f, 0.120f);
        }
    }

    static bool IsFarFromExisting(const TArray<FOffgridAIAudioLandmarkCandidate>& Candidates, EOffgridAIAudioLandmarkClass Class, float TimeSec, float PrerollSec)
    {
        const float MinSpacingSec = RefractorySecondsForClass(Class, PrerollSec);
        for (const FOffgridAIAudioLandmarkCandidate& C : Candidates)
        {
            if (C.Class == Class && FMath::Abs(C.TimeSec - TimeSec) < MinSpacingSec)
            {
                return false;
            }
        }
        return true;
    }

    static FClassProposal ProposeClass(const FWindowFeatures& Prev, const FWindowFeatures& Cur, const FWindowFeatures& Next, bool bOracle)
    {
        FClassProposal Best;
        const float RiseFromPrev = Cur.Rms - Prev.Rms;
        const float FallToNext = Cur.Rms - Next.Rms;
        const float FricationScore = Cur.DiffMean * 7.0f + Cur.Zcr * 2.0f + Cur.Rms * 0.8f;
        const float VoicedScore = Cur.Rms * 2.8f + Cur.MeanAbs * 1.8f;
        const float RoundedScore = Cur.LowDominance * 0.22f + Cur.Rms * 1.5f - Cur.Zcr * 1.2f;

        auto Consider = [&Best](EOffgridAIAudioLandmarkClass Class, float Confidence)
        {
            if (Confidence > Best.Confidence)
            {
                Best.Class = Class;
                Best.Confidence = Confidence;
            }
        };

        // MBP: a low-energy closure/release pocket.  Keep this permissive enough
        // to capture visible labials, but it is still only support evidence.
        if ((Cur.Rms < 0.024f && (RiseFromPrev > 0.010f || FallToNext < -0.010f || Prev.Rms - Cur.Rms > 0.006f))
            || (Prev.Rms < 0.024f && RiseFromPrev > 0.013f))
        {
            Consider(EOffgridAIAudioLandmarkClass::MBP, Clamp01((0.034f - Cur.Rms) * 8.0f + FMath::Max(RiseFromPrev, 0.0f) * 11.0f));
        }

        if (Cur.Rms > 0.010f && Cur.Zcr > 0.090f && Cur.DiffMean > 0.010f)
        {
            Consider(EOffgridAIAudioLandmarkClass::FV, Clamp01((FricationScore - 0.19f) * 2.0f));
        }
        if (Cur.Rms > 0.014f && Cur.Zcr > 0.125f && Cur.DiffMean > 0.014f)
        {
            Consider(EOffgridAIAudioLandmarkClass::SHCH, Clamp01((FricationScore - 0.28f) * 1.8f));
        }

        // G05: WOO used to mean almost any smooth voiced audio, causing floods
        // through "hello there".  Require a low-ZCR rounded pocket with contrast
        // against neighboring windows.  Oracle mode may use both sides; streaming
        // mode remains causal and relies mainly on entry contrast from Prev.
        const float PrevRounded = Prev.LowDominance;
        const float NextRounded = Next.LowDominance;
        const float EntryContrast = Cur.LowDominance - PrevRounded;
        const float LocalPeakContrast = Cur.LowDominance - FMath::Max(PrevRounded, NextRounded);
        const bool bRoundedEntry = EntryContrast > 0.30f || Cur.LowDominance > PrevRounded * 1.22f;
        const bool bRoundedPeak = LocalPeakContrast > 0.18f || Cur.LowDominance > FMath::Max(PrevRounded, NextRounded) * 1.12f;
        const bool bWooContrastOk = bOracle ? (bRoundedEntry || bRoundedPeak) : bRoundedEntry;
        if (Cur.Rms > 0.018f && Cur.Zcr < 0.060f && Cur.LowDominance > 1.70f && bWooContrastOk)
        {
            Consider(EOffgridAIAudioLandmarkClass::WOO, Clamp01((RoundedScore - 0.28f) * 1.45f + FMath::Max(EntryContrast, LocalPeakContrast) * 0.20f));
        }

        if (Cur.Rms > 0.022f && Cur.Zcr >= 0.035f && Cur.Zcr < 0.130f)
        {
            Consider(EOffgridAIAudioLandmarkClass::VowelOpen, Clamp01(VoicedScore + (0.13f - FMath::Abs(Cur.Zcr - 0.075f)) * 0.65f - 0.05f));
        }
        if (Cur.Rms > 0.018f && Cur.Zcr >= 0.080f && Cur.Zcr < 0.210f)
        {
            Consider(EOffgridAIAudioLandmarkClass::VowelFront, Clamp01(VoicedScore + (0.17f - FMath::Abs(Cur.Zcr - 0.135f)) * 0.60f - 0.05f));
        }

        if (Best.Class == EOffgridAIAudioLandmarkClass::Unknown || Best.Confidence <= 0.0f)
        {
            Best.RejectReason = FName(TEXT("G05_no_class_threshold"));
        }
        return Best;
    }
}

void FOffgridAIStreamingLandmarkDetector::Reset()
{
    MonoPCM16.Reset();
    Candidates.Reset();
    RawWindows.Reset();
    SampleRateHz = 0;
    LastAnalyzedFrame = 0;
    StreamStartSample = 0;
    bHasStreamStartSample = false;
    PrerollSec = 0.150f;
}

void FOffgridAIStreamingLandmarkDetector::Configure(float InPrerollSec)
{
    PrerollSec = FMath::Max(0.001f, InPrerollSec);
}

const char* FOffgridAIStreamingLandmarkDetector::ToString(EOffgridAIAudioLandmarkClass Class)
{
    switch (Class)
    {
    case EOffgridAIAudioLandmarkClass::MBP: return "MBP";
    case EOffgridAIAudioLandmarkClass::FV: return "FV";
    case EOffgridAIAudioLandmarkClass::WOO: return "WOO";
    case EOffgridAIAudioLandmarkClass::SHCH: return "SHCH";
    case EOffgridAIAudioLandmarkClass::VowelOpen: return "VowelOpen";
    case EOffgridAIAudioLandmarkClass::VowelFront: return "VowelFront";
    default: return "Unknown";
    }
}

EOffgridAIAudioLandmarkClass FOffgridAIStreamingLandmarkDetector::LandmarkClassForPose(FName PoseID)
{
    if (PoseID == FName(TEXT("22_MBP")))
    {
        return EOffgridAIAudioLandmarkClass::MBP;
    }
    if (PoseID == FName(TEXT("20_FV")) || PoseID == FName(TEXT("19_FV-Or-")) || PoseID == FName(TEXT("21_FV-Ee-")))
    {
        return EOffgridAIAudioLandmarkClass::FV;
    }
    if (PoseID == FName(TEXT("14_ChJjSh")))
    {
        return EOffgridAIAudioLandmarkClass::SHCH;
    }
    if (PoseID == FName(TEXT("12_Ww-Oo-")) || PoseID == FName(TEXT("16_Ww-Ew-")) || PoseID == FName(TEXT("11_Oo")) || PoseID == FName(TEXT("10_Or")))
    {
        return EOffgridAIAudioLandmarkClass::WOO;
    }
    if (PoseID == FName(TEXT("07_Aa")) || PoseID == FName(TEXT("08_Ah")) || PoseID == FName(TEXT("09_Oh")))
    {
        return EOffgridAIAudioLandmarkClass::VowelOpen;
    }
    if (PoseID == FName(TEXT("03_Ee")) || PoseID == FName(TEXT("04_Ih")) || PoseID == FName(TEXT("05_Ay")) || PoseID == FName(TEXT("06_Eh")))
    {
        return EOffgridAIAudioLandmarkClass::VowelFront;
    }
    return EOffgridAIAudioLandmarkClass::Unknown;
}

void FOffgridAIStreamingLandmarkDetector::PushPCM16(const TArray<uint8>& PCMChunk, int32 BytesToUse, int32 SampleRate, int32 NumChannels, int64 ChunkStartSample)
{
    AppendMonoPCM16(PCMChunk, BytesToUse, SampleRate, NumChannels, ChunkStartSample);
    AnalyzeNewFrames(false);
}

void FOffgridAIStreamingLandmarkDetector::AppendMonoPCM16(const TArray<uint8>& PCMChunk, int32 BytesToUse, int32 SampleRate, int32 NumChannels, int64 ChunkStartSample)
{
    if (SampleRate <= 0 || NumChannels <= 0 || BytesToUse <= 0)
    {
        return;
    }
    if (SampleRateHz != SampleRate)
    {
        MonoPCM16.Reset();
        Candidates.Reset();
        RawWindows.Reset();
        LastAnalyzedFrame = 0;
        SampleRateHz = SampleRate;
        bHasStreamStartSample = false;
        StreamStartSample = 0;
    }

    const int32 BytesPerFrame = NumChannels * static_cast<int32>(sizeof(int16));
    const int32 FrameCount = FMath::Min(BytesToUse, PCMChunk.Num()) / FMath::Max(1, BytesPerFrame);
    if (FrameCount <= 0)
    {
        return;
    }

    if (ChunkStartSample >= 0)
    {
        const int64 ExpectedStartSample = bHasStreamStartSample ? StreamStartSample + MonoPCM16.Num() : ChunkStartSample;
        if (!bHasStreamStartSample || FMath::Abs(static_cast<int32>(ChunkStartSample - ExpectedStartSample)) > FMath::Max(8, SampleRateHz / 1000))
        {
            MonoPCM16.Reset();
            Candidates.Reset();
            RawWindows.Reset();
            LastAnalyzedFrame = 0;
            StreamStartSample = ChunkStartSample;
            bHasStreamStartSample = true;
        }
    }
    else if (!bHasStreamStartSample)
    {
        StreamStartSample = 0;
        bHasStreamStartSample = true;
    }

    const uint8* Data = PCMChunk.GetData();
    MonoPCM16.Reserve(MonoPCM16.Num() + FrameCount);
    for (int32 FrameIndex = 0; FrameIndex < FrameCount; ++FrameIndex)
    {
        int32 Sum = 0;
        for (int32 ChannelIndex = 0; ChannelIndex < NumChannels; ++ChannelIndex)
        {
            const int32 ByteOffset = (FrameIndex * NumChannels + ChannelIndex) * static_cast<int32>(sizeof(int16));
            int16 Sample = 0;
            std::memcpy(&Sample, Data + ByteOffset, sizeof(int16));
            Sum += static_cast<int32>(Sample);
        }
        MonoPCM16.Add(static_cast<int16>(Sum / NumChannels));
    }
}

bool FOffgridAIStreamingLandmarkDetector::AddCandidate(EOffgridAIAudioLandmarkClass Class, float TimeSec, float Confidence, float WindowStartSec, float WindowEndSec, bool bOracle, FName& OutRejectReason)
{
    if (Class == EOffgridAIAudioLandmarkClass::Unknown || Confidence <= 0.0f)
    {
        OutRejectReason = FName(TEXT("G05_no_class_threshold"));
        return false;
    }
    if (!IsFarFromExisting(Candidates, Class, TimeSec, PrerollSec))
    {
        OutRejectReason = FName(TEXT("G05_refractory_duplicate"));
        return false;
    }

    FOffgridAIAudioLandmarkCandidate C;
    C.Class = Class;
    C.TimeSec = TimeSec;
    C.Confidence = Clamp01(Confidence);
    C.WindowStartSec = WindowStartSec;
    C.WindowEndSec = WindowEndSec;
    C.AvailableAtSec = bOracle ? 0.0f : WindowEndSec;
    C.bOracle = bOracle;
    Candidates.Add(C);
    OutRejectReason = NAME_None;
    return true;
}

void FOffgridAIStreamingLandmarkDetector::AddRawWindow(const FOffgridAIAudioLandmarkRawWindow& RawWindow)
{
    // Keep diagnostics bounded for long streaming sessions while retaining the
    // recent causal history needed to understand accepted/suppressed landmarks.
    constexpr int32 MaxRawWindows = 20000;
    RawWindows.Add(RawWindow);
    if (RawWindows.Num() > MaxRawWindows)
    {
        RawWindows.RemoveAt(0, RawWindows.Num() - MaxRawWindows, EAllowShrinking::No);
    }
}

void FOffgridAIStreamingLandmarkDetector::AnalyzeNewFrames(bool bOracle, int32 AnalysisLimitFrame)
{
    if (SampleRateHz <= 0)
    {
        return;
    }

    const float WindowSec = FMath::Clamp(PrerollSec / 3.0f, 0.030f, 0.080f);
    const float HopSec = FMath::Clamp(PrerollSec / 15.0f, 0.006f, 0.015f);
    const int32 WindowFrames = FMath::Max(8, FMath::RoundToInt(SampleRateHz * WindowSec));
    const int32 HopFrames = FMath::Max(4, FMath::RoundToInt(SampleRateHz * HopSec));
    const int32 LimitFrame = AnalysisLimitFrame == INDEX_NONE ? MonoPCM16.Num() : FMath::Min(AnalysisLimitFrame, MonoPCM16.Num());

    int32 StartFrame = FMath::Max(0, LastAnalyzedFrame);
    for (; StartFrame + WindowFrames <= LimitFrame; StartFrame += HopFrames)
    {
        const FWindowFeatures Prev = ComputeFeatures(MonoPCM16, FMath::Max(0, StartFrame - WindowFrames), WindowFrames);
        const FWindowFeatures Cur = ComputeFeatures(MonoPCM16, StartFrame, WindowFrames);
        const FWindowFeatures Next = (StartFrame + WindowFrames * 2 <= MonoPCM16.Num())
            ? ComputeFeatures(MonoPCM16, StartFrame + WindowFrames, WindowFrames)
            : FWindowFeatures();

        const float WindowStartSec = static_cast<float>(StreamStartSample + StartFrame) / SampleRateHz;
        const float WindowEndSec = static_cast<float>(StreamStartSample + StartFrame + WindowFrames) / SampleRateHz;
        const float CenterSec = (WindowStartSec + WindowEndSec) * 0.5f;
        const float TimeSec = bOracle ? CenterSec : WindowEndSec;

        const FClassProposal Proposal = ProposeClass(Prev, Cur, Next, bOracle);
        FName RejectReason = Proposal.RejectReason;
        const bool bAccepted = AddCandidate(Proposal.Class, TimeSec, Proposal.Confidence, WindowStartSec, WindowEndSec, bOracle, RejectReason);

        FOffgridAIAudioLandmarkRawWindow Raw;
        Raw.WindowStartSec = WindowStartSec;
        Raw.WindowEndSec = WindowEndSec;
        Raw.CenterSec = CenterSec;
        Raw.Rms = Cur.Rms;
        Raw.MeanAbs = Cur.MeanAbs;
        Raw.Zcr = Cur.Zcr;
        Raw.DiffMean = Cur.DiffMean;
        Raw.LowDominance = Cur.LowDominance;
        Raw.ProposedClass = Proposal.Class;
        Raw.ProposedConfidence = Clamp01(Proposal.Confidence);
        Raw.bAccepted = bAccepted;
        Raw.RejectReason = RejectReason;
        if (!bOracle)
        {
            AddRawWindow(Raw);
        }
    }

    LastAnalyzedFrame = StartFrame;
}

TArray<FOffgridAIAudioLandmarkCandidate> FOffgridAIStreamingLandmarkDetector::GenerateOfflineOracle(const TArray<int16>& MonoPCM16, int32 SampleRate, float PrerollSec)
{
    FOffgridAIStreamingLandmarkDetector Detector;
    Detector.Configure(PrerollSec);
    Detector.SampleRateHz = SampleRate;
    Detector.StreamStartSample = 0;
    Detector.bHasStreamStartSample = true;
    Detector.MonoPCM16 = MonoPCM16;
    Detector.AnalyzeNewFrames(true, MonoPCM16.Num());
    return Detector.Candidates;
}
