#include "NGonViewportGizmo.h"

#include "CanvasItem.h"
#include "CanvasTypes.h"

namespace
{
constexpr double ScreenSizeBase = 86.0;
constexpr double AxisTipLengthPx = 16.0;
constexpr double AxisTipWidthPx = 10.0;
constexpr double ScaleCubeWidthPx = 9.5;
constexpr double PlaneStartFrac = 0.24;
constexpr double PlaneEndFrac = 0.42;
constexpr double TranslateViewPlaneRadiusPx = 7.5;
constexpr double TranslateViewPlaneHitRadiusPx = 12.5;
constexpr double UniformScaleHalfSizePx = 5.5;
constexpr double RotateViewRingScale = 1.14;
constexpr double AxisThicknessPx = 2.4;
constexpr double ActiveThicknessPx = 3.5;
constexpr double DashLengthPx = 5.5;
constexpr double DashGapPx = 4.5;
constexpr double HiddenRotationAlpha = 0.34;
constexpr double AxisHitRadiusPx = 9.5;
constexpr double RingHitRadiusPx = 8.5;
constexpr int32 ConeSegments = 10;
constexpr int32 RingSegmentsCount = 96;

FLinearColor FromSRGB(uint8 R, uint8 G, uint8 B, float A = 1.0f)
{
    FLinearColor C = FLinearColor::FromSRGBColor(FColor(R, G, B));
    C.A = A;
    return C;
}

bool IsXPart(ENGonGizmoPart Part)
{
    return Part == ENGonGizmoPart::TranslateX || Part == ENGonGizmoPart::RotateX || Part == ENGonGizmoPart::ScaleX;
}

bool IsYPart(ENGonGizmoPart Part)
{
    return Part == ENGonGizmoPart::TranslateY || Part == ENGonGizmoPart::RotateY || Part == ENGonGizmoPart::ScaleY;
}

bool IsZPart(ENGonGizmoPart Part)
{
    return Part == ENGonGizmoPart::TranslateZ || Part == ENGonGizmoPart::RotateZ || Part == ENGonGizmoPart::ScaleZ;
}

bool IsXYPart(ENGonGizmoPart Part)
{
    return Part == ENGonGizmoPart::TranslateXY || Part == ENGonGizmoPart::ScaleXY;
}

bool IsXZPart(ENGonGizmoPart Part)
{
    return Part == ENGonGizmoPart::TranslateXZ || Part == ENGonGizmoPart::ScaleXZ;
}

bool IsYZPart(ENGonGizmoPart Part)
{
    return Part == ENGonGizmoPart::TranslateYZ || Part == ENGonGizmoPart::ScaleYZ;
}

FLinearColor GetPartColor(ENGonGizmoPart Part)
{
    if (IsXPart(Part)) return FromSRGB(225, 72, 65);
    if (IsYPart(Part)) return FromSRGB(85, 205, 110);
    if (IsZPart(Part)) return FromSRGB(80, 132, 235);
    if (IsXYPart(Part)) return FromSRGB(205, 190, 68, 0.88f);
    if (IsXZPart(Part)) return FromSRGB(205, 82, 205, 0.88f);
    if (IsYZPart(Part)) return FromSRGB(72, 190, 205, 0.88f);
    return FromSRGB(225, 225, 225);
}

FLinearColor GetHighlightColor()
{
    return FromSRGB(255, 214, 64);
}

double DistanceToSegment2D(const FVector2D& Point, const FVector2D& A, const FVector2D& B)
{
    const FVector2D Edge = B - A;
    const double LenSq = Edge.SizeSquared();
    if (LenSq <= 1.0e-6)
    {
        return FVector2D::Distance(Point, A);
    }
    const double T = FMath::Clamp(FVector2D::DotProduct(Point - A, Edge) / LenSq, 0.0, 1.0);
    return FVector2D::Distance(Point, A + Edge * T);
}

bool PointInQuad2D(const FVector2D& Point, const FVector2D Quad[4])
{
    double TwiceArea = 0.0;
    for (int32 i = 0; i < 4; ++i)
    {
        const FVector2D& A = Quad[i];
        const FVector2D& B = Quad[(i + 1) % 4];
        TwiceArea += A.X * B.Y - B.X * A.Y;
    }
    if (FMath::Abs(TwiceArea) < 4.0)
    {
        return false;
    }

    double Sign = 0.0;
    for (int32 i = 0; i < 4; ++i)
    {
        const FVector2D& A = Quad[i];
        const FVector2D& B = Quad[(i + 1) % 4];
        const double Cross = (B.X - A.X) * (Point.Y - A.Y) - (B.Y - A.Y) * (Point.X - A.X);
        if (FMath::Abs(Cross) <= 1.0e-4)
        {
            continue;
        }
        if (Sign == 0.0)
        {
            Sign = Cross;
        }
        else if ((Sign > 0.0) != (Cross > 0.0))
        {
            return false;
        }
    }
    return true;
}

void DrawLine2D(FCanvas& Canvas, const FVector2D& A, const FVector2D& B, const FLinearColor& Color, float Thickness)
{
    FCanvasLineItem Line(A, B);
    Line.SetColor(Color);
    Line.LineThickness = Thickness;
    Canvas.DrawItem(Line);
}

void DrawFilledTriangle2D(FCanvas& Canvas, const FVector2D& A, const FVector2D& B, const FVector2D& C, const FLinearColor& Color)
{
    const double EdgeBC = FVector2D::Distance(B, C);
    const double EdgeAB = FVector2D::Distance(A, B);
    const double EdgeAC = FVector2D::Distance(A, C);
    const double MaxSpan = FMath::Max3(EdgeBC, EdgeAB, EdgeAC);
    if (MaxSpan < 0.5)
    {
        return;
    }
    const int32 Steps = FMath::Clamp(FMath::CeilToInt(EdgeBC * 1.15), 4, 18);
    for (int32 i = 0; i <= Steps; ++i)
    {
        const double Alpha = static_cast<double>(i) / static_cast<double>(Steps);
        const FVector2D Target = FMath::Lerp(B, C, Alpha);
        DrawLine2D(Canvas, A, Target, Color, 1.65f);
    }
}

void DrawFilledQuad2D(FCanvas& Canvas, const FVector2D& Q0, const FVector2D& Q1, const FVector2D& Q2, const FVector2D& Q3, const FLinearColor& Color)
{
    const double SpanA = FVector2D::Distance(Q0, Q3);
    const double SpanB = FVector2D::Distance(Q1, Q2);
    const double MaxSpan = FMath::Max(SpanA, SpanB);
    if (MaxSpan < 0.5)
    {
        return;
    }
    const int32 Steps = FMath::Clamp(FMath::CeilToInt(MaxSpan * 0.85), 4, 16);
    for (int32 i = 0; i <= Steps; ++i)
    {
        const double Alpha = static_cast<double>(i) / static_cast<double>(Steps);
        const FVector2D Left = FMath::Lerp(Q0, Q3, Alpha);
        const FVector2D Right = FMath::Lerp(Q1, Q2, Alpha);
        DrawLine2D(Canvas, Left, Right, Color, 1.55f);
    }
}

void DrawDashedLineContinuous2D(
    FCanvas& Canvas,
    const FVector2D& From,
    const FVector2D& To,
    const FLinearColor& Color,
    float Thickness,
    double DashLen,
    double GapLen,
    double& InOutPhase)
{
    const FVector2D Delta = To - From;
    const double Length = Delta.Size();
    if (Length <= 1.0e-4)
    {
        return;
    }
    const double Dash = FMath::Max(DashLen, 1.0);
    const double Gap = FMath::Max(GapLen, 0.0);
    const double Period = FMath::Max(Dash + Gap, 1.0);
    const FVector2D Dir = Delta / Length;

    double Cursor = 0.0;
    while (Cursor < Length)
    {
        const double Cycle = FMath::Fmod(FMath::Max(InOutPhase, 0.0), Period);
        const bool bDrawing = Cycle < Dash;
        const double RemainingInState = bDrawing ? (Dash - Cycle) : (Period - Cycle);
        const double Span = FMath::Min(RemainingInState, Length - Cursor);
        if (bDrawing && Span > 1.0e-4)
        {
            DrawLine2D(Canvas, From + Dir * Cursor, From + Dir * (Cursor + Span), Color, Thickness);
        }
        Cursor += Span;
        InOutPhase = FMath::Fmod(InOutPhase + Span, Period);
    }
}

void DrawCircle2D(FCanvas& Canvas, const FVector2D& Center, double Radius, const FLinearColor& Color, float Thickness)
{
    if (Radius <= 0.5)
    {
        return;
    }
    const int32 Segments = FMath::Clamp(FMath::CeilToInt(Radius * 0.85), 24, 96);
    FVector2D Prev(Center.X + Radius, Center.Y);
    for (int32 i = 1; i <= Segments; ++i)
    {
        const double Angle = (2.0 * UE_DOUBLE_PI * static_cast<double>(i)) / static_cast<double>(Segments);
        const FVector2D Curr(Center.X + FMath::Cos(Angle) * Radius, Center.Y + FMath::Sin(Angle) * Radius);
        DrawLine2D(Canvas, Prev, Curr, Color, Thickness);
        Prev = Curr;
    }
}

FTransform OrthonormalizeGizmoFrame(const FTransform& InFrame)
{
    FVector X = InFrame.GetUnitAxis(EAxis::X).GetSafeNormal();
    if (X.IsNearlyZero()) X = FVector::UnitX();

    FVector Y = InFrame.GetUnitAxis(EAxis::Y);
    Y = (Y - X * FVector::DotProduct(Y, X)).GetSafeNormal();
    if (Y.IsNearlyZero())
    {
        FVector FallbackZ;
        X.FindBestAxisVectors(Y, FallbackZ);
    }

    FVector Z = FVector::CrossProduct(X, Y).GetSafeNormal();
    if (FVector::DotProduct(Z, InFrame.GetUnitAxis(EAxis::Z)) < 0.0)
    {
        Y = -Y;
        Z = -Z;
    }

    const FMatrix Basis = FRotationMatrix::MakeFromXY(X, Y);
    return FTransform(FQuat(Basis), InFrame.GetLocation(), FVector::OneVector);
}
} // anonymous namespace

