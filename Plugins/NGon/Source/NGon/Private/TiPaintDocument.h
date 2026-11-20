#pragma once

#include "CoreMinimal.h"
#include "TiPaintTypes.h"

class FNGonUndoSession;

/**
 * High-performance, extensible RGBA Paint Document with:
 * - Dirty-rect incremental compositing & monotonic revision counter (for GPU texture sync)
 * - Per-layer delta undo/redo snapshots (avoids full-stack copies on brush strokes)
 * - Full brush engine (Paint, Erase, Smudge, Blur, Flood Fill, Eyedropper, Tips, Symmetry)
 * - Photoshop-style layer blend modes, opacity, alpha lock, reorder, duplicate, merge, flatten
 * - Procedural & PNG/JPG image stencils
 * - 3D-to-UV projection (Planar, Triplanar, Cylindrical, Spherical, 3D World Brush) + UV seam dilation
 */
class FTiPaintDocument : public TSharedFromThis<FTiPaintDocument>
{
public:
    explicit FTiPaintDocument(int32 InWidth = 512, int32 InHeight = 512);

    // --- Document Info & Revision ---
    int32 Width() const { return SizeX; }
    int32 Height() const { return SizeY; }
    uint64 Revision() const { return DocumentRevision; }
    const TArray<FTiPaintLayer>& Layers() const { return PaintLayers; }
    int32 ActiveLayer() const { return ActiveLayerIndex; }
    const FTiPaintBrush& Brush() const { return ActiveBrush; }
    void SetBrush(const FTiPaintBrush& InBrush) { ActiveBrush = InBrush; }
    const FTiPaintStencil& Stencil() const { return ActiveStencil; }
    void SetStencil(const FTiPaintStencil& InStencil) { ActiveStencil = InStencil; }
    const FTiPaintProjectionSettings& Projection() const { return ActiveProjection; }
    void SetProjection(const FTiPaintProjectionSettings& InProj) { ActiveProjection = InProj; }
    void SetChanged(TFunction<void()> InChanged) { Changed = MoveTemp(InChanged); }
    void SetUndoSession(const TWeakPtr<FNGonUndoSession>& InSession) { UndoSession = InSession; }

    /** Mesh triangles (UV + optional 3D Pos/Normal + SubMeshIndex) used for 3D projection, seam dilation, and UV-overlap masking. */
    void SetMeshTriangles(const TArray<FTiPaintUVTriangle>& InTriangles);
    const TArray<FTiPaintUVTriangle>& MeshTriangles() const { return Triangles; }

    // --- SubMesh / Material UV Filtering (prevents painting across overlapping SubMesh UVs) ---
    void SetSubMeshes(const TArray<FTiPaintSubMeshInfo>& InSubMeshes);
    const TArray<FTiPaintSubMeshInfo>& GetSubMeshes() const { return SubMeshes; }
    int32 GetActiveSubMeshFilter() const { return ActiveSubMeshFilter; }
    void SetActiveSubMeshFilter(int32 SubMeshIndex);
    bool IsSubMeshVisible(int32 SubMeshIndex) const { return ActiveSubMeshFilter == INDEX_NONE || ActiveSubMeshFilter == SubMeshIndex; }
    void SetSubMeshMaskEnabled(bool bEnabled);
    bool IsSubMeshMaskEnabled() const { return bSubMeshMaskEnabled; }

    /** Returns cached composite RGBA pixels, updating only the dirty sub-rectangle when possible. */
    const TArray<FColor>& Composite() const;

    bool IsStroking() const { return bStroking; }
    int32 UndoCount() const { return UndoStack.Num(); }
    int32 RedoCount() const { return RedoStack.Num(); }

    // --- Interactive & Programmatic Painting ---
    void BeginStroke(float U, float V);
    void AddStrokePoint(float U, float V);
    void AddStrokeDiscontinuousPoint(float U, float V);
    void EndStroke();
    void CancelStroke();

    /** Direct drawing primitives (each records a single undo entry). */
    bool DrawStrokePolyline(const TArray<FVector2f>& UVPoints);
    bool DrawLine(float U0, float V0, float U1, float V1);
    bool DrawRect(float U0, float V0, float U1, float V1, bool bFilled = true);
    bool DrawCircle(float CenterU, float CenterV, float RadiusUV, bool bFilled = true);
    bool FloodFill(float U, float V, float Tolerance = 0.08f);
    FColor SampleCompositeColor(float U, float V) const;

    // --- History (Per-Layer Delta + Full-Stack Structural Undo) ---
    void Undo();
    void Redo();

