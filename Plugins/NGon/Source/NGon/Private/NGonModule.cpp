#include "Framework/Application/SlateApplication.h"
#include "Framework/Commands/UIAction.h"
#include "Framework/Docking/TabManager.h"
#include "Modules/ModuleManager.h"
#include "SNGonPreview.h"
#include "NGonUVEditor.h"
#include "STiPaintEditor.h"
#include "ToolMenus.h"
#include "Widgets/Docking/SDockTab.h"

#define LOCTEXT_NAMESPACE "NGonModule"

namespace
{
const FName PreviewTabName(TEXT("NGonViewport"));
const FName UVEditorTabName(TEXT("NGonUVEditor"));
const FName PaintTabName(TEXT("TiPaintEditor"));
}

class FNGonModule final : public IModuleInterface
{
public:
    void StartupModule() override
    {
        if (IsRunningCommandlet())
        {
            return;
        }

        FGlobalTabmanager::Get()->RegisterNomadTabSpawner(
            PreviewTabName, FOnSpawnTab::CreateRaw(this, &FNGonModule::SpawnTab))
            .SetDisplayName(LOCTEXT("TabTitle", "N-Gon"))
            .SetMenuType(ETabSpawnerMenuType::Hidden);
        FGlobalTabmanager::Get()->RegisterNomadTabSpawner(
            UVEditorTabName, FOnSpawnTab::CreateRaw(this, &FNGonModule::SpawnUVTab))
            .SetDisplayName(LOCTEXT("UVTabTitle", "N-Gon UV Editor"))
            .SetMenuType(ETabSpawnerMenuType::Hidden);
        FGlobalTabmanager::Get()->RegisterNomadTabSpawner(
            PaintTabName, FOnSpawnTab::CreateRaw(this, &FNGonModule::SpawnPaintTab))
            .SetDisplayName(LOCTEXT("PaintTabTitle", "Ti Paint"))
            .SetMenuType(ETabSpawnerMenuType::Hidden);

        UToolMenus::RegisterStartupCallback(
            FSimpleMulticastDelegate::FDelegate::CreateRaw(this, &FNGonModule::RegisterMenus));
    }

    void ShutdownModule() override
    {
        UToolMenus::UnRegisterStartupCallback(this);
        UToolMenus::UnregisterOwner(this);
        if (FSlateApplication::IsInitialized() && !IsRunningCommandlet())
        {
            if (const TSharedPtr<SDockTab> Tab = LiveTab.Pin())
            {
                Tab->RequestCloseTab();
            }
            FGlobalTabmanager::Get()->UnregisterNomadTabSpawner(PreviewTabName);
            FGlobalTabmanager::Get()->UnregisterNomadTabSpawner(UVEditorTabName);
            FGlobalTabmanager::Get()->UnregisterNomadTabSpawner(PaintTabName);
        }
    }

    bool SupportsDynamicReloading() override { return false; }

private:
    void RegisterMenus()
    {
        FToolMenuOwnerScoped Owner(this);
        UToolMenu* Menu = UToolMenus::Get()->ExtendMenu(TEXT("LevelEditor.MainMenu.Window"));
        FToolMenuSection& Section = Menu->FindOrAddSection(TEXT("NGon"));
        Section.AddMenuEntry(TEXT("OpenNGon"),
            LOCTEXT("OpenPreview", "N-Gon"),
            LOCTEXT("OpenPreviewTooltip", "Inspect polygon edges, vertices and heatmaps."),
            FSlateIcon(),
            FUIAction(FExecuteAction::CreateLambda([]
            {
                FGlobalTabmanager::Get()->TryInvokeTab(PreviewTabName);
            })));
        Section.AddMenuEntry(TEXT("OpenNGonUV"),
            LOCTEXT("OpenUVEditor", "N-Gon UV Editor"),
            LOCTEXT("OpenUVEditorTooltip", "Edit and inspect the mesh UV layout with corner, edge, face and island tools."),
            FSlateIcon(),
            FUIAction(FExecuteAction::CreateLambda([]
            {
                FGlobalTabmanager::Get()->TryInvokeTab(UVEditorTabName);
            })));
    }

    TSharedRef<SDockTab> SpawnTab(const FSpawnTabArgs&)
    {
        const TSharedRef<SDockTab> Tab = SNew(SDockTab)
            .TabRole(ETabRole::NomadTab)
            .OnTabClosed_Lambda([](TSharedRef<SDockTab>)
            {
                if (FSlateApplication::IsInitialized())
                {
                    if (const TSharedPtr<SDockTab> UVTab = FGlobalTabmanager::Get()->FindExistingLiveTab(UVEditorTabName))
                    {
                        UVTab->RequestCloseTab();
                    }
                }
            })
            [
                SNew(SNGonPreview)
            ];
        LiveTab = Tab;
        return Tab;
    }

    TSharedRef<SDockTab> SpawnUVTab(const FSpawnTabArgs&)
    {
        return SNew(SDockTab).TabRole(ETabRole::NomadTab)
            [
                SNew(SNGonUVEditor)
            ];
    }

    TSharedRef<SDockTab> SpawnPaintTab(const FSpawnTabArgs&)
    {
        return SNew(SDockTab).TabRole(ETabRole::NomadTab)
            [
                SNew(STiPaintEditor)
            ];
    }

    TWeakPtr<SDockTab> LiveTab;
};

IMPLEMENT_MODULE(FNGonModule, NGon)

#undef LOCTEXT_NAMESPACE
// NGon Unified ABI Sync v5.8.2
