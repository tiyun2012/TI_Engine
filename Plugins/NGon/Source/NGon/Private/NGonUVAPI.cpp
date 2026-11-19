#include "NGonUVAPI.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

namespace
{
struct FNGonUVSnapshot
{
    TMap<int32, FVector2f> ElementUV;
    TSet<int32> SelectedElements;
    ENGonUVSelectionMode SelectionMode = ENGonUVSelectionMode::Corner;
};

struct FNGonUVHistoryBuffer
{
    static constexpr int32 MaxHistory = 64;
    TArray<FNGonUVSnapshot> UndoStack;
    TArray<FNGonUVSnapshot> RedoStack;
};

TMap<const FNGonUVDocument*, FNGonUVHistoryBuffer> GUVHistoryMap;

FNGonUVSnapshot CaptureUVSnapshot(const FNGonUVDocument& Doc)
{
    FNGonUVSnapshot Snap;
    Snap.ElementUV = Doc.GetElementUVs();
    Snap.SelectedElements = Doc.GetSelectedElements();
    Snap.SelectionMode = Doc.GetSelectionMode();
    return Snap;
}

TArray<int32> ResolveTargetElements(const FNGonUVDocument& Doc, bool bFallbackToAll = true)
{
    TArray<int32> Out;
    if (!Doc.GetSelectedElements().IsEmpty())
    {
        Out = Doc.GetSelectedElements().Array();
        Out.Sort();
        return Out;
    }
    if (bFallbackToAll)
    {
        Doc.GetElementUVs().GetKeys(Out);
        Out.Sort();
    }
    return Out;
}

double TriangleSignedArea2D(const FVector2f& A, const FVector2f& B, const FVector2f& C)
{
    return 0.5 * (static_cast<double>(B.X - A.X) * static_cast<double>(C.Y - A.Y)
        - static_cast<double>(B.Y - A.Y) * static_cast<double>(C.X - A.X));
}

FString SerializeUVJson(const TSharedRef<FJsonObject>& Obj, bool bPretty = true)
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

TArray<TSharedPtr<FJsonValue>> MakeUVIntArrayJson(const TArray<int32>& Arr)
{
    TArray<TSharedPtr<FJsonValue>> Out;
    Out.Reserve(Arr.Num());
    for (const int32 V : Arr) Out.Add(MakeShared<FJsonValueNumber>(V));
    return Out;
}

TSharedRef<FJsonObject> MakeUV2Json(const FVector2D& V)
{
    TSharedRef<FJsonObject> O = MakeShared<FJsonObject>();
    O->SetNumberField(TEXT("u"), V.X);
    O->SetNumberField(TEXT("v"), V.Y);
    return O;
}

bool ReadVec2FromJson(const TSharedPtr<FJsonObject>& Obj, const FString& Field, FVector2f& Out, const FVector2f& Default)
{
    Out = Default;
    if (!Obj.IsValid() || !Obj->HasField(Field)) return false;
    const TArray<TSharedPtr<FJsonValue>>* Arr = nullptr;
    if (Obj->TryGetArrayField(Field, Arr) && Arr && Arr->Num() >= 2)
    {
        Out.X = static_cast<float>((*Arr)[0]->AsNumber());
        Out.Y = static_cast<float>((*Arr)[1]->AsNumber());
        return true;
    }
    const TSharedPtr<FJsonObject>* Sub = nullptr;
    if (Obj->TryGetObjectField(Field, Sub) && Sub && Sub->IsValid())
    {
        double U = Default.X, V = Default.Y;
        if (!(*Sub)->TryGetNumberField(TEXT("u"), U)) (*Sub)->TryGetNumberField(TEXT("x"), U);
        if (!(*Sub)->TryGetNumberField(TEXT("v"), V)) (*Sub)->TryGetNumberField(TEXT("y"), V);
        Out = FVector2f(static_cast<float>(U), static_cast<float>(V));
        return true;
    }
    return false;
}

TArray<int32> ReadUVIntArray(const TSharedPtr<FJsonObject>& Obj, const FString& Field)
{
    TArray<int32> Out;
    if (!Obj.IsValid()) return Out;
    const TArray<TSharedPtr<FJsonValue>>* Arr = nullptr;
    if (Obj->TryGetArrayField(Field, Arr) && Arr)
    {
        Out.Reserve(Arr->Num());
        for (const TSharedPtr<FJsonValue>& V : *Arr)
        {
            if (V.IsValid()) Out.Add(static_cast<int32>(FMath::RoundToInt(V->AsNumber())));
        }
    }
    return Out;
}

TSet<int32> FindBorderElements(const FNGonUVDocument& Doc)
{
    TMap<uint64, int32> EdgeCounts;
    for (const FNGonUVDocument::FTriangle& Tri : Doc.GetTriangles())
    {
        for (int32 c = 0; c < 3; ++c)
        {
            const int32 A = Tri.Element[c];
            const int32 B = Tri.Element[(c + 1) % 3];
            if (A == INDEX_NONE || B == INDEX_NONE || A == B) continue;
            const uint64 Key = (static_cast<uint64>(static_cast<uint32>(FMath::Min(A, B))) << 32)
                | static_cast<uint32>(FMath::Max(A, B));
            ++EdgeCounts.FindOrAdd(Key);
        }
    }
    TSet<int32> Border;
    for (const TPair<uint64, int32>& Pair : EdgeCounts)
    {
        if (Pair.Value == 1)
        {
            const int32 A = static_cast<int32>(Pair.Key >> 32);
            const int32 B = static_cast<int32>(Pair.Key & 0xffffffffu);
            Border.Add(A);
            Border.Add(B);
        }
    }
    return Border;
}
} // anonymous namespace

FString FNGonUVOpResult::ToJsonString() const
{
    TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
    Root->SetBoolField(TEXT("ok"), bOk);
    Root->SetStringField(TEXT("op"), Operation);
    Root->SetStringField(TEXT("message"), Message);
    if (!SelectedElements.IsEmpty()) Root->SetArrayField(TEXT("selected_elements"), MakeUVIntArrayJson(SelectedElements));
    if (!ModifiedElements.IsEmpty()) Root->SetArrayField(TEXT("modified_elements"), MakeUVIntArrayJson(ModifiedElements));
    if (!PayloadJson.IsEmpty()) Root->SetStringField(TEXT("payload"), PayloadJson);
    return SerializeUVJson(Root, true);
}

// ============================================================================
// History
// ============================================================================

void FNGonUVAPI::ClearHistory(const FNGonUVDocument* Doc)
{
    GUVHistoryMap.Remove(Doc);
}

void FNGonUVAPI::PushUndoSnapshot(const FNGonUVDocument& Doc)
{
    if (Doc.IsEmpty()) return;
    FNGonUVHistoryBuffer& Hist = GUVHistoryMap.FindOrAdd(&Doc);
    Hist.UndoStack.Add(CaptureUVSnapshot(Doc));
    if (Hist.UndoStack.Num() > FNGonUVHistoryBuffer::MaxHistory)
    {
        Hist.UndoStack.RemoveAt(0);
    }
    Hist.RedoStack.Reset();
}

bool FNGonUVAPI::CanUndo(const FNGonUVDocument* Doc)
{
    const FNGonUVHistoryBuffer* Hist = GUVHistoryMap.Find(Doc);
    return Hist && !Hist->UndoStack.IsEmpty();
}

