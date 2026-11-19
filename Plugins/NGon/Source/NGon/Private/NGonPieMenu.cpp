#include "NGonPieMenu.h"

#include "CanvasItem.h"
#include "CanvasTypes.h"
#include "Engine/Engine.h"
#include "Engine/Font.h"
#include "Engine/Texture2D.h"
#include "Fonts/FontMeasure.h"
#include "Framework/Application/SlateApplication.h"
#include "InputCoreTypes.h"
#include "Styling/CoreStyle.h"
#include "TextureResource.h"
#include "UObject/StrongObjectPtr.h"

namespace
{
constexpr float PieTau = 2.0f * UE_PI;

bool IsInsideBox(const FBox2D& Box, const FVector2D& P)
{
    return P.X >= Box.Min.X && P.X < Box.Max.X && P.Y >= Box.Min.Y && P.Y < Box.Max.Y;
}

float SmoothStep(float Edge0, float Edge1, float X)
{
    const float T = FMath::Clamp((X - Edge0) / FMath::Max(Edge1 - Edge0, 1.e-6f), 0.0f, 1.0f);
    return T * T * (3.0f - 2.0f * T);
}

UTexture2D* CreateSdfTexture(int32 Width, int32 Height, TFunctionRef<FColor(float U, float V)> EvalPixel)
{
    UTexture2D* Tex = UTexture2D::CreateTransient(Width, Height, PF_B8G8R8A8);
    if (!Tex)
    {
        return nullptr;
    }
    Tex->SRGB = false;
    Tex->Filter = TF_Bilinear;
    Tex->AddressX = TA_Clamp;
    Tex->AddressY = TA_Clamp;
    Tex->NeverStream = true;

    FTexturePlatformData* PlatformData = Tex->GetPlatformData();
    if (!PlatformData || PlatformData->Mips.IsEmpty())
    {
        return nullptr;
    }
    FTexture2DMipMap& Mip = PlatformData->Mips[0];
    FColor* Pixels = static_cast<FColor*>(Mip.BulkData.Lock(LOCK_READ_WRITE));
    if (Pixels)
    {
        for (int32 Y = 0; Y < Height; ++Y)
        {
            const float V = (static_cast<float>(Y) + 0.5f) / static_cast<float>(Height);
            for (int32 X = 0; X < Width; ++X)
            {
                const float U = (static_cast<float>(X) + 0.5f) / static_cast<float>(Width);
                Pixels[Y * Width + X] = EvalPixel(U, V);
            }
        }
        Mip.BulkData.Unlock();
        Tex->UpdateResource();
    }
    return Tex;
}

struct FPieSdfTextures
{
    TStrongObjectPtr<UTexture2D> CircleFill;
    TStrongObjectPtr<UTexture2D> CircleRingThin;
    TStrongObjectPtr<UTexture2D> CircleRingThick;
    TStrongObjectPtr<UTexture2D> SoftGlow;
    TStrongObjectPtr<UTexture2D> PillFill;
    TStrongObjectPtr<UTexture2D> PillBorder;

