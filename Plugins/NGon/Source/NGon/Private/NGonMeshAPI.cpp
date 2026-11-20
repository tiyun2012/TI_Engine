#include "NGonMeshAPI.h"
#include "Algo/Reverse.h"
#include "NGonPreviewScene.h"
#include "NGonUndoTree.h"
#include "NGonSceneSkeleton.h"

#include "Components/DynamicMeshComponent.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "UDynamicMesh.h"

namespace NGon
{
void UpdateActiveViewportUVMeshFromTopology(const FNGonTopology& Topology);
}

namespace
{
struct FNGonMeshSnapshot
{
    TArray<FVector> Positions;
    TArray<TArray<int32>> Faces;
    ENGonElement SelectedElement = ENGonElement::Face;
    TSet<int32> SelectedIds;
};

struct FNGonHistoryBuffer
{
    static constexpr int32 MaxHistory = 20;
    TArray<FNGonMeshSnapshot> UndoStack;
    TArray<FNGonMeshSnapshot> RedoStack;
};

TMap<const FNGonPreviewScene*, FNGonHistoryBuffer> GMeshHistoryMap;

FNGonMeshSnapshot CaptureSnapshot(const FNGonPreviewScene& Scene)
{
    FNGonMeshSnapshot Snap;
    Snap.Positions = Scene.GetPositions();
    Snap.SelectedElement = Scene.GetSelectedElement();
    Snap.SelectedIds = Scene.GetSelectedIds();

    const FNGonTopology& Topo = Scene.GetTopology();
    TArray<int32> FaceKeys;
    Topo.Faces.GetKeys(FaceKeys);
    FaceKeys.Sort();
    Snap.Faces.Reserve(FaceKeys.Num());
    for (const int32 FaceId : FaceKeys)
    {
        if (const FNGonFace* Face = Topo.Faces.Find(FaceId))
        {
            if (Face->Perimeter.Num() >= 3)
            {
                Snap.Faces.Add(Face->Perimeter);
            }
        }
    }
    return Snap;
}

void PushUndoSnapshot(const FNGonPreviewScene& Scene)
{
    FNGonHistoryBuffer& Hist = GMeshHistoryMap.FindOrAdd(&Scene);
    Hist.UndoStack.Add(CaptureSnapshot(Scene));
    if (Hist.UndoStack.Num() > FNGonHistoryBuffer::MaxHistory)
    {
        Hist.UndoStack.RemoveAt(0);
    }
    Hist.RedoStack.Reset();
}

void CompactMeshData(
    TArray<FVector>& InOutPositions,
    TArray<TArray<int32>>& InOutFaces,
    TArray<int32>* OutOldToNewVertex = nullptr)
{
    TArray<int32> Remap;
    Remap.Init(INDEX_NONE, InOutPositions.Num());
    TArray<FVector> NewPositions;
    NewPositions.Reserve(InOutPositions.Num());

    TArray<TArray<int32>> CleanFaces;
    CleanFaces.Reserve(InOutFaces.Num());

    for (const TArray<int32>& Face : InOutFaces)
    {
        TArray<int32> Dedup;
        Dedup.Reserve(Face.Num());
        for (const int32 V : Face)
        {
            if (!InOutPositions.IsValidIndex(V))
            {
                continue;
            }
            if (Dedup.IsEmpty() || Dedup.Last() != V)
            {
                Dedup.Add(V);
            }
        }
        if (Dedup.Num() >= 2 && Dedup[0] == Dedup.Last())
        {
            Dedup.Pop();
        }
        TSet<int32> Seen;
        TArray<int32> UniqueLoop;
        UniqueLoop.Reserve(Dedup.Num());
        for (const int32 V : Dedup)
        {
            if (!Seen.Contains(V))
            {
                Seen.Add(V);
                UniqueLoop.Add(V);
            }
        }
        if (UniqueLoop.Num() < 3)
        {
            continue;
        }
        // Check non-degenerate area
        const FVector& O = InOutPositions[UniqueLoop[0]];
        FVector AreaVec = FVector::ZeroVector;
        for (int32 i = 1; i + 1 < UniqueLoop.Num(); ++i)
        {
            AreaVec += FVector::CrossProduct(InOutPositions[UniqueLoop[i]] - O, InOutPositions[UniqueLoop[i + 1]] - O);
        }
        if (!FMath::IsFinite(AreaVec.SizeSquared()) || AreaVec.SizeSquared() <= 1e-12)
        {
            continue;
        }

        TArray<int32> RemappedFace;
        RemappedFace.Reserve(UniqueLoop.Num());
        for (const int32 V : UniqueLoop)
        {
            if (Remap[V] == INDEX_NONE)
            {
                Remap[V] = NewPositions.Add(InOutPositions[V]);
            }
            RemappedFace.Add(Remap[V]);
        }
        CleanFaces.Add(MoveTemp(RemappedFace));
    }

    InOutPositions = MoveTemp(NewPositions);
    InOutFaces = MoveTemp(CleanFaces);
    if (OutOldToNewVertex)
    {
        *OutOldToNewVertex = MoveTemp(Remap);
    }
}

FVector ComputePolygonNormal(const TArray<FVector>& Positions, const TArray<int32>& Perimeter)
{
    if (Perimeter.Num() < 3 || !Positions.IsValidIndex(Perimeter[0]))
    {
        return FVector::UpVector;
    }
    const FVector& O = Positions[Perimeter[0]];
    FVector Sum = FVector::ZeroVector;
    for (int32 i = 1; i + 1 < Perimeter.Num(); ++i)
    {
        if (Positions.IsValidIndex(Perimeter[i]) && Positions.IsValidIndex(Perimeter[i + 1]))
        {
            Sum += FVector::CrossProduct(Positions[Perimeter[i]] - O, Positions[Perimeter[i + 1]] - O);
        }
    }
    return Sum.IsNearlyZero() ? FVector::UpVector : Sum.GetSafeNormal();
}

FVector ComputePolygonCentroid(const TArray<FVector>& Positions, const TArray<int32>& Perimeter)
{
    FVector Sum = FVector::ZeroVector;
    int32 Count = 0;
    for (const int32 V : Perimeter)
    {
        if (Positions.IsValidIndex(V))
        {
            Sum += Positions[V];
            ++Count;
        }
    }
    return Count > 0 ? (Sum / static_cast<double>(Count)) : FVector::ZeroVector;
}

TArray<int32> ResolveTargetFaces(const FNGonPreviewScene& Scene, const TArray<int32>& RequestedFaces)
{
    TArray<int32> Result;
    const FNGonTopology& Topo = Scene.GetTopology();
    if (!RequestedFaces.IsEmpty())
    {
        for (const int32 Id : RequestedFaces)
        {
            if (Topo.Faces.Contains(Id))
            {
                Result.AddUnique(Id);
            }
        }
        return Result;
    }

    if (Scene.GetSelectedElement() == ENGonElement::Face && !Scene.GetSelectedIds().IsEmpty())
    {
        for (const int32 Id : Scene.GetSelectedIds())
        {
            if (Topo.Faces.Contains(Id))
            {
                Result.AddUnique(Id);
            }
        }
        Result.Sort();
        return Result;
    }

    return Result;
}

TArray<int32> ResolveTargetVertices(const FNGonPreviewScene& Scene, const TArray<int32>& RequestedVertices)
{
    TArray<int32> Result;
    const TArray<FVector>& Positions = Scene.GetPositions();
    if (!RequestedVertices.IsEmpty())
    {
        for (const int32 V : RequestedVertices)
        {
            if (Positions.IsValidIndex(V))
            {
                Result.AddUnique(V);
            }
        }
        return Result;
    }

    const FNGonTopology& Topo = Scene.GetTopology();
    const ENGonElement Elem = Scene.GetSelectedElement();
    for (const int32 Id : Scene.GetSelectedIds())
    {
        if (Elem == ENGonElement::Vertex && Positions.IsValidIndex(Id))
        {
            Result.AddUnique(Id);
        }
        else if (Elem == ENGonElement::Edge && Topo.Edges.IsValidIndex(Id))
        {
            if (Positions.IsValidIndex(Topo.Edges[Id].X)) Result.AddUnique(Topo.Edges[Id].X);
            if (Positions.IsValidIndex(Topo.Edges[Id].Y)) Result.AddUnique(Topo.Edges[Id].Y);
        }
        else if (Elem == ENGonElement::Face)
        {
            if (const FNGonFace* Face = Topo.Faces.Find(Id))
            {
                for (const int32 V : Face->Perimeter)
                {
                    if (Positions.IsValidIndex(V)) Result.AddUnique(V);
                }
            }
        }
    }
    Result.Sort();
    return Result;
}

bool CommitPolygonMesh(
    FNGonPreviewScene& Scene,
    const TArray<FVector>& Positions,
    const TArray<TArray<int32>>& Faces,
    bool bRecordUndo,
    FText& OutError)
{
    const bool bHasBefore = !Scene.GetPositions().IsEmpty() && !Scene.GetTopology().Faces.IsEmpty();
    FNGonMeshSnapshot BeforeSnap;
    if (bRecordUndo && bHasBefore)
    {
        BeforeSnap = CaptureSnapshot(Scene);
        PushUndoSnapshot(Scene);
    }

    if (!Scene.ReplacePolygonMesh(Positions, Faces, OutError))
    {
        if (bRecordUndo && bHasBefore)
        {
            if (FNGonHistoryBuffer* Hist = GMeshHistoryMap.Find(&Scene); Hist && !Hist->UndoStack.IsEmpty())
            {
                Hist->UndoStack.Pop();
            }
        }
        return false;
    }

    NGon::UpdateActiveViewportUVMeshFromTopology(Scene.GetTopology());

    if (bRecordUndo && bHasBefore)
    {
        if (const TSharedPtr<FNGonUndoSession> Session = NGon::Undo::FindSession(&Scene); Session.IsValid() && !Session->IsApplyingHistory())
        {
            const FNGonMeshSnapshot AfterSnap = CaptureSnapshot(Scene);
            const bool bStructural = (BeforeSnap.Positions.Num() != AfterSnap.Positions.Num() || BeforeSnap.Faces.Num() != AfterSnap.Faces.Num());
            const FString Label = bStructural
                ? FString::Printf(TEXT("Mesh Edit (%d -> %d faces)"), BeforeSnap.Faces.Num(), AfterSnap.Faces.Num())
                : FString::Printf(TEXT("Mesh Deform (%d verts)"), AfterSnap.Positions.Num());
            const SIZE_T ApproxBytes = static_cast<SIZE_T>(BeforeSnap.Positions.Num() + AfterSnap.Positions.Num()) * sizeof(FVector)
                                     + static_cast<SIZE_T>(BeforeSnap.Faces.Num() + AfterSnap.Faces.Num()) * 32ull;
            FNGonPreviewScene* ScenePtr = &Scene;
            TSharedPtr<INGonUndoPayload> Payload = MakeShared<FNGonLambdaUndoPayload>(
                [ScenePtr, BeforeSnap]() -> bool
                {
                    FText Err;
                    if (!CommitPolygonMesh(*ScenePtr, BeforeSnap.Positions, BeforeSnap.Faces, false, Err)) return false;
                    ScenePtr->SetSelectionSet(BeforeSnap.SelectedElement, BeforeSnap.SelectedIds, false);
                    return true;
                },
                [ScenePtr, AfterSnap]() -> bool
                {
                    FText Err;
                    if (!CommitPolygonMesh(*ScenePtr, AfterSnap.Positions, AfterSnap.Faces, false, Err)) return false;
                    ScenePtr->SetSelectionSet(AfterSnap.SelectedElement, AfterSnap.SelectedIds, false);
                    return true;
                },
                ApproxBytes);
            Session->PushEdit(
                ENGonUndoContextId::Mesh,
                Label,
                bStructural ? ENGonUndoEditKind::Structural : ENGonUndoEditKind::Value,
                MoveTemp(Payload),
                ApproxBytes);
        }
    }
    return true;
}

TSharedRef<FJsonObject> MakeVectorJson(const FVector& V)
{
    TSharedRef<FJsonObject> Obj = MakeShared<FJsonObject>();
    Obj->SetNumberField(TEXT("x"), V.X);
    Obj->SetNumberField(TEXT("y"), V.Y);
    Obj->SetNumberField(TEXT("z"), V.Z);
    return Obj;
}

TArray<TSharedPtr<FJsonValue>> MakeIntArrayJson(const TArray<int32>& Arr)
{
    TArray<TSharedPtr<FJsonValue>> Out;
    Out.Reserve(Arr.Num());
    for (const int32 V : Arr)
    {
        Out.Add(MakeShared<FJsonValueNumber>(V));
    }
    return Out;
}

bool ReadVectorFromJson(const TSharedPtr<FJsonObject>& Obj, const FString& Field, FVector& OutVec, const FVector& Default)
{
    OutVec = Default;
    if (!Obj.IsValid() || !Obj->HasField(Field))
    {
        return false;
    }
    const TArray<TSharedPtr<FJsonValue>>* Arr = nullptr;
    if (Obj->TryGetArrayField(Field, Arr) && Arr && Arr->Num() >= 3)
    {
        OutVec.X = (*Arr)[0]->AsNumber();
        OutVec.Y = (*Arr)[1]->AsNumber();
        OutVec.Z = (*Arr)[2]->AsNumber();
        return true;
    }
    const TSharedPtr<FJsonObject>* Sub = nullptr;
    if (Obj->TryGetObjectField(Field, Sub) && Sub && Sub->IsValid())
    {
        (*Sub)->TryGetNumberField(TEXT("x"), OutVec.X);
        (*Sub)->TryGetNumberField(TEXT("y"), OutVec.Y);
        (*Sub)->TryGetNumberField(TEXT("z"), OutVec.Z);
        return true;
    }
    return false;
}

TArray<int32> ReadIntArrayFromJson(const TSharedPtr<FJsonObject>& Obj, const FString& Field)
{
    TArray<int32> Out;
    if (!Obj.IsValid()) return Out;
    const TArray<TSharedPtr<FJsonValue>>* Arr = nullptr;
    if (Obj->TryGetArrayField(Field, Arr) && Arr)
    {
        Out.Reserve(Arr->Num());
        for (const TSharedPtr<FJsonValue>& Val : *Arr)
        {
            if (Val.IsValid())
            {
                Out.Add(static_cast<int32>(FMath::RoundToInt(Val->AsNumber())));
            }
        }
    }
    return Out;
}

EAxis::Type ParseAxisString(const FString& Str, EAxis::Type Default = EAxis::Z)
{
    const FString Upper = Str.ToUpper();
    if (Upper == TEXT("X")) return EAxis::X;
    if (Upper == TEXT("Y")) return EAxis::Y;
    if (Upper == TEXT("Z")) return EAxis::Z;
    return Default;
}

ENGonElement ParseElementString(const FString& Str, ENGonElement Default = ENGonElement::Face)
{
    const FString Lower = Str.ToLower();
    if (Lower == TEXT("vertex") || Lower == TEXT("vertices") || Lower == TEXT("vtx")) return ENGonElement::Vertex;
    if (Lower == TEXT("edge") || Lower == TEXT("edges")) return ENGonElement::Edge;
    if (Lower == TEXT("face") || Lower == TEXT("faces") || Lower == TEXT("polygon")) return ENGonElement::Face;
    return Default;
}

FString SerializeJsonObject(const TSharedRef<FJsonObject>& Obj, bool bPretty = true)
{
    FString Output;
    if (bPretty)
    {
        const TSharedRef<TJsonWriter<TCHAR, TPrettyJsonPrintPolicy<TCHAR>>> Writer =
            TJsonWriterFactory<TCHAR, TPrettyJsonPrintPolicy<TCHAR>>::Create(&Output);
        FJsonSerializer::Serialize(Obj, Writer);
    }
    else
    {
        const TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> Writer =
            TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Output);
        FJsonSerializer::Serialize(Obj, Writer);
    }
    return Output;
}

double DeterministicHashNoise(const FVector& P, int32 Seed)
{
    const double Val = FMath::Sin(P.X * 12.9898 + P.Y * 78.233 + P.Z * 37.719 + static_cast<double>(Seed) * 0.137) * 43758.5453;
    const double Frac = Val - FMath::FloorToDouble(Val);
    return Frac * 2.0 - 1.0;
}
} // anonymous namespace

FString FNGonMeshOpResult::ToJsonString() const
{
    TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
    Root->SetBoolField(TEXT("ok"), bOk);
    Root->SetStringField(TEXT("op"), Operation);
    Root->SetStringField(TEXT("message"), Message);
    if (!CreatedVertices.IsEmpty()) Root->SetArrayField(TEXT("created_vertices"), MakeIntArrayJson(CreatedVertices));
    if (!CreatedFaces.IsEmpty()) Root->SetArrayField(TEXT("created_faces"), MakeIntArrayJson(CreatedFaces));
    if (!ModifiedVertices.IsEmpty()) Root->SetArrayField(TEXT("modified_vertices"), MakeIntArrayJson(ModifiedVertices));
    if (!DeletedFaces.IsEmpty()) Root->SetArrayField(TEXT("deleted_faces"), MakeIntArrayJson(DeletedFaces));
    if (!SelectedIds.IsEmpty()) Root->SetArrayField(TEXT("selected_ids"), MakeIntArrayJson(SelectedIds));
    if (!PayloadJson.IsEmpty()) Root->SetStringField(TEXT("payload"), PayloadJson);
    return SerializeJsonObject(Root, true);
}

// ============================================================================
// History (Undo / Redo)
// ============================================================================

void FNGonMeshAPI::ClearHistory(const FNGonPreviewScene* Scene)
{
    GMeshHistoryMap.Remove(Scene);
}

bool FNGonMeshAPI::CanUndo(const FNGonPreviewScene* Scene)
{
    const FNGonHistoryBuffer* Hist = GMeshHistoryMap.Find(Scene);
    return Hist && !Hist->UndoStack.IsEmpty();
}

bool FNGonMeshAPI::CanRedo(const FNGonPreviewScene* Scene)
{
    const FNGonHistoryBuffer* Hist = GMeshHistoryMap.Find(Scene);
    return Hist && !Hist->RedoStack.IsEmpty();
}

int32 FNGonMeshAPI::GetUndoCount(const FNGonPreviewScene* Scene)
{
    const FNGonHistoryBuffer* Hist = GMeshHistoryMap.Find(Scene);
    return Hist ? Hist->UndoStack.Num() : 0;
}

int32 FNGonMeshAPI::GetRedoCount(const FNGonPreviewScene* Scene)
{
    const FNGonHistoryBuffer* Hist = GMeshHistoryMap.Find(Scene);
    return Hist ? Hist->RedoStack.Num() : 0;
}

FNGonMeshOpResult FNGonMeshAPI::Undo(FNGonPreviewScene& Scene)
{
    FNGonMeshOpResult Res;
    Res.Operation = TEXT("undo");
    FNGonHistoryBuffer* Hist = GMeshHistoryMap.Find(&Scene);
    if (!Hist || Hist->UndoStack.IsEmpty())
    {
        Res.Message = TEXT("Nothing to undo.");
        return Res;
    }

    Hist->RedoStack.Add(CaptureSnapshot(Scene));
    FNGonMeshSnapshot Prev = Hist->UndoStack.Pop();
    FText Error;
    if (!CommitPolygonMesh(Scene, Prev.Positions, Prev.Faces, false, Error))
    {
        Res.Message = Error.ToString();
        return Res;
    }
    Scene.SetSelectionSet(Prev.SelectedElement, Prev.SelectedIds, false);
    Res.bOk = true;
    Res.Message = FString::Printf(TEXT("Undid last mesh edit (%d undo steps remaining)."), Hist->UndoStack.Num());
    return Res;
}

FNGonMeshOpResult FNGonMeshAPI::Redo(FNGonPreviewScene& Scene)
{
    FNGonMeshOpResult Res;
    Res.Operation = TEXT("redo");
    FNGonHistoryBuffer* Hist = GMeshHistoryMap.Find(&Scene);
    if (!Hist || Hist->RedoStack.IsEmpty())
    {
        Res.Message = TEXT("Nothing to redo.");
        return Res;
    }

    Hist->UndoStack.Add(CaptureSnapshot(Scene));
    FNGonMeshSnapshot Next = Hist->RedoStack.Pop();
    FText Error;
    if (!CommitPolygonMesh(Scene, Next.Positions, Next.Faces, false, Error))
    {
        Res.Message = Error.ToString();
        return Res;
    }
    Scene.SetSelectionSet(Next.SelectedElement, Next.SelectedIds, false);
    Res.bOk = true;
    Res.Message = FString::Printf(TEXT("Redid mesh edit (%d redo steps remaining)."), Hist->RedoStack.Num());
    return Res;
}

// ============================================================================
// Procedural Primitives
// ============================================================================

