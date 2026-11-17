#include "NGonUVEditor.h"

#include "NGonFbx.h"
#include "NGonMeshFiles.h"
#include "NGonTopology.h"
#include "DynamicMesh/DynamicMeshAttributeSet.h"
#include "DynamicMesh/DynamicMesh3.h"
#include "Framework/Application/SlateApplication.h"
#include "Rendering/DrawElements.h"
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
float DistSq(const FVector2f& A, const FVector2f& B) { return (A - B).SizeSquared(); }
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
    for (const int32 TriangleId : Mesh.TriangleIndicesItr())
    {
        if (!UVs->IsSetTriangle(TriangleId)) continue;
        const FIndex3i Elements = UVs->GetTriangle(TriangleId);
        FTriangle& Out = Triangles.AddDefaulted_GetRef();
        Out.Element[0] = Elements.A; Out.Element[1] = Elements.B; Out.Element[2] = Elements.C;
        Out.FaceId = Mesh.GetTriangleGroup(TriangleId);
        for (int32 Corner = 0; Corner < 3; ++Corner)
        {
            const int32 ElementId = Out.Element[Corner];
            Out.UV[Corner] = UVs->GetElement(ElementId);
            ElementUV.FindOrAdd(ElementId) = Out.UV[Corner];
            ElementTriangles.FindOrAdd(ElementId).Add(Triangles.Num() - 1);
            Bounds += FVector2D(Out.UV[Corner]);
        }
    }
    if (Triangles.IsEmpty())
    {
        Error = LOCTEXT("NoTriangleUVs", "The mesh primary UV channel contains no triangle data.");
        return false;
    }
    RebuildIslands();
    Error = FText::GetEmpty();
    return true;
}

void FNGonUVDocument::Clear()
{
    Triangles.Reset(); ElementUV.Reset(); ElementTriangles.Reset();
    SelectedElements.Reset(); SelectedFaces.Reset(); SelectedIslands.Reset();
    Bounds = FBox2D(ForceInit); IslandCount = 0;
}

FVector2f FNGonUVDocument::GetElementUV(int32 ElementId) const
{
    if (const FVector2f* UV = ElementUV.Find(ElementId)) return *UV;
    return FVector2f::ZeroVector;
}

void FNGonUVDocument::RebuildBounds()
{
    Bounds = FBox2D(ForceInit);
    for (const TPair<int32, FVector2f>& Pair : ElementUV) Bounds += FVector2D(Pair.Value);
}

void FNGonUVDocument::RebuildIslands()
{
    TArray<int32> Parent; Parent.SetNum(Triangles.Num());
    for (int32 I = 0; I < Parent.Num(); ++I) Parent[I] = I;
    TFunction<int32(int32)> Find = [&Parent, &Find](int32 I) -> int32 { return Parent[I] == I ? I : Parent[I] = Find(Parent[I]); };
    auto Union = [&Parent, &Find](int32 A, int32 B) { A = Find(A); B = Find(B); if (A != B) Parent[B] = A; };
    for (const TPair<int32, TArray<int32>>& Pair : ElementTriangles)
    {
        for (int32 I = 1; I < Pair.Value.Num(); ++I) Union(Pair.Value[0], Pair.Value[I]);
    }
    TMap<int32, int32> Remap; IslandCount = 0;
    for (int32 I = 0; I < Triangles.Num(); ++I)
    {
        const int32 Root = Find(I);
        int32* Island = Remap.Find(Root);
        if (!Island) Island = &Remap.Add(Root, IslandCount++);
        Triangles[I].Island = *Island;
    }
}

void FNGonUVDocument::ClearSelection()
{
    SelectedElements.Reset(); SelectedFaces.Reset(); SelectedIslands.Reset();
}

void FNGonUVDocument::AddElementSelection(int32 Id, bool bAdd)
{
    if (!bAdd) ClearSelection();
    if (Id == INDEX_NONE) return;
    SelectedElements.Add(Id);
    if (const TArray<int32>* Tris = ElementTriangles.Find(Id))
    {
        for (int32 Tri : *Tris) { SelectedFaces.Add(Triangles[Tri].FaceId); SelectedIslands.Add(Triangles[Tri].Island); }
    }
}