    void EnsureInitialized()
    {
        if (!CircleFill.IsValid())
        {
            // Smoothly anti-aliased disc with subtle top-lit studio glass gradient
            CircleFill.Reset(CreateSdfTexture(128, 128, [](float U, float V)
            {
                const float Dx = U * 2.0f - 1.0f;
                const float Dy = V * 2.0f - 1.0f;
                const float Dist = FMath::Sqrt(Dx * Dx + Dy * Dy);
                const float Alpha = 1.0f - SmoothStep(0.92f, 0.985f, Dist);
                // Subtle top-down sheen (brighter at top, deeper at bottom)
                const float Sheen = FMath::Lerp(1.0f, 0.80f, FMath::Clamp(V, 0.0f, 1.0f));
                // Subtle inner rim highlight near top
                const float InnerRim = SmoothStep(0.78f, 0.92f, Dist) * FMath::Clamp(1.0f - V * 1.4f, 0.0f, 1.0f) * 0.12f;
                const uint8 Lum = static_cast<uint8>(FMath::Clamp((Sheen + InnerRim) * 255.0f, 0.0f, 255.0f));
                const uint8 A = static_cast<uint8>(FMath::Clamp(Alpha * 255.0f, 0.0f, 255.0f));
                return FColor(Lum, Lum, Lum, A);
            }));
        }
        if (!CircleRingThin.IsValid())
        {
            // Ultra-crisp anti-aliased thin circular ring
            CircleRingThin.Reset(CreateSdfTexture(256, 256, [](float U, float V)
            {
                const float Dx = U * 2.0f - 1.0f;
                const float Dy = V * 2.0f - 1.0f;
                const float Dist = FMath::Sqrt(Dx * Dx + Dy * Dy);
                const float Band = FMath::Abs(Dist - 0.945f);
                const float Alpha = 1.0f - SmoothStep(0.012f, 0.032f, Band);
                const uint8 A = static_cast<uint8>(FMath::Clamp(Alpha * 255.0f, 0.0f, 255.0f));
                return FColor(255, 255, 255, A);
            }));
        }
        if (!CircleRingThick.IsValid())
        {
            // Crisp hover ring (2.2px look, zero jaggies)
            CircleRingThick.Reset(CreateSdfTexture(256, 256, [](float U, float V)
            {
                const float Dx = U * 2.0f - 1.0f;
                const float Dy = V * 2.0f - 1.0f;
                const float Dist = FMath::Sqrt(Dx * Dx + Dy * Dy);
                const float Band = FMath::Abs(Dist - 0.935f);
                const float Alpha = 1.0f - SmoothStep(0.028f, 0.052f, Band);
                const uint8 A = static_cast<uint8>(FMath::Clamp(Alpha * 255.0f, 0.0f, 255.0f));
                return FColor(255, 255, 255, A);
            }));
        }
        if (!SoftGlow.IsValid())
        {
            // Smooth radial Gaussian halo for drop shadows and active hover bloom
            SoftGlow.Reset(CreateSdfTexture(128, 128, [](float U, float V)
            {
                const float Dx = U * 2.0f - 1.0f;
                const float Dy = V * 2.0f - 1.0f;
                const float DistSq = Dx * Dx + Dy * Dy;
                const float Falloff = FMath::Exp(-DistSq * 4.2f) * (1.0f - SmoothStep(0.75f, 1.0f, FMath::Sqrt(DistSq)));
                const uint8 A = static_cast<uint8>(FMath::Clamp(Falloff * 255.0f, 0.0f, 255.0f));
                return FColor(255, 255, 255, A);
            }));
        }
        if (!PillFill.IsValid())
        {
            // Anti-aliased rounded pill fill (256x64 aspect)
            PillFill.Reset(CreateSdfTexture(256, 64, [](float U, float V)
            {
                const float Aspect = 4.0f;
                const float X = (U * 2.0f - 1.0f) * Aspect;
                const float Y = V * 2.0f - 1.0f;
                const float HalfX = Aspect - 0.92f;
                const float HalfY = 0.08f;
                const float Qx = FMath::Max(FMath::Abs(X) - HalfX, 0.0f);
                const float Qy = FMath::Max(FMath::Abs(Y) - HalfY, 0.0f);
                const float Sdf = FMath::Sqrt(Qx * Qx + Qy * Qy) - 0.84f;
                const float Alpha = 1.0f - SmoothStep(-0.04f, 0.05f, Sdf);
                const float Sheen = FMath::Lerp(1.0f, 0.86f, V);
                const uint8 Lum = static_cast<uint8>(FMath::Clamp(Sheen * 255.0f, 0.0f, 255.0f));
                const uint8 A = static_cast<uint8>(FMath::Clamp(Alpha * 255.0f, 0.0f, 255.0f));
                return FColor(Lum, Lum, Lum, A);
            }));
        }
        if (!PillBorder.IsValid())
        {
            // Anti-aliased rounded pill border
            PillBorder.Reset(CreateSdfTexture(256, 64, [](float U, float V)
            {
                const float Aspect = 4.0f;
                const float X = (U * 2.0f - 1.0f) * Aspect;
                const float Y = V * 2.0f - 1.0f;
                const float HalfX = Aspect - 0.92f;
                const float HalfY = 0.08f;
                const float Qx = FMath::Max(FMath::Abs(X) - HalfX, 0.0f);
                const float Qy = FMath::Max(FMath::Abs(Y) - HalfY, 0.0f);
                const float Sdf = FMath::Sqrt(Qx * Qx + Qy * Qy) - 0.84f;
                const float Alpha = 1.0f - SmoothStep(0.02f, 0.08f, FMath::Abs(Sdf));
                const uint8 A = static_cast<uint8>(FMath::Clamp(Alpha * 255.0f, 0.0f, 255.0f));
                return FColor(255, 255, 255, A);
            }));
        }
    }
};

FPieSdfTextures& GetPieTextures()
{
    static FPieSdfTextures Textures;
    Textures.EnsureInitialized();
    return Textures;
}

void DrawSpriteCentered(FCanvas& Canvas, UTexture2D* Tex, const FVector2D& Center, const FVector2D& Size, const FLinearColor& Tint)
{
    if (!Tex || !Tex->GetResource() || Size.X <= 0.5 || Size.Y <= 0.5)
    {
        return;
    }
    FCanvasTileItem Tile(Center - Size * 0.5, Tex->GetResource(), Size, Tint);
    Tile.BlendMode = SE_BLEND_Translucent;
    Canvas.DrawItem(Tile);
}

void DrawCircleSdf(FCanvas& Canvas, const FVector2D& Center, float Radius, const FLinearColor& Color)
{
    if (Radius <= 0.5f) return;
    FPieSdfTextures& Tex = GetPieTextures();
    // Compensate for 0.955 SDF edge radius so visual radius matches Radius accurately
    const float Diam = Radius * 2.08f;
    DrawSpriteCentered(Canvas, Tex.CircleFill.Get(), Center, FVector2D(Diam, Diam), Color);
}

void DrawRingSdf(FCanvas& Canvas, const FVector2D& Center, float Radius, const FLinearColor& Color, bool bThick)
{
    if (Radius <= 0.5f) return;
    FPieSdfTextures& Tex = GetPieTextures();
    const float Diam = Radius * 2.12f;
    DrawSpriteCentered(Canvas, bThick ? Tex.CircleRingThick.Get() : Tex.CircleRingThin.Get(), Center, FVector2D(Diam, Diam), Color);
}

void DrawGlowSdf(FCanvas& Canvas, const FVector2D& Center, float Radius, const FLinearColor& Color)
{
    if (Radius <= 0.5f) return;
    FPieSdfTextures& Tex = GetPieTextures();
    DrawSpriteCentered(Canvas, Tex.SoftGlow.Get(), Center, FVector2D(Radius * 2.0f, Radius * 2.0f), Color);
}

void DrawFilledRect(FCanvas& Canvas, const FBox2D& Rect, const FLinearColor& Color)
{
    const FVector2D Sz = Rect.GetSize();
    if (Sz.X <= 0.5 || Sz.Y <= 0.5) return;
    FCanvasTileItem Tile(Rect.Min, Sz, Color);
    Tile.BlendMode = SE_BLEND_Translucent;
    Canvas.DrawItem(Tile);
}

void DrawRectOutline(FCanvas& Canvas, const FBox2D& Rect, const FLinearColor& Color, float Thickness)
{
    const FVector2D Sz = Rect.GetSize();
    if (Sz.X <= 0.5 || Sz.Y <= 0.5) return;
    FCanvasBoxItem Box(Rect.Min, Sz);
    Box.SetColor(Color);
    Box.LineThickness = Thickness;
    Canvas.DrawItem(Box);
}

FVector2D MeasureSlateText(const FString& Text, const FSlateFontInfo& FontInfo)
{
    if (FSlateApplication::IsInitialized())
    {
        if (TSharedPtr<FSlateRenderer> Renderer = FSlateApplication::Get().GetRenderer())
        {
            return Renderer->GetFontMeasureService()->Measure(Text, FontInfo);
        }
    }
    return FVector2D(Text.Len() * 7.0, 14.0);
}

void DrawCenteredSlateText(FCanvas& Canvas, const FSlateFontInfo& FontInfo, const FVector2D& Center, const FString& Text, const FLinearColor& Color)
{
    if (Text.IsEmpty()) return;
    const FVector2D Sz = MeasureSlateText(Text, FontInfo);
    const FVector2D Pos(FMath::RoundToDouble(Center.X - Sz.X * 0.5), FMath::RoundToDouble(Center.Y - Sz.Y * 0.5));
    FCanvasTextItem Item(Pos, FText::FromString(Text), FontInfo, Color);
    Item.EnableShadow(FLinearColor(0.0f, 0.0f, 0.0f, 0.72f), FVector2D(0.0f, 1.0f));
    Canvas.DrawItem(Item);
}
} // anonymous namespace

