// Copyright UEGitPlugin contributors. Distributed under the MIT license.
#pragma once

#if WITH_DEV_AUTOMATION_TESTS

#include "Core/GitRepository.h"
#include "Core/GitSession.h"
#include "Process/GitProcess.h"
#include "Provider/GitSourceControlProvider.h"
#include "Provider/GitSourceControlUserSettings.h"

#include "Async/TaskGraphInterfaces.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformFileManager.h"
#include "HAL/PlatformMisc.h"
#include "HAL/PlatformProcess.h"
#include "HAL/PlatformTime.h"
#include "Interfaces/IPluginManager.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Guid.h"
#include "Misc/Paths.h"

/** Shared helpers for the plugin's automation tests. Every fixture uses disposable folders. */
namespace GitTest
{
	inline const FString& GitBinary()
	{
		static const FString Binary = GitCore::FindGitExecutable();
		return Binary;
	}

	/** Runs git for fixture setup (not the code under test). */
	inline GitCore::FProcessResult Git(const FString& Directory, TArray<FString> Args, TArray<uint8> Stdin = {})
	{
		GitCore::FProcessSpec Spec;
		Spec.Executable = GitBinary();
		Spec.Arguments = {TEXT("-C"), Directory, TEXT("-c"), TEXT("core.quotepath=false")};
		Spec.Arguments.Append(MoveTemp(Args));
		Spec.Stdin = MoveTemp(Stdin);
		Spec.TotalTimeoutSeconds = 120.0;
		Spec.UnsetEnvironment = {TEXT("GIT_DIR"), TEXT("GIT_WORK_TREE"), TEXT("GIT_INDEX_FILE")};
		Spec.SetEnvironment.Add(TEXT("GIT_TERMINAL_PROMPT"), TEXT("0"));
#if PLATFORM_MAC
		Spec.SetEnvironment.Add(TEXT("PATH"), FPaths::GetPath(GitBinary()) + TEXT(":/opt/homebrew/bin:/usr/local/bin:") + FPlatformMisc::GetEnvironmentVariable(TEXT("PATH")));
#endif
		return GitCore::RunProcess(Spec);
	}

	inline FString Output(const GitCore::FProcessResult& Result)
	{
		return Result.StdoutText().TrimStartAndEnd();
	}