void FNGonUVDocument::SelectFaceTriangle(int32 TriangleIndex, bool bAdd)
{
    if (!Triangles.IsValidIndex(TriangleIndex)) return;
    if (!bAdd) ClearSelection();
    SelectedFaces.Add(Triangles[TriangleIndex].FaceId);
    for (const FTriangle& Tri : Triangles)
    {
        if (Tri.FaceId == Triangles[TriangleIndex].FaceId)
        {
            SelectedElements.Add(Tri.Element[0]); SelectedElements.Add(Tri.Element[1]); SelectedElements.Add(Tri.Element[2]);
            SelectedIslands.Add(Tri.Island);
        }
    }
}

void FNGonUVDocument::SelectIsland(int32 Island, bool bAdd)
{
    if (!bAdd) ClearSelection();
    if (Island < 0) return;
    SelectedIslands.Add(Island);
    for (const FTriangle& Tri : Triangles) if (Tri.Island == Island)
    {
        SelectedElements.Add(Tri.Element[0]); SelectedElements.Add(Tri.Element[1]); SelectedElements.Add(Tri.Element[2]);
        SelectedFaces.Add(Tri.FaceId);
    }
}

void FNGonUVDocument::SelectAtUV(const FVector2f& UV, bool bAdd, float Radius)
{
    int32 BestElement = INDEX_NONE, BestTri = INDEX_NONE; float Best = Radius * Radius;
    for (const TPair<int32, FVector2f>& Pair : ElementUV) { const float D = DistSq(UV, Pair.Value); if (D <= Best) { Best = D; BestElement = Pair.Key; } }
    float BestFace = TNumericLimits<float>::Max();
    for (int32 I = 0; I < Triangles.Num(); ++I)
    {
        const FTriangle& T = Triangles[I];
        const FVector2f C = (T.UV[0] + T.UV[1] + T.UV[2]) / 3.0f;
        const float D = DistSq(UV, C); if (D < BestFace) { BestFace = D; BestTri = I; }
    }
    switch (SelectionMode)
    {
    case ENGonUVSelectionMode::Corner: AddElementSelection(BestElement, bAdd); break;
    case ENGonUVSelectionMode::Face: SelectFaceTriangle(BestTri, bAdd); break;
    case ENGonUVSelectionMode::Island: if (Triangles.IsValidIndex(BestTri)) SelectIsland(Triangles[BestTri].Island, bAdd); break;
    case ENGonUVSelectionMode::Edge:
    {
        if (Triangles.IsValidIndex(BestTri))
        {
            const FTriangle& T = Triangles[BestTri]; int32 A = 0, B = 1; float DBest = TNumericLimits<float>::Max();
            for (int32 E = 0; E < 3; ++E) { const int32 N = (E + 1) % 3; const FVector2f Mid = (T.UV[E] + T.UV[N]) * 0.5f; const float D = DistSq(UV, Mid); if (D < DBest) { DBest = D; A = E; B = N; } }
            AddElementSelection(T.Element[A], bAdd); SelectedElements.Add(T.Element[B]);
        }
        break;
    }
    }
}

void FNGonUVDocument::SelectRect(const FBox2D& Rect, bool bAdd)
{
    if (!bAdd) ClearSelection();
    for (const TPair<int32, FVector2f>& Pair : ElementUV) if (Rect.IsInside(FVector2D(Pair.Value))) SelectedElements.Add(Pair.Key);
    for (const TPair<int32, TArray<int32>>& Pair : ElementTriangles) if (SelectedElements.Contains(Pair.Key))
        for (int32 Tri : Pair.Value) { SelectedFaces.Add(Triangles[Tri].FaceId); SelectedIslands.Add(Triangles[Tri].Island); }
}

void FNGonUVDocument::TranslateSelected(const FVector2f& Delta)
{
    for (int32 Id : SelectedElements) if (FVector2f* UV = ElementUV.Find(Id)) *UV += Delta;
    for (FTriangle& Tri : Triangles) for (int32 C = 0; C < 3; ++C) Tri.UV[C] = GetElementUV(Tri.Element[C]);
    RebuildBounds();
}