void FNGonPieMenu::Open(const FVector2D& InCenterLogical, TArray<FNGonPieItem> InItems, TFunction<void(const FString&)> InOnChoose)
{
    if (InItems.IsEmpty() || InItems.Num() > 16 || !InOnChoose)
    {
        Close();
        return;
    }
    RequestedCenter = InCenterLogical;
    Center = InCenterLogical;
    Items = MoveTemp(InItems);
    OnChoose = MoveTemp(InOnChoose);
    Highlighted.Reset();
    Branches.Reset();
    BranchDirection = 1;
    bOpen = true;
}

void FNGonPieMenu::Close()
{
    bOpen = false;
    Highlighted.Reset();
    Branches.Reset();
    Items.Reset();
    OnChoose = nullptr;
}

const FNGonPieItem* FNGonPieMenu::FindItem(const TArray<int32>& Path) const
{
    if (Path.IsEmpty()) return nullptr;
    const TArray<FNGonPieItem>* List = &Items;
    const FNGonPieItem* Curr = nullptr;
    for (const int32 Index : Path)
    {
        if (!List->IsValidIndex(Index)) return nullptr;
        Curr = &(*List)[Index];
        List = &Curr->Children;
    }
    return Curr;
}

const TArray<FNGonPieItem>* FNGonPieMenu::FindChildren(const TArray<int32>& Path) const
{
    const FNGonPieItem* Item = FindItem(Path);
    return Item ? &Item->Children : nullptr;
}

TArray<int32> FNGonPieMenu::SelectedPath(int32 Level) const
{
    TArray<int32> Path;
    if (Branches.IsValidIndex(Level))
    {
        Path = Branches[Level].ParentPath;
        Path.Add(Branches[Level].Selected);
    }
    return Path;
}

void FNGonPieMenu::Arrange(const FVector2D& ViewLogicalSize)
{
    if (!bOpen) return;
    const double Margin = ButtonDiameter * 0.5 + 8.0;
    const double RadialExtent = VisualRadius + Margin;
    Center.X = FMath::Clamp(RequestedCenter.X, RadialExtent, FMath::Max(RadialExtent, ViewLogicalSize.X - RadialExtent));
    Center.Y = FMath::Clamp(RequestedCenter.Y, RadialExtent, FMath::Max(RadialExtent, ViewLogicalSize.Y - RadialExtent));
    ArrangeBranches(ViewLogicalSize);
}

void FNGonPieMenu::RevealBranch(int32 Level)
{
    if (!Branches.IsValidIndex(Level)) return;
    FBranchLevel& Branch = Branches[Level];
    const TArray<FNGonPieItem>* Children = FindChildren(Branch.ParentPath);
    if (!Children || Children->IsEmpty() || Branch.Visible <= 0) return;
    const int32 MaxFirst = FMath::Max(0, Children->Num() - FMath::Min(Children->Num(), Branch.Visible));
    Branch.First = FMath::Clamp(Branch.First, 0, MaxFirst);
    if (Branch.Selected < Branch.First) Branch.First = Branch.Selected;
    if (Branch.Selected >= Branch.First + Branch.Visible) Branch.First = Branch.Selected - Branch.Visible + 1;
}