bool FNGonUVAPI::CanRedo(const FNGonUVDocument* Doc)
{
    const FNGonUVHistoryBuffer* Hist = GUVHistoryMap.Find(Doc);
    return Hist && !Hist->RedoStack.IsEmpty();
}

int32 FNGonUVAPI::GetUndoCount(const FNGonUVDocument* Doc)
{
    const FNGonUVHistoryBuffer* Hist = GUVHistoryMap.Find(Doc);
    return Hist ? Hist->UndoStack.Num() : 0;
}

int32 FNGonUVAPI::GetRedoCount(const FNGonUVDocument* Doc)
{
    const FNGonUVHistoryBuffer* Hist = GUVHistoryMap.Find(Doc);
    return Hist ? Hist->RedoStack.Num() : 0;
}

FNGonUVOpResult FNGonUVAPI::Undo(FNGonUVDocument& Doc)
{
    FNGonUVOpResult Res;
    Res.Operation = TEXT("undo");
    FNGonUVHistoryBuffer* Hist = GUVHistoryMap.Find(&Doc);
    if (!Hist || Hist->UndoStack.IsEmpty())
    {
        Res.Message = TEXT("Nothing to undo.");
        return Res;
    }
    Hist->RedoStack.Add(CaptureUVSnapshot(Doc));
    const FNGonUVSnapshot Prev = Hist->UndoStack.Pop();
    Doc.SetAllElementUVs(Prev.ElementUV);
    Doc.SetSelectionMode(Prev.SelectionMode);
    Doc.SetSelectedElements(Prev.SelectedElements, false);
    Res.bOk = true;
    Res.Message = FString::Printf(TEXT("Undid UV edit (%d undo steps left)."), Hist->UndoStack.Num());
    return Res;
}

FNGonUVOpResult FNGonUVAPI::Redo(FNGonUVDocument& Doc)
{
    FNGonUVOpResult Res;
    Res.Operation = TEXT("redo");
    FNGonUVHistoryBuffer* Hist = GUVHistoryMap.Find(&Doc);
    if (!Hist || Hist->RedoStack.IsEmpty())
    {
        Res.Message = TEXT("Nothing to redo.");
        return Res;
    }
    Hist->UndoStack.Add(CaptureUVSnapshot(Doc));
    const FNGonUVSnapshot Next = Hist->RedoStack.Pop();
    Doc.SetAllElementUVs(Next.ElementUV);
    Doc.SetSelectionMode(Next.SelectionMode);
    Doc.SetSelectedElements(Next.SelectedElements, false);
    Res.bOk = true;
    Res.Message = FString::Printf(TEXT("Redid UV edit (%d redo steps left)."), Hist->RedoStack.Num());
    return Res;
}

// ============================================================================
// Selection & Queries
// ============================================================================

FNGonUVOpResult FNGonUVAPI::SelectAll(FNGonUVDocument& Doc)
{
    FNGonUVOpResult Res;
    Res.Operation = TEXT("select_all");
    TSet<int32> All;
    for (const TPair<int32, FVector2f>& Pair : Doc.GetElementUVs()) All.Add(Pair.Key);
    Doc.SetSelectedElements(All, false);
    Res.bOk = true;
    Res.SelectedElements = All.Array();
    Res.SelectedElements.Sort();
    Res.Message = FString::Printf(TEXT("Selected all %d UV corner(s)."), All.Num());
    return Res;
}

FNGonUVOpResult FNGonUVAPI::ClearSelection(FNGonUVDocument& Doc)
{
    FNGonUVOpResult Res;
    Res.Operation = TEXT("clear_selection");
    Doc.ClearSelection();
    Res.bOk = true;
    Res.Message = TEXT("Cleared UV selection.");
    return Res;
}

FNGonUVOpResult FNGonUVAPI::InvertSelection(FNGonUVDocument& Doc)
{
    FNGonUVOpResult Res;
    Res.Operation = TEXT("invert_selection");
    const TSet<int32>& Current = Doc.GetSelectedElements();
    TSet<int32> Inverted;
    for (const TPair<int32, FVector2f>& Pair : Doc.GetElementUVs())
    {
        if (!Current.Contains(Pair.Key)) Inverted.Add(Pair.Key);
    }
    Doc.SetSelectedElements(Inverted, false);
    Res.bOk = true;
    Res.SelectedElements = Inverted.Array();
    Res.SelectedElements.Sort();
    Res.Message = FString::Printf(TEXT("Inverted UV selection (%d selected)."), Inverted.Num());
    return Res;
}

FNGonUVOpResult FNGonUVAPI::GrowSelection(FNGonUVDocument& Doc)
{
    FNGonUVOpResult Res;
    Res.Operation = TEXT("grow_selection");
    const TSet<int32>& Current = Doc.GetSelectedElements();
    if (Current.IsEmpty())
    {
        Res.Message = TEXT("Nothing selected to grow.");
        return Res;
    }
    TSet<int32> Grown = Current;
    for (const FNGonUVDocument::FTriangle& Tri : Doc.GetTriangles())
    {
        if (Current.Contains(Tri.Element[0]) || Current.Contains(Tri.Element[1]) || Current.Contains(Tri.Element[2]))
        {
            Grown.Add(Tri.Element[0]);
            Grown.Add(Tri.Element[1]);
            Grown.Add(Tri.Element[2]);
        }
    }
    Doc.SetSelectedElements(Grown, false);
    Res.bOk = true;
    Res.SelectedElements = Grown.Array();
    Res.SelectedElements.Sort();
    Res.Message = FString::Printf(TEXT("Grew UV selection from %d to %d corner(s)."), Current.Num(), Grown.Num());
    return Res;
}

FNGonUVOpResult FNGonUVAPI::ShrinkSelection(FNGonUVDocument& Doc)
{
    FNGonUVOpResult Res;
    Res.Operation = TEXT("shrink_selection");
    const TSet<int32>& Current = Doc.GetSelectedElements();
    if (Current.IsEmpty())
    {
        Res.Message = TEXT("Nothing selected to shrink.");
        return Res;
    }
    TSet<int32> TouchesUnselected;
    for (const FNGonUVDocument::FTriangle& Tri : Doc.GetTriangles())
    {
        const bool b0 = Current.Contains(Tri.Element[0]);
        const bool b1 = Current.Contains(Tri.Element[1]);
        const bool b2 = Current.Contains(Tri.Element[2]);
        if (!(b0 && b1 && b2))
        {
            TouchesUnselected.Add(Tri.Element[0]);
            TouchesUnselected.Add(Tri.Element[1]);
            TouchesUnselected.Add(Tri.Element[2]);
        }
    }
    TSet<int32> Shrunk;
    for (const int32 Id : Current)
    {
        if (!TouchesUnselected.Contains(Id)) Shrunk.Add(Id);
    }
    Doc.SetSelectedElements(Shrunk, false);
    Res.bOk = true;
    Res.SelectedElements = Shrunk.Array();
    Res.SelectedElements.Sort();
    Res.Message = FString::Printf(TEXT("Shrunk UV selection from %d to %d corner(s)."), Current.Num(), Shrunk.Num());
    return Res;
}

FNGonUVOpResult FNGonUVAPI::SelectElements(FNGonUVDocument& Doc, const TArray<int32>& ElementIds, bool bAdd)
{
    FNGonUVOpResult Res;
    Res.Operation = TEXT("select_elements");
    TSet<int32> Valid;
    for (const int32 Id : ElementIds)
    {
        if (Doc.GetElementUVs().Contains(Id)) Valid.Add(Id);
    }
    Doc.SetSelectedElements(Valid, bAdd);
    Res.bOk = true;
    Res.SelectedElements = Doc.GetSelectedElements().Array();
    Res.SelectedElements.Sort();
    Res.Message = FString::Printf(TEXT("Selected %d UV corner(s)."), Res.SelectedElements.Num());
    return Res;
}

