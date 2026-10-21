// Copyright UEGitPlugin contributors. Distributed under the MIT license.
#include "GitProcess.h"

#if PLATFORM_MAC || PLATFORM_LINUX

#include "HAL/PlatformTime.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <spawn.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include <string>
#include <vector>
#if PLATFORM_MAC
#include <crt_externs.h>
#else
extern char** environ;
#endif

namespace GitCore
{
	namespace
	{
		constexpr double KillGraceSeconds = 2.0;
		constexpr double PipeGraceSeconds = 0.25;
		constexpr int32 ReadChunk = 64 * 1024;
		constexpr int32 WriteChunk = 64 * 1024;

		char** InheritedEnvironment()
		{
#if PLATFORM_MAC
			return *_NSGetEnviron();
#else
			return environ;
#endif
		}

		bool ConfigureDescriptor(int Descriptor, bool bNonBlocking)
		{
			const int DescriptorFlags = fcntl(Descriptor, F_GETFD);
			if (DescriptorFlags < 0 || fcntl(Descriptor, F_SETFD, DescriptorFlags | FD_CLOEXEC) < 0)
			{
				return false;
			}
			if (bNonBlocking)
			{
				const int StatusFlags = fcntl(Descriptor, F_GETFL);
				if (StatusFlags < 0 || fcntl(Descriptor, F_SETFL, StatusFlags | O_NONBLOCK) < 0)
				{
					return false;
				}
			}
			return true;
		}

		struct FPipe
		{
			int Read = -1;
			int Write = -1;

			bool Open(bool bParentReads)
			{
				int Descriptors[2] = {-1, -1};
				if (pipe(Descriptors) != 0)
				{
					return false;
				}
				Read = Descriptors[0];
				Write = Descriptors[1];
				// The parent's end is nonblocking; both ends close on exec (dup2 clears it for the child).
				return ConfigureDescriptor(Read, bParentReads) && ConfigureDescriptor(Write, !bParentReads);
			}

			static void CloseOne(int& Descriptor)
			{
				if (Descriptor >= 0)
				{
					close(Descriptor);
					Descriptor = -1;
				}
			}

			void Close()
			{
				CloseOne(Read);
				CloseOne(Write);
			}
		};

		/** Writes without letting SIGPIPE terminate the editor on Linux (Mac ignores SIGPIPE). */
		ssize_t WriteWithoutSigpipe(int Descriptor, const uint8* Data, size_t Size)
		{
#if PLATFORM_LINUX
			sigset_t Block;
			sigset_t Previous;
			sigemptyset(&Block);
			sigaddset(&Block, SIGPIPE);
			pthread_sigmask(SIG_BLOCK, &Block, &Previous);
			const ssize_t Written = write(Descriptor, Data, Size);
			const int WriteError = errno;
			if (Written < 0 && WriteError == EPIPE)
			{
				const timespec Zero = {0, 0};
				sigtimedwait(&Block, nullptr, &Zero);
			}
			pthread_sigmask(SIG_SETMASK, &Previous, nullptr);
			errno = WriteError;
			return Written;
#else
			return write(Descriptor, Data, Size);
#endif
		}
	}

