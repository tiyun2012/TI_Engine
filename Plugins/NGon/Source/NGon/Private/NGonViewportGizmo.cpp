#include "NGonViewportGizmo.h"

#include "CanvasItem.h"
#include "CanvasTypes.h"

namespace
{
constexpr int32 RingSegments = 48;
constexpr double AxisLengthPixels = 78.0;
const FLinearColor AxisColors[] = {
    FLinearColor(1.0f, 0.14f, 0.10f),
    FLinearColor(0.22f, 1.0f, 0.18f),
    FLinearColor(0.16f, 0.60f, 1.0f)
};
const FLinearColor ActiveColor(1.0f, 0.86f, 0.16f);

void DrawSegment(FCanvas& Canvas, const FVector2D& A, const FVector2D& B,
    const FLinearColor& Color, float Thickness)
{
    FCanvasLineItem Line(A, B);
    Line.SetColor(Color);
    Line.LineThickness = Thickness;
    Canvas.DrawItem(Line);
}
}

void FNGonViewportGizmo::Build(const FTransform& InFrame, ENGonGizmoMode Mode, double WorldUnitsPerPixel,
    double DPIScale, const FVector& ViewRight, const FVector& ViewUp, bool bAllowAxisScale,
    TFunctionRef<bool(const FVector&, FVector2D&)> Project)
{
    Handles.Reset();
    Frame = InFrame;
    PixelScale = FMath::Clamp(DPIScale, 1.0, 2.5);
    if (Mode == ENGonGizmoMode::None || !FMath::IsFinite(WorldUnitsPerPixel) || WorldUnitsPerPixel <= 0.0)
    {
        return;
    }

    const FVector Origin = Frame.GetLocation();
    FVector2D Center;
    if (!Project(Origin, Center))
    {
        return;
    }
    const bool bCombined = Mode == ENGonGizmoMode::Combined;
    const bool bMove = bCombined || Mode == ENGonGizmoMode::Translate;
    const bool bRotate = bCombined || Mode == ENGonGizmoMode::Rotate;
    const bool bScale = bCombined || Mode == ENGonGizmoMode::Scale;
    const double WorldSize = AxisLengthPixels * PixelScale * WorldUnitsPerPixel;
    const FVector Axes[] = { Frame.GetUnitAxis(EAxis::X), Frame.GetUnitAxis(EAxis::Y), Frame.GetUnitAxis(EAxis::Z) };
    FVector2D ScreenAxes[3] = { FVector2D::ZeroVector, FVector2D::ZeroVector, FVector2D::ZeroVector };
    for (int32 Axis = 0; Axis < 3; ++Axis)
    {
        FVector2D Tip;
        if (Project(Origin + Axes[Axis] * WorldSize, Tip))
        {
            ScreenAxes[Axis] = Tip - Center;
        }
    }

    if (bScale || Mode == ENGonGizmoMode::Translate)
    {
        // W uses the center to move in the camera plane. R and Combined use it for uniform scale.
        FHandle& Handle = Handles.AddDefaulted_GetRef();
        Handle.Action = bScale ? EAction::ScaleUniform : EAction::MovePlane;
        Handle.Color = FLinearColor::White;
        Handle.WorldA = ViewRight;
        Handle.WorldB = ViewUp;
        Handle.PixelsPerUnitA = FVector2D(1.0 / WorldUnitsPerPixel, 0);
        Handle.PixelsPerUnitB = FVector2D(0, -1.0 / WorldUnitsPerPixel);
        const double R = 5.0 * PixelScale;
        const FVector2D Diamond[] = { Center + FVector2D(0, -R), Center + FVector2D(R, 0),
            Center + FVector2D(0, R), Center + FVector2D(-R, 0) };
        AddLoop(Handle, MakeArrayView(Diamond));
    }

    for (int32 Axis = 0; Axis < 3; ++Axis)
    {
        if (ScreenAxes[Axis].SizeSquared() < FMath::Square(15.0 * PixelScale))
        {
            continue; // This axis points nearly into the camera.
        }
        const FVector2D Direction = ScreenAxes[Axis].GetSafeNormal();
        const FVector2D Side(-Direction.Y, Direction.X);
        if (bScale && bAllowAxisScale)
        {
            FHandle& Handle = Handles.AddDefaulted_GetRef();
            Handle.Action = EAction::ScaleAxis;
            Handle.AxisIndex = Axis;
            Handle.Color = AxisColors[Axis];
            Handle.PixelsPerUnitA = ScreenAxes[Axis] / WorldSize;
            const FVector2D Tip = Center + ScreenAxes[Axis] * (bCombined ? 0.52 : 1.0);
            const FVector2D Along = Direction * (4.5 * PixelScale);
            const FVector2D Across = Side * (4.5 * PixelScale);
            const FVector2D Square[] = { Tip - Along - Across, Tip + Along - Across,
                Tip + Along + Across, Tip - Along + Across };
            AddLoop(Handle, MakeArrayView(Square));
            if (!bCombined)
            {
                Handle.Segments.Add({ Center + Direction * (11.0 * PixelScale), Tip - Along });
            }
        }
        if (bMove)
        {
            FHandle& Handle = Handles.AddDefaulted_GetRef();
            Handle.Action = EAction::MoveAxis;
            Handle.AxisIndex = Axis;
            Handle.Color = AxisColors[Axis];
            Handle.WorldA = Axes[Axis];
            Handle.PixelsPerUnitA = ScreenAxes[Axis] / WorldSize;
            const FVector2D Tip = Center + ScreenAxes[Axis];
            const FVector2D Head = Tip - Direction * (12.0 * PixelScale);
            Handle.Segments.Add({ Center + Direction * (11.0 * PixelScale), Tip });
            Handle.Segments.Add({ Head - Side * (5.0 * PixelScale), Tip });
            Handle.Segments.Add({ Head + Side * (5.0 * PixelScale), Tip });
        }
    }

    for (int32 Axis = 0; Axis < 3; ++Axis)
    {
        const int32 A = (Axis + 1) % 3;
        const int32 B = (Axis + 2) % 3;
        if (bMove && FMath::Abs(FVector2D::CrossProduct(ScreenAxes[A], ScreenAxes[B])) > 1300.0 * FMath::Square(PixelScale))
        {
            FVector2D Corners[4];
            const FVector Corners3D[] = { Axes[A] + Axes[B], 1.55 * Axes[A] + Axes[B],
                1.55 * (Axes[A] + Axes[B]), Axes[A] + 1.55 * Axes[B] };
            bool bProjected = true;
            for (int32 Corner = 0; Corner < UE_ARRAY_COUNT(Corners); ++Corner)
            {
                bProjected &= Project(Origin + WorldSize * 0.24 * Corners3D[Corner], Corners[Corner]);
            }
            if (bProjected)
            {
                FHandle& Handle = Handles.AddDefaulted_GetRef();
                Handle.Action = EAction::MovePlane;
                Handle.AxisIndex = A;
                Handle.SecondAxisIndex = B;
                Handle.Color = FLinearColor::LerpUsingHSV(AxisColors[A], AxisColors[B], 0.5f);
                Handle.WorldA = Axes[A];
                Handle.WorldB = Axes[B];
                Handle.PixelsPerUnitA = ScreenAxes[A] / WorldSize;
                Handle.PixelsPerUnitB = ScreenAxes[B] / WorldSize;
                AddLoop(Handle, MakeArrayView(Corners));
            }
        }
        if (bRotate)
        {
            FVector2D Ring[RingSegments];
            const double Radius = WorldSize * (bCombined ? 0.74 : 0.88);
            bool bProjected = true;
            for (int32 Step = 0; Step < RingSegments; ++Step)
            {
                const double Angle = (2.0 * UE_DOUBLE_PI / RingSegments) * Step;
                const FVector Point = Origin + Radius * (Axes[A] * FMath::Cos(Angle) + Axes[B] * FMath::Sin(Angle));
                bProjected &= Project(Point, Ring[Step]);
            }
            if (bProjected)
            {
                FHandle& Handle = Handles.AddDefaulted_GetRef();
                Handle.Action = EAction::Rotate;
                Handle.AxisIndex = Axis;
                Handle.Color = AxisColors[Axis];
                Handle.WorldA = Axes[Axis];
                AddLoop(Handle, MakeArrayView(Ring));
            }
        }
    }
}

