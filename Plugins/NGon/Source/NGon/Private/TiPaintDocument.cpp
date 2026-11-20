#include "TiPaintDocument.h"
#include "NGonUndoTree.h"

#include "IImageWrapper.h"
#include "IImageWrapperModule.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Modules/ModuleManager.h"

namespace
{
FORCEINLINE float Hash2DFloat01(int32 X, int32 Y, uint32 Seed)
{
    uint32 H = static_cast<uint32>(X) * 374761393u + static_cast<uint32>(Y) * 668265263u + Seed * 1442695041u;
    H = (H ^ (H >> 13)) * 1274126177u;
    H ^= (H >> 16);
    return static_cast<float>(H & 0x00FFFFFFu) / static_cast<float>(0x01000000u);
}

FORCEINLINE float ValueNoise2D(float X, float Y)
{
    const int32 Xi = FMath::FloorToInt(X);
    const int32 Yi = FMath::FloorToInt(Y);
    const float Fx = X - static_cast<float>(Xi);
    const float Fy = Y - static_cast<float>(Yi);
    const float Ux = Fx * Fx * (3.0f - 2.0f * Fx);
    const float Uy = Fy * Fy * (3.0f - 2.0f * Fy);
    const float N00 = Hash2DFloat01(Xi, Yi, 17u);
    const float N10 = Hash2DFloat01(Xi + 1, Yi, 17u);
    const float N01 = Hash2DFloat01(Xi, Yi + 1, 17u);
    const float N11 = Hash2DFloat01(Xi + 1, Yi + 1, 17u);
    return FMath::Lerp(FMath::Lerp(N00, N10, Ux), FMath::Lerp(N01, N11, Ux), Uy);
}

FORCEINLINE float ApplyBlendChannel(float B, float S, ETiPaintBlendMode Mode)
{
    switch (Mode)
    {
    case ETiPaintBlendMode::Multiply: return B * S;
    case ETiPaintBlendMode::Screen:   return 1.0f - (1.0f - B) * (1.0f - S);
    case ETiPaintBlendMode::Overlay:  return (B < 0.5f) ? (2.0f * B * S) : (1.0f - 2.0f * (1.0f - B) * (1.0f - S));
    case ETiPaintBlendMode::Add:      return FMath::Min(1.0f, B + S);
    case ETiPaintBlendMode::Darken:   return FMath::Min(B, S);
    case ETiPaintBlendMode::Lighten:  return FMath::Max(B, S);
    case ETiPaintBlendMode::Normal:
    default:                          return S;
    }
}

bool DecodeImageFileRGBA(const FString& Filename, TArray<FColor>& OutPixels, int32& OutW, int32& OutH, FText& OutError)
{
    TArray<uint8> Compressed;
    if (!FFileHelper::LoadFileToArray(Compressed, *Filename) || Compressed.IsEmpty())
    {
        OutError = FText::FromString(FString::Printf(TEXT("Could not read image file: %s"), *Filename));
        return false;
    }

    IImageWrapperModule& Module = FModuleManager::LoadModuleChecked<IImageWrapperModule>(TEXT("ImageWrapper"));
    const EImageFormat Format = Module.DetectImageFormat(Compressed.GetData(), Compressed.Num());
    if (Format == EImageFormat::Invalid)
    {
        OutError = FText::FromString(TEXT("Unsupported image format (use PNG, JPEG, or BMP)."));
        return false;
    }

    const TSharedPtr<IImageWrapper> Wrapper = Module.CreateImageWrapper(Format);
    if (!Wrapper.IsValid() || !Wrapper->SetCompressed(Compressed.GetData(), Compressed.Num()))
    {
        OutError = FText::FromString(TEXT("Failed to decode image header."));
        return false;
    }

    TArray<uint8> RawBGRA;
    if (!Wrapper->GetRaw(ERGBFormat::BGRA, 8, RawBGRA))
    {
        OutError = FText::FromString(TEXT("Failed to decode image pixels."));
        return false;
    }

    OutW = Wrapper->GetWidth();
    OutH = Wrapper->GetHeight();
    const int64 Count = static_cast<int64>(OutW) * OutH;
    if (OutW <= 0 || OutH <= 0 || RawBGRA.Num() < Count * 4)
    {
        OutError = FText::FromString(TEXT("Decoded image buffer size mismatch."));
        return false;
    }

    OutPixels.SetNumUninitialized(static_cast<int32>(Count));
    FMemory::Memcpy(OutPixels.GetData(), RawBGRA.GetData(), Count * sizeof(FColor));
    OutError = FText::GetEmpty();
    return true;
}

FColor SampleBilinear(const TArray<FColor>& Pixels, int32 W, int32 H, float U, float V, bool bTile)
{
    if (W <= 0 || H <= 0 || Pixels.Num() < W * H)
    {
        return FColor::Transparent;
    }
    if (bTile)
    {
        U = U - FMath::FloorToFloat(U);
        V = V - FMath::FloorToFloat(V);
    }
    else
    {
        if (U < 0.0f || U > 1.0f || V < 0.0f || V > 1.0f)
        {
            return FColor::Transparent;
        }
    }
    const float Fx = FMath::Clamp(U, 0.0f, 1.0f) * static_cast<float>(W - 1);
    const float Fy = (1.0f - FMath::Clamp(V, 0.0f, 1.0f)) * static_cast<float>(H - 1);
    const int32 X0 = FMath::Clamp(FMath::FloorToInt(Fx), 0, W - 1);
    const int32 Y0 = FMath::Clamp(FMath::FloorToInt(Fy), 0, H - 1);
    const int32 X1 = FMath::Min(W - 1, X0 + 1);
    const int32 Y1 = FMath::Min(H - 1, Y0 + 1);
    const float Tx = Fx - static_cast<float>(X0);
    const float Ty = Fy - static_cast<float>(Y0);

    const FColor C00 = Pixels[Y0 * W + X0];
    const FColor C10 = Pixels[Y0 * W + X1];
    const FColor C01 = Pixels[Y1 * W + X0];
    const FColor C11 = Pixels[Y1 * W + X1];

    auto Bilerp = [Tx, Ty](uint8 A, uint8 B, uint8 C, uint8 D) -> uint8
    {
        const float Top = FMath::Lerp(static_cast<float>(A), static_cast<float>(B), Tx);
        const float Bot = FMath::Lerp(static_cast<float>(C), static_cast<float>(D), Tx);
        return static_cast<uint8>(FMath::Clamp(FMath::RoundToInt(FMath::Lerp(Top, Bot, Ty)), 0, 255));
    };

    return FColor(
        Bilerp(C00.R, C10.R, C01.R, C11.R),
        Bilerp(C00.G, C10.G, C01.G, C11.G),
        Bilerp(C00.B, C10.B, C01.B, C11.B),
        Bilerp(C00.A, C10.A, C01.A, C11.A));
}
} // namespace

FTiPaintDocument::FTiPaintDocument(int32 InWidth, int32 InHeight)
    : SizeX(FMath::Clamp(InWidth, 1, 4096))
    , SizeY(FMath::Clamp(InHeight, 1, 4096))
{
    FTiPaintLayer& Layer = PaintLayers.AddDefaulted_GetRef();
    Layer.Name = TEXT("Paint Layer");
    Layer.Pixels.Init(FColor::Transparent, SizeX * SizeY);
    CompositePixels.Init(FColor::Transparent, SizeX * SizeY);
    MarkFullDirty();
    NotifyChanged();
}

void FTiPaintDocument::SetMeshTriangles(const TArray<FTiPaintUVTriangle>& InTriangles)
{
    Triangles = InTriangles;
    RebuildSubMeshMask();
}

void FTiPaintDocument::SetSubMeshes(const TArray<FTiPaintSubMeshInfo>& InSubMeshes)
{
    SubMeshes = InSubMeshes;
    ActiveSubMeshFilter = (SubMeshes.Num() > 1) ? 0 : INDEX_NONE;
    RebuildSubMeshMask();
    NotifyChanged();
}

void FTiPaintDocument::SetActiveSubMeshFilter(int32 SubMeshIndex)
{
    const int32 Clamped = SubMeshes.IsValidIndex(SubMeshIndex) ? SubMeshIndex : INDEX_NONE;
    if (ActiveSubMeshFilter == Clamped) return;
    ActiveSubMeshFilter = Clamped;
    RebuildSubMeshMask();
    NotifyChanged();
}

void FTiPaintDocument::SetSubMeshMaskEnabled(bool bEnabled)
{
    if (bSubMeshMaskEnabled == bEnabled) return;
    bSubMeshMaskEnabled = bEnabled;
    RebuildSubMeshMask();
    NotifyChanged();
}

void FTiPaintDocument::RebuildSubMeshMask()
{
    bSubMeshMaskActive = false;
    SubMeshPixelMask.Reset();
    if (!bSubMeshMaskEnabled || ActiveSubMeshFilter == INDEX_NONE || Triangles.IsEmpty() || SizeX <= 0 || SizeY <= 0)
    {
        return;
    }

    SubMeshPixelMask.Init(0, SizeX * SizeY);
    const float WMinus1 = static_cast<float>(FMath::Max(1, SizeX - 1));
    const float HMinus1 = static_cast<float>(FMath::Max(1, SizeY - 1));
    int32 RasterizedTris = 0;

    for (const FTiPaintUVTriangle& Tri : Triangles)
    {
        if (Tri.SubMeshIndex != ActiveSubMeshFilter) continue;
        ++RasterizedTris;

        const FVector2f P0(Tri.UV[0].X * WMinus1, (1.0f - Tri.UV[0].Y) * HMinus1);
        const FVector2f P1(Tri.UV[1].X * WMinus1, (1.0f - Tri.UV[1].Y) * HMinus1);
        const FVector2f P2(Tri.UV[2].X * WMinus1, (1.0f - Tri.UV[2].Y) * HMinus1);

        const int32 MinX = FMath::Clamp(FMath::FloorToInt(FMath::Min3(P0.X, P1.X, P2.X)), 0, SizeX - 1);
        const int32 MaxX = FMath::Clamp(FMath::CeilToInt(FMath::Max3(P0.X, P1.X, P2.X)), 0, SizeX - 1);
        const int32 MinY = FMath::Clamp(FMath::FloorToInt(FMath::Min3(P0.Y, P1.Y, P2.Y)), 0, SizeY - 1);
        const int32 MaxY = FMath::Clamp(FMath::CeilToInt(FMath::Max3(P0.Y, P1.Y, P2.Y)), 0, SizeY - 1);

        const float Area = (P1.X - P0.X) * (P2.Y - P0.Y) - (P1.Y - P0.Y) * (P2.X - P0.X);
        if (FMath::Abs(Area) <= 1e-6f) continue;
        const float InvArea = 1.0f / Area;

        for (int32 Py = MinY; Py <= MaxY; ++Py)
        {
            const float FY = static_cast<float>(Py);
            const int32 Row = Py * SizeX;
            for (int32 Px = MinX; Px <= MaxX; ++Px)
            {
                const float FX = static_cast<float>(Px);
                const float W0 = ((P1.X - FX) * (P2.Y - FY) - (P1.Y - FY) * (P2.X - FX)) * InvArea;
                const float W1 = ((P2.X - FX) * (P0.Y - FY) - (P2.Y - FY) * (P0.X - FX)) * InvArea;
                const float W2 = 1.0f - W0 - W1;
                if (W0 >= -0.005f && W1 >= -0.005f && W2 >= -0.005f)
                {
                    SubMeshPixelMask[Row + Px] = 255;
                }
            }
        }
    }

    if (RasterizedTris == 0)
    {
        SubMeshPixelMask.Reset();
        return;
    }

    // 2-pixel seam dilation so UV island borders paint cleanly without jagged edges
    TArray<uint8> Dilated = SubMeshPixelMask;
    for (int32 Pass = 0; Pass < 2; ++Pass)
    {
        for (int32 Y = 0; Y < SizeY; ++Y)
        {
            const int32 Row = Y * SizeX;
            for (int32 X = 0; X < SizeX; ++X)
            {
                if (SubMeshPixelMask[Row + X] != 0) continue;
                const bool bNbr =
                    (X > 0 && SubMeshPixelMask[Row + X - 1] != 0) ||
                    (X + 1 < SizeX && SubMeshPixelMask[Row + X + 1] != 0) ||
                    (Y > 0 && SubMeshPixelMask[(Y - 1) * SizeX + X] != 0) ||
                    (Y + 1 < SizeY && SubMeshPixelMask[(Y + 1) * SizeX + X] != 0);
                if (bNbr) Dilated[Row + X] = 255;
            }
        }
        SubMeshPixelMask = Dilated;
    }

    bSubMeshMaskActive = true;
}

