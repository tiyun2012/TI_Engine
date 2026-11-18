#include "NGonUIIcons.h"

#include "Framework/Application/SlateApplication.h"
#include "InputCoreTypes.h"
#include "Styling/AppStyle.h"
#include "Styling/CoreStyle.h"
#include "Styling/StyleDefaults.h"

namespace
{
TMap<const SNGonPreview*, NGon::FNGonSkeletonTreeUIState> GSkeletonTreeUIStateMap;
}

namespace NGon
{
FNGonSkeletonTreeUIState& GetSkeletonTreeUIState(const SNGonPreview* Preview)
{
    return GSkeletonTreeUIStateMap.FindOrAdd(Preview);
}

FNGonSkeletonTreeUIState* FindUIStateByViewport(const SNGonViewport* InViewport)
{
    if (!InViewport)
    {
        return nullptr;
    }
    for (TPair<const SNGonPreview*, FNGonSkeletonTreeUIState>& Pair : GSkeletonTreeUIStateMap)
    {
        if (Pair.Value.ViewportPtr == InViewport)
        {
            return &Pair.Value;
        }
    }
    return nullptr;
}

const FSlateBrush* TryGetNativeUnrealBrush(ENGonTreeIconKind Kind)
{
    auto TryBrush = [](FName BrushName) -> const FSlateBrush*
    {
        const FSlateBrush* Brush = FAppStyle::Get().GetOptionalBrush(BrushName, nullptr, nullptr);
        if (Brush && Brush != FAppStyle::GetDefaultBrush() && Brush != FStyleDefaults::GetNoBrush() && Brush->GetResourceName() != NAME_None)
        {
            return Brush;
        }
        return nullptr;
    };

    switch (Kind)
    {
    case ENGonTreeIconKind::Bone:
        if (const FSlateBrush* B = TryBrush(TEXT("SkeletonTree.Bone"))) return B;
        return TryBrush(TEXT("Persona.Tabs.SkeletonTree"));
    case ENGonTreeIconKind::Socket:
        return TryBrush(TEXT("SkeletonTree.Socket"));
    case ENGonTreeIconKind::TabSkeleton:
        if (const FSlateBrush* B = TryBrush(TEXT("Persona.Tabs.SkeletonTree"))) return B;
        return TryBrush(TEXT("SkeletonTree.Bone"));
    case ENGonTreeIconKind::TabMorph:
        if (const FSlateBrush* B = TryBrush(TEXT("Persona.Tabs.MorphTargets"))) return B;
        return TryBrush(TEXT("ClassIcon.MorphTarget"));
    case ENGonTreeIconKind::TabCurves:
        if (const FSlateBrush* B = TryBrush(TEXT("Persona.Tabs.AnimCurvePreviewer"))) return B;
        return TryBrush(TEXT("GenericCurveEditor.TabIcon"));
    case ENGonTreeIconKind::Plus:
        if (const FSlateBrush* B = TryBrush(TEXT("Icons.Plus"))) return B;
        return TryBrush(TEXT("Plus"));
    case ENGonTreeIconKind::ToolSelect:
        if (const FSlateBrush* B = TryBrush(TEXT("LevelEditor.SelectMode"))) return B;
        return TryBrush(TEXT("EditorViewport.SelectMode"));
    case ENGonTreeIconKind::ToolTranslate:
        return TryBrush(TEXT("EditorViewport.TranslateMode"));
    case ENGonTreeIconKind::ToolRotate:
        return TryBrush(TEXT("EditorViewport.RotateMode"));
    case ENGonTreeIconKind::ToolScale:
        return TryBrush(TEXT("EditorViewport.ScaleMode"));
    case ENGonTreeIconKind::CoordSpace:
        if (const FSlateBrush* B = TryBrush(TEXT("EditorViewport.RelativeCoordinateSystem_Local"))) return B;
        return TryBrush(TEXT("Icons.Transform"));
    case ENGonTreeIconKind::MoreDots:
        return TryBrush(TEXT("Icons.Settings"));
    case ENGonTreeIconKind::SnapSurface:
        return TryBrush(TEXT("EditorViewport.SurfaceSnapping"));
    case ENGonTreeIconKind::SnapGrid:
        return TryBrush(TEXT("EditorViewport.LocationGridSnap"));
    case ENGonTreeIconKind::SnapRotate:
        return TryBrush(TEXT("EditorViewport.RotationGridSnap"));
    case ENGonTreeIconKind::SnapScale:
        return TryBrush(TEXT("EditorViewport.ScaleGridSnap"));
    case ENGonTreeIconKind::ViewCamera:
        if (const FSlateBrush* B = TryBrush(TEXT("EditorViewport.Perspective"))) return B;
        return TryBrush(TEXT("Icons.Camera"));
    case ENGonTreeIconKind::ViewCamSpeed:
        return TryBrush(TEXT("EditorViewport.CamSpeedSetting"));
    case ENGonTreeIconKind::ViewLit:
        return TryBrush(TEXT("EditorViewport.LitMode"));
    case ENGonTreeIconKind::ViewEye:
        if (const FSlateBrush* B = TryBrush(TEXT("LevelEditor.ViewOptions"))) return B;
        return TryBrush(TEXT("Icons.Visible"));
    case ENGonTreeIconKind::ViewSkull:
        if (const FSlateBrush* B = TryBrush(TEXT("SkeletonTree.Bone"))) return B;
        return TryBrush(TEXT("Persona.Tabs.SkeletonTree"));
    default:
        return nullptr;
    }
}

void DrawBoneIconVector(
    FSlateWindowElementList& OutDrawElements,
    int32 LayerId,
    const FGeometry& AllottedGeometry,
    const FVector2D& Center,
    const FLinearColor& Color)
{
    // 45-degree bone shaft from bottom-left to top-right
    const FVector2D BL = Center + FVector2D(-3.2f, 3.2f);
    const FVector2D TR = Center + FVector2D(3.2f, -3.2f);
    FSlateDrawElement::MakeLines(
        OutDrawElements,
        LayerId,
        AllottedGeometry.ToPaintGeometry(),
        { BL, TR },
        ESlateDrawEffect::None,
        Color,
        true,
        2.1f);

    // Bottom-left and top-right dual condyle knobs (perpendicular to shaft)
    const FVector2D BL_KnobA = Center + FVector2D(-4.8f, 2.1f);
    const FVector2D BL_KnobB = Center + FVector2D(-2.1f, 4.8f);
    FSlateDrawElement::MakeLines(
        OutDrawElements,
        LayerId,
        AllottedGeometry.ToPaintGeometry(),
        { BL_KnobA, BL, BL_KnobB },
        ESlateDrawEffect::None,
        Color,
        true,
        2.2f);

    const FVector2D TR_KnobA = Center + FVector2D(2.1f, -4.8f);
    const FVector2D TR_KnobB = Center + FVector2D(4.8f, -2.1f);
    FSlateDrawElement::MakeLines(
        OutDrawElements,
        LayerId,
        AllottedGeometry.ToPaintGeometry(),
        { TR_KnobA, TR, TR_KnobB },
        ESlateDrawEffect::None,
        Color,
        true,
        2.2f);
}

void DrawSocketIconVector(
    FSlateWindowElementList& OutDrawElements,
    int32 LayerId,
    const FGeometry& AllottedGeometry,
    const FVector2D& Center,
    const FLinearColor& Color)
{
    TArray<FVector2D> Ring;
    constexpr int32 NumPts = 12;
    constexpr float Radius = 4.2f;
    Ring.Reserve(NumPts + 1);
    for (int32 i = 0; i <= NumPts; ++i)
    {
        const float Angle = (static_cast<float>(i) / static_cast<float>(NumPts)) * 2.0f * PI;
        Ring.Add(Center + FVector2D(FMath::Cos(Angle) * Radius, FMath::Sin(Angle) * Radius));
    }
    FSlateDrawElement::MakeLines(
        OutDrawElements,
        LayerId,
        AllottedGeometry.ToPaintGeometry(),
        Ring,
        ESlateDrawEffect::None,
        Color,
        true,
        1.5f);

    // 4 diagonal socket prongs
    for (int32 Corner = 0; Corner < 4; ++Corner)
    {
        const float Angle = (0.25f + 0.5f * static_cast<float>(Corner)) * PI;
        const FVector2D Dir(FMath::Cos(Angle), FMath::Sin(Angle));
        FSlateDrawElement::MakeLines(
            OutDrawElements,
            LayerId,
            AllottedGeometry.ToPaintGeometry(),
            { Center + Dir * 3.8f, Center + Dir * 6.0f },
            ESlateDrawEffect::None,
            Color,
            true,
            1.4f);
    }

    static FSlateBrush SolidBox;
    SolidBox.DrawAs = ESlateBrushDrawType::Box;
    FSlateDrawElement::MakeBox(
        OutDrawElements,
        LayerId,
        AllottedGeometry.ToPaintGeometry(FVector2D(2.4f, 2.4f), FSlateLayoutTransform(Center - FVector2D(1.2f, 1.2f))),
        &SolidBox,
        ESlateDrawEffect::None,
        Color);
}



void SNGonVectorIcon::Construct(const FArguments& InArgs)
{
    Kind = InArgs._Kind;
    IconColor = InArgs._Color;
}

FVector2D SNGonVectorIcon::ComputeDesiredSize(float) const
{
    return FVector2D(14.0f, 14.0f);
}

int32 SNGonVectorIcon::OnPaint(
        const FPaintArgs&,
        const FGeometry& AllottedGeometry,
        const FSlateRect&,
        FSlateWindowElementList& OutDrawElements,
        int32 LayerId,
        const FWidgetStyle&,
        bool) const
    {
        if (const FSlateBrush* NativeBrush = TryGetNativeUnrealBrush(Kind))
        {
            FSlateDrawElement::MakeBox(
                OutDrawElements,
                LayerId,
                AllottedGeometry.ToPaintGeometry(),
                NativeBrush,
                ESlateDrawEffect::None,
                IconColor);
            return LayerId + 1;
        }

        const FVector2D Center = AllottedGeometry.GetLocalSize() * 0.5f;
        switch (Kind)
        {
        case ENGonTreeIconKind::Bone:
        case ENGonTreeIconKind::TabSkeleton:
            DrawBoneIconVector(OutDrawElements, LayerId, AllottedGeometry, Center, IconColor);
            break;
        case ENGonTreeIconKind::Socket:
            DrawSocketIconVector(OutDrawElements, LayerId, AllottedGeometry, Center, IconColor);
            break;
        case ENGonTreeIconKind::TabMorph:
        {
            TArray<FVector2D> HeadRing;
            for (int32 i = 0; i <= 12; ++i)
            {
                const float A = (static_cast<float>(i) / 12.0f) * 2.0f * PI;
                HeadRing.Add(Center + FVector2D(FMath::Cos(A) * 4.8f, FMath::Sin(A) * 4.8f));
            }
            FSlateDrawElement::MakeLines(OutDrawElements, LayerId, AllottedGeometry.ToPaintGeometry(), HeadRing, ESlateDrawEffect::None, IconColor, true, 1.4f);
            FSlateDrawElement::MakeLines(OutDrawElements, LayerId, AllottedGeometry.ToPaintGeometry(),
                { Center + FVector2D(-2.2f, 1.2f), Center + FVector2D(0.0f, 2.6f), Center + FVector2D(2.2f, 1.2f) },
                ESlateDrawEffect::None, IconColor, true, 1.3f);
            break;
        }
        case ENGonTreeIconKind::TabCurves:
        {
            TArray<FVector2D> CurvePts = {
                Center + FVector2D(-5.0f, 4.0f),
                Center + FVector2D(-2.0f, 3.5f),
                Center + FVector2D(0.0f, 0.0f),
                Center + FVector2D(2.0f, -3.5f),
                Center + FVector2D(5.0f, -4.0f)
            };
            FSlateDrawElement::MakeLines(OutDrawElements, LayerId, AllottedGeometry.ToPaintGeometry(), CurvePts, ESlateDrawEffect::None, IconColor, true, 1.6f);
            break;
        }
        case ENGonTreeIconKind::Plus:
        {
            FSlateDrawElement::MakeLines(OutDrawElements, LayerId, AllottedGeometry.ToPaintGeometry(),
                { Center + FVector2D(-4.5f, 0.0f), Center + FVector2D(4.5f, 0.0f) },
                ESlateDrawEffect::None, IconColor, false, 2.0f);
            FSlateDrawElement::MakeLines(OutDrawElements, LayerId, AllottedGeometry.ToPaintGeometry(),
                { Center + FVector2D(0.0f, -4.5f), Center + FVector2D(0.0f, 4.5f) },
                ESlateDrawEffect::None, IconColor, false, 2.0f);
            break;
        }
        case ENGonTreeIconKind::ToolSelect:
        {
            // Cursor arrow icon
            const FVector2D Tip = Center + FVector2D(-3.5f, -5.0f);
            const FVector2D Left = Center + FVector2D(-3.5f, 4.2f);
            const FVector2D Mid = Center + FVector2D(-0.8f, 1.8f);
            const FVector2D Tail = Center + FVector2D(2.2f, 5.2f);
            const FVector2D Right = Center + FVector2D(3.8f, 1.2f);
            FSlateDrawElement::MakeLines(OutDrawElements, LayerId, AllottedGeometry.ToPaintGeometry(),
                { Tip, Left, Mid, Right, Tip }, ESlateDrawEffect::None, IconColor, true, 1.6f);
            FSlateDrawElement::MakeLines(OutDrawElements, LayerId, AllottedGeometry.ToPaintGeometry(),
                { Mid, Tail }, ESlateDrawEffect::None, IconColor, true, 1.6f);
            break;
        }
        case ENGonTreeIconKind::ToolTranslate:
        {
            // 4-way cross move arrows
            FSlateDrawElement::MakeLines(OutDrawElements, LayerId, AllottedGeometry.ToPaintGeometry(),
                { Center + FVector2D(-5.2f, 0.0f), Center + FVector2D(5.2f, 0.0f) }, ESlateDrawEffect::None, IconColor, false, 1.5f);
            FSlateDrawElement::MakeLines(OutDrawElements, LayerId, AllottedGeometry.ToPaintGeometry(),
                { Center + FVector2D(0.0f, -5.2f), Center + FVector2D(0.0f, 5.2f) }, ESlateDrawEffect::None, IconColor, false, 1.5f);
            FSlateDrawElement::MakeLines(OutDrawElements, LayerId, AllottedGeometry.ToPaintGeometry(),
                { Center + FVector2D(-2.0f, -3.5f), Center + FVector2D(0.0f, -5.5f), Center + FVector2D(2.0f, -3.5f) }, ESlateDrawEffect::None, IconColor, true, 1.4f);
            FSlateDrawElement::MakeLines(OutDrawElements, LayerId, AllottedGeometry.ToPaintGeometry(),
                { Center + FVector2D(-2.0f, 3.5f), Center + FVector2D(0.0f, 5.5f), Center + FVector2D(2.0f, 3.5f) }, ESlateDrawEffect::None, IconColor, true, 1.4f);
            FSlateDrawElement::MakeLines(OutDrawElements, LayerId, AllottedGeometry.ToPaintGeometry(),
                { Center + FVector2D(-3.5f, -2.0f), Center + FVector2D(-5.5f, 0.0f), Center + FVector2D(-3.5f, 2.0f) }, ESlateDrawEffect::None, IconColor, true, 1.4f);
            FSlateDrawElement::MakeLines(OutDrawElements, LayerId, AllottedGeometry.ToPaintGeometry(),
                { Center + FVector2D(3.5f, -2.0f), Center + FVector2D(5.5f, 0.0f), Center + FVector2D(3.5f, 2.0f) }, ESlateDrawEffect::None, IconColor, true, 1.4f);
            break;
        }
        case ENGonTreeIconKind::ToolRotate:
        case ENGonTreeIconKind::SnapRotate:
        {
            TArray<FVector2D> Arc;
            for (int32 i = 0; i <= 10; ++i)
            {
                const float A = (-0.35f + 1.55f * (static_cast<float>(i) / 10.0f)) * PI;
                Arc.Add(Center + FVector2D(FMath::Cos(A) * 4.8f, FMath::Sin(A) * 4.8f));
            }
            FSlateDrawElement::MakeLines(OutDrawElements, LayerId, AllottedGeometry.ToPaintGeometry(), Arc, ESlateDrawEffect::None, IconColor, true, 1.6f);
            const FVector2D Tip = Arc.Last();
            FSlateDrawElement::MakeLines(OutDrawElements, LayerId, AllottedGeometry.ToPaintGeometry(),
                { Tip + FVector2D(-2.6f, -1.2f), Tip, Tip + FVector2D(1.4f, -2.4f) }, ESlateDrawEffect::None, IconColor, true, 1.5f);
            break;
        }
        case ENGonTreeIconKind::ToolScale:
        case ENGonTreeIconKind::SnapScale:
        {
            const FVector2D TL = Center + FVector2D(-4.8f, -4.8f);
            const FVector2D TR = Center + FVector2D(4.8f, -4.8f);
            const FVector2D BR = Center + FVector2D(4.8f, 4.8f);
            const FVector2D BL = Center + FVector2D(-4.8f, 4.8f);
            FSlateDrawElement::MakeLines(OutDrawElements, LayerId, AllottedGeometry.ToPaintGeometry(),
                { TL, TR, BR, BL, TL }, ESlateDrawEffect::None, IconColor, false, 1.4f);
            static FSlateBrush InnerBox;
            InnerBox.DrawAs = ESlateBrushDrawType::Box;
            FSlateDrawElement::MakeBox(
                OutDrawElements,
                LayerId + 1,
                AllottedGeometry.ToPaintGeometry(FVector2D(4.2f, 4.2f), FSlateLayoutTransform(Center + FVector2D(-4.8f, 0.6f))),
                &InnerBox,
                ESlateDrawEffect::None,
                IconColor);
            break;
        }
        case ENGonTreeIconKind::CoordSpace:
        {
            // Axis gizmo / cube icon
            FSlateDrawElement::MakeLines(OutDrawElements, LayerId, AllottedGeometry.ToPaintGeometry(),
                { Center, Center + FVector2D(0.0f, -5.5f) }, ESlateDrawEffect::None, IconColor, false, 1.6f);
            FSlateDrawElement::MakeLines(OutDrawElements, LayerId, AllottedGeometry.ToPaintGeometry(),
                { Center, Center + FVector2D(-4.8f, 3.2f) }, ESlateDrawEffect::None, IconColor, false, 1.6f);
            FSlateDrawElement::MakeLines(OutDrawElements, LayerId, AllottedGeometry.ToPaintGeometry(),
                { Center, Center + FVector2D(4.8f, 3.2f) }, ESlateDrawEffect::None, IconColor, false, 1.6f);
            FSlateDrawElement::MakeLines(OutDrawElements, LayerId, AllottedGeometry.ToPaintGeometry(),
                { Center + FVector2D(0.0f, -3.2f), Center + FVector2D(3.0f, -1.2f), Center + FVector2D(3.0f, 2.2f),
                  Center + FVector2D(0.0f, 4.0f), Center + FVector2D(-3.0f, 2.2f), Center + FVector2D(-3.0f, -1.2f),
                  Center + FVector2D(0.0f, -3.2f) }, ESlateDrawEffect::None, IconColor, true, 1.2f);
            break;
        }
        case ENGonTreeIconKind::MoreDots:
        {
            static FSlateBrush DotBrush;
            DotBrush.DrawAs = ESlateBrushDrawType::Box;
            for (int32 i = -1; i <= 1; ++i)
            {
                FSlateDrawElement::MakeBox(
                    OutDrawElements,
                    LayerId,
                    AllottedGeometry.ToPaintGeometry(FVector2D(2.2f, 2.2f), FSlateLayoutTransform(Center + FVector2D(-1.1f, i * 3.8f - 1.1f))),
                    &DotBrush,
                    ESlateDrawEffect::None,
                    IconColor);
            }
            break;
        }
        case ENGonTreeIconKind::CompVertex:
        {
            static FSlateBrush PtBrush;
            PtBrush.DrawAs = ESlateBrushDrawType::Box;
            const FVector2D Offsets[] = { FVector2D(-4.0f, -3.8f), FVector2D(3.5f, -1.2f), FVector2D(-1.5f, 3.8f) };
            for (const FVector2D& Off : Offsets)
            {
                FSlateDrawElement::MakeBox(
                    OutDrawElements,
                    LayerId,
                    AllottedGeometry.ToPaintGeometry(FVector2D(3.0f, 3.0f), FSlateLayoutTransform(Center + Off - FVector2D(1.5f, 1.5f))),
                    &PtBrush,
                    ESlateDrawEffect::None,
                    IconColor);
            }
            break;
        }
        case ENGonTreeIconKind::CompEdge:
        {
            const FVector2D A = Center + FVector2D(-4.5f, -4.0f);
            const FVector2D B = Center + FVector2D(4.8f, -1.5f);
            const FVector2D C = Center + FVector2D(-1.2f, 4.8f);
            FSlateDrawElement::MakeLines(OutDrawElements, LayerId, AllottedGeometry.ToPaintGeometry(),
                { A, B, C, A }, ESlateDrawEffect::None, IconColor, true, 1.6f);
            break;
        }
        case ENGonTreeIconKind::CompFace:
        {
            const FVector2D A = Center + FVector2D(-4.2f, -4.5f);
            const FVector2D B = Center + FVector2D(4.8f, 0.0f);
            const FVector2D C = Center + FVector2D(-4.2f, 4.5f);
            for (int32 r = 0; r <= 6; ++r)
            {
                const float T = static_cast<float>(r) / 6.0f;
                FSlateDrawElement::MakeLines(OutDrawElements, LayerId, AllottedGeometry.ToPaintGeometry(),
                    { FMath::Lerp(A, C, T), B }, ESlateDrawEffect::None, IconColor, true, 1.4f);
            }
            break;
        }
        case ENGonTreeIconKind::OcclusionCube:
        case ENGonTreeIconKind::OverlayHeatmap:
        {
            const FVector2D Top = Center + FVector2D(0.0f, -5.0f);
            const FVector2D L = Center + FVector2D(-4.8f, -2.2f);
            const FVector2D R = Center + FVector2D(4.8f, -2.2f);
            const FVector2D BL = Center + FVector2D(-4.8f, 2.8f);
            const FVector2D BR = Center + FVector2D(4.8f, 2.8f);
            const FVector2D Bot = Center + FVector2D(0.0f, 5.2f);
            FSlateDrawElement::MakeLines(OutDrawElements, LayerId, AllottedGeometry.ToPaintGeometry(),
                { Top, R, BR, Bot, BL, L, Top }, ESlateDrawEffect::None, IconColor, true, 1.4f);
            FSlateDrawElement::MakeLines(OutDrawElements, LayerId, AllottedGeometry.ToPaintGeometry(),
                { L, Center, R }, ESlateDrawEffect::None, IconColor, true, 1.2f);
            FSlateDrawElement::MakeLines(OutDrawElements, LayerId, AllottedGeometry.ToPaintGeometry(),
                { Center, Bot }, ESlateDrawEffect::None, IconColor, false, 1.4f);
            break;
        }
        case ENGonTreeIconKind::OverlayEdges:
        case ENGonTreeIconKind::OverlayVerts:
        {
            const FVector2D TL = Center + FVector2D(-4.5f, -4.2f);
            const FVector2D TR = Center + FVector2D(4.5f, -4.2f);
            const FVector2D BR = Center + FVector2D(4.5f, 4.2f);
            const FVector2D BL = Center + FVector2D(-4.5f, 4.2f);
            FSlateDrawElement::MakeLines(OutDrawElements, LayerId, AllottedGeometry.ToPaintGeometry(),
                { TL, TR, BR, BL, TL }, ESlateDrawEffect::None, IconColor, false, 1.3f);
            if (Kind == ENGonTreeIconKind::OverlayVerts)
            {
                FSlateDrawElement::MakeLines(OutDrawElements, LayerId, AllottedGeometry.ToPaintGeometry(),
                    { Center + FVector2D(0.0f, -4.2f), Center + FVector2D(0.0f, 4.2f) }, ESlateDrawEffect::None, IconColor, false, 1.2f);
            }
            break;
        }
        case ENGonTreeIconKind::SnapMagnet:
        {
            TArray<FVector2D> UShape = {
                Center + FVector2D(-4.0f, -4.5f),
                Center + FVector2D(-4.0f, 1.5f),
                Center + FVector2D(-2.0f, 4.2f),
                Center + FVector2D(2.0f, 4.2f),
                Center + FVector2D(4.0f, 1.5f),
                Center + FVector2D(4.0f, -4.5f)
            };
            FSlateDrawElement::MakeLines(OutDrawElements, LayerId, AllottedGeometry.ToPaintGeometry(), UShape, ESlateDrawEffect::None, IconColor, true, 1.8f);
            break;
        }
        case ENGonTreeIconKind::SnapSurface:
        {
            FSlateDrawElement::MakeLines(OutDrawElements, LayerId, AllottedGeometry.ToPaintGeometry(),
                { Center + FVector2D(-4.8f, 4.2f), Center + FVector2D(4.8f, -4.2f) }, ESlateDrawEffect::None, IconColor, true, 1.5f);
            FSlateDrawElement::MakeLines(OutDrawElements, LayerId, AllottedGeometry.ToPaintGeometry(),
                { Center, Center + FVector2D(-3.8f, -3.8f) }, ESlateDrawEffect::None, IconColor, true, 1.5f);
            break;
        }
        case ENGonTreeIconKind::SnapGrid:
        {
            for (int32 i = -1; i <= 1; ++i)
            {
                const float Offset = i * 3.2f;
                FSlateDrawElement::MakeLines(OutDrawElements, LayerId, AllottedGeometry.ToPaintGeometry(),
                    { Center + FVector2D(-4.5f, Offset), Center + FVector2D(4.5f, Offset) }, ESlateDrawEffect::None, IconColor, false, 1.2f);
                FSlateDrawElement::MakeLines(OutDrawElements, LayerId, AllottedGeometry.ToPaintGeometry(),
                    { Center + FVector2D(Offset, -4.5f), Center + FVector2D(Offset, 4.5f) }, ESlateDrawEffect::None, IconColor, false, 1.2f);
            }
            break;
        }
        case ENGonTreeIconKind::ViewCamera:
        case ENGonTreeIconKind::ViewCamSpeed:
        {
            const FVector2D TL = Center + FVector2D(-5.2f, -3.2f);
            const FVector2D TR = Center + FVector2D(1.5f, -3.2f);
            const FVector2D BR = Center + FVector2D(1.5f, 3.2f);
            const FVector2D BL = Center + FVector2D(-5.2f, 3.2f);
            FSlateDrawElement::MakeLines(OutDrawElements, LayerId, AllottedGeometry.ToPaintGeometry(),
                { TL, TR, BR, BL, TL }, ESlateDrawEffect::None, IconColor, false, 1.5f);
            FSlateDrawElement::MakeLines(OutDrawElements, LayerId, AllottedGeometry.ToPaintGeometry(),
                { Center + FVector2D(1.5f, -1.0f), Center + FVector2D(5.2f, -3.2f), Center + FVector2D(5.2f, 3.2f), Center + FVector2D(1.5f, 1.0f) },
                ESlateDrawEffect::None, IconColor, true, 1.4f);
            break;
        }
        case ENGonTreeIconKind::ViewLit:
        {
            TArray<FVector2D> Ring;
            for (int32 i = 0; i <= 12; ++i)
            {
                const float A = (static_cast<float>(i) / 12.0f) * 2.0f * PI;
                Ring.Add(Center + FVector2D(FMath::Cos(A) * 4.6f, FMath::Sin(A) * 4.6f));
            }
            FSlateDrawElement::MakeLines(OutDrawElements, LayerId, AllottedGeometry.ToPaintGeometry(), Ring, ESlateDrawEffect::None, IconColor, true, 1.6f);
            FSlateDrawElement::MakeLines(OutDrawElements, LayerId, AllottedGeometry.ToPaintGeometry(),
                { Center + FVector2D(-1.8f, -2.5f), Center + FVector2D(-2.5f, 0.0f), Center + FVector2D(-1.2f, 2.5f) },
                ESlateDrawEffect::None, IconColor, true, 1.8f);
            break;
        }
        case ENGonTreeIconKind::ViewEye:
        {
            TArray<FVector2D> TopLid = { Center + FVector2D(-5.5f, 0.0f), Center + FVector2D(-2.2f, -3.4f), Center + FVector2D(2.2f, -3.4f), Center + FVector2D(5.5f, 0.0f) };
            TArray<FVector2D> BotLid = { Center + FVector2D(-5.5f, 0.0f), Center + FVector2D(-2.2f, 3.4f), Center + FVector2D(2.2f, 3.4f), Center + FVector2D(5.5f, 0.0f) };
            FSlateDrawElement::MakeLines(OutDrawElements, LayerId, AllottedGeometry.ToPaintGeometry(), TopLid, ESlateDrawEffect::None, IconColor, true, 1.4f);
            FSlateDrawElement::MakeLines(OutDrawElements, LayerId, AllottedGeometry.ToPaintGeometry(), BotLid, ESlateDrawEffect::None, IconColor, true, 1.4f);
            static FSlateBrush Pupil;
            Pupil.DrawAs = ESlateBrushDrawType::Box;
            FSlateDrawElement::MakeBox(
                OutDrawElements,
                LayerId + 1,
                AllottedGeometry.ToPaintGeometry(FVector2D(3.0f, 3.0f), FSlateLayoutTransform(Center - FVector2D(1.5f, 1.5f))),
                &Pupil,
                ESlateDrawEffect::None,
                IconColor);
            break;
        }
        case ENGonTreeIconKind::ViewSkull:
        {
            // Skull / Skeleton hierarchy icon matching image_3aff81.png
            TArray<FVector2D> Dome = {
                Center + FVector2D(-4.2f, 1.0f),
                Center + FVector2D(-4.2f, -3.2f),
                Center + FVector2D(-2.0f, -5.0f),
                Center + FVector2D(2.0f, -5.0f),
                Center + FVector2D(4.2f, -3.2f),
                Center + FVector2D(4.2f, 1.0f),
                Center + FVector2D(2.4f, 4.6f),
                Center + FVector2D(-2.4f, 4.6f),
                Center + FVector2D(-4.2f, 1.0f)
            };
            FSlateDrawElement::MakeLines(OutDrawElements, LayerId, AllottedGeometry.ToPaintGeometry(), Dome, ESlateDrawEffect::None, IconColor, true, 1.4f);
            FSlateDrawElement::MakeLines(OutDrawElements, LayerId, AllottedGeometry.ToPaintGeometry(),
                { Center + FVector2D(-2.2f, -1.2f), Center + FVector2D(-1.0f, -1.2f) }, ESlateDrawEffect::None, IconColor, false, 2.0f);
            FSlateDrawElement::MakeLines(OutDrawElements, LayerId, AllottedGeometry.ToPaintGeometry(),
                { Center + FVector2D(1.0f, -1.2f), Center + FVector2D(2.2f, -1.2f) }, ESlateDrawEffect::None, IconColor, false, 2.0f);
            FSlateDrawElement::MakeLines(OutDrawElements, LayerId, AllottedGeometry.ToPaintGeometry(),
                { Center + FVector2D(-1.8f, 2.5f), Center + FVector2D(1.8f, 2.5f) }, ESlateDrawEffect::None, IconColor, false, 1.2f);
            break;
        }
        }
        return LayerId + 1;
    }



void SNGonSkeletonTreeRowPrefix::Construct(const FArguments& InArgs)
{
    Depth = FMath::Max(0, InArgs._Depth);
    AncestorHasMoreSiblings = InArgs._AncestorHasMoreSiblings;
    bIsLastChild = InArgs._bIsLastChild;
    bHasChildren = InArgs._bHasChildren;
    bIsExpanded = InArgs._bIsExpanded;
    bIsSocket = InArgs._bIsSocket;
    bFilterActive = InArgs._bFilterActive;
    bIsSelected = InArgs._bIsSelected;
    bIsWeightedBone = InArgs._bIsWeightedBone;
    OnToggleExpand = InArgs._OnToggleExpand;
}

FVector2D SNGonSkeletonTreeRowPrefix::ComputeDesiredSize(float) const
{
    const float TreeColumnsWidth = bFilterActive ? 14.0f : static_cast<float>(Depth + 1) * StepX;
    return FVector2D(TreeColumnsWidth + IconSlotWidth, 20.0f);
}

int32 SNGonSkeletonTreeRowPrefix::OnPaint(
    const FPaintArgs&,
    const FGeometry& AllottedGeometry,
    const FSlateRect&,
    FSlateWindowElementList& OutDrawElements,
    int32 LayerId,
    const FWidgetStyle&,
    bool) const
{
    const FVector2D Size = AllottedGeometry.GetLocalSize();
    const float H = Size.Y;
    const float CenterY = H * 0.5f;

    // Match native UE 5.8 STreeView vertical indent guide lines (subtle dark vertical lines, no horizontal L-bars)
    const FLinearColor GuideLineColor = bIsSelected
        ? FLinearColor(0.55f, 0.72f, 0.95f, 0.35f)
        : FLinearColor(0.22f, 0.23f, 0.25f, 0.85f);

    if (!bFilterActive)
    {
        for (int32 Level = 0; Level < Depth; ++Level)
        {
            const float X = static_cast<float>(Level) * StepX + 7.0f;
            FSlateDrawElement::MakeLines(
                OutDrawElements,
                LayerId,
                AllottedGeometry.ToPaintGeometry(),
                { FVector2D(X, 0.0f), FVector2D(X, H) },
                ESlateDrawEffect::None,
                GuideLineColor,
                false,
                1.0f);
        }

        // Expander arrow at column Depth (using native TreeArrow_Expanded / TreeArrow_Collapsed brush when available)
        if (bHasChildren)
        {
            const float ArrowCenterX = static_cast<float>(Depth) * StepX + 7.0f;
            const FLinearColor ArrowColor = bIsSelected
                ? FLinearColor(0.95f, 0.98f, 1.0f, 1.0f)
                : FLinearColor(0.70f, 0.72f, 0.76f, 1.0f);

            const FName ArrowBrushName = bIsExpanded ? TEXT("TreeArrow_Expanded") : TEXT("TreeArrow_Collapsed");
            const FSlateBrush* ArrowBrush = FAppStyle::Get().GetOptionalBrush(ArrowBrushName, nullptr, nullptr);
            if (ArrowBrush && ArrowBrush != FAppStyle::GetDefaultBrush() && ArrowBrush != FStyleDefaults::GetNoBrush() && ArrowBrush->GetResourceName() != NAME_None)
            {
                FSlateDrawElement::MakeBox(
                    OutDrawElements,
                    LayerId + 1,
                    AllottedGeometry.ToPaintGeometry(FVector2D(10.0f, 10.0f), FSlateLayoutTransform(FVector2D(ArrowCenterX - 5.0f, CenterY - 5.0f))),
                    ArrowBrush,
                    ESlateDrawEffect::None,
                    ArrowColor);
            }
            else if (bIsExpanded)
            {
                for (int32 Row = 0; Row < 4; ++Row)
                {
                    const float HalfW = 3.2f - static_cast<float>(Row) * 0.8f;
                    const float Y = CenterY - 1.5f + static_cast<float>(Row);
                    FSlateDrawElement::MakeLines(
                        OutDrawElements,
                        LayerId + 1,
                        AllottedGeometry.ToPaintGeometry(),
                        { FVector2D(ArrowCenterX - HalfW, Y), FVector2D(ArrowCenterX + HalfW, Y) },
                        ESlateDrawEffect::None,
                        ArrowColor,
                        false,
                        1.2f);
                }
            }
            else
            {
                for (int32 Col = 0; Col < 4; ++Col)
                {
                    const float HalfH = 3.2f - static_cast<float>(Col) * 0.8f;
                    const float X = ArrowCenterX - 1.5f + static_cast<float>(Col);
                    FSlateDrawElement::MakeLines(
                        OutDrawElements,
                        LayerId + 1,
                        AllottedGeometry.ToPaintGeometry(),
                        { FVector2D(X, CenterY - HalfH), FVector2D(X, CenterY + HalfH) },
                        ESlateDrawEffect::None,
                        ArrowColor,
                        false,
                        1.2f);
                }
            }
        }
    }

    // Bone or Socket icon (using native SkeletonTree.Bone / SkeletonTree.BoneNonWeighted / SkeletonTree.Socket brush when available)
    const float IconStartX = bFilterActive ? 14.0f : static_cast<float>(Depth + 1) * StepX;
    const FVector2D IconCenter(IconStartX + IconSlotWidth * 0.5f, CenterY);
    if (bIsSocket)
    {
        const FLinearColor SocketColor = bIsSelected
            ? FLinearColor(1.0f, 1.0f, 1.0f, 1.0f)
            : FLinearColor(0.80f, 0.82f, 0.86f, 1.0f);
        if (const FSlateBrush* SocketBrush = TryGetNativeUnrealBrush(ENGonTreeIconKind::Socket))
        {
            FSlateDrawElement::MakeBox(
                OutDrawElements,
                LayerId + 2,
                AllottedGeometry.ToPaintGeometry(FVector2D(16.0f, 16.0f), FSlateLayoutTransform(IconCenter - FVector2D(8.0f, 8.0f))),
                SocketBrush,
                ESlateDrawEffect::None,
                SocketColor);
        }
        else
        {
            DrawSocketIconVector(OutDrawElements, LayerId + 2, AllottedGeometry, IconCenter, SocketColor);
        }
    }
    else
    {
        const FLinearColor BoneColor = bIsSelected
            ? FLinearColor(1.0f, 1.0f, 1.0f, 1.0f)
            : (bIsWeightedBone
                ? FLinearColor(0.90f, 0.90f, 0.92f, 1.0f)
                : FLinearColor(0.45f, 0.46f, 0.48f, 1.0f));
        const FName BoneBrushName = bIsWeightedBone ? TEXT("SkeletonTree.Bone") : TEXT("SkeletonTree.BoneNonWeighted");
        const FSlateBrush* NativeBoneBrush = FAppStyle::Get().GetOptionalBrush(BoneBrushName, nullptr, nullptr);
        if (!NativeBoneBrush || NativeBoneBrush == FAppStyle::GetDefaultBrush() || NativeBoneBrush == FStyleDefaults::GetNoBrush() || NativeBoneBrush->GetResourceName() == NAME_None)
        {
            NativeBoneBrush = TryGetNativeUnrealBrush(ENGonTreeIconKind::Bone);
        }
        if (NativeBoneBrush)
        {
            FSlateDrawElement::MakeBox(
                OutDrawElements,
                LayerId + 2,
                AllottedGeometry.ToPaintGeometry(FVector2D(16.0f, 16.0f), FSlateLayoutTransform(IconCenter - FVector2D(8.0f, 8.0f))),
                NativeBoneBrush,
                ESlateDrawEffect::None,
                BoneColor);
        }
        else
        {
            DrawBoneIconVector(OutDrawElements, LayerId + 2, AllottedGeometry, IconCenter, BoneColor);
        }
    }

    return LayerId + 3;
}

FReply SNGonSkeletonTreeRowPrefix::OnMouseButtonDown(const FGeometry& MyGeometry, const FPointerEvent& MouseEvent)
    {
        if (!bFilterActive && bHasChildren && MouseEvent.GetEffectingButton() == EKeys::LeftMouseButton)
        {
            const FVector2D LocalPos = MyGeometry.AbsoluteToLocal(MouseEvent.GetScreenSpacePosition());
            const float ArrowMinX = static_cast<float>(Depth) * StepX - 2.0f;
            const float ArrowMaxX = static_cast<float>(Depth + 1) * StepX + 3.0f;
            if (LocalPos.X >= ArrowMinX && LocalPos.X <= ArrowMaxX)
            {
                if (OnToggleExpand.IsBound())
                {
                    OnToggleExpand.Execute();
                }
                return FReply::Handled();
            }
        }
        return FReply::Unhandled();
    }


} // namespace NGon