void FNGonUVDocument::RotateSelected(float Degrees)
{
    if (SelectedElements.IsEmpty()) return; FVector2f Center = FVector2f::ZeroVector; for (int32 Id : SelectedElements) Center += GetElementUV(Id); Center /= SelectedElements.Num();
    const float R = FMath::DegreesToRadians(Degrees), C = FMath::Cos(R), S = FMath::Sin(R);
    for (int32 Id : SelectedElements) { FVector2f P = GetElementUV(Id) - Center; ElementUV[Id] = Center + FVector2f(P.X * C - P.Y * S, P.X * S + P.Y * C); }
    for (FTriangle& Tri : Triangles) for (int32 I = 0; I < 3; ++I) Tri.UV[I] = GetElementUV(Tri.Element[I]); RebuildBounds();
}

void FNGonUVDocument::ScaleSelected(float Factor)
{
    if (SelectedElements.IsEmpty()) return; FVector2f Center = FVector2f::ZeroVector; for (int32 Id : SelectedElements) Center += GetElementUV(Id); Center /= SelectedElements.Num();
    for (int32 Id : SelectedElements) ElementUV[Id] = Center + (GetElementUV(Id) - Center) * Factor;
    for (FTriangle& Tri : Triangles) for (int32 I = 0; I < 3; ++I) Tri.UV[I] = GetElementUV(Tri.Element[I]); RebuildBounds();
}

void FNGonUVDocument::FlipSelected(bool bHorizontal) { ScaleSelected(1.0f); RotateSelected(0.0f); if (SelectedElements.IsEmpty()) return; FVector2f Center = FVector2f::ZeroVector; for (int32 Id : SelectedElements) Center += GetElementUV(Id); Center /= SelectedElements.Num(); for (int32 Id : SelectedElements) { FVector2f P = GetElementUV(Id); if (bHorizontal) P.X = Center.X - (P.X - Center.X); else P.Y = Center.Y - (P.Y - Center.Y); ElementUV[Id] = P; } for (FTriangle& Tri : Triangles) for (int32 I = 0; I < 3; ++I) Tri.UV[I] = GetElementUV(Tri.Element[I]); RebuildBounds(); }

void FNGonUVDocument::NormalizeSelected()
{
    if (SelectedElements.IsEmpty()) return; FBox2D B(ForceInit); for (int32 Id : SelectedElements) B += FVector2D(GetElementUV(Id)); const FVector2D Size = B.GetSize(); if (Size.X <= KINDA_SMALL_NUMBER || Size.Y <= KINDA_SMALL_NUMBER) return; for (int32 Id : SelectedElements) { const FVector2D P = FVector2D(GetElementUV(Id)); ElementUV[Id] = FVector2f((P.X - B.Min.X) / Size.X, (P.Y - B.Min.Y) / Size.Y); } for (FTriangle& Tri : Triangles) for (int32 I = 0; I < 3; ++I) Tri.UV[I] = GetElementUV(Tri.Element[I]); RebuildBounds();
}

FText FNGonUVDocument::Describe() const { return FText::Format(LOCTEXT("UVStatus", "{0} UV corners  |  {1} islands  |  {2} selected"), FText::AsNumber(ElementUV.Num()), FText::AsNumber(IslandCount), FText::AsNumber(SelectedElements.Num())); }