void FTiPaintDocument::MarkDirtyRect(int32 MinX, int32 MinY, int32 MaxX, int32 MaxY)
{
    MinX = FMath::Clamp(MinX, 0, SizeX - 1);
    MinY = FMath::Clamp(MinY, 0, SizeY - 1);
    MaxX = FMath::Clamp(MaxX, 0, SizeX - 1);
    MaxY = FMath::Clamp(MaxY, 0, SizeY - 1);
    if (MinX > MaxX || MinY > MaxY) return;

    if (!bCompositeDirty)
    {
        DirtyRect = FIntRect(MinX, MinY, MaxX, MaxY);
        bCompositeDirty = true;
    }
    else if (!bFullCompositeDirty)
    {
        DirtyRect.Min.X = FMath::Min(DirtyRect.Min.X, MinX);
        DirtyRect.Min.Y = FMath::Min(DirtyRect.Min.Y, MinY);
        DirtyRect.Max.X = FMath::Max(DirtyRect.Max.X, MaxX);
        DirtyRect.Max.Y = FMath::Max(DirtyRect.Max.Y, MaxY);
    }
}

void FTiPaintDocument::MarkFullDirty()
{
    bCompositeDirty = true;
    bFullCompositeDirty = true;
    DirtyRect = FIntRect(0, 0, SizeX - 1, SizeY - 1);
}

void FTiPaintDocument::NotifyChanged()
{
    ++DocumentRevision;
    if (Changed)
    {
        Changed();
    }
}

FColor FTiPaintDocument::BlendPixel(const FColor& Base, const FColor& Paint, float Alpha)
{
    return BlendPixelWithMode(Base, Paint, Alpha, ETiPaintBlendMode::Normal);
}

FColor FTiPaintDocument::BlendPixelWithMode(const FColor& Base, const FColor& Src, float Alpha, ETiPaintBlendMode Mode)
{
    if (Alpha <= KINDA_SMALL_NUMBER)
    {
        return Base;
    }
    const float BaseA = Base.A / 255.0f;
    const float OutA = Alpha + BaseA * (1.0f - Alpha);
    if (OutA <= KINDA_SMALL_NUMBER)
    {
        return FColor::Transparent;
    }

    const float BR = Base.R / 255.0f;
    const float BG = Base.G / 255.0f;
    const float BB = Base.B / 255.0f;
    const float SR = Src.R / 255.0f;
    const float SG = Src.G / 255.0f;
    const float SB = Src.B / 255.0f;

    // Blend RGB only where base has coverage; over transparent base use Src directly
    const float MixR = BaseA > KINDA_SMALL_NUMBER ? ApplyBlendChannel(BR, SR, Mode) : SR;
    const float MixG = BaseA > KINDA_SMALL_NUMBER ? ApplyBlendChannel(BG, SG, Mode) : SG;
    const float MixB = BaseA > KINDA_SMALL_NUMBER ? ApplyBlendChannel(BB, SB, Mode) : SB;

    // Porter-Duff source-over using blended color for overlap region
    const float EffR = FMath::Lerp(SR, MixR, BaseA);
    const float EffG = FMath::Lerp(SG, MixG, BaseA);
    const float EffB = FMath::Lerp(SB, MixB, BaseA);

    const float InvOutA = 1.0f / OutA;
    const float BaseW = BaseA * (1.0f - Alpha);
    const float OutR = (EffR * Alpha + BR * BaseW) * InvOutA;
    const float OutG = (EffG * Alpha + BG * BaseW) * InvOutA;
    const float OutB = (EffB * Alpha + BB * BaseW) * InvOutA;

    return FColor(
        static_cast<uint8>(FMath::Clamp(FMath::RoundToInt(OutR * 255.0f), 0, 255)),
        static_cast<uint8>(FMath::Clamp(FMath::RoundToInt(OutG * 255.0f), 0, 255)),
        static_cast<uint8>(FMath::Clamp(FMath::RoundToInt(OutB * 255.0f), 0, 255)),
        static_cast<uint8>(FMath::Clamp(FMath::RoundToInt(OutA * 255.0f), 0, 255)));
}

const TArray<FColor>& FTiPaintDocument::Composite() const
{
    const int32 TotalPixels = SizeX * SizeY;
    if (CompositePixels.Num() != TotalPixels)
    {
        CompositePixels.Init(FColor::Transparent, TotalPixels);
        bCompositeDirty = true;
        bFullCompositeDirty = true;
    }
    if (!bCompositeDirty)
    {
        return CompositePixels;
    }

    const int32 MinX = bFullCompositeDirty ? 0 : FMath::Clamp(DirtyRect.Min.X, 0, SizeX - 1);
    const int32 MinY = bFullCompositeDirty ? 0 : FMath::Clamp(DirtyRect.Min.Y, 0, SizeY - 1);
    const int32 MaxX = bFullCompositeDirty ? (SizeX - 1) : FMath::Clamp(DirtyRect.Max.X, 0, SizeX - 1);
    const int32 MaxY = bFullCompositeDirty ? (SizeY - 1) : FMath::Clamp(DirtyRect.Max.Y, 0, SizeY - 1);

    // Clear only the dirty sub-rect
    for (int32 Y = MinY; Y <= MaxY; ++Y)
    {
        FColor* Row = CompositePixels.GetData() + Y * SizeX + MinX;
        FMemory::Memzero(Row, static_cast<SIZE_T>(MaxX - MinX + 1) * sizeof(FColor));
    }

    // Blend visible layers only inside the dirty sub-rect
    for (const FTiPaintLayer& Layer : PaintLayers)
    {
        if (!Layer.bVisible || Layer.Pixels.Num() != TotalPixels) continue;
        const float LayerAlpha = FMath::Clamp(Layer.Opacity, 0.0f, 1.0f);
        if (LayerAlpha <= KINDA_SMALL_NUMBER) continue;

        const ETiPaintBlendMode Mode = Layer.BlendMode;
        const FColor* SrcData = Layer.Pixels.GetData();
        FColor* DstData = CompositePixels.GetData();

        for (int32 Y = MinY; Y <= MaxY; ++Y)
        {
            const int32 RowOffset = Y * SizeX;
            for (int32 X = MinX; X <= MaxX; ++X)
            {
                const int32 Idx = RowOffset + X;
                const FColor Source = SrcData[Idx];
                if (Source.A == 0) continue;
                const float Alpha = (Source.A / 255.0f) * LayerAlpha;
                DstData[Idx] = BlendPixelWithMode(DstData[Idx], Source, Alpha, Mode);
            }
        }
    }

    bCompositeDirty = false;
    bFullCompositeDirty = false;
    return CompositePixels;
}

void FTiPaintDocument::SaveLayerUndoSnapshot(int32 LayerIndex)
{
    if (!PaintLayers.IsValidIndex(LayerIndex)) return;
    FUndoEntry Entry;
    Entry.bFullStack = false;
    Entry.LayerIndex = LayerIndex;
    Entry.SingleLayer = PaintLayers[LayerIndex];
    Entry.ActiveIndex = ActiveLayerIndex;
    Entry.DocWidth = SizeX;
    Entry.DocHeight = SizeY;
    UndoStack.Add(MoveTemp(Entry));
    if (UndoStack.Num() > 20) UndoStack.RemoveAt(0);
    RedoStack.Reset();
}

void FTiPaintDocument::SaveFullUndoSnapshot()
{
    FUndoEntry Entry;
    Entry.bFullStack = true;
    Entry.AllLayers = PaintLayers;
    Entry.ActiveIndex = ActiveLayerIndex;
    Entry.DocWidth = SizeX;
    Entry.DocHeight = SizeY;
    UndoStack.Add(MoveTemp(Entry));
    if (UndoStack.Num() > 20) UndoStack.RemoveAt(0);
    RedoStack.Reset();
}

void FTiPaintDocument::RestoreUndoEntry(const FUndoEntry& Entry)
{
    if (Entry.bFullStack)
    {
        SizeX = Entry.DocWidth;
        SizeY = Entry.DocHeight;
        PaintLayers = Entry.AllLayers;
        ActiveLayerIndex = FMath::Clamp(Entry.ActiveIndex, 0, FMath::Max(0, PaintLayers.Num() - 1));
    }
    else if (PaintLayers.IsValidIndex(Entry.LayerIndex) && Entry.DocWidth == SizeX && Entry.DocHeight == SizeY)
    {
        PaintLayers[Entry.LayerIndex] = Entry.SingleLayer;
        ActiveLayerIndex = FMath::Clamp(Entry.ActiveIndex, 0, FMath::Max(0, PaintLayers.Num() - 1));
    }
    MarkFullDirty();
    NotifyChanged();
}

void FTiPaintDocument::Undo()
{
    if (bStroking) CancelStroke();
    if (UndoStack.IsEmpty()) return;

    const FUndoEntry Target = UndoStack.Pop();
    FUndoEntry RedoEntry;
    RedoEntry.bFullStack = Target.bFullStack;
    RedoEntry.LayerIndex = Target.LayerIndex;
    RedoEntry.ActiveIndex = ActiveLayerIndex;
    RedoEntry.DocWidth = SizeX;
    RedoEntry.DocHeight = SizeY;
    if (Target.bFullStack)
    {
        RedoEntry.AllLayers = PaintLayers;
    }
    else if (PaintLayers.IsValidIndex(Target.LayerIndex))
    {
        RedoEntry.SingleLayer = PaintLayers[Target.LayerIndex];
    }
    RedoStack.Add(MoveTemp(RedoEntry));
    RestoreUndoEntry(Target);
}

