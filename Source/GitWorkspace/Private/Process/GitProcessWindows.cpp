// Copyright UEGitPlugin contributors. Distributed under the MIT license.
#include "GitProcess.h"

#if PLATFORM_WINDOWS

#include "HAL/PlatformTime.h"
#include "HAL/Thread.h"

#include "Windows/AllowWindowsPlatformTypes.h"
#include <windows.h>
namespace GitCore
{
	namespace
	{
		// Built from TRUE and FALSE, which Unreal only defines between these two headers.
		constexpr DWORD_PTR HandleListAttribute = PROC_THREAD_ATTRIBUTE_HANDLE_LIST;
	}
}
#include "Windows/HideWindowsPlatformTypes.h"

// Windows maps API names to macros (GetUserName is GetUserNameW). In a unity build they would
// rename ISourceControlRevision::GetUserName and this module's MessageBox in later files.
#undef GetUserName
#undef MessageBox

namespace GitCore
{
	namespace
	{
		constexpr DWORD PipeBufferSize = 1024 * 1024;
		constexpr double PipeGraceSeconds = 0.25;

		struct FHandle
		{
			HANDLE Value = nullptr;

			FHandle() = default;
			explicit FHandle(HANDLE InValue) : Value(InValue) {}
			FHandle(const FHandle&) = delete;
			FHandle& operator=(const FHandle&) = delete;
			~FHandle() { Close(); }

			bool IsValid() const { return Value != nullptr && Value != INVALID_HANDLE_VALUE; }
			void Close()
			{
				if (IsValid())
				{
					::CloseHandle(Value);
				}
				Value = nullptr;
			}
		};

		bool CreateChildPipe(FHandle& ParentEnd, FHandle& ChildEnd, bool bChildReads)
		{
			SECURITY_ATTRIBUTES Security = {};
			Security.nLength = sizeof(Security);
			// TRUE is undefined again after HideWindowsPlatformTypes.h.
			Security.bInheritHandle = true;
			HANDLE ReadEnd = nullptr;
			HANDLE WriteEnd = nullptr;
			if (!::CreatePipe(&ReadEnd, &WriteEnd, &Security, PipeBufferSize))
			{
				return false;
			}
			ParentEnd.Value = bChildReads ? WriteEnd : ReadEnd;
			ChildEnd.Value = bChildReads ? ReadEnd : WriteEnd;
			return ::SetHandleInformation(ParentEnd.Value, HANDLE_FLAG_INHERIT, 0) != 0;
		}

		/** Reads what is already buffered without blocking. Sets bClosed at end of stream. */
		bool ReadAvailable(HANDLE Pipe, TArray<uint8>& Chunk, bool& bClosed)
		{
			Chunk.Reset();
			DWORD Available = 0;
			if (!::PeekNamedPipe(Pipe, nullptr, 0, nullptr, &Available, nullptr))
			{
				bClosed = true;
				return false;
			}
			if (Available == 0)
			{
				return false;
			}
			Chunk.SetNumUninitialized(static_cast<int32>(Available));
			DWORD Read = 0;
			if (!::ReadFile(Pipe, Chunk.GetData(), Available, &Read, nullptr))
			{
				bClosed = true;
				Chunk.Reset();
				return false;
			}
			Chunk.SetNum(static_cast<int32>(Read), EAllowShrinking::No);
			return Read > 0;
		}

		TArray<TCHAR> BuildEnvironmentBlock(const FProcessSpec& Spec)
		{
			TArray<FString> Inherited;
			if (LPWCH Strings = ::GetEnvironmentStringsW())
			{
				for (const WCHAR* Entry = Strings; *Entry; Entry += FCString::Strlen(Entry) + 1)
				{
					Inherited.Add(Entry);
				}
				::FreeEnvironmentStringsW(Strings);
			}
			TArray<FString> Entries = ProcessDetail::BuildEnvironment(Inherited, Spec);
			// Windows expects the block sorted case-insensitively by name.
			Entries.Sort([](const FString& A, const FString& B) { return A.Compare(B, ESearchCase::IgnoreCase) < 0; });
			TArray<TCHAR> Block;
			for (const FString& Entry : Entries)
			{
				Block.Append(*Entry, Entry.Len());
				Block.Add(TEXT('\0'));
			}
			Block.Add(TEXT('\0'));
			return Block;
		}
	}