FNGonMeshOpResult FNGonMeshAPI::CreateBox(
    FNGonPreviewScene& Scene,
    double Width, double Depth, double Height,
    int32 SubdivX, int32 SubdivY, int32 SubdivZ,
    const FVector& Center)
{
    FNGonMeshOpResult Res;
    Res.Operation = TEXT("create_box");

    const double W = FMath::Max(Width, 1.0);
    const double D = FMath::Max(Depth, 1.0);
    const double H = FMath::Max(Height, 1.0);
    const int32 NX = FMath::Clamp(SubdivX, 1, 32);
    const int32 NY = FMath::Clamp(SubdivY, 1, 32);
    const int32 NZ = FMath::Clamp(SubdivZ, 1, 32);

    TArray<FVector> Positions;
    TArray<TArray<int32>> Faces;
    TMap<FIntVector, int32> GridMap;

    auto GetOrAddVert = [&](int32 ix, int32 iy, int32 iz) -> int32
    {
        const FIntVector Key(ix, iy, iz);
        if (const int32* Found = GridMap.Find(Key))
        {
            return *Found;
        }
        const double fx = (static_cast<double>(ix) / NX) - 0.5;
        const double fy = (static_cast<double>(iy) / NY) - 0.5;
        const double fz = (static_cast<double>(iz) / NZ) - 0.5;
        const int32 Id = Positions.Add(Center + FVector(fx * W, fy * D, fz * H));
        GridMap.Add(Key, Id);
        return Id;
    };

    // +Z (top) & -Z (bottom)
    for (int32 ix = 0; ix < NX; ++ix)
    {
        for (int32 iy = 0; iy < NY; ++iy)
        {
            Faces.Add({ GetOrAddVert(ix, iy, NZ), GetOrAddVert(ix + 1, iy, NZ), GetOrAddVert(ix + 1, iy + 1, NZ), GetOrAddVert(ix, iy + 1, NZ) });
            Faces.Add({ GetOrAddVert(ix, iy, 0), GetOrAddVert(ix, iy + 1, 0), GetOrAddVert(ix + 1, iy + 1, 0), GetOrAddVert(ix + 1, iy, 0) });
        }
    }
    // +Y & -Y
    for (int32 ix = 0; ix < NX; ++ix)
    {
        for (int32 iz = 0; iz < NZ; ++iz)
        {
            Faces.Add({ GetOrAddVert(ix, NY, iz), GetOrAddVert(ix, NY, iz + 1), GetOrAddVert(ix + 1, NY, iz + 1), GetOrAddVert(ix + 1, NY, iz) });
            Faces.Add({ GetOrAddVert(ix, 0, iz), GetOrAddVert(ix + 1, 0, iz), GetOrAddVert(ix + 1, 0, iz + 1), GetOrAddVert(ix, 0, iz + 1) });
        }
    }
    // +X & -X
    for (int32 iy = 0; iy < NY; ++iy)
    {
        for (int32 iz = 0; iz < NZ; ++iz)
        {
            Faces.Add({ GetOrAddVert(NX, iy, iz), GetOrAddVert(NX, iy + 1, iz), GetOrAddVert(NX, iy + 1, iz + 1), GetOrAddVert(NX, iy, iz + 1) });
            Faces.Add({ GetOrAddVert(0, iy, iz), GetOrAddVert(0, iy, iz + 1), GetOrAddVert(0, iy + 1, iz + 1), GetOrAddVert(0, iy + 1, iz) });
        }
    }

    FText Error;
    if (!CommitPolygonMesh(Scene, Positions, Faces, true, Error))
    {
        Res.Message = Error.ToString();
        return Res;
    }

    Res.bOk = true;
    for (int32 i = 0; i < Faces.Num(); ++i) Res.CreatedFaces.Add(i);
    for (int32 i = 0; i < Positions.Num(); ++i) Res.CreatedVertices.Add(i);
    Res.Message = FString::Printf(TEXT("Created box %.1f x %.1f x %.1f (%d verts, %d faces)."), W, D, H, Positions.Num(), Faces.Num());
    return Res;
}

FNGonMeshOpResult FNGonMeshAPI::CreatePlane(
    FNGonPreviewScene& Scene,
    double Width, double Depth,
    int32 SubdivX, int32 SubdivY,
    const FVector& Center)
{
    FNGonMeshOpResult Res;
    Res.Operation = TEXT("create_plane");

    const double W = FMath::Max(Width, 1.0);
    const double D = FMath::Max(Depth, 1.0);
    const int32 NX = FMath::Clamp(SubdivX, 1, 64);
    const int32 NY = FMath::Clamp(SubdivY, 1, 64);

    TArray<FVector> Positions;
    Positions.Reserve((NX + 1) * (NY + 1));
    for (int32 iy = 0; iy <= NY; ++iy)
    {
        for (int32 ix = 0; ix <= NX; ++ix)
        {
            const double fx = (static_cast<double>(ix) / NX) - 0.5;
            const double fy = (static_cast<double>(iy) / NY) - 0.5;
            Positions.Add(Center + FVector(fx * W, fy * D, 0.0));
        }
    }

    TArray<TArray<int32>> Faces;
    Faces.Reserve(NX * NY);
    for (int32 iy = 0; iy < NY; ++iy)
    {
        for (int32 ix = 0; ix < NX; ++ix)
        {
            const int32 v00 = iy * (NX + 1) + ix;
            const int32 v10 = v00 + 1;
            const int32 v11 = v00 + (NX + 1) + 1;
            const int32 v01 = v00 + (NX + 1);
            Faces.Add({ v00, v10, v11, v01 });
        }
    }

    FText Error;
    if (!CommitPolygonMesh(Scene, Positions, Faces, true, Error))
    {
        Res.Message = Error.ToString();
        return Res;
    }

    Res.bOk = true;
    for (int32 i = 0; i < Faces.Num(); ++i) Res.CreatedFaces.Add(i);
    Res.Message = FString::Printf(TEXT("Created plane %.1f x %.1f (%d quads)."), W, D, Faces.Num());
    return Res;
}

FNGonMeshOpResult FNGonMeshAPI::CreateCylinder(
    FNGonPreviewScene& Scene,
    double Radius, double Height,
    int32 Sides, int32 HeightSegments,
    const FVector& Center)
{
    FNGonMeshOpResult Res;
    Res.Operation = TEXT("create_cylinder");

    const double R = FMath::Max(Radius, 1.0);
    const double H = FMath::Max(Height, 1.0);
    const int32 N = FMath::Clamp(Sides, 3, 64);
    const int32 Segs = FMath::Clamp(HeightSegments, 1, 32);

    TArray<FVector> Positions;
    Positions.Reserve(N * (Segs + 1));
    for (int32 s = 0; s <= Segs; ++s)
    {
        const double z = ((static_cast<double>(s) / Segs) - 0.5) * H;
        for (int32 i = 0; i < N; ++i)
        {
            const double Angle = (2.0 * UE_DOUBLE_PI * static_cast<double>(i)) / static_cast<double>(N);
            Positions.Add(Center + FVector(FMath::Cos(Angle) * R, FMath::Sin(Angle) * R, z));
        }
    }

    TArray<TArray<int32>> Faces;
    // Side quads
    for (int32 s = 0; s < Segs; ++s)
    {
        for (int32 i = 0; i < N; ++i)
        {
            const int32 Next = (i + 1) % N;
            const int32 b0 = s * N + i;
            const int32 b1 = s * N + Next;
            const int32 t1 = (s + 1) * N + Next;
            const int32 t0 = (s + 1) * N + i;
            Faces.Add({ b0, b1, t1, t0 });
        }
    }

    // Bottom and Top N-Gon caps (preserving true N-Gon topology!)
    TArray<int32> BottomCap;
    BottomCap.Reserve(N);
    for (int32 i = N - 1; i >= 0; --i) BottomCap.Add(i);
    Faces.Add(MoveTemp(BottomCap));

    TArray<int32> TopCap;
    TopCap.Reserve(N);
    for (int32 i = 0; i < N; ++i) TopCap.Add(Segs * N + i);
    Faces.Add(MoveTemp(TopCap));

    FText Error;
    if (!CommitPolygonMesh(Scene, Positions, Faces, true, Error))
    {
        Res.Message = Error.ToString();
        return Res;
    }

    Res.bOk = true;
    for (int32 i = 0; i < Faces.Num(); ++i) Res.CreatedFaces.Add(i);
    Res.Message = FString::Printf(TEXT("Created N-Gon cylinder (R=%.1f, H=%.1f, %d sides, %d faces)."), R, H, N, Faces.Num());
    return Res;
}

FNGonMeshOpResult FNGonMeshAPI::CreateSphere(
    FNGonPreviewScene& Scene,
    double Radius, int32 Rings, int32 Segments,
    const FVector& Center)
{
    FNGonMeshOpResult Res;
    Res.Operation = TEXT("create_sphere");

    const double R = FMath::Max(Radius, 1.0);
    const int32 NRings = FMath::Clamp(Rings, 4, 48);
    const int32 NSegs = FMath::Clamp(Segments, 6, 64);

    TArray<FVector> Positions;
    const int32 BottomPole = Positions.Add(Center + FVector(0.0, 0.0, -R));
    for (int32 r = 1; r < NRings; ++r)
    {
        const double Phi = -UE_DOUBLE_HALF_PI + (UE_DOUBLE_PI * static_cast<double>(r)) / static_cast<double>(NRings);
        const double CosP = FMath::Cos(Phi);
        const double SinP = FMath::Sin(Phi);
        for (int32 s = 0; s < NSegs; ++s)
        {
            const double Theta = (2.0 * UE_DOUBLE_PI * static_cast<double>(s)) / static_cast<double>(NSegs);
            Positions.Add(Center + FVector(CosP * FMath::Cos(Theta) * R, CosP * FMath::Sin(Theta) * R, SinP * R));
        }
    }
    const int32 TopPole = Positions.Add(Center + FVector(0.0, 0.0, R));

    TArray<TArray<int32>> Faces;
    // Bottom cap triangles
    for (int32 s = 0; s < NSegs; ++s)
    {
        const int32 Next = (s + 1) % NSegs;
        Faces.Add({ BottomPole, 1 + Next, 1 + s });
    }
    // Middle quad bands
    for (int32 r = 0; r < NRings - 2; ++r)
    {
        const int32 Ring0 = 1 + r * NSegs;
        const int32 Ring1 = 1 + (r + 1) * NSegs;
        for (int32 s = 0; s < NSegs; ++s)
        {
            const int32 Next = (s + 1) % NSegs;
            Faces.Add({ Ring0 + s, Ring0 + Next, Ring1 + Next, Ring1 + s });
        }
    }
    // Top cap triangles
    const int32 LastRing = 1 + (NRings - 2) * NSegs;
    for (int32 s = 0; s < NSegs; ++s)
    {
        const int32 Next = (s + 1) % NSegs;
        Faces.Add({ LastRing + s, LastRing + Next, TopPole });
    }

    FText Error;
    if (!CommitPolygonMesh(Scene, Positions, Faces, true, Error))
    {
        Res.Message = Error.ToString();
        return Res;
    }

    Res.bOk = true;
    for (int32 i = 0; i < Faces.Num(); ++i) Res.CreatedFaces.Add(i);
    Res.Message = FString::Printf(TEXT("Created UV sphere (R=%.1f, %d rings x %d segs, %d faces)."), R, NRings, NSegs, Faces.Num());
    return Res;
}

FNGonMeshOpResult FNGonMeshAPI::CreatePolygon(FNGonPreviewScene& Scene, const TArray<FVector>& Positions)
{
    FNGonMeshOpResult Res;
    Res.Operation = TEXT("create_polygon");
    if (Positions.Num() < 3)
    {
        Res.Message = TEXT("CreatePolygon requires at least 3 positions.");
        return Res;
    }
    TArray<int32> Loop;
    Loop.Reserve(Positions.Num());
    for (int32 i = 0; i < Positions.Num(); ++i) Loop.Add(i);
    TArray<TArray<int32>> Faces = { Loop };

    FText Error;
    if (!CommitPolygonMesh(Scene, Positions, Faces, true, Error))
    {
        Res.Message = Error.ToString();
        return Res;
    }

    Res.bOk = true;
    Res.CreatedFaces.Add(0);
    Res.Message = FString::Printf(TEXT("Created %d-sided N-Gon polygon."), Positions.Num());
    return Res;
}

// ============================================================================
// Face Modeling Operations
// ============================================================================

FNGonMeshOpResult FNGonMeshAPI::ExtrudeFaces(
    FNGonPreviewScene& Scene,
    const TArray<int32>& FaceIds,
    double Distance,
    double Scale)
{
    FNGonMeshOpResult Res;
    Res.Operation = TEXT("extrude_faces");

    const TArray<int32> Targets = ResolveTargetFaces(Scene, FaceIds);
    if (Targets.IsEmpty())
    {
        Res.Message = TEXT("Select at least one face (or pass face_ids) to extrude.");
        return Res;
    }

    const FNGonTopology& Topo = Scene.GetTopology();
    TArray<FVector> Positions = Scene.GetPositions();
    TSet<int32> TargetSet(Targets);

    TArray<TArray<int32>> NewFaces;
    TArray<int32> FaceKeys;
    Topo.Faces.GetKeys(FaceKeys);
    FaceKeys.Sort();

    // Keep untouched faces first
    for (const int32 Key : FaceKeys)
    {
        if (!TargetSet.Contains(Key))
        {
            if (const FNGonFace* F = Topo.Faces.Find(Key))
            {
                NewFaces.Add(F->Perimeter);
            }
        }
    }

    TArray<int32> NewCapFaceIndices;
    const double ClampedScale = FMath::Clamp(Scale, 0.05, 10.0);

    for (const int32 FaceId : Targets)
    {
        const FNGonFace* Face = Topo.Faces.Find(FaceId);
        if (!Face || Face->Perimeter.Num() < 3) continue;

        const TArray<int32>& Perim = Face->Perimeter;
        const FVector Normal = ComputePolygonNormal(Positions, Perim);
        const FVector Centroid = ComputePolygonCentroid(Positions, Perim);

        TArray<int32> CapVerts;
        CapVerts.Reserve(Perim.Num());
        for (const int32 V : Perim)
        {
            const FVector ScaledPos = Centroid + (Positions[V] - Centroid) * ClampedScale + Normal * Distance;
            const int32 NewV = Positions.Add(ScaledPos);
            CapVerts.Add(NewV);
            Res.CreatedVertices.Add(NewV);
        }

        // Side wall quads
        const int32 N = Perim.Num();
        for (int32 i = 0; i < N; ++i)
        {
            const int32 Next = (i + 1) % N;
            const int32 SideIdx = NewFaces.Add({ Perim[i], Perim[Next], CapVerts[Next], CapVerts[i] });
            Res.CreatedFaces.Add(SideIdx);
        }

        // Extruded cap polygon
        const int32 CapIdx = NewFaces.Add(CapVerts);
        Res.CreatedFaces.Add(CapIdx);
        NewCapFaceIndices.Add(CapIdx);
    }

    FText Error;
    if (!CommitPolygonMesh(Scene, Positions, NewFaces, true, Error))
    {
        Res.Message = Error.ToString();
        return Res;
    }

    TSet<int32> NewSel(NewCapFaceIndices);
    Scene.SetSelectionSet(ENGonElement::Face, NewSel, false);
    Res.bOk = true;
    Res.SelectedIds = NewCapFaceIndices;
    Res.Message = FString::Printf(TEXT("Extruded %d face(s) by %.2f (scale %.2f)."), Targets.Num(), Distance, ClampedScale);
    return Res;
}

FNGonMeshOpResult FNGonMeshAPI::InsetFaces(
    FNGonPreviewScene& Scene,
    const TArray<int32>& FaceIds,
    double Ratio,
    double Depth)
{
    FNGonMeshOpResult Res;
    Res.Operation = TEXT("inset_faces");

    const TArray<int32> Targets = ResolveTargetFaces(Scene, FaceIds);
    if (Targets.IsEmpty())
    {
        Res.Message = TEXT("Select at least one face (or pass face_ids) to inset.");
        return Res;
    }

    const double ClampedRatio = FMath::Clamp(Ratio, 0.05, 0.95);
    const FNGonTopology& Topo = Scene.GetTopology();
    TArray<FVector> Positions = Scene.GetPositions();
    TSet<int32> TargetSet(Targets);

    TArray<TArray<int32>> NewFaces;
    TArray<int32> FaceKeys;
    Topo.Faces.GetKeys(FaceKeys);
    FaceKeys.Sort();

    for (const int32 Key : FaceKeys)
    {
        if (!TargetSet.Contains(Key))
        {
            if (const FNGonFace* F = Topo.Faces.Find(Key))
            {
                NewFaces.Add(F->Perimeter);
            }
        }
    }

    TArray<int32> InnerFaceIndices;
    for (const int32 FaceId : Targets)
    {
        const FNGonFace* Face = Topo.Faces.Find(FaceId);
        if (!Face || Face->Perimeter.Num() < 3) continue;

        const TArray<int32>& Perim = Face->Perimeter;
        const FVector Normal = ComputePolygonNormal(Positions, Perim);
        const FVector Centroid = ComputePolygonCentroid(Positions, Perim);

        TArray<int32> InnerVerts;
        InnerVerts.Reserve(Perim.Num());
        for (const int32 V : Perim)
        {
            const FVector Pos = FMath::Lerp(Positions[V], Centroid, ClampedRatio) + Normal * Depth;
            const int32 NewV = Positions.Add(Pos);
            InnerVerts.Add(NewV);
            Res.CreatedVertices.Add(NewV);
        }

        const int32 N = Perim.Num();
        for (int32 i = 0; i < N; ++i)
        {
            const int32 Next = (i + 1) % N;
            const int32 BorderIdx = NewFaces.Add({ Perim[i], Perim[Next], InnerVerts[Next], InnerVerts[i] });
            Res.CreatedFaces.Add(BorderIdx);
        }

        const int32 InnerIdx = NewFaces.Add(InnerVerts);
        Res.CreatedFaces.Add(InnerIdx);
        InnerFaceIndices.Add(InnerIdx);
    }

    FText Error;
    if (!CommitPolygonMesh(Scene, Positions, NewFaces, true, Error))
    {
        Res.Message = Error.ToString();
        return Res;
    }

    TSet<int32> NewSel(InnerFaceIndices);
    Scene.SetSelectionSet(ENGonElement::Face, NewSel, false);
    Res.bOk = true;
    Res.SelectedIds = InnerFaceIndices;
    Res.Message = FString::Printf(TEXT("Inset %d face(s) by ratio %.2f (depth %.2f)."), Targets.Num(), ClampedRatio, Depth);
    return Res;
}

FNGonMeshOpResult FNGonMeshAPI::BevelFaces(
    FNGonPreviewScene& Scene,
    const TArray<int32>& FaceIds,
    double Ratio,
    double Height)
{
    FNGonMeshOpResult Res = InsetFaces(Scene, FaceIds, Ratio, Height);
    Res.Operation = TEXT("bevel_faces");
    if (Res.bOk)
    {
        Res.Message = FString::Printf(TEXT("Beveled %d face(s) (inset %.2f, height %.2f)."),
            Res.SelectedIds.Num(), FMath::Clamp(Ratio, 0.05, 0.95), Height);
    }
    return Res;
}

namespace
{
void ExtractOrderedFacesForBevel(
    const FNGonTopology& Topo,
    TArray<TArray<int32>>& OutFaces,
    TMap<int32, int32>& OutFaceIdToIndex)
{
    TArray<int32> FaceKeys;
    Topo.Faces.GetKeys(FaceKeys);
    FaceKeys.Sort();
    OutFaces.Reserve(FaceKeys.Num());
    for (const int32 FaceId : FaceKeys)
    {
        if (const FNGonFace* Face = Topo.Faces.Find(FaceId))
        {
            if (Face->Perimeter.Num() >= 3)
            {
                const int32 Idx = OutFaces.Add(Face->Perimeter);
                OutFaceIdToIndex.Add(FaceId, Idx);
            }
        }
    }
}

FNGonMeshOpResult FinalizeBevelCommit(
    FNGonPreviewScene& Scene,
    const FString& OpName,
    const FNGonBevelOutput& Out,
    const FNGonBevelOptions& Options)
{
    FNGonMeshOpResult Res;
    Res.Operation = OpName;
    if (!Out.bOk)
    {
        Res.Message = Out.Message;
        return Res;
    }

    FText Error;
    if (!CommitPolygonMesh(Scene, Out.Positions, Out.Faces, true, Error))
    {
        Res.Message = Error.ToString();
        return Res;
    }

    TArray<int32> NewFaceIds;
    NewFaceIds.Reserve(Out.BevelFaceIndices.Num() + Out.CornerFaceIndices.Num());
    for (const int32 FIdx : Out.BevelFaceIndices) NewFaceIds.AddUnique(FIdx);
    for (const int32 FIdx : Out.CornerFaceIndices) NewFaceIds.AddUnique(FIdx);
    NewFaceIds.Sort();

    if (Options.bSelectResultFaces && !NewFaceIds.IsEmpty())
    {
        TSet<int32> SelSet(NewFaceIds);
        Scene.SetSelectionSet(ENGonElement::Face, SelSet, false);
    }

    Res.bOk = true;
    Res.Message = Out.Message;
    Res.CreatedVertices = Out.CreatedVertices;
    Res.CreatedFaces = NewFaceIds;
    Res.SelectedIds = NewFaceIds;
    Res.PayloadJson = FString::Printf(
        TEXT("{\"requested_width\":%.4f,\"applied_width\":%.4f,\"was_clamped\":%s,\"segments\":%d,\"profile\":%.4f,\"bevel_face_count\":%d,\"corner_face_count\":%d,\"skipped_edges\":%d,\"skipped_vertices\":%d}"),
        Out.RequestedWidth,
        Out.AppliedWidth,
        Out.bWasClamped ? TEXT("true") : TEXT("false"),
        FMath::Clamp(Options.Segments, 1, 32),
        FMath::Clamp(Options.Profile, 0.05, 0.9999),
        Out.BevelFaceIndices.Num(),
        Out.CornerFaceIndices.Num(),
        Out.SkippedEdges,
        Out.SkippedVertices);
    return Res;
}
} // namespace

