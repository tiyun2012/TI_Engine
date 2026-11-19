#include "NGonUVEditor.h"
#include "NGonUVAPI.h"

#include "DynamicMesh/DynamicMesh3.h"
#include "DynamicMesh/DynamicMeshAttributeSet.h"
#include "Framework/Application/SlateApplication.h"
#include "Framework/Docking/TabManager.h"
#include "HAL/PlatformApplicationMisc.h"
#include "Misc/Paths.h"
#include "NGonFbx.h"
#include "NGonMeshFiles.h"
#include "NGonTopology.h"
#include "Rendering/DrawElements.h"
#include "Styling/CoreStyle.h"
#include "Widgets/Docking/SDockTab.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Input/SCheckBox.h"
#include "Widgets/Input/SMultiLineEditableTextBox.h"
#include "Widgets/Input/SSpinBox.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Layout/SScrollBox.h"
#include "Widgets/Layout/SSplitter.h"
#include "Widgets/Layout/SWrapBox.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/Text/STextBlock.h"

using namespace UE::Geometry;

#define LOCTEXT_NAMESPACE "NGonUVEditor"

namespace
{
constexpr double UVGizmoAxisLenPx = 68.0;
constexpr double UVGizmoRingRadiusPx = 56.0;
constexpr double UVGizmoHitRadiusPx = 9.5;
constexpr double UVGizmoCenterHitPx = 11.0;

float DistSq(const FVector2f& A, const FVector2f& B)
{
    return (A - B).SizeSquared();
}

double DistToSegment2D(const FVector2D& P, const FVector2D& A, const FVector2D& B)
{
    const FVector2D AB = B - A;
    const double LenSq = AB.SizeSquared();
    if (LenSq <= 1e-6) return FVector2D::Distance(P, A);
    const double T = FMath::Clamp(FVector2D::DotProduct(P - A, AB) / LenSq, 0.0, 1.0);
    return FVector2D::Distance(P, A + AB * T);
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
                if (E0 == INDEX_NONE || E1 == INDEX_NONE || E0 == E1) continue;
                const int32 MinE = FMath::Min(E0, E1);
                const int32 MaxE = FMath::Max(E0, E1);
                const uint64 Key = (static_cast<uint64>(static_cast<uint32>(MinE)) << 32) | static_cast<uint32>(MaxE);

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
            if (Edge.bIsSeamBorder) ++SeamEdgeCount;
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
            for (const int32 TriIdx : *TriList) TouchedTris.Add(TriIdx);
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

TSharedRef<SWidget> MakeUVSectionCard(
    const FText& Title,
    const FLinearColor& AccentColor,
    const TSharedRef<SWidget>& Content)
{
    return SNew(SBorder)
        .BorderImage(FCoreStyle::Get().GetBrush("WhiteBrush"))
        .BorderBackgroundColor(FLinearColor(0.052f, 0.058f, 0.072f, 1.0f))
        .Padding(FMargin(0.0f))
        [
            SNew(SHorizontalBox)
            + SHorizontalBox::Slot()
            .AutoWidth()
            [
                SNew(SBox)
                .WidthOverride(3.0f)
                [
                    SNew(SBorder)
                    .BorderImage(FCoreStyle::Get().GetBrush("WhiteBrush"))
                    .BorderBackgroundColor(AccentColor)
                ]
            ]
            + SHorizontalBox::Slot()
            .FillWidth(1.0f)
            .Padding(FMargin(8.0f, 5.0f, 8.0f, 6.0f))
            [
                SNew(SVerticalBox)
                + SVerticalBox::Slot()
                .AutoHeight()
                .Padding(0.0f, 0.0f, 0.0f, 4.0f)
                [
                    SNew(STextBlock)
                    .Text(Title)
                    .Font(FCoreStyle::GetDefaultFontStyle("Bold", 8))
                    .ColorAndOpacity(AccentColor)
                ]
                + SVerticalBox::Slot()
                .AutoHeight()
                [
                    Content
                ]
            ]
        ];
}

TSharedRef<SWidget> MakeUVActionChip(
    const FText& Label,
    const FText& Tooltip,
    FOnClicked OnClicked,
    TAttribute<bool> IsEnabled = true)
{
    return SNew(SButton)
        .ContentPadding(FMargin(6.0f, 3.0f))
        .ToolTipText(Tooltip)
        .IsEnabled(IsEnabled)
        .OnClicked(OnClicked)
        [
            SNew(STextBlock)
            .Text(Label)
            .Font(FCoreStyle::GetDefaultFontStyle("Bold", 8))
        ];
}
} // anonymous namespace

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
    if (Topology.RestPositions.IsEmpty() || Topology.Faces.IsEmpty()) return;
    TArray<TArray<int32>> RemainingFaces;
    RemainingFaces.Reserve(Topology.Faces.Num());
    for (const TPair<int32, FNGonFace>& FacePair : Topology.Faces)
    {
        if (FacePair.Value.Perimeter.Num() >= 3) RemainingFaces.Add(FacePair.Value.Perimeter);
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
} // namespace NGon

// ============================================================================
// FNGonUVDocument
// ============================================================================

FNGonUVDocument::~FNGonUVDocument()
{
    FNGonUVAPI::ClearHistory(this);
    GUVDocAccelMap.Remove(this);
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
        if (!UVs->IsSetTriangle(TriangleId)) continue;
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
    FNGonUVAPI::ClearHistory(this);
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
    if (const FVector2f* UV = ElementUV.Find(ElementId)) return *UV;
    return FVector2f::ZeroVector;
}

bool FNGonUVDocument::GetSelectionCentroidUV(FVector2f& OutCenter) const
{
    if (SelectedElements.IsEmpty()) return false;
    FVector2f Sum = FVector2f::ZeroVector;
    int32 Count = 0;
    for (const int32 Id : SelectedElements)
    {
        if (const FVector2f* UV = ElementUV.Find(Id))
        {
            Sum += *UV;
            ++Count;
        }
    }
    if (Count <= 0) return false;
    OutCenter = Sum / static_cast<float>(Count);
    return true;
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
    for (int32 I = 0; I < Parent.Num(); ++I) Parent[I] = I;

    auto Find = [&Parent](int32 I) -> int32
    {
        int32 Root = I;
        while (Parent[Root] != Root) Root = Parent[Root];
        while (I != Root) { const int32 Next = Parent[I]; Parent[I] = Root; I = Next; }
        return Root;
    };
    auto Union = [&Parent, &Find](int32 A, int32 B)
    {
        A = Find(A);
        B = Find(B);
        if (A != B) Parent[B] = A;
    };

    for (const TPair<int32, TArray<int32>>& Pair : ElementTriangles)
    {
        for (int32 I = 1; I < Pair.Value.Num(); ++I) Union(Pair.Value[0], Pair.Value[I]);
    }
    TMap<int32, int32> Remap;
    Remap.Reserve(Triangles.Num());
    IslandCount = 0;
    for (int32 I = 0; I < Triangles.Num(); ++I)
    {
        const int32 Root = Find(I);
        int32* Island = Remap.Find(Root);
        if (!Island) Island = &Remap.Add(Root, IslandCount++);
        Triangles[I].Island = *Island;
    }
}

void FNGonUVDocument::RebuildSelectionMetadata()
{
    SelectedFaces.Reset();
    SelectedIslands.Reset();
    for (const int32 ElemId : SelectedElements)
    {
        if (const TArray<int32>* Tris = ElementTriangles.Find(ElemId))
        {
            for (const int32 Tri : *Tris)
            {
                if (Triangles.IsValidIndex(Tri))
                {
                    SelectedFaces.Add(Triangles[Tri].FaceId);
                    SelectedIslands.Add(Triangles[Tri].Island);
                }
            }
        }
    }
}

void FNGonUVDocument::ClearSelection()
{
    SelectedElements.Reset();
    SelectedFaces.Reset();
    SelectedIslands.Reset();
}

void FNGonUVDocument::SetSelectedElements(const TSet<int32>& InElements, bool bAdd)
{
    if (!bAdd) ClearSelection();
    for (const int32 Id : InElements)
    {
        if (ElementUV.Contains(Id)) SelectedElements.Add(Id);
    }
    RebuildSelectionMetadata();
}

void FNGonUVDocument::SetAllElementUVs(const TMap<int32, FVector2f>& InUVs)
{
    TSet<int32> Changed;
    Changed.Reserve(InUVs.Num());
    for (const TPair<int32, FVector2f>& Pair : InUVs)
    {
        if (FVector2f* Dest = ElementUV.Find(Pair.Key))
        {
            *Dest = Pair.Value;
            Changed.Add(Pair.Key);
        }
    }
    UpdateTouchedTriangles(Changed, ElementTriangles, ElementUV, Triangles);
    RebuildBounds();
}

void FNGonUVDocument::AddElementSelection(int32 Id, bool bAdd)
{
    if (!bAdd) ClearSelection();
    if (Id == INDEX_NONE) return;
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
    if (!Triangles.IsValidIndex(TriangleIndex)) return;
    if (!bAdd) ClearSelection();
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
    if (!bAdd) ClearSelection();
    if (Island < 0) return;
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
    if (LenSq <= 1e-10f) return DistSq(P, A);
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
            if (D <= BestDistSq) { BestDistSq = D; BestElement = Pair.Key; }
        }
        AddElementSelection(BestElement, bAdd);
        break;
    }
    case ENGonUVSelectionMode::Edge:
    {
        const FUVDocumentAccel& Accel = GetUVDocAccel(this);
        int32 BestA = INDEX_NONE, BestB = INDEX_NONE;
        float BestEdgeDistSq = RadiusSq;
        for (const FUVCachedEdge& Edge : Accel.PolygonEdges)
        {
            const float D = PointToSegmentDistSq(UV, GetElementUV(Edge.ElementA), GetElementUV(Edge.ElementB));
            if (D <= BestEdgeDistSq) { BestEdgeDistSq = D; BestA = Edge.ElementA; BestB = Edge.ElementB; }
        }
        if (BestA != INDEX_NONE)
        {
            AddElementSelection(BestA, bAdd);
            AddElementSelection(BestB, true);
        }
        else if (!bAdd) ClearSelection();
        break;
    }
    case ENGonUVSelectionMode::Face:
    case ENGonUVSelectionMode::Island:
    {
        int32 HitTri = INDEX_NONE;
        for (int32 I = 0; I < Triangles.Num(); ++I)
        {
            const FTriangle& T = Triangles[I];
            if (IsPointInUVTriangle(UV, T.UV[0], T.UV[1], T.UV[2])) { HitTri = I; break; }
        }
        if (HitTri == INDEX_NONE)
        {
            float BestCentroidDistSq = RadiusSq * 4.0f;
            for (int32 I = 0; I < Triangles.Num(); ++I)
            {
                const FTriangle& T = Triangles[I];
                const FVector2f C = (T.UV[0] + T.UV[1] + T.UV[2]) / 3.0f;
                const float D = DistSq(UV, C);
                if (D <= BestCentroidDistSq) { BestCentroidDistSq = D; HitTri = I; }
            }
        }
        if (Triangles.IsValidIndex(HitTri))
        {
            if (SelectionMode == ENGonUVSelectionMode::Face) SelectFaceTriangle(HitTri, bAdd);
            else SelectIsland(Triangles[HitTri].Island, bAdd);
        }
        else if (!bAdd) ClearSelection();
        break;
    }
    }
}

