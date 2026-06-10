#pragma once

#include "CoreMinimal.h"

// Embedded CMU Pronouncing Dictionary data for portable OffgridAI/LipLab lipsync.
// See OffgridAICmudictData.cpp for the required Carnegie Mellon University
// copyright notice, redistribution terms, and disclaimer.
namespace OffgridAILipsyncEmbedded
{
    OFFGRIDAI_API const char* GetCmudictDictData();
    OFFGRIDAI_API int32 GetCmudictDictDataSize();
}