FNGonMeshOpResult FNGonMeshAPI::BevelEdges(
    FNGonPreviewScene& Scene,
    const TArray<int32>& EdgeIds,
    const FNGonBevelOptions& Options)
{
    FNGonMeshOpResult Res;
    Res.Operation = TEXT("bevel_edges");

    const FNGonTopology& Topo = Scene.GetTopology();
    TArray<int32> Targets;
    if (!EdgeIds.IsEmpty())
    {
        for (const int32 EId : EdgeIds)
        {
            if (Topo.Edges.IsValidIndex(EId)) Targets.AddUnique(EId);
        }
    }
    else if (Scene.GetSelectedElement() == ENGonElement::Edge)
    {
        for (const int32 EId : Scene.GetSelectedIds())
        {
            if (Topo.Edges.IsValidIndex(EId)) Targets.AddUnique(EId);
        }
    }
    Targets.Sort();

    if (Targets.IsEmpty())
    {
        Res.Message = TEXT("No edges selected to bevel.");
        return Res;
    }

    TArray<FIntPoint> SelectedEdges;
    SelectedEdges.Reserve(Targets.Num());
    for (const int32 EId : Targets)
    {
        SelectedEdges.Add(Topo.Edges[EId]);
    }

    TArray<TArray<int32>> OrderedFaces;
    TMap<int32, int32> FaceIdToIndex;
    ExtractOrderedFacesForBevel(Topo, OrderedFaces, FaceIdToIndex);

    const FNGonBevelOutput Out = NGon::Bevel::BevelEdges(Scene.GetPositions(), OrderedFaces, SelectedEdges, Options);
    return FinalizeBevelCommit(Scene, TEXT("bevel_edges"), Out, Options);
}

FNGonMeshOpResult FNGonMeshAPI::BevelVertices(
    FNGonPreviewScene& Scene,
    const TArray<int32>& VertexIds,
    const FNGonBevelOptions& Options)
{
    FNGonMeshOpResult Res;
    Res.Operation = TEXT("bevel_vertices");

    TArray<int32> Targets = ResolveTargetVertices(Scene, VertexIds);
    if (Targets.IsEmpty())
    {
        Res.Message = TEXT("No vertices selected to bevel.");
        return Res;
    }

    TArray<TArray<int32>> OrderedFaces;
    TMap<int32, int32> FaceIdToIndex;
    ExtractOrderedFacesForBevel(Scene.GetTopology(), OrderedFaces, FaceIdToIndex);

    const FNGonBevelOutput Out = NGon::Bevel::BevelVertices(Scene.GetPositions(), OrderedFaces, Targets, Options);
    return FinalizeBevelCommit(Scene, TEXT("bevel_vertices"), Out, Options);
}

FNGonMeshOpResult FNGonMeshAPI::BevelFacesBoundary(
    FNGonPreviewScene& Scene,
    const TArray<int32>& FaceIds,
    const FNGonBevelOptions& Options)
{
    FNGonMeshOpResult Res;
    Res.Operation = TEXT("bevel_faces_boundary");

    TArray<int32> Targets = ResolveTargetFaces(Scene, FaceIds);
    if (Targets.IsEmpty())
    {
        Res.Message = TEXT("No faces selected to bevel.");
        return Res;
    }

    TArray<TArray<int32>> OrderedFaces;
    TMap<int32, int32> FaceIdToIndex;
    ExtractOrderedFacesForBevel(Scene.GetTopology(), OrderedFaces, FaceIdToIndex);

    TSet<int32> TargetFaceIndices;
    for (const int32 FaceId : Targets)
    {
        if (const int32* Idx = FaceIdToIndex.Find(FaceId))
        {
            TargetFaceIndices.Add(*Idx);
        }
    }

    const FNGonBevelOutput Out = NGon::Bevel::BevelFaceBoundaries(Scene.GetPositions(), OrderedFaces, TargetFaceIndices, Options);
    return FinalizeBevelCommit(Scene, TEXT("bevel_faces_boundary"), Out, Options);
}

FNGonMeshOpResult FNGonMeshAPI::BevelSelection(
    FNGonPreviewScene& Scene,
    const FNGonBevelOptions& Options)
{
    const ENGonElement Elem = Scene.GetSelectedElement();
    if (Elem == ENGonElement::Edge)
    {
        return BevelEdges(Scene, {}, Options);
    }
    if (Elem == ENGonElement::Vertex)
    {
        return BevelVertices(Scene, {}, Options);
    }
    if (Elem == ENGonElement::Face)
    {
        return BevelFacesBoundary(Scene, {}, Options);
    }
    FNGonMeshOpResult Res;
    Res.Operation = TEXT("bevel_selection");
    Res.Message = TEXT("Select at least one edge, vertex, or face to bevel.");
    return Res;
}

FNGonMeshOpResult FNGonMeshAPI::SubdivideFaces(FNGonPreviewScene& Scene, const TArray<int32>& FaceIds)
{
    FNGonMeshOpResult Res;
    Res.Operation = TEXT("subdivide_faces");

    const FNGonTopology& Topo = Scene.GetTopology();
    TArray<int32> Targets = ResolveTargetFaces(Scene, FaceIds);
    if (Targets.IsEmpty())
    {
        // Default to subdividing the entire mesh when no faces are selected
        Topo.Faces.GetKeys(Targets);
        Targets.Sort();
    }
    if (Targets.IsEmpty())
    {
        Res.Message = TEXT("No faces available to subdivide.");
        return Res;
    }

    TArray<FVector> Positions = Scene.GetPositions();
    TSet<int32> TargetSet(Targets);
    TMap<FIntPoint, int32> EdgeMidpoints;

    auto GetOrCreateEdgeMidpoint = [&](int32 A, int32 B) -> int32
    {
        const FIntPoint Key(FMath::Min(A, B), FMath::Max(A, B));
        if (const int32* Found = EdgeMidpoints.Find(Key))
        {
            return *Found;
        }
        const int32 NewV = Positions.Add((Positions[A] + Positions[B]) * 0.5);
        EdgeMidpoints.Add(Key, NewV);
        Res.CreatedVertices.Add(NewV);
        return NewV;
    };

    // Pre-create edge midpoints for all target faces so shared edges stay water-tight
    for (const int32 FaceId : Targets)
    {
        if (const FNGonFace* Face = Topo.Faces.Find(FaceId))
        {
            const int32 N = Face->Perimeter.Num();
            for (int32 i = 0; i < N; ++i)
            {
                GetOrCreateEdgeMidpoint(Face->Perimeter[i], Face->Perimeter[(i + 1) % N]);
            }
        }
    }

    TArray<TArray<int32>> NewFaces;
    TArray<int32> FaceKeys;
    Topo.Faces.GetKeys(FaceKeys);
    FaceKeys.Sort();

    TArray<int32> SubdividedIndices;
    for (const int32 Key : FaceKeys)
    {
        const FNGonFace* Face = Topo.Faces.Find(Key);
        if (!Face || Face->Perimeter.Num() < 3) continue;

        const TArray<int32>& Perim = Face->Perimeter;
        const int32 N = Perim.Num();

        if (TargetSet.Contains(Key))
        {
            const int32 CenterV = Positions.Add(ComputePolygonCentroid(Positions, Perim));
            Res.CreatedVertices.Add(CenterV);

            for (int32 i = 0; i < N; ++i)
            {
                const int32 PrevV = Perim[(i + N - 1) % N];
                const int32 CurrV = Perim[i];
                const int32 NextV = Perim[(i + 1) % N];
                const int32 MidPrev = GetOrCreateEdgeMidpoint(PrevV, CurrV);
                const int32 MidNext = GetOrCreateEdgeMidpoint(CurrV, NextV);
                const int32 SubIdx = NewFaces.Add({ CurrV, MidNext, CenterV, MidPrev });
                Res.CreatedFaces.Add(SubIdx);
                SubdividedIndices.Add(SubIdx);
            }
        }
        else
        {
            // Insert shared edge midpoints into adjacent untouched faces to avoid T-junctions
            TArray<int32> UpdatedLoop;
            UpdatedLoop.Reserve(N * 2);
            for (int32 i = 0; i < N; ++i)
            {
                const int32 A = Perim[i];
                const int32 B = Perim[(i + 1) % N];
                UpdatedLoop.Add(A);
                if (const int32* Mid = EdgeMidpoints.Find(FIntPoint(FMath::Min(A, B), FMath::Max(A, B))))
                {
                    UpdatedLoop.Add(*Mid);
                }
            }
            NewFaces.Add(MoveTemp(UpdatedLoop));
        }
    }

    FText Error;
    if (!CommitPolygonMesh(Scene, Positions, NewFaces, true, Error))
    {
        Res.Message = Error.ToString();
        return Res;
    }

    Res.bOk = true;
    Res.Message = FString::Printf(TEXT("Subdivided %d face(s) into %d quads."), Targets.Num(), SubdividedIndices.Num());
    return Res;
}

FNGonMeshOpResult FNGonMeshAPI::TriangulateFaces(FNGonPreviewScene& Scene, const TArray<int32>& FaceIds)
{
    FNGonMeshOpResult Res;
    Res.Operation = TEXT("triangulate_faces");

    const FNGonTopology& Topo = Scene.GetTopology();
    TArray<int32> Targets = ResolveTargetFaces(Scene, FaceIds);
    if (Targets.IsEmpty())
    {
        Topo.Faces.GetKeys(Targets);
        Targets.Sort();
    }
    if (Targets.IsEmpty())
    {
        Res.Message = TEXT("No faces to triangulate.");
        return Res;
    }

    const TArray<FVector> Positions = Scene.GetPositions();
    TSet<int32> TargetSet(Targets);
    TArray<TArray<int32>> NewFaces;
    TArray<int32> FaceKeys;
    Topo.Faces.GetKeys(FaceKeys);
    FaceKeys.Sort();

    int32 TriCount = 0;
    for (const int32 Key : FaceKeys)
    {
        const FNGonFace* Face = Topo.Faces.Find(Key);
        if (!Face || Face->Perimeter.Num() < 3) continue;

        if (TargetSet.Contains(Key) && Face->Perimeter.Num() > 3)
        {
            const TArray<int32>& P = Face->Perimeter;
            for (int32 i = 1; i + 1 < P.Num(); ++i)
            {
                const int32 Idx = NewFaces.Add({ P[0], P[i], P[i + 1] });
                Res.CreatedFaces.Add(Idx);
                ++TriCount;
            }
        }
        else
        {
            NewFaces.Add(Face->Perimeter);
        }
    }

    FText Error;
    if (!CommitPolygonMesh(Scene, Positions, NewFaces, true, Error))
    {
        Res.Message = Error.ToString();
        return Res;
    }

    Res.bOk = true;
    Res.Message = FString::Printf(TEXT("Triangulated %d face(s) (%d new triangles)."), Targets.Num(), TriCount);
    return Res;
}

FNGonMeshOpResult FNGonMeshAPI::FlipFaces(FNGonPreviewScene& Scene, const TArray<int32>& FaceIds)
{
    FNGonMeshOpResult Res;
    Res.Operation = TEXT("flip_faces");

    const FNGonTopology& Topo = Scene.GetTopology();
    TArray<int32> Targets = ResolveTargetFaces(Scene, FaceIds);
    if (Targets.IsEmpty())
    {
        Topo.Faces.GetKeys(Targets);
        Targets.Sort();
    }
    if (Targets.IsEmpty())
    {
        Res.Message = TEXT("No faces to flip.");
        return Res;
    }

    const TArray<FVector> Positions = Scene.GetPositions();
    TSet<int32> TargetSet(Targets);
    TArray<TArray<int32>> NewFaces;
    TArray<int32> FaceKeys;
    Topo.Faces.GetKeys(FaceKeys);
    FaceKeys.Sort();

    for (const int32 Key : FaceKeys)
    {
        const FNGonFace* Face = Topo.Faces.Find(Key);
        if (!Face || Face->Perimeter.Num() < 3) continue;

        TArray<int32> Loop = Face->Perimeter;
        if (TargetSet.Contains(Key))
        {
            Algo::Reverse(Loop);
        }
        NewFaces.Add(MoveTemp(Loop));
    }

    FText Error;
    if (!CommitPolygonMesh(Scene, Positions, NewFaces, true, Error))
    {
        Res.Message = Error.ToString();
        return Res;
    }

    Res.bOk = true;
    Res.Message = FString::Printf(TEXT("Flipped normals on %d face(s)."), Targets.Num());
    return Res;
}

FNGonMeshOpResult FNGonMeshAPI::DeleteFaces(FNGonPreviewScene& Scene, const TArray<int32>& FaceIds)
{
    FNGonMeshOpResult Res;
    Res.Operation = TEXT("delete_faces");

    const FNGonTopology& Topo = Scene.GetTopology();
    const TArray<int32> Targets = ResolveTargetFaces(Scene, FaceIds);
    if (Targets.IsEmpty())
    {
        Res.Message = TEXT("Select at least one face (or pass face_ids) to delete.");
        return Res;
    }
    if (Targets.Num() >= Topo.Faces.Num())
    {
        Res.Message = TEXT("Cannot delete all faces from the mesh.");
        return Res;
    }

    TArray<FVector> Positions = Scene.GetPositions();
    TSet<int32> TargetSet(Targets);
    TArray<TArray<int32>> NewFaces;
    TArray<int32> FaceKeys;
    Topo.Faces.GetKeys(FaceKeys);
    FaceKeys.Sort();

    for (const int32 Key : FaceKeys)
    {
        if (!TargetSet.Contains(Key))
        {
            if (const FNGonFace* F = Topo.Faces.Find(Key))
            {
                NewFaces.Add(F->Perimeter);
            }
        }
    }

    CompactMeshData(Positions, NewFaces);
    FText Error;
    if (!CommitPolygonMesh(Scene, Positions, NewFaces, true, Error))
    {
        Res.Message = Error.ToString();
        return Res;
    }

    Res.bOk = true;
    Res.DeletedFaces = Targets;
    Res.Message = FString::Printf(TEXT("Deleted %d face(s)."), Targets.Num());
    return Res;
}

// ============================================================================
// Edge & Vertex Topology Operations
// ============================================================================

FNGonMeshOpResult FNGonMeshAPI::SplitEdge(FNGonPreviewScene& Scene, int32 EdgeId, double T)
{
    FNGonMeshOpResult Res;
    Res.Operation = TEXT("split_edge");

    const FNGonTopology& Topo = Scene.GetTopology();
    if (EdgeId == INDEX_NONE && Scene.GetSelectedElement() == ENGonElement::Edge && !Scene.GetSelectedIds().IsEmpty())
    {
        EdgeId = *Scene.GetSelectedIds().CreateConstIterator();
    }
    if (!Topo.Edges.IsValidIndex(EdgeId))
    {
        Res.Message = TEXT("Select a valid edge (or pass edge_id) to split.");
        return Res;
    }

    const FIntPoint Edge = Topo.Edges[EdgeId];
    const double Alpha = FMath::Clamp(T, 0.05, 0.95);
    TArray<FVector> Positions = Scene.GetPositions();
    const int32 NewVert = Positions.Add(FMath::Lerp(Positions[Edge.X], Positions[Edge.Y], Alpha));
    Res.CreatedVertices.Add(NewVert);

    TArray<TArray<int32>> NewFaces;
    TArray<int32> FaceKeys;
    Topo.Faces.GetKeys(FaceKeys);
    FaceKeys.Sort();

    int32 ModifiedFaceCount = 0;
    for (const int32 Key : FaceKeys)
    {
        const FNGonFace* Face = Topo.Faces.Find(Key);
        if (!Face || Face->Perimeter.Num() < 3) continue;

        TArray<int32> Loop;
        Loop.Reserve(Face->Perimeter.Num() + 1);
        const int32 N = Face->Perimeter.Num();
        for (int32 i = 0; i < N; ++i)
        {
            const int32 A = Face->Perimeter[i];
            const int32 B = Face->Perimeter[(i + 1) % N];
            Loop.Add(A);
            if ((A == Edge.X && B == Edge.Y) || (A == Edge.Y && B == Edge.X))
            {
                Loop.Add(NewVert);
                ++ModifiedFaceCount;
            }
        }
        NewFaces.Add(MoveTemp(Loop));
    }

    FText Error;
    if (!CommitPolygonMesh(Scene, Positions, NewFaces, true, Error))
    {
        Res.Message = Error.ToString();
        return Res;
    }

    Scene.SetSelectionSet(ENGonElement::Vertex, { NewVert }, false);
    Res.bOk = true;
    Res.SelectedIds = { NewVert };
    Res.Message = FString::Printf(TEXT("Split edge #%d at t=%.2f -> created vertex #%d across %d face(s)."),
        EdgeId, Alpha, NewVert, ModifiedFaceCount);
    return Res;
}

FNGonMeshOpResult FNGonMeshAPI::CutFace(FNGonPreviewScene& Scene, int32 FaceId, int32 VertexA, int32 VertexB)
{
    FNGonMeshOpResult Res;
    Res.Operation = TEXT("cut_face");

    const FNGonTopology& Topo = Scene.GetTopology();

    // Auto-resolve from 2 selected vertices if FaceId/VertexA/VertexB are not explicitly supplied
    if ((VertexA == INDEX_NONE || VertexB == INDEX_NONE) && Scene.GetSelectedElement() == ENGonElement::Vertex && Scene.GetSelectedIds().Num() >= 2)
    {
        TArray<int32> SelVerts = Scene.GetSelectedIds().Array();
        SelVerts.Sort();
        VertexA = SelVerts[0];
        VertexB = SelVerts[1];
    }

    if (FaceId == INDEX_NONE && VertexA != INDEX_NONE && VertexB != INDEX_NONE)
    {
        for (const TPair<int32, FNGonFace>& Pair : Topo.Faces)
        {
            if (Pair.Value.Perimeter.Contains(VertexA) && Pair.Value.Perimeter.Contains(VertexB))
            {
                FaceId = Pair.Key;
                break;
            }
        }
    }

    const FNGonFace* TargetFace = Topo.Faces.Find(FaceId);
    if (!TargetFace || TargetFace->Perimeter.Num() < 4)
    {
        Res.Message = TEXT("CutFace requires a face with at least 4 vertices and two non-adjacent vertices.");
        return Res;
    }

    const TArray<int32>& P = TargetFace->Perimeter;
    const int32 N = P.Num();
    const int32 IdxA = P.Find(VertexA);
    const int32 IdxB = P.Find(VertexB);
    if (IdxA == INDEX_NONE || IdxB == INDEX_NONE || IdxA == IdxB
        || (IdxA + 1) % N == IdxB || (IdxB + 1) % N == IdxA)
    {
        Res.Message = TEXT("CutFace vertices must belong to the target face and not be adjacent.");
        return Res;
    }

    TArray<int32> Loop1;
    for (int32 i = IdxA; ; i = (i + 1) % N)
    {
        Loop1.Add(P[i]);
        if (i == IdxB) break;
    }

    TArray<int32> Loop2;
    for (int32 i = IdxB; ; i = (i + 1) % N)
    {
        Loop2.Add(P[i]);
        if (i == IdxA) break;
    }

    const TArray<FVector> Positions = Scene.GetPositions();
    TArray<TArray<int32>> NewFaces;
    TArray<int32> FaceKeys;
    Topo.Faces.GetKeys(FaceKeys);
    FaceKeys.Sort();

    for (const int32 Key : FaceKeys)
    {
        if (Key == FaceId)
        {
            Res.CreatedFaces.Add(NewFaces.Add(Loop1));
            Res.CreatedFaces.Add(NewFaces.Add(Loop2));
        }
        else if (const FNGonFace* F = Topo.Faces.Find(Key))
        {
            NewFaces.Add(F->Perimeter);
        }
    }

    FText Error;
    if (!CommitPolygonMesh(Scene, Positions, NewFaces, true, Error))
    {
        Res.Message = Error.ToString();
        return Res;
    }

    Res.bOk = true;
    Res.Message = FString::Printf(TEXT("Cut face #%d between vertices %d and %d into %d-gon and %d-gon."),
        FaceId, VertexA, VertexB, Loop1.Num(), Loop2.Num());
    return Res;
}

FNGonMeshOpResult FNGonMeshAPI::CollapseEdge(FNGonPreviewScene& Scene, int32 EdgeId)
{
    FNGonMeshOpResult Res;
    Res.Operation = TEXT("collapse_edge");

    const FNGonTopology& Topo = Scene.GetTopology();
    if (EdgeId == INDEX_NONE && Scene.GetSelectedElement() == ENGonElement::Edge && !Scene.GetSelectedIds().IsEmpty())
    {
        EdgeId = *Scene.GetSelectedIds().CreateConstIterator();
    }
    if (!Topo.Edges.IsValidIndex(EdgeId))
    {
        Res.Message = TEXT("Select a valid edge (or pass edge_id) to collapse.");
        return Res;
    }

    const FIntPoint Edge = Topo.Edges[EdgeId];
    return WeldVertices(Scene, { Edge.X, Edge.Y }, 0.0);
}