void FTiPaintDocument::Redo()
{
    if (bStroking) CancelStroke();
    if (RedoStack.IsEmpty()) return;

    const FUndoEntry Target = RedoStack.Pop();
    FUndoEntry UndoEntry;
    UndoEntry.bFullStack = Target.bFullStack;
    UndoEntry.LayerIndex = Target.LayerIndex;
    UndoEntry.ActiveIndex = ActiveLayerIndex;
    UndoEntry.DocWidth = SizeX;
    UndoEntry.DocHeight = SizeY;
    if (Target.bFullStack)
    {
        UndoEntry.AllLayers = PaintLayers;
    }
    else if (PaintLayers.IsValidIndex(Target.LayerIndex))
    {
        UndoEntry.SingleLayer = PaintLayers[Target.LayerIndex];
    }
    UndoStack.Add(MoveTemp(UndoEntry));
    RestoreUndoEntry(Target);
}

FColor FTiPaintDocument::SampleCompositeColor(float U, float V) const
{
    const TArray<FColor>& Comp = Composite();
    const int32 X = FMath::Clamp(FMath::RoundToInt(FMath::Clamp(U, 0.0f, 1.0f) * (SizeX - 1)), 0, SizeX - 1);
    const int32 Y = FMath::Clamp(FMath::RoundToInt((1.0f - FMath::Clamp(V, 0.0f, 1.0f)) * (SizeY - 1)), 0, SizeY - 1);
    return Comp[Y * SizeX + X];
}

float FTiPaintDocument::EvaluateStencilMask(float U, float V, FColor* OutStencilColor) const
{
    if (!ActiveStencil.bEnabled || ActiveStencil.Type == ETiPaintStencilType::None)
    {
        return 1.0f;
    }

    // Apply offset, rotation, and scale around (0.5, 0.5)
    const float Rad = FMath::DegreesToRadians(ActiveStencil.RotationDeg);
    const float CosR = FMath::Cos(Rad);
    const float SinR = FMath::Sin(Rad);
    const float Dx = U - 0.5f - ActiveStencil.Offset.X;
    const float Dy = V - 0.5f - ActiveStencil.Offset.Y;
    const float Rx = (Dx * CosR - Dy * SinR) * FMath::Max(0.01f, ActiveStencil.Scale) + 0.5f;
    const float Ry = (Dx * SinR + Dy * CosR) * FMath::Max(0.01f, ActiveStencil.Scale) + 0.5f;

    float Mask = 1.0f;
    if (ActiveStencil.Type == ETiPaintStencilType::Image)
    {
        if (ActiveStencil.ImageWidth > 0 && ActiveStencil.ImageHeight > 0 && !ActiveStencil.ImagePixels.IsEmpty())
        {
            const FColor S = SampleBilinear(ActiveStencil.ImagePixels, ActiveStencil.ImageWidth, ActiveStencil.ImageHeight, Rx, Ry, ActiveStencil.bTile);
            if (OutStencilColor) *OutStencilColor = S;
            const float Luma = (0.299f * S.R + 0.587f * S.G + 0.114f * S.B) / 255.0f;
            Mask = ActiveStencil.bUseAsColorStamp ? (S.A / 255.0f) : (Luma * (S.A / 255.0f));
        }
    }
    else if (ActiveStencil.Type == ETiPaintStencilType::Checker)
    {
        const int32 Cx = FMath::FloorToInt(Rx * 2.0f);
        const int32 Cy = FMath::FloorToInt(Ry * 2.0f);
        Mask = ((Cx + Cy) & 1) == 0 ? 1.0f : 0.0f;
    }
    else if (ActiveStencil.Type == ETiPaintStencilType::Grid)
    {
        const float Fx = FMath::Frac(Rx);
        const float Fy = FMath::Frac(Ry);
        Mask = (Fx < 0.12f || Fx > 0.88f || Fy < 0.12f || Fy > 0.88f) ? 1.0f : 0.0f;
    }
    else if (ActiveStencil.Type == ETiPaintStencilType::Dots)
    {
        const float Fx = FMath::Frac(Rx) - 0.5f;
        const float Fy = FMath::Frac(Ry) - 0.5f;
        const float D = FMath::Sqrt(Fx * Fx + Fy * Fy);
        Mask = FMath::Clamp((0.32f - D) / 0.08f, 0.0f, 1.0f);
    }
    else if (ActiveStencil.Type == ETiPaintStencilType::Stripes)
    {
        const float Fx = FMath::Frac(Rx);
        Mask = Fx < 0.5f ? 1.0f : 0.0f;
    }
    else if (ActiveStencil.Type == ETiPaintStencilType::RadialVignette)
    {
        const float Fx = Rx - 0.5f;
        const float Fy = Ry - 0.5f;
        const float D = FMath::Sqrt(Fx * Fx + Fy * Fy) * 2.0f;
        Mask = FMath::Clamp(1.0f - D, 0.0f, 1.0f);
    }
    else if (ActiveStencil.Type == ETiPaintStencilType::Noise)
    {
        const float N1 = ValueNoise2D(Rx * 4.0f, Ry * 4.0f);
        const float N2 = ValueNoise2D(Rx * 8.0f + 13.7f, Ry * 8.0f - 5.3f);
        Mask = FMath::Clamp(N1 * 0.65f + N2 * 0.35f, 0.0f, 1.0f);
    }

    if (ActiveStencil.bInvert)
    {
        Mask = 1.0f - Mask;
    }
    return FMath::Clamp(Mask * FMath::Clamp(ActiveStencil.Opacity, 0.0f, 1.0f), 0.0f, 1.0f);
}

void FTiPaintDocument::BeginStroke(float U, float V)
{
    if (bStroking || !PaintLayers.IsValidIndex(ActiveLayerIndex)) return;

    const float CU = FMath::Clamp(U, 0.0f, 1.0f);
    const float CV = FMath::Clamp(V, 0.0f, 1.0f);

    // Eyedropper picks color from composite without mutating or pushing undo
    if (ActiveBrush.Mode == ETiPaintMode::Eyedropper)
    {
        const FColor Picked = SampleCompositeColor(CU, CV);
        if (Picked.A > 0)
        {
            ActiveBrush.Color = FColor(Picked.R, Picked.G, Picked.B, 255);
            NotifyChanged();
        }
        return;
    }

    if (PaintLayers[ActiveLayerIndex].bLocked) return;

    // Flood fill executes in one click
    if (ActiveBrush.Mode == ETiPaintMode::Fill)
    {
        FloodFill(CU, CV, 0.08f);
        return;
    }

    ActiveStrokeBackup.bFullStack = false;
    ActiveStrokeBackup.LayerIndex = ActiveLayerIndex;
    ActiveStrokeBackup.SingleLayer = PaintLayers[ActiveLayerIndex];
    ActiveStrokeBackup.ActiveIndex = ActiveLayerIndex;
    ActiveStrokeBackup.DocWidth = SizeX;
    ActiveStrokeBackup.DocHeight = SizeY;

    SaveLayerUndoSnapshot(ActiveLayerIndex);
    bStroking = true;
    LastUV = FVector2f(CU, CV);
    StrokeRemainderPx = 0.0f;
    StrokeStampCounter = 0;
    bHasSmudgeCarry = false;

    if (ActiveBrush.Mode == ETiPaintMode::Smudge)
    {
        const int32 X = FMath::Clamp(FMath::RoundToInt(CU * (SizeX - 1)), 0, SizeX - 1);
        const int32 Y = FMath::Clamp(FMath::RoundToInt((1.0f - CV) * (SizeY - 1)), 0, SizeY - 1);
        SmudgeCarryColor = PaintLayers[ActiveLayerIndex].Pixels[Y * SizeX + X];
        bHasSmudgeCarry = (SmudgeCarryColor.A > 0);
    }

    StampWithSymmetry(CU, CV);
    NotifyChanged();
}

void FTiPaintDocument::AddStrokePoint(float U, float V)
{
    if (!bStroking) return;
    const FVector2f Current(FMath::Clamp(U, 0.0f, 1.0f), FMath::Clamp(V, 0.0f, 1.0f));
    const float SegPx = FVector2f::Distance(LastUV, Current) * static_cast<float>(FMath::Max(SizeX, SizeY));
    const float StepPx = FMath::Max(1.0f, ActiveBrush.Radius * FMath::Clamp(ActiveBrush.Spacing, 0.05f, 2.0f));

    if (SegPx + StrokeRemainderPx < StepPx)
    {
        StrokeRemainderPx += SegPx;
        LastUV = Current;
        return;
    }

    float Dist = StepPx - StrokeRemainderPx;
    int32 Safety = 0;
    while (Dist <= SegPx && Safety < 512)
    {
        const float T = (SegPx > KINDA_SMALL_NUMBER) ? FMath::Clamp(Dist / SegPx, 0.0f, 1.0f) : 1.0f;
        StampWithSymmetry(FMath::Lerp(LastUV.X, Current.X, T), FMath::Lerp(LastUV.Y, Current.Y, T));
        Dist += StepPx;
        ++Safety;
    }
    StrokeRemainderPx = FMath::Max(0.0f, SegPx - (Dist - StepPx));
    LastUV = Current;
    NotifyChanged();
}

void FTiPaintDocument::AddStrokeDiscontinuousPoint(float U, float V)
{
    if (!bStroking) return;
    const FVector2f Current(FMath::Clamp(U, 0.0f, 1.0f), FMath::Clamp(V, 0.0f, 1.0f));
    LastUV = Current;
    StrokeRemainderPx = 0.0f;
    StampWithSymmetry(Current.X, Current.Y);
    NotifyChanged();
}

void FTiPaintDocument::NotifyUndoTreeEdit(const FString& Label, bool bIsProjection)
{
    const TSharedPtr<FNGonUndoSession> Session = UndoSession.Pin();
    if (!Session.IsValid() || Session->IsApplyingHistory()) return;
    TWeakPtr<FTiPaintDocument> WeakDoc = AsShared();
    const SIZE_T ApproxBytes = static_cast<SIZE_T>(SizeX) * static_cast<SIZE_T>(SizeY) * sizeof(FColor);
    TSharedPtr<INGonUndoPayload> Payload = MakeShared<FNGonLambdaUndoPayload>(
        [WeakDoc]() -> bool { if (auto D = WeakDoc.Pin()) { D->Undo(); return true; } return false; },
        [WeakDoc]() -> bool { if (auto D = WeakDoc.Pin()) { D->Redo(); return true; } return false; },
        ApproxBytes);
    Session->PushEdit(
        ENGonUndoContextId::Paint,
        Label,
        bIsProjection ? ENGonUndoEditKind::Projection : ENGonUndoEditKind::Value,
        MoveTemp(Payload),
        ApproxBytes);
}

void FTiPaintDocument::EndStroke()
{
    if (!bStroking) return;
    bStroking = false;
    bHasSmudgeCarry = false;
    NotifyUndoTreeEdit(TEXT("Brush Stroke"), false);
}

void FTiPaintDocument::CancelStroke()
{
    if (!bStroking) return;
    bStroking = false;
    bHasSmudgeCarry = false;
    RestoreUndoEntry(ActiveStrokeBackup);
    if (!UndoStack.IsEmpty()) UndoStack.Pop();
}

