#include "SNGonPreview.h"
#include "NGonMeshAPI.h"

#include "HAL/PlatformApplicationMisc.h"
#include "Styling/CoreStyle.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Input/SMultiLineEditableTextBox.h"
#include "Widgets/Input/SSpinBox.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Layout/SScrollBox.h"
#include "Widgets/Layout/SWrapBox.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/Text/STextBlock.h"

#define LOCTEXT_NAMESPACE "NGonMeshStudio"

namespace
{
struct FNGonMeshStudioUIState
{
    float ExtrudeDistance = 25.0f;
    float InsetRatio = 0.25f;
    float ExtrudeScale = 1.0f;
    float BevelWidth = 8.0f;
    int32 BevelSegments = 2;
    float BevelProfile = 0.5f;
    bool bBevelClampOverlap = true;
    float SmoothStrength = 0.5f;
    float TaperAmount = 0.35f;
    float TwistAngleDeg = 45.0f;
    float NoiseAmplitude = 6.0f;

    FString JsonInput = TEXT(
        "[\n"
        "  {\"op\": \"create_box\", \"width\": 100, \"depth\": 100, \"height\": 80},\n"
        "  {\"op\": \"select_faces_by_normal\", \"direction\": [0, 0, 1], \"max_angle_deg\": 20},\n"
        "  {\"op\": \"inset_faces\", \"ratio\": 0.22},\n"
        "  {\"op\": \"extrude_faces\", \"distance\": 45, \"scale\": 0.78},\n"
        "  {\"op\": \"bevel_faces\", \"ratio\": 0.18, \"height\": 10}\n"
        "]");
    FString LastOutput = TEXT("Ready. Use Mesh Studio tools or run a JSON command batch.");
    bool bLastOk = true;
};

TMap<const SNGonPreview*, FNGonMeshStudioUIState> GMeshStudioStateMap;

FNGonMeshStudioUIState& GetStudioState(const SNGonPreview* Owner)
{
    return GMeshStudioStateMap.FindOrAdd(Owner);
}

TSharedRef<SWidget> MakeSectionCard(
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
            .Padding(FMargin(8.0f, 6.0f, 8.0f, 7.0f))
            [
                SNew(SVerticalBox)
                + SVerticalBox::Slot()
                .AutoHeight()
                .Padding(0.0f, 0.0f, 0.0f, 5.0f)
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

TSharedRef<SWidget> MakeActionChip(
    const FText& Label,
    const FText& Tooltip,
    FOnClicked OnClicked,
    TAttribute<bool> IsEnabled = true)
{
    return SNew(SButton)
        .ContentPadding(FMargin(7.0f, 3.0f))
        .ToolTipText(Tooltip)
        .IsEnabled(IsEnabled)
        .OnClicked(OnClicked)
        [
            SNew(STextBlock)
            .Text(Label)
            .Font(FCoreStyle::GetDefaultFontStyle("Bold", 8))
        ];
}

TSharedRef<SWidget> MakeCompactParam(
    const FText& Label,
    float MinVal,
    float MaxVal,
    TAttribute<float> ValueAttr,
    FOnFloatValueChanged OnChanged)
{
    return SNew(SHorizontalBox)
        + SHorizontalBox::Slot()
        .AutoWidth()
        .VAlign(VAlign_Center)
        .Padding(0.0f, 0.0f, 4.0f, 0.0f)
        [
            SNew(STextBlock)
            .Text(Label)
            .Font(FCoreStyle::GetDefaultFontStyle("Regular", 8))
            .ColorAndOpacity(FLinearColor(0.72f, 0.76f, 0.84f, 1.0f))
        ]
        + SHorizontalBox::Slot()
        .AutoWidth()
        .VAlign(VAlign_Center)
        [
            SNew(SBox)
            .WidthOverride(58.0f)
            [
                SNew(SSpinBox<float>)
                .MinValue(MinVal)
                .MaxValue(MaxVal)
                .Delta(0.05f)
                .Font(FCoreStyle::GetDefaultFontStyle("Regular", 8))
                .Value(ValueAttr)
                .OnValueChanged(OnChanged)
            ]
        ];
}
} // anonymous namespace

TSharedRef<SWidget> SNGonPreview::MakeMeshStudioTab()
{
    auto RunOp = [this](const FNGonMeshOpResult& Result, bool bFrameCamera = false) -> FReply
    {
        FNGonMeshStudioUIState& State = GetStudioState(this);
        State.bLastOk = Result.bOk;
        State.LastOutput = Result.bOk
            ? Result.Message
            : FString::Printf(TEXT("Error (%s): %s"), *Result.Operation, *Result.Message);
        if (Viewport.IsValid())
        {
            if (bFrameCamera && Result.bOk)
            {
                Viewport->FrameMesh();
            }
            Viewport->Invalidate();
        }
        return FReply::Handled();
    };

    return SNew(SScrollBox)
        + SScrollBox::Slot()
        .Padding(0.0f, 0.0f, 0.0f, 6.0f)
        [
            // 1. History & Procedural Primitives
            MakeSectionCard(
                LOCTEXT("HistPrimTitle", "HISTORY & PRIMITIVES"),
                FLinearColor(0.24f, 0.72f, 1.0f, 1.0f),
                SNew(SVerticalBox)
                + SVerticalBox::Slot()
                .AutoHeight()
                .Padding(0.0f, 0.0f, 0.0f, 5.0f)
                [
                    SNew(SWrapBox)
                    .UseAllottedSize(true)
                    .InnerSlotPadding(FVector2D(4.0f, 4.0f))
                    + SWrapBox::Slot()
                    [
                        MakeActionChip(
                            LOCTEXT("UndoBtn", "Undo"),
                            LOCTEXT("UndoTip", "Undo last Mesh API edit"),
                            FOnClicked::CreateLambda([this, RunOp]() { return RunOp(FNGonMeshAPI::Undo(Viewport->Preview())); }),
                            TAttribute<bool>::CreateLambda([this]() { return Viewport.IsValid() && FNGonMeshAPI::CanUndo(&Viewport->Preview()); }))
                    ]
                    + SWrapBox::Slot()
                    [
                        MakeActionChip(
                            LOCTEXT("RedoBtn", "Redo"),
                            LOCTEXT("RedoTip", "Redo last undone Mesh API edit"),
                            FOnClicked::CreateLambda([this, RunOp]() { return RunOp(FNGonMeshAPI::Redo(Viewport->Preview())); }),
                            TAttribute<bool>::CreateLambda([this]() { return Viewport.IsValid() && FNGonMeshAPI::CanRedo(&Viewport->Preview()); }))
                    ]
                    + SWrapBox::Slot()
                    [
                        MakeActionChip(
                            LOCTEXT("ValidateBtn", "Validate Manifold"),
                            LOCTEXT("ValidateTip", "Run oriented manifold, duplicate face, and NaN validation"),
                            FOnClicked::CreateLambda([this]()
                            {
                                const FNGonValidationReport Rep = FNGonMeshAPI::CheckManifold(Viewport->Preview(), true);
                                FNGonMeshStudioUIState& State = GetStudioState(this);
                                State.bLastOk = Rep.IsOk();
                                State.LastOutput = Rep.IsOk()
                                    ? TEXT("Manifold validation passed: 0 issues.")
                                    : FString::Printf(TEXT("Manifold validation found %d issue(s): %s"),
                                        Rep.Issues.Num(), *Rep.Issues[0].Message);
                                return FReply::Handled();
                            }))
                    ]
                ]
                + SVerticalBox::Slot()
                .AutoHeight()
                [
                    SNew(SWrapBox)
                    .UseAllottedSize(true)
                    .InnerSlotPadding(FVector2D(4.0f, 4.0f))
                    + SWrapBox::Slot()
                    [
                        MakeActionChip(
                            LOCTEXT("BoxBtn", "+ Box"),
                            LOCTEXT("BoxTip", "Create a procedural 100x100x100 Box mesh"),
                            FOnClicked::CreateLambda([this, RunOp]() { return RunOp(FNGonMeshAPI::CreateBox(Viewport->Preview()), true); }))
                    ]
                    + SWrapBox::Slot()
                    [
                        MakeActionChip(
                            LOCTEXT("PlaneBtn", "+ Plane"),
                            LOCTEXT("PlaneTip", "Create a subdivided 4x4 Plane mesh"),
                            FOnClicked::CreateLambda([this, RunOp]() { return RunOp(FNGonMeshAPI::CreatePlane(Viewport->Preview()), true); }))
                    ]
                    + SWrapBox::Slot()
                    [
                        MakeActionChip(
                            LOCTEXT("CylBtn", "+ Cylinder"),
                            LOCTEXT("CylTip", "Create a 16-sided N-Gon Cylinder mesh"),
                            FOnClicked::CreateLambda([this, RunOp]() { return RunOp(FNGonMeshAPI::CreateCylinder(Viewport->Preview()), true); }))
                    ]
                    + SWrapBox::Slot()
                    [
                        MakeActionChip(
                            LOCTEXT("SphereBtn", "+ Sphere"),
                            LOCTEXT("SphereTip", "Create a UV Sphere mesh"),
                            FOnClicked::CreateLambda([this, RunOp]() { return RunOp(FNGonMeshAPI::CreateSphere(Viewport->Preview()), true); }))
                    ]
                ])
        ]
        + SScrollBox::Slot()
        .Padding(0.0f, 0.0f, 0.0f, 6.0f)
        [
            // 2. Face Modeling
            MakeSectionCard(
                LOCTEXT("FaceModelTitle", "FACE MODELING"),
                FLinearColor(0.98f, 0.68f, 0.22f, 1.0f),
                SNew(SVerticalBox)
                + SVerticalBox::Slot()
                .AutoHeight()
                .Padding(0.0f, 0.0f, 0.0f, 6.0f)
                [
                    SNew(SWrapBox)
                    .UseAllottedSize(true)
                    .InnerSlotPadding(FVector2D(8.0f, 4.0f))
                    + SWrapBox::Slot()
                    [
                        MakeCompactParam(
                            LOCTEXT("DistLbl", "Dist"),
                            -500.0f, 500.0f,
                            TAttribute<float>::CreateLambda([this]() { return GetStudioState(this).ExtrudeDistance; }),
                            FOnFloatValueChanged::CreateLambda([this](float V) { GetStudioState(this).ExtrudeDistance = V; }))
                    ]
                    + SWrapBox::Slot()
                    [
                        MakeCompactParam(
                            LOCTEXT("InsetLbl", "Inset"),
                            0.05f, 0.90f,
                            TAttribute<float>::CreateLambda([this]() { return GetStudioState(this).InsetRatio; }),
                            FOnFloatValueChanged::CreateLambda([this](float V) { GetStudioState(this).InsetRatio = V; }))
                    ]
                    + SWrapBox::Slot()
                    [
                        MakeCompactParam(
                            LOCTEXT("ScaleLbl", "Scale"),
                            0.10f, 4.0f,
                            TAttribute<float>::CreateLambda([this]() { return GetStudioState(this).ExtrudeScale; }),
                            FOnFloatValueChanged::CreateLambda([this](float V) { GetStudioState(this).ExtrudeScale = V; }))
                    ]
                    + SWrapBox::Slot()
                    [
                        MakeCompactParam(
                            LOCTEXT("BevelWidthLbl", "Bvl W"),
                            0.1f, 200.0f,
                            TAttribute<float>::CreateLambda([this]() { return GetStudioState(this).BevelWidth; }),
                            FOnFloatValueChanged::CreateLambda([this](float V) { GetStudioState(this).BevelWidth = V; }))
                    ]
                    + SWrapBox::Slot()
                    [
                        MakeCompactParam(
                            LOCTEXT("BevelSegLbl", "Segs"),
                            1.0f, 16.0f,
                            TAttribute<float>::CreateLambda([this]() { return static_cast<float>(GetStudioState(this).BevelSegments); }),
                            FOnFloatValueChanged::CreateLambda([this](float V) { GetStudioState(this).BevelSegments = FMath::Clamp(FMath::RoundToInt(V), 1, 16); }))
                    ]
                    + SWrapBox::Slot()
                    [
                        MakeCompactParam(
                            LOCTEXT("BevelProfLbl", "Prof"),
                            0.05f, 1.0f,
                            TAttribute<float>::CreateLambda([this]() { return GetStudioState(this).BevelProfile; }),
                            FOnFloatValueChanged::CreateLambda([this](float V) { GetStudioState(this).BevelProfile = V; }))
                    ]
                    + SWrapBox::Slot()
                    [
                        MakeActionChip(
                            LOCTEXT("BevelClampBtn", "Clamp"),
                            LOCTEXT("BevelClampTip", "Toggle Clamp Overlap safety for Bevel (prevents self-intersections)"),
                            FOnClicked::CreateLambda([this]()
                            {
                                FNGonMeshStudioUIState& S = GetStudioState(this);
                                S.bBevelClampOverlap = !S.bBevelClampOverlap;
                                S.LastOutput = S.bBevelClampOverlap ? TEXT("Bevel Clamp Overlap: ON") : TEXT("Bevel Clamp Overlap: OFF");
                                return FReply::Handled();
                            }))
                    ]
                ]
                + SVerticalBox::Slot()
                .AutoHeight()
                [
                    SNew(SWrapBox)
                    .UseAllottedSize(true)
                    .InnerSlotPadding(FVector2D(4.0f, 4.0f))
                    + SWrapBox::Slot()
                    [
                        MakeActionChip(
                            LOCTEXT("ExtrudeBtn", "Extrude"),
                            LOCTEXT("ExtrudeTip", "Extrude selected face(s) along normal by Dist and Scale"),
                            FOnClicked::CreateLambda([this, RunOp]()
                            {
                                const FNGonMeshStudioUIState& S = GetStudioState(this);
                                return RunOp(FNGonMeshAPI::ExtrudeFaces(Viewport->Preview(), {}, S.ExtrudeDistance, S.ExtrudeScale));
                            }))
                    ]
                    + SWrapBox::Slot()
                    [
                        MakeActionChip(
                            LOCTEXT("InsetBtn", "Inset"),
                            LOCTEXT("InsetTip", "Inset selected face(s) by Inset ratio"),
                            FOnClicked::CreateLambda([this, RunOp]()
                            {
                                const FNGonMeshStudioUIState& S = GetStudioState(this);
                                return RunOp(FNGonMeshAPI::InsetFaces(Viewport->Preview(), {}, S.InsetRatio, 0.0));
                            }))
                    ]
                    + SWrapBox::Slot()
                    [
                        MakeActionChip(
                            LOCTEXT("BevelBtn", "Bevel"),
                            LOCTEXT("BevelTip", "Smart Bevel: bevels selected Edges, Vertices, or Face Boundaries with Width, Segments, Profile & Clamp Overlap"),
                            FOnClicked::CreateLambda([this, RunOp]()
                            {
                                const FNGonMeshStudioUIState& S = GetStudioState(this);
                                FNGonBevelOptions Opts;
                                Opts.Width = S.BevelWidth;
                                Opts.Segments = S.BevelSegments;
                                Opts.Profile = S.BevelProfile;
                                Opts.bClampOverlap = S.bBevelClampOverlap;
                                return RunOp(FNGonMeshAPI::BevelSelection(Viewport->Preview(), Opts));
                            }))
                    ]
                    + SWrapBox::Slot()
                    [
                        MakeActionChip(
                            LOCTEXT("SubdivBtn", "Subdivide"),
                            LOCTEXT("SubdivTip", "Subdivide selected face(s) (or entire mesh if none selected)"),
                            FOnClicked::CreateLambda([this, RunOp]() { return RunOp(FNGonMeshAPI::SubdivideFaces(Viewport->Preview(), {})); }))
                    ]
                    + SWrapBox::Slot()
                    [
                        MakeActionChip(
                            LOCTEXT("TriangBtn", "Triangulate"),
                            LOCTEXT("TriangTip", "Triangulate selected N-Gon/Quad faces (or all faces)"),
                            FOnClicked::CreateLambda([this, RunOp]() { return RunOp(FNGonMeshAPI::TriangulateFaces(Viewport->Preview(), {})); }))
                    ]
                    + SWrapBox::Slot()
                    [
                        MakeActionChip(
                            LOCTEXT("FlipBtn", "Flip Normals"),
                            LOCTEXT("FlipTip", "Reverse winding on selected faces (or all faces)"),
                            FOnClicked::CreateLambda([this, RunOp]() { return RunOp(FNGonMeshAPI::FlipFaces(Viewport->Preview(), {})); }))
                    ]
                    + SWrapBox::Slot()
                    [
                        MakeActionChip(
                            LOCTEXT("DelFaceBtn", "Delete"),
                            LOCTEXT("DelFaceTip", "Delete selected faces"),
                            FOnClicked::CreateLambda([this, RunOp]() { return RunOp(FNGonMeshAPI::DeleteFaces(Viewport->Preview(), {})); }))
                    ]
                ])
        ]
        + SScrollBox::Slot()
        .Padding(0.0f, 0.0f, 0.0f, 6.0f)
        [
            // 3. Edge, Vertex & Selection
            MakeSectionCard(
                LOCTEXT("EdgeVertTitle", "EDGE, VERTEX & SELECTION"),
                FLinearColor(0.18f, 0.86f, 0.88f, 1.0f),
                SNew(SVerticalBox)
                + SVerticalBox::Slot()
                .AutoHeight()
                .Padding(0.0f, 0.0f, 0.0f, 4.0f)
                [
                    SNew(SWrapBox)
                    .UseAllottedSize(true)
                    .InnerSlotPadding(FVector2D(4.0f, 4.0f))
                    + SWrapBox::Slot()
                    [
                        MakeActionChip(
                            LOCTEXT("SplitEdgeBtn", "Split Edge"),
                            LOCTEXT("SplitEdgeTip", "Split selected edge at midpoint"),
                            FOnClicked::CreateLambda([this, RunOp]() { return RunOp(FNGonMeshAPI::SplitEdge(Viewport->Preview(), INDEX_NONE, 0.5)); }))
                    ]
                    + SWrapBox::Slot()
                    [
                        MakeActionChip(
                            LOCTEXT("CutFaceBtn", "Cut Face"),
                            LOCTEXT("CutFaceTip", "Cut a polygon between 2 selected non-adjacent vertices"),
                            FOnClicked::CreateLambda([this, RunOp]() { return RunOp(FNGonMeshAPI::CutFace(Viewport->Preview(), INDEX_NONE, INDEX_NONE, INDEX_NONE)); }))
                    ]
                    + SWrapBox::Slot()
                    [
                        MakeActionChip(
                            LOCTEXT("CollapseBtn", "Collapse"),
                            LOCTEXT("CollapseTip", "Collapse selected edge to midpoint"),
                            FOnClicked::CreateLambda([this, RunOp]() { return RunOp(FNGonMeshAPI::CollapseEdge(Viewport->Preview(), INDEX_NONE)); }))
                    ]
                    + SWrapBox::Slot()
                    [
                        MakeActionChip(
                            LOCTEXT("WeldBtn", "Weld"),
                            LOCTEXT("WeldTip", "Weld selected vertices to centroid"),
                            FOnClicked::CreateLambda([this, RunOp]() { return RunOp(FNGonMeshAPI::WeldVertices(Viewport->Preview(), {}, 0.0)); }))
                    ]
                    + SWrapBox::Slot()
                    [
                        MakeActionChip(
                            LOCTEXT("BridgeBtn", "Bridge"),
                            LOCTEXT("BridgeTip", "Bridge 2 selected edges with a quad"),
                            FOnClicked::CreateLambda([this, RunOp]() { return RunOp(FNGonMeshAPI::BridgeEdges(Viewport->Preview(), INDEX_NONE, INDEX_NONE)); }))
                    ]
                    + SWrapBox::Slot()
                    [
                        MakeActionChip(
                            LOCTEXT("FillHoleBtn", "Fill Hole"),
                            LOCTEXT("FillHoleTip", "Create an N-Gon face from 3+ selected vertices"),
                            FOnClicked::CreateLambda([this, RunOp]() { return RunOp(FNGonMeshAPI::FillHole(Viewport->Preview(), {})); }))
                    ]
                ]
                + SVerticalBox::Slot()
                .AutoHeight()
                [
                    SNew(SWrapBox)
                    .UseAllottedSize(true)
                    .InnerSlotPadding(FVector2D(4.0f, 4.0f))
                    + SWrapBox::Slot()
                    [
                        MakeActionChip(
                            LOCTEXT("GrowSelBtn", "Grow"),
                            LOCTEXT("GrowSelTip", "Grow current selection to adjacent elements"),
                            FOnClicked::CreateLambda([this, RunOp]() { return RunOp(FNGonMeshAPI::GrowSelection(Viewport->Preview())); }))
                    ]
                    + SWrapBox::Slot()
                    [
                        MakeActionChip(
                            LOCTEXT("ShrinkSelBtn", "Shrink"),
                            LOCTEXT("ShrinkSelTip", "Shrink current selection inward"),
                            FOnClicked::CreateLambda([this, RunOp]() { return RunOp(FNGonMeshAPI::ShrinkSelection(Viewport->Preview())); }))
                    ]
                    + SWrapBox::Slot()
                    [
                        MakeActionChip(
                            LOCTEXT("InvertSelBtn", "Invert"),
                            LOCTEXT("InvertSelTip", "Invert selection"),
                            FOnClicked::CreateLambda([this, RunOp]() { return RunOp(FNGonMeshAPI::InvertSelection(Viewport->Preview())); }))
                    ]
                    + SWrapBox::Slot()
                    [
                        MakeActionChip(
                            LOCTEXT("SelTopBtn", "Top (+Z)"),
                            LOCTEXT("SelTopTip", "Select all faces pointing upward (+Z within 30 deg)"),
                            FOnClicked::CreateLambda([this, RunOp]()
                            {
                                SetSelectionMode(ENGonElement::Face);
                                return RunOp(FNGonMeshAPI::SelectFacesByNormal(Viewport->Preview(), FVector::UpVector, 30.0));
                            }))
                    ]
                    + SWrapBox::Slot()
                    [
                        MakeActionChip(
                            LOCTEXT("SelBoundBtn", "Boundary"),
                            LOCTEXT("SelBoundTip", "Select open boundary edges"),
                            FOnClicked::CreateLambda([this, RunOp]()
                            {
                                SetSelectionMode(ENGonElement::Edge);
                                return RunOp(FNGonMeshAPI::SelectBoundaryEdges(Viewport->Preview()));
                            }))
                    ]
                    + SWrapBox::Slot()
                    [
                        MakeActionChip(
                            LOCTEXT("SelNGonBtn", "N-Gons"),
                            LOCTEXT("SelNGonTip", "Select all faces with 5 or more sides"),
                            FOnClicked::CreateLambda([this, RunOp]()
                            {
                                SetSelectionMode(ENGonElement::Face);
                                return RunOp(FNGonMeshAPI::SelectNGons(Viewport->Preview(), 5));
                            }))
                    ]
                    + SWrapBox::Slot()
                    [
                        MakeActionChip(
                            LOCTEXT("VertexLoopBtn", "Vertex Loop"),
                            LOCTEXT("VertexLoopTip", "Trace quad or boundary vertex loop from 2 adjacent vertices (or Click 1st + Double-Click 2nd)"),
                            FOnClicked::CreateLambda([this, RunOp]()
                            {
                                return RunOp(FNGonMeshAPI::SelectVertexLoop(Viewport->Preview()));
                            }))
                    ]
                    + SWrapBox::Slot()
                    [
                        MakeActionChip(
                            LOCTEXT("ShortestPathBtn", "Shortest Path"),
                            LOCTEXT("ShortestPathTip", "Select 3D Dijkstra shortest path between 2 vertices, edges, or faces (or Click 1st + Double-Click 2nd)"),
                            FOnClicked::CreateLambda([this, RunOp]()
                            {
                                return RunOp(FNGonMeshAPI::SelectShortestPath(Viewport->Preview()));
                            }))
                    ]
                    + SWrapBox::Slot()
                    [
                        MakeActionChip(
                            LOCTEXT("EdgeLoopBtn", "Edge Loop"),
                            LOCTEXT("EdgeLoopTip", "Trace continuous edge loop from selected edge"),
                            FOnClicked::CreateLambda([this, RunOp]()
                            {
                                SetSelectionMode(ENGonElement::Edge);
                                return RunOp(FNGonMeshAPI::SelectEdgeLoop(Viewport->Preview()));
                            }))
                    ]
                    + SWrapBox::Slot()
                    [
                        MakeActionChip(
                            LOCTEXT("EdgeRingBtn", "Edge Ring"),
                            LOCTEXT("EdgeRingTip", "Trace parallel quad-strip edge ring from selected edge"),
                            FOnClicked::CreateLambda([this, RunOp]()
                            {
                                SetSelectionMode(ENGonElement::Edge);
                                return RunOp(FNGonMeshAPI::SelectEdgeRing(Viewport->Preview()));
                            }))
                    ]
                    + SWrapBox::Slot()
                    [
                        MakeActionChip(
                            LOCTEXT("FaceLoopBtn", "Face Loop"),
                            LOCTEXT("FaceLoopTip", "Trace quad-strip face loop from selected edge or face"),
                            FOnClicked::CreateLambda([this, RunOp]()
                            {
                                SetSelectionMode(ENGonElement::Face);
                                return RunOp(FNGonMeshAPI::SelectFaceLoop(Viewport->Preview()));
                            }))
                    ]
                    + SWrapBox::Slot()
                    [
                        MakeActionChip(
                            LOCTEXT("LinkedShellBtn", "Linked Shell"),
                            LOCTEXT("LinkedShellTip", "Select entire connected surface shell from current selection"),
                            FOnClicked::CreateLambda([this, RunOp]() { return RunOp(FNGonMeshAPI::SelectLinkedShell(Viewport->Preview())); }))
                    ]
                    + SWrapBox::Slot()
                    [
                        MakeActionChip(
                            LOCTEXT("ToVertBtn", "-> Vtx"),
                            LOCTEXT("ToVertTip", "Convert current selection to Vertices"),
                            FOnClicked::CreateLambda([this, RunOp]()
                            {
                                const FNGonMeshOpResult R = FNGonMeshAPI::ConvertSelection(Viewport->Preview(), ENGonElement::Vertex);
                                SelectionMode = ENGonElement::Vertex;
                                return RunOp(R);
                            }))
                    ]
                    + SWrapBox::Slot()
                    [
                        MakeActionChip(
                            LOCTEXT("ToEdgeBtn", "-> Edge"),
                            LOCTEXT("ToEdgeTip", "Convert current selection to Edges"),
                            FOnClicked::CreateLambda([this, RunOp]()
                            {
                                const FNGonMeshOpResult R = FNGonMeshAPI::ConvertSelection(Viewport->Preview(), ENGonElement::Edge);
                                SelectionMode = ENGonElement::Edge;
                                return RunOp(R);
                            }))
                    ]
                    + SWrapBox::Slot()
                    [
                        MakeActionChip(
                            LOCTEXT("ToFaceBtn", "-> Face"),
                            LOCTEXT("ToFaceTip", "Convert current selection to Faces"),
                            FOnClicked::CreateLambda([this, RunOp]()
                            {
                                const FNGonMeshOpResult R = FNGonMeshAPI::ConvertSelection(Viewport->Preview(), ENGonElement::Face);
                                SelectionMode = ENGonElement::Face;
                                return RunOp(R);
                            }))
                    ]
                ])
        ]
        + SScrollBox::Slot()
        .Padding(0.0f, 0.0f, 0.0f, 6.0f)
        [
            // 4. Deformers & Symmetry
            MakeSectionCard(
                LOCTEXT("DeformSymTitle", "DEFORM & SYMMETRY"),
                FLinearColor(0.68f, 0.48f, 0.98f, 1.0f),
                SNew(SWrapBox)
                .UseAllottedSize(true)
                .InnerSlotPadding(FVector2D(4.0f, 4.0f))
                + SWrapBox::Slot()
                [
                    MakeActionChip(
                        LOCTEXT("SmoothBtn", "Smooth"),
                        LOCTEXT("SmoothTip", "Laplacian relax selected vertices (or whole mesh)"),
                        FOnClicked::CreateLambda([this, RunOp]()
                        {
                            return RunOp(FNGonMeshAPI::SmoothVertices(Viewport->Preview(), {}, 2, GetStudioState(this).SmoothStrength));
                        }))
                ]
                + SWrapBox::Slot()
                [
                    MakeActionChip(
                        LOCTEXT("TaperBtn", "Taper Z"),
                        LOCTEXT("TaperTip", "Taper mesh along Z axis"),
                        FOnClicked::CreateLambda([this, RunOp]()
                        {
                            return RunOp(FNGonMeshAPI::ApplyTaper(Viewport->Preview(), EAxis::Z, GetStudioState(this).TaperAmount));
                        }))
                ]
                + SWrapBox::Slot()
                [
                    MakeActionChip(
                        LOCTEXT("TwistBtn", "Twist Z"),
                        LOCTEXT("TwistTip", "Twist mesh 45 degrees along Z axis"),
                        FOnClicked::CreateLambda([this, RunOp]()
                        {
                            return RunOp(FNGonMeshAPI::ApplyTwist(Viewport->Preview(), EAxis::Z, GetStudioState(this).TwistAngleDeg));
                        }))
                ]
                + SWrapBox::Slot()
                [
                    MakeActionChip(
                        LOCTEXT("NoiseBtn", "Noise"),
                        LOCTEXT("NoiseTip", "Displace vertices deterministically along normals"),
                        FOnClicked::CreateLambda([this, RunOp]()
                        {
                            return RunOp(FNGonMeshAPI::ApplyNoise(Viewport->Preview(), {}, GetStudioState(this).NoiseAmplitude, 0.05, 1337));
                        }))
                ]
                + SWrapBox::Slot()
                [
                    MakeActionChip(
                        LOCTEXT("MirrorBtn", "Mirror X"),
                        LOCTEXT("MirrorTip", "Duplicate and mirror mesh across the X plane"),
                        FOnClicked::CreateLambda([this, RunOp]() { return RunOp(FNGonMeshAPI::MirrorMesh(Viewport->Preview(), EAxis::X, true)); }))
                ])
        ]
        + SScrollBox::Slot()
        [
            // 5. Live Health Strip
            SNew(SBorder)
            .BorderImage(FCoreStyle::Get().GetBrush("WhiteBrush"))
            .BorderBackgroundColor(FLinearColor(0.038f, 0.044f, 0.055f, 1.0f))
            .Padding(FMargin(8.0f, 5.0f))
            [
                SNew(STextBlock)
                .AutoWrapText(true)
                .Font(FCoreStyle::GetDefaultFontStyle("Regular", 8))
                .ColorAndOpacity_Lambda([this]()
                {
                    return GetStudioState(this).bLastOk
                        ? FLinearColor(0.55f, 0.90f, 0.68f, 1.0f)
                        : FLinearColor(1.0f, 0.45f, 0.42f, 1.0f);
                })
                .Text_Lambda([this]()
                {
                    const FNGonMeshStudioUIState& State = GetStudioState(this);
                    return FText::FromString(State.LastOutput);
                })
            ]
        ];
}

TSharedRef<SWidget> SNGonPreview::MakeAIConsoleTab()
{
    auto ApplyPreset = [this](const FString& Script) -> FReply
    {
        GetStudioState(this).JsonInput = Script;
        return FReply::Handled();
    };

    return SNew(SVerticalBox)
        + SVerticalBox::Slot()
        .AutoHeight()
        .Padding(0.0f, 0.0f, 0.0f, 6.0f)
        [
            MakeSectionCard(
                LOCTEXT("AIPresetsTitle", "AI MESH API — PRESETS & SCHEMA"),
                FLinearColor(0.28f, 0.85f, 0.62f, 1.0f),
                SNew(SWrapBox)
                .UseAllottedSize(true)
                .InnerSlotPadding(FVector2D(4.0f, 4.0f))
                + SWrapBox::Slot()
                [
                    MakeActionChip(
                        LOCTEXT("PresetPillar", "Pillar Macro"),
                        LOCTEXT("PresetPillarTip", "Load multi-step JSON macro: Box -> Select Top -> Inset -> Extrude -> Bevel"),
                        FOnClicked::CreateLambda([ApplyPreset]()
                        {
                            return ApplyPreset(TEXT(
                                "[\n"
                                "  {\"op\": \"create_box\", \"width\": 100, \"depth\": 100, \"height\": 80},\n"
                                "  {\"op\": \"select_faces_by_normal\", \"direction\": [0, 0, 1], \"max_angle_deg\": 20},\n"
                                "  {\"op\": \"inset_faces\", \"ratio\": 0.22},\n"
                                "  {\"op\": \"extrude_faces\", \"distance\": 55, \"scale\": 0.78},\n"
                                "  {\"op\": \"bevel_faces\", \"ratio\": 0.18, \"height\": 10}\n"
                                "]"));
                        }))
                ]
                + SWrapBox::Slot()
                [
                    MakeActionChip(
                        LOCTEXT("PresetTower", "Twisted Tower"),
                        LOCTEXT("PresetTowerTip", "Load multi-step JSON macro: Cylinder -> Twist 60 deg -> Taper"),
                        FOnClicked::CreateLambda([ApplyPreset]()
                        {
                            return ApplyPreset(TEXT(
                                "[\n"
                                "  {\"op\": \"create_cylinder\", \"radius\": 45, \"height\": 160, \"sides\": 8, \"height_segments\": 6},\n"
                                "  {\"op\": \"apply_twist\", \"axis\": \"Z\", \"angle_deg\": 65},\n"
                                "  {\"op\": \"apply_taper\", \"axis\": \"Z\", \"amount\": 0.42}\n"
                                "]"));
                        }))
                ]
                + SWrapBox::Slot()
                [
                    MakeActionChip(
                        LOCTEXT("InspectStateBtn", "Inspect State JSON"),
                        LOCTEXT("InspectStateTip", "Dump live mesh topology, bounds, shells, and selection to output & clipboard"),
                        FOnClicked::CreateLambda([this]()
                        {
                            const FString Summary = FNGonMeshAPI::ExportMeshSummaryJson(Viewport->Preview(), false);
                            FNGonMeshStudioUIState& State = GetStudioState(this);
                            State.bLastOk = true;
                            State.LastOutput = Summary;
                            FPlatformApplicationMisc::ClipboardCopy(*Summary);
                            return FReply::Handled();
                        }))
                ]
                + SWrapBox::Slot()
                [
                    MakeActionChip(
                        LOCTEXT("CopySchemaBtn", "Copy AI Schema"),
                        LOCTEXT("CopySchemaTip", "Copy complete NGonMeshAPI JSON schema to clipboard for AI agents"),
                        FOnClicked::CreateLambda([this]()
                        {
                            const FString Schema = FNGonMeshAPI::GetApiSchemaJson();
                            FPlatformApplicationMisc::ClipboardCopy(*Schema);
                            FNGonMeshStudioUIState& State = GetStudioState(this);
                            State.bLastOk = true;
                            State.LastOutput = TEXT("Copied full NGonMeshAPI JSON schema to clipboard.");
                            return FReply::Handled();
                        }))
                ])
        ]
        + SVerticalBox::Slot()
        .AutoHeight()
        .Padding(0.0f, 0.0f, 0.0f, 6.0f)
        [
            SNew(SBox)
            .HeightOverride(135.0f)
            [
                SNew(SMultiLineEditableTextBox)
                .Font(FCoreStyle::GetDefaultFontStyle("Mono", 8))
                .Text_Lambda([this]() { return FText::FromString(GetStudioState(this).JsonInput); })
                .OnTextChanged_Lambda([this](const FText& NewText)
                {
                    GetStudioState(this).JsonInput = NewText.ToString();
                })
            ]
        ]
        + SVerticalBox::Slot()
        .AutoHeight()
        .Padding(0.0f, 0.0f, 0.0f, 6.0f)
        [
            SNew(SHorizontalBox)
            + SHorizontalBox::Slot()
            .AutoWidth()
            .Padding(0.0f, 0.0f, 6.0f, 0.0f)
            [
                SNew(SButton)
                .ButtonColorAndOpacity(FLinearColor(0.18f, 0.65f, 0.45f, 1.0f))
                .ContentPadding(FMargin(12.0f, 4.0f))
                .OnClicked_Lambda([this]()
                {
                    FNGonMeshStudioUIState& State = GetStudioState(this);
                    const FNGonMeshOpResult Result = FNGonMeshAPI::ExecuteJsonCommand(Viewport->Preview(), State.JsonInput);
                    State.bLastOk = Result.bOk;
                    State.LastOutput = Result.PayloadJson.IsEmpty() ? Result.ToJsonString() : Result.PayloadJson;
                    if (Viewport.IsValid())
                    {
                        Viewport->Invalidate();
                    }
                    return FReply::Handled();
                })
                [
                    SNew(STextBlock)
                    .Text(LOCTEXT("RunJsonBtn", "Run JSON Command"))
                    .Font(FCoreStyle::GetDefaultFontStyle("Bold", 9))
                ]
            ]
            + SHorizontalBox::Slot()
            .AutoWidth()
            .Padding(0.0f, 0.0f, 6.0f, 0.0f)
            [
                MakeActionChip(
                    LOCTEXT("ConsoleUndoBtn", "Undo"),
                    LOCTEXT("ConsoleUndoTip", "Undo last mesh command"),
                    FOnClicked::CreateLambda([this]()
                    {
                        const FNGonMeshOpResult Res = FNGonMeshAPI::Undo(Viewport->Preview());
                        FNGonMeshStudioUIState& State = GetStudioState(this);
                        State.bLastOk = Res.bOk;
                        State.LastOutput = Res.ToJsonString();
                        Viewport->Invalidate();
                        return FReply::Handled();
                    }),
                    TAttribute<bool>::CreateLambda([this]() { return Viewport.IsValid() && FNGonMeshAPI::CanUndo(&Viewport->Preview()); }))
            ]
            + SHorizontalBox::Slot()
            .AutoWidth()
            [
                MakeActionChip(
                    LOCTEXT("ConsoleCopyOutBtn", "Copy Output"),
                    LOCTEXT("ConsoleCopyOutTip", "Copy latest JSON output to clipboard"),
                    FOnClicked::CreateLambda([this]()
                    {
                        FPlatformApplicationMisc::ClipboardCopy(*GetStudioState(this).LastOutput);
                        return FReply::Handled();
                    }))
            ]
        ]
        + SVerticalBox::Slot()
        .AutoHeight()
        [
            SNew(SBorder)
            .BorderImage(FCoreStyle::Get().GetBrush("WhiteBrush"))
            .BorderBackgroundColor(FLinearColor(0.032f, 0.036f, 0.046f, 1.0f))
            .Padding(FMargin(8.0f, 6.0f))
            [
                SNew(SBox)
                .MaxDesiredHeight(150.0f)
                [
                    SNew(SScrollBox)
                    + SScrollBox::Slot()
                    [
                        SNew(STextBlock)
                        .AutoWrapText(true)
                        .Font(FCoreStyle::GetDefaultFontStyle("Mono", 8))
                        .ColorAndOpacity_Lambda([this]()
                        {
                            return GetStudioState(this).bLastOk
                                ? FLinearColor(0.62f, 0.92f, 0.74f, 1.0f)
                                : FLinearColor(1.0f, 0.48f, 0.45f, 1.0f);
                        })
                        .Text_Lambda([this]() { return FText::FromString(GetStudioState(this).LastOutput); })
                    ]
                ]
            ]
        ];
}

#undef LOCTEXT_NAMESPACE
// NGon Unified ABI Sync v5.8.2
