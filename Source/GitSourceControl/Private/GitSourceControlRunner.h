// Copyright Project Borealis

#pragma once

#include "CoreMinimal.h"

#include "HAL/Runnable.h"
#include "Templates/SharedPointer.h"

#include "ISourceControlProvider.h"
#include "ISourceControlOperation.h"

#include <atomic>

/**
 * Background thread that periodically kicks off an asynchronous fetch + status refresh
 * so the editor keeps an up to date view of the remote without the user asking for it.
 */
class FGitSourceControlRunner : public FRunnable
{
public:
	FGitSourceControlRunner();

	// Destructor
	virtual ~FGitSourceControlRunner() override;

	bool Init() override;
	uint32 Run() override;
	void Stop() override;


private:
	FRunnableThread* Thread;
	FEvent* StopEvent;
	/** Set to false to ask the worker loop to exit. Read from both threads. */
	std::atomic<bool> bRunThread;
	/**
	 * True while a refresh operation is queued or in flight. Held by shared pointer so
	 * that game-thread continuations (the dispatch task and the completion delegate) can
	 * clear it safely even if they outlive this runner, which is destroyed from the game
	 * thread while those tasks may still be queued.
	 */
	TSharedRef<std::atomic<bool>, ESPMode::ThreadSafe> bRefreshSpawned = MakeShared<std::atomic<bool>, ESPMode::ThreadSafe>(false);
};