FNGonMeshOpResult FNGonMeshAPI::WeldVertices(FNGonPreviewScene& Scene, const TArray<int32>& VertexIds, double Threshold)
{
    FNGonMeshOpResult Res;
    Res.Operation = TEXT("weld_vertices");

    TArray<FVector> Positions = Scene.GetPositions();
    const FNGonTopology& Topo = Scene.GetTopology();
    TArray<int32> Targets = ResolveTargetVertices(Scene, VertexIds);

    TArray<int32> Remap;
    Remap.SetNum(Positions.Num());
    for (int32 i = 0; i < Positions.Num(); ++i) Remap[i] = i;

    int32 WeldedCount = 0;
    if (Threshold > 0.0)
    {
        if (Targets.IsEmpty())
        {
            for (int32 i = 0; i < Positions.Num(); ++i) Targets.Add(i);
        }
        const double ThreshSq = Threshold * Threshold;
        for (int32 i = 0; i < Targets.Num(); ++i)
        {
            const int32 A = Targets[i];
            if (Remap[A] != A) continue;
            for (int32 j = i + 1; j < Targets.Num(); ++j)
            {
                const int32 B = Targets[j];
                if (Remap[B] != B) continue;
                if (FVector::DistSquared(Positions[A], Positions[B]) <= ThreshSq)
                {
                    Remap[B] = A;
                    ++WeldedCount;
                }
            }
        }
    }
    else
    {
        if (Targets.Num() < 2)
        {
            Res.Message = TEXT("Select at least 2 vertices (or specify threshold > 0) to weld.");
            return Res;
        }
        FVector Centroid = FVector::ZeroVector;
        for (const int32 V : Targets) Centroid += Positions[V];
        Centroid /= static_cast<double>(Targets.Num());

        const int32 Survivor = Targets[0];
        Positions[Survivor] = Centroid;
        for (int32 i = 1; i < Targets.Num(); ++i)
        {
            Remap[Targets[i]] = Survivor;
            ++WeldedCount;
        }
    }

    if (WeldedCount == 0)
    {
        Res.Message = TEXT("No vertices were within the weld criteria.");
        return Res;
    }

    TArray<TArray<int32>> NewFaces;
    TArray<int32> FaceKeys;
    Topo.Faces.GetKeys(FaceKeys);
    FaceKeys.Sort();

    for (const int32 Key : FaceKeys)
    {
        if (const FNGonFace* Face = Topo.Faces.Find(Key))
        {
            TArray<int32> Loop;
            Loop.Reserve(Face->Perimeter.Num());
            for (const int32 V : Face->Perimeter)
            {
                Loop.Add(Remap.IsValidIndex(V) ? Remap[V] : V);
            }
            NewFaces.Add(MoveTemp(Loop));
        }
    }

    CompactMeshData(Positions, NewFaces);
    if (NewFaces.IsEmpty())
    {
        Res.Message = TEXT("Weld would collapse all faces; operation rejected.");
        return Res;
    }

    FText Error;
    if (!CommitPolygonMesh(Scene, Positions, NewFaces, true, Error))
    {
        Res.Message = Error.ToString();
        return Res;
    }

    Res.bOk = true;
    Res.Message = FString::Printf(TEXT("Welded %d vertex merge(s) (%d verts, %d faces remaining)."),
        WeldedCount, Positions.Num(), NewFaces.Num());
    return Res;
}

FNGonMeshOpResult FNGonMeshAPI::BridgeEdges(FNGonPreviewScene& Scene, int32 EdgeA, int32 EdgeB)
{
    FNGonMeshOpResult Res;
    Res.Operation = TEXT("bridge_edges");

    const FNGonTopology& Topo = Scene.GetTopology();
    if ((EdgeA == INDEX_NONE || EdgeB == INDEX_NONE) && Scene.GetSelectedElement() == ENGonElement::Edge && Scene.GetSelectedIds().Num() >= 2)
    {
        TArray<int32> SelEdges = Scene.GetSelectedIds().Array();
        SelEdges.Sort();
        EdgeA = SelEdges[0];
        EdgeB = SelEdges[1];
    }

    if (!Topo.Edges.IsValidIndex(EdgeA) || !Topo.Edges.IsValidIndex(EdgeB) || EdgeA == EdgeB)
    {
        Res.Message = TEXT("Select 2 distinct edges (or pass edge_a and edge_b) to bridge.");
        return Res;
    }

    const FIntPoint EA = Topo.Edges[EdgeA];
    const FIntPoint EB = Topo.Edges[EdgeB];
    if (EA.X == EB.X || EA.X == EB.Y || EA.Y == EB.X || EA.Y == EB.Y)
    {
        Res.Message = TEXT("Edges to bridge must not share endpoints.");
        return Res;
    }

    const TArray<FVector> Positions = Scene.GetPositions();
    // Pick untwisted pairing by comparing diagonal lengths
    const double Pair1 = FVector::Distance(Positions[EA.X], Positions[EB.X]) + FVector::Distance(Positions[EA.Y], Positions[EB.Y]);
    const double Pair2 = FVector::Distance(Positions[EA.X], Positions[EB.Y]) + FVector::Distance(Positions[EA.Y], Positions[EB.X]);
    TArray<int32> BridgeQuad = (Pair1 <= Pair2)
        ? TArray<int32>{ EA.X, EA.Y, EB.Y, EB.X }
        : TArray<int32>{ EA.X, EA.Y, EB.X, EB.Y };

    TArray<TArray<int32>> NewFaces;
    TArray<int32> FaceKeys;
    Topo.Faces.GetKeys(FaceKeys);
    FaceKeys.Sort();
    for (const int32 Key : FaceKeys)
    {
        if (const FNGonFace* F = Topo.Faces.Find(Key)) NewFaces.Add(F->Perimeter);
    }
    const int32 NewFaceIdx = NewFaces.Add(BridgeQuad);

    FText Error;
    if (!CommitPolygonMesh(Scene, Positions, NewFaces, true, Error))
    {
        Res.Message = Error.ToString();
        return Res;
    }

    Scene.SetSelectionSet(ENGonElement::Face, { NewFaceIdx }, false);
    Res.bOk = true;
    Res.CreatedFaces = { NewFaceIdx };
    Res.SelectedIds = { NewFaceIdx };
    Res.Message = FString::Printf(TEXT("Bridged edges #%d and #%d -> created quad face #%d."), EdgeA, EdgeB, NewFaceIdx);
    return Res;
}

FNGonMeshOpResult FNGonMeshAPI::FillHole(FNGonPreviewScene& Scene, const TArray<int32>& OrderedVertices)
{
    FNGonMeshOpResult Res;
    Res.Operation = TEXT("fill_hole");

    TArray<int32> Loop = OrderedVertices;
    if (Loop.Num() < 3)
    {
        Loop = ResolveTargetVertices(Scene, OrderedVertices);
    }
    if (Loop.Num() < 3)
    {
        Res.Message = TEXT("FillHole requires at least 3 ordered vertices (or 3+ selected vertices).");
        return Res;
    }

    const TArray<FVector> Positions = Scene.GetPositions();
    const FNGonTopology& Topo = Scene.GetTopology();
    TArray<TArray<int32>> NewFaces;
    TArray<int32> FaceKeys;
    Topo.Faces.GetKeys(FaceKeys);
    FaceKeys.Sort();
    for (const int32 Key : FaceKeys)
    {
        if (const FNGonFace* F = Topo.Faces.Find(Key)) NewFaces.Add(F->Perimeter);
    }
    const int32 NewFaceIdx = NewFaces.Add(Loop);

    FText Error;
    if (!CommitPolygonMesh(Scene, Positions, NewFaces, true, Error))
    {
        Res.Message = Error.ToString();
        return Res;
    }

    Scene.SetSelectionSet(ENGonElement::Face, { NewFaceIdx }, false);
    Res.bOk = true;
    Res.CreatedFaces = { NewFaceIdx };
    Res.SelectedIds = { NewFaceIdx };
    Res.Message = FString::Printf(TEXT("Filled hole with %d-gon face #%d."), Loop.Num(), NewFaceIdx);
    return Res;
}

FNGonMeshOpResult FNGonMeshAPI::MirrorMesh(FNGonPreviewScene& Scene, EAxis::Type Axis, bool bKeepOriginal)
{
    FNGonMeshOpResult Res;
    Res.Operation = TEXT("mirror_mesh");

    TArray<FVector> Positions = Scene.GetPositions();
    const FNGonTopology& Topo = Scene.GetTopology();
    if (Positions.IsEmpty() || Topo.Faces.IsEmpty())
    {
        Res.Message = TEXT("Mesh is empty.");
        return Res;
    }

    auto MirrorPoint = [Axis](FVector P) -> FVector
    {
        if (Axis == EAxis::X) P.X = -P.X;
        else if (Axis == EAxis::Y) P.Y = -P.Y;
        else P.Z = -P.Z;
        return P;
    };

    TArray<TArray<int32>> NewFaces;
    TArray<int32> FaceKeys;
    Topo.Faces.GetKeys(FaceKeys);
    FaceKeys.Sort();

    if (!bKeepOriginal)
    {
        for (FVector& P : Positions) P = MirrorPoint(P);
        for (const int32 Key : FaceKeys)
        {
            if (const FNGonFace* F = Topo.Faces.Find(Key))
            {
                TArray<int32> Reversed = F->Perimeter;
                Algo::Reverse(Reversed);
                NewFaces.Add(MoveTemp(Reversed));
            }
        }
    }
    else
    {
        const int32 BaseVertCount = Positions.Num();
        Positions.Reserve(BaseVertCount * 2);
        for (int32 i = 0; i < BaseVertCount; ++i)
        {
            Res.CreatedVertices.Add(Positions.Add(MirrorPoint(Positions[i])));
        }
        for (const int32 Key : FaceKeys)
        {
            if (const FNGonFace* F = Topo.Faces.Find(Key))
            {
                NewFaces.Add(F->Perimeter);
                TArray<int32> MirroredLoop;
                MirroredLoop.Reserve(F->Perimeter.Num());
                for (int32 i = F->Perimeter.Num() - 1; i >= 0; --i)
                {
                    MirroredLoop.Add(BaseVertCount + F->Perimeter[i]);
                }
                Res.CreatedFaces.Add(NewFaces.Add(MoveTemp(MirroredLoop)));
            }
        }
    }

    FText Error;
    if (!CommitPolygonMesh(Scene, Positions, NewFaces, true, Error))
    {
        Res.Message = Error.ToString();
        return Res;
    }

    Res.bOk = true;
    Res.Message = FString::Printf(TEXT("Mirrored mesh across %s axis (%d total faces)."),
        Axis == EAxis::X ? TEXT("X") : (Axis == EAxis::Y ? TEXT("Y") : TEXT("Z")), NewFaces.Num());
    return Res;
}

// ============================================================================
// Transforms & Procedural Deformers
// ============================================================================

FNGonMeshOpResult FNGonMeshAPI::TransformVertices(
    FNGonPreviewScene& Scene,
    const TArray<int32>& VertexIds,
    const FVector& DeltaTranslation,
    const FRotator& DeltaRotation,
    const FVector& Scale)
{
    FNGonMeshOpResult Res;
    Res.Operation = TEXT("transform_vertices");

    TArray<FVector> Positions = Scene.GetPositions();
    TArray<int32> Targets = ResolveTargetVertices(Scene, VertexIds);
    if (Targets.IsEmpty())
    {
        for (int32 i = 0; i < Positions.Num(); ++i) Targets.Add(i);
    }
    if (Targets.IsEmpty())
    {
        Res.Message = TEXT("No vertices to transform.");
        return Res;
    }

    FVector Pivot = FVector::ZeroVector;
    for (const int32 V : Targets) Pivot += Positions[V];
    Pivot /= static_cast<double>(Targets.Num());

    const FQuat Rot = DeltaRotation.Quaternion();
    for (const int32 V : Targets)
    {
        FVector Local = Positions[V] - Pivot;
        Local = FVector(Local.X * Scale.X, Local.Y * Scale.Y, Local.Z * Scale.Z);
        Local = Rot.RotateVector(Local);
        Positions[V] = Pivot + Local + DeltaTranslation;
    }

    const FNGonTopology& Topo = Scene.GetTopology();
    TArray<TArray<int32>> Faces;
    TArray<int32> FaceKeys;
    Topo.Faces.GetKeys(FaceKeys);
    FaceKeys.Sort();
    for (const int32 Key : FaceKeys)
    {
        if (const FNGonFace* F = Topo.Faces.Find(Key)) Faces.Add(F->Perimeter);
    }

    FText Error;
    if (!CommitPolygonMesh(Scene, Positions, Faces, true, Error))
    {
        Res.Message = Error.ToString();
        return Res;
    }

    Res.bOk = true;
    Res.ModifiedVertices = Targets;
    Res.Message = FString::Printf(TEXT("Transformed %d vertex(es)."), Targets.Num());
    return Res;
}

FNGonMeshOpResult FNGonMeshAPI::SmoothVertices(
    FNGonPreviewScene& Scene,
    const TArray<int32>& VertexIds,
    int32 Iterations,
    double Strength)
{
    FNGonMeshOpResult Res;
    Res.Operation = TEXT("smooth_vertices");

    TArray<FVector> Positions = Scene.GetPositions();
    const FNGonTopology& Topo = Scene.GetTopology();
    TArray<int32> Targets = ResolveTargetVertices(Scene, VertexIds);
    if (Targets.IsEmpty())
    {
        for (int32 i = 0; i < Positions.Num(); ++i) Targets.Add(i);
    }
    if (Targets.IsEmpty())
    {
        Res.Message = TEXT("No vertices to smooth.");
        return Res;
    }

    TArray<TArray<int32>> Neighbors;
    Neighbors.SetNum(Positions.Num());
    for (const FIntPoint& E : Topo.Edges)
    {
        if (Positions.IsValidIndex(E.X) && Positions.IsValidIndex(E.Y))
        {
            Neighbors[E.X].AddUnique(E.Y);
            Neighbors[E.Y].AddUnique(E.X);
        }
    }

    const int32 Iters = FMath::Clamp(Iterations, 1, 32);
    const double Alpha = FMath::Clamp(Strength, 0.05, 1.0);
    for (int32 Iter = 0; Iter < Iters; ++Iter)
    {
        TArray<FVector> NextPos = Positions;
        for (const int32 V : Targets)
        {
            const TArray<int32>& Adj = Neighbors[V];
            if (Adj.IsEmpty()) continue;
            FVector Avg = FVector::ZeroVector;
            for (const int32 Nb : Adj) Avg += Positions[Nb];
            Avg /= static_cast<double>(Adj.Num());
            NextPos[V] = FMath::Lerp(Positions[V], Avg, Alpha);
        }
        Positions = MoveTemp(NextPos);
    }

    TArray<TArray<int32>> Faces;
    TArray<int32> FaceKeys;
    Topo.Faces.GetKeys(FaceKeys);
    FaceKeys.Sort();
    for (const int32 Key : FaceKeys)
    {
        if (const FNGonFace* F = Topo.Faces.Find(Key)) Faces.Add(F->Perimeter);
    }

    FText Error;
    if (!CommitPolygonMesh(Scene, Positions, Faces, true, Error))
    {
        Res.Message = Error.ToString();
        return Res;
    }

    Res.bOk = true;
    Res.ModifiedVertices = Targets;
    Res.Message = FString::Printf(TEXT("Smoothed %d vertex(es) (%d iters, strength %.2f)."), Targets.Num(), Iters, Alpha);
    return Res;
}

FNGonMeshOpResult FNGonMeshAPI::ApplyNoise(
    FNGonPreviewScene& Scene,
    const TArray<int32>& VertexIds,
    double Amplitude,
    double Frequency,
    int32 Seed)
{
    FNGonMeshOpResult Res;
    Res.Operation = TEXT("apply_noise");

    TArray<FVector> Positions = Scene.GetPositions();
    const FNGonTopology& Topo = Scene.GetTopology();
    TArray<int32> Targets = ResolveTargetVertices(Scene, VertexIds);
    if (Targets.IsEmpty())
    {
        for (int32 i = 0; i < Positions.Num(); ++i) Targets.Add(i);
    }
    if (Targets.IsEmpty())
    {
        Res.Message = TEXT("No vertices for noise displacement.");
        return Res;
    }

    // Compute approximate vertex normals
    TArray<FVector> Normals;
    Normals.Init(FVector::ZeroVector, Positions.Num());
    for (const TPair<int32, FNGonFace>& Pair : Topo.Faces)
    {
        const FVector N = ComputePolygonNormal(Positions, Pair.Value.Perimeter);
        for (const int32 V : Pair.Value.Perimeter)
        {
            if (Normals.IsValidIndex(V)) Normals[V] += N;
        }
    }

    for (const int32 V : Targets)
    {
        const FVector N = Normals[V].IsNearlyZero() ? FVector::UpVector : Normals[V].GetSafeNormal();
        const double NoiseVal = DeterministicHashNoise(Positions[V] * Frequency, Seed);
        Positions[V] += N * (NoiseVal * Amplitude);
    }

    TArray<TArray<int32>> Faces;
    TArray<int32> FaceKeys;
    Topo.Faces.GetKeys(FaceKeys);
    FaceKeys.Sort();
    for (const int32 Key : FaceKeys)
    {
        if (const FNGonFace* F = Topo.Faces.Find(Key)) Faces.Add(F->Perimeter);
    }

    FText Error;
    if (!CommitPolygonMesh(Scene, Positions, Faces, true, Error))
    {
        Res.Message = Error.ToString();
        return Res;
    }

    Res.bOk = true;
    Res.ModifiedVertices = Targets;
    Res.Message = FString::Printf(TEXT("Applied noise (amp=%.2f, freq=%.3f) to %d vertex(es)."), Amplitude, Frequency, Targets.Num());
    return Res;
}

FNGonMeshOpResult FNGonMeshAPI::ApplyTaper(FNGonPreviewScene& Scene, EAxis::Type Axis, double Amount)
{
    FNGonMeshOpResult Res;
    Res.Operation = TEXT("apply_taper");

    TArray<FVector> Positions = Scene.GetPositions();
    if (Positions.IsEmpty())
    {
        Res.Message = TEXT("Mesh is empty.");
        return Res;
    }

    const FBox Bounds = Scene.GetMeshBounds();
    const FVector Center = Bounds.GetCenter();
    const int32 AxisIdx = (Axis == EAxis::X) ? 0 : (Axis == EAxis::Y ? 1 : 2);
    const double MinVal = Bounds.Min[AxisIdx];
    const double Span = FMath::Max(Bounds.Max[AxisIdx] - MinVal, 1e-4);

    for (int32 i = 0; i < Positions.Num(); ++i)
    {
        const double T = (Positions[i][AxisIdx] - MinVal) / Span;
        const double Factor = FMath::Max(0.05, 1.0 - Amount * T);
        for (int32 a = 0; a < 3; ++a)
        {
            if (a != AxisIdx)
            {
                Positions[i][a] = Center[a] + (Positions[i][a] - Center[a]) * Factor;
            }
        }
        Res.ModifiedVertices.Add(i);
    }

    const FNGonTopology& Topo = Scene.GetTopology();
    TArray<TArray<int32>> Faces;
    TArray<int32> FaceKeys;
    Topo.Faces.GetKeys(FaceKeys);
    FaceKeys.Sort();
    for (const int32 Key : FaceKeys)
    {
        if (const FNGonFace* F = Topo.Faces.Find(Key)) Faces.Add(F->Perimeter);
    }

    FText Error;
    if (!CommitPolygonMesh(Scene, Positions, Faces, true, Error))
    {
        Res.Message = Error.ToString();
        return Res;
    }

    Res.bOk = true;
    Res.Message = FString::Printf(TEXT("Applied taper (amount=%.2f) along %s axis."),
        Amount, Axis == EAxis::X ? TEXT("X") : (Axis == EAxis::Y ? TEXT("Y") : TEXT("Z")));
    return Res;
}

