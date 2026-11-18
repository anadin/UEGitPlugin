// Copyright UEGitPlugin contributors. Distributed under the MIT license.
#include "GitRepository.h"
#include "GitParse.h"

#include "HAL/FileManager.h"
#include "HAL/PlatformProcess.h"
#include "Misc/Guid.h"
#include "Misc/Paths.h"

namespace GitCore
{
	namespace
	{
		/** Removes user:password@ from a URL so endpoints can be shown and used as keys safely. */
		FString WithoutCredentials(const FString& Url)
		{
			const int32 Scheme = Url.Find(TEXT("://"));
			if (Scheme == INDEX_NONE)
			{
				return Url;
			}
			const int32 HostStart = Scheme + 3;
			const int32 PathStart = Url.Find(TEXT("/"), ESearchCase::CaseSensitive, ESearchDir::FromStart, HostStart);
			const int32 At = Url.Find(TEXT("@"), ESearchCase::CaseSensitive, ESearchDir::FromStart, HostStart);
			if (At == INDEX_NONE || (PathStart != INDEX_NONE && At > PathStart))
			{
				return Url;
			}
			return Url.Left(HostStart) + Url.Mid(At + 1);
		}

		FGitCall LfsCall(ECallKind Kind, const FString& Remote, TArray<FString> LfsArgs, const FString& Storage = FString())
		{
			FGitCall Call;
			Call.Kind = Kind;
			// Git LFS matches paths internally; literal pathspec mode makes it silently skip work.
			Call.bLiteralPathspecs = false;
			Call.Args = {TEXT("-c"), TEXT("remote.lfsdefault=") + Remote, TEXT("-c"), TEXT("remote.lfspushdefault=") + Remote};
			if (!Storage.IsEmpty())
			{
				Call.Args.Append({TEXT("-c"), TEXT("lfs.storage=") + Storage});
			}
			Call.Args.Add(TEXT("lfs"));
			Call.Args.Append(MoveTemp(LfsArgs));
			return Call;
		}

		struct FScopedTempDirectory
		{
			FString Path = FPaths::Combine(FPlatformProcess::UserTempDir(), TEXT("git-workspace-locks-") + FGuid::NewGuid().ToString(EGuidFormats::Digits));
			~FScopedTempDirectory() { IFileManager::Get().DeleteDirectory(*Path, false, true); }
		};
	}

	bool FGitRepository::ResolveLockEndpoint(const FString& Remote, FString& OutEndpoint, FString& OutError) const
	{
		const FProcessResult Env = Run(LfsCall(ECallKind::LocalQuick, Remote, {TEXT("env")}));
		if (!Env.Succeeded())
		{
			OutError = DescribeFailure(Env, TEXT("Checking Git LFS"));
			return false;
		}
		TArray<FString> Lines;
		Env.StdoutText().ParseIntoArrayLines(Lines);
		const FString RemotePrefix = TEXT("Endpoint (") + Remote + TEXT(")=");
		FString Endpoint;
		for (const FString& Line : Lines)
		{
			const bool bRemoteLine = Line.StartsWith(RemotePrefix);
			if (bRemoteLine || (Endpoint.IsEmpty() && Line.StartsWith(TEXT("Endpoint="))))
			{
				Endpoint = Line.Mid(bRemoteLine ? RemotePrefix.Len() : 9);
				const int32 Auth = Endpoint.Find(TEXT(" (auth="));
				if (Auth != INDEX_NONE)
				{
					Endpoint.LeftInline(Auth);
				}
				if (bRemoteLine)
				{
					break;
				}
			}
		}
		if (Endpoint.IsEmpty())
		{
			OutError = FString::Printf(TEXT("Git LFS has no lock server for the remote \"%s\"."), *Remote);
			return false;
		}
		OutEndpoint = WithoutCredentials(Endpoint);
		return true;
	}

