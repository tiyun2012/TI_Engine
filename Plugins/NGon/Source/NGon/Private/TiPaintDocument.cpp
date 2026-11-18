#include "TiPaintDocument.h"

#include "IImageWrapper.h"
#include "IImageWrapperModule.h"
#include "Modules/ModuleManager.h"

FTiPaintDocument::FTiPaintDocument(int32 InWidth, int32 InHeight)
    : SizeX(FMath::Max(1, InWidth)), SizeY(FMath::Max(1, InHeight))
{
    FTiPaintLayer& Layer = PaintLayers.AddDefaulted_GetRef();
    Layer.Name = TEXT("Paint Layer");
    Layer.Pixels.Init(FColor::Transparent, SizeX * SizeY);
    CompositePixels.Init(FColor::Transparent, SizeX * SizeY);
    MarkChanged();
}

const TArray<FColor>& FTiPaintDocument::Composite() const
{
    CompositePixels.Init(FColor::Transparent, SizeX * SizeY);
    for (const FTiPaintLayer& Layer : PaintLayers)
    {
        if (!Layer.bVisible || Layer.Pixels.Num() != CompositePixels.Num()) continue;
        const float LayerAlpha = FMath::Clamp(Layer.Opacity, 0.0f, 1.0f);
        for (int32 I = 0; I < CompositePixels.Num(); ++I)
        {
            const FColor Source = Layer.Pixels[I];
            const float Alpha = (Source.A / 255.0f) * LayerAlpha;
            CompositePixels[I] = BlendPixel(CompositePixels[I], Source, Alpha);
        }
    }
    return CompositePixels;
}

FColor FTiPaintDocument::BlendPixel(const FColor& Base, const FColor& Paint, float Alpha)
{
    const float BaseA = Base.A / 255.0f;
    const float OutA = Alpha + BaseA * (1.0f - Alpha);
    if (OutA <= KINDA_SMALL_NUMBER) return FColor::Transparent;
    auto Channel = [Alpha, BaseA, OutA](uint8 B, uint8 P) -> uint8
    {
        return static_cast<uint8>(FMath::Clamp(FMath::RoundToInt((P * Alpha + B * BaseA * (1.0f - Alpha)) / OutA), 0, 255));
    };
    return FColor(Channel(Base.R, Paint.R), Channel(Base.G, Paint.G), Channel(Base.B, Paint.B),
        static_cast<uint8>(FMath::Clamp(FMath::RoundToInt(OutA * 255.0f), 0, 255)));
}

void FTiPaintDocument::SaveUndoSnapshot()
{
    UndoStack.Add({ PaintLayers, ActiveLayerIndex });
    if (UndoStack.Num() > 32) UndoStack.RemoveAt(0);
    RedoStack.Reset();
}

void FTiPaintDocument::RestoreSnapshot(const FSnapshot& Snapshot)
{
    PaintLayers = Snapshot.Layers;
    ActiveLayerIndex = FMath::Clamp(Snapshot.Active, 0, FMath::Max(0, PaintLayers.Num() - 1));
    MarkChanged();
}

void FTiPaintDocument::BeginStroke(float U, float V)
{
    if (bStroking || !PaintLayers.IsValidIndex(ActiveLayerIndex) || PaintLayers[ActiveLayerIndex].bLocked) return;
    StrokeSnapshot = { PaintLayers, ActiveLayerIndex };
    SaveUndoSnapshot();
    bStroking = true;
    LastUV = FVector2f(FMath::Clamp(U, 0.0f, 1.0f), FMath::Clamp(V, 0.0f, 1.0f));
    Stamp(U, V);
    // Notify once per input event.  StampAtPixel used to notify for every pixel
    // in the brush footprint, which caused the NGon preview texture and render
    // resource to be rebuilt dozens of times for one mouse event.
    MarkChanged();
}

void FTiPaintDocument::AddStrokePoint(float U, float V)
{
    if (!bStroking) return;
    const FVector2f Current(FMath::Clamp(U, 0.0f, 1.0f), FMath::Clamp(V, 0.0f, 1.0f));
    const float Distance = FVector2f::Distance(LastUV, Current) * FMath::Max(SizeX, SizeY);
    const float Step = FMath::Max(1.0f, ActiveBrush.Radius * 0.25f);
    const int32 Count = FMath::Clamp(FMath::CeilToInt(Distance / Step), 1, 256);
    for (int32 I = 1; I <= Count; ++I)
    {
        const float T = static_cast<float>(I) / Count;
        Stamp(FMath::Lerp(LastUV.X, Current.X, T), FMath::Lerp(LastUV.Y, Current.Y, T));
    }
    LastUV = Current;
    MarkChanged();
}

void FTiPaintDocument::EndStroke()
{
    if (!bStroking) return;
    bStroking = false;
}

void FTiPaintDocument::CancelStroke()
{
    if (!bStroking) return;
    bStroking = false;
    RestoreSnapshot(StrokeSnapshot);
    if (UndoStack.Num() > 0) UndoStack.Pop();
}

void FTiPaintDocument::Stamp(float U, float V)
{
    const int32 X = FMath::RoundToInt(FMath::Clamp(U, 0.0f, 1.0f) * (SizeX - 1));
    const int32 Y = FMath::RoundToInt((1.0f - FMath::Clamp(V, 0.0f, 1.0f)) * (SizeY - 1));
    StampAtPixel(X, Y);
}

