#include "Core/OffgridAIResourceMonitorSubsystem.h"

#include "HAL/PlatformProcess.h"
#include "Misc/Paths.h"
#include "Stats/Stats.h"

#if PLATFORM_WINDOWS
#include "Windows/AllowWindowsPlatformTypes.h"
#include <Windows.h>
#include <TlHelp32.h>
#include <Psapi.h>
#include <Pdh.h>
#include <PdhMsg.h>
#include <dxgi1_4.h>
#include <wrl/client.h>
#include "Windows/HideWindowsPlatformTypes.h"
#endif

namespace OffgridAIResourceMonitor
{
#if PLATFORM_WINDOWS
    struct FProcessCpuSample
    {
        uint64 LastKernelTime = 0;
        uint64 LastUserTime = 0;
        uint64 LastWallTime = 0;
        bool bHasSample = false;
    };

    static uint64 FileTimeToUInt64(const FILETIME& Time)
    {
        ULARGE_INTEGER Value;
        Value.LowPart = Time.dwLowDateTime;
        Value.HighPart = Time.dwHighDateTime;
        return Value.QuadPart;
    }

    static uint64 NowFileTime()
    {
        FILETIME Time;
        GetSystemTimeAsFileTime(&Time);
        return FileTimeToUInt64(Time);
    }

    static bool TryGetProcessIdByExecutableName(const FString& ExecutableName, uint32& OutProcessId)
    {
        OutProcessId = 0;

        HANDLE Snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
        if (Snapshot == INVALID_HANDLE_VALUE)
        {
            return false;
        }

        PROCESSENTRY32W Entry;
        FMemory::Memzero(Entry);
        Entry.dwSize = sizeof(Entry);

        bool bFound = false;
        if (Process32FirstW(Snapshot, &Entry))
        {
            do
            {
                if (ExecutableName.Equals(FString(Entry.szExeFile), ESearchCase::IgnoreCase))
                {
                    OutProcessId = Entry.th32ProcessID;
                    bFound = true;
                    break;
                }
            }
            while (Process32NextW(Snapshot, &Entry));
        }

        CloseHandle(Snapshot);
        return bFound;
    }

    static bool TryGetProcessMemoryMB(uint32 ProcessId, float& OutMemoryMB)
    {
        OutMemoryMB = 0.0f;

        HANDLE ProcessHandle = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | PROCESS_VM_READ, false, ProcessId);
        if (!ProcessHandle)
        {
            return false;
        }

        PROCESS_MEMORY_COUNTERS_EX MemoryCounters;
        FMemory::Memzero(MemoryCounters);
        MemoryCounters.cb = sizeof(MemoryCounters);