FNGonMeshOpResult FNGonMeshAPI::ApplyTwist(FNGonPreviewScene& Scene, EAxis::Type Axis, double AngleDeg)
{
    FNGonMeshOpResult Res;
    Res.Operation = TEXT("apply_twist");

    TArray<FVector> Positions = Scene.GetPositions();
    if (Positions.IsEmpty())
    {
        Res.Message = TEXT("Mesh is empty.");
        return Res;
    }

    const FBox Bounds = Scene.GetMeshBounds();
    const FVector Center = Bounds.GetCenter();
    const int32 AxisIdx = (Axis == EAxis::X) ? 0 : (Axis == EAxis::Y ? 1 : 2);
    const FVector AxisVec = (Axis == EAxis::X) ? FVector::UnitX() : (Axis == EAxis::Y ? FVector::UnitY() : FVector::UnitZ());
    const double MinVal = Bounds.Min[AxisIdx];
    const double Span = FMath::Max(Bounds.Max[AxisIdx] - MinVal, 1e-4);
    const double AngleRad = FMath::DegreesToRadians(AngleDeg);

    for (int32 i = 0; i < Positions.Num(); ++i)
    {
        const double T = (Positions[i][AxisIdx] - MinVal) / Span;
        const FQuat Rot(AxisVec, AngleRad * T);
        Positions[i] = Center + Rot.RotateVector(Positions[i] - Center);
        Res.ModifiedVertices.Add(i);
    }

    const FNGonTopology& Topo = Scene.GetTopology();
    TArray<TArray<int32>> Faces;
    TArray<int32> FaceKeys;
    Topo.Faces.GetKeys(FaceKeys);
    FaceKeys.Sort();
    for (const int32 Key : FaceKeys)
    {
        if (const FNGonFace* F = Topo.Faces.Find(Key)) Faces.Add(F->Perimeter);
    }

    FText Error;
    if (!CommitPolygonMesh(Scene, Positions, Faces, true, Error))
    {
        Res.Message = Error.ToString();
        return Res;
    }

    Res.bOk = true;
    Res.Message = FString::Printf(TEXT("Applied twist (%.1f deg) along %s axis."),
        AngleDeg, Axis == EAxis::X ? TEXT("X") : (Axis == EAxis::Y ? TEXT("Y") : TEXT("Z")));
    return Res;
}

// ============================================================================
// AI-Friendly Selection Helpers
// ============================================================================

FNGonMeshOpResult FNGonMeshAPI::SelectElements(
    FNGonPreviewScene& Scene,
    ENGonElement Element,
    const TArray<int32>& Ids,
    bool bAdd)
{
    FNGonMeshOpResult Res;
    Res.Operation = TEXT("select_elements");

    const FNGonTopology& Topo = Scene.GetTopology();
    TSet<int32> Valid;
    for (const int32 Id : Ids)
    {
        if (Element == ENGonElement::Vertex && Scene.GetPositions().IsValidIndex(Id)) Valid.Add(Id);
        else if (Element == ENGonElement::Edge && Topo.Edges.IsValidIndex(Id)) Valid.Add(Id);
        else if (Element == ENGonElement::Face && Topo.Faces.Contains(Id)) Valid.Add(Id);
    }

    Scene.SetSelectionSet(Element, Valid, bAdd);
    Res.bOk = true;
    Res.SelectedIds = Scene.GetSelectedIds().Array();
    Res.SelectedIds.Sort();
    Res.Message = FString::Printf(TEXT("Selected %d element(s)."), Res.SelectedIds.Num());
    return Res;
}

FNGonMeshOpResult FNGonMeshAPI::SelectAll(FNGonPreviewScene& Scene, ENGonElement Element)
{
    FNGonMeshOpResult Res;
    Res.Operation = TEXT("select_all");

    const FNGonTopology& Topo = Scene.GetTopology();
    TSet<int32> All;
    if (Element == ENGonElement::Vertex)
    {
        for (int32 i = 0; i < Scene.GetPositions().Num(); ++i) All.Add(i);
    }
    else if (Element == ENGonElement::Edge)
    {
        for (int32 i = 0; i < Topo.Edges.Num(); ++i) All.Add(i);
    }
    else
    {
        for (const TPair<int32, FNGonFace>& Pair : Topo.Faces) All.Add(Pair.Key);
    }

    Scene.SetSelectionSet(Element, All, false);
    Res.bOk = true;
    Res.SelectedIds = All.Array();
    Res.SelectedIds.Sort();
    Res.Message = FString::Printf(TEXT("Selected all %d element(s)."), Res.SelectedIds.Num());
    return Res;
}

FNGonMeshOpResult FNGonMeshAPI::InvertSelection(FNGonPreviewScene& Scene)
{
    FNGonMeshOpResult Res;
    Res.Operation = TEXT("invert_selection");

    const ENGonElement Elem = Scene.GetSelectedElement();
    const TSet<int32>& Current = Scene.GetSelectedIds();
    const FNGonTopology& Topo = Scene.GetTopology();
    TSet<int32> Inverted;

    if (Elem == ENGonElement::Vertex)
    {
        for (int32 i = 0; i < Scene.GetPositions().Num(); ++i) if (!Current.Contains(i)) Inverted.Add(i);
    }
    else if (Elem == ENGonElement::Edge)
    {
        for (int32 i = 0; i < Topo.Edges.Num(); ++i) if (!Current.Contains(i)) Inverted.Add(i);
    }
    else
    {
        for (const TPair<int32, FNGonFace>& Pair : Topo.Faces) if (!Current.Contains(Pair.Key)) Inverted.Add(Pair.Key);
    }

    Scene.SetSelectionSet(Elem, Inverted, false);
    Res.bOk = true;
    Res.SelectedIds = Inverted.Array();
    Res.SelectedIds.Sort();
    Res.Message = FString::Printf(TEXT("Inverted selection (%d selected)."), Res.SelectedIds.Num());
    return Res;
}

FNGonMeshOpResult FNGonMeshAPI::GrowSelection(FNGonPreviewScene& Scene)
{
    FNGonMeshOpResult Res;
    Res.Operation = TEXT("grow_selection");

    const ENGonElement Elem = Scene.GetSelectedElement();
    const TSet<int32>& Current = Scene.GetSelectedIds();
    const FNGonTopology& Topo = Scene.GetTopology();
    if (Current.IsEmpty())
    {
        Res.Message = TEXT("Nothing selected to grow.");
        return Res;
    }

    TSet<int32> SeedVerts;
    if (Elem == ENGonElement::Vertex) SeedVerts = Current;
    else if (Elem == ENGonElement::Edge)
    {
        for (const int32 EId : Current)
        {
            if (Topo.Edges.IsValidIndex(EId)) { SeedVerts.Add(Topo.Edges[EId].X); SeedVerts.Add(Topo.Edges[EId].Y); }
        }
    }
    else
    {
        for (const int32 FId : Current)
        {
            if (const FNGonFace* F = Topo.Faces.Find(FId)) for (const int32 V : F->Perimeter) SeedVerts.Add(V);
        }
    }

    TSet<int32> Grown = Current;
    if (Elem == ENGonElement::Vertex)
    {
        for (const FIntPoint& E : Topo.Edges)
        {
            if (SeedVerts.Contains(E.X) || SeedVerts.Contains(E.Y)) { Grown.Add(E.X); Grown.Add(E.Y); }
        }
    }
    else if (Elem == ENGonElement::Edge)
    {
        for (int32 i = 0; i < Topo.Edges.Num(); ++i)
        {
            if (SeedVerts.Contains(Topo.Edges[i].X) || SeedVerts.Contains(Topo.Edges[i].Y)) Grown.Add(i);
        }
    }
    else
    {
        for (const TPair<int32, FNGonFace>& Pair : Topo.Faces)
        {
            for (const int32 V : Pair.Value.Perimeter)
            {
                if (SeedVerts.Contains(V)) { Grown.Add(Pair.Key); break; }
            }
        }
    }

    Scene.SetSelectionSet(Elem, Grown, false);
    Res.bOk = true;
    Res.SelectedIds = Grown.Array();
    Res.SelectedIds.Sort();
    Res.Message = FString::Printf(TEXT("Grew selection from %d to %d."), Current.Num(), Grown.Num());
    return Res;
}

FNGonMeshOpResult FNGonMeshAPI::ShrinkSelection(FNGonPreviewScene& Scene)
{
    FNGonMeshOpResult Res;
    Res.Operation = TEXT("shrink_selection");

    const ENGonElement Elem = Scene.GetSelectedElement();
    const TSet<int32>& Current = Scene.GetSelectedIds();
    const FNGonTopology& Topo = Scene.GetTopology();
    if (Current.IsEmpty())
    {
        Res.Message = TEXT("Nothing selected to shrink.");
        return Res;
    }

    TSet<int32> Shrunk;
    if (Elem == ENGonElement::Face)
    {
        TSet<int32> UnselectedVerts;
        for (const TPair<int32, FNGonFace>& Pair : Topo.Faces)
        {
            if (!Current.Contains(Pair.Key))
            {
                for (const int32 V : Pair.Value.Perimeter) UnselectedVerts.Add(V);
            }
        }
        for (const int32 FId : Current)
        {
            if (const FNGonFace* F = Topo.Faces.Find(FId))
            {
                bool bTouchesUnselected = false;
                for (const int32 V : F->Perimeter)
                {
                    if (UnselectedVerts.Contains(V)) { bTouchesUnselected = true; break; }
                }
                if (!bTouchesUnselected) Shrunk.Add(FId);
            }
        }
    }
    else if (Elem == ENGonElement::Vertex)
    {
        for (const int32 V : Current)
        {
            bool bHasUnselectedNeighbor = false;
            for (const FIntPoint& E : Topo.Edges)
            {
                if (E.X == V && !Current.Contains(E.Y)) { bHasUnselectedNeighbor = true; break; }
                if (E.Y == V && !Current.Contains(E.X)) { bHasUnselectedNeighbor = true; break; }
            }
            if (!bHasUnselectedNeighbor) Shrunk.Add(V);
        }
    }
    else
    {
        // Shrink edge selection: drop edges that touch any vertex connected to an unselected edge
        TSet<int32> UnselectedEdgeVerts;
        for (int32 e = 0; e < Topo.Edges.Num(); ++e)
        {
            if (!Current.Contains(e))
            {
                UnselectedEdgeVerts.Add(Topo.Edges[e].X);
                UnselectedEdgeVerts.Add(Topo.Edges[e].Y);
            }
        }
        for (const int32 EId : Current)
        {
            if (Topo.Edges.IsValidIndex(EId))
            {
                const FIntPoint E = Topo.Edges[EId];
                if (!UnselectedEdgeVerts.Contains(E.X) && !UnselectedEdgeVerts.Contains(E.Y))
                {
                    Shrunk.Add(EId);
                }
            }
        }
    }

    Scene.SetSelectionSet(Elem, Shrunk, false);
    Res.bOk = true;
    Res.SelectedIds = Shrunk.Array();
    Res.SelectedIds.Sort();
    Res.Message = FString::Printf(TEXT("Shrunk selection from %d to %d."), Current.Num(), Shrunk.Num());
    return Res;
}

FNGonMeshOpResult FNGonMeshAPI::SelectFacesByNormal(
    FNGonPreviewScene& Scene,
    const FVector& Direction,
    double MaxAngleDeg,
    bool bAdd)
{
    FNGonMeshOpResult Res;
    Res.Operation = TEXT("select_faces_by_normal");

    const FVector Dir = Direction.GetSafeNormal();
    if (Dir.IsNearlyZero())
    {
        Res.Message = TEXT("Direction vector cannot be zero.");
        return Res;
    }

    const double CosThreshold = FMath::Cos(FMath::DegreesToRadians(FMath::Clamp(MaxAngleDeg, 0.1, 180.0)));
    const TArray<FVector>& Positions = Scene.GetPositions();
    const FNGonTopology& Topo = Scene.GetTopology();
    TSet<int32> Matched;

    for (const TPair<int32, FNGonFace>& Pair : Topo.Faces)
    {
        const FVector N = ComputePolygonNormal(Positions, Pair.Value.Perimeter);
        if (FVector::DotProduct(N, Dir) >= CosThreshold)
        {
            Matched.Add(Pair.Key);
        }
    }

    Scene.SetSelectionSet(ENGonElement::Face, Matched, bAdd);
    Res.bOk = true;
    Res.SelectedIds = Scene.GetSelectedIds().Array();
    Res.SelectedIds.Sort();
    Res.Message = FString::Printf(TEXT("Selected %d face(s) within %.1f deg of normal."), Matched.Num(), MaxAngleDeg);
    return Res;
}

FNGonMeshOpResult FNGonMeshAPI::SelectByBounds(
    FNGonPreviewScene& Scene,
    ENGonElement Element,
    const FBox& Box,
    bool bAdd)
{
    FNGonMeshOpResult Res;
    Res.Operation = TEXT("select_by_bounds");

    const TArray<FVector>& Positions = Scene.GetPositions();
    const FNGonTopology& Topo = Scene.GetTopology();
    TSet<int32> Matched;

    if (Element == ENGonElement::Vertex)
    {
        for (int32 i = 0; i < Positions.Num(); ++i)
        {
            if (Box.IsInsideOrOn(Positions[i])) Matched.Add(i);
        }
    }
    else if (Element == ENGonElement::Edge)
    {
        for (int32 i = 0; i < Topo.Edges.Num(); ++i)
        {
            const FVector Mid = (Positions[Topo.Edges[i].X] + Positions[Topo.Edges[i].Y]) * 0.5;
            if (Box.IsInsideOrOn(Mid)) Matched.Add(i);
        }
    }
    else
    {
        for (const TPair<int32, FNGonFace>& Pair : Topo.Faces)
        {
            const FVector C = ComputePolygonCentroid(Positions, Pair.Value.Perimeter);
            if (Box.IsInsideOrOn(C)) Matched.Add(Pair.Key);
        }
    }

    Scene.SetSelectionSet(Element, Matched, bAdd);
    Res.bOk = true;
    Res.SelectedIds = Scene.GetSelectedIds().Array();
    Res.SelectedIds.Sort();
    Res.Message = FString::Printf(TEXT("Selected %d element(s) inside bounds."), Matched.Num());
    return Res;
}

FNGonMeshOpResult FNGonMeshAPI::SelectNGons(FNGonPreviewScene& Scene, int32 MinSides)
{
    FNGonMeshOpResult Res;
    Res.Operation = TEXT("select_ngons");

    const int32 MinS = FMath::Max(3, MinSides);
    TSet<int32> Matched;
    for (const TPair<int32, FNGonFace>& Pair : Scene.GetTopology().Faces)
    {
        if (Pair.Value.Perimeter.Num() >= MinS)
        {
            Matched.Add(Pair.Key);
        }
    }

    Scene.SetSelectionSet(ENGonElement::Face, Matched, false);
    Res.bOk = true;
    Res.SelectedIds = Matched.Array();
    Res.SelectedIds.Sort();
    Res.Message = FString::Printf(TEXT("Selected %d face(s) with >= %d sides."), Matched.Num(), MinS);
    return Res;
}

FNGonMeshOpResult FNGonMeshAPI::SelectBoundaryEdges(FNGonPreviewScene& Scene)
{
    FNGonMeshOpResult Res;
    Res.Operation = TEXT("select_boundary_edges");

    const FNGonTopology& Topo = Scene.GetTopology();
    TMap<FIntPoint, int32> EdgeCounts;
    for (const TPair<int32, FNGonFace>& Pair : Topo.Faces)
    {
        const TArray<int32>& P = Pair.Value.Perimeter;
        for (int32 i = 0; i < P.Num(); ++i)
        {
            const int32 A = P[i];
            const int32 B = P[(i + 1) % P.Num()];
            ++EdgeCounts.FindOrAdd(FIntPoint(FMath::Min(A, B), FMath::Max(A, B)));
        }
    }

    TSet<int32> Boundary;
    for (int32 i = 0; i < Topo.Edges.Num(); ++i)
    {
        const FIntPoint Key(FMath::Min(Topo.Edges[i].X, Topo.Edges[i].Y), FMath::Max(Topo.Edges[i].X, Topo.Edges[i].Y));
        if (EdgeCounts.FindRef(Key) == 1)
        {
            Boundary.Add(i);
        }
    }

    Scene.SetSelectionSet(ENGonElement::Edge, Boundary, false);
    Res.bOk = true;
    Res.SelectedIds = Boundary.Array();
    Res.SelectedIds.Sort();
    Res.Message = FString::Printf(TEXT("Selected %d boundary edge(s)."), Boundary.Num());
    return Res;
}

FNGonMeshOpResult FNGonMeshAPI::SelectEdgeLoop(FNGonPreviewScene& Scene, int32 StartEdgeId, bool bAdd)
{
    FNGonMeshOpResult Res;
    Res.Operation = TEXT("select_edge_loop");

    const FNGonTopology& Topo = Scene.GetTopology();
    if (StartEdgeId == INDEX_NONE && Scene.GetSelectedElement() == ENGonElement::Edge && !Scene.GetSelectedIds().IsEmpty())
    {
        StartEdgeId = *Scene.GetSelectedIds().CreateConstIterator();
    }
    if (!Topo.Edges.IsValidIndex(StartEdgeId))
    {
        Res.Message = TEXT("Select a valid edge (or pass edge_id) for edge loop.");
        return Res;
    }

    // Build edge lookup and vertex -> edges adjacency
    TMap<FIntPoint, int32> EdgeToId;
    EdgeToId.Reserve(Topo.Edges.Num());
    TMap<int32, TArray<int32>> VertEdges;
    for (int32 i = 0; i < Topo.Edges.Num(); ++i)
    {
        const FIntPoint Key(FMath::Min(Topo.Edges[i].X, Topo.Edges[i].Y), FMath::Max(Topo.Edges[i].X, Topo.Edges[i].Y));
        EdgeToId.Add(Key, i);
        VertEdges.FindOrAdd(Key.X).Add(i);
        VertEdges.FindOrAdd(Key.Y).Add(i);
    }

    // Build edge -> incident faces adjacency
    TMap<int32, TArray<int32>> EdgeFaces;
    for (const TPair<int32, FNGonFace>& Pair : Topo.Faces)
    {
        const TArray<int32>& P = Pair.Value.Perimeter;
        for (int32 i = 0; i < P.Num(); ++i)
        {
            const FIntPoint Key(FMath::Min(P[i], P[(i + 1) % P.Num()]), FMath::Max(P[i], P[(i + 1) % P.Num()]));
            if (const int32* EId = EdgeToId.Find(Key))
            {
                EdgeFaces.FindOrAdd(*EId).AddUnique(Pair.Key);
            }
        }
    }

    // Walk along both endpoints across valence-4 interior vertices (or valence-2/3 boundary chains)
    TSet<int32> Loop;
    Loop.Add(StartEdgeId);

    auto WalkFromVertex = [&](int32 CurrEdge, int32 CurrVert)
    {
        const int32 MaxSteps = Topo.Edges.Num();
        for (int32 Step = 0; Step < MaxSteps; ++Step)
        {
            const TArray<int32>* IncEdges = VertEdges.Find(CurrVert);
            if (!IncEdges) break;

            const TArray<int32>* CurrFaces = EdgeFaces.Find(CurrEdge);
            int32 NextEdge = INDEX_NONE;

            if (IncEdges->Num() == 4 && CurrFaces)
            {
                // Pick the unique edge at CurrVert that does NOT share a face with CurrEdge
                for (const int32 Cand : *IncEdges)
                {
                    if (Cand == CurrEdge) continue;
                    const TArray<int32>* CandFaces = EdgeFaces.Find(Cand);
                    bool bSharesFace = false;
                    if (CandFaces)
                    {
                        for (const int32 FId : *CurrFaces)
                        {
                            if (CandFaces->Contains(FId)) { bSharesFace = true; break; }
                        }
                    }
                    if (!bSharesFace)
                    {
                        NextEdge = Cand;
                        break;
                    }
                }
            }
            else if (CurrFaces && CurrFaces->Num() == 1)
            {
                // Follow open boundary edge chain
                for (const int32 Cand : *IncEdges)
                {
                    if (Cand == CurrEdge) continue;
                    const TArray<int32>* CandFaces = EdgeFaces.Find(Cand);
                    if (CandFaces && CandFaces->Num() == 1)
                    {
                        NextEdge = Cand;
                        break;
                    }
                }
            }

            if (NextEdge == INDEX_NONE || Loop.Contains(NextEdge)) break;
            Loop.Add(NextEdge);
            const FIntPoint NE = Topo.Edges[NextEdge];
            CurrVert = (NE.X == CurrVert) ? NE.Y : NE.X;
            CurrEdge = NextEdge;
        }
    };

    const FIntPoint StartE = Topo.Edges[StartEdgeId];
    WalkFromVertex(StartEdgeId, StartE.X);
    WalkFromVertex(StartEdgeId, StartE.Y);

    Scene.SetSelectionSet(ENGonElement::Edge, Loop, bAdd);
    Res.bOk = true;
    Res.SelectedIds = Scene.GetSelectedIds().Array();
    Res.SelectedIds.Sort();
    Res.Message = FString::Printf(TEXT("Selected edge loop (%d edge(s))."), Loop.Num());
    return Res;
}

FNGonMeshOpResult FNGonMeshAPI::SelectEdgeRing(FNGonPreviewScene& Scene, int32 StartEdgeId, bool bAdd)
{
    FNGonMeshOpResult Res;
    Res.Operation = TEXT("select_edge_ring");

    const FNGonTopology& Topo = Scene.GetTopology();
    if (StartEdgeId == INDEX_NONE && Scene.GetSelectedElement() == ENGonElement::Edge && !Scene.GetSelectedIds().IsEmpty())
    {
        StartEdgeId = *Scene.GetSelectedIds().CreateConstIterator();
    }
    if (!Topo.Edges.IsValidIndex(StartEdgeId))
    {
        Res.Message = TEXT("Select a valid edge (or pass edge_id) for edge ring.");
        return Res;
    }

    if (!Scene.SelectEdgeLoop(StartEdgeId, bAdd))
    {
        Res.Message = TEXT("Failed to trace edge ring from selected edge.");
        return Res;
    }

    Res.bOk = true;
    Res.SelectedIds = Scene.GetSelectedIds().Array();
    Res.SelectedIds.Sort();
    Res.Message = FString::Printf(TEXT("Selected edge ring (%d edge(s))."), Res.SelectedIds.Num());
    return Res;
}