FNGonUVOpResult FNGonUVAPI::SelectIsland(FNGonUVDocument& Doc, int32 IslandId, bool bAdd)
{
    FNGonUVOpResult Res;
    Res.Operation = TEXT("select_island");
    if (IslandId < 0 || IslandId >= Doc.GetIslandCount())
    {
        Res.Message = FString::Printf(TEXT("Island #%d is out of range (0..%d)."), IslandId, FMath::Max(0, Doc.GetIslandCount() - 1));
        return Res;
    }
    TSet<int32> Elems;
    for (const FNGonUVDocument::FTriangle& Tri : Doc.GetTriangles())
    {
        if (Tri.Island == IslandId)
        {
            Elems.Add(Tri.Element[0]);
            Elems.Add(Tri.Element[1]);
            Elems.Add(Tri.Element[2]);
        }
    }
    Doc.SetSelectedElements(Elems, bAdd);
    Res.bOk = true;
    Res.SelectedElements = Doc.GetSelectedElements().Array();
    Res.SelectedElements.Sort();
    Res.Message = FString::Printf(TEXT("Selected UV island #%d (%d corner(s))."), IslandId, Elems.Num());
    return Res;
}

FNGonUVOpResult FNGonUVAPI::SelectLinkedIslands(FNGonUVDocument& Doc)
{
    FNGonUVOpResult Res;
    Res.Operation = TEXT("select_linked_islands");
    const TSet<int32>& Sel = Doc.GetSelectedElements();
    if (Sel.IsEmpty())
    {
        Res.Message = TEXT("Select at least one UV corner/edge/face first.");
        return Res;
    }
    TSet<int32> Islands;
    for (const FNGonUVDocument::FTriangle& Tri : Doc.GetTriangles())
    {
        if (Sel.Contains(Tri.Element[0]) || Sel.Contains(Tri.Element[1]) || Sel.Contains(Tri.Element[2]))
        {
            Islands.Add(Tri.Island);
        }
    }
    TSet<int32> Linked;
    for (const FNGonUVDocument::FTriangle& Tri : Doc.GetTriangles())
    {
        if (Islands.Contains(Tri.Island))
        {
            Linked.Add(Tri.Element[0]);
            Linked.Add(Tri.Element[1]);
            Linked.Add(Tri.Element[2]);
        }
    }
    Doc.SetSelectedElements(Linked, false);
    Res.bOk = true;
    Res.SelectedElements = Linked.Array();
    Res.SelectedElements.Sort();
    Res.Message = FString::Printf(TEXT("Selected %d linked UV island(s) (%d corner(s))."), Islands.Num(), Linked.Num());
    return Res;
}

FNGonUVOpResult FNGonUVAPI::SelectSeamCorners(FNGonUVDocument& Doc)
{
    FNGonUVOpResult Res;
    Res.Operation = TEXT("select_seams");
    const TSet<int32> Border = FindBorderElements(Doc);
    Doc.SetSelectedElements(Border, false);
    Res.bOk = true;
    Res.SelectedElements = Border.Array();
    Res.SelectedElements.Sort();
    Res.Message = FString::Printf(TEXT("Selected %d seam/border UV corner(s)."), Border.Num());
    return Res;
}

FNGonUVOpResult FNGonUVAPI::SelectFlippedTriangles(FNGonUVDocument& Doc)
{
    FNGonUVOpResult Res;
    Res.Operation = TEXT("select_flipped");
    TSet<int32> FlippedElems;
    int32 FlippedTris = 0;
    for (const FNGonUVDocument::FTriangle& Tri : Doc.GetTriangles())
    {
        if (TriangleSignedArea2D(Tri.UV[0], Tri.UV[1], Tri.UV[2]) < -1e-8)
        {
            ++FlippedTris;
            FlippedElems.Add(Tri.Element[0]);
            FlippedElems.Add(Tri.Element[1]);
            FlippedElems.Add(Tri.Element[2]);
        }
    }
    Doc.SetSelectedElements(FlippedElems, false);
    Res.bOk = true;
    Res.SelectedElements = FlippedElems.Array();
    Res.SelectedElements.Sort();
    Res.Message = FString::Printf(TEXT("Selected %d flipped UV triangle(s) (%d corner(s))."), FlippedTris, FlippedElems.Num());
    return Res;
}

FNGonUVOpResult FNGonUVAPI::SelectByBounds(FNGonUVDocument& Doc, const FBox2D& Rect, bool bAdd)
{
    FNGonUVOpResult Res;
    Res.Operation = TEXT("select_by_bounds");
    Doc.SelectRect(Rect, bAdd);
    Res.bOk = true;
    Res.SelectedElements = Doc.GetSelectedElements().Array();
    Res.SelectedElements.Sort();
    Res.Message = FString::Printf(TEXT("Selected %d UV corner(s) inside bounds."), Res.SelectedElements.Num());
    return Res;
}

// ============================================================================
// Transform & Alignment
// ============================================================================

FNGonUVOpResult FNGonUVAPI::TranslateSelected(FNGonUVDocument& Doc, const FVector2f& Delta)
{
    FNGonUVOpResult Res;
    Res.Operation = TEXT("translate");
    const TArray<int32> Targets = ResolveTargetElements(Doc, false);
    if (Targets.IsEmpty())
    {
        Res.Message = TEXT("Select UV elements to translate.");
        return Res;
    }
    PushUndoSnapshot(Doc);
    Doc.TranslateSelected(Delta);
    Res.bOk = true;
    Res.ModifiedElements = Targets;
    Res.Message = FString::Printf(TEXT("Translated %d UV corner(s) by (%.3f, %.3f)."), Targets.Num(), Delta.X, Delta.Y);
    return Res;
}

FNGonUVOpResult FNGonUVAPI::RotateSelected(FNGonUVDocument& Doc, float Degrees)
{
    FNGonUVOpResult Res;
    Res.Operation = TEXT("rotate");
    const TArray<int32> Targets = ResolveTargetElements(Doc, false);
    if (Targets.IsEmpty())
    {
        Res.Message = TEXT("Select UV elements to rotate.");
        return Res;
    }
    PushUndoSnapshot(Doc);
    Doc.RotateSelected(Degrees);
    Res.bOk = true;
    Res.ModifiedElements = Targets;
    Res.Message = FString::Printf(TEXT("Rotated %d UV corner(s) by %.1f deg."), Targets.Num(), Degrees);
    return Res;
}

FNGonUVOpResult FNGonUVAPI::ScaleSelected(FNGonUVDocument& Doc, const FVector2f& ScaleUV)
{
    FNGonUVOpResult Res;
    Res.Operation = TEXT("scale");
    const TArray<int32> Targets = ResolveTargetElements(Doc, false);
    if (Targets.IsEmpty())
    {
        Res.Message = TEXT("Select UV elements to scale.");
        return Res;
    }
    PushUndoSnapshot(Doc);
    Doc.ScaleSelectedNonUniform(ScaleUV);
    Res.bOk = true;
    Res.ModifiedElements = Targets;
    Res.Message = FString::Printf(TEXT("Scaled %d UV corner(s) by (%.3f, %.3f)."), Targets.Num(), ScaleUV.X, ScaleUV.Y);
    return Res;
}