        const bool bSuccess = GetProcessMemoryInfo(ProcessHandle, reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&MemoryCounters), sizeof(MemoryCounters)) != 0;
        CloseHandle(ProcessHandle);

        if (!bSuccess)
        {
            return false;
        }

        OutMemoryMB = static_cast<float>(MemoryCounters.WorkingSetSize) / (1024.0f * 1024.0f);
        return true;
    }

    static float SampleProcessCPUPercent(uint32 ProcessId, FProcessCpuSample& Sample)
    {
        HANDLE ProcessHandle = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, false, ProcessId);
        if (!ProcessHandle)
        {
            Sample = FProcessCpuSample();
            return 0.0f;
        }

        FILETIME CreationTime;
        FILETIME ExitTime;
        FILETIME KernelTime;
        FILETIME UserTime;
        const bool bGotTimes = GetProcessTimes(ProcessHandle, &CreationTime, &ExitTime, &KernelTime, &UserTime) != 0;
        CloseHandle(ProcessHandle);

        if (!bGotTimes)
        {
            Sample = FProcessCpuSample();
            return 0.0f;
        }

        const uint64 CurrentKernelTime = FileTimeToUInt64(KernelTime);
        const uint64 CurrentUserTime = FileTimeToUInt64(UserTime);
        const uint64 CurrentWallTime = NowFileTime();

        float CPUPercent = 0.0f;
        if (Sample.bHasSample && CurrentWallTime > Sample.LastWallTime)
        {
            const uint64 ProcessDelta = (CurrentKernelTime - Sample.LastKernelTime) + (CurrentUserTime - Sample.LastUserTime);
            const uint64 WallDelta = CurrentWallTime - Sample.LastWallTime;
            const int32 CoreCount = FMath::Max(1, FPlatformMisc::NumberOfCoresIncludingHyperthreads());
            CPUPercent = FMath::Clamp((static_cast<float>(ProcessDelta) / static_cast<float>(WallDelta)) * 100.0f / static_cast<float>(CoreCount), 0.0f, 100.0f);
        }

        Sample.LastKernelTime = CurrentKernelTime;
        Sample.LastUserTime = CurrentUserTime;
        Sample.LastWallTime = CurrentWallTime;
        Sample.bHasSample = true;

        return CPUPercent;
    }

    struct FSystemCpuSample
    {
        uint64 LastIdleTime = 0;
        uint64 LastKernelTime = 0;
        uint64 LastUserTime = 0;
        bool bHasSample = false;
    };

    static float SampleSystemCPUPercent(FSystemCpuSample& Sample)
    {
        FILETIME IdleTime;
        FILETIME KernelTime;
        FILETIME UserTime;
        if (GetSystemTimes(&IdleTime, &KernelTime, &UserTime) == 0)
        {
            Sample = FSystemCpuSample();
            return 0.0f;
        }

        const uint64 CurrentIdleTime = FileTimeToUInt64(IdleTime);
        const uint64 CurrentKernelTime = FileTimeToUInt64(KernelTime);
        const uint64 CurrentUserTime = FileTimeToUInt64(UserTime);

        float CPUPercent = 0.0f;
        if (Sample.bHasSample)
        {
            const uint64 IdleDelta = CurrentIdleTime - Sample.LastIdleTime;
            const uint64 KernelDelta = CurrentKernelTime - Sample.LastKernelTime;
            const uint64 UserDelta = CurrentUserTime - Sample.LastUserTime;
            const uint64 TotalDelta = KernelDelta + UserDelta;

            if (TotalDelta > 0)
            {
                CPUPercent = FMath::Clamp((1.0f - (static_cast<float>(IdleDelta) / static_cast<float>(TotalDelta))) * 100.0f, 0.0f, 100.0f);
            }
        }

        Sample.LastIdleTime = CurrentIdleTime;
        Sample.LastKernelTime = CurrentKernelTime;
        Sample.LastUserTime = CurrentUserTime;
        Sample.bHasSample = true;
        return CPUPercent;
    }

    static void SampleSystemRAM(float& OutUsedGB, float& OutTotalGB)
    {
        OutUsedGB = 0.0f;
        OutTotalGB = 0.0f;

        MEMORYSTATUSEX MemoryStatus;
        FMemory::Memzero(MemoryStatus);
        MemoryStatus.dwLength = sizeof(MemoryStatus);

        if (GlobalMemoryStatusEx(&MemoryStatus) == 0)
        {
            return;
        }

        constexpr double BytesPerGB = 1024.0 * 1024.0 * 1024.0;
        OutTotalGB = static_cast<float>(static_cast<double>(MemoryStatus.ullTotalPhys) / BytesPerGB);
        OutUsedGB = static_cast<float>(static_cast<double>(MemoryStatus.ullTotalPhys - MemoryStatus.ullAvailPhys) / BytesPerGB);
    }

    static void SampleDXGIVRAM(float& OutUsedGB, float& OutBudgetGB, float& OutTotalGB)
    {
        OutUsedGB = 0.0f;
        OutBudgetGB = 0.0f;
        OutTotalGB = 0.0f;

        Microsoft::WRL::ComPtr<IDXGIFactory4> Factory;
        if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&Factory))))
        {
            return;
        }

        Microsoft::WRL::ComPtr<IDXGIAdapter1> BestAdapter;
        DXGI_ADAPTER_DESC1 BestDesc;
        FMemory::Memzero(&BestDesc, sizeof(BestDesc));

        for (uint32 AdapterIndex = 0;; ++AdapterIndex)
        {
            Microsoft::WRL::ComPtr<IDXGIAdapter1> Adapter;
            if (Factory->EnumAdapters1(AdapterIndex, &Adapter) == DXGI_ERROR_NOT_FOUND)
            {
                break;
            }

            DXGI_ADAPTER_DESC1 Desc;
            if (FAILED(Adapter->GetDesc1(&Desc)))
            {
                continue;
            }

            if ((Desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) != 0)
            {
                continue;
            }

            if (!BestAdapter || Desc.DedicatedVideoMemory > BestDesc.DedicatedVideoMemory)
            {
                BestAdapter = Adapter;
                BestDesc = Desc;
            }
        }

        if (!BestAdapter)
        {
            return;
        }

        constexpr double BytesPerGB = 1024.0 * 1024.0 * 1024.0;
        OutTotalGB = static_cast<float>(static_cast<double>(BestDesc.DedicatedVideoMemory) / BytesPerGB);

        Microsoft::WRL::ComPtr<IDXGIAdapter3> Adapter3;
        if (SUCCEEDED(BestAdapter.As(&Adapter3)))
        {
            DXGI_QUERY_VIDEO_MEMORY_INFO MemoryInfo;
            if (SUCCEEDED(Adapter3->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &MemoryInfo)))
            {
                OutUsedGB = static_cast<float>(static_cast<double>(MemoryInfo.CurrentUsage) / BytesPerGB);
                OutBudgetGB = static_cast<float>(static_cast<double>(MemoryInfo.Budget) / BytesPerGB);
            }
        }
    }

    class FPDHFormattedCounterArray
    {
    public:
        ~FPDHFormattedCounterArray()
        {
            if (Query)
            {
                PdhCloseQuery(Query);
                Query = nullptr;
            }
        }

        bool Sample(const wchar_t* CounterPath, TArray<PDH_FMT_COUNTERVALUE_ITEM_W>& OutItems)
        {
            OutItems.Reset();

            if (!bInitialized)
            {
                bInitialized = true;
                if (PdhOpenQueryW(nullptr, 0, &Query) != ERROR_SUCCESS)
                {
                    Query = nullptr;
                    return false;
                }

                if (PdhAddEnglishCounterW(Query, CounterPath, 0, &Counter) != ERROR_SUCCESS)
                {
                    PdhCloseQuery(Query);
                    Query = nullptr;
                    return false;
                }

                PdhCollectQueryData(Query);
                return false;
            }

            if (!Query || PdhCollectQueryData(Query) != ERROR_SUCCESS)
            {
                return false;
            }

            DWORD BufferSize = 0;
            DWORD ItemCount = 0;
            PDH_STATUS Status = PdhGetFormattedCounterArrayW(Counter, PDH_FMT_DOUBLE, &BufferSize, &ItemCount, nullptr);
            if (Status != PDH_MORE_DATA || BufferSize == 0 || ItemCount == 0)
            {
                return false;
            }

            TArray<uint8> Buffer;
            Buffer.SetNumZeroed(static_cast<int32>(BufferSize));
            PPDH_FMT_COUNTERVALUE_ITEM_W RawItems = reinterpret_cast<PPDH_FMT_COUNTERVALUE_ITEM_W>(Buffer.GetData());

            Status = PdhGetFormattedCounterArrayW(Counter, PDH_FMT_DOUBLE, &BufferSize, &ItemCount, RawItems);
            if (Status != ERROR_SUCCESS)
            {
                return false;
            }

            OutItems.SetNum(static_cast<int32>(ItemCount));
            for (DWORD Index = 0; Index < ItemCount; ++Index)
            {
                OutItems[static_cast<int32>(Index)] = RawItems[Index];
            }
            return true;
        }

    private:
        PDH_HQUERY Query = nullptr;
        PDH_HCOUNTER Counter = nullptr;
        bool bInitialized = false;
    };

    static bool InstanceNameContainsPid(const wchar_t* InstanceName, uint32 ProcessId)
    {
        if (!InstanceName || ProcessId == 0)
        {
            return false;
        }

        const FString Instance(InstanceName);
        const FString PidNeedle = FString::Printf(TEXT("pid_%u"), ProcessId);
        return Instance.Contains(PidNeedle, ESearchCase::IgnoreCase);
    }

    class FPDHGPUEngineCounter
    {
    public:
        void Sample(float& OutTotalGPUPercent, TMap<uint32, float>& OutProcessGPUPercentByPid)
        {
            OutTotalGPUPercent = 0.0f;
            OutProcessGPUPercentByPid.Reset();

            TArray<PDH_FMT_COUNTERVALUE_ITEM_W> Items;
            if (!CounterArray.Sample(L"\\GPU Engine(*)\\Utilization Percentage", Items))
            {
                return;
            }

            double TotalGPU = 0.0;
            for (const PDH_FMT_COUNTERVALUE_ITEM_W& Item : Items)
            {
                if (Item.FmtValue.CStatus != ERROR_SUCCESS)
                {
                    continue;
                }

                const float Value = FMath::Max(0.0f, static_cast<float>(Item.FmtValue.doubleValue));
                TotalGPU += Value;

                const FString Instance(Item.szName ? Item.szName : L"");
                const int32 PidIndex = Instance.Find(TEXT("pid_"), ESearchCase::IgnoreCase);
                if (PidIndex != INDEX_NONE)
                {
                    int32 Cursor = PidIndex + 4;
                    FString PidDigits;
                    while (Cursor < Instance.Len() && FChar::IsDigit(Instance[Cursor]))
                    {
                        PidDigits.AppendChar(Instance[Cursor]);
                        ++Cursor;
                    }

                    if (!PidDigits.IsEmpty())
                    {
                        const uint32 Pid = static_cast<uint32>(FCString::Atoi(*PidDigits));
                        float& ProcessTotal = OutProcessGPUPercentByPid.FindOrAdd(Pid);
                        ProcessTotal += Value;
                    }
                }
            }

            OutTotalGPUPercent = FMath::Clamp(static_cast<float>(TotalGPU), 0.0f, 100.0f);
            for (TPair<uint32, float>& Pair : OutProcessGPUPercentByPid)
            {
                Pair.Value = FMath::Clamp(Pair.Value, 0.0f, 100.0f);
            }
        }

    private:
        FPDHFormattedCounterArray CounterArray;
    };

    class FPDHGPUProcessMemoryCounter
    {
    public:
        void Sample(TMap<uint32, float>& OutVRAMMBByPid)
        {
            OutVRAMMBByPid.Reset();

            TArray<PDH_FMT_COUNTERVALUE_ITEM_W> Items;
            if (!DedicatedUsageCounter.Sample(L"\\GPU Process Memory(*)\\Dedicated Usage", Items))
            {
                return;
            }

            constexpr double BytesPerMB = 1024.0 * 1024.0;
            for (const PDH_FMT_COUNTERVALUE_ITEM_W& Item : Items)
            {
                if (Item.FmtValue.CStatus != ERROR_SUCCESS)
                {
                    continue;
                }

                const FString Instance(Item.szName ? Item.szName : L"");
                const int32 PidIndex = Instance.Find(TEXT("pid_"), ESearchCase::IgnoreCase);
                if (PidIndex == INDEX_NONE)
                {
                    continue;
                }

                int32 Cursor = PidIndex + 4;
                FString PidDigits;
                while (Cursor < Instance.Len() && FChar::IsDigit(Instance[Cursor]))
                {
                    PidDigits.AppendChar(Instance[Cursor]);
                    ++Cursor;
                }

                if (PidDigits.IsEmpty())
                {
                    continue;
                }

                const uint32 Pid = static_cast<uint32>(FCString::Atoi(*PidDigits));
                const float ValueMB = FMath::Max(0.0f, static_cast<float>(Item.FmtValue.doubleValue / BytesPerMB));
                float& ProcessTotal = OutVRAMMBByPid.FindOrAdd(Pid);
                ProcessTotal += ValueMB;
            }
        }

    private:
        FPDHFormattedCounterArray DedicatedUsageCounter;
    };

    static FSystemCpuSample GSystemCpuSample;
    static FProcessCpuSample GUnrealCpuSample;
    static FProcessCpuSample GASRCpuSample;
    static FProcessCpuSample GLLMCpuSample;
    static FProcessCpuSample GTTSCpuSample;
    static FPDHGPUEngineCounter GGPUCounter;
    static FPDHGPUProcessMemoryCounter GGPUProcessMemoryCounter;

    static FOffgridAIProcessResourceStats SampleNamedProcess(const FString& ExecutableName, FProcessCpuSample& CpuSample)
    {
        FOffgridAIProcessResourceStats Stats;

        uint32 ProcessId = 0;
        if (!TryGetProcessIdByExecutableName(ExecutableName, ProcessId))
        {
            CpuSample = FProcessCpuSample();
            return Stats;
        }

        Stats.bFound = true;
        Stats.ProcessId = static_cast<int32>(ProcessId);
        Stats.CPUPercent = SampleProcessCPUPercent(ProcessId, CpuSample);
        TryGetProcessMemoryMB(ProcessId, Stats.RAMUsedMB);
        return Stats;
    }

    static FOffgridAIProcessResourceStats SampleProcessByPid(uint32 ProcessId, FProcessCpuSample& CpuSample)
    {
        FOffgridAIProcessResourceStats Stats;
        Stats.bFound = ProcessId != 0;
        Stats.ProcessId = static_cast<int32>(ProcessId);
        if (!Stats.bFound)
        {
            CpuSample = FProcessCpuSample();
            return Stats;
        }

        Stats.CPUPercent = SampleProcessCPUPercent(ProcessId, CpuSample);
        TryGetProcessMemoryMB(ProcessId, Stats.RAMUsedMB);
        return Stats;
    }