FNGonMeshOpResult FNGonMeshAPI::SelectFaceLoop(FNGonPreviewScene& Scene, int32 StartEdgeId, bool bAdd)
{
    FNGonMeshOpResult Res;
    Res.Operation = TEXT("select_face_loop");

    const FNGonTopology& Topo = Scene.GetTopology();
    TMap<FIntPoint, int32> EdgeToId;
    EdgeToId.Reserve(Topo.Edges.Num());
    for (int32 i = 0; i < Topo.Edges.Num(); ++i)
    {
        EdgeToId.Add(FIntPoint(FMath::Min(Topo.Edges[i].X, Topo.Edges[i].Y), FMath::Max(Topo.Edges[i].X, Topo.Edges[i].Y)), i);
    }

    if (StartEdgeId == INDEX_NONE)
    {
        if (Scene.GetSelectedElement() == ENGonElement::Edge && !Scene.GetSelectedIds().IsEmpty())
        {
            StartEdgeId = *Scene.GetSelectedIds().CreateConstIterator();
        }
        else if (Scene.GetSelectedElement() == ENGonElement::Face && !Scene.GetSelectedIds().IsEmpty())
        {
            // If 2 adjacent faces are selected, use their shared edge; otherwise use first edge of selected face
            TArray<int32> SelFaces = Scene.GetSelectedIds().Array();
            SelFaces.Sort();
            if (SelFaces.Num() >= 2)
            {
                const FNGonFace* FA = Topo.Faces.Find(SelFaces[0]);
                const FNGonFace* FB = Topo.Faces.Find(SelFaces[1]);
                if (FA && FB)
                {
                    for (int32 i = 0; i < FA->Perimeter.Num(); ++i)
                    {
                        const int32 A = FA->Perimeter[i];
                        const int32 B = FA->Perimeter[(i + 1) % FA->Perimeter.Num()];
                        if (FB->Perimeter.Contains(A) && FB->Perimeter.Contains(B))
                        {
                            if (const int32* Shared = EdgeToId.Find(FIntPoint(FMath::Min(A, B), FMath::Max(A, B))))
                            {
                                StartEdgeId = *Shared;
                                break;
                            }
                        }
                    }
                }
            }
            if (StartEdgeId == INDEX_NONE)
            {
                if (const FNGonFace* F = Topo.Faces.Find(SelFaces[0]); F && F->Perimeter.Num() >= 2)
                {
                    const FIntPoint Key(FMath::Min(F->Perimeter[0], F->Perimeter[1]), FMath::Max(F->Perimeter[0], F->Perimeter[1]));
                    if (const int32* E = EdgeToId.Find(Key)) StartEdgeId = *E;
                }
            }
        }
    }

    if (!Topo.Edges.IsValidIndex(StartEdgeId))
    {
        Res.Message = TEXT("Select an edge or face (or pass edge_id) to trace a face loop.");
        return Res;
    }

    TMap<int32, TArray<int32>> EdgeFaces;
    for (const TPair<int32, FNGonFace>& Pair : Topo.Faces)
    {
        const TArray<int32>& P = Pair.Value.Perimeter;
        for (int32 i = 0; i < P.Num(); ++i)
        {
            const FIntPoint Key(FMath::Min(P[i], P[(i + 1) % P.Num()]), FMath::Max(P[i], P[(i + 1) % P.Num()]));
            if (const int32* EId = EdgeToId.Find(Key))
            {
                EdgeFaces.FindOrAdd(*EId).AddUnique(Pair.Key);
            }
        }
    }

    TSet<int32> VisitedEdges;
    TSet<int32> FaceLoop;
    TArray<int32> Queue{ StartEdgeId };
    while (!Queue.IsEmpty())
    {
        const int32 EdgeId = Queue.Pop(EAllowShrinking::No);
        if (VisitedEdges.Contains(EdgeId)) continue;
        VisitedEdges.Add(EdgeId);

        const TArray<int32>* AdjFaces = EdgeFaces.Find(EdgeId);
        if (!AdjFaces) continue;

        for (const int32 FId : *AdjFaces)
        {
            FaceLoop.Add(FId);
            const FNGonFace* Face = Topo.Faces.Find(FId);
            if (!Face || Face->Perimeter.Num() != 4) continue;

            const FIntPoint E = Topo.Edges[EdgeId];
            int32 Corner = INDEX_NONE;
            for (int32 i = 0; i < 4; ++i)
            {
                const int32 A = Face->Perimeter[i];
                const int32 B = Face->Perimeter[(i + 1) % 4];
                if (FMath::Min(A, B) == FMath::Min(E.X, E.Y) && FMath::Max(A, B) == FMath::Max(E.X, E.Y))
                {
                    Corner = i;
                    break;
                }
            }
            if (Corner != INDEX_NONE)
            {
                const int32 OppA = Face->Perimeter[(Corner + 2) % 4];
                const int32 OppB = Face->Perimeter[(Corner + 3) % 4];
                if (const int32* OppEdge = EdgeToId.Find(FIntPoint(FMath::Min(OppA, OppB), FMath::Max(OppA, OppB))))
                {
                    if (!VisitedEdges.Contains(*OppEdge)) Queue.Add(*OppEdge);
                }
            }
        }
    }

    Scene.SetSelectionSet(ENGonElement::Face, FaceLoop, bAdd);
    Res.bOk = true;
    Res.SelectedIds = Scene.GetSelectedIds().Array();
    Res.SelectedIds.Sort();
    Res.Message = FString::Printf(TEXT("Selected face loop (%d face(s))."), FaceLoop.Num());
    return Res;
}

FNGonMeshOpResult FNGonMeshAPI::SelectLinkedShell(FNGonPreviewScene& Scene)
{
    FNGonMeshOpResult Res;
    Res.Operation = TEXT("select_linked_shell");

    const TArray<int32> SeedVerts = ResolveTargetVertices(Scene, {});
    if (SeedVerts.IsEmpty())
    {
        Res.Message = TEXT("Select at least one vertex, edge, or face to select its linked shell.");
        return Res;
    }

    const FNGonShellAnalysis Analysis = AnalyzeShells(Scene);
    const TSet<int32> SeedSet(SeedVerts);
    const ENGonElement Elem = Scene.GetSelectedElement();
    TSet<int32> LinkedIds;

    for (const FNGonMeshShell& Shell : Analysis.Shells)
    {
        bool bContainsSeed = false;
        for (const int32 V : Shell.Vertices)
        {
            if (SeedSet.Contains(V)) { bContainsSeed = true; break; }
        }
        if (!bContainsSeed) continue;

        if (Elem == ENGonElement::Vertex)
        {
            for (const int32 V : Shell.Vertices) LinkedIds.Add(V);
        }
        else if (Elem == ENGonElement::Edge)
        {
            for (const int32 E : Shell.Edges) LinkedIds.Add(E);
        }
        else
        {
            for (const int32 F : Shell.Faces) LinkedIds.Add(F);
        }
    }

    Scene.SetSelectionSet(Elem, LinkedIds, false);
    Res.bOk = true;
    Res.SelectedIds = LinkedIds.Array();
    Res.SelectedIds.Sort();
    Res.Message = FString::Printf(TEXT("Selected linked shell (%d element(s))."), LinkedIds.Num());
    return Res;
}

namespace
{
int32 FindEdgeBetweenVertices(const FNGonTopology& Topo, int32 V0, int32 V1)
{
    if (V0 == V1 || V0 < 0 || V1 < 0) return INDEX_NONE;
    for (int32 e = 0; e < Topo.Edges.Num(); ++e)
    {
        const FIntPoint& E = Topo.Edges[e];
        if ((E.X == V0 && E.Y == V1) || (E.X == V1 && E.Y == V0))
        {
            return e;
        }
    }
    return INDEX_NONE;
}

int32 FindSharedEdgeBetweenFaces(const FNGonTopology& Topo, int32 FaceA, int32 FaceB)
{
    const FNGonFace* FA = Topo.Faces.Find(FaceA);
    const FNGonFace* FB = Topo.Faces.Find(FaceB);
    if (!FA || !FB || FA->Perimeter.Num() < 3 || FB->Perimeter.Num() < 3) return INDEX_NONE;

    TSet<uint64> EdgesA;
    const int32 NA = FA->Perimeter.Num();
    for (int32 i = 0; i < NA; ++i)
    {
        const int32 A = FA->Perimeter[i];
        const int32 B = FA->Perimeter[(i + 1) % NA];
        const uint64 Key = (static_cast<uint64>(FMath::Min(A, B)) << 32) | static_cast<uint32>(FMath::Max(A, B));
        EdgesA.Add(Key);
    }

    const int32 NB = FB->Perimeter.Num();
    for (int32 i = 0; i < NB; ++i)
    {
        const int32 A = FB->Perimeter[i];
        const int32 B = FB->Perimeter[(i + 1) % NB];
        const uint64 Key = (static_cast<uint64>(FMath::Min(A, B)) << 32) | static_cast<uint32>(FMath::Max(A, B));
        if (EdgesA.Contains(Key))
        {
            return FindEdgeBetweenVertices(Topo, A, B);
        }
    }
    return INDEX_NONE;
}

bool AreEdgesOppositeInQuad(const FNGonTopology& Topo, int32 EdgeA, int32 EdgeB)
{
    if (!Topo.Edges.IsValidIndex(EdgeA) || !Topo.Edges.IsValidIndex(EdgeB) || EdgeA == EdgeB) return false;
    const FIntPoint EA = Topo.Edges[EdgeA];
    const FIntPoint EB = Topo.Edges[EdgeB];
    for (const TPair<int32, FNGonFace>& Pair : Topo.Faces)
    {
        const TArray<int32>& P = Pair.Value.Perimeter;
        if (P.Num() != 4) continue;
        int32 IdxA = INDEX_NONE, IdxB = INDEX_NONE;
        for (int32 i = 0; i < 4; ++i)
        {
            const int32 U = P[i];
            const int32 V = P[(i + 1) % 4];
            if ((U == EA.X && V == EA.Y) || (U == EA.Y && V == EA.X)) IdxA = i;
            if ((U == EB.X && V == EB.Y) || (U == EB.Y && V == EB.X)) IdxB = i;
        }
        if (IdxA != INDEX_NONE && IdxB != INDEX_NONE && ((IdxA + 2) % 4 == IdxB))
        {
            return true;
        }
    }
    return false;
}

bool AreEdgesLoopAdjacent(const FNGonTopology& Topo, int32 EdgeA, int32 EdgeB)
{
    if (!Topo.Edges.IsValidIndex(EdgeA) || !Topo.Edges.IsValidIndex(EdgeB) || EdgeA == EdgeB) return false;
    const FIntPoint EA = Topo.Edges[EdgeA];
    const FIntPoint EB = Topo.Edges[EdgeB];
    int32 SharedV = INDEX_NONE;
    if (EA.X == EB.X || EA.X == EB.Y) SharedV = EA.X;
    else if (EA.Y == EB.X || EA.Y == EB.Y) SharedV = EA.Y;
    if (SharedV == INDEX_NONE) return false;

    // Loop continuation edges share a vertex but do NOT share a face
    for (const TPair<int32, FNGonFace>& Pair : Topo.Faces)
    {
        const TArray<int32>& P = Pair.Value.Perimeter;
        bool bHasA = false, bHasB = false;
        const int32 N = P.Num();
        for (int32 i = 0; i < N; ++i)
        {
            const int32 U = P[i];
            const int32 V = P[(i + 1) % N];
            if ((U == EA.X && V == EA.Y) || (U == EA.Y && V == EA.X)) bHasA = true;
            if ((U == EB.X && V == EB.Y) || (U == EB.Y && V == EB.X)) bHasB = true;
        }
        if (bHasA && bHasB)
        {
            return false;
        }
    }
    return true;
}

TArray<int32> RunDijkstraPath(int32 NodeCount, int32 StartNode, int32 EndNode, const TArray<TArray<TPair<int32, double>>>& Adj)
{
    TArray<int32> Path;
    if (StartNode < 0 || StartNode >= NodeCount || EndNode < 0 || EndNode >= NodeCount) return Path;
    if (StartNode == EndNode) { Path.Add(StartNode); return Path; }

    TArray<double> Dist;
    TArray<int32> Prev;
    TArray<uint8> Visited;
    Dist.Init(TNumericLimits<double>::Max(), NodeCount);
    Prev.Init(INDEX_NONE, NodeCount);
    Visited.Init(0, NodeCount);

    struct FHeapEntry
    {
        double Cost;
        int32 Node;
        bool operator<(const FHeapEntry& Other) const { return Cost < Other.Cost; }
    };
    TArray<FHeapEntry> Heap;
    Dist[StartNode] = 0.0;
    Heap.HeapPush({ 0.0, StartNode });

    while (!Heap.IsEmpty())
    {
        FHeapEntry Top;
        Heap.HeapPop(Top, EAllowShrinking::No);
        if (Visited[Top.Node]) continue;
        Visited[Top.Node] = 1;
        if (Top.Node == EndNode) break;

        for (const TPair<int32, double>& Link : Adj[Top.Node])
        {
            const int32 Next = Link.Key;
            if (Visited[Next]) continue;
            const double Cand = Top.Cost + FMath::Max(Link.Value, 1.e-6);
            if (Cand < Dist[Next])
            {
                Dist[Next] = Cand;
                Prev[Next] = Top.Node;
                Heap.HeapPush({ Cand, Next });
            }
        }
    }

    if (Prev[EndNode] == INDEX_NONE) return Path;
    for (int32 Cur = EndNode; Cur != INDEX_NONE; Cur = Prev[Cur])
    {
        Path.Add(Cur);
    }
    Algo::Reverse(Path);
    return Path;
}
} // anonymous namespace

FNGonMeshOpResult FNGonMeshAPI::SelectVertexLoop(FNGonPreviewScene& Scene, int32 AnchorVert, int32 TargetVert, bool bAdd)
{
    FNGonMeshOpResult Res;
    Res.Operation = TEXT("select_vertex_loop");
    const FNGonTopology& Topo = Scene.GetTopology();

    int32 StartEdge = FindEdgeBetweenVertices(Topo, AnchorVert, TargetVert);
    if (StartEdge == INDEX_NONE && Scene.GetSelectedElement() == ENGonElement::Vertex)
    {
        const TArray<int32> SelArr = Scene.GetSelectedIds().Array();
        const int32 Active = Scene.GetActiveSelectedId();
        for (int32 i = 0; i < SelArr.Num() && StartEdge == INDEX_NONE; ++i)
        {
            if (Active != INDEX_NONE && SelArr[i] != Active)
            {
                StartEdge = FindEdgeBetweenVertices(Topo, Active, SelArr[i]);
            }
        }
        for (int32 i = 0; i < SelArr.Num() && StartEdge == INDEX_NONE; ++i)
        {
            for (int32 j = i + 1; j < SelArr.Num() && StartEdge == INDEX_NONE; ++j)
            {
                StartEdge = FindEdgeBetweenVertices(Topo, SelArr[i], SelArr[j]);
            }
        }
    }

    if (StartEdge == INDEX_NONE)
    {
        Res.Message = TEXT("Select two vertices that share an edge to select a vertex loop.");
        return Res;
    }

    const TSet<int32> PrevSel = bAdd && Scene.GetSelectedElement() == ENGonElement::Vertex ? Scene.GetSelectedIds() : TSet<int32>();
    if (!FNGonMeshAPI::SelectEdgeLoop(Scene, StartEdge, false).bOk)
    {
        Res.Message = TEXT("Could not trace a quad vertex loop from the selected edge.");
        return Res;
    }

    TSet<int32> LoopVerts = PrevSel;
    for (const int32 EdgeId : Scene.GetSelectedIds())
    {
        if (Topo.Edges.IsValidIndex(EdgeId))
        {
            LoopVerts.Add(Topo.Edges[EdgeId].X);
            LoopVerts.Add(Topo.Edges[EdgeId].Y);
        }
    }
    Scene.SetSelectionSet(ENGonElement::Vertex, LoopVerts, false);
    Res.bOk = !LoopVerts.IsEmpty();
    Res.Message = FString::Printf(TEXT("Selected vertex loop (%d vertices)."), LoopVerts.Num());
    return Res;
}

FNGonMeshOpResult FNGonMeshAPI::SelectShortestPath(FNGonPreviewScene& Scene, int32 AnchorId, int32 TargetId, bool bAdd)
{
    FNGonMeshOpResult Res;
    Res.Operation = TEXT("select_shortest_path");
    const FNGonTopology& Topo = Scene.GetTopology();
    const TArray<FVector>& Pos = Scene.GetPositions();
    const ENGonElement Elem = Scene.GetSelectedElement();

    if (AnchorId == INDEX_NONE || TargetId == INDEX_NONE)
    {
        const TArray<int32> SelArr = Scene.GetSelectedIds().Array();
        if (SelArr.Num() < 2)
        {
            Res.Message = TEXT("Select at least 2 elements (start and end) for shortest path.");
            return Res;
        }
        const int32 Active = Scene.GetActiveSelectedId();
        TargetId = (Active != INDEX_NONE && Scene.GetSelectedIds().Contains(Active)) ? Active : SelArr.Last();
        AnchorId = (SelArr[0] != TargetId) ? SelArr[0] : SelArr[1];
    }

    TSet<int32> PathSet;
    if (bAdd)
    {
        PathSet = Scene.GetSelectedIds();
    }

    if (Elem == ENGonElement::Vertex)
    {
        const int32 N = Pos.Num();
        TArray<TArray<TPair<int32, double>>> Adj;
        Adj.SetNum(N);
        for (const FIntPoint& E : Topo.Edges)
        {
            if (Pos.IsValidIndex(E.X) && Pos.IsValidIndex(E.Y))
            {
                const double Len = FVector::Distance(Pos[E.X], Pos[E.Y]);
                Adj[E.X].Add({ E.Y, Len });
                Adj[E.Y].Add({ E.X, Len });
            }
        }
        const TArray<int32> Path = RunDijkstraPath(N, AnchorId, TargetId, Adj);
        for (const int32 V : Path) PathSet.Add(V);
    }
    else if (Elem == ENGonElement::Edge)
    {
        const int32 NE = Topo.Edges.Num();
        TArray<TArray<int32>> VertToEdges;
        VertToEdges.SetNum(Pos.Num());
        TArray<FVector> Midpoints;
        Midpoints.SetNum(NE);
        for (int32 e = 0; e < NE; ++e)
        {
            const FIntPoint& E = Topo.Edges[e];
            if (Pos.IsValidIndex(E.X) && Pos.IsValidIndex(E.Y))
            {
                VertToEdges[E.X].Add(e);
                VertToEdges[E.Y].Add(e);
                Midpoints[e] = (Pos[E.X] + Pos[E.Y]) * 0.5;
            }
        }
        TArray<TArray<TPair<int32, double>>> Adj;
        Adj.SetNum(NE);
        for (const TArray<int32>& Inc : VertToEdges)
        {
            for (int32 i = 0; i < Inc.Num(); ++i)
            {
                for (int32 j = i + 1; j < Inc.Num(); ++j)
                {
                    const int32 A = Inc[i], B = Inc[j];
                    const double D = FVector::Distance(Midpoints[A], Midpoints[B]);
                    Adj[A].Add({ B, D });
                    Adj[B].Add({ A, D });
                }
            }
        }
        const TArray<int32> Path = RunDijkstraPath(NE, AnchorId, TargetId, Adj);
        for (const int32 E : Path) PathSet.Add(E);
    }
    else
    {
        TArray<int32> FaceIds;
        TMap<int32, int32> FaceToDense;
        Topo.Faces.GetKeys(FaceIds);
        const int32 NF = FaceIds.Num();
        TArray<FVector> Centroids;
        Centroids.SetNum(NF);
        TMap<uint64, TArray<int32, TInlineAllocator<2>>> EdgeToFaces;
        for (int32 i = 0; i < NF; ++i)
        {
            FaceToDense.Add(FaceIds[i], i);
            const FNGonFace& F = Topo.Faces[FaceIds[i]];
            FVector C = FVector::ZeroVector;
            int32 Cnt = 0;
            const int32 PNum = F.Perimeter.Num();
            for (int32 k = 0; k < PNum; ++k)
            {
                const int32 A = F.Perimeter[k];
                const int32 B = F.Perimeter[(k + 1) % PNum];
                if (Pos.IsValidIndex(A)) { C += Pos[A]; ++Cnt; }
                const uint64 Key = (static_cast<uint64>(FMath::Min(A, B)) << 32) | static_cast<uint32>(FMath::Max(A, B));
                EdgeToFaces.FindOrAdd(Key).AddUnique(i);
            }
            Centroids[i] = Cnt > 0 ? (C / static_cast<double>(Cnt)) : FVector::ZeroVector;
        }
        const int32* StartDense = FaceToDense.Find(AnchorId);
        const int32* EndDense = FaceToDense.Find(TargetId);
        if (StartDense && EndDense)
        {
            TArray<TArray<TPair<int32, double>>> Adj;
            Adj.SetNum(NF);
            for (const auto& Pair : EdgeToFaces)
            {
                const auto& FList = Pair.Value;
                for (int32 i = 0; i < FList.Num(); ++i)
                {
                    for (int32 j = i + 1; j < FList.Num(); ++j)
                    {
                        const int32 A = FList[i], B = FList[j];
                        const double D = FVector::Distance(Centroids[A], Centroids[B]);
                        Adj[A].Add({ B, D });
                        Adj[B].Add({ A, D });
                    }
                }
            }
            const TArray<int32> DensePath = RunDijkstraPath(NF, *StartDense, *EndDense, Adj);
            for (const int32 DIdx : DensePath) PathSet.Add(FaceIds[DIdx]);
        }
    }

    if (PathSet.IsEmpty())
    {
        Res.Message = TEXT("No connected shortest path found between the two elements.");
        return Res;
    }

    Scene.SetSelectionSet(Elem, PathSet, false);
    Res.bOk = true;
    Res.Message = FString::Printf(TEXT("Selected shortest path (%d items)."), PathSet.Num());
    return Res;
}

