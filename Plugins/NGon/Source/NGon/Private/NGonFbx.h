#pragma once

#include "CoreMinimal.h"

struct FNGonMeshData;

namespace NGon
{
    bool LoadFbx(const FString& Filename, FNGonMeshData& Out, FText& Error);
}
