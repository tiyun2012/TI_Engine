#include "NGonUVEditor.h"

#include "DynamicMesh/DynamicMesh3.h"
#include "DynamicMesh/DynamicMeshAttributeSet.h"
#include "Framework/Application/SlateApplication.h"
#include "Framework/Docking/TabManager.h"
#include "Misc/Paths.h"
#include "NGonFbx.h"
#include "NGonMeshFiles.h"
#include "NGonTopology.h"
#include "Rendering/DrawElements.h"
#include "Widgets/Docking/SDockTab.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Layout/SWrapBox.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/Text/STextBlock.h"

using namespace UE::Geometry;

#define LOCTEXT_NAMESPACE "NGonUVEditor"

namespace
{
float DistSq(const FVector2f& A, const FVector2f& B)
{
    return (A - B).SizeSquared();
}

struct FUVCachedEdge
{
    int32 ElementA = INDEX_NONE;
    int32 ElementB = INDEX_NONE;
    int32 FaceA = INDEX_NONE;
    int32 FaceB = INDEX_NONE;
    bool bIsSeamBorder = false;
};

struct FUVDocumentAccel
{
    TArray<FUVCachedEdge> PolygonEdges;
    int32 SeamEdgeCount = 0;

    void BuildFromDocument(const FNGonUVDocument& Doc)
    {
        PolygonEdges.Reset();
        SeamEdgeCount = 0;

        struct FEdgeAccum
        {
            int32 ElementA = INDEX_NONE;
            int32 ElementB = INDEX_NONE;
            int32 FaceA = INDEX_NONE;
            int32 FaceB = INDEX_NONE;
            int32 Count = 0;
        };

        const TArray<FNGonUVDocument::FTriangle>& Tris = Doc.GetTriangles();
        TMap<uint64, FEdgeAccum> EdgeMap;
        EdgeMap.Reserve(Tris.Num() * 2);

        for (const FNGonUVDocument::FTriangle& Tri : Tris)
        {
            for (int32 Corner = 0; Corner < 3; ++Corner)
            {
                const int32 E0 = Tri.Element[Corner];
                const int32 E1 = Tri.Element[(Corner + 1) % 3];
                if (E0 == INDEX_NONE || E1 == INDEX_NONE || E0 == E1)
                {
                    continue;
                }
                const int32 MinE = FMath::Min(E0, E1);
                const int32 MaxE = FMath::Max(E0, E1);
                const uint64 Key = (static_cast<uint64>(static_cast<uint32>(MinE)) << 32)
                    | static_cast<uint32>(MaxE);

                FEdgeAccum& Accum = EdgeMap.FindOrAdd(Key);
                if (Accum.Count == 0)
                {
                    Accum.ElementA = MinE;
                    Accum.ElementB = MaxE;
                    Accum.FaceA = Tri.FaceId;
                    Accum.Count = 1;
                }
                else if (Accum.Count == 1)
                {
                    Accum.FaceB = Tri.FaceId;
                    Accum.Count = 2;
                }
                else
                {
                    ++Accum.Count;
                }
            }
        }

        PolygonEdges.Reserve(EdgeMap.Num());
        for (const TPair<uint64, FEdgeAccum>& Pair : EdgeMap)
        {
            const FEdgeAccum& Accum = Pair.Value;
            // Filter out internal triangulation diagonals inside the same authored N-Gon/Quad face
            if (Accum.Count == 2 && Accum.FaceA != INDEX_NONE && Accum.FaceA == Accum.FaceB)
            {
                continue;
            }

            FUVCachedEdge Edge;
            Edge.ElementA = Accum.ElementA;
            Edge.ElementB = Accum.ElementB;
            Edge.FaceA = Accum.FaceA;
            Edge.FaceB = Accum.FaceB;
            Edge.bIsSeamBorder = (Accum.Count == 1);
            if (Edge.bIsSeamBorder)
            {
                ++SeamEdgeCount;
            }
            PolygonEdges.Add(Edge);
        }
    }
};

TMap<const FNGonUVDocument*, FUVDocumentAccel> GUVDocAccelMap;

const FUVDocumentAccel& GetUVDocAccel(const FNGonUVDocument* Doc)
{
    if (const FUVDocumentAccel* Existing = GUVDocAccelMap.Find(Doc))
    {
        return *Existing;
    }
    FUVDocumentAccel& Created = GUVDocAccelMap.Add(Doc);
    Created.BuildFromDocument(*Doc);
    return Created;
}

void UpdateTouchedTriangles(
    const TSet<int32>& ChangedElements,
    const TMap<int32, TArray<int32>>& ElementTriangles,
    const TMap<int32, FVector2f>& ElementUV,
    TArray<FNGonUVDocument::FTriangle>& Triangles)
{
    TSet<int32> TouchedTris;
    TouchedTris.Reserve(ChangedElements.Num() * 4);
    for (const int32 ElemId : ChangedElements)
    {
        if (const TArray<int32>* TriList = ElementTriangles.Find(ElemId))
        {
            for (const int32 TriIdx : *TriList)
            {
                TouchedTris.Add(TriIdx);
            }
        }
    }

    for (const int32 TriIdx : TouchedTris)
    {
        if (Triangles.IsValidIndex(TriIdx))
        {
            FNGonUVDocument::FTriangle& Tri = Triangles[TriIdx];
            for (int32 Corner = 0; Corner < 3; ++Corner)
            {
                if (const FVector2f* FoundUV = ElementUV.Find(Tri.Element[Corner]))
                {
                    Tri.UV[Corner] = *FoundUV;
                }
            }
        }
    }
}

struct FActiveViewportUVState
{
    FNGonMeshData MeshData;
    FText SourceLabel;
    bool bHasActiveMesh = false;
    FSimpleMulticastDelegate OnActiveMeshChanged;
};

FActiveViewportUVState& GetActiveViewportUVState()
{
    static FActiveViewportUVState State;
    return State;
}
}