FNGonUVOpResult FNGonUVAPI::FlipSelected(FNGonUVDocument& Doc, bool bHorizontal)
{
    FNGonUVOpResult Res;
    Res.Operation = TEXT("flip");
    const TArray<int32> Targets = ResolveTargetElements(Doc, false);
    if (Targets.IsEmpty())
    {
        Res.Message = TEXT("Select UV elements to flip.");
        return Res;
    }
    PushUndoSnapshot(Doc);
    Doc.FlipSelected(bHorizontal);
    Res.bOk = true;
    Res.ModifiedElements = Targets;
    Res.Message = FString::Printf(TEXT("Flipped %d UV corner(s) %s."), Targets.Num(), bHorizontal ? TEXT("horizontally (U)") : TEXT("vertically (V)"));
    return Res;
}

FNGonUVOpResult FNGonUVAPI::AlignSelected(FNGonUVDocument& Doc, ENGonUVAlign Alignment)
{
    FNGonUVOpResult Res;
    Res.Operation = TEXT("align");
    const TArray<int32> Targets = ResolveTargetElements(Doc, false);
    if (Targets.Num() < 2)
    {
        Res.Message = TEXT("Select at least 2 UV corners to align.");
        return Res;
    }

    FBox2D Box(ForceInit);
    for (const int32 Id : Targets) Box += FVector2D(Doc.GetElementUV(Id));

    PushUndoSnapshot(Doc);
    TMap<int32, FVector2f> UVs = Doc.GetElementUVs();
    for (const int32 Id : Targets)
    {
        FVector2f& P = UVs.FindChecked(Id);
        switch (Alignment)
        {
        case ENGonUVAlign::Left:    P.X = static_cast<float>(Box.Min.X); break;
        case ENGonUVAlign::Right:   P.X = static_cast<float>(Box.Max.X); break;
        case ENGonUVAlign::Bottom:  P.Y = static_cast<float>(Box.Min.Y); break;
        case ENGonUVAlign::Top:     P.Y = static_cast<float>(Box.Max.Y); break;
        case ENGonUVAlign::CenterU: P.X = static_cast<float>(Box.GetCenter().X); break;
        case ENGonUVAlign::CenterV: P.Y = static_cast<float>(Box.GetCenter().Y); break;
        }
    }
    Doc.SetAllElementUVs(UVs);
    Res.bOk = true;
    Res.ModifiedElements = Targets;
    Res.Message = FString::Printf(TEXT("Aligned %d UV corner(s)."), Targets.Num());
    return Res;
}

FNGonUVOpResult FNGonUVAPI::StraightenSelected(FNGonUVDocument& Doc)
{
    FNGonUVOpResult Res;
    Res.Operation = TEXT("straighten");
    const TArray<int32> Targets = ResolveTargetElements(Doc, false);
    if (Targets.Num() < 2)
    {
        Res.Message = TEXT("Select at least 2 UV corners to straighten.");
        return Res;
    }
    FBox2D Box(ForceInit);
    for (const int32 Id : Targets) Box += FVector2D(Doc.GetElementUV(Id));
    const FVector2D Sz = Box.GetSize();
    return AlignSelected(Doc, Sz.X >= Sz.Y ? ENGonUVAlign::CenterV : ENGonUVAlign::CenterU);
}

FNGonUVOpResult FNGonUVAPI::SnapSelectedToGrid(FNGonUVDocument& Doc, float GridStep)
{
    FNGonUVOpResult Res;
    Res.Operation = TEXT("snap_grid");
    const TArray<int32> Targets = ResolveTargetElements(Doc, true);
    if (Targets.IsEmpty())
    {
        Res.Message = TEXT("No UV elements to snap.");
        return Res;
    }
    const float Step = FMath::Max(GridStep, 0.001f);
    PushUndoSnapshot(Doc);
    TMap<int32, FVector2f> UVs = Doc.GetElementUVs();
    for (const int32 Id : Targets)
    {
        FVector2f& P = UVs.FindChecked(Id);
        P.X = FMath::RoundToFloat(P.X / Step) * Step;
        P.Y = FMath::RoundToFloat(P.Y / Step) * Step;
    }
    Doc.SetAllElementUVs(UVs);
    Res.bOk = true;
    Res.ModifiedElements = Targets;
    Res.Message = FString::Printf(TEXT("Snapped %d UV corner(s) to %.4f grid."), Targets.Num(), Step);
    return Res;
}

// ============================================================================
// Layout, Packing & Relaxation
// ============================================================================

FNGonUVOpResult FNGonUVAPI::NormalizeSelected(FNGonUVDocument& Doc, bool bKeepAspect, float Padding)
{
    FNGonUVOpResult Res;
    Res.Operation = TEXT("normalize");
    const TArray<int32> Targets = ResolveTargetElements(Doc, true);
    if (Targets.IsEmpty())
    {
        Res.Message = TEXT("No UV elements to normalize.");
        return Res;
    }
    FBox2D Box(ForceInit);
    for (const int32 Id : Targets) Box += FVector2D(Doc.GetElementUV(Id));
    const FVector2D Size = Box.GetSize();
    if (Size.X <= 1e-6 && Size.Y <= 1e-6)
    {
        Res.Message = TEXT("UV selection has zero size.");
        return Res;
    }
    const float Pad = FMath::Clamp(Padding, 0.0f, 0.25f);
    const double TargetSpan = FMath::Max(0.01, 1.0 - 2.0 * Pad);
    const double ScaleX = bKeepAspect ? (TargetSpan / FMath::Max(Size.X, Size.Y)) : (Size.X > 1e-6 ? TargetSpan / Size.X : 1.0);
    const double ScaleY = bKeepAspect ? ScaleX : (Size.Y > 1e-6 ? TargetSpan / Size.Y : 1.0);

    PushUndoSnapshot(Doc);
    TMap<int32, FVector2f> UVs = Doc.GetElementUVs();
    for (const int32 Id : Targets)
    {
        const FVector2f P = UVs.FindChecked(Id);
        UVs[Id] = FVector2f(
            static_cast<float>(Pad + (P.X - Box.Min.X) * ScaleX),
            static_cast<float>(Pad + (P.Y - Box.Min.Y) * ScaleY));
    }
    Doc.SetAllElementUVs(UVs);
    Res.bOk = true;
    Res.ModifiedElements = Targets;
    Res.Message = FString::Printf(TEXT("Normalized %d UV corner(s) into [0,1] (keep_aspect=%s)."),
        Targets.Num(), bKeepAspect ? TEXT("true") : TEXT("false"));
    return Res;
}

