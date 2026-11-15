#pragma once

#include "CoreMinimal.h"

namespace NGon
{
    FString GetMeshesDirectory();
    bool FindDefaultFbx(const FString& Directory, FString& OutPath, FText& Error);
}