void FNGonViewportGizmo::Build(
    const FTransform& InFrame,
    ENGonGizmoMode InMode,
    double InWorldUnitsPerPixel,
    double InDPIScale,
    const FVector& InViewLocation,
    const FVector& InViewForward,
    const FVector& InViewRight,
    const FVector& InViewUp,
    bool bInAllowAxisScale,
    TFunctionRef<bool(const FVector&, FVector2D&)> InProject)
{
    Axes.Reset();
    PlaneQuads.Reset();
    Rings.Reset();
    bOriginValid = false;
    bCenterCubeValid = false;

    // Freeze the gizmo orientation during an active drag so changing normals do not wobble axes mid-gesture
    if (bDragging)
    {
        Frame = FTransform(DragStartFrame.GetRotation(), InFrame.GetLocation(), FVector::OneVector);
    }
    else
    {
        Frame = OrthonormalizeGizmoFrame(InFrame);
    }

    Mode = InMode;
    PixelScale = FMath::Clamp(InDPIScale, 1.0, 2.5);
    WorldUnitsPerPixel = InWorldUnitsPerPixel;
    ViewLocation = InViewLocation;
    ViewForward = InViewForward.GetSafeNormal();
    ViewRight = InViewRight.GetSafeNormal();
    ViewUp = InViewUp.GetSafeNormal();
    bAllowAxisScale = bInAllowAxisScale;

    if (Mode == ENGonGizmoMode::None || !FMath::IsFinite(WorldUnitsPerPixel) || WorldUnitsPerPixel <= 0.0)
    {
        return;
    }

    const FVector Origin = Frame.GetLocation();
    if (!InProject(Origin, OriginPx))
    {
        return;
    }
    bOriginValid = true;

    const double ScreenSize = ScreenSizeBase * PixelScale;
    GizmoWorldLength = ScreenSize * WorldUnitsPerPixel;

    // Project all 3D gizmo geometry orthographically at Origin's depth plane so the gizmo never changes screen size or perspective-warps during Alt+LMB camera rotation
    const double InvUnitsPerPixel = 1.0 / FMath::Max(WorldUnitsPerPixel, 1.0e-6);
    auto ProjectGizmoPoint = [&](const FVector& WorldPt, FVector2D& OutPx) -> bool
    {
        const FVector Offset = WorldPt - Origin;
        OutPx.X = OriginPx.X + FVector::DotProduct(Offset, ViewRight) * InvUnitsPerPixel;
        OutPx.Y = OriginPx.Y - FVector::DotProduct(Offset, ViewUp) * InvUnitsPerPixel;
        return true;
    };

    const FVector FrameAxes[3] = {
        Frame.GetUnitAxis(EAxis::X),
        Frame.GetUnitAxis(EAxis::Y),
        Frame.GetUnitAxis(EAxis::Z)
    };

    const bool bCombined = (Mode == ENGonGizmoMode::Combined);
    const bool bTranslate = bCombined || (Mode == ENGonGizmoMode::Translate);
    const bool bRotate = bCombined || (Mode == ENGonGizmoMode::Rotate);
    const bool bScale = bCombined || (Mode == ENGonGizmoMode::Scale);

    // 1. Build 3D Axes (Translate Cones and/or Frame-Aligned 3D Scale Cubes)
    auto Build3DAxis = [&](ENGonGizmoPart Part, int32 AxisIdx, double LengthScale, bool bCubeTip)
    {
        const FVector AxisUnit = FrameAxes[AxisIdx];
        const FVector OrthoU = FrameAxes[(AxisIdx + 1) % 3];
        const FVector OrthoV = FrameAxes[(AxisIdx + 2) % 3];
        const double Len = GizmoWorldLength * LengthScale;
        const FVector TipWorld = Origin + AxisUnit * Len;

        FProjectedAxis OutAxis;
        OutAxis.Part = Part;
        OutAxis.AxisWorld = AxisUnit;
        OutAxis.OrthoU = OrthoU;
        OutAxis.OrthoV = OrthoV;
        OutAxis.Color = GetPartColor(Part);
        OutAxis.StartPx = OriginPx;
        OutAxis.bIsScaleCube = bCubeTip;

        if (!ProjectGizmoPoint(TipWorld, OutAxis.EndPx))
        {
            return;
        }
        OutAxis.TipPx = OutAxis.EndPx;

        if (bCubeTip)
        {
            const double Half = GizmoWorldLength * ((ScaleCubeWidthPx * 0.5) / ScreenSizeBase);
            const FVector CubeCenter = TipWorld - AxisUnit * Half;
            const FVector NearCenter = CubeCenter - AxisUnit * Half;
            if (!ProjectGizmoPoint(NearCenter, OutAxis.ShaftEndPx))
            {
                OutAxis.ShaftEndPx = OutAxis.EndPx;
            }

            const FVector Corners3D[8] = {
                CubeCenter - AxisUnit * Half - OrthoU * Half - OrthoV * Half,
                CubeCenter - AxisUnit * Half + OrthoU * Half - OrthoV * Half,
                CubeCenter - AxisUnit * Half + OrthoU * Half + OrthoV * Half,
                CubeCenter - AxisUnit * Half - OrthoU * Half + OrthoV * Half,
                CubeCenter + AxisUnit * Half - OrthoU * Half - OrthoV * Half,
                CubeCenter + AxisUnit * Half + OrthoU * Half - OrthoV * Half,
                CubeCenter + AxisUnit * Half + OrthoU * Half + OrthoV * Half,
                CubeCenter + AxisUnit * Half - OrthoU * Half + OrthoV * Half
            };
            bool bAllCorners = true;
            for (int32 c = 0; c < 8; ++c)
            {
                bAllCorners &= ProjectGizmoPoint(Corners3D[c], OutAxis.CubeCornersPx[c]);
            }
            OutAxis.bHasCube = bAllCorners;
        }
        else
        {
            const double ConeLen = GizmoWorldLength * (AxisTipLengthPx / ScreenSizeBase);
            const double ConeRad = GizmoWorldLength * ((AxisTipWidthPx * 0.5) / ScreenSizeBase);
            const FVector BaseCenter = TipWorld - AxisUnit * ConeLen;
            if (!ProjectGizmoPoint(BaseCenter, OutAxis.ShaftEndPx))
            {
                OutAxis.ShaftEndPx = OutAxis.EndPx;
            }
            for (int32 i = 0; i < ConeSegments; ++i)
            {
                const double Angle = (2.0 * UE_DOUBLE_PI * static_cast<double>(i)) / static_cast<double>(ConeSegments);
                const FVector RingPt = BaseCenter + (OrthoU * FMath::Cos(Angle) + OrthoV * FMath::Sin(Angle)) * ConeRad;
                FVector2D ProjPt;
                if (ProjectGizmoPoint(RingPt, ProjPt))
                {
                    OutAxis.ConeBasePx.Add(ProjPt);
                }
            }
        }

        OutAxis.bValid = true;
        Axes.Add(OutAxis);
    };

    if (bTranslate)
    {
        Build3DAxis(ENGonGizmoPart::TranslateX, 0, 1.0, false);
        Build3DAxis(ENGonGizmoPart::TranslateY, 1, 1.0, false);
        Build3DAxis(ENGonGizmoPart::TranslateZ, 2, 1.0, false);
    }

    if (bScale && bAllowAxisScale)
    {
        const double ScaleAxisLen = bCombined ? 0.62 : 1.0;
        Build3DAxis(ENGonGizmoPart::ScaleX, 0, ScaleAxisLen, true);
        Build3DAxis(ENGonGizmoPart::ScaleY, 1, ScaleAxisLen, true);
        Build3DAxis(ENGonGizmoPart::ScaleZ, 2, ScaleAxisLen, true);
    }

    // Build 3D Center Uniform Scale Cube when in Scale or Combined mode
    if (bScale)
    {
        const double Half = GizmoWorldLength * ((UniformScaleHalfSizePx * 1.15) / ScreenSizeBase);
        const FVector UX = FrameAxes[0];
        const FVector UY = FrameAxes[1];
        const FVector UZ = FrameAxes[2];
        const FVector Corners3D[8] = {
            Origin - UX * Half - UY * Half - UZ * Half,
            Origin + UX * Half - UY * Half - UZ * Half,
            Origin + UX * Half + UY * Half - UZ * Half,
            Origin - UX * Half + UY * Half - UZ * Half,
            Origin - UX * Half - UY * Half + UZ * Half,
            Origin + UX * Half - UY * Half + UZ * Half,
            Origin + UX * Half + UY * Half + UZ * Half,
            Origin - UX * Half + UY * Half + UZ * Half
        };
        bool bAll = true;
        for (int32 c = 0; c < 8; ++c)
        {
            bAll &= ProjectGizmoPoint(Corners3D[c], CenterCubeCornersPx[c]);
        }
        bCenterCubeValid = bAll;
    }

    // 2. Build 3D Planar Handles (XY, XZ, YZ)
    if (bTranslate || (bScale && bAllowAxisScale))
    {
        const ENGonGizmoPart PlaneParts[3] = {
            bTranslate ? ENGonGizmoPart::TranslateXY : ENGonGizmoPart::ScaleXY,
            bTranslate ? ENGonGizmoPart::TranslateXZ : ENGonGizmoPart::ScaleXZ,
            bTranslate ? ENGonGizmoPart::TranslateYZ : ENGonGizmoPart::ScaleYZ
        };
        const int32 PairA[3] = { 0, 0, 1 };
        const int32 PairB[3] = { 1, 2, 2 };

        for (int32 p = 0; p < 3; ++p)
        {
            const FVector& A = FrameAxes[PairA[p]];
            const FVector& B = FrameAxes[PairB[p]];
            const FVector Pts3D[4] = {
                Origin + (A + B) * (GizmoWorldLength * PlaneStartFrac),
                Origin + A * (GizmoWorldLength * PlaneEndFrac) + B * (GizmoWorldLength * PlaneStartFrac),
                Origin + (A + B) * (GizmoWorldLength * PlaneEndFrac),
                Origin + A * (GizmoWorldLength * PlaneStartFrac) + B * (GizmoWorldLength * PlaneEndFrac)
            };
            FProjectedQuad Quad;
            Quad.Part = PlaneParts[p];
            Quad.Color = GetPartColor(Quad.Part);
            bool bAllProj = true;
            for (int32 c = 0; c < 4; ++c)
            {
                bAllProj &= ProjectGizmoPoint(Pts3D[c], Quad.Corners[c]);
            }
            if (bAllProj)
            {
                // Skip nearly edge-on planes so they don't clutter or steal hits from axes
                double TwiceArea = 0.0;
                for (int32 c = 0; c < 4; ++c)
                {
                    const FVector2D& P0 = Quad.Corners[c];
                    const FVector2D& P1 = Quad.Corners[(c + 1) % 4];
                    TwiceArea += P0.X * P1.Y - P1.X * P0.Y;
                }
                if (FMath::Abs(TwiceArea) >= 16.0 * PixelScale)
                {
                    Quad.bValid = true;
                    PlaneQuads.Add(Quad);
                }
            }
        }
    }

    // 3. Build 3D Rotation Rings (with front solid / back dashed classification) + Outer View Ring
    if (bRotate)
    {
        const ENGonGizmoPart RotParts[3] = {
            ENGonGizmoPart::RotateX,
            ENGonGizmoPart::RotateY,
            ENGonGizmoPart::RotateZ
        };
        const double RingRadius = GizmoWorldLength * (bCombined ? 0.78 : 1.0);
        const FVector TowardCamera = -ViewForward;

        for (int32 r = 0; r < 3; ++r)
        {
            const FVector Axis = FrameAxes[r];
            const FVector U = FrameAxes[(r + 1) % 3];
            const FVector V = FrameAxes[(r + 2) % 3];

            FProjectedRing Ring;
            Ring.Part = RotParts[r];
            Ring.AxisWorld = Axis;
            Ring.Color = GetPartColor(Ring.Part);

            FVector PrevWorld = Origin + U * RingRadius;
            FVector2D PrevPx;
            bool bPrevValid = ProjectGizmoPoint(PrevWorld, PrevPx);

            for (int32 s = 1; s <= RingSegmentsCount; ++s)
            {
                const double Angle = (2.0 * UE_DOUBLE_PI * static_cast<double>(s)) / static_cast<double>(RingSegmentsCount);
                const FVector CurrWorld = Origin + (U * FMath::Cos(Angle) + V * FMath::Sin(Angle)) * RingRadius;
                FVector2D CurrPx;
                const bool bCurrValid = ProjectGizmoPoint(CurrWorld, CurrPx);
                if (bPrevValid && bCurrValid)
                {
                    const FVector MidWorld = (PrevWorld + CurrWorld) * 0.5;
                    const FVector Radial = (MidWorld - Origin).GetSafeNormal();
                    const bool bBack = FVector::DotProduct(Radial, TowardCamera) < 0.0;
                    Ring.Segments.Add({ PrevPx, CurrPx, bBack });
                }
                PrevWorld = CurrWorld;
                PrevPx = CurrPx;
                bPrevValid = bCurrValid;
            }
            Rings.Add(Ring);
        }

        // Outer camera view-plane rotation ring
        FProjectedRing ViewRing;
        ViewRing.Part = ENGonGizmoPart::RotateView;
        ViewRing.AxisWorld = ViewForward;
        ViewRing.Color = GetPartColor(ENGonGizmoPart::RotateView);
        ViewRing.bIsViewRing = true;
        ViewRing.ViewRingRadiusPx = static_cast<float>(ScreenSize * RotateViewRingScale);
        Rings.Add(ViewRing);
    }
}

