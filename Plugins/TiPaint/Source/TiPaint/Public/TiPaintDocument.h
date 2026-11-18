#pragma once

#include "CoreMinimal.h"

enum class ETiPaintMode : uint8
{
    Paint,
    Erase
};

/** One authored UV triangle used to keep the painter canvas aligned with the
 * active N-Gon mesh.  UVs use the same 0..1 convention as the paint document. */
struct FTiPaintUVTriangle
{
    FVector2f UV[3] = { FVector2f::ZeroVector, FVector2f::ZeroVector, FVector2f::ZeroVector };
};

struct FTiPaintBrush
{
    FColor Color = FColor(77, 128, 255, 255);
    float Radius = 24.0f;
    float Opacity = 1.0f;
    float Hardness = 0.7f;
    ETiPaintMode Mode = ETiPaintMode::Paint;
};

struct FTiPaintLayer
{
    FString Name;
    bool bVisible = true;
    bool bLocked = false;
    float Opacity = 1.0f;
    TArray<FColor> Pixels;
};

/** Portable RGBA document modeled after TiGUI's tiled PaintDocument API. */
class TIPAINT_API FTiPaintDocument : public TSharedFromThis<FTiPaintDocument>
{
public:
    explicit FTiPaintDocument(int32 InWidth = 512, int32 InHeight = 512);

    int32 Width() const { return SizeX; }
    int32 Height() const { return SizeY; }
    const TArray<FTiPaintLayer>& Layers() const { return PaintLayers; }
    int32 ActiveLayer() const { return ActiveLayerIndex; }
    const FTiPaintBrush& Brush() const { return ActiveBrush; }
    void SetBrush(const FTiPaintBrush& InBrush) { ActiveBrush = InBrush; }
    void SetChanged(TFunction<void()> InChanged) { Changed = MoveTemp(InChanged); }
    const TArray<FColor>& Composite() const;
    bool IsStroking() const { return bStroking; }
    int32 UndoCount() const { return UndoStack.Num(); }
    int32 RedoCount() const { return RedoStack.Num(); }

    void BeginStroke(float U, float V);
    void AddStrokePoint(float U, float V);
    void EndStroke();
    void CancelStroke();
    void Undo();
    void Redo();
    void AddLayer();
    void RemoveActiveLayer();
    void ToggleActiveVisibility();
    void ToggleActiveLock();
    void SetActiveLayer(int32 Index);
    void Clear();
    bool ExportPNG(const FString& Filename, FText& Error) const;

private:
    struct FSnapshot
    {
        TArray<FTiPaintLayer> Layers;
        int32 Active = 0;
    };
    void SaveUndoSnapshot();
    void RestoreSnapshot(const FSnapshot& Snapshot);
    void Stamp(float U, float V);
    void StampAtPixel(int32 X, int32 Y);
    void MarkChanged();
    static FColor BlendPixel(const FColor& Base, const FColor& Paint, float Alpha);

    int32 SizeX = 512;
    int32 SizeY = 512;
    TArray<FTiPaintLayer> PaintLayers;
    int32 ActiveLayerIndex = 0;
    FTiPaintBrush ActiveBrush;
    mutable TArray<FColor> CompositePixels;
    TArray<FSnapshot> UndoStack;
    TArray<FSnapshot> RedoStack;
    FSnapshot StrokeSnapshot;
    bool bStroking = false;
    FVector2f LastUV = FVector2f::ZeroVector;
    TFunction<void()> Changed;
};