namespace NGon
{
void SetActiveViewportUVMesh(const FNGonMeshData& Data, const FText& InSourceName, const FText& InSourceType)
{
    FActiveViewportUVState& State = GetActiveViewportUVState();
    State.MeshData.Topology = Data.Topology;
    State.MeshData.Mesh.Copy(Data.Mesh);
    State.MeshData.ImportedHeatSamples = Data.ImportedHeatSamples;
    State.SourceLabel = InSourceType.IsEmpty()
        ? InSourceName
        : FText::Format(LOCTEXT("ActiveViewportMeshSource", "{0} ({1})"), InSourceName, InSourceType);
    State.bHasActiveMesh = true;
    State.OnActiveMeshChanged.Broadcast();
}

void UpdateActiveViewportUVMeshFromTopology(const FNGonTopology& Topology)
{
    if (Topology.RestPositions.IsEmpty() || Topology.Faces.IsEmpty())
    {
        return;
    }

    TArray<TArray<int32>> RemainingFaces;
    RemainingFaces.Reserve(Topology.Faces.Num());
    for (const TPair<int32, FNGonFace>& FacePair : Topology.Faces)
    {
        if (FacePair.Value.Perimeter.Num() >= 3)
        {
            RemainingFaces.Add(FacePair.Value.Perimeter);
        }
    }

    FNGonMeshData RebuiltData;
    FText Error;
    if (BuildPolygonMesh(Topology.RestPositions, RemainingFaces, RebuiltData, Error))
    {
        FActiveViewportUVState& State = GetActiveViewportUVState();
        State.MeshData = MoveTemp(RebuiltData);
        if (State.SourceLabel.IsEmpty())
        {
            State.SourceLabel = LOCTEXT("ViewportActiveMeshLabel", "Viewport Active Mesh");
        }
        State.bHasActiveMesh = true;
        State.OnActiveMeshChanged.Broadcast();
    }
}

void NotifyHostViewportClosed()
{
    FActiveViewportUVState& State = GetActiveViewportUVState();
    State.bHasActiveMesh = false;
    State.MeshData = FNGonMeshData{};
    State.SourceLabel = FText::GetEmpty();

    if (FSlateApplication::IsInitialized())
    {
        if (const TSharedPtr<SDockTab> UVTab = FGlobalTabmanager::Get()->FindExistingLiveTab(FName(TEXT("NGonUVEditor"))))
        {
            UVTab->RequestCloseTab();
        }
    }
}
}

bool FNGonUVDocument::Load(const FNGonMeshData& Data, FText& Error)
{
    Clear();
    const FDynamicMesh3& Mesh = Data.Mesh;
    if (!Mesh.HasAttributes() || !Mesh.Attributes()->PrimaryUV())
    {
        Error = LOCTEXT("NoUVs", "The mesh has no primary UV channel.");
        return false;
    }
    const FDynamicMeshUVOverlay* UVs = Mesh.Attributes()->PrimaryUV();
    Triangles.Reserve(Mesh.TriangleCount());
    ElementUV.Reserve(UVs->MaxElementID());
    ElementTriangles.Reserve(UVs->MaxElementID());

    for (const int32 TriangleId : Mesh.TriangleIndicesItr())
    {
        if (!UVs->IsSetTriangle(TriangleId))
        {
            continue;
        }
        const FIndex3i Elements = UVs->GetTriangle(TriangleId);
        const int32 TriIdx = Triangles.AddDefaulted();
        FTriangle& Out = Triangles[TriIdx];
        Out.Element[0] = Elements.A;
        Out.Element[1] = Elements.B;
        Out.Element[2] = Elements.C;
        Out.FaceId = Mesh.GetTriangleGroup(TriangleId);
        for (int32 Corner = 0; Corner < 3; ++Corner)
        {
            const int32 ElementId = Out.Element[Corner];
            Out.UV[Corner] = UVs->GetElement(ElementId);
            ElementUV.FindOrAdd(ElementId) = Out.UV[Corner];
            ElementTriangles.FindOrAdd(ElementId).Add(TriIdx);
            Bounds += FVector2D(Out.UV[Corner]);
        }
    }
    if (Triangles.IsEmpty())
    {
        Error = LOCTEXT("NoTriangleUVs", "The mesh primary UV channel contains no triangle data.");
        return false;
    }
    RebuildIslands();
    GUVDocAccelMap.FindOrAdd(this).BuildFromDocument(*this);
    Error = FText::GetEmpty();
    return true;
}

void FNGonUVDocument::Clear()
{
    GUVDocAccelMap.Remove(this);
    Triangles.Reset();
    ElementUV.Reset();
    ElementTriangles.Reset();
    SelectedElements.Reset();
    SelectedFaces.Reset();
    SelectedIslands.Reset();
    Bounds = FBox2D(ForceInit);
    IslandCount = 0;
}

FVector2f FNGonUVDocument::GetElementUV(int32 ElementId) const
{
    if (const FVector2f* UV = ElementUV.Find(ElementId))
    {
        return *UV;
    }
    return FVector2f::ZeroVector;
}

void FNGonUVDocument::RebuildBounds()
{
    Bounds = FBox2D(ForceInit);
    for (const TPair<int32, FVector2f>& Pair : ElementUV)
    {
        Bounds += FVector2D(Pair.Value);
    }
}