void FNGonPieMenu::ArrangeBranches(const FVector2D& ViewLogicalSize)
{
    if (Branches.IsEmpty()) return;
    const double Width = FMath::Min(static_cast<double>(BranchWidth), FMath::Max(1.0, ViewLogicalSize.X));

    for (int32 Level = 0; Level < Branches.Num(); ++Level)
    {
        FBranchLevel& Branch = Branches[Level];
        const TArray<FNGonPieItem>* Children = FindChildren(Branch.ParentPath);
        if (!Children || Children->IsEmpty())
        {
            Branches.SetNum(Level);
            break;
        }
        const int32 MaxRowsByHeight = FMath::Max(1, FMath::FloorToInt(ViewLogicalSize.Y / BranchRowHeight));
        Branch.Visible = FMath::Min3(MaxBranchRows, Children->Num(), MaxRowsByHeight);
        const double Height = FMath::Min(ViewLogicalSize.Y, static_cast<double>(Branch.Visible) * BranchRowHeight);

        double Left = 0.0;
        double Top = 0.0;
        if (Level == 0)
        {
            if (!Highlighted.IsSet() || !Items.IsValidIndex(Highlighted.GetValue()))
            {
                Branches.Reset();
                return;
            }
            const float Step = PieTau / static_cast<float>(Items.Num());
            const float Angle = StartAngle + Step * static_cast<float>(Highlighted.GetValue());
            const FVector2D RootPt(Center.X + FMath::Cos(Angle) * VisualRadius, Center.Y + FMath::Sin(Angle) * VisualRadius);

            int32 Dir = BranchDirection;
            const double Candidate = (Dir > 0)
                ? (RootPt.X + ButtonDiameter * 0.5 + BranchGap)
                : (RootPt.X - ButtonDiameter * 0.5 - BranchGap - Width);
            if (Dir > 0 && Candidate + Width > ViewLogicalSize.X && RootPt.X - ButtonDiameter * 0.5 - BranchGap - Width >= 0.0)
            {
                Dir = -1;
            }
            else if (Dir < 0 && Candidate < 0.0 && RootPt.X + ButtonDiameter * 0.5 + BranchGap + Width <= ViewLogicalSize.X)
            {
                Dir = 1;
            }
            BranchDirection = Dir;
            Left = (Dir > 0)
                ? (RootPt.X + ButtonDiameter * 0.5 + BranchGap)
                : (RootPt.X - ButtonDiameter * 0.5 - BranchGap - Width);
            Top = RootPt.Y - BranchRowHeight * 0.5;
        }
        else
        {
            const FBranchLevel& Parent = Branches[Level - 1];
            const double RowTop = Parent.Bounds.Min.Y + static_cast<double>(Parent.Selected - Parent.First) * BranchRowHeight;
            Left = (BranchDirection > 0) ? (Parent.Bounds.Max.X + BranchGap) : (Parent.Bounds.Min.X - BranchGap - Width);
            if (Left + Width > ViewLogicalSize.X && Parent.Bounds.Min.X - BranchGap - Width >= 0.0)
            {
                Left = Parent.Bounds.Min.X - BranchGap - Width;
            }
            else if (Left < 0.0 && Parent.Bounds.Max.X + BranchGap + Width <= ViewLogicalSize.X)
            {
                Left = Parent.Bounds.Max.X + BranchGap;
            }
            Top = RowTop;
        }

        Left = FMath::Clamp(Left, 0.0, FMath::Max(0.0, ViewLogicalSize.X - Width));
        Top = FMath::Clamp(Top, 0.0, FMath::Max(0.0, ViewLogicalSize.Y - Height));
        Branch.Bounds = FBox2D(FVector2D(Left, Top), FVector2D(Left + Width, Top + Height));
        RevealBranch(Level);
    }
}

TOptional<int32> FNGonPieMenu::Sector(const FVector2D& Point) const
{
    if (Items.IsEmpty() || FVector2D::Distance(Point, Center) < DeadZoneRadius)
    {
        return TOptional<int32>();
    }
    const float Step = PieTau / static_cast<float>(Items.Num());
    float Angle = FMath::Atan2(static_cast<float>(Point.Y - Center.Y), static_cast<float>(Point.X - Center.X)) - StartAngle + Step * 0.5f;
    Angle = FMath::Fmod(Angle, PieTau);
    if (Angle < 0.0f) Angle += PieTau;
    return FMath::Min(Items.Num() - 1, static_cast<int32>(Angle / Step));
}

TOptional<int32> FNGonPieMenu::StepEnabled(int32 Start, int32 Direction) const
{
    if (Items.IsEmpty()) return TOptional<int32>();
    int32 Index = FMath::Clamp(Start, 0, Items.Num() - 1);
    for (int32 n = 0; n < Items.Num(); ++n)
    {
        Index = (Direction >= 0) ? ((Index + 1) % Items.Num()) : ((Index + Items.Num() - 1) % Items.Num());
        if (Items[Index].bEnabled) return Index;
    }
    return TOptional<int32>();
}

bool FNGonPieMenu::StepBranch(int32 Level, int32 Direction)
{
    if (!Branches.IsValidIndex(Level)) return false;
    FBranchLevel& Branch = Branches[Level];
    const TArray<FNGonPieItem>* Children = FindChildren(Branch.ParentPath);
    if (!Children || Children->IsEmpty()) return false;

    int32 Index = FMath::Clamp(Branch.Selected, 0, Children->Num() - 1);
    for (int32 n = 0; n < Children->Num(); ++n)
    {
        Index = (Direction >= 0) ? ((Index + 1) % Children->Num()) : ((Index + Children->Num() - 1) % Children->Num());
        if ((*Children)[Index].bEnabled)
        {
            Branch.Selected = Index;
            Branches.SetNum(Level + 1);
            RevealBranch(Level);
            return true;
        }
    }
    return false;
}

