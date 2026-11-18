// Copyright (c) 2014-2020 Sebastien Rombauts (sebastien.rombauts@gmail.com)
//
// Distributed under the MIT License (MIT) (See accompanying file LICENSE.txt
// or copy at http://opensource.org/licenses/MIT)

#pragma once

#include "CoreMinimal.h"
#include "Modules/ModuleInterface.h"
#include "Provider/GitWorkspaceProvider.h"

/** Registers the Git Workspace revision control provider and, in the editor, its status bar and menus. */
class FGitWorkspaceModule final : public IModuleInterface
{
public:
	virtual void StartupModule() override;
	virtual void ShutdownModule() override;

	FGitWorkspaceProvider& GetProvider() { return Provider; }
	const FGitWorkspaceProvider& GetProvider() const { return Provider; }

	static FGitWorkspaceModule& Get();

private:
	FGitWorkspaceProvider Provider;
};