void FNGonUVDocument::RebuildIslands()
{
    TArray<int32> Parent;
    Parent.SetNumUninitialized(Triangles.Num());
    for (int32 I = 0; I < Parent.Num(); ++I)
    {
        Parent[I] = I;
    }
    auto Find = [&Parent](int32 I) -> int32
    {
        int32 Root = I;
        while (Parent[Root] != Root)
        {
            Root = Parent[Root];
        }
        while (I != Root)
        {
            const int32 Next = Parent[I];
            Parent[I] = Root;
            I = Next;
        }
        return Root;
    };
    auto Union = [&Parent, &Find](int32 A, int32 B)
    {
        A = Find(A);
        B = Find(B);
        if (A != B)
        {
            Parent[B] = A;
        }
    };
    for (const TPair<int32, TArray<int32>>& Pair : ElementTriangles)
    {
        for (int32 I = 1; I < Pair.Value.Num(); ++I)
        {
            Union(Pair.Value[0], Pair.Value[I]);
        }
    }
    TMap<int32, int32> Remap;
    Remap.Reserve(Triangles.Num());
    IslandCount = 0;
    for (int32 I = 0; I < Triangles.Num(); ++I)
    {
        const int32 Root = Find(I);
        int32* Island = Remap.Find(Root);
        if (!Island)
        {
            Island = &Remap.Add(Root, IslandCount++);
        }
        Triangles[I].Island = *Island;
    }
}

void FNGonUVDocument::ClearSelection()
{
    SelectedElements.Reset();
    SelectedFaces.Reset();
    SelectedIslands.Reset();
}

void FNGonUVDocument::AddElementSelection(int32 Id, bool bAdd)
{
    if (!bAdd)
    {
        ClearSelection();
    }
    if (Id == INDEX_NONE)
    {
        return;
    }
    SelectedElements.Add(Id);
    if (const TArray<int32>* Tris = ElementTriangles.Find(Id))
    {
        for (const int32 Tri : *Tris)
        {
            SelectedFaces.Add(Triangles[Tri].FaceId);
            SelectedIslands.Add(Triangles[Tri].Island);
        }
    }
}

void FNGonUVDocument::SelectFaceTriangle(int32 TriangleIndex, bool bAdd)
{
    if (!Triangles.IsValidIndex(TriangleIndex))
    {
        return;
    }
    if (!bAdd)
    {
        ClearSelection();
    }
    const int32 TargetFaceId = Triangles[TriangleIndex].FaceId;
    SelectedFaces.Add(TargetFaceId);
    for (const FTriangle& Tri : Triangles)
    {
        if (Tri.FaceId == TargetFaceId)
        {
            SelectedElements.Add(Tri.Element[0]);
            SelectedElements.Add(Tri.Element[1]);
            SelectedElements.Add(Tri.Element[2]);
            SelectedIslands.Add(Tri.Island);
        }
    }
}

void FNGonUVDocument::SelectIsland(int32 Island, bool bAdd)
{
    if (!bAdd)
    {
        ClearSelection();
    }
    if (Island < 0)
    {
        return;
    }
    SelectedIslands.Add(Island);
    for (const FTriangle& Tri : Triangles)
    {
        if (Tri.Island == Island)
        {
            SelectedElements.Add(Tri.Element[0]);
            SelectedElements.Add(Tri.Element[1]);
            SelectedElements.Add(Tri.Element[2]);
            SelectedFaces.Add(Tri.FaceId);
        }
    }
}

static bool IsPointInUVTriangle(const FVector2f& P, const FVector2f& A, const FVector2f& B, const FVector2f& C)
{
    const auto Cross2D = [](const FVector2f& U, const FVector2f& V) { return U.X * V.Y - U.Y * V.X; };
    const float D1 = Cross2D(B - A, P - A);
    const float D2 = Cross2D(C - B, P - B);
    const float D3 = Cross2D(A - C, P - C);
    const bool bHasNeg = (D1 < -1e-6f) || (D2 < -1e-6f) || (D3 < -1e-6f);
    const bool bHasPos = (D1 > 1e-6f) || (D2 > 1e-6f) || (D3 > 1e-6f);
    return !(bHasNeg && bHasPos);
}

static float PointToSegmentDistSq(const FVector2f& P, const FVector2f& A, const FVector2f& B)
{
    const FVector2f AB = B - A;
    const float LenSq = AB.SizeSquared();
    if (LenSq <= 1e-10f)
    {
        return DistSq(P, A);
    }
    const float T = FMath::Clamp(((P - A) | AB) / LenSq, 0.0f, 1.0f);
    return DistSq(P, A + AB * T);
}

void FNGonUVDocument::SelectAtUV(const FVector2f& UV, bool bAdd, float Radius)
{
    const float RadiusSq = Radius * Radius;
    switch (SelectionMode)
    {
    case ENGonUVSelectionMode::Corner:
    {
        int32 BestElement = INDEX_NONE;
        float BestDistSq = RadiusSq;
        for (const TPair<int32, FVector2f>& Pair : ElementUV)
        {
            const float D = DistSq(UV, Pair.Value);
            if (D <= BestDistSq)
            {
                BestDistSq = D;
                BestElement = Pair.Key;
            }
        }
        AddElementSelection(BestElement, bAdd);
        break;
    }
    case ENGonUVSelectionMode::Edge:
    {
        const FUVDocumentAccel& Accel = GetUVDocAccel(this);
        int32 BestA = INDEX_NONE;
        int32 BestB = INDEX_NONE;
        float BestEdgeDistSq = RadiusSq;
        for (const FUVCachedEdge& Edge : Accel.PolygonEdges)
        {
            const FVector2f A = GetElementUV(Edge.ElementA);
            const FVector2f B = GetElementUV(Edge.ElementB);
            const float D = PointToSegmentDistSq(UV, A, B);
            if (D <= BestEdgeDistSq)
            {
                BestEdgeDistSq = D;
                BestA = Edge.ElementA;
                BestB = Edge.ElementB;
            }
        }
        if (BestA != INDEX_NONE)
        {
            AddElementSelection(BestA, bAdd);
            AddElementSelection(BestB, true);
        }
        else if (!bAdd)
        {
            ClearSelection();
        }
        break;
    }
    case ENGonUVSelectionMode::Face:
    case ENGonUVSelectionMode::Island:
    {
        int32 HitTri = INDEX_NONE;
        for (int32 I = 0; I < Triangles.Num(); ++I)
        {
            const FTriangle& T = Triangles[I];
            if (IsPointInUVTriangle(UV, T.UV[0], T.UV[1], T.UV[2]))
            {
                HitTri = I;
                break;
            }
        }
        if (HitTri == INDEX_NONE)
        {
            float BestCentroidDistSq = RadiusSq * 4.0f;
            for (int32 I = 0; I < Triangles.Num(); ++I)
            {
                const FTriangle& T = Triangles[I];
                const FVector2f C = (T.UV[0] + T.UV[1] + T.UV[2]) / 3.0f;
                const float D = DistSq(UV, C);
                if (D <= BestCentroidDistSq)
                {
                    BestCentroidDistSq = D;
                    HitTri = I;
                }
            }
        }
        if (Triangles.IsValidIndex(HitTri))
        {
            if (SelectionMode == ENGonUVSelectionMode::Face)
            {
                SelectFaceTriangle(HitTri, bAdd);
            }
            else
            {
                SelectIsland(Triangles[HitTri].Island, bAdd);
            }
        }
        else if (!bAdd)
        {
            ClearSelection();
        }
        break;
    }
    }
}