    // --- Layer Management ---
    void AddLayer(const FString& Name = FString());
    void DuplicateActiveLayer();
    void RemoveActiveLayer();
    bool MoveActiveLayer(int32 Delta);
    bool MergeActiveLayerDown();
    void FlattenVisibleLayers();
    void ToggleActiveVisibility();
    void ToggleActiveLock();
    void ToggleActiveAlphaLock();
    void SetActiveLayer(int32 Index);
    void RenameActiveLayer(const FString& NewName);
    void SetActiveLayerOpacity(float NewOpacity);
    void SetActiveLayerBlendMode(ETiPaintBlendMode NewMode);
    void FillActiveLayer(const FColor& Color);
    void Clear();

    // --- Document Resize & I/O ---
    bool Resize(int32 NewWidth, int32 NewHeight);
    bool ExportPNG(const FString& Filename, FText& Error) const;
    bool ImportImageToNewLayer(const FString& Filename, FText& Error);

    // --- Stencil & 3D Projection ---
    bool LoadStencilImage(const FString& Filename, FText& Error);
    float EvaluateStencilMask(float U, float V, FColor* OutStencilColor = nullptr) const;

    /** Projects current stencil/color onto the active layer using the mesh's 3D triangles. */
    bool ProjectOntoMesh(const FTiPaintProjectionSettings& Settings, FString& OutMessage);

    /** Stamps a 3D world-space sphere brush across all mesh triangles (seamless across UV seams). */
    bool StampWorld3D(const FVector3f& WorldPos, const FVector3f& WorldNormal, float WorldRadius, FString& OutMessage);

    /** Bleeds opaque UV island border pixels outward by Iterations pixels to eliminate mip/UV seams. */
    bool DilateSeams(int32 Iterations = 2);

    /** Utility helpers for blend mode / stencil / tip / projection string conversion. */
    static FString BlendModeToString(ETiPaintBlendMode Mode);
    static bool ParseBlendMode(const FString& Str, ETiPaintBlendMode& OutMode);

private:
    struct FUndoEntry
    {
        bool bFullStack = false;
        int32 LayerIndex = 0;
        FTiPaintLayer SingleLayer;      // Used when bFullStack == false (fast per-layer delta!)
        TArray<FTiPaintLayer> AllLayers; // Used when bFullStack == true (structural ops)
        int32 ActiveIndex = 0;
        int32 DocWidth = 512;
        int32 DocHeight = 512;
    };

    void SaveLayerUndoSnapshot(int32 LayerIndex);
    void SaveFullUndoSnapshot();
    void RestoreUndoEntry(const FUndoEntry& Entry);

    void StampWithSymmetry(float U, float V);
    void StampSingleUV(float U, float V);
    void StampAtPixel(int32 X, int32 Y, float StrokeRandSeed);
    float EvaluateBrushTip(float DX, float DY, float Radius, float Seed) const;
    void MarkDirtyRect(int32 MinX, int32 MinY, int32 MaxX, int32 MaxY);
    void MarkFullDirty();
    void NotifyChanged();

    static FColor BlendPixelWithMode(const FColor& Base, const FColor& Src, float Alpha, ETiPaintBlendMode Mode);
    static FColor BlendPixel(const FColor& Base, const FColor& Paint, float Alpha);

    int32 SizeX = 512;
    int32 SizeY = 512;
    TArray<FTiPaintLayer> PaintLayers;
    int32 ActiveLayerIndex = 0;

    FTiPaintBrush ActiveBrush;
    FTiPaintStencil ActiveStencil;
    FTiPaintProjectionSettings ActiveProjection;
    TArray<FTiPaintUVTriangle> Triangles;
    TArray<FTiPaintSubMeshInfo> SubMeshes;
    int32 ActiveSubMeshFilter = INDEX_NONE;
    bool bSubMeshMaskEnabled = true;
    bool bSubMeshMaskActive = false;
    TArray<uint8> SubMeshPixelMask;
    void RebuildSubMeshMask();

    // Cached incremental composite buffer
    mutable TArray<FColor> CompositePixels;
    mutable bool bCompositeDirty = true;
    mutable bool bFullCompositeDirty = true;
    mutable FIntRect DirtyRect = FIntRect(0, 0, 0, 0);
    uint64 DocumentRevision = 1;

    // Undo / Redo stacks (delta for strokes, full for structural ops)
    TArray<FUndoEntry> UndoStack;
    TArray<FUndoEntry> RedoStack;
    FUndoEntry ActiveStrokeBackup;

    // Interactive stroke state
    bool bStroking = false;
    FVector2f LastUV = FVector2f::ZeroVector;
    float StrokeRemainderPx = 0.0f;
    uint32 StrokeStampCounter = 0;
    FColor SmudgeCarryColor = FColor::Transparent;
    bool bHasSmudgeCarry = false;

    void NotifyUndoTreeEdit(const FString& Label, bool bIsProjection = false);
    TWeakPtr<FNGonUndoSession> UndoSession;
    TFunction<void()> Changed;
};
