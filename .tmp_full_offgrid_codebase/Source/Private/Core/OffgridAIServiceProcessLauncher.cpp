#include "Core/OffgridAIServiceProcessLauncher.h"

FProcHandle FOffgridAIServiceProcessLauncher::LaunchDetached(const FString& ExecutablePath, const FString& Arguments, const FString& WorkingDirectory, uint32& OutProcessId)
{
    return FPlatformProcess::CreateProc(
        *ExecutablePath,
        *Arguments,
        true,
        true,
        true,
        &OutProcessId,
        0,
        WorkingDirectory.IsEmpty() ? nullptr : *WorkingDirectory,
        nullptr);
}

bool FOffgridAIServiceProcessLauncher::IsRunning(FProcHandle& Handle)
{
    return FPlatformProcess::IsProcRunning(Handle);
}

void FOffgridAIServiceProcessLauncher::Terminate(FProcHandle& Handle)
{
    if (!Handle.IsValid())
    {
        return;
    }

    FPlatformProcess::TerminateProc(Handle);
    FPlatformProcess::CloseProc(Handle);
    Handle.Reset();
}
