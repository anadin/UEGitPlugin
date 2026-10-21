// Copyright (c) 2014-2020 Sebastien Rombauts (sebastien.rombauts@gmail.com)
//
// Distributed under the MIT License (MIT) (See accompanying file LICENSE.txt
// or copy at http://opensource.org/licenses/MIT)

#pragma once

#include "CoreMinimal.h"
#include "Modules/ModuleInterface.h"
#include "Provider/GitSourceControlProvider.h"

/** Registers the Git Workspace revision control provider and, in the editor, its status bar and menus. */
class FGitSourceControlModule final : public IModuleInterface
{
public:
	virtual void StartupModule() override;
	virtual void ShutdownModule() override;

	FGitSourceControlProvider& GetProvider() { return Provider; }
	const FGitSourceControlProvider& GetProvider() const { return Provider; }

	static FGitSourceControlModule& Get();

private:
	FGitSourceControlProvider Provider;
};
