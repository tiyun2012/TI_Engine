#pragma once

#include "CoreMinimal.h"
#include "NGonPicking.h"
#include "SEditorViewport.h"
#include "Widgets/SCompoundWidget.h"

struct FAssetData;
class FNGonPreviewScene;
struct FNGonMeshData;

class SNGonViewport final : public SEditorViewport
{
public:
    SLATE_BEGIN_ARGS(SNGonViewport) {}
    SLATE_END_ARGS()

    void Construct(const FArguments& InArgs);
    void SetMesh(FNGonMeshData&& Data);
    void FrameMesh();
    void SetSelectionMode(ENGonElement Element);
    void Pick(const FSceneView& View, const FVector2D& Cursor, bool bAdd);
    FNGonPreviewScene& Preview() { return *PreviewScene; }

protected:
    TSharedRef<FEditorViewportClient> MakeEditorViewportClient() override;
    void OnFocusViewportToSelection() override;

private:
    TSharedPtr<FNGonPreviewScene> PreviewScene;
    ENGonElement SelectionMode = ENGonElement::Face;
};

class SNGonPreview final : public SCompoundWidget
{
public:
    SLATE_BEGIN_ARGS(SNGonPreview) {}
    SLATE_END_ARGS()

    void Construct(const FArguments& InArgs);

private:
    FReply LoadDemo();
    FReply ReloadDefault();
    FReply OpenFbx();
    bool TryLoadDefault(FText& Error);
    bool TryLoadFbx(const FString& Filename, FText& Error);
    void LoadStaticMesh(const FAssetData& Asset);
    void ShowMesh(FNGonMeshData&& Data, const FText& SourceName);
    void ShowError(const FText& Error);
    void SetSelectionMode(ENGonElement Element);

    TSharedRef<SWidget> MakeSelectionMode(ENGonElement Element, const FText& Label);
    TSharedRef<SWidget> MakeControls();
    TSharedRef<SWidget> MakeSliders();
    TSharedRef<SWidget> MakeStatus();

    TSharedPtr<SNGonViewport> Viewport;
    FString AssetPath;
    FText SourceLabel;
    FText ErrorText;
    ENGonElement SelectionMode = ENGonElement::Face;
    bool bShowHeatmap = false;
    float HeatBias = 0.0f;
    float Bend = 0.0f;
};