void SNGonUVCanvas::Construct(const FArguments&, const TSharedRef<FNGonUVDocument>& InDocument) { Document = InDocument; }
void SNGonUVCanvas::FitToCanvas() { bNeedsFit = true; Invalidate(EInvalidateWidgetReason::Paint); }
void SNGonUVCanvas::SetSelectionMode(ENGonUVSelectionMode Mode) { Document->SetSelectionMode(Mode); Invalidate(EInvalidateWidgetReason::Paint); }
void SNGonUVCanvas::Refresh() { Invalidate(EInvalidateWidgetReason::Paint); }
void SNGonUVCanvas::UpdateViewFromBounds(const FGeometry& Geometry) const { if (!bNeedsFit || Document->IsEmpty()) return; const FVector2D Size = Geometry.GetLocalSize(); const FVector2D BSize = Document->GetBounds().GetSize(); ViewCenter = FVector2f(Document->GetBounds().GetCenter()); Zoom = FMath::Max(40.0f, static_cast<float>(FMath::Min(Size.X / FMath::Max(BSize.X, 0.01), Size.Y / FMath::Max(BSize.Y, 0.01)) * 0.8)); bNeedsFit = false; }
FVector2f SNGonUVCanvas::ScreenToUV(const FGeometry& Geometry, const FVector2D& Screen) const { UpdateViewFromBounds(Geometry); const FVector2D C = Geometry.GetLocalSize() * 0.5; return ViewCenter + FVector2f(static_cast<float>((Screen.X - C.X) / Zoom), static_cast<float>((C.Y - Screen.Y) / Zoom)); }
FVector2D SNGonUVCanvas::UVToScreen(const FGeometry& Geometry, const FVector2f& UV) const { UpdateViewFromBounds(Geometry); const FVector2D C = Geometry.GetLocalSize() * 0.5; return C + FVector2D((UV.X - ViewCenter.X) * Zoom, -(UV.Y - ViewCenter.Y) * Zoom); }

int32 SNGonUVCanvas::OnPaint(const FPaintArgs& Args, const FGeometry& Geo, const FSlateRect& Clip, FSlateWindowElementList& Out, int32 Layer, const FWidgetStyle& Style, bool Enabled) const
{
    UpdateViewFromBounds(Geo); const FVector2D Size = Geo.GetLocalSize();
    static FSlateBrush BoxBrush; BoxBrush.DrawAs = ESlateBrushDrawType::Box;
    FSlateDrawElement::MakeBox(Out, Layer, Geo.ToPaintGeometry(Size, FSlateLayoutTransform()), &BoxBrush, ESlateDrawEffect::None, FLinearColor(0.035f, 0.045f, 0.06f, 1));
    const float Grid = FMath::Max(8.0f, Zoom * 0.25f); for (float X = FMath::Fmod(Size.X * 0.5f - ViewCenter.X * Zoom, Grid); X < Size.X; X += Grid) FSlateDrawElement::MakeLines(Out, Layer + 1, Geo.ToPaintGeometry(), { FVector2D(X, 0), FVector2D(X, Size.Y) }, ESlateDrawEffect::None, FLinearColor(0.12f, 0.15f, 0.2f, 1), false, 1); for (float Y = FMath::Fmod(Size.Y * 0.5f + ViewCenter.Y * Zoom, Grid); Y < Size.Y; Y += Grid) FSlateDrawElement::MakeLines(Out, Layer + 1, Geo.ToPaintGeometry(), { FVector2D(0, Y), FVector2D(Size.X, Y) }, ESlateDrawEffect::None, FLinearColor(0.12f, 0.15f, 0.2f, 1), false, 1);
    for (const FNGonUVDocument::FTriangle& T : Document->GetTriangles())
    {
        TArray<FVector2D> Line{ UVToScreen(Geo, T.UV[0]), UVToScreen(Geo, T.UV[1]), UVToScreen(Geo, T.UV[2]), UVToScreen(Geo, T.UV[0]) };
        const bool bSelected = Document->GetSelectedElements().Contains(T.Element[0]) || Document->GetSelectedFaces().Contains(T.FaceId);
        FSlateDrawElement::MakeLines(Out, Layer + 2, Geo.ToPaintGeometry(), Line, ESlateDrawEffect::None, bSelected ? FLinearColor(0.95f, 0.65f, 0.12f) : FLinearColor(0.5f, 0.72f, 0.95f), false, bSelected ? 2.5f : 1.2f);
    }
    for (const TPair<int32, FVector2f>& Pair : Document->GetElementUVs()) { const FVector2D P = UVToScreen(Geo, Pair.Value); const float R = Document->GetSelectedElements().Contains(Pair.Key) ? 5.0f : 3.0f; FSlateDrawElement::MakeBox(Out, Layer + 3, Geo.ToPaintGeometry(FVector2D(R * 2), FSlateLayoutTransform(P - FVector2D(R))), &BoxBrush, ESlateDrawEffect::None, Document->GetSelectedElements().Contains(Pair.Key) ? FLinearColor(1, 0.6f, 0.1f) : FLinearColor(0.2f, 0.85f, 1)); }
    if (bMarquee) { const FVector2D A = UVToScreen(Geo, FVector2f(Marquee.Min)); const FVector2D B = UVToScreen(Geo, FVector2f(Marquee.Max)); FSlateDrawElement::MakeBox(Out, Layer + 4, Geo.ToPaintGeometry(FVector2D(FMath::Abs(B.X - A.X), FMath::Abs(B.Y - A.Y)), FSlateLayoutTransform(FVector2D(FMath::Min(A.X, B.X), FMath::Min(A.Y, B.Y)))), &BoxBrush, ESlateDrawEffect::None, FLinearColor(0.2f, 0.6f, 1, 0.18f)); }
    return Layer + 4;
}