#endif
}

void UOffgridAIResourceMonitorSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
    Super::Initialize(Collection);
    bInitialized = true;
    SecondsUntilNextSample = 0.0;
    SampleResourceStats();
}

void UOffgridAIResourceMonitorSubsystem::Deinitialize()
{
    bInitialized = false;
    Super::Deinitialize();
}

void UOffgridAIResourceMonitorSubsystem::Tick(float DeltaTime)
{
    SecondsUntilNextSample -= static_cast<double>(DeltaTime);
    if (SecondsUntilNextSample <= 0.0)
    {
        SampleResourceStats();
        SecondsUntilNextSample = static_cast<double>(SampleIntervalSeconds);
    }
}

bool UOffgridAIResourceMonitorSubsystem::IsTickable() const
{
    return bInitialized && !HasAnyFlags(RF_ClassDefaultObject);
}

TStatId UOffgridAIResourceMonitorSubsystem::GetStatId() const
{
    RETURN_QUICK_DECLARE_CYCLE_STAT(UOffgridAIResourceMonitorSubsystem, STATGROUP_Tickables);
}

void UOffgridAIResourceMonitorSubsystem::SetResourceSampleInterval(float InSampleIntervalSeconds)
{
    SampleIntervalSeconds = FMath::Clamp(InSampleIntervalSeconds, 0.1f, 5.0f);
}