	FProcessResult RunProcess(const FProcessSpec& Spec)
	{
		FProcessResult Result;
		const double Start = FPlatformTime::Seconds();

#if !PLATFORM_MAC
		if (!Spec.WorkingDirectory.IsEmpty())
		{
			Result.Diagnostic = TEXT("This platform's process launcher cannot set a working directory; pass -C to git instead.");
			return Result;
		}
#endif

		const bool bHasStdin = Spec.Stdin.Num() > 0;
		FPipe Input;
		FPipe Output;
		FPipe Errors;
		if ((bHasStdin && !Input.Open(false)) || !Output.Open(true) || !Errors.Open(true))
		{
			Result.Diagnostic = FString::Printf(TEXT("Could not create process pipes (error %d)."), errno);
			Input.Close();
			Output.Close();
			Errors.Close();
			return Result;
		}

		posix_spawn_file_actions_t Actions;
		posix_spawnattr_t Attributes;
		posix_spawn_file_actions_init(&Actions);
		posix_spawnattr_init(&Attributes);
		int SetupError = 0;
		if (bHasStdin)
		{
			SetupError = posix_spawn_file_actions_adddup2(&Actions, Input.Read, STDIN_FILENO);
		}
		else
		{
			SetupError = posix_spawn_file_actions_addopen(&Actions, STDIN_FILENO, "/dev/null", O_RDONLY, 0);
		}
		if (!SetupError)
		{
			SetupError = posix_spawn_file_actions_adddup2(&Actions, Output.Write, STDOUT_FILENO);
		}
		if (!SetupError)
		{
			SetupError = posix_spawn_file_actions_adddup2(&Actions, Errors.Write, STDERR_FILENO);
		}
#if PLATFORM_MAC
		if (!SetupError && !Spec.WorkingDirectory.IsEmpty())
		{
			SetupError = posix_spawn_file_actions_addchdir_np(&Actions, TCHAR_TO_UTF8(*Spec.WorkingDirectory));
		}
#endif

		// A new session detaches the child from any terminal, so ssh or credential
		// helpers cannot block on a prompt the user will never see. It also makes
		// the child a process-group leader, which lets us stop its whole tree.
		short SpawnFlags = POSIX_SPAWN_SETSIGMASK | POSIX_SPAWN_SETSIGDEF;
#ifdef POSIX_SPAWN_SETSID
		SpawnFlags |= POSIX_SPAWN_SETSID;
#else
		SpawnFlags |= POSIX_SPAWN_SETPGROUP;
		if (!SetupError)
		{
			SetupError = posix_spawnattr_setpgroup(&Attributes, 0);
		}
#endif
#if PLATFORM_MAC
		// Only stdin/stdout/stderr reach the child, never the editor's other descriptors.
		SpawnFlags |= POSIX_SPAWN_CLOEXEC_DEFAULT;
#endif
		sigset_t EmptyMask;
		sigemptyset(&EmptyMask);
		sigset_t DefaultSignals;
		sigemptyset(&DefaultSignals);
		sigaddset(&DefaultSignals, SIGPIPE);
		if (!SetupError)
		{
			SetupError = posix_spawnattr_setsigmask(&Attributes, &EmptyMask);
		}
		if (!SetupError)
		{
			SetupError = posix_spawnattr_setsigdefault(&Attributes, &DefaultSignals);
		}
		if (!SetupError)
		{
			SetupError = posix_spawnattr_setflags(&Attributes, SpawnFlags);
		}

		std::vector<std::string> ArgumentStrings;
		ArgumentStrings.emplace_back(TCHAR_TO_UTF8(*Spec.Executable));
		for (const FString& Argument : Spec.Arguments)
		{
			ArgumentStrings.emplace_back(TCHAR_TO_UTF8(*Argument));
		}
		std::vector<char*> Argv;
		for (std::string& Argument : ArgumentStrings)
		{
			Argv.push_back(Argument.data());
		}
		Argv.push_back(nullptr);

		TArray<FString> Inherited;
		for (char** Entry = InheritedEnvironment(); Entry && *Entry; ++Entry)
		{
			Inherited.Add(UTF8_TO_TCHAR(*Entry));
		}
		std::vector<std::string> EnvironmentStrings;
		for (const FString& Entry : ProcessDetail::BuildEnvironment(Inherited, Spec))
		{
			EnvironmentStrings.emplace_back(TCHAR_TO_UTF8(*Entry));
		}
		std::vector<char*> Envp;
		for (std::string& Entry : EnvironmentStrings)
		{
			Envp.push_back(Entry.data());
		}
		Envp.push_back(nullptr);

		pid_t ProcessId = 0;
		int SpawnError = SetupError;
		if (!SpawnError)
		{
			// posix_spawnp searches PATH when the executable is a bare name.
			SpawnError = posix_spawnp(&ProcessId, ArgumentStrings[0].c_str(), &Actions, &Attributes, Argv.data(), Envp.data());
		}
		posix_spawn_file_actions_destroy(&Actions);
		posix_spawnattr_destroy(&Attributes);
		FPipe::CloseOne(Input.Read);
		FPipe::CloseOne(Output.Write);
		FPipe::CloseOne(Errors.Write);
		if (SpawnError)
		{
			Input.Close();
			Output.Close();
			Errors.Close();
			Result.Diagnostic = FString::Printf(TEXT("Could not start %s (error %d)."), *Spec.Executable, SpawnError);
			return Result;
		}

		bool bStdoutOpen = true;
		bool bStderrOpen = true;
		bool bStdinOpen = bHasStdin;
		int64 StdinOffset = 0;
		int64 StdoutBytes = 0;
		bool bExited = false;
		bool bLostChild = false;
		int WaitStatus = 0;
		double ExitedAt = 0.0;
		double LastActivity = Start;
		bool bStopping = false;
		bool bKilled = false;
		double StopRequestedAt = 0.0;
		EProcessEnd StopReason = EProcessEnd::Cancelled;

		auto RequestStop = [&](EProcessEnd Reason, const FString& Why)
		{
			if (bStopping)
			{
				return;
			}
			bStopping = true;
			StopReason = Reason;
			Result.Diagnostic = Why;
			StopRequestedAt = FPlatformTime::Seconds();
			if (!bExited)
			{
				// Git removes its lock files on SIGTERM; escalate only if it ignores us.
				kill(-ProcessId, SIGTERM);
			}
		};

		// Heap buffer: editor worker threads can have small stacks.
		TArray<uint8> ReadBuffer;
		ReadBuffer.SetNumUninitialized(ReadChunk);
		auto Drain = [&](int& Descriptor, bool& bOpen, bool bIsStdout)
		{
			uint8* Buffer = ReadBuffer.GetData();
			for (;;)
			{
				const ssize_t Count = read(Descriptor, Buffer, ReadChunk);
				if (Count > 0)
				{
					LastActivity = FPlatformTime::Seconds();
					if (bIsStdout)
					{
						StdoutBytes += Count;
						if (Spec.StdoutSink)
						{
							if (!Spec.StdoutSink(Buffer, static_cast<int32>(Count)))
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
							Result.Stdout.Append(Buffer, static_cast<int32>(Count));
						}
					}
					else if (Result.Stderr.Num() + Count > Spec.MaxStderrBytes)
					{
						RequestStop(EProcessEnd::OutputLimit, FString::Printf(TEXT("Error output exceeded %lld bytes."), Spec.MaxStderrBytes));
					}
					else
					{
						Result.Stderr.Append(Buffer, static_cast<int32>(Count));
					}
					if (bStopping && StopReason == EProcessEnd::OutputLimit)
					{
						return;
					}
					continue;
				}
				if (Count == 0)
				{
					FPipe::CloseOne(Descriptor);
					bOpen = false;
					return;
				}
				if (errno == EINTR)
				{
					continue;
				}
				if (errno != EAGAIN && errno != EWOULDBLOCK)
				{
					FPipe::CloseOne(Descriptor);
					bOpen = false;
				}
				return;
			}
		};

		for (;;)
		{
			const double Now = FPlatformTime::Seconds();
			if (!bExited)
			{
				const pid_t Waited = waitpid(ProcessId, &WaitStatus, WNOHANG);
				if (Waited == ProcessId)
				{
					bExited = true;
					ExitedAt = Now;
				}
				else if (Waited < 0 && errno == ECHILD)
				{
					// Someone else reaped the child (for example an engine SIGCHLD handler).
					bExited = true;
					bLostChild = true;
					ExitedAt = Now;
				}
			}
			if (bExited && !bStdoutOpen && !bStderrOpen)
			{
				break;
			}
			if (bExited && Now - ExitedAt > PipeGraceSeconds)
			{
				// A grandchild (for example a credential daemon) still holds the pipes.
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
			if (bStopping && !bExited && !bKilled && Now - StopRequestedAt > KillGraceSeconds)
			{
				kill(-ProcessId, SIGKILL);
				bKilled = true;
			}

			pollfd Ready[3];
			int ReadyCount = 0;
			int StdoutSlot = -1;
			int StderrSlot = -1;
			int StdinSlot = -1;
			if (bStdoutOpen)
			{
				StdoutSlot = ReadyCount;
				Ready[ReadyCount++] = {Output.Read, POLLIN, 0};
			}
			if (bStderrOpen)
			{
				StderrSlot = ReadyCount;
				Ready[ReadyCount++] = {Errors.Read, POLLIN, 0};
			}
			if (bStdinOpen)
			{
				StdinSlot = ReadyCount;
				Ready[ReadyCount++] = {Input.Write, POLLOUT, 0};
			}
			const int Polled = poll(Ready, ReadyCount, 20);
			if (Polled < 0 && errno != EINTR)
			{
				RequestStop(EProcessEnd::Cancelled, FString::Printf(TEXT("Could not wait for process output (error %d)."), errno));
			}
			if (Polled <= 0)
			{
				continue;
			}
			if (StdoutSlot >= 0 && Ready[StdoutSlot].revents)
			{
				Drain(Output.Read, bStdoutOpen, true);
			}
			if (StderrSlot >= 0 && Ready[StderrSlot].revents)
			{
				Drain(Errors.Read, bStderrOpen, false);
			}
			if (StdinSlot >= 0 && Ready[StdinSlot].revents)
			{
				const int64 Remaining = Spec.Stdin.Num() - StdinOffset;
				const ssize_t Written = Remaining > 0
					? WriteWithoutSigpipe(Input.Write, Spec.Stdin.GetData() + StdinOffset, static_cast<size_t>(FMath::Min<int64>(Remaining, WriteChunk)))
					: 0;
				if (Written > 0)
				{
					StdinOffset += Written;
				}
				const bool bBroken = Written < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR;
				if (bBroken || StdinOffset >= Spec.Stdin.Num())
				{
					FPipe::CloseOne(Input.Write);
					bStdinOpen = false;
				}
			}
		}

		if (!bExited)
		{
			kill(-ProcessId, SIGKILL);
			while (waitpid(ProcessId, &WaitStatus, 0) < 0 && errno == EINTR)
			{
			}
		}
		Input.Close();
		Output.Close();
		Errors.Close();
		Result.Seconds = FPlatformTime::Seconds() - Start;

		if (bStopping)
		{
			Result.End = StopReason;
		}
		else if (bLostChild)
		{
			Result.End = EProcessEnd::Signaled;
			Result.Diagnostic = TEXT("The process exit status was lost; treat the outcome as unknown.");
		}
		else if (WIFEXITED(WaitStatus))
		{
			Result.End = EProcessEnd::Exited;
			Result.ExitCode = WEXITSTATUS(WaitStatus);
		}
		else if (WIFSIGNALED(WaitStatus))
		{
			Result.End = EProcessEnd::Signaled;
			Result.Signal = WTERMSIG(WaitStatus);
			Result.Diagnostic = FString::Printf(TEXT("Terminated by signal %d."), Result.Signal);
		}
		return Result;
	}
}

#endif