FVector FNGonViewportGizmo::PartAxis(const FTransform& InFrame, ENGonGizmoPart Part) const
{
    if (IsXPart(Part)) return InFrame.GetUnitAxis(EAxis::X);
    if (IsYPart(Part)) return InFrame.GetUnitAxis(EAxis::Y);
    if (IsZPart(Part)) return InFrame.GetUnitAxis(EAxis::Z);
    if (Part == ENGonGizmoPart::RotateView) return ViewForward;
    return FVector::ZeroVector;
}

bool FNGonViewportGizmo::PartPlane(const FTransform& InFrame, ENGonGizmoPart Part, FVector& OutAxisA, FVector& OutAxisB) const
{
    if (IsXYPart(Part))
    {
        OutAxisA = InFrame.GetUnitAxis(EAxis::X);
        OutAxisB = InFrame.GetUnitAxis(EAxis::Y);
        return true;
    }
    if (IsXZPart(Part))
    {
        OutAxisA = InFrame.GetUnitAxis(EAxis::X);
        OutAxisB = InFrame.GetUnitAxis(EAxis::Z);
        return true;
    }
    if (IsYZPart(Part))
    {
        OutAxisA = InFrame.GetUnitAxis(EAxis::Y);
        OutAxisB = InFrame.GetUnitAxis(EAxis::Z);
        return true;
    }
    if (Part == ENGonGizmoPart::TranslateViewPlane)
    {
        OutAxisA = ViewRight;
        OutAxisB = ViewUp;
        return true;
    }
    return false;
}