	inline bool WriteFile(const FString& Repo, const FString& Relative, const FString& Content)
	{
		const FString Full = Repo / Relative;
		FPlatformFileManager::Get().GetPlatformFile().SetReadOnly(*Full, false);
		IFileManager::Get().MakeDirectory(*FPaths::GetPath(Full), true);
		return FFileHelper::SaveStringToFile(Content, *Full, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
	}

	inline FString ReadFile(const FString& Repo, const FString& Relative)
	{
		FString Content;
		FFileHelper::LoadFileToString(Content, *(Repo / Relative));
		return Content;
	}

	inline bool FileExists(const FString& Repo, const FString& Relative)
	{
		return IFileManager::Get().FileExists(*(Repo / Relative));
	}

	/** Deterministic settings so the developer's global git config cannot change results. */
	inline void Configure(const FString& Repo, const FString& UserName = TEXT("Test User"))
	{
		Git(Repo, {TEXT("config"), TEXT("user.name"), UserName});
		Git(Repo, {TEXT("config"), TEXT("user.email"), TEXT("test@example.invalid")});
		Git(Repo, {TEXT("config"), TEXT("commit.gpgsign"), TEXT("false")});
		Git(Repo, {TEXT("config"), TEXT("core.autocrlf"), TEXT("false")});
		Git(Repo, {TEXT("config"), TEXT("core.safecrlf"), TEXT("false")});
		Git(Repo, {TEXT("config"), TEXT("core.hooksPath"), TEXT(".git/test-hooks")});
		Git(Repo, {TEXT("config"), TEXT("pull.rebase"), TEXT("false")});
	}

	inline FString CommitAll(const FString& Repo, const FString& Message)
	{
		Git(Repo, {TEXT("add"), TEXT("-A")});
		Git(Repo, {TEXT("commit"), TEXT("-q"), TEXT("--no-verify"), TEXT("-m"), Message});
		return Output(Git(Repo, {TEXT("rev-parse"), TEXT("HEAD")}));
	}

	/** A folder of disposable repositories, deleted with the fixture. */
	struct FFixture
	{
		FString Root;

		FFixture()
		{
			Root = FPaths::ConvertRelativePathToFull(FPlatformProcess::UserTempDir() / (TEXT("git-workspace-test-") + FGuid::NewGuid().ToString(EGuidFormats::Digits)));
			FPaths::NormalizeDirectoryName(Root);
			IFileManager::Get().MakeDirectory(*Root, true);
		}
		~FFixture()
		{
			IFileManager::Get().DeleteDirectory(*Root, false, true);
		}

		FString InitRepo(const FString& Name = TEXT("repo"), const FString& UserName = TEXT("Test User")) const
		{
			const FString Repo = Root / Name;
			IFileManager::Get().MakeDirectory(*Repo, true);
			Git(Repo, {TEXT("init"), TEXT("-q"), TEXT("-b"), TEXT("main")});
			Configure(Repo, UserName);
			return Repo;
		}

		FString InitBare(const FString& Name = TEXT("remote.git")) const
		{
			Git(Root, {TEXT("init"), TEXT("-q"), TEXT("--bare"), TEXT("-b"), TEXT("main"), Root / Name});
			return Root / Name;
		}

		FString Clone(const FString& Remote, const FString& Name, const FString& UserName = TEXT("Test User")) const
		{
			// Pin line endings at checkout time; the developer's global config must not leak in.
			Git(Root, {TEXT("clone"), TEXT("-q"), TEXT("-c"), TEXT("core.autocrlf=false"), TEXT("-c"), TEXT("core.safecrlf=false"), Remote, Root / Name});
			Configure(Root / Name, UserName);
			return Root / Name;
		}
	};

	/** Opens the code under test on a fixture repository. */
	inline GitCore::FGitRepositoryPtr OpenRepository(FAutomationTestBase& Test, const FString& Repo)
	{
		FString Error;
		GitCore::FGitRepositoryPtr Repository = GitCore::FGitRepository::Open(GitBinary(), Repo, Error);
		if (!Repository.IsValid())
		{
			Test.AddError(TEXT("Could not open the fixture repository: ") + Error);
		}
		return Repository;
	}

	/** Pumps game-thread tasks until Predicate holds; false on timeout. */
	inline bool WaitFor(TFunctionRef<bool()> Predicate, double Seconds = 60.0)
	{
		const double Deadline = FPlatformTime::Seconds() + Seconds;
		while (!Predicate())
		{
			if (FPlatformTime::Seconds() > Deadline)
			{
				return false;
			}
			if (IsInGameThread())
			{
				FTaskGraphInterface::Get().ProcessThreadUntilIdle(ENamedThreads::GameThread);
			}
			FPlatformProcess::Sleep(0.01f);
		}
		return true;
	}

	inline FString PythonExecutable()
	{
		const FString Configured = FPlatformMisc::GetEnvironmentVariable(TEXT("GIT_WORKSPACE_TEST_PYTHON"));
		if (!Configured.IsEmpty())
		{
			return Configured;
		}
		// The engine ships its own Python on every platform. A PATH search can find stubs that do
		// not run scripts, such as the Microsoft Store's python.exe on Windows.
		const FString Bundled = FPaths::ConvertRelativePathToFull(FPaths::EngineDir() / TEXT("Binaries/ThirdParty/Python3") /
			FPlatformProcess::GetBinariesSubdirectory() / (PLATFORM_WINDOWS ? TEXT("python.exe") : TEXT("bin/python3")));
		if (IFileManager::Get().FileExists(*Bundled))
		{
			return Bundled;
		}
		TArray<FString> Candidates;
		TArray<FString> SearchPath;
		FPlatformMisc::GetEnvironmentVariable(TEXT("PATH")).ParseIntoArray(SearchPath, PLATFORM_WINDOWS ? TEXT(";") : TEXT(":"), true);
		for (const FString& Directory : SearchPath)
		{
			Candidates.Add(Directory / (PLATFORM_WINDOWS ? TEXT("python.exe") : TEXT("python3")));
		}
		Candidates.Append({TEXT("/opt/homebrew/bin/python3"), TEXT("/usr/local/bin/python3"), TEXT("/usr/bin/python3")});
		for (const FString& Candidate : Candidates)
		{
			if (IFileManager::Get().FileExists(*Candidate))
			{
				return Candidate;
			}
		}
		return FString();
	}

	/**
	 * Loopback Git LFS server (LockTestServer.py) supporting locks and object transfer.
	 * Endpoints under /users/alice and /users/bob act as two different people.
	 */
	struct FLockServer
	{
		FString Root;
		FString BaseUrl;
		FProcHandle Process;

		explicit FLockServer(FAutomationTestBase& Test, const FString& InRoot)
			: Root(InRoot)
		{
			IFileManager::Get().MakeDirectory(*Root, true);
			const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("GitSourceControl"));
			const FString Python = PythonExecutable();
			if (!Plugin.IsValid() || Python.IsEmpty())
			{
				Test.AddError(TEXT("The LFS test server needs Python 3: the engine's own, one on PATH, or GIT_WORKSPACE_TEST_PYTHON."));
				return;
			}
			const FString Script = FPaths::ConvertRelativePathToFull(Plugin->GetBaseDir() / TEXT("Source/GitSourceControl/Private/Tests/LockTestServer.py"));
			const FString Arguments = FString::Printf(TEXT("\"%s\" \"%s\""), *Script, *Root);
			Process = FPlatformProcess::CreateProc(*Python, *Arguments, false, true, true, nullptr, 0, *Root, nullptr);
			FString Port;
			WaitFor([this, &Port]()
			{
				FFileHelper::LoadFileToString(Port, *(Root / TEXT("port")));
				return !Port.TrimStartAndEnd().IsEmpty();
			}, 15.0);
			if (Port.TrimStartAndEnd().IsEmpty())
			{
				Test.AddError(TEXT("The LFS test server did not start."));
				return;
			}
			BaseUrl = TEXT("http://127.0.0.1:") + Port.TrimStartAndEnd();
		}

		~FLockServer()
		{
			if (Process.IsValid())
			{
				FPlatformProcess::TerminateProc(Process, true);
				FPlatformProcess::CloseProc(Process);
			}
		}

		bool IsRunning() const { return !BaseUrl.IsEmpty(); }
		/** LFS endpoint for a person; empty principal uses the shared default identity. */
		FString Endpoint(const FString& Principal = FString()) const
		{
			return Principal.IsEmpty() ? BaseUrl + TEXT("/lfs") : BaseUrl + TEXT("/users/") + Principal + TEXT("/lfs");
		}
		void SetMode(const FString& Mode) const
		{
			FFileHelper::SaveStringToFile(Mode, *(Root / TEXT("mode")));
		}
		int32 CountRequests(const FString& PathFragment) const
		{
			TArray<FString> Lines;
			FFileHelper::LoadFileToStringArray(Lines, *(Root / TEXT("requests")));
			int32 Count = 0;
			for (const FString& Line : Lines)
			{
				Count += Line.Contains(PathFragment) ? 1 : 0;
			}
			return Count;
		}
	};