void FNGonUVDocument::SelectRect(const FBox2D& Rect, bool bAdd)
{
    if (!bAdd)
    {
        ClearSelection();
    }
    const FVector2D MinPt(FMath::Min(Rect.Min.X, Rect.Max.X), FMath::Min(Rect.Min.Y, Rect.Max.Y));
    const FVector2D MaxPt(FMath::Max(Rect.Min.X, Rect.Max.X), FMath::Max(Rect.Min.Y, Rect.Max.Y));
    const FBox2D NormalizedRect(MinPt, MaxPt);

    for (const TPair<int32, FVector2f>& Pair : ElementUV)
    {
        if (NormalizedRect.IsInside(FVector2D(Pair.Value)))
        {
            SelectedElements.Add(Pair.Key);
        }
    }
    for (const int32 ElemId : SelectedElements)
    {
        if (const TArray<int32>* TriList = ElementTriangles.Find(ElemId))
        {
            for (const int32 Tri : *TriList)
            {
                SelectedFaces.Add(Triangles[Tri].FaceId);
                SelectedIslands.Add(Triangles[Tri].Island);
            }
        }
    }
}

void FNGonUVDocument::TranslateSelected(const FVector2f& Delta)
{
    if (SelectedElements.IsEmpty() || Delta.IsNearlyZero(1e-7f))
    {
        return;
    }
    for (const int32 Id : SelectedElements)
    {
        if (FVector2f* UV = ElementUV.Find(Id))
        {
            *UV += Delta;
            Bounds += FVector2D(*UV);
        }
    }
    UpdateTouchedTriangles(SelectedElements, ElementTriangles, ElementUV, Triangles);
}

void FNGonUVDocument::RotateSelected(float Degrees)
{
    if (SelectedElements.IsEmpty())
    {
        return;
    }
    FVector2f Center = FVector2f::ZeroVector;
    for (const int32 Id : SelectedElements)
    {
        Center += GetElementUV(Id);
    }
    Center /= static_cast<float>(SelectedElements.Num());
    const float R = FMath::DegreesToRadians(Degrees);
    const float C = FMath::Cos(R);
    const float S = FMath::Sin(R);
    for (const int32 Id : SelectedElements)
    {
        const FVector2f P = GetElementUV(Id) - Center;
        ElementUV[Id] = Center + FVector2f(P.X * C - P.Y * S, P.X * S + P.Y * C);
    }
    UpdateTouchedTriangles(SelectedElements, ElementTriangles, ElementUV, Triangles);
    RebuildBounds();
}

void FNGonUVDocument::ScaleSelected(float Factor)
{
    if (SelectedElements.IsEmpty())
    {
        return;
    }
    FVector2f Center = FVector2f::ZeroVector;
    for (const int32 Id : SelectedElements)
    {
        Center += GetElementUV(Id);
    }
    Center /= static_cast<float>(SelectedElements.Num());
    for (const int32 Id : SelectedElements)
    {
        ElementUV[Id] = Center + (GetElementUV(Id) - Center) * Factor;
    }
    UpdateTouchedTriangles(SelectedElements, ElementTriangles, ElementUV, Triangles);
    RebuildBounds();
}

void FNGonUVDocument::FlipSelected(bool bHorizontal)
{
    if (SelectedElements.IsEmpty())
    {
        return;
    }
    FVector2f Center = FVector2f::ZeroVector;
    for (const int32 Id : SelectedElements)
    {
        Center += GetElementUV(Id);
    }
    Center /= static_cast<float>(SelectedElements.Num());
    for (const int32 Id : SelectedElements)
    {
        FVector2f P = GetElementUV(Id);
        if (bHorizontal)
        {
            P.X = Center.X - (P.X - Center.X);
        }
        else
        {
            P.Y = Center.Y - (P.Y - Center.Y);
        }
        ElementUV[Id] = P;
    }
    UpdateTouchedTriangles(SelectedElements, ElementTriangles, ElementUV, Triangles);
    RebuildBounds();
}

void FNGonUVDocument::NormalizeSelected()
{
    if (SelectedElements.IsEmpty())
    {
        return;
    }
    FBox2D B(ForceInit);
    for (const int32 Id : SelectedElements)
    {
        B += FVector2D(GetElementUV(Id));
    }
    const FVector2D Size = B.GetSize();
    if (Size.X <= KINDA_SMALL_NUMBER || Size.Y <= KINDA_SMALL_NUMBER)
    {
        return;
    }
    for (const int32 Id : SelectedElements)
    {
        const FVector2D P = FVector2D(GetElementUV(Id));
        ElementUV[Id] = FVector2f(
            static_cast<float>((P.X - B.Min.X) / Size.X),
            static_cast<float>((P.Y - B.Min.Y) / Size.Y));
    }
    UpdateTouchedTriangles(SelectedElements, ElementTriangles, ElementUV, Triangles);
    RebuildBounds();
}

