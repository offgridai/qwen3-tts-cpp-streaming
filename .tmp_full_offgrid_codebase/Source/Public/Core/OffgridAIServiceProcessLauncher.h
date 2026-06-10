#pragma once

#include "CoreMinimal.h"

class OFFGRIDAI_API FOffgridAIServiceProcessLauncher
{
public:
    static FProcHandle LaunchDetached(const FString& ExecutablePath, const FString& Arguments, const FString& WorkingDirectory, uint32& OutProcessId);
    static bool IsRunning(FProcHandle& Handle);
    static void Terminate(FProcHandle& Handle);
};
