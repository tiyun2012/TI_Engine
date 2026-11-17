#include "Modules/ModuleManager.h"
#include "Framework/Commands/UIAction.h"
#include "Framework/Docking/TabManager.h"
#include "STiPaintEditor.h"
#include "ToolMenus.h"
#include "Widgets/Docking/SDockTab.h"

#define LOCTEXT_NAMESPACE "TiPaintModule"
namespace { const FName PaintTabName(TEXT("TiPaintEditor")); }

class FTiPaintModule final : public IModuleInterface
{
public:
    void StartupModule() override
    {
        if (IsRunningCommandlet()) return;
        FGlobalTabmanager::Get()->RegisterNomadTabSpawner(PaintTabName, FOnSpawnTab::CreateRaw(this, &FTiPaintModule::SpawnTab))
            .SetDisplayName(LOCTEXT("PaintTab", "Ti Paint")).SetMenuType(ETabSpawnerMenuType::Hidden);
        UToolMenus::RegisterStartupCallback(FSimpleMulticastDelegate::FDelegate::CreateRaw(this, &FTiPaintModule::RegisterMenus));
    }
    void ShutdownModule() override
    {
        UToolMenus::UnRegisterStartupCallback(this); UToolMenus::UnregisterOwner(this);
        if (FSlateApplication::IsInitialized() && !IsRunningCommandlet()) FGlobalTabmanager::Get()->UnregisterNomadTabSpawner(PaintTabName);
    }
private:
    void RegisterMenus()
    {
        FToolMenuOwnerScoped Owner(this);
        UToolMenu* Menu = UToolMenus::Get()->ExtendMenu(TEXT("LevelEditor.MainMenu.Window"));
        FToolMenuSection& Section = Menu->FindOrAddSection(TEXT("TiTools"));
        Section.AddMenuEntry(TEXT("OpenTiPaint"), LOCTEXT("OpenPaint", "Ti Paint"), LOCTEXT("OpenPaintTip", "Open the TiGUI-style layered texture painter."), FSlateIcon(), FUIAction(FExecuteAction::CreateLambda([] { FGlobalTabmanager::Get()->TryInvokeTab(PaintTabName); })));
    }
    TSharedRef<SDockTab> SpawnTab(const FSpawnTabArgs&) { return SNew(SDockTab).TabRole(ETabRole::NomadTab)[SNew(STiPaintEditor)]; }
};

IMPLEMENT_MODULE(FTiPaintModule, TiPaint)
#undef LOCTEXT_NAMESPACE