FText FNGonUVDocument::Describe() const
{
    const FUVDocumentAccel& Accel = GetUVDocAccel(this);
    return FText::Format(
        LOCTEXT("UVStatus", "{0} UV corners  |  {1} islands ({2} seam edges)  |  {3} selected"),
        FText::AsNumber(ElementUV.Num()),
        FText::AsNumber(IslandCount),
        FText::AsNumber(Accel.SeamEdgeCount),
        FText::AsNumber(SelectedElements.Num()));
}

void SNGonUVCanvas::Construct(const FArguments&, const TSharedRef<FNGonUVDocument>& InDocument)
{
    Document = InDocument;
}

void SNGonUVCanvas::FitToCanvas()
{
    bNeedsFit = true;
    Invalidate(EInvalidateWidgetReason::Paint);
}

void SNGonUVCanvas::SetSelectionMode(ENGonUVSelectionMode Mode)
{
    Document->SetSelectionMode(Mode);
    Invalidate(EInvalidateWidgetReason::Paint);
}

void SNGonUVCanvas::Refresh()
{
    Invalidate(EInvalidateWidgetReason::Paint);
}

void SNGonUVCanvas::UpdateViewFromBounds(const FGeometry& Geometry) const
{
    if (!bNeedsFit || Document->IsEmpty())
    {
        return;
    }
    const FVector2D Size = Geometry.GetLocalSize();
    if (Size.X <= 1.0 || Size.Y <= 1.0)
    {
        return;
    }
    const FVector2D BSize = Document->GetBounds().GetSize();
    ViewCenter = FVector2f(Document->GetBounds().GetCenter());
    Zoom = FMath::Max(
        40.0f,
        static_cast<float>(FMath::Min(Size.X / FMath::Max(BSize.X, 0.01), Size.Y / FMath::Max(BSize.Y, 0.01)) * 0.82));
    bNeedsFit = false;
}

FVector2f SNGonUVCanvas::ScreenToUV(const FGeometry& Geometry, const FVector2D& Screen) const
{
    UpdateViewFromBounds(Geometry);
    const FVector2D C = Geometry.GetLocalSize() * 0.5;
    return ViewCenter + FVector2f(
        static_cast<float>((Screen.X - C.X) / Zoom),
        static_cast<float>((C.Y - Screen.Y) / Zoom));
}

FVector2D SNGonUVCanvas::UVToScreen(const FGeometry& Geometry, const FVector2f& UV) const
{
    UpdateViewFromBounds(Geometry);
    const FVector2D C = Geometry.GetLocalSize() * 0.5;
    return C + FVector2D((UV.X - ViewCenter.X) * Zoom, -(UV.Y - ViewCenter.Y) * Zoom);
}