FOffgridAIResourceStats UOffgridAIResourceMonitorSubsystem::GetLatestResourceStats() const
{
    return LatestStats;
}

FOffgridAIResourceStats UOffgridAIResourceMonitorSubsystem::SampleResourceStatsNow()
{
    SampleResourceStats();
    SecondsUntilNextSample = static_cast<double>(SampleIntervalSeconds);
    return LatestStats;
}

void UOffgridAIResourceMonitorSubsystem::SetLatestRoundTripTimeMs(float InRoundTripTimeMs)
{
    LatestStats.bHasRoundTripTime = true;
    LatestStats.RoundTripTimeMs = FMath::Max(0.0f, InRoundTripTimeMs);
}

void UOffgridAIResourceMonitorSubsystem::LatchAndResetTurnResourcePeaksIfNeeded(int32 InPerceivedLatencySampleCount)
{
    if (InPerceivedLatencySampleCount <= LastLatchedPerceivedLatencySampleCount)
    {
        return;
    }

    LastLatchedPerceivedLatencySampleCount = InPerceivedLatencySampleCount;

    LastTurnASRServiceCPUPeakPercent = CurrentTurnASRServiceCPUPeakPercent;
    LastTurnLLMServiceCPUPeakPercent = CurrentTurnLLMServiceCPUPeakPercent;
    LastTurnTTSServiceCPUPeakPercent = CurrentTurnTTSServiceCPUPeakPercent;

    LastTurnASRServiceGPUPeakPercent = CurrentTurnASRServiceGPUPeakPercent;
    LastTurnLLMServiceGPUPeakPercent = CurrentTurnLLMServiceGPUPeakPercent;
    LastTurnTTSServiceGPUPeakPercent = CurrentTurnTTSServiceGPUPeakPercent;

    CurrentTurnASRServiceCPUPeakPercent = 0.0f;
    CurrentTurnLLMServiceCPUPeakPercent = 0.0f;
    CurrentTurnTTSServiceCPUPeakPercent = 0.0f;

    CurrentTurnASRServiceGPUPeakPercent = 0.0f;
    CurrentTurnLLMServiceGPUPeakPercent = 0.0f;
    CurrentTurnTTSServiceGPUPeakPercent = 0.0f;

    ApplyTurnResourcePeakPresentation();
}