void FTiPaintDocument::StampWithSymmetry(float U, float V)
{
    StampSingleUV(U, V);
    switch (ActiveBrush.Symmetry)
    {
    case ETiPaintSymmetryMode::MirrorU:
        StampSingleUV(1.0f - U, V);
        break;
    case ETiPaintSymmetryMode::MirrorV:
        StampSingleUV(U, 1.0f - V);
        break;
    case ETiPaintSymmetryMode::Radial4:
        StampSingleUV(1.0f - U, V);
        StampSingleUV(U, 1.0f - V);
        StampSingleUV(1.0f - U, 1.0f - V);
        break;
    case ETiPaintSymmetryMode::None:
    default:
        break;
    }
}

void FTiPaintDocument::StampSingleUV(float U, float V)
{
    ++StrokeStampCounter;
    float OutU = U;
    float OutV = V;
    if (ActiveBrush.Scatter > KINDA_SMALL_NUMBER)
    {
        const float Angle = Hash2DFloat01(StrokeStampCounter, 11, 91u) * 2.0f * PI;
        const float Mag = Hash2DFloat01(StrokeStampCounter, 29, 91u) * ActiveBrush.Scatter * (ActiveBrush.Radius / static_cast<float>(FMath::Max(SizeX, SizeY)));
        OutU = FMath::Clamp(OutU + FMath::Cos(Angle) * Mag, 0.0f, 1.0f);
        OutV = FMath::Clamp(OutV + FMath::Sin(Angle) * Mag, 0.0f, 1.0f);
    }

    const int32 X = FMath::Clamp(FMath::RoundToInt(OutU * (SizeX - 1)), 0, SizeX - 1);
    const int32 Y = FMath::Clamp(FMath::RoundToInt((1.0f - OutV) * (SizeY - 1)), 0, SizeY - 1);
    StampAtPixel(X, Y, static_cast<float>(StrokeStampCounter));
}

float FTiPaintDocument::EvaluateBrushTip(float DX, float DY, float Radius, float Seed) const
{
    const float InvR = 1.0f / FMath::Max(Radius, 1.0f);
    float LX = DX * InvR;
    float LY = DY * InvR;
    if (FMath::Abs(ActiveBrush.RotationDeg) > 0.01f)
    {
        const float Rad = FMath::DegreesToRadians(ActiveBrush.RotationDeg);
        const float C = FMath::Cos(Rad);
        const float S = FMath::Sin(Rad);
        const float RX = LX * C - LY * S;
        const float RY = LX * S + LY * C;
        LX = RX;
        LY = RY;
    }

    const float Hard = FMath::Clamp(ActiveBrush.Hardness, 0.0f, 0.999f);
    switch (ActiveBrush.Tip)
    {
    case ETiPaintBrushTip::Square:
    {
        const float Dist = FMath::Max(FMath::Abs(LX), FMath::Abs(LY));
        if (Dist > 1.0f) return 0.0f;
        return Dist < Hard ? 1.0f : FMath::Clamp((1.0f - Dist) / FMath::Max(1.0f - Hard, 0.001f), 0.0f, 1.0f);
    }
    case ETiPaintBrushTip::SoftAirbrush:
    {
        const float DistSq = LX * LX + LY * LY;
        if (DistSq > 1.0f) return 0.0f;
        return FMath::Exp(-3.5f * DistSq) * (1.0f - DistSq);
    }
    case ETiPaintBrushTip::Chalk:
    {
        const float Dist = FMath::Sqrt(LX * LX + LY * LY);
        if (Dist > 1.0f) return 0.0f;
        const float Base = Dist < Hard ? 1.0f : FMath::Clamp((1.0f - Dist) / FMath::Max(1.0f - Hard, 0.001f), 0.0f, 1.0f);
        const float Grain = Hash2DFloat01(FMath::RoundToInt(DX * 2.0f), FMath::RoundToInt(DY * 2.0f), static_cast<uint32>(Seed));
        return Base * (Grain > 0.25f ? 1.0f : 0.2f);
    }
    case ETiPaintBrushTip::Noise:
    {
        const float Dist = FMath::Sqrt(LX * LX + LY * LY);
        if (Dist > 1.0f) return 0.0f;
        const float Base = Dist < Hard ? 1.0f : FMath::Clamp((1.0f - Dist) / FMath::Max(1.0f - Hard, 0.001f), 0.0f, 1.0f);
        const float N = ValueNoise2D(LX * 5.0f + Seed * 0.37f, LY * 5.0f - Seed * 0.19f);
        return Base * N;
    }
    case ETiPaintBrushTip::Round:
    default:
    {
        const float DistSq = LX * LX + LY * LY;
        if (DistSq > 1.0f) return 0.0f;
        const float Dist = FMath::Sqrt(DistSq);
        if (Dist <= Hard) return 1.0f;
        const float T = FMath::Clamp((1.0f - Dist) / FMath::Max(1.0f - Hard, 0.001f), 0.0f, 1.0f);
        return T * T * (3.0f - 2.0f * T); // smoothstep falloff
    }
    }
}

void FTiPaintDocument::StampAtPixel(int32 X, int32 Y, float StrokeRandSeed)
{
    if (!PaintLayers.IsValidIndex(ActiveLayerIndex)) return;
    FTiPaintLayer& Layer = PaintLayers[ActiveLayerIndex];
    if (Layer.bLocked) return;

    float EffRadius = FMath::Max(1.0f, ActiveBrush.Radius);
    if (ActiveBrush.JitterSize > KINDA_SMALL_NUMBER)
    {
        const float J = Hash2DFloat01(X, Y, static_cast<uint32>(StrokeRandSeed) + 7u);
        EffRadius *= FMath::Lerp(1.0f - ActiveBrush.JitterSize * 0.75f, 1.0f + ActiveBrush.JitterSize * 0.25f, J);
    }

    float EffOpacity = FMath::Clamp(ActiveBrush.Opacity, 0.0f, 1.0f) * FMath::Clamp(ActiveBrush.Flow, 0.01f, 1.0f);
    if (ActiveBrush.JitterOpacity > KINDA_SMALL_NUMBER)
    {
        const float J = Hash2DFloat01(X, Y, static_cast<uint32>(StrokeRandSeed) + 31u);
        EffOpacity *= FMath::Lerp(1.0f - ActiveBrush.JitterOpacity * 0.8f, 1.0f, J);
    }

    const int32 IntRadius = FMath::CeilToInt(EffRadius);
    const int32 MinX = FMath::Max(0, X - IntRadius);
    const int32 MaxX = FMath::Min(SizeX - 1, X + IntRadius);
    const int32 MinY = FMath::Max(0, Y - IntRadius);
    const int32 MaxY = FMath::Min(SizeY - 1, Y + IntRadius);

    const float InvW = (SizeX > 1) ? (1.0f / static_cast<float>(SizeX - 1)) : 1.0f;
    const float InvH = (SizeY > 1) ? (1.0f / static_cast<float>(SizeY - 1)) : 1.0f;

    // Blur mode needs a local copy of the stamp window to avoid directional smearing
    TArray<FColor> BlurSrc;
    const int32 BoxW = MaxX - MinX + 1;
    const int32 BoxH = MaxY - MinY + 1;
    if (ActiveBrush.Mode == ETiPaintMode::Blur)
    {
        BlurSrc.SetNumUninitialized(BoxW * BoxH);
        for (int32 Py = MinY; Py <= MaxY; ++Py)
        {
            FMemory::Memcpy(BlurSrc.GetData() + (Py - MinY) * BoxW, Layer.Pixels.GetData() + Py * SizeX + MinX, BoxW * sizeof(FColor));
        }
    }

    // Update Smudge carry color toward current pixel before stamping
    if (ActiveBrush.Mode == ETiPaintMode::Smudge)
    {
        const FColor Cur = Layer.Pixels[Y * SizeX + X];
        if (!bHasSmudgeCarry && Cur.A > 0)
        {
            SmudgeCarryColor = Cur;
            bHasSmudgeCarry = true;
        }
    }

    for (int32 Py = MinY; Py <= MaxY; ++Py)
    {
        const float V = 1.0f - static_cast<float>(Py) * InvH;
        const int32 RowOffset = Py * SizeX;
        for (int32 Px = MinX; Px <= MaxX; ++Px)
        {
            if (bSubMeshMaskActive && SubMeshPixelMask[RowOffset + Px] == 0) continue;
            const float TipMask = EvaluateBrushTip(static_cast<float>(Px - X), static_cast<float>(Py - Y), EffRadius, StrokeRandSeed);
            if (TipMask <= KINDA_SMALL_NUMBER) continue;

            const float U = static_cast<float>(Px) * InvW;
            FColor StencilColor = ActiveBrush.Color;
            const float StencilMask = EvaluateStencilMask(U, V, &StencilColor);
            if (StencilMask <= KINDA_SMALL_NUMBER) continue;

            const float Alpha = TipMask * EffOpacity * StencilMask;
            if (Alpha <= KINDA_SMALL_NUMBER) continue;

            FColor& Pixel = Layer.Pixels[RowOffset + Px];
            if (Layer.bAlphaLock && Pixel.A == 0) continue;

            if (ActiveBrush.Mode == ETiPaintMode::Erase)
            {
                if (!Layer.bAlphaLock)
                {
                    Pixel.A = static_cast<uint8>(FMath::Clamp(FMath::RoundToInt(Pixel.A * (1.0f - Alpha)), 0, 255));
                }
            }
            else if (ActiveBrush.Mode == ETiPaintMode::Blur)
            {
                int32 SumR = 0, SumG = 0, SumB = 0, SumA = 0, Count = 0;
                for (int32 Oy = -1; Oy <= 1; ++Oy)
                {
                    const int32 Sy = FMath::Clamp((Py - MinY) + Oy, 0, BoxH - 1);
                    for (int32 Ox = -1; Ox <= 1; ++Ox)
                    {
                        const int32 Sx = FMath::Clamp((Px - MinX) + Ox, 0, BoxW - 1);
                        const FColor S = BlurSrc[Sy * BoxW + Sx];
                        SumR += S.R; SumG += S.G; SumB += S.B; SumA += S.A; ++Count;
                    }
                }
                if (Count > 0)
                {
                    const FColor Avg(
                        static_cast<uint8>(SumR / Count),
                        static_cast<uint8>(SumG / Count),
                        static_cast<uint8>(SumB / Count),
                        static_cast<uint8>(SumA / Count));
                    const uint8 SavedA = Pixel.A;
                    Pixel = FColor(
                        static_cast<uint8>(FMath::Lerp(static_cast<float>(Pixel.R), static_cast<float>(Avg.R), Alpha)),
                        static_cast<uint8>(FMath::Lerp(static_cast<float>(Pixel.G), static_cast<float>(Avg.G), Alpha)),
                        static_cast<uint8>(FMath::Lerp(static_cast<float>(Pixel.B), static_cast<float>(Avg.B), Alpha)),
                        Layer.bAlphaLock ? SavedA : static_cast<uint8>(FMath::Lerp(static_cast<float>(Pixel.A), static_cast<float>(Avg.A), Alpha)));
                }
            }
            else if (ActiveBrush.Mode == ETiPaintMode::Smudge)
            {
                if (bHasSmudgeCarry)
                {
                    const uint8 SavedA = Pixel.A;
                    Pixel = BlendPixel(Pixel, SmudgeCarryColor, Alpha * 0.65f);
                    if (Layer.bAlphaLock) Pixel.A = SavedA;
                }
            }
            else
            {
                const FColor PaintCol = (ActiveStencil.bEnabled && ActiveStencil.Type == ETiPaintStencilType::Image && ActiveStencil.bUseAsColorStamp)
                    ? StencilColor
                    : ActiveBrush.Color;
                const uint8 SavedA = Pixel.A;
                Pixel = BlendPixel(Pixel, PaintCol, Alpha * (PaintCol.A / 255.0f));
                if (Layer.bAlphaLock) Pixel.A = SavedA;
            }
        }
    }

    if (ActiveBrush.Mode == ETiPaintMode::Smudge && bHasSmudgeCarry)
    {
        const FColor Cur = Layer.Pixels[Y * SizeX + X];
        if (Cur.A > 0)
        {
            SmudgeCarryColor = BlendPixel(SmudgeCarryColor, Cur, 0.25f);
        }
    }

    MarkDirtyRect(MinX, MinY, MaxX, MaxY);
}