void FNGonPieMenu::OpenRootBranch(int32 RootIndex)
{
    if (!Items.IsValidIndex(RootIndex) || !Items[RootIndex].bEnabled || Items[RootIndex].Children.IsEmpty())
    {
        Branches.Reset();
        return;
    }
    if (!Branches.IsEmpty() && Branches[0].ParentPath.Num() == 1 && Branches[0].ParentPath[0] == RootIndex)
    {
        return;
    }
    const float Step = PieTau / static_cast<float>(Items.Num());
    const float Angle = StartAngle + Step * static_cast<float>(RootIndex);
    BranchDirection = (FMath::Cos(Angle) < -0.15f) ? -1 : 1;

    FBranchLevel Root;
    Root.ParentPath = { RootIndex };
    Root.Selected = 0;
    if (!Items[RootIndex].Children[0].bEnabled)
    {
        for (int32 i = 0; i < Items[RootIndex].Children.Num(); ++i)
        {
            if (Items[RootIndex].Children[i].bEnabled) { Root.Selected = i; break; }
        }
    }
    Branches.Reset();
    Branches.Add(MoveTemp(Root));
}

void FNGonPieMenu::OpenSubBranch(int32 Level)
{
    if (!Branches.IsValidIndex(Level)) return;
    const TArray<int32> Path = SelectedPath(Level);
    const FNGonPieItem* Sel = FindItem(Path);
    Branches.SetNum(Level + 1);
    if (!Sel || !Sel->bEnabled || Sel->Children.IsEmpty()) return;

    FBranchLevel Next;
    Next.ParentPath = Path;
    Next.Selected = 0;
    if (!Sel->Children[0].bEnabled)
    {
        for (int32 i = 0; i < Sel->Children.Num(); ++i)
        {
            if (Sel->Children[i].bEnabled) { Next.Selected = i; break; }
        }
    }
    Branches.Add(MoveTemp(Next));
}

void FNGonPieMenu::UpdateHighlight(const FVector2D& Point)
{
    TOptional<int32> Index = Sector(Point);
    if (Index.IsSet() && (!Items.IsValidIndex(Index.GetValue()) || !Items[Index.GetValue()].bEnabled))
    {
        Index.Reset();
    }
    if (Index != Highlighted)
    {
        Highlighted = Index;
        if (Highlighted.IsSet()) OpenRootBranch(Highlighted.GetValue());
        else Branches.Reset();
    }
    else if (Highlighted.IsSet())
    {
        OpenRootBranch(Highlighted.GetValue());
    }
}

TOptional<int32> FNGonPieMenu::BranchAt(const FVector2D& Point) const
{
    for (int32 i = Branches.Num() - 1; i >= 0; --i)
    {
        if (IsInsideBox(Branches[i].Bounds, Point)) return i;
    }
    return TOptional<int32>();
}

TOptional<int32> FNGonPieMenu::BranchRowAt(int32 Level, const FVector2D& Point) const
{
    if (!Branches.IsValidIndex(Level) || !IsInsideBox(Branches[Level].Bounds, Point)) return TOptional<int32>();
    const FBranchLevel& Branch = Branches[Level];
    const TArray<FNGonPieItem>* Children = FindChildren(Branch.ParentPath);
    if (!Children) return TOptional<int32>();
    const int32 Local = FMath::Max(0, FMath::FloorToInt((Point.Y - Branch.Bounds.Min.Y) / BranchRowHeight));
    const int32 Index = Branch.First + Local;
    return Children->IsValidIndex(Index) ? TOptional<int32>(Index) : TOptional<int32>();
}

void FNGonPieMenu::ActivateItem(const FNGonPieItem& Item)
{
    if (!Item.bEnabled || !Item.Children.IsEmpty() || Item.Command.IsEmpty()) return;
    TFunction<void(const FString&)> Callback = MoveTemp(OnChoose);
    const FString Cmd = Item.Command;
    Close();
    if (Callback)
    {
        Callback(Cmd);
    }
}

void FNGonPieMenu::OnMouseMove(const FVector2D& ViewLogicalSize, const FVector2D& LogicalPos)
{
    if (!bOpen) return;
    Arrange(ViewLogicalSize);
    if (const TOptional<int32> Level = BranchAt(LogicalPos); Level.IsSet())
    {
        if (const TOptional<int32> Row = BranchRowAt(Level.GetValue(), LogicalPos); Row.IsSet())
        {
            FBranchLevel& Branch = Branches[Level.GetValue()];
            const TArray<FNGonPieItem>* Children = FindChildren(Branch.ParentPath);
            if (Children && Children->IsValidIndex(Row.GetValue()) && (*Children)[Row.GetValue()].bEnabled)
            {
                Branch.Selected = Row.GetValue();
                Branches.SetNum(Level.GetValue() + 1);
                OpenSubBranch(Level.GetValue());
                Arrange(ViewLogicalSize);
            }
        }
        return;
    }
    UpdateHighlight(LogicalPos);
    Arrange(ViewLogicalSize);
}

bool FNGonPieMenu::OnMouseDown(const FVector2D& ViewLogicalSize, const FVector2D& LogicalPos)
{
    if (!bOpen) return false;
    OnMouseMove(ViewLogicalSize, LogicalPos);
    return true;
}