	FProcessResult RunProcess(const FProcessSpec& Spec)
	{
		FProcessResult Result;
		const double Start = FPlatformTime::Seconds();

		FHandle StdinParent;
		FHandle StdinChild;
		FHandle StdoutParent;
		FHandle StdoutChild;
		FHandle StderrParent;
		FHandle StderrChild;
		if (!CreateChildPipe(StdinParent, StdinChild, true) || !CreateChildPipe(StdoutParent, StdoutChild, false) || !CreateChildPipe(StderrParent, StderrChild, false))
		{
			Result.Diagnostic = FString::Printf(TEXT("Could not create process pipes (error %u)."), ::GetLastError());
			return Result;
		}

		SIZE_T AttributeSize = 0;
		::InitializeProcThreadAttributeList(nullptr, 1, 0, &AttributeSize);
		TArray<uint8> AttributeStorage;
		AttributeStorage.SetNumZeroed(static_cast<int32>(AttributeSize));
		LPPROC_THREAD_ATTRIBUTE_LIST AttributeList = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(AttributeStorage.GetData());
		if (!::InitializeProcThreadAttributeList(AttributeList, 1, 0, &AttributeSize))
		{
			Result.Diagnostic = FString::Printf(TEXT("Could not prepare process attributes (error %u)."), ::GetLastError());
			return Result;
		}
		// Only the three pipe ends are inherited, never the editor's other handles.
		HANDLE Inherited[3] = {StdinChild.Value, StdoutChild.Value, StderrChild.Value};
		if (!::UpdateProcThreadAttribute(AttributeList, 0, HandleListAttribute, Inherited, sizeof(Inherited), nullptr, nullptr))
		{
			::DeleteProcThreadAttributeList(AttributeList);
			Result.Diagnostic = FString::Printf(TEXT("Could not restrict inherited handles (error %u)."), ::GetLastError());
			return Result;
		}

		STARTUPINFOEXW Startup = {};
		Startup.StartupInfo.cb = sizeof(Startup);
		Startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
		Startup.StartupInfo.hStdInput = StdinChild.Value;
		Startup.StartupInfo.hStdOutput = StdoutChild.Value;
		Startup.StartupInfo.hStdError = StderrChild.Value;
		Startup.lpAttributeList = AttributeList;

		FString CommandLine = ProcessDetail::QuoteWindowsArgument(Spec.Executable);
		for (const FString& Argument : Spec.Arguments)
		{
			CommandLine += TEXT(" ");
			CommandLine += ProcessDetail::QuoteWindowsArgument(Argument);
		}
		TArray<TCHAR> CommandBuffer;
		CommandBuffer.Append(*CommandLine, CommandLine.Len());
		CommandBuffer.Add(TEXT('\0'));
		TArray<TCHAR> Environment = BuildEnvironmentBlock(Spec);

		PROCESS_INFORMATION Information = {};
		const DWORD CreationFlags = CREATE_NO_WINDOW | CREATE_UNICODE_ENVIRONMENT | EXTENDED_STARTUPINFO_PRESENT | CREATE_SUSPENDED;
		const BOOL bCreated = ::CreateProcessW(nullptr, CommandBuffer.GetData(), nullptr, nullptr, true, CreationFlags, Environment.GetData(),
			Spec.WorkingDirectory.IsEmpty() ? nullptr : *Spec.WorkingDirectory, &Startup.StartupInfo, &Information);
		const DWORD CreateError = ::GetLastError();
		::DeleteProcThreadAttributeList(AttributeList);
		StdinChild.Close();
		StdoutChild.Close();
		StderrChild.Close();
		if (!bCreated)
		{
			Result.Diagnostic = FString::Printf(TEXT("Could not start %s (error %u)."), *Spec.Executable, CreateError);
			return Result;
		}
		FHandle Process(Information.hProcess);
		FHandle MainThread(Information.hThread);

		// A kill-on-close job stops the whole tree on cancel and if the editor dies.
		FHandle Job(::CreateJobObjectW(nullptr, nullptr));
		if (Job.IsValid())
		{
			JOBOBJECT_EXTENDED_LIMIT_INFORMATION Limits = {};
			Limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
			if (!::SetInformationJobObject(Job.Value, JobObjectExtendedLimitInformation, &Limits, sizeof(Limits)) || !::AssignProcessToJobObject(Job.Value, Process.Value))
			{
				Job.Close();
			}
		}
		::ResumeThread(MainThread.Value);

		// Stdin is written on its own thread so a child that stops reading cannot deadlock us.
		TUniquePtr<FThread> StdinWriter;
		if (Spec.Stdin.Num() > 0)
		{
			HANDLE WriteEnd = StdinParent.Value;
			StdinParent.Value = nullptr;
			StdinWriter = MakeUnique<FThread>(TEXT("GitProcessStdin"), [WriteEnd, &Spec]()
			{
				int64 Offset = 0;
				while (Offset < Spec.Stdin.Num())
				{
					DWORD Written = 0;
					const DWORD Chunk = static_cast<DWORD>(FMath::Min<int64>(Spec.Stdin.Num() - Offset, 64 * 1024));
					if (!::WriteFile(WriteEnd, Spec.Stdin.GetData() + Offset, Chunk, &Written, nullptr) || Written == 0)
					{
						break;
					}
					Offset += Written;
				}
				::CloseHandle(WriteEnd);
			});
		}
		else
		{
			StdinParent.Close();
		}

		bool bStdoutOpen = true;
		bool bStderrOpen = true;
		bool bExited = false;
		double ExitedAt = 0.0;
		double LastActivity = Start;
		int64 StdoutBytes = 0;
		bool bStopping = false;
		EProcessEnd StopReason = EProcessEnd::Cancelled;
		TArray<uint8> Chunk;

		auto RequestStop = [&](EProcessEnd Reason, const FString& Why)
		{
			if (bStopping)
			{
				return;
			}
			bStopping = true;
			StopReason = Reason;
			Result.Diagnostic = Why;
			if (Job.IsValid())
			{
				::TerminateJobObject(Job.Value, 1);
			}
			else
			{
				::TerminateProcess(Process.Value, 1);
			}
		};

		for (;;)
		{
			bool bActivity = false;
			if (bStdoutOpen)
			{
				bool bClosed = false;
				if (ReadAvailable(StdoutParent.Value, Chunk, bClosed))
				{
					bActivity = true;
					StdoutBytes += Chunk.Num();
					if (Spec.StdoutSink)
					{
						if (!Spec.StdoutSink(Chunk.GetData(), Chunk.Num()))
						{
							RequestStop(EProcessEnd::Cancelled, TEXT("The output consumer stopped the process."));
						}
					}
					else if (StdoutBytes > Spec.MaxStdoutBytes)
					{
						RequestStop(EProcessEnd::OutputLimit, FString::Printf(TEXT("Output exceeded %lld bytes."), Spec.MaxStdoutBytes));
					}
					else
					{
						Result.Stdout.Append(Chunk);
					}
				}
				bStdoutOpen = !bClosed;
			}
			if (bStderrOpen)
			{
				bool bClosed = false;
				if (ReadAvailable(StderrParent.Value, Chunk, bClosed))
				{
					bActivity = true;
					if (Result.Stderr.Num() + Chunk.Num() > Spec.MaxStderrBytes)
					{
						RequestStop(EProcessEnd::OutputLimit, FString::Printf(TEXT("Error output exceeded %lld bytes."), Spec.MaxStderrBytes));
					}
					else
					{
						Result.Stderr.Append(Chunk);
					}
				}
				bStderrOpen = !bClosed;
			}

			const double Now = FPlatformTime::Seconds();
			if (bActivity)
			{
				LastActivity = Now;
			}
			if (!bExited && ::WaitForSingleObject(Process.Value, 0) == WAIT_OBJECT_0)
			{
				bExited = true;
				ExitedAt = Now;
			}
			if (bExited && !bStdoutOpen && !bStderrOpen)
			{
				break;
			}
			if (bExited && !bActivity && Now - ExitedAt > PipeGraceSeconds)
			{
				// A process outside the job (or a detached helper) still holds the pipes.
				break;
			}
			if (!bStopping)
			{
				if (Spec.Cancel.IsValid() && Spec.Cancel->IsCancelled())
				{
					RequestStop(EProcessEnd::Cancelled, TEXT("Cancelled."));
				}
				else if (Spec.TotalTimeoutSeconds > 0.0 && Now - Start > Spec.TotalTimeoutSeconds)
				{
					RequestStop(EProcessEnd::TotalTimeout, FString::Printf(TEXT("Stopped after %.0f seconds."), Spec.TotalTimeoutSeconds));
				}
				else if (Spec.IdleTimeoutSeconds > 0.0 && Now - LastActivity > Spec.IdleTimeoutSeconds)
				{
					RequestStop(EProcessEnd::IdleTimeout, FString::Printf(TEXT("Stopped after %.0f seconds without output."), Spec.IdleTimeoutSeconds));
				}
			}
			if (!bActivity)
			{
				::WaitForSingleObject(Process.Value, bExited ? 2 : 5);
			}
		}

		if (!bExited)
		{
			if (Job.IsValid())
			{
				::TerminateJobObject(Job.Value, 1);
			}
			else
			{
				::TerminateProcess(Process.Value, 1);
			}
			::WaitForSingleObject(Process.Value, INFINITE);
		}
		if (StdinWriter.IsValid())
		{
			StdinWriter->Join();
		}
		if (!bStopping && Job.IsValid())
		{
			// Helpers git leaves running on purpose (its file system monitor, for one) outlive it,
			// as they do on macOS and Linux. Cancels, timeouts and an editor crash still stop the tree.
			JOBOBJECT_EXTENDED_LIMIT_INFORMATION Release = {};
			::SetInformationJobObject(Job.Value, JobObjectExtendedLimitInformation, &Release, sizeof(Release));
		}
		DWORD ExitCode = 0;
		::GetExitCodeProcess(Process.Value, &ExitCode);
		Result.Seconds = FPlatformTime::Seconds() - Start;
		if (bStopping)
		{
			Result.End = StopReason;
			Result.Diagnostic += TEXT(" Git may have left a stale .git/index.lock; remove it only if no Git process is running.");
		}
		else
		{
			Result.End = EProcessEnd::Exited;
			Result.ExitCode = static_cast<int32>(ExitCode);
		}
		return Result;
	}
}

#endif