void FTiPaintDocument::StampAtPixel(int32 X, int32 Y)
{
    if (!PaintLayers.IsValidIndex(ActiveLayerIndex)) return;
    FTiPaintLayer& Layer = PaintLayers[ActiveLayerIndex];
    const int32 Radius = FMath::CeilToInt(FMath::Max(1.0f, ActiveBrush.Radius));
    const int32 MinX = FMath::Max(0, X - Radius), MaxX = FMath::Min(SizeX - 1, X + Radius);
    const int32 MinY = FMath::Max(0, Y - Radius), MaxY = FMath::Min(SizeY - 1, Y + Radius);
    const float RadiusSq = ActiveBrush.Radius * ActiveBrush.Radius;
    for (int32 Py = MinY; Py <= MaxY; ++Py)
    {
        for (int32 Px = MinX; Px <= MaxX; ++Px)
        {
            const float Distance = FVector2D(Px - X, Py - Y).Size();
            if (Distance > ActiveBrush.Radius) continue;
            const float Normalized = Distance / FMath::Max(ActiveBrush.Radius, 1.0f);
            const float Falloff = Normalized < ActiveBrush.Hardness ? 1.0f :
                FMath::Clamp((1.0f - Normalized) / FMath::Max(1.0f - ActiveBrush.Hardness, 0.001f), 0.0f, 1.0f);
            const float Alpha = Falloff * FMath::Clamp(ActiveBrush.Opacity, 0.0f, 1.0f);
            FColor& Pixel = Layer.Pixels[Py * SizeX + Px];
            if (ActiveBrush.Mode == ETiPaintMode::Erase)
            {
                Pixel.A = static_cast<uint8>(FMath::Clamp(FMath::RoundToInt(Pixel.A * (1.0f - Alpha)), 0, 255));
            }
            else
            {
                Pixel = BlendPixel(Pixel, ActiveBrush.Color, Alpha * (ActiveBrush.Color.A / 255.0f));
            }
        }
    }
}

void FTiPaintDocument::Undo()
{
    if (bStroking) CancelStroke();
    if (UndoStack.IsEmpty()) return;
    RedoStack.Add({ PaintLayers, ActiveLayerIndex });
    const FSnapshot Snapshot = UndoStack.Pop();
    RestoreSnapshot(Snapshot);
}

void FTiPaintDocument::Redo()
{
    if (bStroking) CancelStroke();
    if (RedoStack.IsEmpty()) return;
    UndoStack.Add({ PaintLayers, ActiveLayerIndex });
    const FSnapshot Snapshot = RedoStack.Pop();
    RestoreSnapshot(Snapshot);
}

void FTiPaintDocument::AddLayer()
{
    SaveUndoSnapshot();
    FTiPaintLayer& Layer = PaintLayers.AddDefaulted_GetRef();
    Layer.Name = FString::Printf(TEXT("Paint Layer %d"), PaintLayers.Num());
    Layer.Pixels.Init(FColor::Transparent, SizeX * SizeY);
    ActiveLayerIndex = PaintLayers.Num() - 1;
    MarkChanged();
}

void FTiPaintDocument::RemoveActiveLayer()
{
    if (PaintLayers.Num() <= 1 || !PaintLayers.IsValidIndex(ActiveLayerIndex)) return;
    SaveUndoSnapshot();
    PaintLayers.RemoveAt(ActiveLayerIndex);
    ActiveLayerIndex = FMath::Clamp(ActiveLayerIndex, 0, PaintLayers.Num() - 1);
    MarkChanged();
}

void FTiPaintDocument::ToggleActiveVisibility() { if (PaintLayers.IsValidIndex(ActiveLayerIndex)) { SaveUndoSnapshot(); PaintLayers[ActiveLayerIndex].bVisible = !PaintLayers[ActiveLayerIndex].bVisible; MarkChanged(); } }
void FTiPaintDocument::ToggleActiveLock() { if (PaintLayers.IsValidIndex(ActiveLayerIndex)) { SaveUndoSnapshot(); PaintLayers[ActiveLayerIndex].bLocked = !PaintLayers[ActiveLayerIndex].bLocked; MarkChanged(); } }
void FTiPaintDocument::SetActiveLayer(int32 Index) { if (PaintLayers.IsValidIndex(Index) && Index != ActiveLayerIndex) { ActiveLayerIndex = Index; MarkChanged(); } }

void FTiPaintDocument::Clear()
{
    if (!PaintLayers.IsValidIndex(ActiveLayerIndex) || PaintLayers[ActiveLayerIndex].bLocked) return;
    SaveUndoSnapshot();
    PaintLayers[ActiveLayerIndex].Pixels.Init(FColor::Transparent, SizeX * SizeY);
    MarkChanged();
}

bool FTiPaintDocument::ExportPNG(const FString& Filename, FText& Error) const
{
    IImageWrapperModule& Module = FModuleManager::LoadModuleChecked<IImageWrapperModule>(TEXT("ImageWrapper"));
    const TSharedPtr<IImageWrapper> Wrapper = Module.CreateImageWrapper(EImageFormat::PNG);
    TArray<uint8> RGBA; RGBA.Reserve(Composite().Num() * 4);
    for (const FColor& Pixel : Composite()) { RGBA.Add(Pixel.R); RGBA.Add(Pixel.G); RGBA.Add(Pixel.B); RGBA.Add(Pixel.A); }
    if (!Wrapper->SetRaw(RGBA.GetData(), RGBA.Num(), SizeX, SizeY, ERGBFormat::RGBA, 8)) { Error = FText::FromString(TEXT("PNG encoder rejected the image.")); return false; }
    if (!FFileHelper::SaveArrayToFile(Wrapper->GetCompressed(), *Filename)) { Error = FText::FromString(TEXT("Could not write the PNG file.")); return false; }
    Error = FText::GetEmpty(); return true;
}

void FTiPaintDocument::MarkChanged() { CompositePixels.Reset(); if (Changed) Changed(); }
// NGon Unified ABI Sync v5.8.2