void FNGonUVDocument::SelectRect(const FBox2D& Rect, bool bAdd)
{
    if (!bAdd) ClearSelection();
    const FBox2D NormalizedRect(
        FVector2D(FMath::Min(Rect.Min.X, Rect.Max.X), FMath::Min(Rect.Min.Y, Rect.Max.Y)),
        FVector2D(FMath::Max(Rect.Min.X, Rect.Max.X), FMath::Max(Rect.Min.Y, Rect.Max.Y)));

    for (const TPair<int32, FVector2f>& Pair : ElementUV)
    {
        if (NormalizedRect.IsInside(FVector2D(Pair.Value))) SelectedElements.Add(Pair.Key);
    }
    RebuildSelectionMetadata();
}

void FNGonUVDocument::TranslateSelected(const FVector2f& Delta)
{
    if (SelectedElements.IsEmpty() || Delta.IsNearlyZero(1e-7f)) return;
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
    FVector2f Center;
    if (!GetSelectionCentroidUV(Center)) return;
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
    ScaleSelectedNonUniform(FVector2f(Factor, Factor));
}

void FNGonUVDocument::ScaleSelectedNonUniform(const FVector2f& ScaleUV)
{
    FVector2f Center;
    if (!GetSelectionCentroidUV(Center)) return;
    for (const int32 Id : SelectedElements)
    {
        const FVector2f Local = GetElementUV(Id) - Center;
        ElementUV[Id] = Center + FVector2f(Local.X * ScaleUV.X, Local.Y * ScaleUV.Y);
    }
    UpdateTouchedTriangles(SelectedElements, ElementTriangles, ElementUV, Triangles);
    RebuildBounds();
}

void FNGonUVDocument::FlipSelected(bool bHorizontal)
{
    FVector2f Center;
    if (!GetSelectionCentroidUV(Center)) return;
    for (const int32 Id : SelectedElements)
    {
        FVector2f P = GetElementUV(Id);
        if (bHorizontal) P.X = Center.X - (P.X - Center.X);
        else P.Y = Center.Y - (P.Y - Center.Y);
        ElementUV[Id] = P;
    }
    UpdateTouchedTriangles(SelectedElements, ElementTriangles, ElementUV, Triangles);
    RebuildBounds();
}

