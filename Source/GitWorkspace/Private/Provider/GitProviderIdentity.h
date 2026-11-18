// Copyright Git Workspace contributors. Distributed under the MIT license.
#pragma once
#include "CoreMinimal.h"
#include "Containers/ArrayView.h"

namespace GitWorkspaceProvider
{
const FName& Name();
bool ShouldMigrateLegacySelection(const FString& SavedProvider, const TCHAR* CommandLine);
// Call after registering the provider, so Unreal can select and persist the new identity.
void MigrateLegacySelection();
// Settings saved before the plugin was renamed from GitSourceControl to GitWorkspace: copies Keys
// from EarlierSection to Section in Ini, unless Section is there already, and leaves EarlierSection
// as it is. True when anything was copied; the caller flushes Ini.
bool CopyRenamedSection(const TCHAR* EarlierSection, const TCHAR* Section, TConstArrayView<FString> Keys, const FString& Ini);
}
