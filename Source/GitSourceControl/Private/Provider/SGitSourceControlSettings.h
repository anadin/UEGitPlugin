// Copyright (c) 2014-2020 Sebastien Rombauts (sebastien.rombauts@gmail.com)
//
// Distributed under the MIT License (MIT) (See accompanying file LICENSE.txt
// or copy at http://opensource.org/licenses/MIT)

#pragma once

#include "CoreMinimal.h"
#include "Widgets/DeclarativeSyntaxSupport.h"
#include "Widgets/SCompoundWidget.h"

/** Settings shown in Unreal's "Connect to Revision Control" dialog. */
class SGitSourceControlSettings : public SCompoundWidget
{
public:
	SLATE_BEGIN_ARGS(SGitSourceControlSettings) {}
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs);

private:
	FText GetBinaryPathText() const;
	void OnBinaryPathCommitted(const FText& InText, ETextCommit::Type InCommitType);
	FText GetRepositoryText() const;
	ECheckBoxState IsUsingLocking() const;
	void OnLockingChanged(ECheckBoxState NewState);
	FText GetLockRemoteText() const;
	void OnLockRemoteCommitted(const FText& InText, ETextCommit::Type InCommitType);
};