FNGonUVOpResult FNGonUVAPI::PackIslands(FNGonUVDocument& Doc, float Padding, bool bRotateToFit)
{
    FNGonUVOpResult Res;
    Res.Operation = TEXT("pack_islands");
    const FNGonUVAnalysis Analysis = AnalyzeUVs(Doc);
    if (Analysis.Islands.IsEmpty())
    {
        Res.Message = TEXT("No UV islands to pack.");
        return Res;
    }

    struct FPackItem
    {
        int32 IslandIdx = 0;
        FVector2D Min = FVector2D::ZeroVector;
        FVector2D Size = FVector2D::ZeroVector;
        bool bRotate90 = false;
        FVector2D PlacedPos = FVector2D::ZeroVector;
    };

    TArray<FPackItem> Items;
    Items.Reserve(Analysis.Islands.Num());
    double TotalArea = 0.0;
    double MaxDim = 0.0;

    for (int32 i = 0; i < Analysis.Islands.Num(); ++i)
    {
        const FNGonUVIslandInfo& Isl = Analysis.Islands[i];
        FPackItem Item;
        Item.IslandIdx = i;
        Item.Min = Isl.Bounds.Min;
        Item.Size = FVector2D(FMath::Max(Isl.Bounds.GetSize().X, 1e-4), FMath::Max(Isl.Bounds.GetSize().Y, 1e-4));
        if (bRotateToFit && Item.Size.Y > Item.Size.X)
        {
            Item.bRotate90 = true;
            Swap(Item.Size.X, Item.Size.Y);
        }
        TotalArea += Item.Size.X * Item.Size.Y;
        MaxDim = FMath::Max3(MaxDim, Item.Size.X, Item.Size.Y);
        Items.Add(Item);
    }

    // Sort tallest first (standard shelf-packing heuristic)
    Items.Sort([](const FPackItem& A, const FPackItem& B) { return A.Size.Y > B.Size.Y; });

    const float Pad = FMath::Clamp(Padding, 0.0f, 0.15f);
    const double RawPad = Pad * FMath::Max(FMath::Sqrt(TotalArea), MaxDim);
    const double TargetRowWidth = FMath::Max(MaxDim, FMath::Sqrt(TotalArea) * 1.15);

    double CursorX = 0.0;
    double CursorY = 0.0;
    double RowHeight = 0.0;
    double MaxUsedX = 0.0;
    double MaxUsedY = 0.0;

    for (FPackItem& Item : Items)
    {
        if (CursorX > 0.0 && CursorX + Item.Size.X > TargetRowWidth)
        {
            CursorX = 0.0;
            CursorY += RowHeight + RawPad;
            RowHeight = 0.0;
        }
        Item.PlacedPos = FVector2D(CursorX, CursorY);
        CursorX += Item.Size.X + RawPad;
        RowHeight = FMath::Max(RowHeight, Item.Size.Y);
        MaxUsedX = FMath::Max(MaxUsedX, Item.PlacedPos.X + Item.Size.X);
        MaxUsedY = FMath::Max(MaxUsedY, Item.PlacedPos.Y + Item.Size.Y);
    }

    const double Span = FMath::Max3(MaxUsedX, MaxUsedY, 1e-4);
    const double Scale = FMath::Max(0.01, (1.0 - 2.0 * Pad)) / Span;

    PushUndoSnapshot(Doc);
    TMap<int32, FVector2f> UVs = Doc.GetElementUVs();
    for (const FPackItem& Item : Items)
    {
        const FNGonUVIslandInfo& Isl = Analysis.Islands[Item.IslandIdx];
        const double OrigH = Isl.Bounds.GetSize().Y;
        for (const int32 ElemId : Isl.Elements)
        {
            const FVector2f P = UVs.FindChecked(ElemId);
            double lx = static_cast<double>(P.X) - Item.Min.X;
            double ly = static_cast<double>(P.Y) - Item.Min.Y;
            if (Item.bRotate90)
            {
                const double rx = OrigH - ly;
                const double ry = lx;
                lx = rx;
                ly = ry;
            }
            UVs[ElemId] = FVector2f(
                static_cast<float>(Pad + (Item.PlacedPos.X + lx) * Scale),
                static_cast<float>(Pad + (Item.PlacedPos.Y + ly) * Scale));
            Res.ModifiedElements.Add(ElemId);
        }
    }
    Doc.SetAllElementUVs(UVs);
    Res.bOk = true;
    Res.Message = FString::Printf(TEXT("Packed %d UV island(s) into [0,1] (padding=%.3f)."), Items.Num(), Pad);
    return Res;
}

FNGonUVOpResult FNGonUVAPI::RelaxSelected(FNGonUVDocument& Doc, int32 Iterations, float Strength, bool bPinBorder)
{
    FNGonUVOpResult Res;
    Res.Operation = TEXT("relax");
    const TArray<int32> Targets = ResolveTargetElements(Doc, true);
    if (Targets.IsEmpty())
    {
        Res.Message = TEXT("No UV elements to relax.");
        return Res;
    }

    TMap<int32, TArray<int32>> Neighbors;
    for (const FNGonUVDocument::FTriangle& Tri : Doc.GetTriangles())
    {
        for (int32 c = 0; c < 3; ++c)
        {
            const int32 A = Tri.Element[c];
            const int32 B = Tri.Element[(c + 1) % 3];
            if (A != INDEX_NONE && B != INDEX_NONE && A != B)
            {
                Neighbors.FindOrAdd(A).AddUnique(B);
                Neighbors.FindOrAdd(B).AddUnique(A);
            }
        }
    }

    const TSet<int32> Border = bPinBorder ? FindBorderElements(Doc) : TSet<int32>();
    const int32 Iters = FMath::Clamp(Iterations, 1, 64);
    const float Alpha = FMath::Clamp(Strength, 0.05f, 1.0f);

    PushUndoSnapshot(Doc);
    TMap<int32, FVector2f> UVs = Doc.GetElementUVs();
    for (int32 Iter = 0; Iter < Iters; ++Iter)
    {
        TMap<int32, FVector2f> NextUVs = UVs;
        for (const int32 ElemId : Targets)
        {
            if (bPinBorder && Border.Contains(ElemId)) continue;
            const TArray<int32>* Adj = Neighbors.Find(ElemId);
            if (!Adj || Adj->IsEmpty()) continue;
            FVector2f Avg = FVector2f::ZeroVector;
            for (const int32 Nb : *Adj) Avg += UVs.FindRef(Nb);
            Avg /= static_cast<float>(Adj->Num());
            NextUVs[ElemId] = FMath::Lerp(UVs[ElemId], Avg, Alpha);
        }
        UVs = MoveTemp(NextUVs);
    }

    Doc.SetAllElementUVs(UVs);
    Res.bOk = true;
    Res.ModifiedElements = Targets;
    Res.Message = FString::Printf(TEXT("Relaxed %d UV corner(s) (%d iters, strength=%.2f, pin_border=%s)."),
        Targets.Num(), Iters, Alpha, bPinBorder ? TEXT("true") : TEXT("false"));
    return Res;
}

FNGonUVOpResult FNGonUVAPI::StackSelectedIslands(FNGonUVDocument& Doc)
{
    FNGonUVOpResult Res;
    Res.Operation = TEXT("stack_islands");
    const FNGonUVAnalysis Analysis = AnalyzeUVs(Doc);
    if (Analysis.Islands.Num() < 2)
    {
        Res.Message = TEXT("Need at least 2 UV islands to stack.");
        return Res;
    }

    const TSet<int32>& Sel = Doc.GetSelectedElements();
    TArray<int32> TargetIslands;
    for (int32 i = 0; i < Analysis.Islands.Num(); ++i)
    {
        if (Sel.IsEmpty())
        {
            TargetIslands.Add(i);
        }
        else
        {
            for (const int32 E : Analysis.Islands[i].Elements)
            {
                if (Sel.Contains(E)) { TargetIslands.Add(i); break; }
            }
        }
    }
    if (TargetIslands.Num() < 2)
    {
        Res.Message = TEXT("Select at least 2 UV islands (or clear selection to stack all).");
        return Res;
    }

    const FVector2D AnchorCenter = Analysis.Islands[TargetIslands[0]].Bounds.GetCenter();
    PushUndoSnapshot(Doc);
    TMap<int32, FVector2f> UVs = Doc.GetElementUVs();
    for (int32 idx = 1; idx < TargetIslands.Num(); ++idx)
    {
        const FNGonUVIslandInfo& Isl = Analysis.Islands[TargetIslands[idx]];
        const FVector2f Delta = FVector2f(AnchorCenter - Isl.Bounds.GetCenter());
        for (const int32 E : Isl.Elements)
        {
            UVs[E] += Delta;
            Res.ModifiedElements.Add(E);
        }
    }
    Doc.SetAllElementUVs(UVs);
    Res.bOk = true;
    Res.Message = FString::Printf(TEXT("Stacked %d UV island(s) onto anchor center."), TargetIslands.Num());
    return Res;
}

