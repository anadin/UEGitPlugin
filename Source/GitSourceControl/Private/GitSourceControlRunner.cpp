// Copyright Project Borealis

#include "GitSourceControlRunner.h"

#include "GitSourceControlModule.h"
#include "GitSourceControlProvider.h"
#include "GitSourceControlOperations.h"

#include "Async/Async.h"

namespace GitSourceControlRunnerConstants
{
	/** How long to wait between automatic background fetch + status refreshes.
	 *  TODO: expose this as a source control setting. */
	static constexpr float RefreshIntervalSeconds = 30.0f;
}

FGitSourceControlRunner::FGitSourceControlRunner()
{
	bRunThread = true;
	*bRefreshSpawned = false;
	StopEvent = FPlatformProcess::GetSynchEventFromPool(true);
	Thread = FRunnableThread::Create(this, TEXT("GitSourceControlRunner"));
}

FGitSourceControlRunner::~FGitSourceControlRunner()
{
	if (Thread)
	{
		// Kill(true) triggers Stop() and blocks until Run() has returned, so the worker
		// loop is guaranteed to be finished before we tear anything down.
		Thread->Kill(true);
		delete Thread;
		Thread = nullptr;
	}
	if (StopEvent)
	{
		FPlatformProcess::ReturnSynchEventToPool(StopEvent);
		StopEvent = nullptr;
	}
	// Any game-thread task still queued keeps the shared bRefreshSpawned flag alive on
	// its own; it never touches this runner, so it is safe for us to go away now.
}

bool FGitSourceControlRunner::Init()
{
	return true;
}

uint32 FGitSourceControlRunner::Run()
{
	while (bRunThread)
	{
		StopEvent->Wait(FTimespan::FromSeconds(GitSourceControlRunnerConstants::RefreshIntervalSeconds));
		if (!bRunThread)
		{
			break;
		}
		// If we're not running the task already
		if (bRefreshSpawned->exchange(true))
		{
			continue;
		}

		// The provider must be driven from the game thread. Dispatch and return
		// immediately: blocking this thread on a game thread task would deadlock
		// shutdown, since ~FGitSourceControlRunner() waits on this thread from the
		// game thread. The lambda captures only the shared flag, never `this`, so it
		// stays safe if the runner is destroyed before the task runs.
		TSharedRef<std::atomic<bool>, ESPMode::ThreadSafe> RefreshFlag = bRefreshSpawned;
		AsyncTask(ENamedThreads::GameThread, [RefreshFlag]
		{
			FGitSourceControlModule* GitSourceControl = FGitSourceControlModule::GetThreadSafe();
			// Module not loaded, bail. Usually happens when editor is shutting down, and this prevents a crash from bad timing.
			if (!GitSourceControl)
			{
				*RefreshFlag = false;
				return;
			}
			FGitSourceControlProvider& Provider = GitSourceControl->GetProvider();
			TSharedRef<FGitFetch, ESPMode::ThreadSafe> RefreshOperation = ISourceControlOperation::Create<FGitFetch>();
			RefreshOperation->bUpdateStatus = true;
			const FSourceControlOperationComplete CompleteDelegate = FSourceControlOperationComplete::CreateLambda(
				[RefreshFlag](const FSourceControlOperationRef&, ECommandResult::Type)
				{
					// The refresh finished (or failed): allow the next one to be scheduled.
					*RefreshFlag = false;
				});
#if ENGINE_MAJOR_VERSION >= 5
			const ECommandResult::Type Result = Provider.Execute(RefreshOperation, FSourceControlChangelistPtr(), FGitSourceControlModule::GetEmptyStringArray(), EConcurrency::Asynchronous, CompleteDelegate);
#else
			const ECommandResult::Type Result = Provider.Execute(RefreshOperation, FGitSourceControlModule::GetEmptyStringArray(), EConcurrency::Asynchronous, CompleteDelegate);
#endif
			// If the command was not queued, the completion delegate will never fire,
			// so clear the flag here to allow a retry on the next tick.
			if (Result != ECommandResult::Succeeded)
			{
				*RefreshFlag = false;
			}
		});
	}

	return 0;
}

void FGitSourceControlRunner::Stop()
{
	bRunThread = false;
	StopEvent->Trigger();
}