bool FTiPaintDocument::DrawStrokePolyline(const TArray<FVector2f>& UVPoints)
{
    if (UVPoints.IsEmpty() || !PaintLayers.IsValidIndex(ActiveLayerIndex) || PaintLayers[ActiveLayerIndex].bLocked)
    {
        return false;
    }
    BeginStroke(UVPoints[0].X, UVPoints[0].Y);
    for (int32 i = 1; i < UVPoints.Num(); ++i)
    {
        AddStrokePoint(UVPoints[i].X, UVPoints[i].Y);
    }
    EndStroke();
    return true;
}

bool FTiPaintDocument::DrawLine(float U0, float V0, float U1, float V1)
{
    return DrawStrokePolyline({ FVector2f(U0, V0), FVector2f(U1, V1) });
}

bool FTiPaintDocument::DrawRect(float U0, float V0, float U1, float V1, bool bFilled)
{
    if (!PaintLayers.IsValidIndex(ActiveLayerIndex) || PaintLayers[ActiveLayerIndex].bLocked) return false;
    const float MinU = FMath::Clamp(FMath::Min(U0, U1), 0.0f, 1.0f);
    const float MaxU = FMath::Clamp(FMath::Max(U0, U1), 0.0f, 1.0f);
    const float MinV = FMath::Clamp(FMath::Min(V0, V1), 0.0f, 1.0f);
    const float MaxV = FMath::Clamp(FMath::Max(V0, V1), 0.0f, 1.0f);

    if (!bFilled)
    {
        return DrawStrokePolyline({
            FVector2f(MinU, MinV), FVector2f(MaxU, MinV),
            FVector2f(MaxU, MaxV), FVector2f(MinU, MaxV), FVector2f(MinU, MinV)
        });
    }

    SaveLayerUndoSnapshot(ActiveLayerIndex);
    FTiPaintLayer& Layer = PaintLayers[ActiveLayerIndex];
    const int32 X0 = FMath::Clamp(FMath::RoundToInt(MinU * (SizeX - 1)), 0, SizeX - 1);
    const int32 X1 = FMath::Clamp(FMath::RoundToInt(MaxU * (SizeX - 1)), 0, SizeX - 1);
    const int32 Y0 = FMath::Clamp(FMath::RoundToInt((1.0f - MaxV) * (SizeY - 1)), 0, SizeY - 1);
    const int32 Y1 = FMath::Clamp(FMath::RoundToInt((1.0f - MinV) * (SizeY - 1)), 0, SizeY - 1);
    const float Alpha = FMath::Clamp(ActiveBrush.Opacity, 0.0f, 1.0f) * (ActiveBrush.Color.A / 255.0f);

    for (int32 Y = Y0; Y <= Y1; ++Y)
    {
        for (int32 X = X0; X <= X1; ++X)
        {
            FColor& P = Layer.Pixels[Y * SizeX + X];
            if (Layer.bAlphaLock && P.A == 0) continue;
            const uint8 SavedA = P.A;
            P = BlendPixel(P, ActiveBrush.Color, Alpha);
            if (Layer.bAlphaLock) P.A = SavedA;
        }
    }
    MarkDirtyRect(X0, Y0, X1, Y1);
    NotifyChanged();
    return true;
}

bool FTiPaintDocument::DrawCircle(float CenterU, float CenterV, float RadiusUV, bool bFilled)
{
    if (!PaintLayers.IsValidIndex(ActiveLayerIndex) || PaintLayers[ActiveLayerIndex].bLocked || RadiusUV <= 0.0f)
    {
        return false;
    }
    if (!bFilled)
    {
        TArray<FVector2f> Ring;
        constexpr int32 Steps = 48;
        Ring.Reserve(Steps + 1);
        for (int32 i = 0; i <= Steps; ++i)
        {
            const float A = (2.0f * PI * i) / static_cast<float>(Steps);
            Ring.Add(FVector2f(CenterU + FMath::Cos(A) * RadiusUV, CenterV + FMath::Sin(A) * RadiusUV));
        }
        return DrawStrokePolyline(Ring);
    }

    SaveLayerUndoSnapshot(ActiveLayerIndex);
    FTiPaintLayer& Layer = PaintLayers[ActiveLayerIndex];
    const float Cx = FMath::Clamp(CenterU, 0.0f, 1.0f) * (SizeX - 1);
    const float Cy = (1.0f - FMath::Clamp(CenterV, 0.0f, 1.0f)) * (SizeY - 1);
    const float Rx = RadiusUV * SizeX;
    const float Ry = RadiusUV * SizeY;
    const int32 MinX = FMath::Max(0, FMath::FloorToInt(Cx - Rx));
    const int32 MaxX = FMath::Min(SizeX - 1, FMath::CeilToInt(Cx + Rx));
    const int32 MinY = FMath::Max(0, FMath::FloorToInt(Cy - Ry));
    const int32 MaxY = FMath::Min(SizeY - 1, FMath::CeilToInt(Cy + Ry));
    const float Alpha = FMath::Clamp(ActiveBrush.Opacity, 0.0f, 1.0f) * (ActiveBrush.Color.A / 255.0f);

    for (int32 Y = MinY; Y <= MaxY; ++Y)
    {
        const float Dy = (Y - Cy) / FMath::Max(Ry, 1.0f);
        for (int32 X = MinX; X <= MaxX; ++X)
        {
            const float Dx = (X - Cx) / FMath::Max(Rx, 1.0f);
            if (Dx * Dx + Dy * Dy > 1.0f) continue;
            FColor& P = Layer.Pixels[Y * SizeX + X];
            if (Layer.bAlphaLock && P.A == 0) continue;
            const uint8 SavedA = P.A;
            P = BlendPixel(P, ActiveBrush.Color, Alpha);
            if (Layer.bAlphaLock) P.A = SavedA;
        }
    }
    MarkDirtyRect(MinX, MinY, MaxX, MaxY);
    NotifyChanged();
    return true;
}

bool FTiPaintDocument::FloodFill(float U, float V, float Tolerance)
{
    if (!PaintLayers.IsValidIndex(ActiveLayerIndex) || PaintLayers[ActiveLayerIndex].bLocked) return false;
    FTiPaintLayer& Layer = PaintLayers[ActiveLayerIndex];
    const int32 StartX = FMath::Clamp(FMath::RoundToInt(FMath::Clamp(U, 0.0f, 1.0f) * (SizeX - 1)), 0, SizeX - 1);
    const int32 StartY = FMath::Clamp(FMath::RoundToInt((1.0f - FMath::Clamp(V, 0.0f, 1.0f)) * (SizeY - 1)), 0, SizeY - 1);

    if (bSubMeshMaskActive && SubMeshPixelMask[StartY * SizeX + StartX] == 0) return false;
    const FColor Target = Layer.Pixels[StartY * SizeX + StartX];
    if (Layer.bAlphaLock && Target.A == 0) return false;

    const int32 Tol = FMath::Clamp(FMath::RoundToInt(Tolerance * 255.0f), 0, 255);
    auto Matches = [Target, Tol](const FColor& C) -> bool
    {
        return FMath::Abs(int32(C.R) - int32(Target.R)) <= Tol &&
               FMath::Abs(int32(C.G) - int32(Target.G)) <= Tol &&
               FMath::Abs(int32(C.B) - int32(Target.B)) <= Tol &&
               FMath::Abs(int32(C.A) - int32(Target.A)) <= Tol;
    };

    SaveLayerUndoSnapshot(ActiveLayerIndex);
    TArray<uint8> Visited;
    Visited.Init(0, SizeX * SizeY);
    TArray<FIntPoint> Queue;
    Queue.Reserve(1024);
    Queue.Add(FIntPoint(StartX, StartY));
    Visited[StartY * SizeX + StartX] = 1;

    const float Alpha = FMath::Clamp(ActiveBrush.Opacity, 0.0f, 1.0f) * (ActiveBrush.Color.A / 255.0f);
    int32 MinX = StartX, MaxX = StartX, MinY = StartY, MaxY = StartY;

    while (!Queue.IsEmpty())
    {
        const FIntPoint Pt = Queue.Pop(EAllowShrinking::No);
        const int32 Idx = Pt.Y * SizeX + Pt.X;
        FColor& P = Layer.Pixels[Idx];
        const uint8 SavedA = P.A;
        P = BlendPixel(P, ActiveBrush.Color, Alpha);
        if (Layer.bAlphaLock) P.A = SavedA;

        MinX = FMath::Min(MinX, Pt.X); MaxX = FMath::Max(MaxX, Pt.X);
        MinY = FMath::Min(MinY, Pt.Y); MaxY = FMath::Max(MaxY, Pt.Y);

        const FIntPoint Nbrs[4] = {
            FIntPoint(Pt.X - 1, Pt.Y), FIntPoint(Pt.X + 1, Pt.Y),
            FIntPoint(Pt.X, Pt.Y - 1), FIntPoint(Pt.X, Pt.Y + 1)
        };
        for (const FIntPoint& N : Nbrs)
        {
            if (N.X < 0 || N.X >= SizeX || N.Y < 0 || N.Y >= SizeY) continue;
            const int32 NIdx = N.Y * SizeX + N.X;
            if (!Visited[NIdx] && (!bSubMeshMaskActive || SubMeshPixelMask[NIdx] != 0) && Matches(Layer.Pixels[NIdx]))
            {
                Visited[NIdx] = 1;
                Queue.Add(N);
            }
        }
    }

    MarkDirtyRect(MinX, MinY, MaxX, MaxY);
    NotifyChanged();
    return true;
}

void FTiPaintDocument::AddLayer(const FString& Name)
{
    SaveFullUndoSnapshot();
    FTiPaintLayer& Layer = PaintLayers.AddDefaulted_GetRef();
    Layer.Name = Name.IsEmpty() ? FString::Printf(TEXT("Paint Layer %d"), PaintLayers.Num()) : Name;
    Layer.Pixels.Init(FColor::Transparent, SizeX * SizeY);
    ActiveLayerIndex = PaintLayers.Num() - 1;
    MarkFullDirty();
    NotifyChanged();
}

