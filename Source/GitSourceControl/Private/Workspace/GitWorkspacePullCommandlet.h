// Copyright UEGitPlugin contributors. Distributed under the MIT license.
#pragma once
#include "Commandlets/Commandlet.h"
#include "GitWorkspacePullCommandlet.generated.h"

UCLASS()
class UGitWorkspacePullCommandlet : public UCommandlet
{
    GENERATED_BODY()
public:
    UGitWorkspacePullCommandlet();
    virtual int32 Main(const FString& Params) override;
};