FNGonUVOpResult FNGonUVAPI::DistributeSelectedIslands(FNGonUVDocument& Doc, bool bHorizontal, float Spacing)
{
    FNGonUVOpResult Res;
    Res.Operation = TEXT("distribute_islands");
    const FNGonUVAnalysis Analysis = AnalyzeUVs(Doc);
    if (Analysis.Islands.Num() < 2)
    {
        Res.Message = TEXT("Need at least 2 UV islands to distribute.");
        return Res;
    }

    const TSet<int32>& Sel = Doc.GetSelectedElements();
    TArray<int32> TargetIslands;
    for (int32 i = 0; i < Analysis.Islands.Num(); ++i)
    {
        if (Sel.IsEmpty())
        {
            TargetIslands.Add(i);
        }
        else
        {
            for (const int32 E : Analysis.Islands[i].Elements)
            {
                if (Sel.Contains(E)) { TargetIslands.Add(i); break; }
            }
        }
    }
    if (TargetIslands.Num() < 2)
    {
        Res.Message = TEXT("Select at least 2 UV islands to distribute.");
        return Res;
    }

    PushUndoSnapshot(Doc);
    TMap<int32, FVector2f> UVs = Doc.GetElementUVs();
    double Cursor = bHorizontal ? Analysis.Islands[TargetIslands[0]].Bounds.Min.X : Analysis.Islands[TargetIslands[0]].Bounds.Min.Y;
    for (const int32 IslIdx : TargetIslands)
    {
        const FNGonUVIslandInfo& Isl = Analysis.Islands[IslIdx];
        const FVector2f Delta = bHorizontal
            ? FVector2f(static_cast<float>(Cursor - Isl.Bounds.Min.X), 0.0f)
            : FVector2f(0.0f, static_cast<float>(Cursor - Isl.Bounds.Min.Y));
        for (const int32 E : Isl.Elements)
        {
            UVs[E] += Delta;
            Res.ModifiedElements.Add(E);
        }
        Cursor += (bHorizontal ? Isl.Bounds.GetSize().X : Isl.Bounds.GetSize().Y) + FMath::Max(0.0f, Spacing);
    }
    Doc.SetAllElementUVs(UVs);
    Res.bOk = true;
    Res.Message = FString::Printf(TEXT("Distributed %d UV island(s) along %s."), TargetIslands.Num(), bHorizontal ? TEXT("U") : TEXT("V"));
    return Res;
}

FNGonUVOpResult FNGonUVAPI::MoveToUDIMTile(FNGonUVDocument& Doc, int32 TileU, int32 TileV, bool bNormalizeInsideTile)
{
    FNGonUVOpResult Res;
    Res.Operation = TEXT("move_to_udim");
    const TArray<int32> Targets = ResolveTargetElements(Doc, true);
    if (Targets.IsEmpty())
    {
        Res.Message = TEXT("No UV elements to move to UDIM tile.");
        return Res;
    }

    PushUndoSnapshot(Doc);
    if (bNormalizeInsideTile)
    {
        FBox2D Box(ForceInit);
        for (const int32 Id : Targets) Box += FVector2D(Doc.GetElementUV(Id));
        const double Span = FMath::Max3(Box.GetSize().X, Box.GetSize().Y, 1e-4);
        const double Scale = 0.96 / Span;
        TMap<int32, FVector2f> UVs = Doc.GetElementUVs();
        for (const int32 Id : Targets)
        {
            const FVector2f P = UVs[Id];
            UVs[Id] = FVector2f(
                static_cast<float>(TileU + 0.02 + (P.X - Box.Min.X) * Scale),
                static_cast<float>(TileV + 0.02 + (P.Y - Box.Min.Y) * Scale));
        }
        Doc.SetAllElementUVs(UVs);
    }
    else
    {
        FBox2D Box(ForceInit);
        for (const int32 Id : Targets) Box += FVector2D(Doc.GetElementUV(Id));
        const FVector2D Offset(
            static_cast<double>(TileU) - FMath::FloorToDouble(Box.GetCenter().X),
            static_cast<double>(TileV) - FMath::FloorToDouble(Box.GetCenter().Y));
        TMap<int32, FVector2f> UVs = Doc.GetElementUVs();
        for (const int32 Id : Targets) UVs[Id] += FVector2f(Offset);
        Doc.SetAllElementUVs(UVs);
    }

    Res.bOk = true;
    Res.ModifiedElements = Targets;
    const int32 UdimId = 1001 + TileU + TileV * 10;
    Res.Message = FString::Printf(TEXT("Moved %d UV corner(s) to UDIM %d (tile %d, %d)."), Targets.Num(), UdimId, TileU, TileV);
    return Res;
}

// ============================================================================
// Analysis & AI JSON Interface
// ============================================================================

FNGonUVAnalysis FNGonUVAPI::AnalyzeUVs(const FNGonUVDocument& Doc)
{
    FNGonUVAnalysis Out;
    Out.TotalBounds = Doc.GetBounds();
    const int32 NumIslands = Doc.GetIslandCount();
    Out.Islands.SetNum(NumIslands);
    for (int32 i = 0; i < NumIslands; ++i)
    {
        Out.Islands[i].IslandId = i;
    }

    TArray<TSet<int32>> IslandElemSets;
    IslandElemSets.SetNum(NumIslands);

    const TArray<FNGonUVDocument::FTriangle>& Tris = Doc.GetTriangles();
    for (int32 t = 0; t < Tris.Num(); ++t)
    {
        const FNGonUVDocument::FTriangle& Tri = Tris[t];
        const double SignedArea = TriangleSignedArea2D(Tri.UV[0], Tri.UV[1], Tri.UV[2]);
        const double AbsArea = FMath::Abs(SignedArea);
        Out.TotalAreaUV += AbsArea;
        const bool bFlipped = SignedArea < -1e-8;
        if (bFlipped) ++Out.FlippedTriangleCount;

        if (Out.Islands.IsValidIndex(Tri.Island))
        {
            FNGonUVIslandInfo& Isl = Out.Islands[Tri.Island];
            Isl.Triangles.Add(t);
            Isl.AreaUV += AbsArea;
            if (bFlipped) ++Isl.FlippedTriangles;
            for (int32 c = 0; c < 3; ++c)
            {
                IslandElemSets[Tri.Island].Add(Tri.Element[c]);
                Isl.Bounds += FVector2D(Tri.UV[c]);
            }
        }
    }

    for (int32 i = 0; i < NumIslands; ++i)
    {
        Out.Islands[i].Elements = IslandElemSets[i].Array();
        Out.Islands[i].Elements.Sort();
    }

    for (const TPair<int32, FVector2f>& Pair : Doc.GetElementUVs())
    {
        if (Pair.Value.X < -1e-4f || Pair.Value.X > 1.0001f || Pair.Value.Y < -1e-4f || Pair.Value.Y > 1.0001f)
        {
            ++Out.OutOfUnitBoundsCorners;
        }
    }

    Out.SeamEdgeCount = FindBorderElements(Doc).Num();
    return Out;
}