void FTiPaintDocument::DuplicateActiveLayer()
{
    if (!PaintLayers.IsValidIndex(ActiveLayerIndex)) return;
    SaveFullUndoSnapshot();
    FTiPaintLayer Copy = PaintLayers[ActiveLayerIndex];
    Copy.Name += TEXT(" Copy");
    const int32 InsertIdx = ActiveLayerIndex + 1;
    PaintLayers.Insert(MoveTemp(Copy), InsertIdx);
    ActiveLayerIndex = InsertIdx;
    MarkFullDirty();
    NotifyChanged();
}

void FTiPaintDocument::RemoveActiveLayer()
{
    if (PaintLayers.Num() <= 1 || !PaintLayers.IsValidIndex(ActiveLayerIndex)) return;
    SaveFullUndoSnapshot();
    PaintLayers.RemoveAt(ActiveLayerIndex);
    ActiveLayerIndex = FMath::Clamp(ActiveLayerIndex, 0, PaintLayers.Num() - 1);
    MarkFullDirty();
    NotifyChanged();
}

bool FTiPaintDocument::MoveActiveLayer(int32 Delta)
{
    const int32 Target = ActiveLayerIndex + Delta;
    if (!PaintLayers.IsValidIndex(ActiveLayerIndex) || !PaintLayers.IsValidIndex(Target) || Target == ActiveLayerIndex)
    {
        return false;
    }
    SaveFullUndoSnapshot();
    PaintLayers.Swap(ActiveLayerIndex, Target);
    ActiveLayerIndex = Target;
    MarkFullDirty();
    NotifyChanged();
    return true;
}

bool FTiPaintDocument::MergeActiveLayerDown()
{
    if (ActiveLayerIndex <= 0 || !PaintLayers.IsValidIndex(ActiveLayerIndex)) return false;
    SaveFullUndoSnapshot();
    const FTiPaintLayer Top = PaintLayers[ActiveLayerIndex];
    FTiPaintLayer& Bottom = PaintLayers[ActiveLayerIndex - 1];
    if (Top.bVisible && Top.Pixels.Num() == Bottom.Pixels.Num())
    {
        const float TopAlpha = FMath::Clamp(Top.Opacity, 0.0f, 1.0f);
        for (int32 i = 0; i < Bottom.Pixels.Num(); ++i)
        {
            const FColor Src = Top.Pixels[i];
            if (Src.A == 0) continue;
            Bottom.Pixels[i] = BlendPixelWithMode(Bottom.Pixels[i], Src, (Src.A / 255.0f) * TopAlpha, Top.BlendMode);
        }
    }
    PaintLayers.RemoveAt(ActiveLayerIndex);
    ActiveLayerIndex = FMath::Clamp(ActiveLayerIndex - 1, 0, PaintLayers.Num() - 1);
    MarkFullDirty();
    NotifyChanged();
    return true;
}

void FTiPaintDocument::FlattenVisibleLayers()
{
    if (PaintLayers.Num() <= 1) return;
    const TArray<FColor> Flat = Composite();
    SaveFullUndoSnapshot();
    PaintLayers.Reset();
    FTiPaintLayer& Layer = PaintLayers.AddDefaulted_GetRef();
    Layer.Name = TEXT("Flattened");
    Layer.Pixels = Flat;
    ActiveLayerIndex = 0;
    MarkFullDirty();
    NotifyChanged();
}

void FTiPaintDocument::ToggleActiveVisibility()
{
    if (!PaintLayers.IsValidIndex(ActiveLayerIndex)) return;
    SaveFullUndoSnapshot();
    PaintLayers[ActiveLayerIndex].bVisible = !PaintLayers[ActiveLayerIndex].bVisible;
    MarkFullDirty();
    NotifyChanged();
}

void FTiPaintDocument::ToggleActiveLock()
{
    if (!PaintLayers.IsValidIndex(ActiveLayerIndex)) return;
    SaveFullUndoSnapshot();
    PaintLayers[ActiveLayerIndex].bLocked = !PaintLayers[ActiveLayerIndex].bLocked;
    NotifyChanged();
}

void FTiPaintDocument::ToggleActiveAlphaLock()
{
    if (!PaintLayers.IsValidIndex(ActiveLayerIndex)) return;
    SaveFullUndoSnapshot();
    PaintLayers[ActiveLayerIndex].bAlphaLock = !PaintLayers[ActiveLayerIndex].bAlphaLock;
    NotifyChanged();
}

void FTiPaintDocument::SetActiveLayer(int32 Index)
{
    if (PaintLayers.IsValidIndex(Index) && Index != ActiveLayerIndex)
    {
        ActiveLayerIndex = Index;
        NotifyChanged();
    }
}

void FTiPaintDocument::RenameActiveLayer(const FString& NewName)
{
    if (!PaintLayers.IsValidIndex(ActiveLayerIndex) || NewName.IsEmpty()) return;
    SaveFullUndoSnapshot();
    PaintLayers[ActiveLayerIndex].Name = NewName;
    NotifyChanged();
}

void FTiPaintDocument::SetActiveLayerOpacity(float NewOpacity)
{
    if (!PaintLayers.IsValidIndex(ActiveLayerIndex)) return;
    const float Clamped = FMath::Clamp(NewOpacity, 0.0f, 1.0f);
    if (FMath::IsNearlyEqual(PaintLayers[ActiveLayerIndex].Opacity, Clamped, 1e-4f)) return;
    PaintLayers[ActiveLayerIndex].Opacity = Clamped;
    MarkFullDirty();
    NotifyChanged();
}

void FTiPaintDocument::SetActiveLayerBlendMode(ETiPaintBlendMode NewMode)
{
    if (!PaintLayers.IsValidIndex(ActiveLayerIndex) || PaintLayers[ActiveLayerIndex].BlendMode == NewMode) return;
    SaveFullUndoSnapshot();
    PaintLayers[ActiveLayerIndex].BlendMode = NewMode;
    MarkFullDirty();
    NotifyChanged();
}

void FTiPaintDocument::FillActiveLayer(const FColor& Color)
{
    if (!PaintLayers.IsValidIndex(ActiveLayerIndex) || PaintLayers[ActiveLayerIndex].bLocked) return;
    SaveLayerUndoSnapshot(ActiveLayerIndex);
    FTiPaintLayer& Layer = PaintLayers[ActiveLayerIndex];
    if (Layer.bAlphaLock)
    {
        for (FColor& P : Layer.Pixels)
        {
            if (P.A > 0) P = FColor(Color.R, Color.G, Color.B, P.A);
        }
    }
    else
    {
        Layer.Pixels.Init(Color, SizeX * SizeY);
    }
    MarkFullDirty();
    NotifyChanged();
}

void FTiPaintDocument::Clear()
{
    if (!PaintLayers.IsValidIndex(ActiveLayerIndex) || PaintLayers[ActiveLayerIndex].bLocked) return;
    SaveLayerUndoSnapshot(ActiveLayerIndex);
    PaintLayers[ActiveLayerIndex].Pixels.Init(FColor::Transparent, SizeX * SizeY);
    MarkFullDirty();
    NotifyChanged();
}

bool FTiPaintDocument::Resize(int32 NewWidth, int32 NewHeight)
{
    NewWidth = FMath::Clamp(NewWidth, 16, 4096);
    NewHeight = FMath::Clamp(NewHeight, 16, 4096);
    if (NewWidth == SizeX && NewHeight == SizeY) return true;

    SaveFullUndoSnapshot();
    const int32 OldW = SizeX;
    const int32 OldH = SizeY;
    for (FTiPaintLayer& Layer : PaintLayers)
    {
        TArray<FColor> Resized;
        Resized.SetNumUninitialized(NewWidth * NewHeight);
        for (int32 Y = 0; Y < NewHeight; ++Y)
        {
            const float V = 1.0f - (static_cast<float>(Y) / FMath::Max(1.0f, static_cast<float>(NewHeight - 1)));
            for (int32 X = 0; X < NewWidth; ++X)
            {
                const float U = static_cast<float>(X) / FMath::Max(1.0f, static_cast<float>(NewWidth - 1));
                Resized[Y * NewWidth + X] = SampleBilinear(Layer.Pixels, OldW, OldH, U, V, false);
            }
        }
        Layer.Pixels = MoveTemp(Resized);
    }
    SizeX = NewWidth;
    SizeY = NewHeight;
    RebuildSubMeshMask();
    CompositePixels.Init(FColor::Transparent, SizeX * SizeY);
    MarkFullDirty();
    NotifyChanged();
    return true;
}

bool FTiPaintDocument::ExportPNG(const FString& Filename, FText& Error) const
{
    IImageWrapperModule& Module = FModuleManager::LoadModuleChecked<IImageWrapperModule>(TEXT("ImageWrapper"));
    const TSharedPtr<IImageWrapper> Wrapper = Module.CreateImageWrapper(EImageFormat::PNG);
    const TArray<FColor>& Comp = Composite();
    TArray<uint8> RGBA;
    RGBA.Reserve(Comp.Num() * 4);
    for (const FColor& Pixel : Comp)
    {
        RGBA.Add(Pixel.R);
        RGBA.Add(Pixel.G);
        RGBA.Add(Pixel.B);
        RGBA.Add(Pixel.A);
    }
    if (!Wrapper.IsValid() || !Wrapper->SetRaw(RGBA.GetData(), RGBA.Num(), SizeX, SizeY, ERGBFormat::RGBA, 8))
    {
        Error = FText::FromString(TEXT("PNG encoder rejected the image."));
        return false;
    }
    if (!FFileHelper::SaveArrayToFile(Wrapper->GetCompressed(), *Filename))
    {
        Error = FText::FromString(TEXT("Could not write the PNG file."));
        return false;
    }
    Error = FText::GetEmpty();
    return true;
}

bool FTiPaintDocument::ImportImageToNewLayer(const FString& Filename, FText& Error)
{
    TArray<FColor> SrcPixels;
    int32 SrcW = 0, SrcH = 0;
    if (!DecodeImageFileRGBA(Filename, SrcPixels, SrcW, SrcH, Error))
    {
        return false;
    }

    SaveFullUndoSnapshot();
    FTiPaintLayer& Layer = PaintLayers.AddDefaulted_GetRef();
    Layer.Name = FPaths::GetBaseFilename(Filename);
    Layer.Pixels.SetNumUninitialized(SizeX * SizeY);
    for (int32 Y = 0; Y < SizeY; ++Y)
    {
        const float V = 1.0f - (static_cast<float>(Y) / FMath::Max(1.0f, static_cast<float>(SizeY - 1)));
        for (int32 X = 0; X < SizeX; ++X)
        {
            const float U = static_cast<float>(X) / FMath::Max(1.0f, static_cast<float>(SizeX - 1));
            Layer.Pixels[Y * SizeX + X] = SampleBilinear(SrcPixels, SrcW, SrcH, U, V, false);
        }
    }
    ActiveLayerIndex = PaintLayers.Num() - 1;
    MarkFullDirty();
    NotifyChanged();
    return true;
}

bool FTiPaintDocument::LoadStencilImage(const FString& Filename, FText& Error)
{
    TArray<FColor> Pixels;
    int32 W = 0, H = 0;
    if (!DecodeImageFileRGBA(Filename, Pixels, W, H, Error))
    {
        return false;
    }
    ActiveStencil.ImagePath = Filename;
    ActiveStencil.ImageWidth = W;
    ActiveStencil.ImageHeight = H;
    ActiveStencil.ImagePixels = MoveTemp(Pixels);
    ActiveStencil.Type = ETiPaintStencilType::Image;
    ActiveStencil.bEnabled = true;
    NotifyChanged();
    return true;
}

