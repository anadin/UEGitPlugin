// Copyright UEGitPlugin contributors. Distributed under the MIT license.
#pragma once

#include "CoreMinimal.h"
#include "Containers/Queue.h"
#include "HAL/Runnable.h"
#include "HAL/ThreadSafeBool.h"
#include "HAL/ThreadSafeCounter.h"

class FEvent;
class FRunnableThread;

namespace GitCore
{
	/** One worker thread that runs tasks in the order they were queued. */
	class FSerialWorkQueue final : public FRunnable
	{
	public:
		explicit FSerialWorkQueue(const TCHAR* InName);
		virtual ~FSerialWorkQueue() override;

		void Enqueue(TUniqueFunction<void()> Task);
		/** Blocks until every task queued so far has run. Runs inline when called on the worker itself. */
		void Flush();
		/** Stops after the running task; queued tasks are discarded. */
		void Shutdown();
		bool IsWorkerThread() const;
		bool IsBusy() const { return Pending.GetValue() > 0; }

	private:
		virtual uint32 Run() override;
		virtual void Stop() override;

		FString Name;
		TQueue<TUniqueFunction<void()>, EQueueMode::Mpsc> Tasks;
		FThreadSafeCounter Pending;
		FThreadSafeBool bStopping;
		FEvent* WakeUp = nullptr;
		FRunnableThread* Thread = nullptr;
		uint32 ThreadId = 0;
	};
}