ENGonGizmoPart FNGonViewportGizmo::HitTest(const FVector2D& Pixel, int32* OutRingSegIdx, const FProjectedRing** OutRing) const
{
    if (OutRingSegIdx) *OutRingSegIdx = INDEX_NONE;
    if (OutRing) *OutRing = nullptr;
    if (!bOriginValid || Mode == ENGonGizmoMode::None)
    {
        return ENGonGizmoPart::None;
    }

    // 1. Center handle priority (TranslateViewPlane or ScaleUniform)
    if (Mode == ENGonGizmoMode::Translate)
    {
        if (FVector2D::Distance(Pixel, OriginPx) <= TranslateViewPlaneHitRadiusPx * PixelScale)
        {
            return ENGonGizmoPart::TranslateViewPlane;
        }
    }
    else if (Mode == ENGonGizmoMode::Scale || Mode == ENGonGizmoMode::Combined)
    {
        if (FVector2D::Distance(Pixel, OriginPx) <= FMath::Max(AxisHitRadiusPx, UniformScaleHalfSizePx + 3.0) * PixelScale)
        {
            return ENGonGizmoPart::ScaleUniform;
        }
    }

    // 2. Planar handles (XY, XZ, YZ)
    for (const FProjectedQuad& Quad : PlaneQuads)
    {
        if (Quad.bValid && PointInQuad2D(Pixel, Quad.Corners))
        {
            return Quad.Part;
        }
    }

    // 3. Rotation Rings (when in Rotate or Combined mode)
    if (!Rings.IsEmpty())
    {
        double BestRingDist = RingHitRadiusPx * PixelScale;
        ENGonGizmoPart BestRingPart = ENGonGizmoPart::None;
        int32 BestSeg = INDEX_NONE;
        const FProjectedRing* BestRingPtr = nullptr;

        for (const FProjectedRing& Ring : Rings)
        {
            if (Ring.bIsViewRing)
            {
                const double DistToCircle = FMath::Abs(FVector2D::Distance(Pixel, OriginPx) - Ring.ViewRingRadiusPx);
                if (DistToCircle <= BestRingDist)
                {
                    BestRingDist = DistToCircle;
                    BestRingPart = Ring.Part;
                    BestRingPtr = &Ring;
                    BestSeg = INDEX_NONE;
                }
            }
            else
            {
                for (int32 s = 0; s < Ring.Segments.Num(); ++s)
                {
                    // Prioritize front-facing arc segments slightly over back-facing dashed segments
                    const double Penalty = Ring.Segments[s].bBackFacing ? (1.5 * PixelScale) : 0.0;
                    const double D = DistanceToSegment2D(Pixel, Ring.Segments[s].A, Ring.Segments[s].B) + Penalty;
                    if (D <= BestRingDist)
                    {
                        BestRingDist = D;
                        BestRingPart = Ring.Part;
                        BestRingPtr = &Ring;
                        BestSeg = s;
                    }
                }
            }
        }
        if (BestRingPart != ENGonGizmoPart::None)
        {
            if (OutRingSegIdx) *OutRingSegIdx = BestSeg;
            if (OutRing) *OutRing = BestRingPtr;
            return BestRingPart;
        }
    }

    // 4. Axis shafts & 3D Cone / 3D Cube tips
    double BestAxisDist = AxisHitRadiusPx * PixelScale;
    ENGonGizmoPart BestAxisPart = ENGonGizmoPart::None;
    for (const FProjectedAxis& Axis : Axes)
    {
        if (!Axis.bValid)
        {
            continue;
        }
        double D = DistanceToSegment2D(Pixel, Axis.StartPx, Axis.EndPx);
        if (Axis.bHasCube)
        {
            for (int32 c = 0; c < 8; ++c)
            {
                D = FMath::Min(D, FVector2D::Distance(Pixel, Axis.CubeCornersPx[c]));
            }
        }
        else
        {
            for (const FVector2D& BasePt : Axis.ConeBasePx)
            {
                D = FMath::Min(D, DistanceToSegment2D(Pixel, Axis.TipPx, BasePt));
            }
        }
        if (D <= BestAxisDist)
        {
            BestAxisDist = D;
            BestAxisPart = Axis.Part;
        }
    }
    if (BestAxisPart != ENGonGizmoPart::None)
    {
        return BestAxisPart;
    }

    // 5. Common interior area of the Rotation Gizmo: clicking/dragging anywhere inside the rotation sphere
    // captures the input for gizmo trackball rotation (weighting X/Y/Z rotation by drag direction) and blocks mesh picking
    if (Mode == ENGonGizmoMode::Rotate || Mode == ENGonGizmoMode::Combined)
    {
        const double CommonRotateRadiusPx = (Mode == ENGonGizmoMode::Combined)
            ? (ScreenSizeBase * 0.78 * PixelScale)
            : (ScreenSizeBase * RotateViewRingScale * PixelScale);
        if (FVector2D::Distance(Pixel, OriginPx) <= CommonRotateRadiusPx)
        {
            return ENGonGizmoPart::RotateTrackball;
        }
    }

    return ENGonGizmoPart::None;
}

