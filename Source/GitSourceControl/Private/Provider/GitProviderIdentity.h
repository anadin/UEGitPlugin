// Copyright Git Workspace contributors. Distributed under the MIT license.
#pragma once
#include "CoreMinimal.h"

namespace GitWorkspaceProvider
{
const FName& Name();
bool ShouldMigrateLegacySelection(const FString& SavedProvider, const TCHAR* CommandLine);
// Call after registering the provider, so Unreal can select and persist the new identity.
void MigrateLegacySelection();
}