	bool FGitRepository::VerifyLocks(const FString& Remote, ECallKind Kind, FLockSnapshot& Out) const
	{
		Out.Remote = Remote;
		FString Endpoint;
		if (!ResolveLockEndpoint(Remote, Endpoint, Out.Error))
		{
			return false;
		}
		Out.Endpoint = Endpoint;
		// `locks --verify --json` can exit successfully with partial results after a server
		// error (Git LFS 3.x). Require the plain verification to succeed into a private cache,
		// then read that complete cache as JSON. A shared old cache is never trusted.
		FScopedTempDirectory Cache;
		const FProcessResult Verified = Run(LfsCall(Kind, Remote, {TEXT("locks"), TEXT("--verify"), TEXT("--remote=") + Remote}, Cache.Path));
		if (!Verified.Succeeded())
		{
			Out.Error = DescribeFailure(Verified, TEXT("Checking locks"));
			return false;
		}
		const FProcessResult Cached = Run(LfsCall(ECallKind::LocalQuick, Remote, {TEXT("locks"), TEXT("--verify"), TEXT("--cached"), TEXT("--json"), TEXT("--remote=") + Remote}, Cache.Path));
		TMap<FString, FLockInfo> Locks;
		FString ParseError;
		if (!Cached.Succeeded() || !ParseVerifiedLocks(Cached.StdoutText(), Locks, ParseError))
		{
			Out.Error = Cached.Succeeded() ? ParseError : DescribeFailure(Cached, TEXT("Checking locks"));
			return false;
		}
		Out.ByPath = MoveTemp(Locks);
		Out.bVerified = true;
		Out.VerifiedAtUtc = FDateTime::UtcNow();
		Out.Error.Reset();
		return true;
	}

	FOpResult FGitRepository::Lock(const FString& Remote, const FString& Path, FLockInfo& OutLock) const
	{
		if (!IsSafeRelativePath(Path))
		{
			return FOpResult::Failure(FString::Printf(TEXT("Cannot lock \"%s\": it is not a file in this repository."), *Path));
		}
		const FProcessResult Result = Run(LfsCall(ECallKind::UserNetwork, Remote, {TEXT("lock"), TEXT("--json"), TEXT("--remote=") + Remote, TEXT("--"), Path}));
		if (!Result.Succeeded())
		{
			const FString Errors = Result.ErrorText();
			if (Errors.Contains(TEXT("already"), ESearchCase::IgnoreCase) || Errors.Contains(TEXT("409")))
			{
				return FOpResult::Failure(FString::Printf(TEXT("%s is already locked by someone else."), *FPaths::GetCleanFilename(Path)), Errors);
			}
			return FOpResult::Failure(DescribeFailure(Result, FString::Printf(TEXT("Locking %s"), *FPaths::GetCleanFilename(Path))), Errors);
		}
		TArray<FLockInfo> Locks;
		FString ParseError;
		if (!ParseLockResponse(Result.StdoutText(), Locks, ParseError) || Locks.Num() != 1 || Locks[0].Path != Path)
		{
			return FOpResult::Failure(TEXT("The lock server's reply could not be confirmed. Check locks before editing."), ParseError + Result.StdoutText());
		}
		OutLock = MoveTemp(Locks[0]);
		return FOpResult::Success();
	}

	FOpResult FGitRepository::Unlock(const FString& Remote, const FString& LockId, bool bForce) const
	{
		TArray<FString> Args = {TEXT("unlock"), TEXT("--json"), TEXT("--remote=") + Remote, TEXT("--id=") + LockId};
		if (bForce)
		{
			Args.Add(TEXT("--force"));
		}
		const FProcessResult Result = Run(LfsCall(ECallKind::UserNetwork, Remote, Args));
		if (!Result.Succeeded())
		{
			return FOpResult::Failure(DescribeFailure(Result, TEXT("Unlocking")), Result.ErrorText());
		}
		return FOpResult::Success();
	}
}
