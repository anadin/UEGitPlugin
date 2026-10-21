// Copyright Git Workspace contributors. Distributed under the MIT license.
#include "GitProviderIdentity.h"
#include "ISourceControlModule.h"
#include "SourceControlHelpers.h"
#include "Misc/CommandLine.h"
#include "Misc/ConfigCacheIni.h"

namespace GitWorkspaceProvider
{
const FName& Name()
{
    static const FName ProviderName(TEXT("Git Workspace"));
    return ProviderName;
}

bool ShouldMigrateLegacySelection(const FString& SavedProvider, const TCHAR* CommandLine)
{
    // Match Unreal's SourceControlSettings command-line precedence, including quoted
    // values and the first SCCProvider switch winning when it appears more than once.
    TArray<FString> Tokens, Switches;
    FCommandLine::Parse(CommandLine, Tokens, Switches);
    for (const FString& Switch : Switches)
    {
        TArray<FString> Pair;
        if (Switch.ParseIntoArray(Pair, TEXT("="), true) == 2 && Pair[0] == TEXT("SCCProvider"))
            return Pair[1].TrimQuotes() == TEXT("Git LFS 2");
    }
    return SavedProvider == TEXT("Git LFS 2");
}

void MigrateLegacySelection()
{
    // GetSettingsIni respects Unreal's active project/global scope. Do not scan or
    // rewrite inactive projects, provider-specific settings, or unrelated providers.
    const FString Ini = SourceControlHelpers::GetSettingsIni();
    FString SavedProvider;
    GConfig->GetString(TEXT("SourceControl.SourceControlSettings"), TEXT("Provider"), SavedProvider, Ini);
    if (ShouldMigrateLegacySelection(SavedProvider, FCommandLine::Get()))
    {
        ISourceControlModule::Get().SetProvider(Name());
        GConfig->Flush(false, Ini);
        UE_LOG(LogSourceControl, Log, TEXT("Migrated Git LFS 2 provider selection to Git Workspace."));
    }
}
}