void UOffgridAIResourceMonitorSubsystem::ApplyTurnResourcePeakPresentation()
{
    const float ASRCPU = LastTurnASRServiceCPUPeakPercent > 0.0f ? LastTurnASRServiceCPUPeakPercent : CurrentTurnASRServiceCPUPeakPercent;
    const float LLMCPU = LastTurnLLMServiceCPUPeakPercent > 0.0f ? LastTurnLLMServiceCPUPeakPercent : CurrentTurnLLMServiceCPUPeakPercent;
    const float TTSCPU = LastTurnTTSServiceCPUPeakPercent > 0.0f ? LastTurnTTSServiceCPUPeakPercent : CurrentTurnTTSServiceCPUPeakPercent;

    const float ASRGPU = LastTurnASRServiceGPUPeakPercent > 0.0f ? LastTurnASRServiceGPUPeakPercent : CurrentTurnASRServiceGPUPeakPercent;
    const float LLMGPU = LastTurnLLMServiceGPUPeakPercent > 0.0f ? LastTurnLLMServiceGPUPeakPercent : CurrentTurnLLMServiceGPUPeakPercent;
    const float TTSGPU = LastTurnTTSServiceGPUPeakPercent > 0.0f ? LastTurnTTSServiceGPUPeakPercent : CurrentTurnTTSServiceGPUPeakPercent;

    if (LatestStats.ASRService.bFound)
    {
        LatestStats.ASRService.CPUPercent = ASRCPU;
    }
    if (LatestStats.LLMService.bFound)
    {
        LatestStats.LLMService.CPUPercent = LLMCPU;
    }
    if (LatestStats.TTSService.bFound)
    {
        LatestStats.TTSService.CPUPercent = TTSCPU;
    }

    LatestStats.ASRServiceGPUPercent = LatestStats.ASRService.bFound ? ASRGPU : 0.0f;
    LatestStats.LLMServiceGPUPercent = LatestStats.LLMService.bFound ? LLMGPU : 0.0f;
    LatestStats.TTSServiceGPUPercent = LatestStats.TTSService.bFound ? TTSGPU : 0.0f;
}

