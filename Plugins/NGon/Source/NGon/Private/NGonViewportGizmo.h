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

enum class ENGonGizmoPart : uint8
{
    None,
    TranslateX,
    TranslateY,
    TranslateZ,
    TranslateXY,
    TranslateXZ,
    TranslateYZ,
    TranslateViewPlane,
    RotateX,
    RotateY,
    RotateZ,
    RotateView,
    ScaleX,
    ScaleY,
    ScaleZ,
    ScaleXY,
    ScaleXZ,
    ScaleYZ,
    ScaleUniform
};

// Direct port of TIGUIViewport::TransformGizmo for Unreal FCanvas overlay rendering and 3D ray-plane interaction.
class FNGonViewportGizmo
{
public:
    void Build(
        const FTransform& InFrame,
        ENGonGizmoMode InMode,
        double InWorldUnitsPerPixel,
        double InDPIScale,
        const FVector& InViewLocation,
        const FVector& InViewForward,
        const FVector& InViewRight,
        const FVector& InViewUp,
        bool bInAllowAxisScale,
        TFunctionRef<bool(const FVector&, FVector2D&)> InProject);

    void Build(
        const FTransform& InFrame,
        ENGonGizmoMode InMode,
        double InWorldUnitsPerPixel,
        double InDPIScale,
        const FVector& InViewRight,
        const FVector& InViewUp,
        bool bInAllowAxisScale,
        TFunctionRef<bool(const FVector&, FVector2D&)> InProject);

    void Draw(FCanvas& Canvas, const TOptional<FVector2D>& HoverPixel) const;
    bool BeginDrag(const FVector2D& Pixel);
    FTransform DragTo(const FVector2D& Pixel) const;
    void EndDrag()
    {
        ActivePart = ENGonGizmoPart::None;
        bDragging = false;
    }
    bool IsDragging() const { return bDragging; }

private:
    struct FProjectedQuad
    {
        ENGonGizmoPart Part = ENGonGizmoPart::None;
        FVector2D Corners[4] = { FVector2D::ZeroVector, FVector2D::ZeroVector, FVector2D::ZeroVector, FVector2D::ZeroVector };
        FLinearColor Color = FLinearColor::White;
        bool bValid = false;
    };

    struct FProjectedAxis
    {
        ENGonGizmoPart Part = ENGonGizmoPart::None;
        FVector AxisWorld = FVector::UnitX();
        FVector OrthoU = FVector::UnitY();
        FVector OrthoV = FVector::UnitZ();
        FVector2D StartPx = FVector2D::ZeroVector;
        FVector2D EndPx = FVector2D::ZeroVector;
        FVector2D ShaftEndPx = FVector2D::ZeroVector;
        FVector2D TipPx = FVector2D::ZeroVector;
        TArray<FVector2D, TInlineAllocator<12>> ConeBasePx;
        FVector2D CubeCornersPx[8];
        bool bHasCube = false;
        bool bIsScaleCube = false;
        FLinearColor Color = FLinearColor::White;
        bool bValid = false;
    };

    struct FRingSegment
    {
        FVector2D A = FVector2D::ZeroVector;
        FVector2D B = FVector2D::ZeroVector;
        bool bBackFacing = false;
    };

    struct FProjectedRing
    {
        ENGonGizmoPart Part = ENGonGizmoPart::None;
        FVector AxisWorld = FVector::UnitX();
        FLinearColor Color = FLinearColor::White;
        TArray<FRingSegment, TInlineAllocator<128>> Segments;
        bool bIsViewRing = false;
        float ViewRingRadiusPx = 0.0f;
    };

    ENGonGizmoPart HitTest(const FVector2D& Pixel, int32* OutRingSegIdx = nullptr, const FProjectedRing** OutRing = nullptr) const;
    FVector PartAxis(const FTransform& InFrame, ENGonGizmoPart Part) const;
    bool PartPlane(const FTransform& InFrame, ENGonGizmoPart Part, FVector& OutAxisA, FVector& OutAxisB) const;
    FVector2D WorldDeltaPerPixel(const FVector& WorldDir) const;

    FTransform Frame = FTransform::Identity;
    FTransform DragStartFrame = FTransform::Identity;
    ENGonGizmoMode Mode = ENGonGizmoMode::Translate;
    double WorldUnitsPerPixel = 1.0;
    double PixelScale = 1.0;
    double GizmoWorldLength = 80.0;
    FVector ViewLocation = FVector::ZeroVector;
    FVector ViewForward = FVector::ForwardVector;
    FVector ViewRight = FVector::RightVector;
    FVector ViewUp = FVector::UpVector;
    bool bAllowAxisScale = true;

    FVector2D OriginPx = FVector2D::ZeroVector;
    bool bOriginValid = false;
    FVector2D CenterCubeCornersPx[8];
    bool bCenterCubeValid = false;

    TArray<FProjectedAxis, TInlineAllocator<6>> Axes;
    TArray<FProjectedQuad, TInlineAllocator<6>> PlaneQuads;
    TArray<FProjectedRing, TInlineAllocator<4>> Rings;

    ENGonGizmoPart ActivePart = ENGonGizmoPart::None;
    bool bDragging = false;
    FVector2D DragStartPixel = FVector2D::ZeroVector;
    double DragStartWorldUnitsPerPixel = 1.0;
    double DragStartPixelScale = 1.0;
    FVector2D DragScreenAxisA = FVector2D::ZeroVector;
    FVector2D DragScreenAxisB = FVector2D::ZeroVector;
    FVector2D DragRotationTangent = FVector2D::ZeroVector;
    double DragRotationPixelsPerRadian = 60.0;
};