void FNGonViewportGizmo::Draw(FCanvas& Canvas, const TOptional<FVector2D>& HoverPixel) const
{
    if (!bOriginValid || Mode == ENGonGizmoMode::None)
    {
        return;
    }

    const double InvDPI = 1.0 / FMath::Max(static_cast<double>(Canvas.GetDPIScale()), 0.01);
    auto ToCanvas = [InvDPI](const FVector2D& Px) -> FVector2D
    {
        return Px * InvDPI;
    };

    const ENGonGizmoPart HoveredPart = (!bDragging && HoverPixel.IsSet())
        ? HitTest(HoverPixel.GetValue())
        : ENGonGizmoPart::None;

    auto IsHighlighted = [&](ENGonGizmoPart Part) -> bool
    {
        return (bDragging && ActivePart == Part) || (!bDragging && HoveredPart == Part);
    };

    const float BaseThickness = static_cast<float>(AxisThicknessPx);
    const float HighThickness = static_cast<float>(ActiveThicknessPx);

    // 1. Draw Planar Handles (TranslateXY/XZ/YZ or ScaleXY/XZ/YZ) with translucent fill + crisp outline
    for (const FProjectedQuad& Quad : PlaneQuads)
    {
        if (!Quad.bValid)
        {
            continue;
        }
        const bool bHigh = IsHighlighted(Quad.Part);
        const FLinearColor EdgeCol = bHigh ? GetHighlightColor() : Quad.Color;
        FLinearColor FillCol = EdgeCol;
        FillCol.A = bHigh ? 0.50f : 0.28f;

        const FVector2D C0 = ToCanvas(Quad.Corners[0]);
        const FVector2D C1 = ToCanvas(Quad.Corners[1]);
        const FVector2D C2 = ToCanvas(Quad.Corners[2]);
        const FVector2D C3 = ToCanvas(Quad.Corners[3]);

        DrawFilledQuad2D(Canvas, C0, C1, C2, C3, FillCol);
        const float OutlineThick = bHigh ? 2.2f : 1.4f;
        DrawLine2D(Canvas, C0, C1, EdgeCol, OutlineThick);
        DrawLine2D(Canvas, C1, C2, EdgeCol, OutlineThick);
        DrawLine2D(Canvas, C2, C3, EdgeCol, OutlineThick);
        DrawLine2D(Canvas, C3, C0, EdgeCol, OutlineThick);
    }

    // 2. Draw Rotation Rings (solid front arc + continuous dashed hidden back arc, plus outer view ring)
    if (IsHighlighted(ENGonGizmoPart::RotateTrackball))
    {
        const double TrackballRadius = ScreenSizeBase * ((Mode == ENGonGizmoMode::Combined) ? 0.78 : 1.0) * PixelScale * InvDPI;
        FLinearColor TrackballCol = GetHighlightColor();
        TrackballCol.A = 0.55f;
        DrawCircle2D(Canvas, ToCanvas(OriginPx), TrackballRadius, TrackballCol, 1.8f);
    }
    for (const FProjectedRing& Ring : Rings)
    {
        const bool bHigh = IsHighlighted(Ring.Part);
        const FLinearColor Col = bHigh ? GetHighlightColor() : Ring.Color;
        const float Thick = bHigh ? HighThickness : BaseThickness;

        if (Ring.bIsViewRing)
        {
            DrawCircle2D(Canvas, ToCanvas(OriginPx), Ring.ViewRingRadiusPx * InvDPI, Col, Thick);
            continue;
        }

        FLinearColor HiddenCol = Col;
        HiddenCol.A *= static_cast<float>(HiddenRotationAlpha);

        bool bPrevBack = false;
        double DashPhase = 0.0;
        for (const FRingSegment& Seg : Ring.Segments)
        {
            const FVector2D From = ToCanvas(Seg.A);
            const FVector2D To = ToCanvas(Seg.B);
            if (Seg.bBackFacing)
            {
                if (!bPrevBack)
                {
                    DashPhase = 0.0;
                }
                DrawDashedLineContinuous2D(Canvas, From, To, HiddenCol, Thick, DashLengthPx, DashGapPx, DashPhase);
            }
            else
            {
                DrawLine2D(Canvas, From, To, Col, Thick);
                if (bPrevBack)
                {
                    DashPhase = 0.0;
                }
            }
            bPrevBack = Seg.bBackFacing;
        }
    }

    // 3. Draw 3D Axes (Shafts + 3D Cone Fans for Translate, Frame-Aligned 3D Cubes for Scale)
    static constexpr int32 CubeFaces[6][4] = {
        { 0, 1, 2, 3 }, { 4, 7, 6, 5 }, { 0, 4, 5, 1 },
        { 1, 5, 6, 2 }, { 2, 6, 7, 3 }, { 3, 7, 4, 0 }
    };
    static constexpr int32 CubeEdges[12][2] = {
        { 0, 1 }, { 1, 2 }, { 2, 3 }, { 3, 0 },
        { 4, 5 }, { 5, 6 }, { 6, 7 }, { 7, 4 },
        { 0, 4 }, { 1, 5 }, { 2, 6 }, { 3, 7 }
    };

    for (const FProjectedAxis& Axis : Axes)
    {
        if (!Axis.bValid)
        {
            continue;
        }
        const bool bHigh = IsHighlighted(Axis.Part);
        const FLinearColor Col = bHigh ? GetHighlightColor() : Axis.Color;
        const float Thick = bHigh ? HighThickness : BaseThickness;

        const FVector2D StartC = ToCanvas(Axis.StartPx);
        const FVector2D ShaftEndC = ToCanvas(Axis.ShaftEndPx);
        DrawLine2D(Canvas, StartC, ShaftEndC, Col, Thick);

        if (Axis.bIsScaleCube && Axis.bHasCube)
        {
            FVector2D ScreenC[8];
            for (int32 i = 0; i < 8; ++i)
            {
                ScreenC[i] = ToCanvas(Axis.CubeCornersPx[i]);
            }
            for (int32 f = 0; f < 6; ++f)
            {
                DrawFilledQuad2D(
                    Canvas,
                    ScreenC[CubeFaces[f][0]],
                    ScreenC[CubeFaces[f][1]],
                    ScreenC[CubeFaces[f][2]],
                    ScreenC[CubeFaces[f][3]],
                    Col);
            }
            const FLinearColor EdgeOutline = bHigh ? FLinearColor::White : Col * 0.72f;
            for (int32 e = 0; e < 12; ++e)
            {
                DrawLine2D(Canvas, ScreenC[CubeEdges[e][0]], ScreenC[CubeEdges[e][1]], EdgeOutline, bHigh ? 1.6f : 1.1f);
            }
        }
        else if (!Axis.bIsScaleCube && Axis.ConeBasePx.Num() >= 3)
        {
            const FVector2D TipC = ToCanvas(Axis.TipPx);
            const int32 NumB = Axis.ConeBasePx.Num();
            for (int32 i = 0; i < NumB; ++i)
            {
                const FVector2D A = ToCanvas(Axis.ConeBasePx[i]);
                const FVector2D B = ToCanvas(Axis.ConeBasePx[(i + 1) % NumB]);
                DrawFilledTriangle2D(Canvas, TipC, A, B, Col);
                DrawLine2D(Canvas, A, B, Col * 0.85f, 1.2f);
            }
        }
    }

    // 4. Draw Center Handle (View-Plane Circle for Translate, 3D Frame-Aligned Center Cube for Scale)
    if (Mode == ENGonGizmoMode::Translate)
    {
        const bool bHigh = IsHighlighted(ENGonGizmoPart::TranslateViewPlane);
        const FLinearColor Col = bHigh ? GetHighlightColor() : GetPartColor(ENGonGizmoPart::TranslateViewPlane);
        DrawCircle2D(Canvas, ToCanvas(OriginPx), TranslateViewPlaneRadiusPx * PixelScale * InvDPI, Col, bHigh ? HighThickness : BaseThickness);
    }
    else if ((Mode == ENGonGizmoMode::Scale || Mode == ENGonGizmoMode::Combined) && bCenterCubeValid)
    {
        const bool bHigh = IsHighlighted(ENGonGizmoPart::ScaleUniform);
        const FLinearColor Col = bHigh ? GetHighlightColor() : GetPartColor(ENGonGizmoPart::ScaleUniform);
        FVector2D ScreenC[8];
        for (int32 i = 0; i < 8; ++i)
        {
            ScreenC[i] = ToCanvas(CenterCubeCornersPx[i]);
        }
        for (int32 f = 0; f < 6; ++f)
        {
            DrawFilledQuad2D(
                Canvas,
                ScreenC[CubeFaces[f][0]],
                ScreenC[CubeFaces[f][1]],
                ScreenC[CubeFaces[f][2]],
                ScreenC[CubeFaces[f][3]],
                Col);
        }
        for (int32 e = 0; e < 12; ++e)
        {
            DrawLine2D(Canvas, ScreenC[CubeEdges[e][0]], ScreenC[CubeEdges[e][1]], Col * 0.70f, 1.1f);
        }
    }
}