bool FTiPaintDocument::ProjectOntoMesh(const FTiPaintProjectionSettings& Settings, FString& OutMessage)
{
    if (!PaintLayers.IsValidIndex(ActiveLayerIndex) || PaintLayers[ActiveLayerIndex].bLocked)
    {
        OutMessage = TEXT("Active layer is locked or invalid.");
        return false;
    }

    // Compute 3D bounding box of mesh triangles if 3D data is present
    FBox3f Bounds(ForceInit);
    bool bAny3D = false;
    for (const FTiPaintUVTriangle& Tri : Triangles)
    {
        if (!IsSubMeshVisible(Tri.SubMeshIndex)) continue;
        if (Tri.bHas3D)
        {
            bAny3D = true;
            Bounds += Tri.Pos[0];
            Bounds += Tri.Pos[1];
            Bounds += Tri.Pos[2];
        }
    }

    if (!bAny3D && Settings.Mode != ETiPaintProjectionMode::UV2D)
    {
        OutMessage = TEXT("Mesh has no 3D triangle data for 3D projection (load a mesh or use UV2D mode).");
        return false;
    }

    const FVector3f BoundsCenter = bAny3D ? Bounds.GetCenter() : FVector3f::ZeroVector;
    const FVector3f BoundsExtent = bAny3D ? Bounds.GetSize().ComponentMax(FVector3f(1e-3f)) : FVector3f(1.0f);
    const float InvMaxExtent = 1.0f / FMath::Max3(BoundsExtent.X, BoundsExtent.Y, BoundsExtent.Z);

    SaveLayerUndoSnapshot(ActiveLayerIndex);
    FTiPaintLayer& Layer = PaintLayers[ActiveLayerIndex];
    const float ProjScale = FMath::Max(0.01f, Settings.Scale);
    const float Opacity = FMath::Clamp(Settings.Opacity, 0.0f, 1.0f);

    auto SampleProjected = [&](const FVector3f& WorldP, const FVector3f& WorldN, const FVector2f& UV, FColor& OutCol, float& OutMask)
    {
        const FVector3f Rel = (WorldP - BoundsCenter) * InvMaxExtent * ProjScale + Settings.Offset;
        auto EvalAt = [&](float SU, float SV, FColor& Col) -> float
        {
            Col = Settings.TintColor;
            if (Settings.bUseStencil && ActiveStencil.Type != ETiPaintStencilType::None)
            {
                const bool bWasEnabled = ActiveStencil.bEnabled;
                ActiveStencil.bEnabled = true;
                const float M = EvaluateStencilMask(SU, SV, &Col);
                ActiveStencil.bEnabled = bWasEnabled;
                if (!(ActiveStencil.Type == ETiPaintStencilType::Image && ActiveStencil.bUseAsColorStamp))
                {
                    Col = Settings.TintColor;
                }
                return M;
            }
            return 1.0f;
        };

        switch (Settings.Mode)
        {
        case ETiPaintProjectionMode::PlanarX:
            OutMask = EvalAt(Rel.Y + 0.5f, Rel.Z + 0.5f, OutCol);
            break;
        case ETiPaintProjectionMode::PlanarY:
            OutMask = EvalAt(Rel.X + 0.5f, Rel.Z + 0.5f, OutCol);
            break;
        case ETiPaintProjectionMode::PlanarZ:
            OutMask = EvalAt(Rel.X + 0.5f, Rel.Y + 0.5f, OutCol);
            break;
        case ETiPaintProjectionMode::Cylindrical:
        {
            const float Angle = FMath::Atan2(Rel.Y, Rel.X) / (2.0f * PI) + 0.5f;
            OutMask = EvalAt(Angle, Rel.Z + 0.5f, OutCol);
            break;
        }
        case ETiPaintProjectionMode::Spherical:
        {
            const FVector3f Dir = Rel.GetSafeNormal();
            const float SU = FMath::Atan2(Dir.Y, Dir.X) / (2.0f * PI) + 0.5f;
            const float SV = FMath::Asin(FMath::Clamp(Dir.Z, -1.0f, 1.0f)) / PI + 0.5f;
            OutMask = EvalAt(SU, SV, OutCol);
            break;
        }
        case ETiPaintProjectionMode::Triplanar:
        {
            const float Sharp = FMath::Clamp(Settings.BlendSharpness, 1.0f, 16.0f);
            float WX = FMath::Pow(FMath::Abs(WorldN.X), Sharp);
            float WY = FMath::Pow(FMath::Abs(WorldN.Y), Sharp);
            float WZ = FMath::Pow(FMath::Abs(WorldN.Z), Sharp);
            const float WSum = FMath::Max(1e-5f, WX + WY + WZ);
            WX /= WSum; WY /= WSum; WZ /= WSum;

            FColor CX, CY, CZ;
            const float MX = EvalAt(Rel.Y + 0.5f, Rel.Z + 0.5f, CX);
            const float MY = EvalAt(Rel.X + 0.5f, Rel.Z + 0.5f, CY);
            const float MZ = EvalAt(Rel.X + 0.5f, Rel.Y + 0.5f, CZ);
            OutMask = MX * WX + MY * WY + MZ * WZ;
            OutCol = FColor(
                static_cast<uint8>(FMath::Clamp(FMath::RoundToInt(CX.R * WX + CY.R * WY + CZ.R * WZ), 0, 255)),
                static_cast<uint8>(FMath::Clamp(FMath::RoundToInt(CX.G * WX + CY.G * WY + CZ.G * WZ), 0, 255)),
                static_cast<uint8>(FMath::Clamp(FMath::RoundToInt(CX.B * WX + CY.B * WY + CZ.B * WZ), 0, 255)),
                255);
            break;
        }
        case ETiPaintProjectionMode::UV2D:
        default:
            OutMask = EvalAt(UV.X, UV.Y, OutCol);
            break;
        }
    };

    int32 PaintedPixels = 0;
    if (Settings.Mode == ETiPaintProjectionMode::UV2D || Triangles.IsEmpty())
    {
        for (int32 Y = 0; Y < SizeY; ++Y)
        {
            const float V = 1.0f - (static_cast<float>(Y) / FMath::Max(1.0f, static_cast<float>(SizeY - 1)));
            for (int32 X = 0; X < SizeX; ++X)
            {
                const float U = static_cast<float>(X) / FMath::Max(1.0f, static_cast<float>(SizeX - 1));
                FColor Col; float Mask = 0.0f;
                SampleProjected(FVector3f::ZeroVector, FVector3f::UpVector, FVector2f(U, V), Col, Mask);
                const float Alpha = Mask * Opacity * (Col.A / 255.0f);
                if (Alpha > KINDA_SMALL_NUMBER)
                {
                    FColor& P = Layer.Pixels[Y * SizeX + X];
                    if (Layer.bAlphaLock && P.A == 0) continue;
                    const uint8 SavedA = P.A;
                    P = BlendPixel(P, Col, Alpha);
                    if (Layer.bAlphaLock) P.A = SavedA;
                    ++PaintedPixels;
                }
            }
        }
    }
    else
    {
        // Rasterize every mesh triangle in UV space with barycentric 3D position & normal interpolation
        for (const FTiPaintUVTriangle& Tri : Triangles)
        {
            if (!Tri.bHas3D) continue;
            const FVector2f P0(Tri.UV[0].X * (SizeX - 1), (1.0f - Tri.UV[0].Y) * (SizeY - 1));
            const FVector2f P1(Tri.UV[1].X * (SizeX - 1), (1.0f - Tri.UV[1].Y) * (SizeY - 1));
            const FVector2f P2(Tri.UV[2].X * (SizeX - 1), (1.0f - Tri.UV[2].Y) * (SizeY - 1));

            const float Area = (P1.X - P0.X) * (P2.Y - P0.Y) - (P2.X - P0.X) * (P1.Y - P0.Y);
            if (FMath::Abs(Area) <= 1e-4f) continue;
            const float InvArea = 1.0f / Area;

            const int32 MinX = FMath::Clamp(FMath::FloorToInt(FMath::Min3(P0.X, P1.X, P2.X)), 0, SizeX - 1);
            const int32 MaxX = FMath::Clamp(FMath::CeilToInt(FMath::Max3(P0.X, P1.X, P2.X)), 0, SizeX - 1);
            const int32 MinY = FMath::Clamp(FMath::FloorToInt(FMath::Min3(P0.Y, P1.Y, P2.Y)), 0, SizeY - 1);
            const int32 MaxY = FMath::Clamp(FMath::CeilToInt(FMath::Max3(P0.Y, P1.Y, P2.Y)), 0, SizeY - 1);

            for (int32 Y = MinY; Y <= MaxY; ++Y)
            {
                const float Py = static_cast<float>(Y) + 0.5f;
                for (int32 X = MinX; X <= MaxX; ++X)
                {
                    const float Px = static_cast<float>(X) + 0.5f;
                    const float W0 = ((P1.X - Px) * (P2.Y - Py) - (P2.X - Px) * (P1.Y - Py)) * InvArea;
                    const float W1 = ((P2.X - Px) * (P0.Y - Py) - (P0.X - Px) * (P2.Y - Py)) * InvArea;
                    const float W2 = 1.0f - W0 - W1;
                    if (W0 < -1e-3f || W1 < -1e-3f || W2 < -1e-3f) continue;

                    const FVector3f WorldP = Tri.Pos[0] * W0 + Tri.Pos[1] * W1 + Tri.Pos[2] * W2;
                    const FVector3f WorldN = (Tri.Normal[0] * W0 + Tri.Normal[1] * W1 + Tri.Normal[2] * W2).GetSafeNormal();
                    const FVector2f UV = Tri.UV[0] * W0 + Tri.UV[1] * W1 + Tri.UV[2] * W2;

                    FColor Col; float Mask = 0.0f;
                    SampleProjected(WorldP, WorldN, UV, Col, Mask);
                    const float Alpha = Mask * Opacity * (Col.A / 255.0f);
                    if (Alpha <= KINDA_SMALL_NUMBER) continue;

                    FColor& Pixel = Layer.Pixels[Y * SizeX + X];
                    if (Layer.bAlphaLock && Pixel.A == 0) continue;
                    const uint8 SavedA = Pixel.A;
                    Pixel = BlendPixel(Pixel, Col, Alpha);
                    if (Layer.bAlphaLock) Pixel.A = SavedA;
                    ++PaintedPixels;
                }
            }
        }
    }

    if (Settings.SeamDilationPixels > 0)
    {
        // Dilate without adding a second undo entry
        const int32 Iters = FMath::Clamp(Settings.SeamDilationPixels, 1, 8);
        TArray<FColor> Temp;
        for (int32 It = 0; It < Iters; ++It)
        {
            Temp = Layer.Pixels;
            for (int32 Y = 0; Y < SizeY; ++Y)
            {
                for (int32 X = 0; X < SizeX; ++X)
                {
                    if (Layer.Pixels[Y * SizeX + X].A > 0) continue;
                    int32 R = 0, G = 0, B = 0, A = 0, C = 0;
                    for (int32 Oy = -1; Oy <= 1; ++Oy)
                    {
                        const int32 Ny = Y + Oy;
                        if (Ny < 0 || Ny >= SizeY) continue;
                        for (int32 Ox = -1; Ox <= 1; ++Ox)
                        {
                            const int32 Nx = X + Ox;
                            if (Nx < 0 || Nx >= SizeX) continue;
                            const FColor N = Layer.Pixels[Ny * SizeX + Nx];
                            if (N.A > 0) { R += N.R; G += N.G; B += N.B; A += N.A; ++C; }
                        }
                    }
                    if (C > 0)
                    {
                        Temp[Y * SizeX + X] = FColor(uint8(R / C), uint8(G / C), uint8(B / C), uint8(A / C));
                    }
                }
            }
            Layer.Pixels = MoveTemp(Temp);
        }
    }

    MarkFullDirty();
    NotifyChanged();
    NotifyUndoTreeEdit(TEXT("3D Mesh Projection"), true);
    OutMessage = FString::Printf(TEXT("Projected onto mesh (%d pixels updated)."), PaintedPixels);
    return true;
}

