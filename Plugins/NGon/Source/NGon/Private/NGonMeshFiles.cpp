#include "NGonMeshFiles.h"

#include "HAL/FileManager.h"
#include "Interfaces/IPluginManager.h"
#include "Misc/Paths.h"

#define LOCTEXT_NAMESPACE "NGonMeshFiles"

FString NGon::GetMeshesDirectory()
{
    const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("NGon"));
    const FString PluginDirectory = Plugin.IsValid()
        ? Plugin->GetBaseDir()
        : FPaths::Combine(FPaths::ProjectPluginsDir(), TEXT("NGon"));
    return FPaths::ConvertRelativePathToFull(FPaths::Combine(PluginDirectory, TEXT("Meshes")));
}

bool NGon::FindDefaultFbx(const FString& Directory, FString& OutPath, FText& Error)
{
    const FString Preferred = FPaths::Combine(Directory, TEXT("Default.fbx"));
    if (IFileManager::Get().FileExists(*Preferred))
    {
        OutPath = Preferred;
        Error = FText::GetEmpty();
        return true;
    }

    TArray<FString> Files;
    IFileManager::Get().FindFiles(Files, *FPaths::Combine(Directory, TEXT("*")), true, false);
    Files.RemoveAll([](const FString& Filename)
    {
        return !FPaths::GetExtension(Filename).Equals(TEXT("fbx"), ESearchCase::IgnoreCase);
    });
    if (Files.Num() == 1)
    {
        OutPath = FPaths::Combine(Directory, Files[0]);
        Error = FText::GetEmpty();
        return true;
    }

    OutPath.Reset();
    Error = Files.IsEmpty()
        ? LOCTEXT("NoDefaultFbx", "Put an FBX in the plugin's Meshes folder to load it as the default.")
        : LOCTEXT("MultipleDefaultFbx", "Multiple FBXs in Meshes. Name the default Default.fbx or use Open FBX.");
    return false;
}

#undef LOCTEXT_NAMESPACE
// NGon Unified ABI Sync v5.8.2
