#include "Framework/Application/SlateApplication.h"
#include "Framework/Commands/UIAction.h"
#include "Framework/Docking/TabManager.h"
#include "Modules/ModuleManager.h"
#include "SNGonPreview.h"
#include "ToolMenus.h"
#include "Widgets/Docking/SDockTab.h"

#define LOCTEXT_NAMESPACE "NGonModule"

namespace
{
const FName PreviewTabName(TEXT("NGonViewport"));
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
    }

    TSharedRef<SDockTab> SpawnTab(const FSpawnTabArgs&)
    {
        const TSharedRef<SDockTab> Tab = SNew(SDockTab).TabRole(ETabRole::NomadTab)
            [
                SNew(SNGonPreview)
            ];
        LiveTab = Tab;
        return Tab;
    }

    TWeakPtr<SDockTab> LiveTab;
};

IMPLEMENT_MODULE(FNGonModule, NGon)

#undef LOCTEXT_NAMESPACE