bool FNGonPieMenu::OnMouseUp(const FVector2D& ViewLogicalSize, const FVector2D& LogicalPos)
{
    if (!bOpen) return false;
    Arrange(ViewLogicalSize);

    if (const TOptional<int32> Level = BranchAt(LogicalPos); Level.IsSet())
    {
        if (const TOptional<int32> Row = BranchRowAt(Level.GetValue(), LogicalPos); Row.IsSet())
        {
            FBranchLevel& Branch = Branches[Level.GetValue()];
            const TArray<FNGonPieItem>* Children = FindChildren(Branch.ParentPath);
            if (Children && Children->IsValidIndex(Row.GetValue()) && (*Children)[Row.GetValue()].bEnabled)
            {
                Branch.Selected = Row.GetValue();
                Branches.SetNum(Level.GetValue() + 1);
                const FNGonPieItem& Item = (*Children)[Row.GetValue()];
                if (Item.Children.IsEmpty()) ActivateItem(Item);
                else { OpenSubBranch(Level.GetValue()); Arrange(ViewLogicalSize); }
            }
        }
        return true;
    }

    UpdateHighlight(LogicalPos);
    Arrange(ViewLogicalSize);
    if (Highlighted.IsSet() && Items.IsValidIndex(Highlighted.GetValue()))
    {
        const FNGonPieItem& Root = Items[Highlighted.GetValue()];
        if (Root.Children.IsEmpty()) ActivateItem(Root);
        else OpenRootBranch(Highlighted.GetValue());
    }
    else
    {
        Close();
    }
    return true;
}

bool FNGonPieMenu::OnMouseWheel(const FVector2D& ViewLogicalSize, const FVector2D& LogicalPos, float WheelDelta)
{
    if (!bOpen) return false;
    Arrange(ViewLogicalSize);
    if (const TOptional<int32> Level = BranchAt(LogicalPos); Level.IsSet())
    {
        FBranchLevel& Branch = Branches[Level.GetValue()];
        const TArray<FNGonPieItem>* Children = FindChildren(Branch.ParentPath);
        if (Children)
        {
            const int32 MaxFirst = FMath::Max(0, Children->Num() - Branch.Visible);
            Branch.First = FMath::Clamp(Branch.First - FMath::RoundToInt(WheelDelta * 2.0f), 0, MaxFirst);
        }
    }
    return true;
}

bool FNGonPieMenu::OnKeyDown(const FVector2D& ViewLogicalSize, const FKey& Key, bool bIsRepeat)
{
    if (!bOpen) return false;
    if (Key == EKeys::Escape) { Close(); return true; }
    if (bIsRepeat && Key == EKeys::SpaceBar) return true;
    Arrange(ViewLogicalSize);

    if (!Branches.IsEmpty())
    {
        const int32 Level = Branches.Num() - 1;
        FBranchLevel& Branch = Branches[Level];
        const TArray<FNGonPieItem>* Children = FindChildren(Branch.ParentPath);
        if (!Children || Children->IsEmpty()) return true;

        if (Key == EKeys::Down) { StepBranch(Level, 1); Arrange(ViewLogicalSize); return true; }
        if (Key == EKeys::Up) { StepBranch(Level, -1); Arrange(ViewLogicalSize); return true; }
        if (Key == EKeys::Right) { OpenSubBranch(Level); Arrange(ViewLogicalSize); return true; }
        if (Key == EKeys::Left)
        {
            if (Branches.Num() > 1) Branches.Pop(); else Branches.Reset();
            return true;
        }
        if (Key == EKeys::Enter || Key == EKeys::SpaceBar)
        {
            const FNGonPieItem& Sel = (*Children)[Branch.Selected];
            if (Sel.bEnabled)
            {
                if (Sel.Children.IsEmpty()) ActivateItem(Sel);
                else { OpenSubBranch(Level); Arrange(ViewLogicalSize); }
            }
            return true;
        }
        return true;
    }

    if (Key == EKeys::Right || Key == EKeys::Down)
    {
        Highlighted = StepEnabled(Highlighted.Get(Items.Num() - 1), 1);
        if (Highlighted.IsSet()) OpenRootBranch(Highlighted.GetValue());
        Arrange(ViewLogicalSize);
        return true;
    }
    if (Key == EKeys::Left || Key == EKeys::Up)
    {
        Highlighted = StepEnabled(Highlighted.Get(0), -1);
        if (Highlighted.IsSet()) OpenRootBranch(Highlighted.GetValue());
        Arrange(ViewLogicalSize);
        return true;
    }
    if (Key == EKeys::Enter || Key == EKeys::SpaceBar)
    {
        if (Highlighted.IsSet() && Items.IsValidIndex(Highlighted.GetValue()))
        {
            const FNGonPieItem& Sel = Items[Highlighted.GetValue()];
            if (Sel.Children.IsEmpty()) ActivateItem(Sel);
            else { OpenRootBranch(Highlighted.GetValue()); Arrange(ViewLogicalSize); }
        }
        return true;
    }
    return true;
}