void FNGonViewportGizmo::AddLoop(FHandle& Handle, TConstArrayView<FVector2D> Points)
{
    for (int32 Index = 0; Index < Points.Num(); ++Index)
    {
        Handle.Segments.Add({ Points[Index], Points[(Index + 1) % Points.Num()] });
    }
}

int32 FNGonViewportGizmo::HitTest(const FVector2D& Pixel, int32& SegmentIndex) const
{
    int32 Best = INDEX_NONE;
    SegmentIndex = INDEX_NONE;
    double BestDistance = FMath::Square(7.0 * PixelScale);
    for (int32 Index = 0; Index < Handles.Num(); ++Index)
    {
        const auto& Segments = Handles[Index].Segments;
        for (int32 LineIndex = 0; LineIndex < Segments.Num(); ++LineIndex)
        {
            const FSegment& Segment = Segments[LineIndex];
            const FVector2D Edge = Segment.B - Segment.A;
            const double LengthSquared = Edge.SizeSquared();
            if (LengthSquared <= UE_DOUBLE_SMALL_NUMBER)
            {
                continue;
            }
            const double Along = FMath::Clamp(FVector2D::DotProduct(Edge, Pixel - Segment.A) / LengthSquared, 0.0, 1.0);
            const double Distance = FVector2D::DistSquared(Pixel, Segment.A + Along * Edge);
            if (Distance < BestDistance)
            {
                BestDistance = Distance;
                Best = Index;
                SegmentIndex = LineIndex;
            }
        }
    }
    return Best;
}

