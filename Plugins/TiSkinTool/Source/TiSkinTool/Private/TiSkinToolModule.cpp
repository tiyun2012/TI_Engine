#include "Modules/ModuleManager.h"
#include "ToolMenus.h"
#include "ISkeletalMeshEditor.h"
#include "ISkeletalMeshEditorModule.h"
#include "Subsystems/AssetEditorSubsystem.h"
#include "Editor.h"
#include "EditorModeManager.h"
#include "Tools/EdModeInteractiveToolsContext.h"
#include "InteractiveToolManager.h"
#include "TiSkinWeightToolBuilder.h"

class FTiSkinToolModule : public IModuleInterface
{
public:
    virtual void StartupModule() override
    {
        UToolMenus::RegisterStartupCallback(FSimpleMulticastDelegate::FDelegate::CreateRaw(this, &FTiSkinToolModule::RegisterToolbarButton));
    }

    virtual void ShutdownModule() override
    {
        UToolMenus::UnRegisterStartupCallback(this);
    }

private:
    // Safely track which ToolManagers we have already hijacked
    TArray<TWeakObjectPtr<UInteractiveToolManager>> HijackedManagers;

    void RegisterToolbarButton()
    {
        UToolMenu* ToolbarMenu = UToolMenus::Get()->ExtendMenu("AssetEditor.SkeletalMeshEditor.ToolBar");
        
        FToolMenuSection& Section = ToolbarMenu->FindOrAddSection("PluginTools");
        Section.AddEntry(FToolMenuEntry::InitToolBarButton(
            "TiSkinToolActivate",
            FUIAction(FExecuteAction::CreateRaw(this, &FTiSkinToolModule::ActivateTiSkinTool)),
            FText::FromString("Ti Skin Debug"),
            FText::FromString("Activates the TiSkinWeightTool for debugging.")
        ));
    }

    void ActivateTiSkinTool()
    {
        UAssetEditorSubsystem* AssetEditorSubsystem = GEditor->GetEditorSubsystem<UAssetEditorSubsystem>();
        if (!AssetEditorSubsystem) return;

        TArray<UObject*> EditedAssets = AssetEditorSubsystem->GetAllEditedAssets();
        for (UObject* Asset : EditedAssets)
        {
            USkeletalMesh* SkelMesh = Cast<USkeletalMesh>(Asset);
            if (!SkelMesh) continue;

            IAssetEditorInstance* EditorInstance = AssetEditorSubsystem->FindEditorForAsset(SkelMesh, false);
            if (!EditorInstance) continue;

            ISkeletalMeshEditor* SkelMeshEditor = static_cast<ISkeletalMeshEditor*>(EditorInstance);
            if (!SkelMeshEditor) continue;

            FEditorModeTools& ModeTools = SkelMeshEditor->GetEditorModeManager();
            UModeManagerInteractiveToolsContext* ToolsContext = ModeTools.GetInteractiveToolsContext();
            
            if (!ToolsContext) continue;

            UInteractiveToolManager* ToolManager = ToolsContext->ToolManager;
            if (!ToolManager) continue;

            const FString EpicToolName = TEXT("SkinWeightsPaintTool");

            // 1. Check if we have already hijacked this specific editor's ToolManager
            bool bAlreadyHijacked = false;
            for (int32 i = HijackedManagers.Num() - 1; i >= 0; --i)
            {
                if (!HijackedManagers[i].IsValid())
                {
                    HijackedManagers.RemoveAt(i); // Clean up stale pointers to closed editor windows
                }
                else if (HijackedManagers[i].Get() == ToolManager)
                {
                    bAlreadyHijacked = true;
                }
            }

            // 2. Only perform the hijack if we haven't done it yet
            if (!bAlreadyHijacked)
            {
                // CRITICAL FIX: Unregister Epic's native tool FIRST to prevent the Ensure crash!
                ToolManager->UnregisterToolType(EpicToolName);

                // Now it is safe to register our custom builder into the empty slot
                ToolManager->RegisterToolType(EpicToolName, NewObject<UTiSkinWeightToolBuilder>(ToolManager));
                
                // Remember that we hijacked this manager so we don't do it again
                HijackedManagers.Add(ToolManager);

                UE_LOG(LogTemp, Display, TEXT("=================================================="));
                UE_LOG(LogTemp, Display, TEXT("SUCCESS: TiSkinWeightTool has hijacked the native tool!"));
                UE_LOG(LogTemp, Display, TEXT("Please click the 'Skin Weights' tab in the editor viewport to start."));
                UE_LOG(LogTemp, Display, TEXT("=================================================="));
            }
            else
            {
                UE_LOG(LogTemp, Display, TEXT("Native tool is already hijacked! Click 'Skin Weights' to use it."));
            }

            return;
        }
    }
};

IMPLEMENT_MODULE(FTiSkinToolModule, TiSkinTool)