#pragma once

#include "CoreMinimal.h"

class FCanvas;

enum class ENGonGizmoMode : uint8
{
    Combined,
    Translate,
    Rotate,
    Scale,
    None
};

// The viewport draws these projected handles and uses the same segments for hits.
class FNGonViewportGizmo
{
public:
    void Build(const FTransform& InFrame, ENGonGizmoMode Mode, double WorldUnitsPerPixel, double DPIScale,
        const FVector& ViewRight, const FVector& ViewUp, bool bAllowAxisScale,
        TFunctionRef<bool(const FVector&, FVector2D&)> Project);
    void Draw(FCanvas& Canvas, const TOptional<FVector2D>& HoverPixel) const;
    bool BeginDrag(const FVector2D& Pixel);
    FTransform DragTo(const FVector2D& Pixel) const;
    void EndDrag() { DragHandle.Reset(); }

private:
    enum class EAction : uint8 { MoveAxis, MovePlane, Rotate, ScaleAxis, ScaleUniform };

    struct FSegment
    {
        FVector2D A;
        FVector2D B;
    };

    struct FHandle
    {
        EAction Action = EAction::MoveAxis;
        int32 AxisIndex = INDEX_NONE;
        int32 SecondAxisIndex = INDEX_NONE;
        FLinearColor Color = FLinearColor::White;
        FVector WorldA = FVector::ZeroVector;
        FVector WorldB = FVector::ZeroVector;
        FVector2D PixelsPerUnitA = FVector2D::ZeroVector;
        FVector2D PixelsPerUnitB = FVector2D::ZeroVector;
        TArray<FSegment, TInlineAllocator<48>> Segments;
    };

    int32 HitTest(const FVector2D& Pixel, int32& SegmentIndex) const;
    static void AddLoop(FHandle& Handle, TConstArrayView<FVector2D> Points);

    FTransform Frame = FTransform::Identity;
    FTransform DragFrame = FTransform::Identity;
    TArray<FHandle> Handles;
    TOptional<FHandle> DragHandle;
    FVector2D DragStart = FVector2D::ZeroVector;
    FVector2D RotationTangent = FVector2D::ZeroVector;
    double RotationPixelsPerRadian = 1.0;
    double PixelScale = 1.0;
    double DragPixelScale = 1.0;
};