FNGonMeshOpResult FNGonMeshAPI::SelectDoubleClickGesture(
    FNGonPreviewScene& Scene, ENGonElement Element, int32 AnchorId, int32 TargetId, bool bAdd)
{
    FNGonMeshOpResult Res;
    Res.Operation = TEXT("double_click_gesture");
    const FNGonTopology& Topo = Scene.GetTopology();

    if (TargetId == INDEX_NONE)
    {
        return Res;
    }

    if (Element == ENGonElement::Vertex)
    {
        if (AnchorId != INDEX_NONE && AnchorId != TargetId)
        {
            if (FindEdgeBetweenVertices(Topo, AnchorId, TargetId) != INDEX_NONE)
            {
                return SelectVertexLoop(Scene, AnchorId, TargetId, bAdd);
            }
            // Not sharing an edge -> shortest path of vertices!
            TSet<int32> Init = bAdd ? Scene.GetSelectedIds() : TSet<int32>();
            Init.Add(AnchorId);
            Init.Add(TargetId);
            Scene.SetSelectionSet(ENGonElement::Vertex, Init, false);
            return SelectShortestPath(Scene, AnchorId, TargetId, bAdd);
        }
        // Single vertex double-click with no separate anchor: select vertex
        TSet<int32> One;
        One.Add(TargetId);
        Scene.SetSelectionSet(ENGonElement::Vertex, One, bAdd);
        Res.bOk = true;
        return Res;
    }

    if (Element == ENGonElement::Edge)
    {
        if (AnchorId != INDEX_NONE && AnchorId != TargetId)
        {
            if (AreEdgesLoopAdjacent(Topo, AnchorId, TargetId))
            {
                return SelectEdgeLoop(Scene, TargetId, bAdd);
            }
            if (AreEdgesOppositeInQuad(Topo, AnchorId, TargetId))
            {
                return SelectEdgeRing(Scene, TargetId, bAdd);
            }
            // Non-adjacent edges -> shortest path of edges!
            TSet<int32> Init = bAdd ? Scene.GetSelectedIds() : TSet<int32>();
            Init.Add(AnchorId);
            Init.Add(TargetId);
            Scene.SetSelectionSet(ENGonElement::Edge, Init, false);
            return SelectShortestPath(Scene, AnchorId, TargetId, bAdd);
        }
        // Classic Maya single-edge double-click -> Edge Loop
        return SelectEdgeLoop(Scene, TargetId, bAdd);
    }

    // Face mode
    if (AnchorId != INDEX_NONE && AnchorId != TargetId)
    {
        const int32 SharedEdge = FindSharedEdgeBetweenFaces(Topo, AnchorId, TargetId);
        if (SharedEdge != INDEX_NONE)
        {
            return SelectFaceLoop(Scene, SharedEdge, bAdd);
        }
        // Non-adjacent faces -> shortest path of faces!
        TSet<int32> Init = bAdd ? Scene.GetSelectedIds() : TSet<int32>();
        Init.Add(AnchorId);
        Init.Add(TargetId);
        Scene.SetSelectionSet(ENGonElement::Face, Init, false);
        return SelectShortestPath(Scene, AnchorId, TargetId, bAdd);
    }
    // Single face double-click -> Linked Shell
    TSet<int32> OneFace;
    OneFace.Add(TargetId);
    Scene.SetSelectionSet(ENGonElement::Face, OneFace, bAdd);
    return SelectLinkedShell(Scene);
}

FNGonMeshOpResult FNGonMeshAPI::ConvertSelection(FNGonPreviewScene& Scene, ENGonElement TargetElement)
{
    FNGonMeshOpResult Res;
    Res.Operation = TEXT("convert_selection");

    const ENGonElement SrcElem = Scene.GetSelectedElement();
    if (SrcElem == TargetElement)
    {
        Res.bOk = true;
        Res.SelectedIds = Scene.GetSelectedIds().Array();
        Res.SelectedIds.Sort();
        Res.Message = TEXT("Selection is already in the target element mode.");
        return Res;
    }

    const TArray<int32> Verts = ResolveTargetVertices(Scene, {});
    const TSet<int32> VertSet(Verts);
    const FNGonTopology& Topo = Scene.GetTopology();
    TSet<int32> Converted;

    if (TargetElement == ENGonElement::Vertex)
    {
        Converted = VertSet;
    }
    else if (TargetElement == ENGonElement::Edge)
    {
        for (int32 e = 0; e < Topo.Edges.Num(); ++e)
        {
            if (VertSet.Contains(Topo.Edges[e].X) && VertSet.Contains(Topo.Edges[e].Y))
            {
                Converted.Add(e);
            }
        }
    }
    else
    {
        for (const TPair<int32, FNGonFace>& Pair : Topo.Faces)
        {
            bool bAllVerts = !Pair.Value.Perimeter.IsEmpty();
            for (const int32 V : Pair.Value.Perimeter)
            {
                if (!VertSet.Contains(V)) { bAllVerts = false; break; }
            }
            if (bAllVerts) Converted.Add(Pair.Key);
        }
    }

    Scene.SetSelectionSet(TargetElement, Converted, false);
    Res.bOk = true;
    Res.SelectedIds = Converted.Array();
    Res.SelectedIds.Sort();
    Res.Message = FString::Printf(TEXT("Converted selection to %s (%d element(s))."),
        TargetElement == ENGonElement::Vertex ? TEXT("Vertex") : (TargetElement == ENGonElement::Edge ? TEXT("Edge") : TEXT("Face")),
        Converted.Num());
    return Res;
}

// ============================================================================
// Topology & Manifold Analysis (Ported from TiMesh::MeshShells & CheckManifold)
// ============================================================================

FNGonShellAnalysis FNGonMeshAPI::AnalyzeShells(const FNGonPreviewScene& Scene)
{
    FNGonShellAnalysis Out;
    const FNGonTopology& Topo = Scene.GetTopology();
    const TArray<FVector>& Positions = Scene.GetPositions();
    const int32 NumVerts = Positions.Num();
    if (NumVerts == 0) return Out;

    TArray<int32> Parent;
    Parent.SetNum(NumVerts);
    for (int32 i = 0; i < NumVerts; ++i) Parent[i] = i;

    auto FindRoot = [&](int32 X) -> int32
    {
        int32 R = X;
        while (Parent[R] != R) R = Parent[R];
        while (Parent[X] != R)
        {
            const int32 Next = Parent[X];
            Parent[X] = R;
            X = Next;
        }
        return R;
    };

    auto UnionVerts = [&](int32 A, int32 B)
    {
        const int32 RA = FindRoot(A);
        const int32 RB = FindRoot(B);
        if (RA != RB)
        {
            if (RA < RB) Parent[RB] = RA;
            else Parent[RA] = RB;
        }
    };

    TBitArray<> UsedVert(false, NumVerts);
    TMap<FIntPoint, int32> EdgeFaceCount;
    for (const TPair<int32, FNGonFace>& Pair : Topo.Faces)
    {
        const TArray<int32>& P = Pair.Value.Perimeter;
        if (P.Num() > 4) ++Out.NGonFaceCount;
        for (int32 i = 0; i < P.Num(); ++i)
        {
            const int32 A = P[i];
            const int32 B = P[(i + 1) % P.Num()];
            if (Positions.IsValidIndex(A)) UsedVert[A] = true;
            if (Positions.IsValidIndex(B)) UsedVert[B] = true;
            if (Positions.IsValidIndex(A) && Positions.IsValidIndex(B))
            {
                UnionVerts(A, B);
                ++EdgeFaceCount.FindOrAdd(FIntPoint(FMath::Min(A, B), FMath::Max(A, B)));
            }
        }
    }

    TMap<int32, int32> RootToShell;
    for (int32 v = 0; v < NumVerts; ++v)
    {
        if (!UsedVert[v])
        {
            Out.LooseVertices.Add(v);
            continue;
        }
        const int32 Root = FindRoot(v);
        int32* ShellIdx = RootToShell.Find(Root);
        if (!ShellIdx)
        {
            FNGonMeshShell Shell;
            Shell.AnchorVertex = Root;
            ShellIdx = &RootToShell.Add(Root, Out.Shells.Add(MoveTemp(Shell)));
        }
        FNGonMeshShell& S = Out.Shells[*ShellIdx];
        S.Vertices.Add(v);
        S.Bounds += Positions[v];
    }

    for (const TPair<int32, FNGonFace>& Pair : Topo.Faces)
    {
        if (Pair.Value.Perimeter.IsEmpty()) continue;
        const int32 Root = FindRoot(Pair.Value.Perimeter[0]);
        if (const int32* ShellIdx = RootToShell.Find(Root))
        {
            Out.Shells[*ShellIdx].Faces.Add(Pair.Key);
            Out.Shells[*ShellIdx].TriangleCount += Pair.Value.Triangles.Num();
        }
    }

    for (int32 e = 0; e < Topo.Edges.Num(); ++e)
    {
        const FIntPoint Key(FMath::Min(Topo.Edges[e].X, Topo.Edges[e].Y), FMath::Max(Topo.Edges[e].X, Topo.Edges[e].Y));
        const int32 Incident = EdgeFaceCount.FindRef(Key);
        if (Incident == 1) ++Out.TotalBoundaryEdges;
        else if (Incident > 2) ++Out.TotalNonManifoldEdges;

        if (Positions.IsValidIndex(Topo.Edges[e].X))
        {
            const int32 Root = FindRoot(Topo.Edges[e].X);
            if (const int32* ShellIdx = RootToShell.Find(Root))
            {
                FNGonMeshShell& S = Out.Shells[*ShellIdx];
                S.Edges.Add(e);
                if (Incident == 1) ++S.BoundaryEdges;
                else if (Incident > 2) ++S.NonManifoldEdges;
            }
        }
    }

    return Out;
}

FNGonValidationReport FNGonMeshAPI::CheckManifold(const FNGonPreviewScene& Scene, bool bOriented)
{
    FNGonValidationReport Report;
    const FNGonTopology& Topo = Scene.GetTopology();
    const TArray<FVector>& Positions = Scene.GetPositions();

    for (int32 v = 0; v < Positions.Num(); ++v)
    {
        if (Positions[v].ContainsNaN())
        {
            Report.Issues.Add({ TEXT("NAN_VERTEX"), FString::Printf(TEXT("Vertex #%d has NaN coordinates."), v) });
        }
    }

    TMap<FIntPoint, int32> UndirectedCount;
    TMap<FIntPoint, int32> DirectedCount;
    TSet<FString> FaceSignatures;

    for (const TPair<int32, FNGonFace>& Pair : Topo.Faces)
    {
        const TArray<int32>& P = Pair.Value.Perimeter;
        if (P.Num() < 3)
        {
            Report.Issues.Add({ TEXT("SHORT_FACE"), FString::Printf(TEXT("Face #%d has fewer than 3 vertices."), Pair.Key) });
            continue;
        }

        TArray<int32> Sorted = P;
        Sorted.Sort();
        FString Sig;
        for (const int32 V : Sorted) Sig += FString::Printf(TEXT("%d,"), V);
        bool bDup = false;
        FaceSignatures.Add(Sig, &bDup);
        if (bDup)
        {
            Report.Issues.Add({ TEXT("DUPLICATE_FACE"), FString::Printf(TEXT("Face #%d duplicates an existing face."), Pair.Key) });
        }

        for (int32 i = 0; i < P.Num(); ++i)
        {
            const int32 A = P[i];
            const int32 B = P[(i + 1) % P.Num()];
            ++UndirectedCount.FindOrAdd(FIntPoint(FMath::Min(A, B), FMath::Max(A, B)));
            ++DirectedCount.FindOrAdd(FIntPoint(A, B));
        }
    }

    for (const TPair<FIntPoint, int32>& Pair : UndirectedCount)
    {
        if (Pair.Value > 2)
        {
            Report.Issues.Add({ TEXT("NON_MANIFOLD_EDGE"),
                FString::Printf(TEXT("Edge (%d-%d) is shared by %d faces."), Pair.Key.X, Pair.Key.Y, Pair.Value) });
        }
    }

    if (bOriented)
    {
        for (const TPair<FIntPoint, int32>& Pair : DirectedCount)
        {
            if (Pair.Value > 1)
            {
                Report.Issues.Add({ TEXT("ORIENTATION_CONFLICT"),
                    FString::Printf(TEXT("Directed edge (%d->%d) appears %d times (inconsistent winding)."), Pair.Key.X, Pair.Key.Y, Pair.Value) });
            }
        }
    }

    return Report;
}

// ============================================================================
// AI JSON Inspection, Schema, and Command Execution
// ============================================================================

FString FNGonMeshAPI::ExportMeshSummaryJson(const FNGonPreviewScene& Scene, bool bIncludeGeometry)
{
    const FNGonTopology& Topo = Scene.GetTopology();
    const TArray<FVector>& Positions = Scene.GetPositions();
    const FBox Bounds = Scene.GetMeshBounds();
    const FNGonShellAnalysis Shells = AnalyzeShells(Scene);
    const FNGonValidationReport Validation = CheckManifold(Scene, true);

    TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
    Root->SetNumberField(TEXT("vertex_count"), Positions.Num());
    Root->SetNumberField(TEXT("edge_count"), Topo.Edges.Num());
    Root->SetNumberField(TEXT("face_count"), Topo.Faces.Num());
    Root->SetNumberField(TEXT("triangle_count"), Topo.TriangleCount);
    Root->SetNumberField(TEXT("ngon_face_count"), Shells.NGonFaceCount);
    Root->SetNumberField(TEXT("max_face_corners"), Topo.MaxFaceCorners);
    Root->SetNumberField(TEXT("shell_count"), Shells.Shells.Num());
    Root->SetNumberField(TEXT("boundary_edge_count"), Shells.TotalBoundaryEdges);
    Root->SetNumberField(TEXT("non_manifold_edge_count"), Shells.TotalNonManifoldEdges);
    Root->SetBoolField(TEXT("manifold_ok"), Validation.IsOk());
    Root->SetNumberField(TEXT("undo_depth"), GetUndoCount(&Scene));
    Root->SetNumberField(TEXT("redo_depth"), GetRedoCount(&Scene));

    if (Bounds.IsValid)
    {
        TSharedRef<FJsonObject> BoundsObj = MakeShared<FJsonObject>();
        BoundsObj->SetObjectField(TEXT("min"), MakeVectorJson(Bounds.Min));
        BoundsObj->SetObjectField(TEXT("max"), MakeVectorJson(Bounds.Max));
        BoundsObj->SetObjectField(TEXT("size"), MakeVectorJson(Bounds.GetSize()));
        BoundsObj->SetObjectField(TEXT("center"), MakeVectorJson(Bounds.GetCenter()));
        Root->SetObjectField(TEXT("bounds"), BoundsObj);
    }

    const ENGonElement SelElem = Scene.GetSelectedElement();
    Root->SetStringField(TEXT("selection_mode"),
        SelElem == ENGonElement::Vertex ? TEXT("vertex") : (SelElem == ENGonElement::Edge ? TEXT("edge") : TEXT("face")));
    TArray<int32> SelArr = Scene.GetSelectedIds().Array();
    SelArr.Sort();
    Root->SetArrayField(TEXT("selected_ids"), MakeIntArrayJson(SelArr));

    if (bIncludeGeometry)
    {
        TArray<TSharedPtr<FJsonValue>> FaceList;
        TArray<int32> FaceKeys;
        Topo.Faces.GetKeys(FaceKeys);
        FaceKeys.Sort();
        const int32 MaxFaces = FMath::Min(FaceKeys.Num(), 256);
        for (int32 i = 0; i < MaxFaces; ++i)
        {
            const int32 FId = FaceKeys[i];
            if (const FNGonFace* F = Topo.Faces.Find(FId))
            {
                TSharedRef<FJsonObject> FObj = MakeShared<FJsonObject>();
                FObj->SetNumberField(TEXT("id"), FId);
                FObj->SetArrayField(TEXT("verts"), MakeIntArrayJson(F->Perimeter));
                FObj->SetObjectField(TEXT("center"), MakeVectorJson(ComputePolygonCentroid(Positions, F->Perimeter)));
                FObj->SetObjectField(TEXT("normal"), MakeVectorJson(ComputePolygonNormal(Positions, F->Perimeter)));
                FaceList.Add(MakeShared<FJsonValueObject>(FObj));
            }
        }
        Root->SetArrayField(TEXT("faces"), FaceList);
    }

    return SerializeJsonObject(Root, true);
}

FString FNGonMeshAPI::GetApiSchemaJson()
{
    return TEXT(
        "{\n"
        "  \"api\": \"NGonMeshAPI\",\n"
        "  \"version\": \"1.0\",\n"
        "  \"format\": \"Single JSON object {\\\"op\\\": ...} or JSON array [{\\\"op\\\": ...}, ...]\",\n"
        "  \"operations\": [\n"
        "    {\"op\": \"create_box\", \"params\": {\"width\": 100, \"depth\": 100, \"height\": 100, \"subdiv_x\": 1, \"subdiv_y\": 1, \"subdiv_z\": 1, \"center\": [0,0,0]}},\n"
        "    {\"op\": \"create_plane\", \"params\": {\"width\": 200, \"depth\": 200, \"subdiv_x\": 4, \"subdiv_y\": 4, \"center\": [0,0,0]}},\n"
        "    {\"op\": \"create_cylinder\", \"params\": {\"radius\": 50, \"height\": 120, \"sides\": 16, \"height_segments\": 1, \"center\": [0,0,0]}},\n"
        "    {\"op\": \"create_sphere\", \"params\": {\"radius\": 60, \"rings\": 10, \"segments\": 16, \"center\": [0,0,0]}},\n"
        "    {\"op\": \"extrude_faces\", \"params\": {\"face_ids\": [], \"distance\": 25, \"scale\": 1.0}},\n"
        "    {\"op\": \"inset_faces\", \"params\": {\"face_ids\": [], \"ratio\": 0.25, \"depth\": 0.0}},\n"
        "    {\"op\": \"bevel_faces\", \"params\": {\"face_ids\": [], \"ratio\": 0.18, \"height\": 8.0}},\n"
        "    {\"op\": \"bevel_edges\", \"params\": {\"edge_ids\": [], \"width\": 5.0, \"segments\": 1, \"profile\": 0.5, \"clamp_overlap\": true}},\n"
        "    {\"op\": \"bevel_vertices\", \"params\": {\"vertex_ids\": [], \"width\": 5.0, \"segments\": 1, \"profile\": 0.5, \"clamp_overlap\": true}},\n"
        "    {\"op\": \"bevel_selection\", \"params\": {\"width\": 5.0, \"segments\": 1, \"profile\": 0.5, \"clamp_overlap\": true}},\n"
        "    {\"op\": \"subdivide_faces\", \"params\": {\"face_ids\": []}},\n"
        "    {\"op\": \"triangulate_faces\", \"params\": {\"face_ids\": []}},\n"
        "    {\"op\": \"flip_faces\", \"params\": {\"face_ids\": []}},\n"
        "    {\"op\": \"delete_faces\", \"params\": {\"face_ids\": []}},\n"
        "    {\"op\": \"split_edge\", \"params\": {\"edge_id\": 0, \"t\": 0.5}},\n"
        "    {\"op\": \"cut_face\", \"params\": {\"face_id\": 0, \"vertex_a\": 0, \"vertex_b\": 2}},\n"
        "    {\"op\": \"collapse_edge\", \"params\": {\"edge_id\": 0}},\n"
        "    {\"op\": \"weld_vertices\", \"params\": {\"vertex_ids\": [], \"threshold\": 0.0}},\n"
        "    {\"op\": \"bridge_edges\", \"params\": {\"edge_a\": 0, \"edge_b\": 1}},\n"
        "    {\"op\": \"fill_hole\", \"params\": {\"vertex_ids\": []}},\n"
        "    {\"op\": \"mirror_mesh\", \"params\": {\"axis\": \"X\", \"keep_original\": true}},\n"
        "    {\"op\": \"transform_vertices\", \"params\": {\"vertex_ids\": [], \"translate\": [0,0,0], \"rotate\": [0,0,0], \"scale\": [1,1,1]}},\n"
        "    {\"op\": \"smooth_vertices\", \"params\": {\"vertex_ids\": [], \"iterations\": 2, \"strength\": 0.5}},\n"
        "    {\"op\": \"apply_noise\", \"params\": {\"vertex_ids\": [], \"amplitude\": 5.0, \"frequency\": 0.05, \"seed\": 1337}},\n"
        "    {\"op\": \"apply_taper\", \"params\": {\"axis\": \"Z\", \"amount\": 0.35}},\n"
        "    {\"op\": \"apply_twist\", \"params\": {\"axis\": \"Z\", \"angle_deg\": 45.0}},\n"
        "    {\"op\": \"select_elements\", \"params\": {\"element\": \"face|edge|vertex\", \"ids\": [], \"add\": false}},\n"
        "    {\"op\": \"select_all\", \"params\": {\"element\": \"face|edge|vertex\"}},\n"
        "    {\"op\": \"invert_selection\", \"params\": {}},\n"
        "    {\"op\": \"grow_selection\", \"params\": {}},\n"
        "    {\"op\": \"shrink_selection\", \"params\": {}},\n"
        "    {\"op\": \"select_faces_by_normal\", \"params\": {\"direction\": [0,0,1], \"max_angle_deg\": 30.0, \"add\": false}},\n"
        "    {\"op\": \"select_by_bounds\", \"params\": {\"element\": \"face\", \"min\": [-50,-50,-50], \"max\": [50,50,50], \"add\": false}},\n"
        "    {\"op\": \"select_ngons\", \"params\": {\"min_sides\": 5}},\n"
        "    {\"op\": \"select_boundary_edges\", \"params\": {}},\n"
        "    {\"op\": \"select_edge_loop\", \"params\": {\"edge_id\": 0, \"add\": false}},\n"
        "    {\"op\": \"select_edge_ring\", \"params\": {\"edge_id\": 0, \"add\": false}},\n"
        "    {\"op\": \"select_face_loop\", \"params\": {\"edge_id\": 0, \"add\": false}},\n"
        "    {\"op\": \"select_linked_shell\", \"params\": {}},\n"
        "    {\"op\": \"convert_selection\", \"params\": {\"element\": \"vertex|edge|face\"}},\n"
        "    {\"op\": \"inspect\", \"params\": {\"include_geometry\": false}},\n"
        "    {\"op\": \"check_manifold\", \"params\": {\"oriented\": true}},\n"
        "    {\"op\": \"undo\", \"params\": {}},\n"
        "    {\"op\": \"redo\", \"params\": {}}\n"
        "  ]\n"
        "}");
}