int32 SNGonUVCanvas::OnPaint(
    const FPaintArgs& Args,
    const FGeometry& Geo,
    const FSlateRect& Clip,
    FSlateWindowElementList& Out,
    int32 Layer,
    const FWidgetStyle& Style,
    bool Enabled) const
{
    UpdateViewFromBounds(Geo);
    const FVector2D Size = Geo.GetLocalSize();
    static FSlateBrush BoxBrush;
    BoxBrush.DrawAs = ESlateBrushDrawType::Box;

    // 1. Dark UV Canvas Background
    FSlateDrawElement::MakeBox(
        Out,
        Layer,
        Geo.ToPaintGeometry(Size, FSlateLayoutTransform()),
        &BoxBrush,
        ESlateDrawEffect::None,
        FLinearColor(0.032f, 0.038f, 0.048f, 1.0f));

    // 2. Adaptive UV Grid
    TArray<FVector2D> Seg;
    Seg.SetNumUninitialized(2);
    const float Grid = FMath::Max(16.0f, Zoom * 0.25f);
    for (float X = FMath::Fmod(Size.X * 0.5f - ViewCenter.X * Zoom, Grid); X < Size.X; X += Grid)
    {
        if (X >= 0.0f)
        {
            Seg[0] = FVector2D(X, 0.0);
            Seg[1] = FVector2D(X, Size.Y);
            FSlateDrawElement::MakeLines(Out, Layer + 1, Geo.ToPaintGeometry(), Seg, ESlateDrawEffect::None, FLinearColor(0.10f, 0.12f, 0.16f, 1.0f), false, 1.0f);
        }
    }
    for (float Y = FMath::Fmod(Size.Y * 0.5f + ViewCenter.Y * Zoom, Grid); Y < Size.Y; Y += Grid)
    {
        if (Y >= 0.0f)
        {
            Seg[0] = FVector2D(0.0, Y);
            Seg[1] = FVector2D(Size.X, Y);
            FSlateDrawElement::MakeLines(Out, Layer + 1, Geo.ToPaintGeometry(), Seg, ESlateDrawEffect::None, FLinearColor(0.10f, 0.12f, 0.16f, 1.0f), false, 1.0f);
        }
    }

    // 3. Maya-style [0, 1] Primary UV Tile Box & Origin U/V Axes
    const FVector2D Tile00 = UVToScreen(Geo, FVector2f(0.0f, 0.0f));
    const FVector2D Tile10 = UVToScreen(Geo, FVector2f(1.0f, 0.0f));
    const FVector2D Tile11 = UVToScreen(Geo, FVector2f(1.0f, 1.0f));
    const FVector2D Tile01 = UVToScreen(Geo, FVector2f(0.0f, 1.0f));
    FSlateDrawElement::MakeLines(
        Out,
        Layer + 1,
        Geo.ToPaintGeometry(),
        { Tile00, Tile10, Tile11, Tile01, Tile00 },
        ESlateDrawEffect::None,
        FLinearColor(0.24f, 0.30f, 0.38f, 0.90f),
        false,
        1.5f);
    // U axis (red) & V axis (green) along (0,0)->(1,0) and (0,0)->(0,1)
    Seg[0] = Tile00;
    Seg[1] = Tile10;
    FSlateDrawElement::MakeLines(Out, Layer + 1, Geo.ToPaintGeometry(), Seg, ESlateDrawEffect::None, FLinearColor(0.75f, 0.22f, 0.22f, 0.90f), false, 1.8f);
    Seg[0] = Tile00;
    Seg[1] = Tile01;
    FSlateDrawElement::MakeLines(Out, Layer + 1, Geo.ToPaintGeometry(), Seg, ESlateDrawEffect::None, FLinearColor(0.22f, 0.72f, 0.28f, 0.90f), false, 1.8f);

    // 4. Deduplicated N-Gon/Quad Polygon UV Edges (no internal triangulation diagonals, with frustum culling)
    const FUVDocumentAccel& Accel = GetUVDocAccel(&Document.Get());
    const TSet<int32>& SelectedElems = Document->GetSelectedElements();
    const TSet<int32>& SelectedFaces = Document->GetSelectedFaces();

    for (const FUVCachedEdge& Edge : Accel.PolygonEdges)
    {
        const FVector2D A = UVToScreen(Geo, Document->GetElementUV(Edge.ElementA));
        const FVector2D B = UVToScreen(Geo, Document->GetElementUV(Edge.ElementB));

        // Fast viewport frustum culling
        if ((A.X < -4.0 && B.X < -4.0) || (A.X > Size.X + 4.0 && B.X > Size.X + 4.0) ||
            (A.Y < -4.0 && B.Y < -4.0) || (A.Y > Size.Y + 4.0 && B.Y > Size.Y + 4.0))
        {
            continue;
        }

        const bool bEdgeSelected =
            (SelectedElems.Contains(Edge.ElementA) && SelectedElems.Contains(Edge.ElementB)) ||
            (Edge.FaceA != INDEX_NONE && SelectedFaces.Contains(Edge.FaceA)) ||
            (Edge.FaceB != INDEX_NONE && SelectedFaces.Contains(Edge.FaceB));

        Seg[0] = A;
        Seg[1] = B;

        if (bEdgeSelected)
        {
            FSlateDrawElement::MakeLines(
                Out,
                Layer + 3,
                Geo.ToPaintGeometry(),
                Seg,
                ESlateDrawEffect::None,
                FLinearColor(1.0f, 0.65f, 0.12f, 1.0f),
                true,
                2.4f);
        }
        else if (Edge.bIsSeamBorder)
        {
            // Maya-style UV seam / island border highlight
            FSlateDrawElement::MakeLines(
                Out,
                Layer + 2,
                Geo.ToPaintGeometry(),
                Seg,
                ESlateDrawEffect::None,
                FLinearColor(0.28f, 0.88f, 0.66f, 0.95f),
                true,
                1.6f);
        }
        else
        {
            FSlateDrawElement::MakeLines(
                Out,
                Layer + 2,
                Geo.ToPaintGeometry(),
                Seg,
                ESlateDrawEffect::None,
                FLinearColor(0.46f, 0.66f, 0.88f, 0.82f),
                false,
                1.1f);
        }
    }

    // 5. UV Corner Markers (draw unselected dots in Corner mode when visible; always draw selected corners)
    const bool bDrawUnselectedCorners =
        (Document->GetSelectionMode() == ENGonUVSelectionMode::Corner) &&
        (Document->GetCornerCount() <= 8000 || Zoom >= 600.0f);

    if (bDrawUnselectedCorners)
    {
        for (const TPair<int32, FVector2f>& Pair : Document->GetElementUVs())
        {
            if (SelectedElems.Contains(Pair.Key))
            {
                continue;
            }
            const FVector2D P = UVToScreen(Geo, Pair.Value);
            if (P.X < -4.0 || P.X > Size.X + 4.0 || P.Y < -4.0 || P.Y > Size.Y + 4.0)
            {
                continue;
            }
            FSlateDrawElement::MakeBox(
                Out,
                Layer + 4,
                Geo.ToPaintGeometry(FVector2D(5.0, 5.0), FSlateLayoutTransform(P - FVector2D(2.5, 2.5))),
                &BoxBrush,
                ESlateDrawEffect::None,
                FLinearColor(0.20f, 0.82f, 1.0f, 0.90f));
        }
    }

    for (const int32 ElemId : SelectedElems)
    {
        const FVector2D P = UVToScreen(Geo, Document->GetElementUV(ElemId));
        if (P.X < -6.0 || P.X > Size.X + 6.0 || P.Y < -6.0 || P.Y > Size.Y + 6.0)
        {
            continue;
        }
        FSlateDrawElement::MakeBox(
            Out,
            Layer + 5,
            Geo.ToPaintGeometry(FVector2D(7.0, 7.0), FSlateLayoutTransform(P - FVector2D(3.5, 3.5))),
            &BoxBrush,
            ESlateDrawEffect::None,
            FLinearColor(1.0f, 0.62f, 0.10f, 1.0f));
    }

    // 6. Marquee Selection Box
    if (bMarquee)
    {
        const FVector2D A = UVToScreen(Geo, FVector2f(Marquee.Min));
        const FVector2D B = UVToScreen(Geo, FVector2f(Marquee.Max));
        const FVector2D TopLeft(FMath::Min(A.X, B.X), FMath::Min(A.Y, B.Y));
        const FVector2D BoxSize(FMath::Abs(B.X - A.X), FMath::Abs(B.Y - A.Y));
        FSlateDrawElement::MakeBox(
            Out,
            Layer + 6,
            Geo.ToPaintGeometry(BoxSize, FSlateLayoutTransform(TopLeft)),
            &BoxBrush,
            ESlateDrawEffect::None,
            FLinearColor(0.20f, 0.62f, 1.0f, 0.18f));
        FSlateDrawElement::MakeLines(
            Out,
            Layer + 6,
            Geo.ToPaintGeometry(),
            { TopLeft, FVector2D(TopLeft.X + BoxSize.X, TopLeft.Y), TopLeft + BoxSize, FVector2D(TopLeft.X, TopLeft.Y + BoxSize.Y), TopLeft },
            ESlateDrawEffect::None,
            FLinearColor(0.35f, 0.78f, 1.0f, 0.85f),
            false,
            1.0f);
    }
    return Layer + 6;
}

