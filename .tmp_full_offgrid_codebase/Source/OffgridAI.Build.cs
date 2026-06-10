using UnrealBuildTool;

public class OffgridAI : ModuleRules
{
    public OffgridAI(ReadOnlyTargetRules Target) : base(Target)
    {
        PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

        PublicDefinitions.Add("OFFGRIDAI_ENABLE_TEST_STUBS=1");

        PublicDependencyModuleNames.AddRange(new string[]
        {
            "Core",
            "CoreUObject",
            "Engine",
            "ControlRig",
            "RigVM"
        });

        PrivateDependencyModuleNames.AddRange(new string[]
        {
            "Json",
            "JsonUtilities",
            "AudioCaptureCore",
            "AudioMixer",
            "SignalProcessing"
        });

        if (Target.Platform == UnrealTargetPlatform.Win64)
        {
            PublicSystemLibraries.AddRange(new string[]
            {
                "Psapi.lib",
                "Pdh.lib",
                "Dxgi.lib"
            });
        }
    }
}