void UOffgridAIResourceMonitorSubsystem::SetLatestPerformanceMetricStats(const FOffgridAIMetricAccumulator& MetricAccumulator)
{
    auto CopyWindow = [&MetricAccumulator](const FString& MetricName, FOffgridAIPerformanceMetricStats& OutStats)
    {
        FOffgridAIMetricWindow Window;
        if (MetricAccumulator.GetWindow(MetricName, Window))
        {
            OutStats.bHasSamples = true;
            OutStats.LastMs = static_cast<float>(Window.LastMs);
            OutStats.MedianMs = static_cast<float>(Window.MedianMs);
            OutStats.P90Ms = static_cast<float>(Window.P90Ms);
            OutStats.SampleCount = Window.SampleCount;
        }
        else
        {
            OutStats = FOffgridAIPerformanceMetricStats();
        }
    };

    CopyWindow(TEXT("RoundTripLatency"), LatestStats.RoundTripLatency);
    CopyWindow(TEXT("PerceivedLatency"), LatestStats.PerceivedLatency);
    CopyWindow(TEXT("ASRLatency"), LatestStats.ASRLatency);
    CopyWindow(TEXT("LLMTotalLatency"), LatestStats.LLMTotalLatency);
    CopyWindow(TEXT("TTSFirstAudioLatency"), LatestStats.TTSFirstAudioLatency);
    CopyWindow(TEXT("PlaybackDelay"), LatestStats.PlaybackDelay);

    LatchAndResetTurnResourcePeaksIfNeeded(LatestStats.PerceivedLatency.SampleCount);
}