namespace
{
FNGonMeshOpResult ExecuteSingleJsonObject(FNGonPreviewScene& Scene, const TSharedPtr<FJsonObject>& Obj)
{
    FNGonMeshOpResult Res;
    if (!Obj.IsValid())
    {
        Res.Message = TEXT("Invalid JSON command object.");
        return Res;
    }

    FString Op;
    if (!Obj->TryGetStringField(TEXT("op"), Op))
    {
        Obj->TryGetStringField(TEXT("command"), Op);
    }
    Op = Op.ToLower().TrimStartAndEnd();
    Res.Operation = Op;

    auto GetNum = [&](const FString& Key, double Default) -> double
    {
        double V = Default;
        Obj->TryGetNumberField(Key, V);
        return V;
    };
    auto GetInt = [&](const FString& Key, int32 Default) -> int32
    {
        double V = static_cast<double>(Default);
        Obj->TryGetNumberField(Key, V);
        return static_cast<int32>(FMath::RoundToInt(V));
    };
    auto GetBool = [&](const FString& Key, bool Default) -> bool
    {
        bool V = Default;
        Obj->TryGetBoolField(Key, V);
        return V;
    };
    auto GetStr = [&](const FString& Key, const FString& Default) -> FString
    {
        FString V = Default;
        Obj->TryGetStringField(Key, V);
        return V;
    };

    if (Op == TEXT("create_box") || Op == TEXT("box"))
    {
        FVector Center;
        ReadVectorFromJson(Obj, TEXT("center"), Center, FVector::ZeroVector);
        return FNGonMeshAPI::CreateBox(Scene, GetNum(TEXT("width"), 100.0), GetNum(TEXT("depth"), 100.0),
            GetNum(TEXT("height"), 100.0), GetInt(TEXT("subdiv_x"), 1), GetInt(TEXT("subdiv_y"), 1), GetInt(TEXT("subdiv_z"), 1), Center);
    }
    if (Op == TEXT("create_plane") || Op == TEXT("plane"))
    {
        FVector Center;
        ReadVectorFromJson(Obj, TEXT("center"), Center, FVector::ZeroVector);
        return FNGonMeshAPI::CreatePlane(Scene, GetNum(TEXT("width"), 200.0), GetNum(TEXT("depth"), 200.0),
            GetInt(TEXT("subdiv_x"), 4), GetInt(TEXT("subdiv_y"), 4), Center);
    }
    if (Op == TEXT("create_cylinder") || Op == TEXT("cylinder"))
    {
        FVector Center;
        ReadVectorFromJson(Obj, TEXT("center"), Center, FVector::ZeroVector);
        return FNGonMeshAPI::CreateCylinder(Scene, GetNum(TEXT("radius"), 50.0), GetNum(TEXT("height"), 120.0),
            GetInt(TEXT("sides"), 16), GetInt(TEXT("height_segments"), 1), Center);
    }
    if (Op == TEXT("create_sphere") || Op == TEXT("sphere"))
    {
        FVector Center;
        ReadVectorFromJson(Obj, TEXT("center"), Center, FVector::ZeroVector);
        return FNGonMeshAPI::CreateSphere(Scene, GetNum(TEXT("radius"), 60.0), GetInt(TEXT("rings"), 10), GetInt(TEXT("segments"), 16), Center);
    }
    if (Op == TEXT("extrude_faces") || Op == TEXT("extrude"))
    {
        return FNGonMeshAPI::ExtrudeFaces(Scene, ReadIntArrayFromJson(Obj, TEXT("face_ids")),
            GetNum(TEXT("distance"), 25.0), GetNum(TEXT("scale"), 1.0));
    }
    if (Op == TEXT("inset_faces") || Op == TEXT("inset"))
    {
        return FNGonMeshAPI::InsetFaces(Scene, ReadIntArrayFromJson(Obj, TEXT("face_ids")),
            GetNum(TEXT("ratio"), 0.25), GetNum(TEXT("depth"), 0.0));
    }
    auto ReadBevelOptions = [&]() -> FNGonBevelOptions
    {
        FNGonBevelOptions Opts;
        Opts.Width = GetNum(TEXT("width"), 5.0);
        Opts.Segments = FMath::Clamp(GetInt(TEXT("segments"), 1), 1, 32);
        Opts.Profile = FMath::Clamp(GetNum(TEXT("profile"), 0.5), 0.05, 0.9999);
        Opts.bClampOverlap = GetBool(TEXT("clamp_overlap"), true);
        Opts.bSkipNonManifoldEdges = GetBool(TEXT("skip_non_manifold"), true);
        Opts.bSelectResultFaces = GetBool(TEXT("select_result"), true);
        return Opts;
    };
    if (Op == TEXT("bevel_edges"))
    {
        return FNGonMeshAPI::BevelEdges(Scene, ReadIntArrayFromJson(Obj, TEXT("edge_ids")), ReadBevelOptions());
    }
    if (Op == TEXT("bevel_vertices"))
    {
        return FNGonMeshAPI::BevelVertices(Scene, ReadIntArrayFromJson(Obj, TEXT("vertex_ids")), ReadBevelOptions());
    }
    if (Op == TEXT("bevel_faces_boundary"))
    {
        return FNGonMeshAPI::BevelFacesBoundary(Scene, ReadIntArrayFromJson(Obj, TEXT("face_ids")), ReadBevelOptions());
    }
    if (Op == TEXT("bevel_selection"))
    {
        return FNGonMeshAPI::BevelSelection(Scene, ReadBevelOptions());
    }
    if (Op == TEXT("bevel_faces"))
    {
        if (Obj->HasField(TEXT("width")) || Obj->HasField(TEXT("segments")))
        {
            return FNGonMeshAPI::BevelFacesBoundary(Scene, ReadIntArrayFromJson(Obj, TEXT("face_ids")), ReadBevelOptions());
        }
        return FNGonMeshAPI::BevelFaces(Scene, ReadIntArrayFromJson(Obj, TEXT("face_ids")),
            GetNum(TEXT("ratio"), 0.18), GetNum(TEXT("height"), 8.0));
    }
    if (Op == TEXT("bevel"))
    {
        if (Obj->HasField(TEXT("edge_ids")))
        {
            return FNGonMeshAPI::BevelEdges(Scene, ReadIntArrayFromJson(Obj, TEXT("edge_ids")), ReadBevelOptions());
        }
        if (Obj->HasField(TEXT("vertex_ids")))
        {
            return FNGonMeshAPI::BevelVertices(Scene, ReadIntArrayFromJson(Obj, TEXT("vertex_ids")), ReadBevelOptions());
        }
        if (Obj->HasField(TEXT("width")) || Obj->HasField(TEXT("segments")))
        {
            return FNGonMeshAPI::BevelSelection(Scene, ReadBevelOptions());
        }
        return FNGonMeshAPI::BevelFaces(Scene, ReadIntArrayFromJson(Obj, TEXT("face_ids")),
            GetNum(TEXT("ratio"), 0.18), GetNum(TEXT("height"), 8.0));
    }
    if (Op == TEXT("subdivide_faces") || Op == TEXT("subdivide"))
    {
        return FNGonMeshAPI::SubdivideFaces(Scene, ReadIntArrayFromJson(Obj, TEXT("face_ids")));
    }
    if (Op == TEXT("triangulate_faces") || Op == TEXT("triangulate"))
    {
        return FNGonMeshAPI::TriangulateFaces(Scene, ReadIntArrayFromJson(Obj, TEXT("face_ids")));
    }
    if (Op == TEXT("flip_faces") || Op == TEXT("flip"))
    {
        return FNGonMeshAPI::FlipFaces(Scene, ReadIntArrayFromJson(Obj, TEXT("face_ids")));
    }
    if (Op == TEXT("delete_faces") || Op == TEXT("delete"))
    {
        return FNGonMeshAPI::DeleteFaces(Scene, ReadIntArrayFromJson(Obj, TEXT("face_ids")));
    }
    if (Op == TEXT("split_edge"))
    {
        return FNGonMeshAPI::SplitEdge(Scene, GetInt(TEXT("edge_id"), INDEX_NONE), GetNum(TEXT("t"), 0.5));
    }
    if (Op == TEXT("cut_face"))
    {
        return FNGonMeshAPI::CutFace(Scene, GetInt(TEXT("face_id"), INDEX_NONE),
            GetInt(TEXT("vertex_a"), INDEX_NONE), GetInt(TEXT("vertex_b"), INDEX_NONE));
    }
    if (Op == TEXT("collapse_edge"))
    {
        return FNGonMeshAPI::CollapseEdge(Scene, GetInt(TEXT("edge_id"), INDEX_NONE));
    }
    if (Op == TEXT("weld_vertices") || Op == TEXT("weld"))
    {
        return FNGonMeshAPI::WeldVertices(Scene, ReadIntArrayFromJson(Obj, TEXT("vertex_ids")), GetNum(TEXT("threshold"), 0.0));
    }
    if (Op == TEXT("bridge_edges") || Op == TEXT("bridge"))
    {
        return FNGonMeshAPI::BridgeEdges(Scene, GetInt(TEXT("edge_a"), INDEX_NONE), GetInt(TEXT("edge_b"), INDEX_NONE));
    }
    if (Op == TEXT("fill_hole"))
    {
        return FNGonMeshAPI::FillHole(Scene, ReadIntArrayFromJson(Obj, TEXT("vertex_ids")));
    }
    if (Op == TEXT("mirror_mesh") || Op == TEXT("mirror"))
    {
        return FNGonMeshAPI::MirrorMesh(Scene, ParseAxisString(GetStr(TEXT("axis"), TEXT("X")), EAxis::X),
            GetBool(TEXT("keep_original"), true));
    }
    if (Op == TEXT("transform_vertices") || Op == TEXT("transform"))
    {
        FVector T, R, S;
        ReadVectorFromJson(Obj, TEXT("translate"), T, FVector::ZeroVector);
        ReadVectorFromJson(Obj, TEXT("rotate"), R, FVector::ZeroVector);
        ReadVectorFromJson(Obj, TEXT("scale"), S, FVector::OneVector);
        return FNGonMeshAPI::TransformVertices(Scene, ReadIntArrayFromJson(Obj, TEXT("vertex_ids")),
            T, FRotator(R.Y, R.Z, R.X), S);
    }
    if (Op == TEXT("smooth_vertices") || Op == TEXT("smooth"))
    {
        return FNGonMeshAPI::SmoothVertices(Scene, ReadIntArrayFromJson(Obj, TEXT("vertex_ids")),
            GetInt(TEXT("iterations"), 2), GetNum(TEXT("strength"), 0.5));
    }
    if (Op == TEXT("apply_noise") || Op == TEXT("noise"))
    {
        return FNGonMeshAPI::ApplyNoise(Scene, ReadIntArrayFromJson(Obj, TEXT("vertex_ids")),
            GetNum(TEXT("amplitude"), 5.0), GetNum(TEXT("frequency"), 0.05), GetInt(TEXT("seed"), 1337));
    }
    if (Op == TEXT("apply_taper") || Op == TEXT("taper"))
    {
        return FNGonMeshAPI::ApplyTaper(Scene, ParseAxisString(GetStr(TEXT("axis"), TEXT("Z")), EAxis::Z), GetNum(TEXT("amount"), 0.35));
    }
    if (Op == TEXT("apply_twist") || Op == TEXT("twist"))
    {
        return FNGonMeshAPI::ApplyTwist(Scene, ParseAxisString(GetStr(TEXT("axis"), TEXT("Z")), EAxis::Z), GetNum(TEXT("angle_deg"), 45.0));
    }
    if (Op == TEXT("select_elements") || Op == TEXT("select"))
    {
        return FNGonMeshAPI::SelectElements(Scene, ParseElementString(GetStr(TEXT("element"), TEXT("face"))),
            ReadIntArrayFromJson(Obj, TEXT("ids")), GetBool(TEXT("add"), false));
    }
    if (Op == TEXT("select_all"))
    {
        return FNGonMeshAPI::SelectAll(Scene, ParseElementString(GetStr(TEXT("element"), TEXT("face"))));
    }
    if (Op == TEXT("invert_selection"))
    {
        return FNGonMeshAPI::InvertSelection(Scene);
    }
    if (Op == TEXT("grow_selection"))
    {
        return FNGonMeshAPI::GrowSelection(Scene);
    }
    if (Op == TEXT("shrink_selection"))
    {
        return FNGonMeshAPI::ShrinkSelection(Scene);
    }
    if (Op == TEXT("select_faces_by_normal") || Op == TEXT("select_by_normal"))
    {
        FVector Dir;
        ReadVectorFromJson(Obj, TEXT("direction"), Dir, FVector::UpVector);
        return FNGonMeshAPI::SelectFacesByNormal(Scene, Dir, GetNum(TEXT("max_angle_deg"), 30.0), GetBool(TEXT("add"), false));
    }
    if (Op == TEXT("select_by_bounds"))
    {
        FVector MinV, MaxV;
        ReadVectorFromJson(Obj, TEXT("min"), MinV, FVector(-50.0));
        ReadVectorFromJson(Obj, TEXT("max"), MaxV, FVector(50.0));
        return FNGonMeshAPI::SelectByBounds(Scene, ParseElementString(GetStr(TEXT("element"), TEXT("face"))),
            FBox(MinV, MaxV), GetBool(TEXT("add"), false));
    }
    if (Op == TEXT("select_ngons"))
    {
        return FNGonMeshAPI::SelectNGons(Scene, GetInt(TEXT("min_sides"), 5));
    }
    if (Op == TEXT("select_boundary_edges"))
    {
        return FNGonMeshAPI::SelectBoundaryEdges(Scene);
    }
    if (Op == TEXT("select_edge_loop") || Op == TEXT("edge_loop"))
    {
        return FNGonMeshAPI::SelectEdgeLoop(Scene, GetInt(TEXT("edge_id"), INDEX_NONE), GetBool(TEXT("add"), false));
    }
    if (Op == TEXT("select_edge_ring") || Op == TEXT("edge_ring"))
    {
        return FNGonMeshAPI::SelectEdgeRing(Scene, GetInt(TEXT("edge_id"), INDEX_NONE), GetBool(TEXT("add"), false));
    }
    if (Op == TEXT("select_face_loop") || Op == TEXT("face_loop"))
    {
        return FNGonMeshAPI::SelectFaceLoop(Scene, GetInt(TEXT("edge_id"), INDEX_NONE), GetBool(TEXT("add"), false));
    }
    if (Op == TEXT("select_linked_shell") || Op == TEXT("select_linked") || Op == TEXT("select_shell"))
    {
        return FNGonMeshAPI::SelectLinkedShell(Scene);
    }
    if (Op == TEXT("convert_selection"))
    {
        return FNGonMeshAPI::ConvertSelection(Scene, ParseElementString(GetStr(TEXT("element"), TEXT("face"))));
    }
    if (Op == TEXT("inspect") || Op == TEXT("summary"))
    {
        Res.bOk = true;
        Res.PayloadJson = FNGonMeshAPI::ExportMeshSummaryJson(Scene, GetBool(TEXT("include_geometry"), false));
        Res.Message = TEXT("Exported mesh summary JSON.");
        return Res;
    }
    if (Op == TEXT("check_manifold") || Op == TEXT("validate"))
    {
        const FNGonValidationReport Report = FNGonMeshAPI::CheckManifold(Scene, GetBool(TEXT("oriented"), true));
        Res.bOk = Report.IsOk();
        Res.Message = Report.IsOk()
            ? TEXT("Manifold check passed (0 issues).")
            : FString::Printf(TEXT("Manifold check found %d issue(s)."), Report.Issues.Num());
        return Res;
    }
    if (Op == TEXT("undo"))
    {
        return FNGonMeshAPI::Undo(Scene);
    }
    if (Op == TEXT("redo"))
    {
        return FNGonMeshAPI::Redo(Scene);
    }

    Res.Message = FString::Printf(TEXT("Unknown Mesh API operation: '%s'."), *Op);
    return Res;
}
} // anonymous namespace

FNGonMeshOpResult FNGonMeshAPI::ExecuteJsonCommand(FNGonPreviewScene& Scene, const FString& JsonText)
{
    FNGonMeshOpResult Res;
    Res.Operation = TEXT("execute_json");

    const FString Trimmed = JsonText.TrimStartAndEnd();
    if (Trimmed.IsEmpty())
    {
        Res.Message = TEXT("JSON command is empty.");
        return Res;
    }

    // Batch array mode: [ { "op": ... }, ... ]
    if (Trimmed.StartsWith(TEXT("[")))
    {
        TArray<TSharedPtr<FJsonValue>> Commands;
        const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Trimmed);
        if (!FJsonSerializer::Deserialize(Reader, Commands))
        {
            Res.Message = TEXT("Failed to parse JSON array.");
            return Res;
        }
        if (Commands.IsEmpty())
        {
            Res.Message = TEXT("JSON command array is empty.");
            return Res;
        }

        int32 StepIdx = 0;
        TArray<FString> StepLogs;
        for (const TSharedPtr<FJsonValue>& Val : Commands)
        {
            ++StepIdx;
            if (!Val.IsValid() || Val->Type != EJson::Object)
            {
                Res.Message = FString::Printf(TEXT("Step %d is not a JSON object."), StepIdx);
                return Res;
            }
            const FNGonMeshOpResult StepRes = ExecuteSingleJsonObject(Scene, Val->AsObject());
            StepLogs.Add(FString::Printf(TEXT("[%d] %s: %s"), StepIdx, *StepRes.Operation, *StepRes.Message));
            if (!StepRes.bOk)
            {
                Res.Message = FString::Printf(TEXT("Batch stopped at step %d (%s): %s"), StepIdx, *StepRes.Operation, *StepRes.Message);
                return Res;
            }
            Res.CreatedFaces.Append(StepRes.CreatedFaces);
            Res.CreatedVertices.Append(StepRes.CreatedVertices);
            Res.SelectedIds = StepRes.SelectedIds;
            if (!StepRes.PayloadJson.IsEmpty()) Res.PayloadJson = StepRes.PayloadJson;
        }

        Res.bOk = true;
        Res.Message = FString::Printf(TEXT("Executed %d command(s) successfully.\n%s"),
            Commands.Num(), *FString::Join(StepLogs, TEXT("\n")));
        return Res;
    }

    // Single object mode: { "op": ... }
    TSharedPtr<FJsonObject> Obj;
    const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Trimmed);
    if (!FJsonSerializer::Deserialize(Reader, Obj) || !Obj.IsValid())
    {
        Res.Message = TEXT("Failed to parse JSON object. Expected {\"op\": \"...\", ...} or an array of commands.");
        return Res;
    }

    return ExecuteSingleJsonObject(Scene, Obj);
}
// NGon Unified ABI Sync v5.8.2