	/** Adds LFS tracking (with lockable assets) and points the repository at the test server. */
	inline void EnableLfs(const FString& Repo, const FString& Endpoint)
	{
		Git(Repo, {TEXT("lfs"), TEXT("install"), TEXT("--local")});
		Git(Repo, {TEXT("config"), TEXT("lfs.url"), Endpoint});
		Git(Repo, {TEXT("config"), TEXT("lfs.activitytimeout"), TEXT("10")});
		WriteFile(Repo, TEXT(".gitattributes"), TEXT("*.uasset filter=lfs diff=lfs merge=lfs -text lockable\n*.umap filter=lfs diff=lfs merge=lfs -text lockable\n"));
	}

	/** Alice and Bob clones of one origin, each talking to the LFS server as themselves. */
	struct FLockedTeam
	{
		FFixture Fixture;
		TUniquePtr<FLockServer> Server;
		FString Origin;
		FString Alice;
		FString Bob;

		explicit FLockedTeam(FAutomationTestBase& Test)
		{
			Server = MakeUnique<FLockServer>(Test, Fixture.Root / TEXT("server"));
			if (!Server->IsRunning())
			{
				return;
			}
			Origin = Fixture.InitBare();
			Alice = Fixture.InitRepo(TEXT("alice"), TEXT("Alice"));
			EnableLfs(Alice, Server->Endpoint(TEXT("alice")));
			WriteFile(Alice, TEXT("Content/Hero.uasset"), TEXT("hero v1\n"));
			WriteFile(Alice, TEXT("Content/Map.umap"), TEXT("map v1\n"));
			WriteFile(Alice, TEXT("readme.txt"), TEXT("text\n"));
			CommitAll(Alice, TEXT("base"));
			Git(Alice, {TEXT("remote"), TEXT("add"), TEXT("origin"), Origin});
			Git(Alice, {TEXT("push"), TEXT("-q"), TEXT("-u"), TEXT("origin"), TEXT("main")});
			Bob = Fixture.Root / TEXT("bob");
			Git(Fixture.Root, {TEXT("-c"), TEXT("lfs.url=") + Server->Endpoint(TEXT("bob")), TEXT("clone"), TEXT("-q"), TEXT("-c"), TEXT("core.autocrlf=false"), Origin, Bob});
			Configure(Bob, TEXT("Bob"));
			Git(Bob, {TEXT("lfs"), TEXT("install"), TEXT("--local")});
			Git(Bob, {TEXT("config"), TEXT("lfs.url"), Server->Endpoint(TEXT("bob"))});
		}

		bool IsReady() const { return !Alice.IsEmpty(); }
	};

	/** Turns off background fetches so a test decides when the remote is read. */
	struct FScopedNoBackgroundFetch
	{
		int32 Saved;
		FScopedNoBackgroundFetch()
		{
			Saved = GetDefault<UGitSourceControlUserSettings>()->BackgroundFetchSeconds;
			GetMutableDefault<UGitSourceControlUserSettings>()->BackgroundFetchSeconds = 0;
		}
		~FScopedNoBackgroundFetch()
		{
			GetMutableDefault<UGitSourceControlUserSettings>()->BackgroundFetchSeconds = Saved;
		}
	};

	/** A provider for a fixture repository, closed together with its session at the end of the test. */
	struct FTestProvider
	{
		FGitSourceControlProvider Provider;

		explicit FTestProvider(const FString& Repo)
			: Provider(Repo)
		{
			Provider.Init();
		}
		~FTestProvider()
		{
			Provider.Close();
		}
		FGitSourceControlProvider* operator->() { return &Provider; }
		GitCore::FSessionSnapshotRef Snapshot() const { return Provider.GetSession()->GetSnapshot(); }
	};
}

#endif
