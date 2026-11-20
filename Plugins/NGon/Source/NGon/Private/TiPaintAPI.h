#pragma once

#include "CoreMinimal.h"
#include "TiPaintDocument.h"

/**
 * Dedicated AI-controllable JSON Command & Inspection API for FTiPaintDocument.
 * Mirrors FNGonMeshAPI so external agents, scripts, and the TiPaint AI Console
 * can drive brushes, layers, stencils, 3D projections, and export safely.
 */
class FTiPaintAPI
{
public:
    static FString GetApiSchemaJson();
    static FString ExportDocumentSummaryJson(const FTiPaintDocument& Document);
    static FTiPaintOpResult ExecuteJsonCommand(FTiPaintDocument& Document, const FString& JsonText);
};