FVector2D FNGonViewportGizmo::WorldDeltaPerPixel(const FVector& WorldDir) const
{
    for (const FProjectedAxis& Ax : Axes)
    {
        if (Ax.bValid && FVector::DotProduct(Ax.AxisWorld, WorldDir) > 0.99)
        {
            return (Ax.EndPx - OriginPx) / GizmoWorldLength;
        }
    }
    const double R = FVector::DotProduct(WorldDir, ViewRight) / FMath::Max(WorldUnitsPerPixel, 1.0e-6);
    const double U = -FVector::DotProduct(WorldDir, ViewUp) / FMath::Max(WorldUnitsPerPixel, 1.0e-6);
    return FVector2D(R, U);
}

bool FNGonViewportGizmo::BeginDrag(const FVector2D& Pixel)
{
    int32 SegIdx = INDEX_NONE;
    const FProjectedRing* HitRing = nullptr;
    const ENGonGizmoPart Hit = HitTest(Pixel, &SegIdx, &HitRing);
    if (Hit == ENGonGizmoPart::None)
    {
        return false;
    }

    ActivePart = Hit;
    bDragging = true;
    DragStartFrame = Frame;
    DragStartPixel = Pixel;
    DragStartWorldUnitsPerPixel = WorldUnitsPerPixel;
    DragStartPixelScale = PixelScale;

    FVector AxisA, AxisB;
    if (PartPlane(DragStartFrame, ActivePart, AxisA, AxisB))
    {
        DragScreenAxisA = WorldDeltaPerPixel(AxisA);
        DragScreenAxisB = WorldDeltaPerPixel(AxisB);
    }
    else if (ActivePart == ENGonGizmoPart::RotateTrackball)
    {
        DragRotationTangent = FVector2D(1.0, 0.0);
        DragRotationPixelsPerRadian = FMath::Max(ScreenSizeBase * PixelScale, 35.0);
    }
    else if (ActivePart == ENGonGizmoPart::RotateX || ActivePart == ENGonGizmoPart::RotateY ||
             ActivePart == ENGonGizmoPart::RotateZ || ActivePart == ENGonGizmoPart::RotateView)
    {
        if (HitRing && !HitRing->bIsViewRing && HitRing->Segments.IsValidIndex(SegIdx))
        {
            const FVector2D SegVec = HitRing->Segments[SegIdx].B - HitRing->Segments[SegIdx].A;
            DragRotationTangent = SegVec.GetSafeNormal();
            DragRotationPixelsPerRadian = FMath::Max(SegVec.Size() * RingSegmentsCount / (2.0 * UE_DOUBLE_PI), 24.0 * PixelScale);
        }
        else
        {
            const FVector2D Radial = (Pixel - OriginPx).GetSafeNormal();
            // Positive rotation around ViewForward (into screen) moves CamRight toward +CamUp (screen-CCW, since pixel +Y is down)
            DragRotationTangent = FVector2D(Radial.Y, -Radial.X);
            if (DragRotationTangent.IsNearlyZero())
            {
                DragRotationTangent = FVector2D(1.0, 0.0);
            }
            DragRotationPixelsPerRadian = FMath::Max(ScreenSizeBase * PixelScale, 35.0);
        }
    }
    else
    {
        const FVector Axis = PartAxis(DragStartFrame, ActivePart);
        DragScreenAxisA = WorldDeltaPerPixel(Axis);
    }

    return true;
}