void FNGonUVDocument::NormalizeSelected()
{
    if (SelectedElements.IsEmpty()) return;
    FBox2D B(ForceInit);
    for (const int32 Id : SelectedElements) B += FVector2D(GetElementUV(Id));
    const FVector2D Size = B.GetSize();
    if (Size.X <= KINDA_SMALL_NUMBER || Size.Y <= KINDA_SMALL_NUMBER) return;
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

// ============================================================================
// SNGonUVCanvas + Interactive 2D UV Transform Gizmo
// ============================================================================

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

void SNGonUVCanvas::SetGizmoMode(ENGonUVGizmoMode Mode)
{
    GizmoMode = Mode;
    HoveredGizmoPart = ENGonUVGizmoPart::None;
    Invalidate(EInvalidateWidgetReason::Paint);
}

void SNGonUVCanvas::Refresh()
{
    Invalidate(EInvalidateWidgetReason::Paint);
}

void SNGonUVCanvas::UpdateViewFromBounds(const FGeometry& Geometry) const
{
    if (!bNeedsFit || Document->IsEmpty()) return;
    const FVector2D Size = Geometry.GetLocalSize();
    if (Size.X <= 1.0 || Size.Y <= 1.0) return;
    const FVector2D BSize = Document->GetBounds().GetSize();
    ViewCenter = FVector2f(Document->GetBounds().GetCenter());
    Zoom = FMath::Max(
        40.0f,
        static_cast<float>(FMath::Min(Size.X / FMath::Max(BSize.X, 0.01), Size.Y / FMath::Max(BSize.Y, 0.01)) * 0.80));
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

ENGonUVGizmoPart SNGonUVCanvas::HitTestGizmo(const FGeometry& Geometry, const FVector2D& LocalPx) const
{
    if (GizmoMode == ENGonUVGizmoMode::None) return ENGonUVGizmoPart::None;
    FVector2f PivotUV;
    if (!Document->GetSelectionCentroidUV(PivotUV)) return ENGonUVGizmoPart::None;

    const FVector2D Origin = UVToScreen(Geometry, PivotUV);
    const double DistToCenter = FVector2D::Distance(LocalPx, Origin);
    const bool bCombined = (GizmoMode == ENGonUVGizmoMode::Combined);
    const bool bTranslate = bCombined || (GizmoMode == ENGonUVGizmoMode::Translate);
    const bool bRotate = bCombined || (GizmoMode == ENGonUVGizmoMode::Rotate);
    const bool bScale = bCombined || (GizmoMode == ENGonUVGizmoMode::Scale);

    // 1. Center handle
    if (bScale && DistToCenter <= UVGizmoCenterHitPx) return ENGonUVGizmoPart::ScaleUniform;
    if (bTranslate && DistToCenter <= UVGizmoCenterHitPx) return ENGonUVGizmoPart::TranslateUV;

    // 2. Translate UV plane box (upper-right quadrant offset)
    if (bTranslate)
    {
        const FVector2D PlaneMin(Origin.X + 14.0, Origin.Y - 30.0);
        const FVector2D PlaneMax(Origin.X + 30.0, Origin.Y - 14.0);
        if (LocalPx.X >= PlaneMin.X && LocalPx.X <= PlaneMax.X && LocalPx.Y >= PlaneMin.Y && LocalPx.Y <= PlaneMax.Y)
        {
            return ENGonUVGizmoPart::TranslateUV;
        }
    }

    // 3. Axis handles (U = right +X, V = up -Y)
    if (bTranslate)
    {
        const FVector2D UTip(Origin.X + UVGizmoAxisLenPx, Origin.Y);
        const FVector2D VTip(Origin.X, Origin.Y - UVGizmoAxisLenPx);
        if (DistToSegment2D(LocalPx, Origin, UTip) <= UVGizmoHitRadiusPx) return ENGonUVGizmoPart::TranslateU;
        if (DistToSegment2D(LocalPx, Origin, VTip) <= UVGizmoHitRadiusPx) return ENGonUVGizmoPart::TranslateV;
    }
    if (bScale)
    {
        const double Len = bCombined ? (UVGizmoAxisLenPx * 0.65) : UVGizmoAxisLenPx;
        const FVector2D UTip(Origin.X + Len, Origin.Y);
        const FVector2D VTip(Origin.X, Origin.Y - Len);
        if (FVector2D::Distance(LocalPx, UTip) <= UVGizmoHitRadiusPx + 2.0 || (!bCombined && DistToSegment2D(LocalPx, Origin, UTip) <= UVGizmoHitRadiusPx))
            return ENGonUVGizmoPart::ScaleU;
        if (FVector2D::Distance(LocalPx, VTip) <= UVGizmoHitRadiusPx + 2.0 || (!bCombined && DistToSegment2D(LocalPx, Origin, VTip) <= UVGizmoHitRadiusPx))
            return ENGonUVGizmoPart::ScaleV;
    }

    // 4. Rotation ring + common interior rotation area (blocks accidental deselection inside ring)
    if (bRotate)
    {
        const double RingR = bCombined ? (UVGizmoRingRadiusPx * 0.85) : UVGizmoRingRadiusPx;
        if (DistToCenter <= RingR + UVGizmoHitRadiusPx)
        {
            return ENGonUVGizmoPart::RotateRing;
        }
    }

    return ENGonUVGizmoPart::None;
}

void SNGonUVCanvas::DrawGizmo2D(const FGeometry& Geo, FSlateWindowElementList& Out, int32 Layer) const
{
    if (GizmoMode == ENGonUVGizmoMode::None) return;
    FVector2f PivotUV;
    if (!Document->GetSelectionCentroidUV(PivotUV)) return;

    static FSlateBrush BoxBrush;
    BoxBrush.DrawAs = ESlateBrushDrawType::Box;

    const FVector2D Origin = UVToScreen(Geo, PivotUV);
    const bool bCombined = (GizmoMode == ENGonUVGizmoMode::Combined);
    const bool bTranslate = bCombined || (GizmoMode == ENGonUVGizmoMode::Translate);
    const bool bRotate = bCombined || (GizmoMode == ENGonUVGizmoMode::Rotate);
    const bool bScale = bCombined || (GizmoMode == ENGonUVGizmoMode::Scale);

    auto IsHigh = [&](ENGonUVGizmoPart Part) -> bool
    {
        return (bGizmoDragging && ActiveGizmoPart == Part) || (!bGizmoDragging && HoveredGizmoPart == Part);
    };

    const FLinearColor HighlightCol(1.0f, 0.84f, 0.24f, 1.0f);
    const FLinearColor RedU(0.92f, 0.28f, 0.25f, 1.0f);
    const FLinearColor GreenV(0.30f, 0.82f, 0.42f, 1.0f);
    const FLinearColor CyanRing(0.28f, 0.78f, 0.98f, 0.95f);

    // 1. Rotation Ring
    if (bRotate)
    {
        const double RingR = bCombined ? (UVGizmoRingRadiusPx * 0.85) : UVGizmoRingRadiusPx;
        const bool bHigh = IsHigh(ENGonUVGizmoPart::RotateRing);
        const FLinearColor Col = bHigh ? HighlightCol : CyanRing;
        constexpr int32 Segs = 48;
        TArray<FVector2D> Pts;
        Pts.Reserve(Segs + 1);
        for (int32 i = 0; i <= Segs; ++i)
        {
            const double A = (2.0 * UE_DOUBLE_PI * i) / Segs;
            Pts.Add(Origin + FVector2D(FMath::Cos(A) * RingR, FMath::Sin(A) * RingR));
        }
        FSlateDrawElement::MakeLines(Out, Layer, Geo.ToPaintGeometry(), Pts, ESlateDrawEffect::None, Col, true, bHigh ? 3.2f : 2.2f);
        if (bGizmoDragging && ActiveGizmoPart == ENGonUVGizmoPart::RotateRing)
        {
            FSlateDrawElement::MakeLines(Out, Layer, Geo.ToPaintGeometry(), { Origin, LastMouse }, ESlateDrawEffect::None, HighlightCol, true, 1.6f);
        }
    }

    // 2. Translate UV Plane Square & Axes
    if (bTranslate)
    {
        const bool bHighPlane = IsHigh(ENGonUVGizmoPart::TranslateUV);
        const FLinearColor PlaneCol = bHighPlane ? HighlightCol : FLinearColor(0.95f, 0.78f, 0.24f, 0.85f);
        FLinearColor FillCol = PlaneCol;
        FillCol.A = bHighPlane ? 0.42f : 0.22f;
        const FVector2D PMin(Origin.X + 14.0, Origin.Y - 30.0);
        FSlateDrawElement::MakeBox(Out, Layer, Geo.ToPaintGeometry(FVector2D(16.0, 16.0), FSlateLayoutTransform(PMin)), &BoxBrush, ESlateDrawEffect::None, FillCol);
        FSlateDrawElement::MakeLines(Out, Layer, Geo.ToPaintGeometry(),
            { PMin, PMin + FVector2D(16, 0), PMin + FVector2D(16, 16), PMin + FVector2D(0, 16), PMin },
            ESlateDrawEffect::None, PlaneCol, false, bHighPlane ? 2.2f : 1.4f);

        // +U Arrow
        const bool bHighU = IsHigh(ENGonUVGizmoPart::TranslateU);
        const FLinearColor ColU = bHighU ? HighlightCol : RedU;
        const FVector2D UTip(Origin.X + UVGizmoAxisLenPx, Origin.Y);
        FSlateDrawElement::MakeLines(Out, Layer + 1, Geo.ToPaintGeometry(), { Origin, UTip }, ESlateDrawEffect::None, ColU, true, bHighU ? 3.4f : 2.4f);
        FSlateDrawElement::MakeLines(Out, Layer + 1, Geo.ToPaintGeometry(),
            { UTip, UTip + FVector2D(-12.0, -5.5), UTip + FVector2D(-12.0, 5.5), UTip }, ESlateDrawEffect::None, ColU, true, 2.2f);

        // +V Arrow
        const bool bHighV = IsHigh(ENGonUVGizmoPart::TranslateV);
        const FLinearColor ColV = bHighV ? HighlightCol : GreenV;
        const FVector2D VTip(Origin.X, Origin.Y - UVGizmoAxisLenPx);
        FSlateDrawElement::MakeLines(Out, Layer + 1, Geo.ToPaintGeometry(), { Origin, VTip }, ESlateDrawEffect::None, ColV, true, bHighV ? 3.4f : 2.4f);
        FSlateDrawElement::MakeLines(Out, Layer + 1, Geo.ToPaintGeometry(),
            { VTip, VTip + FVector2D(-5.5, 12.0), VTip + FVector2D(5.5, 12.0), VTip }, ESlateDrawEffect::None, ColV, true, 2.2f);
    }

    // 3. Scale Handles
    if (bScale)
    {
        const double Len = bCombined ? (UVGizmoAxisLenPx * 0.65) : UVGizmoAxisLenPx;
        const bool bHighSU = IsHigh(ENGonUVGizmoPart::ScaleU);
        const bool bHighSV = IsHigh(ENGonUVGizmoPart::ScaleV);
        const FLinearColor ColSU = bHighSU ? HighlightCol : RedU;
        const FLinearColor ColSV = bHighSV ? HighlightCol : GreenV;
        const FVector2D UTip(Origin.X + Len, Origin.Y);
        const FVector2D VTip(Origin.X, Origin.Y - Len);

        if (!bCombined)
        {
            FSlateDrawElement::MakeLines(Out, Layer + 1, Geo.ToPaintGeometry(), { Origin, UTip }, ESlateDrawEffect::None, ColSU, true, bHighSU ? 3.4f : 2.4f);
            FSlateDrawElement::MakeLines(Out, Layer + 1, Geo.ToPaintGeometry(), { Origin, VTip }, ESlateDrawEffect::None, ColSV, true, bHighSV ? 3.4f : 2.4f);
        }
        FSlateDrawElement::MakeBox(Out, Layer + 2, Geo.ToPaintGeometry(FVector2D(9.0, 9.0), FSlateLayoutTransform(UTip - FVector2D(4.5, 4.5))), &BoxBrush, ESlateDrawEffect::None, ColSU);
        FSlateDrawElement::MakeBox(Out, Layer + 2, Geo.ToPaintGeometry(FVector2D(9.0, 9.0), FSlateLayoutTransform(VTip - FVector2D(4.5, 4.5))), &BoxBrush, ESlateDrawEffect::None, ColSV);
    }

    // 4. Center Pivot Handle
    const bool bHighCenter = IsHigh(ENGonUVGizmoPart::ScaleUniform) || IsHigh(ENGonUVGizmoPart::TranslateUV);
    const FLinearColor CenterCol = bHighCenter ? HighlightCol : FLinearColor(0.92f, 0.94f, 0.98f, 1.0f);
    FSlateDrawElement::MakeBox(
        Out, Layer + 2,
        Geo.ToPaintGeometry(FVector2D(10.0, 10.0), FSlateLayoutTransform(Origin - FVector2D(5.0, 5.0))),
        &BoxBrush, ESlateDrawEffect::None, CenterCol);
}

void SNGonUVCanvas::ApplyGizmoDrag(const FGeometry& Geo, const FVector2D& CurrentLocalPx)
{
    if (!bGizmoDragging || GizmoDragStartUVs.IsEmpty()) return;

    const FVector2D DeltaPx = CurrentLocalPx - DragStart;
    const FVector2f DeltaUV(static_cast<float>(DeltaPx.X / Zoom), static_cast<float>(-DeltaPx.Y / Zoom));
    TMap<int32, FVector2f> Updated = GizmoDragStartUVs;

    if (ActiveGizmoPart == ENGonUVGizmoPart::TranslateUV)
    {
        for (const int32 Id : Document->GetSelectedElements())
        {
            if (FVector2f* P = Updated.Find(Id)) *P += DeltaUV;
        }
    }
    else if (ActiveGizmoPart == ENGonUVGizmoPart::TranslateU)
    {
        for (const int32 Id : Document->GetSelectedElements())
        {
            if (FVector2f* P = Updated.Find(Id)) P->X += DeltaUV.X;
        }
    }
    else if (ActiveGizmoPart == ENGonUVGizmoPart::TranslateV)
    {
        for (const int32 Id : Document->GetSelectedElements())
        {
            if (FVector2f* P = Updated.Find(Id)) P->Y += DeltaUV.Y;
        }
    }
    else if (ActiveGizmoPart == ENGonUVGizmoPart::RotateRing)
    {
        const FVector2D StartVec = DragStart - GizmoDragPivotPx;
        const FVector2D CurrVec = CurrentLocalPx - GizmoDragPivotPx;
        double AngleRad = 0.0;
        if (StartVec.SizeSquared() > 16.0 && CurrVec.SizeSquared() > 16.0)
        {
            // Negate screen-space angle because screen +Y is down while UV +V is up
            AngleRad = -(FMath::Atan2(CurrVec.Y, CurrVec.X) - FMath::Atan2(StartVec.Y, StartVec.X));
        }
        else
        {
            AngleRad = -DeltaPx.X / 80.0;
        }
        const float C = static_cast<float>(FMath::Cos(AngleRad));
        const float S = static_cast<float>(FMath::Sin(AngleRad));
        for (const int32 Id : Document->GetSelectedElements())
        {
            if (FVector2f* P = Updated.Find(Id))
            {
                const FVector2f L = *P - GizmoDragPivotUV;
                *P = GizmoDragPivotUV + FVector2f(L.X * C - L.Y * S, L.X * S + L.Y * C);
            }
        }
    }
    else if (ActiveGizmoPart == ENGonUVGizmoPart::ScaleU || ActiveGizmoPart == ENGonUVGizmoPart::ScaleV || ActiveGizmoPart == ENGonUVGizmoPart::ScaleUniform)
    {
        float SU = 1.0f, SV = 1.0f;
        if (ActiveGizmoPart == ENGonUVGizmoPart::ScaleU)
        {
            SU = FMath::Clamp(1.0f + static_cast<float>(DeltaPx.X / UVGizmoAxisLenPx), 0.05f, 20.0f);
        }
        else if (ActiveGizmoPart == ENGonUVGizmoPart::ScaleV)
        {
            SV = FMath::Clamp(1.0f + static_cast<float>(-DeltaPx.Y / UVGizmoAxisLenPx), 0.05f, 20.0f);
        }
        else
        {
            const float Uni = FMath::Clamp(1.0f + static_cast<float>((DeltaPx.X - DeltaPx.Y) * 0.7071 / UVGizmoAxisLenPx), 0.05f, 20.0f);
            SU = Uni;
            SV = Uni;
        }
        for (const int32 Id : Document->GetSelectedElements())
        {
            if (FVector2f* P = Updated.Find(Id))
            {
                const FVector2f L = *P - GizmoDragPivotUV;
                *P = GizmoDragPivotUV + FVector2f(L.X * SU, L.Y * SV);
            }
        }
    }

    Document->SetAllElementUVs(Updated);
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
        Out, Layer, Geo.ToPaintGeometry(Size, FSlateLayoutTransform()),
        &BoxBrush, ESlateDrawEffect::None, FLinearColor(0.032f, 0.038f, 0.048f, 1.0f));

    // 1b. Optional subtle [0,1] 8x8 Checkerboard Overlay for visual texel alignment
    if (bShowChecker)
    {
        const FVector2D TileMin = UVToScreen(Geo, FVector2f(0.0f, 1.0f));
        const FVector2D TileMax = UVToScreen(Geo, FVector2f(1.0f, 0.0f));
        const FVector2D CellSize = (TileMax - TileMin) / 8.0;
        if (CellSize.X >= 3.0 && CellSize.Y >= 3.0)
        {
            for (int32 r = 0; r < 8; ++r)
            {
                for (int32 c = 0; c < 8; ++c)
                {
                    if (((r + c) & 1) == 0)
                    {
                        const FVector2D Pos = TileMin + FVector2D(c * CellSize.X, r * CellSize.Y);
                        FSlateDrawElement::MakeBox(
                            Out, Layer, Geo.ToPaintGeometry(CellSize, FSlateLayoutTransform(Pos)),
                            &BoxBrush, ESlateDrawEffect::None, FLinearColor(0.055f, 0.064f, 0.080f, 1.0f));
                    }
                }
            }
        }
    }

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

    // 3. [0, 1] Primary UV Tile Box & Origin U/V Axes
    const FVector2D Tile00 = UVToScreen(Geo, FVector2f(0.0f, 0.0f));
    const FVector2D Tile10 = UVToScreen(Geo, FVector2f(1.0f, 0.0f));
    const FVector2D Tile11 = UVToScreen(Geo, FVector2f(1.0f, 1.0f));
    const FVector2D Tile01 = UVToScreen(Geo, FVector2f(0.0f, 1.0f));
    FSlateDrawElement::MakeLines(Out, Layer + 1, Geo.ToPaintGeometry(), { Tile00, Tile10, Tile11, Tile01, Tile00 },
        ESlateDrawEffect::None, FLinearColor(0.28f, 0.35f, 0.44f, 0.92f), false, 1.5f);
    Seg[0] = Tile00; Seg[1] = Tile10;
    FSlateDrawElement::MakeLines(Out, Layer + 1, Geo.ToPaintGeometry(), Seg, ESlateDrawEffect::None, FLinearColor(0.80f, 0.24f, 0.24f, 0.90f), false, 1.8f);
    Seg[0] = Tile00; Seg[1] = Tile01;
    FSlateDrawElement::MakeLines(Out, Layer + 1, Geo.ToPaintGeometry(), Seg, ESlateDrawEffect::None, FLinearColor(0.24f, 0.76f, 0.30f, 0.90f), false, 1.8f);

    // 4. Deduplicated N-Gon/Quad Polygon UV Edges
    const FUVDocumentAccel& Accel = GetUVDocAccel(&Document.Get());
    const TSet<int32>& SelectedElems = Document->GetSelectedElements();
    const TSet<int32>& SelectedFaces = Document->GetSelectedFaces();

    for (const FUVCachedEdge& Edge : Accel.PolygonEdges)
    {
        const FVector2D A = UVToScreen(Geo, Document->GetElementUV(Edge.ElementA));
        const FVector2D B = UVToScreen(Geo, Document->GetElementUV(Edge.ElementB));
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
            FSlateDrawElement::MakeLines(Out, Layer + 3, Geo.ToPaintGeometry(), Seg, ESlateDrawEffect::None, FLinearColor(1.0f, 0.65f, 0.12f, 1.0f), true, 2.4f);
        }
        else if (Edge.bIsSeamBorder)
        {
            FSlateDrawElement::MakeLines(Out, Layer + 2, Geo.ToPaintGeometry(), Seg, ESlateDrawEffect::None, FLinearColor(0.28f, 0.88f, 0.66f, 0.95f), true, 1.6f);
        }
        else
        {
            FSlateDrawElement::MakeLines(Out, Layer + 2, Geo.ToPaintGeometry(), Seg, ESlateDrawEffect::None, FLinearColor(0.46f, 0.66f, 0.88f, 0.82f), false, 1.1f);
        }
    }

    // 5. UV Corner Markers
    const bool bDrawUnselectedCorners =
        (Document->GetSelectionMode() == ENGonUVSelectionMode::Corner) &&
        (Document->GetCornerCount() <= 8000 || Zoom >= 600.0f);

    if (bDrawUnselectedCorners)
    {
        for (const TPair<int32, FVector2f>& Pair : Document->GetElementUVs())
        {
            if (SelectedElems.Contains(Pair.Key)) continue;
            const FVector2D P = UVToScreen(Geo, Pair.Value);
            if (P.X < -4.0 || P.X > Size.X + 4.0 || P.Y < -4.0 || P.Y > Size.Y + 4.0) continue;
            FSlateDrawElement::MakeBox(Out, Layer + 4, Geo.ToPaintGeometry(FVector2D(5.0, 5.0), FSlateLayoutTransform(P - FVector2D(2.5, 2.5))),
                &BoxBrush, ESlateDrawEffect::None, FLinearColor(0.20f, 0.82f, 1.0f, 0.90f));
        }
    }

    for (const int32 ElemId : SelectedElems)
    {
        const FVector2D P = UVToScreen(Geo, Document->GetElementUV(ElemId));
        if (P.X < -6.0 || P.X > Size.X + 6.0 || P.Y < -6.0 || P.Y > Size.Y + 6.0) continue;
        FSlateDrawElement::MakeBox(Out, Layer + 5, Geo.ToPaintGeometry(FVector2D(7.0, 7.0), FSlateLayoutTransform(P - FVector2D(3.5, 3.5))),
            &BoxBrush, ESlateDrawEffect::None, FLinearColor(1.0f, 0.62f, 0.10f, 1.0f));
    }

    // 6. Marquee Selection Box
    if (bMarquee)
    {
        const FVector2D A = UVToScreen(Geo, FVector2f(Marquee.Min));
        const FVector2D B = UVToScreen(Geo, FVector2f(Marquee.Max));
        const FVector2D TopLeft(FMath::Min(A.X, B.X), FMath::Min(A.Y, B.Y));
        const FVector2D BoxSize(FMath::Abs(B.X - A.X), FMath::Abs(B.Y - A.Y));
        FSlateDrawElement::MakeBox(Out, Layer + 6, Geo.ToPaintGeometry(BoxSize, FSlateLayoutTransform(TopLeft)),
            &BoxBrush, ESlateDrawEffect::None, FLinearColor(0.20f, 0.62f, 1.0f, 0.18f));
        FSlateDrawElement::MakeLines(Out, Layer + 6, Geo.ToPaintGeometry(),
            { TopLeft, FVector2D(TopLeft.X + BoxSize.X, TopLeft.Y), TopLeft + BoxSize, FVector2D(TopLeft.X, TopLeft.Y + BoxSize.Y), TopLeft },
            ESlateDrawEffect::None, FLinearColor(0.35f, 0.78f, 1.0f, 0.85f), false, 1.0f);
    }

    // 7. Interactive 2D UV Transform Gizmo (top layer)
    DrawGizmo2D(Geo, Out, Layer + 7);
    return Layer + 10;
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
        return FReply::Handled().CaptureMouse(AsShared()).SetUserFocus(AsShared());
    }

    if (Button == EKeys::LeftMouseButton)
    {
        // Top priority: 2D UV Transform Gizmo (blocks mesh/UV deselection or marquee when cursor is inside gizmo)
        const ENGonUVGizmoPart HitPart = HitTestGizmo(Geo, P);
        if (HitPart != ENGonUVGizmoPart::None && Document->GetSelectionCentroidUV(GizmoDragPivotUV))
        {
            FNGonUVAPI::PushUndoSnapshot(*Document);
            ActiveGizmoPart = HitPart;
            bGizmoDragging = true;
            GizmoDragPivotPx = UVToScreen(Geo, GizmoDragPivotUV);
            GizmoDragStartUVs = Document->GetElementUVs();
            Invalidate(EInvalidateWidgetReason::Paint);
            return FReply::Handled().CaptureMouse(AsShared()).SetUserFocus(AsShared());
        }

        if (E.IsControlDown())
        {
            bMarquee = true;
            Marquee = FBox2D(FVector2D(LastUV), FVector2D(LastUV));
        }
        else
        {
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

            if (!Document->GetSelectedElements().IsEmpty() && GizmoMode == ENGonUVGizmoMode::None)
            {
                FNGonUVAPI::PushUndoSnapshot(*Document);
                bDragging = true;
            }
            else if (Document->GetSelectedElements().IsEmpty())
            {
                bMarquee = true;
                Marquee = FBox2D(FVector2D(LastUV), FVector2D(LastUV));
            }
        }
        Invalidate(EInvalidateWidgetReason::Paint);
        return FReply::Handled().CaptureMouse(AsShared()).SetUserFocus(AsShared());
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
    bGizmoDragging = false;
    ActiveGizmoPart = ENGonUVGizmoPart::None;
    GizmoDragStartUVs.Reset();
    bDragging = false;
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
    else if (bGizmoDragging)
    {
        LastMouse = P;
        ApplyGizmoDrag(Geo, P);
        Invalidate(EInvalidateWidgetReason::Paint);
    }
    else if (bMarquee)
    {
        Marquee.Max = FVector2D(ScreenToUV(Geo, P));
        Invalidate(EInvalidateWidgetReason::Paint);
    }
    else if (bDragging)
    {
        const FVector2f UV = ScreenToUV(Geo, P);
        Document->TranslateSelected(UV - LastUV);
        LastUV = UV;
        Invalidate(EInvalidateWidgetReason::Paint);
    }
    else
    {
        const ENGonUVGizmoPart NewHover = HitTestGizmo(Geo, P);
        if (NewHover != HoveredGizmoPart)
        {
            HoveredGizmoPart = NewHover;
            Invalidate(EInvalidateWidgetReason::Paint);
        }
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

FReply SNGonUVCanvas::OnKeyDown(const FGeometry& MyGeometry, const FKeyEvent& InKeyEvent)
{
    const FKey Key = InKeyEvent.GetKey();
    if (InKeyEvent.IsControlDown())
    {
        if (Key == EKeys::Z)
        {
            FNGonUVAPI::Undo(*Document);
            Refresh();
            return FReply::Handled();
        }
        if (Key == EKeys::Y)
        {
            FNGonUVAPI::Redo(*Document);
            Refresh();
            return FReply::Handled();
        }
        if (Key == EKeys::A)
        {
            FNGonUVAPI::SelectAll(*Document);
            Refresh();
            return FReply::Handled();
        }
    }
    if (Key == EKeys::Q) { SetGizmoMode(ENGonUVGizmoMode::None); return FReply::Handled(); }
    if (Key == EKeys::W) { SetGizmoMode(ENGonUVGizmoMode::Translate); return FReply::Handled(); }
    if (Key == EKeys::E) { SetGizmoMode(ENGonUVGizmoMode::Rotate); return FReply::Handled(); }
    if (Key == EKeys::R) { SetGizmoMode(ENGonUVGizmoMode::Scale); return FReply::Handled(); }
    if (Key == EKeys::F) { FitToCanvas(); return FReply::Handled(); }
    return FReply::Unhandled();
}

// ============================================================================
// SNGonUVEditor (Studio Splitter Layout + UV Studio + AI UV Console)
// ============================================================================

void SNGonUVEditor::Construct(const FArguments&)
{
    Document = MakeShared<FNGonUVDocument>();
    Canvas = SNew(SNGonUVCanvas, Document.ToSharedRef());
    SourceLabel = LOCTEXT("NoSource", "No UV mesh loaded");
    AIJsonInput = TEXT(
        "[\n"
        "  {\"op\": \"relax\", \"iterations\": 4, \"strength\": 0.45, \"pin_border\": true},\n"
        "  {\"op\": \"pack_islands\", \"padding\": 0.02, \"rotate_to_fit\": true},\n"
        "  {\"op\": \"inspect\", \"include_islands\": true}\n"
        "]");
    AILastOutput = TEXT("Ready. Use 2D UV Gizmo (Q/W/E/R), UV Studio tools, or run AI JSON commands.");

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
        + SVerticalBox::Slot().FillHeight(1.0f)
        [
            SNew(SSplitter)
            .Orientation(Orient_Horizontal)
            + SSplitter::Slot()
            .Value(0.68f)
            [
                Canvas.ToSharedRef()
            ]
            + SSplitter::Slot()
            .Value(0.32f)
            [
                SNew(SSplitter)
                .Orientation(Orient_Vertical)
                + SSplitter::Slot()
                .Value(0.52f)
                [
                    SNew(SBorder)
                    .Padding(FMargin(6.0f))
                    [
                        MakeUVStudioPanel()
                    ]
                ]
                + SSplitter::Slot()
                .Value(0.48f)
                [
                    SNew(SBorder)
                    .Padding(FMargin(6.0f))
                    [
                        MakeUVAIConsolePanel()
                    ]
                ]
            ]
        ]
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
        if (Document->Load(ActiveState.MeshData, Error)) ErrorText = FText::GetEmpty();
        else ShowError(Error);
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

void SNGonUVEditor::SetGizmoMode(ENGonUVGizmoMode Mode)
{
    Canvas->SetGizmoMode(Mode);
}

void SNGonUVEditor::ApplyCommand(const TCHAR* Command)
{
    FNGonUVOpResult Res;
    if (FCString::Stricmp(Command, TEXT("Rotate")) == 0) Res = FNGonUVAPI::RotateSelected(*Document, 90.0f);
    else if (FCString::Stricmp(Command, TEXT("FlipU")) == 0) Res = FNGonUVAPI::FlipSelected(*Document, true);
    else if (FCString::Stricmp(Command, TEXT("FlipV")) == 0) Res = FNGonUVAPI::FlipSelected(*Document, false);
    else if (FCString::Stricmp(Command, TEXT("Normalize")) == 0) Res = FNGonUVAPI::NormalizeSelected(*Document, true, PackPadding);
    bAILastOk = Res.bOk;
    AILastOutput = Res.Message;
    Canvas->Refresh();
}

TSharedRef<SWidget> SNGonUVEditor::MakeModeButton(ENGonUVSelectionMode Mode, const FText& Label)
{
    return SNew(SCheckBox)
        .Style(FCoreStyle::Get(), "ToggleButtonCheckbox")
        .Padding(FMargin(7.0f, 3.0f))
        .IsChecked_Lambda([this, Mode]() { return Document->GetSelectionMode() == Mode ? ECheckBoxState::Checked : ECheckBoxState::Unchecked; })
        .OnCheckStateChanged_Lambda([this, Mode](ECheckBoxState) { SetMode(Mode); })
        [
            SNew(STextBlock).Text(Label).Font(FCoreStyle::GetDefaultFontStyle("Bold", 8))
        ];
}

TSharedRef<SWidget> SNGonUVEditor::MakeGizmoModeButton(ENGonUVGizmoMode Mode, const FText& Label, const FText& Tooltip)
{
    return SNew(SCheckBox)
        .Style(FCoreStyle::Get(), "ToggleButtonCheckbox")
        .Padding(FMargin(7.0f, 3.0f))
        .ToolTipText(Tooltip)
        .IsChecked_Lambda([this, Mode]() { return Canvas->GetGizmoMode() == Mode ? ECheckBoxState::Checked : ECheckBoxState::Unchecked; })
        .OnCheckStateChanged_Lambda([this, Mode](ECheckBoxState) { SetGizmoMode(Mode); })
        [
            SNew(STextBlock).Text(Label).Font(FCoreStyle::GetDefaultFontStyle("Bold", 8))
        ];
}

TSharedRef<SWidget> SNGonUVEditor::MakeToolbar()
{
    return SNew(SBorder)
        .BorderImage(FCoreStyle::Get().GetBrush("WhiteBrush"))
        .BorderBackgroundColor(FLinearColor(0.045f, 0.050f, 0.062f, 1.0f))
        .Padding(FMargin(8.0f, 5.0f))
        [
            SNew(SWrapBox)
            .UseAllottedSize(true)
            .InnerSlotPadding(FVector2D(5.0f, 4.0f))
            + SWrapBox::Slot()[MakeUVActionChip(LOCTEXT("UVReload", "Reload"), LOCTEXT("UVReloadTip", "Reload UV mesh from active viewport"), FOnClicked::CreateSP(this, &SNGonUVEditor::Reload))]
            + SWrapBox::Slot()[MakeUVActionChip(LOCTEXT("UVFit", "Fit (F)"), LOCTEXT("UVFitTip", "Fit UV canvas to bounds"), FOnClicked::CreateLambda([this]() { Canvas->FitToCanvas(); return FReply::Handled(); }))]
            + SWrapBox::Slot()[MakeModeButton(ENGonUVSelectionMode::Corner, LOCTEXT("UVCorner", "Corner"))]
            + SWrapBox::Slot()[MakeModeButton(ENGonUVSelectionMode::Edge, LOCTEXT("UVEdge", "Edge"))]
            + SWrapBox::Slot()[MakeModeButton(ENGonUVSelectionMode::Face, LOCTEXT("UVFace", "Face"))]
            + SWrapBox::Slot()[MakeModeButton(ENGonUVSelectionMode::Island, LOCTEXT("UVIsland", "Island"))]
            + SWrapBox::Slot()[MakeGizmoModeButton(ENGonUVGizmoMode::None, LOCTEXT("GizQ", "Q Select"), LOCTEXT("GizQTip", "Direct select/drag without gizmo (Q)"))]
            + SWrapBox::Slot()[MakeGizmoModeButton(ENGonUVGizmoMode::Translate, LOCTEXT("GizW", "W Move"), LOCTEXT("GizWTip", "2D UV Translate Gizmo (W)"))]
            + SWrapBox::Slot()[MakeGizmoModeButton(ENGonUVGizmoMode::Rotate, LOCTEXT("GizE", "E Rotate"), LOCTEXT("GizETip", "2D UV Rotate Ring Gizmo (E)"))]
            + SWrapBox::Slot()[MakeGizmoModeButton(ENGonUVGizmoMode::Scale, LOCTEXT("GizR", "R Scale"), LOCTEXT("GizRTip", "2D UV Scale Gizmo (R)"))]
            + SWrapBox::Slot()[MakeGizmoModeButton(ENGonUVGizmoMode::Combined, LOCTEXT("GizAll", "Combined"), LOCTEXT("GizAllTip", "Combined 2D Translate/Rotate/Scale Gizmo"))]
            + SWrapBox::Slot()
            [
                MakeUVActionChip(
                    LOCTEXT("UVUndo", "Undo"),
                    LOCTEXT("UVUndoTip", "Undo last UV edit (Ctrl+Z)"),
                    FOnClicked::CreateLambda([this]()
                    {
                        const FNGonUVOpResult R = FNGonUVAPI::Undo(*Document);
                        bAILastOk = R.bOk; AILastOutput = R.Message; Canvas->Refresh();
                        return FReply::Handled();
                    }),
                    TAttribute<bool>::CreateLambda([this]() { return FNGonUVAPI::CanUndo(Document.Get()); }))
            ]
            + SWrapBox::Slot()
            [
                MakeUVActionChip(
                    LOCTEXT("UVRedo", "Redo"),
                    LOCTEXT("UVRedoTip", "Redo last undone UV edit (Ctrl+Y)"),
                    FOnClicked::CreateLambda([this]()
                    {
                        const FNGonUVOpResult R = FNGonUVAPI::Redo(*Document);
                        bAILastOk = R.bOk; AILastOutput = R.Message; Canvas->Refresh();
                        return FReply::Handled();
                    }),
                    TAttribute<bool>::CreateLambda([this]() { return FNGonUVAPI::CanRedo(Document.Get()); }))
            ]
            + SWrapBox::Slot()
            [
                SNew(SCheckBox)
                .Style(FCoreStyle::Get(), "ToggleButtonCheckbox")
                .Padding(FMargin(7.0f, 3.0f))
                .IsChecked_Lambda([this]() { return Canvas->IsShowingChecker() ? ECheckBoxState::Checked : ECheckBoxState::Unchecked; })
                .OnCheckStateChanged_Lambda([this](ECheckBoxState) { Canvas->SetShowChecker(!Canvas->IsShowingChecker()); })
                [
                    SNew(STextBlock).Text(LOCTEXT("UVChecker", "Checker")).Font(FCoreStyle::GetDefaultFontStyle("Bold", 8))
                ]
            ]
        ];
}

TSharedRef<SWidget> SNGonUVEditor::MakeUVStudioPanel()
{
    auto RunUVOp = [this](const FNGonUVOpResult& Res, bool bFit = false) -> FReply
    {
        bAILastOk = Res.bOk;
        AILastOutput = Res.PayloadJson.IsEmpty() ? Res.Message : Res.PayloadJson;
        if (bFit && Res.bOk) Canvas->FitToCanvas();
        else Canvas->Refresh();
        return FReply::Handled();
    };

    return SNew(SScrollBox)
        + SScrollBox::Slot().Padding(0.0f, 0.0f, 0.0f, 6.0f)
        [
            MakeUVSectionCard(
                LOCTEXT("UVTransformSec", "TRANSFORM & ALIGN"),
                FLinearColor(0.98f, 0.68f, 0.22f, 1.0f),
                SNew(SWrapBox)
                .UseAllottedSize(true)
                .InnerSlotPadding(FVector2D(4.0f, 4.0f))
                + SWrapBox::Slot()[MakeUVActionChip(LOCTEXT("RotP90", "Rot +90"), LOCTEXT("RotP90Tip", "Rotate selection +90 deg"), FOnClicked::CreateLambda([this, RunUVOp]() { return RunUVOp(FNGonUVAPI::RotateSelected(*Document, 90.0f)); }))]
                + SWrapBox::Slot()[MakeUVActionChip(LOCTEXT("RotM90", "Rot -90"), LOCTEXT("RotM90Tip", "Rotate selection -90 deg"), FOnClicked::CreateLambda([this, RunUVOp]() { return RunUVOp(FNGonUVAPI::RotateSelected(*Document, -90.0f)); }))]
                + SWrapBox::Slot()[MakeUVActionChip(LOCTEXT("FlipUBtn", "Flip U"), LOCTEXT("FlipUTip", "Mirror UVs horizontally"), FOnClicked::CreateLambda([this, RunUVOp]() { return RunUVOp(FNGonUVAPI::FlipSelected(*Document, true)); }))]
                + SWrapBox::Slot()[MakeUVActionChip(LOCTEXT("FlipVBtn", "Flip V"), LOCTEXT("FlipVTip", "Mirror UVs vertically"), FOnClicked::CreateLambda([this, RunUVOp]() { return RunUVOp(FNGonUVAPI::FlipSelected(*Document, false)); }))]
                + SWrapBox::Slot()[MakeUVActionChip(LOCTEXT("AlignL", "Align L"), LOCTEXT("AlignLTip", "Align selected corners to min U"), FOnClicked::CreateLambda([this, RunUVOp]() { return RunUVOp(FNGonUVAPI::AlignSelected(*Document, ENGonUVAlign::Left)); }))]
                + SWrapBox::Slot()[MakeUVActionChip(LOCTEXT("AlignR", "Align R"), LOCTEXT("AlignRTip", "Align selected corners to max U"), FOnClicked::CreateLambda([this, RunUVOp]() { return RunUVOp(FNGonUVAPI::AlignSelected(*Document, ENGonUVAlign::Right)); }))]
                + SWrapBox::Slot()[MakeUVActionChip(LOCTEXT("AlignT", "Align T"), LOCTEXT("AlignTTip", "Align selected corners to max V"), FOnClicked::CreateLambda([this, RunUVOp]() { return RunUVOp(FNGonUVAPI::AlignSelected(*Document, ENGonUVAlign::Top)); }))]
                + SWrapBox::Slot()[MakeUVActionChip(LOCTEXT("AlignB", "Align B"), LOCTEXT("AlignBTip", "Align selected corners to min V"), FOnClicked::CreateLambda([this, RunUVOp]() { return RunUVOp(FNGonUVAPI::AlignSelected(*Document, ENGonUVAlign::Bottom)); }))]
                + SWrapBox::Slot()[MakeUVActionChip(LOCTEXT("StraightenBtn", "Straighten"), LOCTEXT("StraightenTip", "Straighten selected UVs along dominant axis"), FOnClicked::CreateLambda([this, RunUVOp]() { return RunUVOp(FNGonUVAPI::StraightenSelected(*Document)); }))]
                + SWrapBox::Slot()[MakeUVActionChip(LOCTEXT("SnapGridBtn", "Snap Grid"), LOCTEXT("SnapGridTip", "Snap selected UVs to 1/16 grid"), FOnClicked::CreateLambda([this, RunUVOp]() { return RunUVOp(FNGonUVAPI::SnapSelectedToGrid(*Document, SnapStep)); }))])
        ]
        + SScrollBox::Slot().Padding(0.0f, 0.0f, 0.0f, 6.0f)
        [
            MakeUVSectionCard(
                LOCTEXT("UVLayoutSec", "LAYOUT, PACK & RELAX"),
                FLinearColor(0.24f, 0.72f, 1.0f, 1.0f),
                SNew(SVerticalBox)
                + SVerticalBox::Slot().AutoHeight().Padding(0.0f, 0.0f, 0.0f, 5.0f)
                [
                    SNew(SHorizontalBox)
                    + SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0.0f, 0.0f, 5.0f, 0.0f)
                    [
                        SNew(STextBlock).Text(LOCTEXT("PadLbl", "Padding")).Font(FCoreStyle::GetDefaultFontStyle("Regular", 8))
                    ]
                    + SHorizontalBox::Slot().AutoWidth()
                    [
                        SNew(SBox).WidthOverride(64.0f)
                        [
                            SNew(SSpinBox<float>)
                            .MinValue(0.0f).MaxValue(0.15f).Delta(0.005f)
                            .Font(FCoreStyle::GetDefaultFontStyle("Regular", 8))
                            .Value_Lambda([this]() { return PackPadding; })
                            .OnValueChanged_Lambda([this](float V) { PackPadding = V; })
                        ]
                    ]
                ]
                + SVerticalBox::Slot().AutoHeight()
                [
                    SNew(SWrapBox)
                    .UseAllottedSize(true)
                    .InnerSlotPadding(FVector2D(4.0f, 4.0f))
                    + SWrapBox::Slot()[MakeUVActionChip(LOCTEXT("PackBtn", "Pack Islands"), LOCTEXT("PackTip", "Shelf-pack all UV islands cleanly into [0,1]"), FOnClicked::CreateLambda([this, RunUVOp]() { return RunUVOp(FNGonUVAPI::PackIslands(*Document, PackPadding, true), true); }))]
                    + SWrapBox::Slot()[MakeUVActionChip(LOCTEXT("NormBtn", "Normalize 0-1"), LOCTEXT("NormTip", "Normalize selection into [0,1] preserving aspect"), FOnClicked::CreateLambda([this, RunUVOp]() { return RunUVOp(FNGonUVAPI::NormalizeSelected(*Document, true, PackPadding), true); }))]
                    + SWrapBox::Slot()[MakeUVActionChip(LOCTEXT("RelaxBtn", "Relax UVs"), LOCTEXT("RelaxTip", "Laplacian UV relaxation with pinned seam borders"), FOnClicked::CreateLambda([this, RunUVOp]() { return RunUVOp(FNGonUVAPI::RelaxSelected(*Document, 4, 0.45f, true)); }))]
                    + SWrapBox::Slot()[MakeUVActionChip(LOCTEXT("StackBtn", "Stack"), LOCTEXT("StackTip", "Stack selected UV islands onto the same center"), FOnClicked::CreateLambda([this, RunUVOp]() { return RunUVOp(FNGonUVAPI::StackSelectedIslands(*Document)); }))]
                    + SWrapBox::Slot()[MakeUVActionChip(LOCTEXT("DistUBtn", "Distribute U"), LOCTEXT("DistUTip", "Distribute selected islands horizontally"), FOnClicked::CreateLambda([this, RunUVOp]() { return RunUVOp(FNGonUVAPI::DistributeSelectedIslands(*Document, true, PackPadding)); }))]
                    + SWrapBox::Slot()[MakeUVActionChip(LOCTEXT("UDIMNext", "-> UDIM 1002"), LOCTEXT("UDIMTip", "Move selection to UDIM tile (1, 0)"), FOnClicked::CreateLambda([this, RunUVOp]() { return RunUVOp(FNGonUVAPI::MoveToUDIMTile(*Document, 1, 0, false), true); }))]
                ])
        ]
        + SScrollBox::Slot()
        [
            MakeUVSectionCard(
                LOCTEXT("UVSelSec", "SELECTION & AUDIT"),
                FLinearColor(0.18f, 0.86f, 0.88f, 1.0f),
                SNew(SWrapBox)
                .UseAllottedSize(true)
                .InnerSlotPadding(FVector2D(4.0f, 4.0f))
                + SWrapBox::Slot()[MakeUVActionChip(LOCTEXT("UVAll", "All"), LOCTEXT("UVAllTip", "Select all UV corners (Ctrl+A)"), FOnClicked::CreateLambda([this, RunUVOp]() { return RunUVOp(FNGonUVAPI::SelectAll(*Document)); }))]
                + SWrapBox::Slot()[MakeUVActionChip(LOCTEXT("UVInv", "Invert"), LOCTEXT("UVInvTip", "Invert UV selection"), FOnClicked::CreateLambda([this, RunUVOp]() { return RunUVOp(FNGonUVAPI::InvertSelection(*Document)); }))]
                + SWrapBox::Slot()[MakeUVActionChip(LOCTEXT("UVGrow", "Grow"), LOCTEXT("UVGrowTip", "Grow UV selection by 1 triangle ring"), FOnClicked::CreateLambda([this, RunUVOp]() { return RunUVOp(FNGonUVAPI::GrowSelection(*Document)); }))]
                + SWrapBox::Slot()[MakeUVActionChip(LOCTEXT("UVShrink", "Shrink"), LOCTEXT("UVShrinkTip", "Shrink UV selection inward"), FOnClicked::CreateLambda([this, RunUVOp]() { return RunUVOp(FNGonUVAPI::ShrinkSelection(*Document)); }))]
                + SWrapBox::Slot()[MakeUVActionChip(LOCTEXT("UVLinked", "Linked"), LOCTEXT("UVLinkedTip", "Expand selection to full UV island(s)"), FOnClicked::CreateLambda([this, RunUVOp]() { return RunUVOp(FNGonUVAPI::SelectLinkedIslands(*Document)); }))]
                + SWrapBox::Slot()[MakeUVActionChip(LOCTEXT("UVSeams", "Seams"), LOCTEXT("UVSeamsTip", "Select UV island boundary/seam corners"), FOnClicked::CreateLambda([this, RunUVOp]() { return RunUVOp(FNGonUVAPI::SelectSeamCorners(*Document)); }))]
                + SWrapBox::Slot()[MakeUVActionChip(LOCTEXT("UVFlipped", "Flipped"), LOCTEXT("UVFlippedTip", "Select inverted/negative-area UV triangles"), FOnClicked::CreateLambda([this, RunUVOp]() { return RunUVOp(FNGonUVAPI::SelectFlippedTriangles(*Document)); }))])
        ];
}

TSharedRef<SWidget> SNGonUVEditor::MakeUVAIConsolePanel()
{
    return SNew(SVerticalBox)
        + SVerticalBox::Slot().AutoHeight().Padding(0.0f, 0.0f, 0.0f, 5.0f)
        [
            MakeUVSectionCard(
                LOCTEXT("UVAISec", "AI UV API — PRESETS & SCHEMA"),
                FLinearColor(0.28f, 0.85f, 0.62f, 1.0f),
                SNew(SWrapBox)
                .UseAllottedSize(true)
                .InnerSlotPadding(FVector2D(4.0f, 4.0f))
                + SWrapBox::Slot()
                [
                    MakeUVActionChip(
                        LOCTEXT("UVPresetPack", "Auto Pack + Relax"),
                        LOCTEXT("UVPresetPackTip", "Load JSON macro: Relax -> Pack Islands -> Inspect"),
                        FOnClicked::CreateLambda([this]()
                        {
                            AIJsonInput = TEXT(
                                "[\n"
                                "  {\"op\": \"relax\", \"iterations\": 4, \"strength\": 0.45, \"pin_border\": true},\n"
                                "  {\"op\": \"pack_islands\", \"padding\": 0.02, \"rotate_to_fit\": true},\n"
                                "  {\"op\": \"inspect\", \"include_islands\": true}\n"
                                "]");
                            return FReply::Handled();
                        }))
                ]
                + SWrapBox::Slot()
                [
                    MakeUVActionChip(
                        LOCTEXT("UVInspectBtn", "Inspect JSON"),
                        LOCTEXT("UVInspectTip", "Export UV summary & islands JSON to output and clipboard"),
                        FOnClicked::CreateLambda([this]()
                        {
                            const FString Summary = FNGonUVAPI::ExportUVSummaryJson(*Document, true);
                            bAILastOk = true;
                            AILastOutput = Summary;
                            FPlatformApplicationMisc::ClipboardCopy(*Summary);
                            return FReply::Handled();
                        }))
                ]
                + SWrapBox::Slot()
                [
                    MakeUVActionChip(
                        LOCTEXT("UVSchemaBtn", "Copy UV Schema"),
                        LOCTEXT("UVSchemaTip", "Copy complete NGonUVAPI JSON schema to clipboard"),
                        FOnClicked::CreateLambda([this]()
                        {
                            const FString Schema = FNGonUVAPI::GetApiSchemaJson();
                            FPlatformApplicationMisc::ClipboardCopy(*Schema);
                            bAILastOk = true;
                            AILastOutput = TEXT("Copied full NGonUVAPI JSON schema to clipboard.");
                            return FReply::Handled();
                        }))
                ])
        ]
        + SVerticalBox::Slot().FillHeight(0.48f).Padding(0.0f, 0.0f, 0.0f, 5.0f)
        [
            SNew(SMultiLineEditableTextBox)
            .Font(FCoreStyle::GetDefaultFontStyle("Mono", 8))
            .Text_Lambda([this]() { return FText::FromString(AIJsonInput); })
            .OnTextChanged_Lambda([this](const FText& NewText) { AIJsonInput = NewText.ToString(); })
        ]
        + SVerticalBox::Slot().AutoHeight().Padding(0.0f, 0.0f, 0.0f, 5.0f)
        [
            SNew(SHorizontalBox)
            + SHorizontalBox::Slot().AutoWidth().Padding(0.0f, 0.0f, 6.0f, 0.0f)
            [
                SNew(SButton)
                .ButtonColorAndOpacity(FLinearColor(0.18f, 0.65f, 0.45f, 1.0f))
                .ContentPadding(FMargin(10.0f, 3.0f))
                .OnClicked_Lambda([this]()
                {
                    const FNGonUVOpResult Res = FNGonUVAPI::ExecuteJsonCommand(*Document, AIJsonInput);
                    bAILastOk = Res.bOk;
                    AILastOutput = Res.PayloadJson.IsEmpty() ? Res.ToJsonString() : Res.PayloadJson;
                    Canvas->Refresh();
                    return FReply::Handled();
                })
                [
                    SNew(STextBlock).Text(LOCTEXT("RunUVJson", "Run UV JSON")).Font(FCoreStyle::GetDefaultFontStyle("Bold", 8))
                ]
            ]
            + SHorizontalBox::Slot().AutoWidth()
            [
                MakeUVActionChip(
                    LOCTEXT("CopyUVOut", "Copy Output"),
                    LOCTEXT("CopyUVOutTip", "Copy latest UV JSON output to clipboard"),
                    FOnClicked::CreateLambda([this]()
                    {
                        FPlatformApplicationMisc::ClipboardCopy(*AILastOutput);
                        return FReply::Handled();
                    }))
            ]
        ]
        + SVerticalBox::Slot().FillHeight(0.52f)
        [
            SNew(SBorder)
            .BorderImage(FCoreStyle::Get().GetBrush("WhiteBrush"))
            .BorderBackgroundColor(FLinearColor(0.032f, 0.036f, 0.046f, 1.0f))
            .Padding(FMargin(7.0f, 5.0f))
            [
                SNew(SScrollBox)
                + SScrollBox::Slot()
                [
                    SNew(STextBlock)
                    .AutoWrapText(true)
                    .Font(FCoreStyle::GetDefaultFontStyle("Mono", 8))
                    .ColorAndOpacity_Lambda([this]()
                    {
                        return bAILastOk
                            ? FLinearColor(0.62f, 0.92f, 0.74f, 1.0f)
                            : FLinearColor(1.0f, 0.48f, 0.45f, 1.0f);
                    })
                    .Text_Lambda([this]() { return FText::FromString(AILastOutput); })
                ]
            ]
        ];
}

TSharedRef<SWidget> SNGonUVEditor::MakeStatus()
{
    return SNew(SBorder)
        .BorderImage(FCoreStyle::Get().GetBrush("WhiteBrush"))
        .BorderBackgroundColor(FLinearColor(0.042f, 0.048f, 0.058f, 1.0f))
        .Padding(FMargin(8.0f, 4.0f))
        [
            SNew(SHorizontalBox)
            + SHorizontalBox::Slot().FillWidth(1.0f)
            [
                SNew(STextBlock)
                .Font(FCoreStyle::GetDefaultFontStyle("Regular", 8))
                .Text_Lambda([this]()
                {
                    if (!ErrorText.IsEmpty()) return ErrorText;
                    return FText::Format(LOCTEXT("UVStatusLine", "{0}  |  {1}  |  Hotkeys: Q/W/E/R Gizmo, F Fit, Alt+LMB/MMB Pan, Ctrl+Z/Y Undo"),
                        SourceLabel, Document->Describe());
                })
            ]
        ];
}

void SNGonUVEditor::ShowError(const FText& Error)
{
    ErrorText = Error;
}

#undef LOCTEXT_NAMESPACE
// NGon Unified ABI Sync v5.8.2
