// Copyright UEGitPlugin contributors. Distributed under the MIT license.
#pragma once

#include "CoreMinimal.h"
#include "HAL/ThreadSafeBool.h"

namespace GitCore
{
	/** Shared flag a caller can set to stop a running process. */
	class FCancelToken
	{
	public:
		void Cancel() { bCancelled = true; }
		bool IsCancelled() const { return bCancelled; }

	private:
		FThreadSafeBool bCancelled;
	};
	using FCancelTokenPtr = TSharedPtr<FCancelToken, ESPMode::ThreadSafe>;

	/** How a child process ended. Only Exited carries a trustworthy exit code. */
	enum class EProcessEnd : uint8
	{
		Exited,
		Signaled,
		IdleTimeout,
		TotalTimeout,
		Cancelled,
		OutputLimit,
		SpawnFailed
	};

	/** Everything needed to run one child process without a shell. */
	struct FProcessSpec
	{
		FString Executable;
		/** Empty keeps the editor's working directory. Git calls pass -C instead. */
		FString WorkingDirectory;
		TArray<FString> Arguments;
		/** Variables added to or replaced in the inherited environment. */
		TMap<FString, FString> SetEnvironment;
		/** Variables removed from the inherited environment. */
		TArray<FString> UnsetEnvironment;
		/** Bytes written to the child's stdin, which is then closed. */
		TArray<uint8> Stdin;
		/** Seconds without any output before the process is stopped. Zero disables. */
		double IdleTimeoutSeconds = 0.0;
		/** Seconds before the process is stopped regardless of output. Zero disables. */
		double TotalTimeoutSeconds = 0.0;
		int64 MaxStdoutBytes = 256ll * 1024 * 1024;
		int64 MaxStderrBytes = 4ll * 1024 * 1024;
		/** Optional streaming consumer. When set, stdout is not accumulated. Return false to stop. */
		TFunction<bool(const uint8* Data, int32 Size)> StdoutSink;
		FCancelTokenPtr Cancel;
	};

	struct FProcessResult
	{
		EProcessEnd End = EProcessEnd::SpawnFailed;
		int32 ExitCode = -1;
		int32 Signal = 0;
		TArray<uint8> Stdout;
		TArray<uint8> Stderr;
		double Seconds = 0.0;
		/** Runner-level explanation when the process did not exit normally. */
		FString Diagnostic;

		bool Succeeded() const { return End == EProcessEnd::Exited && ExitCode == 0; }
		bool Exited() const { return End == EProcessEnd::Exited; }
		FString StdoutText() const;
		FString StderrText() const;
		/** Stderr (or the runner diagnostic) trimmed for messages. */
		FString ErrorText() const;
	};

	/** Runs a process and returns once it and its output pipes are finished. Blocking; never call on the game thread for git work. */
	FProcessResult RunProcess(const FProcessSpec& Spec);

	/** Decodes UTF-8 bytes. */
	FString DecodeUtf8(const uint8* Data, int32 Size);
	inline FString DecodeUtf8(const TArray<uint8>& Bytes) { return DecodeUtf8(Bytes.GetData(), Bytes.Num()); }
	void AppendUtf8(TArray<uint8>& Bytes, FStringView Text);

	namespace ProcessDetail
	{
		/** Merges the inherited environment with the spec's overrides as NAME=VALUE entries. */
		TArray<FString> BuildEnvironment(const TArray<FString>& Inherited, const FProcessSpec& Spec);
		/** Quotes one argument using the Windows CRT parsing rules. */
		FString QuoteWindowsArgument(const FString& Argument);
	}
}
