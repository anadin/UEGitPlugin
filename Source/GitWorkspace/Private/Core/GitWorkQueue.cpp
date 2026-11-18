// Copyright UEGitPlugin contributors. Distributed under the MIT license.
#include "GitWorkQueue.h"

#include "HAL/Event.h"
#include "HAL/PlatformProcess.h"
#include "HAL/PlatformTLS.h"
#include "HAL/RunnableThread.h"

namespace GitCore
{
	FSerialWorkQueue::FSerialWorkQueue(const TCHAR* InName)
		: Name(InName)
	{
		WakeUp = FPlatformProcess::GetSynchEventFromPool(false);
		Thread = FRunnableThread::Create(this, *Name, 512 * 1024, TPri_BelowNormal);
	}

	FSerialWorkQueue::~FSerialWorkQueue()
	{
		Shutdown();
		if (WakeUp)
		{
			FPlatformProcess::ReturnSynchEventToPool(WakeUp);
			WakeUp = nullptr;
		}
	}

	void FSerialWorkQueue::Enqueue(TUniqueFunction<void()> Task)
	{
		if (bStopping || !Thread)
		{
			return;
		}
		Pending.Increment();
		Tasks.Enqueue(MoveTemp(Task));
		WakeUp->Trigger();
	}

	void FSerialWorkQueue::Flush()
	{
		if (IsWorkerThread())
		{
			return;
		}
		// Shared ownership keeps the event alive even if a late task triggers it after we stop waiting.
		const TSharedRef<FEventRef, ESPMode::ThreadSafe> Done = MakeShared<FEventRef, ESPMode::ThreadSafe>(EEventMode::ManualReset);
		Enqueue([Done]() { (*Done)->Trigger(); });
		while (!(*Done)->Wait(100))
		{
			if (bStopping || !Thread)
			{
				break;
			}
		}
	}

	void FSerialWorkQueue::Shutdown()
	{
		if (!Thread)
		{
			return;
		}
		bStopping = true;
		WakeUp->Trigger();
		Thread->WaitForCompletion();
		delete Thread;
		Thread = nullptr;
		TUniqueFunction<void()> Discarded;
		while (Tasks.Dequeue(Discarded))
		{
			Pending.Decrement();
		}
	}

	bool FSerialWorkQueue::IsWorkerThread() const
	{
		return ThreadId != 0 && FPlatformTLS::GetCurrentThreadId() == ThreadId;
	}

	uint32 FSerialWorkQueue::Run()
	{
		ThreadId = FPlatformTLS::GetCurrentThreadId();
		while (!bStopping)
		{
			TUniqueFunction<void()> Task;
			if (Tasks.Dequeue(Task))
			{
				Task();
				Pending.Decrement();
				continue;
			}
			WakeUp->Wait(250);
		}
		return 0;
	}

	void FSerialWorkQueue::Stop()
	{
		bStopping = true;
		if (WakeUp)
		{
			WakeUp->Trigger();
		}
	}
}