FReply SNGonUVCanvas::OnMouseButtonDown(const FGeometry& Geo, const FPointerEvent& E) { const FVector2D P = Geo.AbsoluteToLocal(E.GetScreenSpacePosition()); LastMouse = P; DragStart = P; LastUV = ScreenToUV(Geo, P); const FKey Button = E.GetEffectingButton(); if (Button == EKeys::MiddleMouseButton || Button == EKeys::RightMouseButton) { bPanning = true; return FReply::Handled().CaptureMouse(AsShared()); } if (Button == EKeys::LeftMouseButton) { if (E.IsControlDown()) { bMarquee = true; Marquee = FBox2D(FVector2D(LastUV), FVector2D(LastUV)); } else { Document->SelectAtUV(LastUV, E.IsShiftDown(), 0.04f); bDragging = true; } Invalidate(EInvalidateWidgetReason::Paint); return FReply::Handled().CaptureMouse(AsShared()); } return FReply::Unhandled(); }
FReply SNGonUVCanvas::OnMouseButtonUp(const FGeometry& Geo, const FPointerEvent& E) { if (bMarquee) { Document->SelectRect(Marquee, E.IsShiftDown()); bMarquee = false; } bDragging = false; bPanning = false; Invalidate(EInvalidateWidgetReason::Paint); return FReply::Handled().ReleaseMouseCapture(); }
FReply SNGonUVCanvas::OnMouseMove(const FGeometry& Geo, const FPointerEvent& E) { const FVector2D P = Geo.AbsoluteToLocal(E.GetScreenSpacePosition()); if (bPanning) { const FVector2D D = P - LastMouse; ViewCenter -= FVector2f(static_cast<float>(D.X / Zoom), static_cast<float>(-D.Y / Zoom)); LastMouse = P; } else if (bMarquee) { const FVector2f UV = ScreenToUV(Geo, P); Marquee.Max = FVector2D(UV); } else if (bDragging) { const FVector2f UV = ScreenToUV(Geo, P); Document->TranslateSelected(UV - LastUV); LastUV = UV; } Invalidate(EInvalidateWidgetReason::Paint); return FReply::Handled(); }
FReply SNGonUVCanvas::OnMouseWheel(const FGeometry& Geo, const FPointerEvent& E) { const FVector2D Local = Geo.AbsoluteToLocal(E.GetScreenSpacePosition()); const FVector2f Before = ScreenToUV(Geo, Local); Zoom = FMath::Clamp(Zoom * FMath::Pow(1.15f, E.GetWheelDelta()), 20.0f, 5000.0f); const FVector2f After = ScreenToUV(Geo, Local); ViewCenter += Before - After; Invalidate(EInvalidateWidgetReason::Paint); return FReply::Handled(); }

void SNGonUVEditor::Construct(const FArguments&)
{
    Document = MakeShared<FNGonUVDocument>(); Canvas = SNew(SNGonUVCanvas, Document.ToSharedRef()); SourceLabel = LOCTEXT("NoSource", "No UV mesh loaded"); Reload();
    ChildSlot[SNew(SVerticalBox) + SVerticalBox::Slot().AutoHeight()[MakeToolbar()] + SVerticalBox::Slot().FillHeight(1)[Canvas.ToSharedRef()] + SVerticalBox::Slot().AutoHeight()[MakeStatus()]];
}