void UOffgridAIResourceMonitorSubsystem::SampleResourceStats()
{
#if PLATFORM_WINDOWS
    using namespace OffgridAIResourceMonitor;

    LatestStats.SystemCPUPercent = SampleSystemCPUPercent(GSystemCpuSample);
    SampleSystemRAM(LatestStats.SystemRAMUsedGB, LatestStats.SystemRAMTotalGB);

    TMap<uint32, float> ProcessGPUPercentByPid;
    GGPUCounter.Sample(LatestStats.GPUPercent, ProcessGPUPercentByPid);
    SampleDXGIVRAM(LatestStats.VRAMUsedGB, LatestStats.VRAMBudgetGB, LatestStats.VRAMTotalGB);

    LatestStats.UnrealProcess = SampleProcessByPid(FPlatformProcess::GetCurrentProcessId(), GUnrealCpuSample);
    LatestStats.ASRService = SampleNamedProcess(TEXT("OffgridAI_ASR.exe"), GASRCpuSample);
    LatestStats.LLMService = SampleNamedProcess(TEXT("OffgridAI_LLM.exe"), GLLMCpuSample);
    LatestStats.TTSService = SampleNamedProcess(TEXT("OffgridAI_TTS.exe"), GTTSCpuSample);

    TMap<uint32, float> ProcessVRAMMBByPid;
    GGPUProcessMemoryCounter.Sample(ProcessVRAMMBByPid);

    const uint32 ASRPid = LatestStats.ASRService.bFound ? static_cast<uint32>(LatestStats.ASRService.ProcessId) : 0;
    const uint32 LLMPid = LatestStats.LLMService.bFound ? static_cast<uint32>(LatestStats.LLMService.ProcessId) : 0;
    const uint32 TTSPid = LatestStats.TTSService.bFound ? static_cast<uint32>(LatestStats.TTSService.ProcessId) : 0;

    const float ASRInstantGPU = ASRPid != 0 ? ProcessGPUPercentByPid.FindRef(ASRPid) : 0.0f;
    const float LLMInstantGPU = LLMPid != 0 ? ProcessGPUPercentByPid.FindRef(LLMPid) : 0.0f;
    const float TTSInstantGPU = TTSPid != 0 ? ProcessGPUPercentByPid.FindRef(TTSPid) : 0.0f;

    LatestStats.ASRServiceVRAMMB = ASRPid != 0 ? ProcessVRAMMBByPid.FindRef(ASRPid) : 0.0f;
    LatestStats.LLMServiceVRAMMB = LLMPid != 0 ? ProcessVRAMMBByPid.FindRef(LLMPid) : 0.0f;
    LatestStats.TTSServiceVRAMMB = TTSPid != 0 ? ProcessVRAMMBByPid.FindRef(TTSPid) : 0.0f;

    if (LatestStats.ASRService.bFound)
    {
        CurrentTurnASRServiceCPUPeakPercent = FMath::Max(CurrentTurnASRServiceCPUPeakPercent, LatestStats.ASRService.CPUPercent);
        CurrentTurnASRServiceGPUPeakPercent = FMath::Max(CurrentTurnASRServiceGPUPeakPercent, ASRInstantGPU);
    }
    else
    {
        CurrentTurnASRServiceCPUPeakPercent = 0.0f;
        CurrentTurnASRServiceGPUPeakPercent = 0.0f;
        LastTurnASRServiceCPUPeakPercent = 0.0f;
        LastTurnASRServiceGPUPeakPercent = 0.0f;
    }

    if (LatestStats.LLMService.bFound)
    {
        CurrentTurnLLMServiceCPUPeakPercent = FMath::Max(CurrentTurnLLMServiceCPUPeakPercent, LatestStats.LLMService.CPUPercent);
        CurrentTurnLLMServiceGPUPeakPercent = FMath::Max(CurrentTurnLLMServiceGPUPeakPercent, LLMInstantGPU);
    }
    else
    {
        CurrentTurnLLMServiceCPUPeakPercent = 0.0f;
        CurrentTurnLLMServiceGPUPeakPercent = 0.0f;
        LastTurnLLMServiceCPUPeakPercent = 0.0f;
        LastTurnLLMServiceGPUPeakPercent = 0.0f;
    }

    if (LatestStats.TTSService.bFound)
    {
        CurrentTurnTTSServiceCPUPeakPercent = FMath::Max(CurrentTurnTTSServiceCPUPeakPercent, LatestStats.TTSService.CPUPercent);
        CurrentTurnTTSServiceGPUPeakPercent = FMath::Max(CurrentTurnTTSServiceGPUPeakPercent, TTSInstantGPU);
    }
    else
    {
        CurrentTurnTTSServiceCPUPeakPercent = 0.0f;
        CurrentTurnTTSServiceGPUPeakPercent = 0.0f;
        LastTurnTTSServiceCPUPeakPercent = 0.0f;
        LastTurnTTSServiceGPUPeakPercent = 0.0f;
    }

    ApplyTurnResourcePeakPresentation();
#else
    LatestStats = FOffgridAIResourceStats();
#endif
}