void FNGonPieMenu::Draw(FCanvas& Canvas, const FVector2D& ViewLogicalSize) const
{
    if (!bOpen || Items.IsEmpty()) return;

    FPieSdfTextures& Tex = GetPieTextures();
    const FSlateFontInfo TagFont = FCoreStyle::GetDefaultFontStyle("Bold", 9);
    const FSlateFontInfo LabelFont = FCoreStyle::GetDefaultFontStyle("Bold", 10);
    const FSlateFontInfo MenuFont = FCoreStyle::GetDefaultFontStyle("Regular", 10);
    const FSlateFontInfo MenuBoldFont = FCoreStyle::GetDefaultFontStyle("Bold", 10);

    const FLinearColor Accent(0.26f, 0.84f, 1.0f, 1.0f);
    const FLinearColor AccentSoft(0.20f, 0.76f, 1.0f, 0.36f);
    const FLinearColor NodeBg(0.11f, 0.13f, 0.17f, 0.92f);
    const FLinearColor NodeActiveBg(0.12f, 0.30f, 0.46f, 0.96f);
    const FLinearColor NodeDisabledBg(0.07f, 0.08f, 0.10f, 0.78f);
    const FLinearColor HubBg(0.09f, 0.105f, 0.14f, 0.94f);
    const FLinearColor PanelBg(0.075f, 0.085f, 0.11f, 0.96f);
    const FLinearColor BorderCol(0.32f, 0.38f, 0.48f, 0.65f);
    const FLinearColor TrackRingCol(0.36f, 0.46f, 0.62f, 0.28f);
    const FLinearColor TextCol(0.92f, 0.95f, 0.99f, 1.0f);
    const FLinearColor TextActiveCol(1.0f, 1.0f, 1.0f, 1.0f);
    const FLinearColor MutedCol(0.50f, 0.56f, 0.64f, 0.85f);
    const FLinearColor ShadowCol(0.0f, 0.0f, 0.0f, 0.48f);

    const float Step = PieTau / static_cast<float>(Items.Num());
    const float HalfBtn = ButtonDiameter * 0.5f;
    const float HubR = DeadZoneRadius * 0.68f;

    // 1. Soft dark backdrop aura + silky anti-aliased radial orbit ring
    DrawGlowSdf(Canvas, Center, VisualRadius + HalfBtn + 26.0f, FLinearColor(0.01f, 0.015f, 0.025f, 0.46f));
    DrawRingSdf(Canvas, Center, VisualRadius, TrackRingCol, false);

    // 2. Active directional beam from hub edge to highlighted node edge
    if (Highlighted.IsSet() && Items.IsValidIndex(Highlighted.GetValue()))
    {
        const float SelAngle = StartAngle + Step * static_cast<float>(Highlighted.GetValue());
        const FVector2D Dir(FMath::Cos(SelAngle), FMath::Sin(SelAngle));
        const FVector2D BeamStart = Center + Dir * (HubR + 1.0f);
        const FVector2D BeamEnd = Center + Dir * (VisualRadius - HalfBtn - 2.0f);

        FCanvasLineItem GlowBeam(BeamStart, BeamEnd);
        GlowBeam.SetColor(FLinearColor(0.24f, 0.82f, 1.0f, 0.28f));
        GlowBeam.LineThickness = 4.5f;
        Canvas.DrawItem(GlowBeam);

        FCanvasLineItem CoreBeam(BeamStart, BeamEnd);
        CoreBeam.SetColor(FLinearColor(0.45f, 0.92f, 1.0f, 0.92f));
        CoreBeam.LineThickness = 1.6f;
        Canvas.DrawItem(CoreBeam);
    }

    // 3. Radial Root Nodes (SDF anti-aliased discs, soft drop shadows, luminous hover halo)
    for (int32 i = 0; i < Items.Num(); ++i)
    {
        const float Angle = StartAngle + Step * static_cast<float>(i);
        const FVector2D Dir(FMath::Cos(Angle), FMath::Sin(Angle));
        const FVector2D Pos = Center + Dir * VisualRadius;
        const bool bSel = Highlighted.IsSet() && Highlighted.GetValue() == i;
        const FNGonPieItem& Item = Items[i];
        const float NodeR = bSel ? (HalfBtn + 2.5f) : HalfBtn;

        // Soft offset drop shadow
        DrawGlowSdf(Canvas, Pos + FVector2D(0.0, 3.0), NodeR + 11.0f, ShadowCol);

        // Luminous outer glow halo on hovered node
        if (bSel && Item.bEnabled)
        {
            DrawGlowSdf(Canvas, Pos, NodeR + 14.0f, AccentSoft);
        }

        // Smooth glass disc fill + anti-aliased ring border
        const FLinearColor FillCol = !Item.bEnabled ? NodeDisabledBg : (bSel ? NodeActiveBg : NodeBg);
        DrawCircleSdf(Canvas, Pos, NodeR, FillCol);
        DrawRingSdf(Canvas, Pos, NodeR, (bSel && Item.bEnabled) ? Accent : BorderCol, bSel);

        // Crisp TrueType tag label
        const FString Tag = Item.ShortTag.IsEmpty() ? Item.Label.Left(4) : Item.ShortTag;
        DrawCenteredSlateText(Canvas, TagFont, Pos, Tag, !Item.bEnabled ? MutedCol : (bSel ? TextActiveCol : TextCol));

        // Clean anti-aliased submenu indicator dot on outer edge
        if (!Item.Children.IsEmpty())
        {
            const FVector2D DotPos = Pos + Dir * (NodeR - 5.5f);
            DrawCircleSdf(Canvas, DotPos, 2.8f, Item.bEnabled ? Accent : MutedCol);
        }
    }

    // 4. Center Dead-Zone Hub
    DrawGlowSdf(Canvas, Center + FVector2D(0.0, 2.0), HubR + 8.0f, ShadowCol);
    DrawCircleSdf(Canvas, Center, HubR, HubBg);
    DrawRingSdf(Canvas, Center, HubR, Highlighted.IsSet() ? FLinearColor(0.26f, 0.84f, 1.0f, 0.72f) : BorderCol, false);
    if (Highlighted.IsSet())
    {
        DrawGlowSdf(Canvas, Center, 7.5f, AccentSoft);
    }
    DrawCircleSdf(Canvas, Center, Highlighted.IsSet() ? 3.2f : 2.4f, Highlighted.IsSet() ? Accent : MutedCol);

    // 5. Rounded Tooltip Pill positioned cleanly BELOW the entire pie (never overlapping buttons)
    const FString CenterLabel = (Highlighted.IsSet() && Items.IsValidIndex(Highlighted.GetValue()))
        ? Items[Highlighted.GetValue()].Label
        : TEXT("Move cursor to select");
    const FVector2D TextSz = MeasureSlateText(CenterLabel, LabelFont);
    const double PillW = FMath::Max(TextSz.X + 28.0, 96.0);
    const double PillH = FMath::Max(TextSz.Y + 12.0, 26.0);
    const double BelowY = Center.Y + VisualRadius + HalfBtn + 22.0;
    const double AboveY = Center.Y - VisualRadius - HalfBtn - 22.0;
    const double PillY = (BelowY + PillH * 0.5 <= ViewLogicalSize.Y - 6.0)
        ? BelowY
        : FMath::Max(PillH * 0.5 + 6.0, AboveY);
    const FVector2D PillCenter(
        FMath::Clamp(Center.X, PillW * 0.5 + 8.0, FMath::Max(PillW * 0.5 + 8.0, ViewLogicalSize.X - PillW * 0.5 - 8.0)),
        PillY);

    DrawGlowSdf(Canvas, PillCenter + FVector2D(0.0, 3.0), static_cast<float>(PillW * 0.55), FLinearColor(0.0f, 0.0f, 0.0f, 0.35f));
    DrawSpriteCentered(Canvas, Tex.PillFill.Get(), PillCenter, FVector2D(PillW, PillH), FLinearColor(0.06f, 0.075f, 0.10f, 0.94f));
    DrawSpriteCentered(Canvas, Tex.PillBorder.Get(), PillCenter, FVector2D(PillW, PillH),
        Highlighted.IsSet() ? FLinearColor(0.26f, 0.84f, 1.0f, 0.85f) : BorderCol);
    DrawCenteredSlateText(Canvas, LabelFont, PillCenter, CenterLabel, Highlighted.IsSet() ? TextActiveCol : MutedCol);

    // 6. Sleek Fly-Out Submenu Panels
    for (int32 Level = 0; Level < Branches.Num(); ++Level)
    {
        const FBranchLevel& Branch = Branches[Level];
        const TArray<FNGonPieItem>* Children = FindChildren(Branch.ParentPath);
        if (!Children || Children->IsEmpty()) continue;

        const FBox2D B = Branch.Bounds;
        // Soft multi-pass drop shadow + frosted panel + crisp border
        DrawFilledRect(Canvas, FBox2D(B.Min + FVector2D(-4.0, 4.0), B.Max + FVector2D(4.0, 8.0)), FLinearColor(0.0f, 0.0f, 0.0f, 0.22f));
        DrawFilledRect(Canvas, FBox2D(B.Min + FVector2D(-2.0, 2.0), B.Max + FVector2D(2.0, 4.0)), FLinearColor(0.0f, 0.0f, 0.0f, 0.32f));
        DrawFilledRect(Canvas, B, PanelBg);
        DrawRectOutline(Canvas, B, BorderCol, 1.0f);

        for (int32 Row = 0; Row < Branch.Visible; ++Row)
        {
            const int32 Idx = Branch.First + Row;
            if (!Children->IsValidIndex(Idx)) break;
            const FNGonPieItem& Item = (*Children)[Idx];

            const double RowTop = B.Min.Y + static_cast<double>(Row) * BranchRowHeight;
            const FBox2D RowRect(
                FVector2D(B.Min.X + 3.0, RowTop + 2.0),
                FVector2D(B.Max.X - 3.0, FMath::Min(B.Max.Y - 2.0, RowTop + BranchRowHeight - 2.0)));
            const bool bRowSel = (Idx == Branch.Selected);

            if (bRowSel)
            {
                DrawFilledRect(Canvas, RowRect, NodeActiveBg);
                // Crisp left cyan accent bar
                DrawFilledRect(Canvas, FBox2D(RowRect.Min, FVector2D(RowRect.Min.X + 2.5, RowRect.Max.Y)), Accent);
            }

            const FSlateFontInfo& RowFont = bRowSel ? MenuBoldFont : MenuFont;
            const FVector2D ItemSz = MeasureSlateText(Item.Label, RowFont);
            const FVector2D TextPt(
                FMath::RoundToDouble(B.Min.X + 12.0),
                FMath::RoundToDouble(RowTop + FMath::Max(0.0, (BranchRowHeight - ItemSz.Y) * 0.5)));
            FCanvasTextItem TItem(TextPt, FText::FromString(Item.Label), RowFont,
                !Item.bEnabled ? MutedCol : (bRowSel ? TextActiveCol : TextCol));
            TItem.EnableShadow(FLinearColor(0.0f, 0.0f, 0.0f, 0.72f), FVector2D(0.0f, 1.0f));
            Canvas.DrawItem(TItem);

            // Sub-branch chevron
            if (!Item.Children.IsEmpty())
            {
                const double Cx = B.Max.X - 11.0;
                const double Cy = RowTop + BranchRowHeight * 0.5;
                const FLinearColor ChevCol = !Item.bEnabled ? MutedCol : (bRowSel ? Accent : TextCol);
                FCanvasLineItem L1(FVector2D(Cx - 2.5, Cy - 3.5), FVector2D(Cx + 1.5, Cy));
                L1.SetColor(ChevCol); L1.LineThickness = 1.4f; Canvas.DrawItem(L1);
                FCanvasLineItem L2(FVector2D(Cx + 1.5, Cy), FVector2D(Cx - 2.5, Cy + 3.5));
                L2.SetColor(ChevCol); L2.LineThickness = 1.4f; Canvas.DrawItem(L2);
            }
        }
    }
}
// NGon Unified ABI Sync v5.8.2