FReply SNGonUVCanvas::OnMouseButtonDown(const FGeometry& Geo, const FPointerEvent& E)
{
    const FVector2D P = Geo.AbsoluteToLocal(E.GetScreenSpacePosition());
    LastMouse = P;
    DragStart = P;
    LastUV = ScreenToUV(Geo, P);
    const FKey Button = E.GetEffectingButton();

    // Maya-style camera pan: Middle mouse, Right mouse, or Alt + Left mouse
    if (Button == EKeys::MiddleMouseButton || Button == EKeys::RightMouseButton ||
        (Button == EKeys::LeftMouseButton && E.IsAltDown()))
    {
        bPanning = true;
        return FReply::Handled().CaptureMouse(AsShared());
    }

    if (Button == EKeys::LeftMouseButton)
    {
        if (E.IsControlDown())
        {
            bMarquee = true;
            Marquee = FBox2D(FVector2D(LastUV), FVector2D(LastUV));
        }
        else
        {
            // Screen-space 10px pick radius converted to UV space
            const float PickRadiusUV = FMath::Clamp(10.0f / FMath::Max(Zoom, 1.0f), 0.002f, 0.08f);
            bool bClickedExistingSelection = false;
            if (!E.IsShiftDown() && !Document->GetSelectedElements().IsEmpty())
            {
                const float DragHitSq = FMath::Square(PickRadiusUV * 1.5f);
                for (const int32 SelId : Document->GetSelectedElements())
                {
                    if (DistSq(LastUV, Document->GetElementUV(SelId)) <= DragHitSq)
                    {
                        bClickedExistingSelection = true;
                        break;
                    }
                }
            }

            if (!bClickedExistingSelection)
            {
                Document->SelectAtUV(LastUV, E.IsShiftDown(), PickRadiusUV);
            }

            if (!Document->GetSelectedElements().IsEmpty())
            {
                bDragging = true;
            }
            else
            {
                // Clicked empty UV space -> start a Maya marquee selection box automatically
                bMarquee = true;
                Marquee = FBox2D(FVector2D(LastUV), FVector2D(LastUV));
            }
        }
        Invalidate(EInvalidateWidgetReason::Paint);
        return FReply::Handled().CaptureMouse(AsShared());
    }
    return FReply::Unhandled();
}

FReply SNGonUVCanvas::OnMouseButtonUp(const FGeometry& Geo, const FPointerEvent& E)
{
    if (bMarquee)
    {
        const FVector2D DragDelta = (Geo.AbsoluteToLocal(E.GetScreenSpacePosition()) - DragStart).GetAbs();
        if (DragDelta.X > 3.0 || DragDelta.Y > 3.0)
        {
            Document->SelectRect(Marquee, E.IsShiftDown());
        }
        bMarquee = false;
    }
    if (bDragging)
    {
        bDragging = false;
    }
    bPanning = false;
    Invalidate(EInvalidateWidgetReason::Paint);
    return FReply::Handled().ReleaseMouseCapture();
}

FReply SNGonUVCanvas::OnMouseMove(const FGeometry& Geo, const FPointerEvent& E)
{
    const FVector2D P = Geo.AbsoluteToLocal(E.GetScreenSpacePosition());
    if (bPanning)
    {
        const FVector2D D = P - LastMouse;
        ViewCenter -= FVector2f(static_cast<float>(D.X / Zoom), static_cast<float>(-D.Y / Zoom));
        LastMouse = P;
        Invalidate(EInvalidateWidgetReason::Paint);
    }
    else if (bMarquee)
    {
        const FVector2f UV = ScreenToUV(Geo, P);
        Marquee.Max = FVector2D(UV);
        Invalidate(EInvalidateWidgetReason::Paint);
    }
    else if (bDragging)
    {
        const FVector2f UV = ScreenToUV(Geo, P);
        Document->TranslateSelected(UV - LastUV);
        LastUV = UV;
        Invalidate(EInvalidateWidgetReason::Paint);
    }
    return FReply::Handled();
}

FReply SNGonUVCanvas::OnMouseWheel(const FGeometry& Geo, const FPointerEvent& E)
{
    const FVector2D Local = Geo.AbsoluteToLocal(E.GetScreenSpacePosition());
    const FVector2f Before = ScreenToUV(Geo, Local);
    Zoom = FMath::Clamp(Zoom * FMath::Pow(1.15f, E.GetWheelDelta()), 20.0f, 15000.0f);
    const FVector2f After = ScreenToUV(Geo, Local);
    ViewCenter += Before - After;
    Invalidate(EInvalidateWidgetReason::Paint);
    return FReply::Handled();
}