FString FNGonUVAPI::ExportUVSummaryJson(const FNGonUVDocument& Doc, bool bIncludeIslands)
{
    const FNGonUVAnalysis Analysis = AnalyzeUVs(Doc);
    TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
    Root->SetNumberField(TEXT("corner_count"), Doc.GetCornerCount());
    Root->SetNumberField(TEXT("triangle_count"), Doc.GetTriangles().Num());
    Root->SetNumberField(TEXT("island_count"), Analysis.Islands.Num());
    Root->SetNumberField(TEXT("selected_corner_count"), Doc.GetSelectedElements().Num());
    Root->SetNumberField(TEXT("total_uv_area"), Analysis.TotalAreaUV);
    Root->SetNumberField(TEXT("flipped_triangle_count"), Analysis.FlippedTriangleCount);
    Root->SetNumberField(TEXT("out_of_unit_bounds_corners"), Analysis.OutOfUnitBoundsCorners);
    Root->SetNumberField(TEXT("undo_depth"), GetUndoCount(&Doc));
    Root->SetNumberField(TEXT("redo_depth"), GetRedoCount(&Doc));

    if (Analysis.TotalBounds.bIsValid)
    {
        TSharedRef<FJsonObject> B = MakeShared<FJsonObject>();
        B->SetObjectField(TEXT("min"), MakeUV2Json(Analysis.TotalBounds.Min));
        B->SetObjectField(TEXT("max"), MakeUV2Json(Analysis.TotalBounds.Max));
        B->SetObjectField(TEXT("size"), MakeUV2Json(Analysis.TotalBounds.GetSize()));
        Root->SetObjectField(TEXT("bounds"), B);
    }

    if (bIncludeIslands)
    {
        TArray<TSharedPtr<FJsonValue>> Arr;
        const int32 MaxIslands = FMath::Min(Analysis.Islands.Num(), 128);
        for (int32 i = 0; i < MaxIslands; ++i)
        {
            const FNGonUVIslandInfo& Isl = Analysis.Islands[i];
            TSharedRef<FJsonObject> IO = MakeShared<FJsonObject>();
            IO->SetNumberField(TEXT("id"), Isl.IslandId);
            IO->SetNumberField(TEXT("corners"), Isl.Elements.Num());
            IO->SetNumberField(TEXT("triangles"), Isl.Triangles.Num());
            IO->SetNumberField(TEXT("area"), Isl.AreaUV);
            IO->SetNumberField(TEXT("flipped"), Isl.FlippedTriangles);
            IO->SetObjectField(TEXT("min"), MakeUV2Json(Isl.Bounds.Min));
            IO->SetObjectField(TEXT("max"), MakeUV2Json(Isl.Bounds.Max));
            Arr.Add(MakeShared<FJsonValueObject>(IO));
        }
        Root->SetArrayField(TEXT("islands"), Arr);
    }

    return SerializeUVJson(Root, true);
}

FString FNGonUVAPI::GetApiSchemaJson()
{
    return TEXT(
        "{\n"
        "  \"api\": \"NGonUVAPI\",\n"
        "  \"version\": \"1.0\",\n"
        "  \"format\": \"Single JSON object {\\\"op\\\": ...} or JSON array [{\\\"op\\\": ...}, ...]\",\n"
        "  \"operations\": [\n"
        "    {\"op\": \"select_all\", \"params\": {}},\n"
        "    {\"op\": \"clear_selection\", \"params\": {}},\n"
        "    {\"op\": \"invert_selection\", \"params\": {}},\n"
        "    {\"op\": \"grow_selection\", \"params\": {}},\n"
        "    {\"op\": \"shrink_selection\", \"params\": {}},\n"
        "    {\"op\": \"select_island\", \"params\": {\"island_id\": 0, \"add\": false}},\n"
        "    {\"op\": \"select_linked_islands\", \"params\": {}},\n"
        "    {\"op\": \"select_seams\", \"params\": {}},\n"
        "    {\"op\": \"select_flipped\", \"params\": {}},\n"
        "    {\"op\": \"select_by_bounds\", \"params\": {\"min\": [0,0], \"max\": [1,1], \"add\": false}},\n"
        "    {\"op\": \"translate\", \"params\": {\"delta\": [0.1, 0.0]}},\n"
        "    {\"op\": \"rotate\", \"params\": {\"degrees\": 90}},\n"
        "    {\"op\": \"scale\", \"params\": {\"scale\": [1.0, 1.0]}},\n"
        "    {\"op\": \"flip\", \"params\": {\"horizontal\": true}},\n"
        "    {\"op\": \"align\", \"params\": {\"direction\": \"left|right|top|bottom|center_u|center_v\"}},\n"
        "    {\"op\": \"straighten\", \"params\": {}},\n"
        "    {\"op\": \"snap_grid\", \"params\": {\"step\": 0.0625}},\n"
        "    {\"op\": \"normalize\", \"params\": {\"keep_aspect\": true, \"padding\": 0.02}},\n"
        "    {\"op\": \"pack_islands\", \"params\": {\"padding\": 0.02, \"rotate_to_fit\": true}},\n"
        "    {\"op\": \"relax\", \"params\": {\"iterations\": 4, \"strength\": 0.45, \"pin_border\": true}},\n"
        "    {\"op\": \"stack_islands\", \"params\": {}},\n"
        "    {\"op\": \"distribute_islands\", \"params\": {\"horizontal\": true, \"spacing\": 0.03}},\n"
        "    {\"op\": \"move_to_udim\", \"params\": {\"tile_u\": 1, \"tile_v\": 0, \"normalize\": false}},\n"
        "    {\"op\": \"inspect\", \"params\": {\"include_islands\": true}},\n"
        "    {\"op\": \"undo\", \"params\": {}},\n"
        "    {\"op\": \"redo\", \"params\": {}}\n"
        "  ]\n"
        "}");
}