bool FTiPaintDocument::StampWorld3D(const FVector3f& WorldPos, const FVector3f& WorldNormal, float WorldRadius, FString& OutMessage)
{
    if (!PaintLayers.IsValidIndex(ActiveLayerIndex) || PaintLayers[ActiveLayerIndex].bLocked)
    {
        OutMessage = TEXT("Active layer is locked or invalid.");
        return false;
    }
    if (Triangles.IsEmpty() || WorldRadius <= 1e-4f)
    {
        OutMessage = TEXT("3D brush stamp requires mesh triangles and positive radius.");
        return false;
    }

    SaveLayerUndoSnapshot(ActiveLayerIndex);
    FTiPaintLayer& Layer = PaintLayers[ActiveLayerIndex];
    const float RadiusSq = WorldRadius * WorldRadius;
    const float Hard = FMath::Clamp(ActiveBrush.Hardness, 0.0f, 0.999f);
    const float EffOpacity = FMath::Clamp(ActiveBrush.Opacity, 0.0f, 1.0f) * FMath::Clamp(ActiveBrush.Flow, 0.01f, 1.0f);
    const FVector3f NormDir = WorldNormal.GetSafeNormal();
    const bool bUseNormalFalloff = !NormDir.IsNearlyZero();

    int32 MinDirtyX = SizeX, MinDirtyY = SizeY, MaxDirtyX = -1, MaxDirtyY = -1;
    int32 PaintedPixels = 0;

    for (const FTiPaintUVTriangle& Tri : Triangles)
    {
        if (!Tri.bHas3D) continue;
        // Quick sphere-AABB reject
        const FVector3f TMin = Tri.Pos[0].ComponentMin(Tri.Pos[1]).ComponentMin(Tri.Pos[2]);
        const FVector3f TMax = Tri.Pos[0].ComponentMax(Tri.Pos[1]).ComponentMax(Tri.Pos[2]);
        const FVector3f Clamped = WorldPos.ComponentMax(TMin).ComponentMin(TMax);
        if (FVector3f::DistSquared(WorldPos, Clamped) > RadiusSq) continue;

        const FVector2f P0(Tri.UV[0].X * (SizeX - 1), (1.0f - Tri.UV[0].Y) * (SizeY - 1));
        const FVector2f P1(Tri.UV[1].X * (SizeX - 1), (1.0f - Tri.UV[1].Y) * (SizeY - 1));
        const FVector2f P2(Tri.UV[2].X * (SizeX - 1), (1.0f - Tri.UV[2].Y) * (SizeY - 1));
        const float Area = (P1.X - P0.X) * (P2.Y - P0.Y) - (P2.X - P0.X) * (P1.Y - P0.Y);
        if (FMath::Abs(Area) <= 1e-4f) continue;
        const float InvArea = 1.0f / Area;

        const int32 MinX = FMath::Clamp(FMath::FloorToInt(FMath::Min3(P0.X, P1.X, P2.X)), 0, SizeX - 1);
        const int32 MaxX = FMath::Clamp(FMath::CeilToInt(FMath::Max3(P0.X, P1.X, P2.X)), 0, SizeX - 1);
        const int32 MinY = FMath::Clamp(FMath::FloorToInt(FMath::Min3(P0.Y, P1.Y, P2.Y)), 0, SizeY - 1);
        const int32 MaxY = FMath::Clamp(FMath::CeilToInt(FMath::Max3(P0.Y, P1.Y, P2.Y)), 0, SizeY - 1);

        for (int32 Y = MinY; Y <= MaxY; ++Y)
        {
            const float Py = static_cast<float>(Y) + 0.5f;
            for (int32 X = MinX; X <= MaxX; ++X)
            {
                const float Px = static_cast<float>(X) + 0.5f;
                const float W0 = ((P1.X - Px) * (P2.Y - Py) - (P2.X - Px) * (P1.Y - Py)) * InvArea;
                const float W1 = ((P2.X - Px) * (P0.Y - Py) - (P0.X - Px) * (P2.Y - Py)) * InvArea;
                const float W2 = 1.0f - W0 - W1;
                if (W0 < -1e-3f || W1 < -1e-3f || W2 < -1e-3f) continue;

                const FVector3f SamplePos = Tri.Pos[0] * W0 + Tri.Pos[1] * W1 + Tri.Pos[2] * W2;
                const float DistSq = FVector3f::DistSquared(SamplePos, WorldPos);
                if (DistSq > RadiusSq) continue;

                float Facing = 1.0f;
                if (bUseNormalFalloff)
                {
                    const FVector3f SampleN = (Tri.Normal[0] * W0 + Tri.Normal[1] * W1 + Tri.Normal[2] * W2).GetSafeNormal();
                    Facing = FMath::Clamp(FVector3f::DotProduct(SampleN, NormDir), 0.0f, 1.0f);
                    if (Facing <= 0.05f) continue;
                }

                const float NormD = FMath::Sqrt(DistSq) / WorldRadius;
                const float Falloff = (NormD <= Hard) ? 1.0f : FMath::Clamp((1.0f - NormD) / FMath::Max(1.0f - Hard, 0.001f), 0.0f, 1.0f);
                const float Alpha = Falloff * Facing * EffOpacity;
                if (Alpha <= KINDA_SMALL_NUMBER) continue;

                FColor& Pixel = Layer.Pixels[Y * SizeX + X];
                if (Layer.bAlphaLock && Pixel.A == 0) continue;
                if (ActiveBrush.Mode == ETiPaintMode::Erase)
                {
                    if (!Layer.bAlphaLock)
                    {
                        Pixel.A = static_cast<uint8>(FMath::Clamp(FMath::RoundToInt(Pixel.A * (1.0f - Alpha)), 0, 255));
                    }
                }
                else
                {
                    const uint8 SavedA = Pixel.A;
                    Pixel = BlendPixel(Pixel, ActiveBrush.Color, Alpha * (ActiveBrush.Color.A / 255.0f));
                    if (Layer.bAlphaLock) Pixel.A = SavedA;
                }
                MinDirtyX = FMath::Min(MinDirtyX, X); MaxDirtyX = FMath::Max(MaxDirtyX, X);
                MinDirtyY = FMath::Min(MinDirtyY, Y); MaxDirtyY = FMath::Max(MaxDirtyY, Y);
                ++PaintedPixels;
            }
        }
    }

    if (MaxDirtyX >= MinDirtyX && MaxDirtyY >= MinDirtyY)
    {
        MarkDirtyRect(MinDirtyX, MinDirtyY, MaxDirtyX, MaxDirtyY);
        NotifyChanged();
    }
    OutMessage = FString::Printf(TEXT("3D brush stamped %d pixel(s)."), PaintedPixels);
    return PaintedPixels > 0;
}

bool FTiPaintDocument::DilateSeams(int32 Iterations)
{
    if (!PaintLayers.IsValidIndex(ActiveLayerIndex) || PaintLayers[ActiveLayerIndex].bLocked) return false;
    Iterations = FMath::Clamp(Iterations, 1, 16);
    SaveLayerUndoSnapshot(ActiveLayerIndex);
    FTiPaintLayer& Layer = PaintLayers[ActiveLayerIndex];

    TArray<FColor> Temp;
    for (int32 It = 0; It < Iterations; ++It)
    {
        Temp = Layer.Pixels;
        for (int32 Y = 0; Y < SizeY; ++Y)
        {
            for (int32 X = 0; X < SizeX; ++X)
            {
                if (Layer.Pixels[Y * SizeX + X].A > 0) continue;
                int32 R = 0, G = 0, B = 0, A = 0, C = 0;
                for (int32 Oy = -1; Oy <= 1; ++Oy)
                {
                    const int32 Ny = Y + Oy;
                    if (Ny < 0 || Ny >= SizeY) continue;
                    for (int32 Ox = -1; Ox <= 1; ++Ox)
                    {
                        const int32 Nx = X + Ox;
                        if (Nx < 0 || Nx >= SizeX) continue;
                        const FColor N = Layer.Pixels[Ny * SizeX + Nx];
                        if (N.A > 0) { R += N.R; G += N.G; B += N.B; A += N.A; ++C; }
                    }
                }
                if (C > 0)
                {
                    Temp[Y * SizeX + X] = FColor(uint8(R / C), uint8(G / C), uint8(B / C), uint8(A / C));
                }
            }
        }
        Layer.Pixels = MoveTemp(Temp);
    }

    MarkFullDirty();
    NotifyChanged();
    return true;
}

FString FTiPaintDocument::BlendModeToString(ETiPaintBlendMode Mode)
{
    switch (Mode)
    {
    case ETiPaintBlendMode::Multiply: return TEXT("multiply");
    case ETiPaintBlendMode::Screen:   return TEXT("screen");
    case ETiPaintBlendMode::Overlay:  return TEXT("overlay");
    case ETiPaintBlendMode::Add:      return TEXT("add");
    case ETiPaintBlendMode::Darken:   return TEXT("darken");
    case ETiPaintBlendMode::Lighten:  return TEXT("lighten");
    case ETiPaintBlendMode::Normal:
    default:                          return TEXT("normal");
    }
}

bool FTiPaintDocument::ParseBlendMode(const FString& Str, ETiPaintBlendMode& OutMode)
{
    const FString L = Str.ToLower().TrimStartAndEnd();
    if (L == TEXT("normal"))   { OutMode = ETiPaintBlendMode::Normal; return true; }
    if (L == TEXT("multiply")) { OutMode = ETiPaintBlendMode::Multiply; return true; }
    if (L == TEXT("screen"))   { OutMode = ETiPaintBlendMode::Screen; return true; }
    if (L == TEXT("overlay"))  { OutMode = ETiPaintBlendMode::Overlay; return true; }
    if (L == TEXT("add"))      { OutMode = ETiPaintBlendMode::Add; return true; }
    if (L == TEXT("darken"))   { OutMode = ETiPaintBlendMode::Darken; return true; }
    if (L == TEXT("lighten"))  { OutMode = ETiPaintBlendMode::Lighten; return true; }
    return false;
}