void SNGonUVEditor::Construct(const FArguments&)
{
    Document = MakeShared<FNGonUVDocument>();
    Canvas = SNew(SNGonUVCanvas, Document.ToSharedRef());
    SourceLabel = LOCTEXT("NoSource", "No UV mesh loaded");

    TWeakPtr<SNGonUVEditor> WeakEditor = SharedThis(this);
    GetActiveViewportUVState().OnActiveMeshChanged.AddLambda([WeakEditor]()
    {
        if (const TSharedPtr<SNGonUVEditor> Pinned = WeakEditor.Pin())
        {
            Pinned->Reload();
        }
    });

    Reload();
    ChildSlot
    [
        SNew(SVerticalBox)
        + SVerticalBox::Slot().AutoHeight()[MakeToolbar()]
        + SVerticalBox::Slot().FillHeight(1)[Canvas.ToSharedRef()]
        + SVerticalBox::Slot().AutoHeight()[MakeStatus()]
    ];
}

FReply SNGonUVEditor::Reload()
{
    FText Error;
    const FActiveViewportUVState& ActiveState = GetActiveViewportUVState();
    if (ActiveState.bHasActiveMesh)
    {
        SourceLabel = ActiveState.SourceLabel;
        if (Document->Load(ActiveState.MeshData, Error))
        {
            ErrorText = FText::GetEmpty();
        }
        else
        {
            ShowError(Error);
        }
        Canvas->FitToCanvas();
        return FReply::Handled();
    }

    FString Path;
    FNGonMeshData Data;
    if (NGon::FindDefaultFbx(NGon::GetMeshesDirectory(), Path, Error) && NGon::LoadFbx(Path, Data, Error) && Document->Load(Data, Error))
    {
        SourceLabel = FText::FromString(FPaths::GetCleanFilename(Path));
        ErrorText = FText::GetEmpty();
    }
    else if (NGon::MakeDemo(Data, Error) && Document->Load(Data, Error))
    {
        SourceLabel = LOCTEXT("DemoSource", "Demo mesh");
        ErrorText = FText::GetEmpty();
    }
    else
    {
        ShowError(Error);
    }
    Canvas->FitToCanvas();
    return FReply::Handled();
}

void SNGonUVEditor::SetMode(ENGonUVSelectionMode Mode)
{
    Document->SetSelectionMode(Mode);
    Canvas->Refresh();
}

void SNGonUVEditor::ApplyCommand(const TCHAR* Command)
{
    if (FCString::Stricmp(Command, TEXT("Rotate")) == 0)
    {
        Document->RotateSelected(90);
    }
    else if (FCString::Stricmp(Command, TEXT("FlipU")) == 0)
    {
        Document->FlipSelected(true);
    }
    else if (FCString::Stricmp(Command, TEXT("FlipV")) == 0)
    {
        Document->FlipSelected(false);
    }
    else if (FCString::Stricmp(Command, TEXT("Normalize")) == 0)
    {
        Document->NormalizeSelected();
    }
    Canvas->Refresh();
}

TSharedRef<SWidget> SNGonUVEditor::MakeModeButton(ENGonUVSelectionMode Mode, const FText& Label)
{
    return SNew(SButton)
        .Text(Label)
        .OnClicked_Lambda([this, Mode]
        {
            SetMode(Mode);
            return FReply::Handled();
        });
}

TSharedRef<SWidget> SNGonUVEditor::MakeToolbar()
{
    return SNew(SBorder).Padding(6)
    [
        SNew(SWrapBox).UseAllottedSize(true).InnerSlotPadding(FVector2D(4, 2))
        + SWrapBox::Slot()[MakeModeButton(ENGonUVSelectionMode::Corner, LOCTEXT("Corner", "UV Vertex"))]
        + SWrapBox::Slot()[MakeModeButton(ENGonUVSelectionMode::Edge, LOCTEXT("Edge", "UV Edge"))]
        + SWrapBox::Slot()[MakeModeButton(ENGonUVSelectionMode::Face, LOCTEXT("Face", "UV Face"))]
        + SWrapBox::Slot()[MakeModeButton(ENGonUVSelectionMode::Island, LOCTEXT("Island", "UV Shell / Island"))]
        + SWrapBox::Slot()[SNew(SButton).Text(LOCTEXT("Fit", "Fit (F)")).OnClicked_Lambda([this] { Canvas->FitToCanvas(); return FReply::Handled(); })]
        + SWrapBox::Slot()[SNew(SButton).Text(LOCTEXT("Rotate", "Rotate 90")).OnClicked_Lambda([this] { ApplyCommand(TEXT("Rotate")); return FReply::Handled(); })]
        + SWrapBox::Slot()[SNew(SButton).Text(LOCTEXT("FlipU", "Flip U")).OnClicked_Lambda([this] { ApplyCommand(TEXT("FlipU")); return FReply::Handled(); })]
        + SWrapBox::Slot()[SNew(SButton).Text(LOCTEXT("FlipV", "Flip V")).OnClicked_Lambda([this] { ApplyCommand(TEXT("FlipV")); return FReply::Handled(); })]
        + SWrapBox::Slot()[SNew(SButton).Text(LOCTEXT("Normalize", "Normalize 0-1")).OnClicked_Lambda([this] { ApplyCommand(TEXT("Normalize")); return FReply::Handled(); })]
        + SWrapBox::Slot()[SNew(SButton).Text(LOCTEXT("ReloadUV", "Reset to Active Mesh")).OnClicked(this, &SNGonUVEditor::Reload)]
    ];
}

TSharedRef<SWidget> SNGonUVEditor::MakeStatus()
{
    return SNew(SBorder).Padding(6)
    [
        SNew(STextBlock).Text_Lambda([this]
        {
            return ErrorText.IsEmpty()
                ? FText::Format(LOCTEXT("Status", "{0}  |  {1}"), SourceLabel, Document->Describe())
                : FText::Format(LOCTEXT("StatusError", "{0}  |  {1}  |  {2}"), SourceLabel, Document->Describe(), ErrorText);
        })
    ];
}

void SNGonUVEditor::ShowError(const FText& Error)
{
    ErrorText = Error;
}

#undef LOCTEXT_NAMESPACE
// NGon Unified ABI Sync v5.8.2