namespace
{
FNGonUVOpResult ExecuteSingleUVJson(FNGonUVDocument& Doc, const TSharedPtr<FJsonObject>& Obj)
{
    FNGonUVOpResult Res;
    if (!Obj.IsValid())
    {
        Res.Message = TEXT("Invalid JSON object.");
        return Res;
    }

    FString Op;
    if (!Obj->TryGetStringField(TEXT("op"), Op)) Obj->TryGetStringField(TEXT("command"), Op);
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

    if (Op == TEXT("select_all")) return FNGonUVAPI::SelectAll(Doc);
    if (Op == TEXT("clear_selection")) return FNGonUVAPI::ClearSelection(Doc);
    if (Op == TEXT("invert_selection")) return FNGonUVAPI::InvertSelection(Doc);
    if (Op == TEXT("grow_selection")) return FNGonUVAPI::GrowSelection(Doc);
    if (Op == TEXT("shrink_selection")) return FNGonUVAPI::ShrinkSelection(Doc);
    if (Op == TEXT("select_elements")) return FNGonUVAPI::SelectElements(Doc, ReadUVIntArray(Obj, TEXT("ids")), GetBool(TEXT("add"), false));
    if (Op == TEXT("select_island")) return FNGonUVAPI::SelectIsland(Doc, GetInt(TEXT("island_id"), 0), GetBool(TEXT("add"), false));
    if (Op == TEXT("select_linked_islands") || Op == TEXT("select_linked")) return FNGonUVAPI::SelectLinkedIslands(Doc);
    if (Op == TEXT("select_seams")) return FNGonUVAPI::SelectSeamCorners(Doc);
    if (Op == TEXT("select_flipped")) return FNGonUVAPI::SelectFlippedTriangles(Doc);
    if (Op == TEXT("select_by_bounds"))
    {
        FVector2f MinP, MaxP;
        ReadVec2FromJson(Obj, TEXT("min"), MinP, FVector2f(0.0f, 0.0f));
        ReadVec2FromJson(Obj, TEXT("max"), MaxP, FVector2f(1.0f, 1.0f));
        return FNGonUVAPI::SelectByBounds(Doc, FBox2D(FVector2D(MinP), FVector2D(MaxP)), GetBool(TEXT("add"), false));
    }
    if (Op == TEXT("translate") || Op == TEXT("move"))
    {
        FVector2f Delta;
        ReadVec2FromJson(Obj, TEXT("delta"), Delta, FVector2f::ZeroVector);
        return FNGonUVAPI::TranslateSelected(Doc, Delta);
    }
    if (Op == TEXT("rotate")) return FNGonUVAPI::RotateSelected(Doc, static_cast<float>(GetNum(TEXT("degrees"), 90.0)));
    if (Op == TEXT("scale"))
    {
        FVector2f Sc;
        if (!ReadVec2FromJson(Obj, TEXT("scale"), Sc, FVector2f(1.0f, 1.0f)))
        {
            const float Uni = static_cast<float>(GetNum(TEXT("factor"), 1.0));
            Sc = FVector2f(Uni, Uni);
        }
        return FNGonUVAPI::ScaleSelected(Doc, Sc);
    }
    if (Op == TEXT("flip")) return FNGonUVAPI::FlipSelected(Doc, GetBool(TEXT("horizontal"), true));
    if (Op == TEXT("align"))
    {
        const FString D = GetStr(TEXT("direction"), TEXT("left")).ToLower();
        ENGonUVAlign Mode = ENGonUVAlign::Left;
        if (D == TEXT("right")) Mode = ENGonUVAlign::Right;
        else if (D == TEXT("top")) Mode = ENGonUVAlign::Top;
        else if (D == TEXT("bottom")) Mode = ENGonUVAlign::Bottom;
        else if (D == TEXT("center_u")) Mode = ENGonUVAlign::CenterU;
        else if (D == TEXT("center_v")) Mode = ENGonUVAlign::CenterV;
        return FNGonUVAPI::AlignSelected(Doc, Mode);
    }
    if (Op == TEXT("straighten")) return FNGonUVAPI::StraightenSelected(Doc);
    if (Op == TEXT("snap_grid") || Op == TEXT("snap")) return FNGonUVAPI::SnapSelectedToGrid(Doc, static_cast<float>(GetNum(TEXT("step"), 0.0625)));
    if (Op == TEXT("normalize")) return FNGonUVAPI::NormalizeSelected(Doc, GetBool(TEXT("keep_aspect"), true), static_cast<float>(GetNum(TEXT("padding"), 0.02)));
    if (Op == TEXT("pack_islands") || Op == TEXT("pack")) return FNGonUVAPI::PackIslands(Doc, static_cast<float>(GetNum(TEXT("padding"), 0.02)), GetBool(TEXT("rotate_to_fit"), true));
    if (Op == TEXT("relax")) return FNGonUVAPI::RelaxSelected(Doc, GetInt(TEXT("iterations"), 4), static_cast<float>(GetNum(TEXT("strength"), 0.45)), GetBool(TEXT("pin_border"), true));
    if (Op == TEXT("stack_islands") || Op == TEXT("stack")) return FNGonUVAPI::StackSelectedIslands(Doc);
    if (Op == TEXT("distribute_islands") || Op == TEXT("distribute")) return FNGonUVAPI::DistributeSelectedIslands(Doc, GetBool(TEXT("horizontal"), true), static_cast<float>(GetNum(TEXT("spacing"), 0.03)));
    if (Op == TEXT("move_to_udim") || Op == TEXT("udim")) return FNGonUVAPI::MoveToUDIMTile(Doc, GetInt(TEXT("tile_u"), 1), GetInt(TEXT("tile_v"), 0), GetBool(TEXT("normalize"), false));
    if (Op == TEXT("inspect") || Op == TEXT("summary"))
    {
        Res.bOk = true;
        Res.PayloadJson = FNGonUVAPI::ExportUVSummaryJson(Doc, GetBool(TEXT("include_islands"), true));
        Res.Message = TEXT("Exported UV summary JSON.");
        return Res;
    }
    if (Op == TEXT("undo")) return FNGonUVAPI::Undo(Doc);
    if (Op == TEXT("redo")) return FNGonUVAPI::Redo(Doc);

    Res.Message = FString::Printf(TEXT("Unknown UV API operation: '%s'."), *Op);
    return Res;
}
} // anonymous namespace

FNGonUVOpResult FNGonUVAPI::ExecuteJsonCommand(FNGonUVDocument& Doc, const FString& JsonText)
{
    FNGonUVOpResult Res;
    Res.Operation = TEXT("execute_uv_json");
    const FString Trimmed = JsonText.TrimStartAndEnd();
    if (Trimmed.IsEmpty())
    {
        Res.Message = TEXT("JSON command is empty.");
        return Res;
    }

    if (Trimmed.StartsWith(TEXT("[")))
    {
        TArray<TSharedPtr<FJsonValue>> Commands;
        const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Trimmed);
        if (!FJsonSerializer::Deserialize(Reader, Commands) || Commands.IsEmpty())
        {
            Res.Message = TEXT("Failed to parse non-empty JSON command array.");
            return Res;
        }
        TArray<FString> Logs;
        int32 Step = 0;
        for (const TSharedPtr<FJsonValue>& Val : Commands)
        {
            ++Step;
            if (!Val.IsValid() || Val->Type != EJson::Object)
            {
                Res.Message = FString::Printf(TEXT("Step %d is not a JSON object."), Step);
                return Res;
            }
            const FNGonUVOpResult StepRes = ExecuteSingleUVJson(Doc, Val->AsObject());
            Logs.Add(FString::Printf(TEXT("[%d] %s: %s"), Step, *StepRes.Operation, *StepRes.Message));
            if (!StepRes.bOk)
            {
                Res.Message = FString::Printf(TEXT("Batch stopped at step %d (%s): %s"), Step, *StepRes.Operation, *StepRes.Message);
                return Res;
            }
            Res.SelectedElements = StepRes.SelectedElements;
            if (!StepRes.PayloadJson.IsEmpty()) Res.PayloadJson = StepRes.PayloadJson;
        }
        Res.bOk = true;
        Res.Message = FString::Printf(TEXT("Executed %d UV command(s).\n%s"), Commands.Num(), *FString::Join(Logs, TEXT("\n")));
        return Res;
    }

    TSharedPtr<FJsonObject> Obj;
    const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Trimmed);
    if (!FJsonSerializer::Deserialize(Reader, Obj) || !Obj.IsValid())
    {
        Res.Message = TEXT("Failed to parse JSON object.");
        return Res;
    }
    return ExecuteSingleUVJson(Doc, Obj);
}
// NGon Unified ABI Sync v5.8.2