FReply SNGonUVEditor::Reload() { FString Path; FText Error; FNGonMeshData Data; if (NGon::FindDefaultFbx(NGon::GetMeshesDirectory(), Path, Error) && NGon::LoadFbx(Path, Data, Error) && Document->Load(Data, Error)) SourceLabel = FText::FromString(FPaths::GetCleanFilename(Path)); else if (NGon::MakeDemo(Data, Error) && Document->Load(Data, Error)) SourceLabel = LOCTEXT("DemoSource", "Demo mesh"); else ShowError(Error); Canvas->FitToCanvas(); return FReply::Handled(); }
void SNGonUVEditor::SetMode(ENGonUVSelectionMode Mode) { Document->SetSelectionMode(Mode); Canvas->Refresh(); }
void SNGonUVEditor::ApplyCommand(const TCHAR* Command) { if (FCString::Stricmp(Command, TEXT("Rotate")) == 0) Document->RotateSelected(90); else if (FCString::Stricmp(Command, TEXT("FlipU")) == 0) Document->FlipSelected(true); else if (FCString::Stricmp(Command, TEXT("FlipV")) == 0) Document->FlipSelected(false); else if (FCString::Stricmp(Command, TEXT("Normalize")) == 0) Document->NormalizeSelected(); Canvas->Refresh(); }
TSharedRef<SWidget> SNGonUVEditor::MakeModeButton(ENGonUVSelectionMode Mode, const FText& Label) { return SNew(SButton).Text(Label).OnClicked_Lambda([this, Mode] { SetMode(Mode); return FReply::Handled(); }); }
TSharedRef<SWidget> SNGonUVEditor::MakeToolbar() { return SNew(SBorder).Padding(6)[SNew(SWrapBox).UseAllottedSize(true).InnerSlotPadding(FVector2D(4, 2)) + SWrapBox::Slot()[MakeModeButton(ENGonUVSelectionMode::Corner, LOCTEXT("Corner", "Corner"))] + SWrapBox::Slot()[MakeModeButton(ENGonUVSelectionMode::Edge, LOCTEXT("Edge", "Edge"))] + SWrapBox::Slot()[MakeModeButton(ENGonUVSelectionMode::Face, LOCTEXT("Face", "Face"))] + SWrapBox::Slot()[MakeModeButton(ENGonUVSelectionMode::Island, LOCTEXT("Island", "Island"))] + SWrapBox::Slot()[SNew(SButton).Text(LOCTEXT("Fit", "Fit")).OnClicked_Lambda([this] { Canvas->FitToCanvas(); return FReply::Handled(); })] + SWrapBox::Slot()[SNew(SButton).Text(LOCTEXT("Rotate", "Rotate 90")).OnClicked_Lambda([this] { ApplyCommand(TEXT("Rotate")); return FReply::Handled(); })] + SWrapBox::Slot()[SNew(SButton).Text(LOCTEXT("FlipU", "Flip U")).OnClicked_Lambda([this] { ApplyCommand(TEXT("FlipU")); return FReply::Handled(); })] + SWrapBox::Slot()[SNew(SButton).Text(LOCTEXT("FlipV", "Flip V")).OnClicked_Lambda([this] { ApplyCommand(TEXT("FlipV")); return FReply::Handled(); })] + SWrapBox::Slot()[SNew(SButton).Text(LOCTEXT("Normalize", "Normalize")).OnClicked_Lambda([this] { ApplyCommand(TEXT("Normalize")); return FReply::Handled(); })] + SWrapBox::Slot()[SNew(SButton).Text(LOCTEXT("ReloadUV", "Reload")).OnClicked(this, &SNGonUVEditor::Reload)]]; }
TSharedRef<SWidget> SNGonUVEditor::MakeStatus() { return SNew(SBorder).Padding(6)[SNew(STextBlock).Text_Lambda([this] { return FText::Format(LOCTEXT("Status", "{0}  |  {1}"), SourceLabel, Document->Describe()); })]; }
void SNGonUVEditor::ShowError(const FText& Error) { ErrorText = Error; }

#undef LOCTEXT_NAMESPACE