void FNGonViewportGizmo::Draw(FCanvas& Canvas, const TOptional<FVector2D>& HoverPixel) const
{
    int32 Segment;
    const int32 Hovered = HoverPixel.IsSet() && !DragHandle.IsSet() ? HitTest(HoverPixel.GetValue(), Segment) : INDEX_NONE;
    const double InverseDPI = 1.0 / FMath::Max(static_cast<double>(Canvas.GetDPIScale()), 0.01);
    const FLinearColor Outline(0.016f, 0.019f, 0.023f);
    for (const FHandle& Handle : Handles)
    {
        for (const FSegment& Line : Handle.Segments)
        {
            DrawSegment(Canvas, Line.A * InverseDPI, Line.B * InverseDPI, Outline, static_cast<float>(6.0 * PixelScale));
        }
    }
    for (int32 Index = 0; Index < Handles.Num(); ++Index)
    {
        const FHandle& Handle = Handles[Index];
        const bool bActive = DragHandle.IsSet() && Handle.Action == DragHandle->Action
            && Handle.AxisIndex == DragHandle->AxisIndex && Handle.SecondAxisIndex == DragHandle->SecondAxisIndex;
        for (const FSegment& Line : Handle.Segments)
        {
            DrawSegment(Canvas, Line.A * InverseDPI, Line.B * InverseDPI, bActive || Index == Hovered ? ActiveColor : Handle.Color,
                static_cast<float>(2.8 * PixelScale));
        }
    }
}

bool FNGonViewportGizmo::BeginDrag(const FVector2D& Pixel)
{
    int32 SegmentIndex;
    const int32 Index = HitTest(Pixel, SegmentIndex);
    if (Index == INDEX_NONE)
    {
        return false;
    }
    DragHandle = Handles[Index];
    DragFrame = Frame;
    DragStart = Pixel;
    DragPixelScale = PixelScale;
    if (DragHandle->Action == EAction::Rotate)
    {
        const FSegment& Segment = DragHandle->Segments[SegmentIndex];
        const FVector2D Tangent = Segment.B - Segment.A;
        RotationTangent = Tangent.GetSafeNormal();
        RotationPixelsPerRadian = FMath::Max(Tangent.Size() * RingSegments / (2.0 * UE_DOUBLE_PI), 20.0 * PixelScale);
    }
    return true;
}

FTransform FNGonViewportGizmo::DragTo(const FVector2D& Pixel) const
{
    FTransform Result = DragFrame;
    if (!DragHandle.IsSet())
    {
        return Result;
    }
    const FHandle& Handle = DragHandle.GetValue();
    const FVector2D Delta = Pixel - DragStart;
    const FVector2D A = Handle.PixelsPerUnitA;
    const FVector2D B = Handle.PixelsPerUnitB;
    switch (Handle.Action)
    {
    case EAction::MoveAxis:
        Result.AddToTranslation(Handle.WorldA * (FVector2D::DotProduct(Delta, A) / A.SizeSquared()));
        break;
    case EAction::MovePlane:
    {
        const double Det = FVector2D::CrossProduct(A, B);
        if (FMath::Abs(Det) > UE_DOUBLE_SMALL_NUMBER)
        {
            Result.AddToTranslation((Handle.WorldA * FVector2D::CrossProduct(Delta, B)
                + Handle.WorldB * FVector2D::CrossProduct(A, Delta)) / Det);
        }
        break;
    }
    case EAction::Rotate:
    {
        const double Angle = FVector2D::DotProduct(Delta, RotationTangent) / RotationPixelsPerRadian;
        Result.SetRotation((FQuat(Handle.WorldA, Angle) * DragFrame.GetRotation()).GetNormalized());
        break;
    }
    case EAction::ScaleAxis:
    case EAction::ScaleUniform:
    {
        const bool bUniform = Handle.Action == EAction::ScaleUniform;
        const FVector2D Direction = bUniform ? FVector2D(1, -1).GetSafeNormal() : A.GetSafeNormal();
        const double Factor = FMath::Clamp(1.0 + FVector2D::DotProduct(Delta, Direction) / (80.0 * DragPixelScale), 0.05, 100.0);
        FVector Scale = FVector::OneVector;
        if (bUniform)
        {
            Scale = FVector(Factor);
        }
        else
        {
            Scale[Handle.AxisIndex] = Factor;
        }
        Result.SetScale3D(Scale);
        break;
    }
    }
    return Result;
}