FTransform FNGonViewportGizmo::DragTo(const FVector2D& Pixel) const
{
    FTransform Result = DragStartFrame;
    if (!bDragging || ActivePart == ENGonGizmoPart::None)
    {
        return Result;
    }

    const FVector2D Delta = Pixel - DragStartPixel;

    // 1. Translation (Single Axis, Plane XY/XZ/YZ, or ViewPlane)
    if (ActivePart == ENGonGizmoPart::TranslateX || ActivePart == ENGonGizmoPart::TranslateY || ActivePart == ENGonGizmoPart::TranslateZ)
    {
        const double LenSq = DragScreenAxisA.SizeSquared();
        if (LenSq > 1.0e-4)
        {
            const FVector WorldAxis = PartAxis(DragStartFrame, ActivePart);
            const double Amount = FVector2D::DotProduct(Delta, DragScreenAxisA) / LenSq;
            Result.AddToTranslation(WorldAxis * Amount);
        }
        return Result;
    }
    if (ActivePart == ENGonGizmoPart::TranslateXY || ActivePart == ENGonGizmoPart::TranslateXZ ||
        ActivePart == ENGonGizmoPart::TranslateYZ || ActivePart == ENGonGizmoPart::TranslateViewPlane)
    {
        FVector AxisA, AxisB;
        if (PartPlane(DragStartFrame, ActivePart, AxisA, AxisB))
        {
            if (ActivePart == ENGonGizmoPart::TranslateViewPlane)
            {
                Result.AddToTranslation((ViewRight * Delta.X - ViewUp * Delta.Y) * DragStartWorldUnitsPerPixel);
            }
            else
            {
                const double Det = FVector2D::CrossProduct(DragScreenAxisA, DragScreenAxisB);
                if (FMath::Abs(Det) > 1.0e-4)
                {
                    const double MoveA = FVector2D::CrossProduct(Delta, DragScreenAxisB) / Det;
                    const double MoveB = FVector2D::CrossProduct(DragScreenAxisA, Delta) / Det;
                    Result.AddToTranslation(AxisA * MoveA + AxisB * MoveB);
                }
            }
        }
        return Result;
    }

    // 2. Rotation (RotateX, RotateY, RotateZ, RotateView, and common-area RotateTrackball)
    if (ActivePart == ENGonGizmoPart::RotateTrackball)
    {
        const double DragPixels = Delta.Size();
        if (DragPixels > 1.0e-4)
        {
            const FVector2D DragDir2D = Delta / DragPixels;
            const FVector DragWorldDir = (ViewRight * DragDir2D.X - ViewUp * DragDir2D.Y).GetSafeNormal();

            // Weight each of the 3 gizmo axes (X, Y, Z) by how strongly its front-hemisphere tangent aligns with the drag direction
            const FVector AxisX = DragStartFrame.GetUnitAxis(EAxis::X);
            const FVector AxisY = DragStartFrame.GetUnitAxis(EAxis::Y);
            const FVector AxisZ = DragStartFrame.GetUnitAxis(EAxis::Z);
            const FVector TowardCam = -ViewForward;

            const double WeightX = FVector::DotProduct(FVector::CrossProduct(AxisX, TowardCam), DragWorldDir);
            const double WeightY = FVector::DotProduct(FVector::CrossProduct(AxisY, TowardCam), DragWorldDir);
            const double WeightZ = FVector::DotProduct(FVector::CrossProduct(AxisZ, TowardCam), DragWorldDir);

            FVector AvgWorldAxis = (AxisX * WeightX + AxisY * WeightY + AxisZ * WeightZ).GetSafeNormal();
            if (AvgWorldAxis.IsNearlyZero())
            {
                AvgWorldAxis = FVector::CrossProduct(TowardCam, DragWorldDir).GetSafeNormal();
            }
            if (!AvgWorldAxis.IsNearlyZero())
            {
                const double Angle = DragPixels / FMath::Max(DragRotationPixelsPerRadian, 1.0);
                Result.SetRotation((FQuat(AvgWorldAxis, Angle) * DragStartFrame.GetRotation()).GetNormalized());
            }
        }
        return Result;
    }
    if (ActivePart == ENGonGizmoPart::RotateX || ActivePart == ENGonGizmoPart::RotateY ||
        ActivePart == ENGonGizmoPart::RotateZ || ActivePart == ENGonGizmoPart::RotateView)
    {
        const FVector WorldAxis = PartAxis(DragStartFrame, ActivePart).GetSafeNormal();
        const double Angle = FVector2D::DotProduct(Delta, DragRotationTangent) / FMath::Max(DragRotationPixelsPerRadian, 1.0);
        Result.SetRotation((FQuat(WorldAxis, Angle) * DragStartFrame.GetRotation()).GetNormalized());
        return Result;
    }

    // 3. Scale (ScaleUniform, ScaleX/Y/Z, ScaleXY/XZ/YZ)
    auto ClampScale = [](double RelDelta) -> double
    {
        return FMath::Clamp(1.0 + RelDelta, 0.02, 100.0);
    };

    if (ActivePart == ENGonGizmoPart::ScaleUniform)
    {
        const double Rel = (Delta.X - Delta.Y) * 0.70710678 / FMath::Max(ScreenSizeBase * DragStartPixelScale, 1.0);
        Result.SetScale3D(FVector(ClampScale(Rel)));
        return Result;
    }

    FVector ScaleVec = FVector::OneVector;
    if (ActivePart == ENGonGizmoPart::ScaleX || ActivePart == ENGonGizmoPart::ScaleY || ActivePart == ENGonGizmoPart::ScaleZ)
    {
        const FVector2D Dir = DragScreenAxisA.GetSafeNormal();
        const double Rel = FVector2D::DotProduct(Delta, Dir) / FMath::Max(ScreenSizeBase * DragStartPixelScale, 1.0);
        const double Factor = ClampScale(Rel);
        if (ActivePart == ENGonGizmoPart::ScaleX) ScaleVec.X = Factor;
        else if (ActivePart == ENGonGizmoPart::ScaleY) ScaleVec.Y = Factor;
        else ScaleVec.Z = Factor;
    }
    else if (ActivePart == ENGonGizmoPart::ScaleXY || ActivePart == ENGonGizmoPart::ScaleXZ || ActivePart == ENGonGizmoPart::ScaleYZ)
    {
        const FVector2D DirA = DragScreenAxisA.GetSafeNormal();
        const FVector2D DirB = DragScreenAxisB.GetSafeNormal();
        const double FactorA = ClampScale(FVector2D::DotProduct(Delta, DirA) / FMath::Max(ScreenSizeBase * DragStartPixelScale, 1.0));
        const double FactorB = ClampScale(FVector2D::DotProduct(Delta, DirB) / FMath::Max(ScreenSizeBase * DragStartPixelScale, 1.0));
        if (ActivePart == ENGonGizmoPart::ScaleXY) { ScaleVec.X = FactorA; ScaleVec.Y = FactorB; }
        else if (ActivePart == ENGonGizmoPart::ScaleXZ) { ScaleVec.X = FactorA; ScaleVec.Z = FactorB; }
        else { ScaleVec.Y = FactorA; ScaleVec.Z = FactorB; }
    }
    Result.SetScale3D(ScaleVec);
    return Result;
}

void FNGonViewportGizmo::Build(
    const FTransform& InFrame,
    ENGonGizmoMode InMode,
    double InWorldUnitsPerPixel,
    double InDPIScale,
    const FVector& InViewRight,
    const FVector& InViewUp,
    bool bInAllowAxisScale,
    TFunctionRef<bool(const FVector&, FVector2D&)> InProject)
{
    const FVector Right = InViewRight.GetSafeNormal();
    const FVector Up = InViewUp.GetSafeNormal();
    FVector Forward = FVector::CrossProduct(Right, Up).GetSafeNormal();
    if (Forward.IsNearlyZero())
    {
        Forward = -FVector::UpVector;
    }
    const FVector FallbackViewLocation = InFrame.GetLocation() - Forward * 500.0;
    Build(
        InFrame,
        InMode,
        InWorldUnitsPerPixel,
        InDPIScale,
        FallbackViewLocation,
        Forward,
        Right,
        Up,
        bInAllowAxisScale,
        InProject);
}
