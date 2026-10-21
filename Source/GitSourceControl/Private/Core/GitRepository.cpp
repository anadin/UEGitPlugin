// Copyright UEGitPlugin contributors. Distributed under the MIT license.
#include "GitRepository.h"

#include "HAL/FileManager.h"
#include "HAL/PlatformMisc.h"
#include "Misc/Paths.h"

namespace GitCore
{
	namespace
	{
		/** Variables that redirect git to another repository, index or config; never inherited. */
		const TCHAR* const ScrubbedVariables[] = {
			TEXT("GIT_DIR"), TEXT("GIT_WORK_TREE"), TEXT("GIT_INDEX_FILE"), TEXT("GIT_OBJECT_DIRECTORY"),
			TEXT("GIT_ALTERNATE_OBJECT_DIRECTORIES"), TEXT("GIT_COMMON_DIR"), TEXT("GIT_CONFIG"),
			TEXT("GIT_CONFIG_PARAMETERS"), TEXT("GIT_CONFIG_COUNT"), TEXT("GIT_IMPLICIT_WORK_TREE"),
			TEXT("GIT_GRAFT_FILE"), TEXT("GIT_NO_REPLACE_OBJECTS"), TEXT("GIT_REPLACE_REF_BASE"), TEXT("GIT_PREFIX"),
			TEXT("GIT_SHALLOW_FILE"), TEXT("GIT_NAMESPACE"), TEXT("GIT_LITERAL_PATHSPECS"), TEXT("GIT_GLOB_PATHSPECS"),
			TEXT("GIT_NOGLOB_PATHSPECS"), TEXT("GIT_ICASE_PATHSPECS")};

		FString NormalizeDirectory(FString Path)
		{
			Path = FPaths::ConvertRelativePathToFull(Path);
			FPaths::NormalizeFilename(Path);
			while (Path.Len() > 1 && Path.EndsWith(TEXT("/")))
			{
				Path.LeftChopInline(1);
			}
			return Path;
		}

		ESearchCase::Type PathCase()
		{
#if PLATFORM_LINUX
			return ESearchCase::CaseSensitive;
#else
			return ESearchCase::IgnoreCase;
#endif
		}

		bool StripRoot(const FString& Absolute, const FString& Base, FString& OutRelative)
		{
			if (Base.IsEmpty())
			{
				return false;
			}
			if (Absolute.Equals(Base, PathCase()))
			{
				OutRelative.Reset();
				return true;
			}
			const FString Prefix = Base + TEXT("/");
			if (Absolute.StartsWith(Prefix, PathCase()))
			{
				OutRelative = Absolute.Mid(Prefix.Len());
				return true;
			}
			return false;
		}
	}

	FString FindGitExecutable(const FString& ConfiguredPath)
	{
		if (!ConfiguredPath.IsEmpty() && IFileManager::Get().FileExists(*ConfiguredPath))
		{
			return ConfiguredPath;
		}
		TArray<FString> Candidates;
#if PLATFORM_MAC
		Candidates = {TEXT("/opt/homebrew/bin/git"), TEXT("/usr/local/bin/git"), TEXT("/usr/bin/git")};
#elif PLATFORM_LINUX
		Candidates = {TEXT("/usr/bin/git"), TEXT("/usr/local/bin/git")};
#endif
		TArray<FString> SearchPath;
		FPlatformMisc::GetEnvironmentVariable(TEXT("PATH")).ParseIntoArray(SearchPath, PLATFORM_WINDOWS ? TEXT(";") : TEXT(":"), true);
		for (const FString& Directory : SearchPath)
		{
			Candidates.Add(FPaths::Combine(Directory, PLATFORM_WINDOWS ? TEXT("git.exe") : TEXT("git")));
		}
#if PLATFORM_WINDOWS
		Candidates.Add(TEXT("C:/Program Files/Git/cmd/git.exe"));
		Candidates.Add(TEXT("C:/Program Files/Git/bin/git.exe"));
		Candidates.Add(TEXT("C:/Program Files (x86)/Git/cmd/git.exe"));
#endif
		for (const FString& Candidate : Candidates)
		{
			if (IFileManager::Get().FileExists(*Candidate))
			{
				return Candidate;
			}
		}
		return FString();
	}

	bool FToolVersions::IsGitSupported() const
	{
		// "git version 2.39.5 (Apple Git-154)" or "git version 2.47.1.windows.1"
		TArray<FString> Words;
		Git.ParseIntoArrayWS(Words);
		TArray<FString> Parts;
		if (Words.Num() < 3 || Words[2].ParseIntoArray(Parts, TEXT(".")) < 2)
		{
			return false;
		}
		const int32 Major = FCString::Atoi(*Parts[0]);
		const int32 Minor = FCString::Atoi(*Parts[1]);
		return Major > 2 || (Major == 2 && Minor >= 38);
	}

	FToolVersions FGitRepository::ReadToolVersions(const FString& GitBinary)
	{
		FToolVersions Versions;
		if (GitBinary.IsEmpty())
		{
			return Versions;
		}
		FGitRepository Probe;
		Probe.GitBinary = GitBinary;
		const FProcessResult Git = Probe.Run(ECallKind::LocalQuick, {TEXT("version")});
		if (Git.Succeeded())
		{
			Versions.Git = Git.StdoutText().TrimStartAndEnd();
		}
		const FProcessResult Lfs = Probe.Run(ECallKind::LocalQuick, {TEXT("lfs"), TEXT("version")});
		if (Lfs.Succeeded())
		{
			Versions.Lfs = Lfs.StdoutText().TrimStartAndEnd();
		}
		return Versions;
	}

	FGitRepositoryPtr FGitRepository::Open(const FString& GitBinary, const FString& Path, FString& OutError)
	{
		if (GitBinary.IsEmpty())
		{
			OutError = TEXT("Git was not found. Install Git, or set its path in the revision control settings.");
			return nullptr;
		}
		FString Directory = NormalizeDirectory(Path);
		while (!Directory.IsEmpty() && !IFileManager::Get().DirectoryExists(*Directory))
		{
			Directory = FPaths::GetPath(Directory);
		}

		TSharedRef<FGitRepository, ESPMode::ThreadSafe> Repository = MakeShareable(new FGitRepository());
		Repository->GitBinary = GitBinary;
		FGitCall Discover;
		Discover.Kind = ECallKind::LocalQuick;
		Discover.Args = {TEXT("-C"), Directory, TEXT("rev-parse"), TEXT("--path-format=absolute"), TEXT("--show-toplevel"),
			TEXT("--git-dir"), TEXT("--git-common-dir"), TEXT("--show-prefix")};
		const FProcessResult Result = Repository->Run(Discover);
		TArray<FString> Lines;
		Result.StdoutText().ParseIntoArrayLines(Lines, false);
		if (!Result.Succeeded() || Lines.Num() < 3)
		{
			OutError = Result.Succeeded() ? TEXT("Git did not report the repository location.") : DescribeFailure(Result, TEXT("Finding the Git repository"));
			return nullptr;
		}
		Repository->Root = NormalizeDirectory(Lines[0]);
		Repository->GitDir = NormalizeDirectory(Lines[1]);
		Repository->CommonDir = NormalizeDirectory(Lines[2]);
		// When the editor reaches the project through a symlink, git reports the resolved
		// root. Remember the editor's spelling too so its paths still map into the repository.
		const FString Prefix = Lines.Num() > 3 ? Lines[3] : FString();
		const FString PrefixDir = Prefix.EndsWith(TEXT("/")) ? Prefix.LeftChop(1) : Prefix;
		// FString::EndsWith is false for an empty suffix, so the top-level case is checked separately.
		if (PrefixDir.IsEmpty() || Directory.EndsWith(TEXT("/") + PrefixDir, PathCase()))
		{
			const FString Alias = NormalizeDirectory(PrefixDir.IsEmpty() ? Directory : Directory.LeftChop(PrefixDir.Len()));
			if (!Alias.Equals(Repository->Root, PathCase()))
			{
				Repository->AliasRoot = Alias;
			}
		}

		const FString UserSsh = FPlatformMisc::GetEnvironmentVariable(TEXT("GIT_SSH_COMMAND")) + FPlatformMisc::GetEnvironmentVariable(TEXT("GIT_SSH"));
		const FProcessResult SshConfig = Repository->Run(ECallKind::LocalQuick, {TEXT("config"), TEXT("--get"), TEXT("core.sshCommand")});
		Repository->bUserSshCommand = !UserSsh.IsEmpty() || (SshConfig.Succeeded() && !SshConfig.StdoutText().TrimStartAndEnd().IsEmpty());
		return Repository;
	}

	void FGitRepository::AddEnvironment(FProcessSpec& Spec, ECallKind Kind) const
	{
		for (const TCHAR* Variable : ScrubbedVariables)
		{
			Spec.UnsetEnvironment.Add(Variable);
		}
		Spec.SetEnvironment.Add(TEXT("GIT_TERMINAL_PROMPT"), TEXT("0"));
		Spec.SetEnvironment.Add(TEXT("LC_ALL"), TEXT("C"));
#if PLATFORM_MAC
		// Apps started from Finder do not inherit the shell PATH; git must still find git-lfs.
		TArray<FString> Path = {FPaths::GetPath(GitBinary), TEXT("/opt/homebrew/bin"), TEXT("/usr/local/bin")};
		Path.Add(FPlatformMisc::GetEnvironmentVariable(TEXT("PATH")));
		Spec.SetEnvironment.Add(TEXT("PATH"), FString::Join(Path, TEXT(":")));
#endif
		if (Kind == ECallKind::BackgroundNetwork)
		{
			Spec.SetEnvironment.Add(TEXT("GCM_INTERACTIVE"), TEXT("never"));
			if (!bUserSshCommand)
			{
				Spec.SetEnvironment.Add(TEXT("GIT_SSH_COMMAND"), TEXT("ssh -o BatchMode=yes"));
			}
		}
	}

	FProcessResult FGitRepository::Run(const FGitCall& Call) const
	{
		FProcessSpec Spec;
		Spec.Executable = GitBinary;
		if (!Root.IsEmpty())
		{
			Spec.Arguments = {TEXT("-C"), Root};
		}
		if (!Call.bOptionalLocks)
		{
			Spec.Arguments.Add(TEXT("--no-optional-locks"));
		}
		if (Call.bLiteralPathspecs)
		{
			Spec.Arguments.Add(TEXT("--literal-pathspecs"));
		}
		Spec.Arguments.Append({TEXT("-c"), TEXT("core.quotepath=false"), TEXT("-c"), TEXT("color.ui=false")});
#if PLATFORM_WINDOWS
		Spec.Arguments.Append({TEXT("-c"), TEXT("core.longpaths=true")});
#endif
		if (Call.Kind == ECallKind::BackgroundNetwork)
		{
			Spec.Arguments.Append({TEXT("-c"), TEXT("credential.interactive=never")});
		}
		Spec.Arguments.Append(Call.Args);
		Spec.Stdin = Call.Stdin;
		Spec.Cancel = Call.Cancel;
		Spec.StdoutSink = Call.StdoutSink;
		switch (Call.Kind)
		{
		case ECallKind::LocalQuick:
			Spec.TotalTimeoutSeconds = 30.0;
			break;
		case ECallKind::LocalScan:
		case ECallKind::LocalMutate:
			Spec.IdleTimeoutSeconds = 120.0;
			break;
		case ECallKind::BackgroundNetwork:
			Spec.IdleTimeoutSeconds = 120.0;
			Spec.TotalTimeoutSeconds = 900.0;
			break;
		case ECallKind::Blob:
			Spec.TotalTimeoutSeconds = 600.0;
			break;
		case ECallKind::Hooked:
		case ECallKind::UserNetwork:
		case ECallKind::Integrate:
			// No limit: hooks, uploads and downloads can legitimately take a long time.
			break;
		}
		AddEnvironment(Spec, Call.Kind);
		return RunProcess(Spec);
	}

	FProcessResult FGitRepository::Run(ECallKind Kind, TArray<FString> Args) const
	{
		FGitCall Call;
		Call.Kind = Kind;
		Call.Args = MoveTemp(Args);
		return Run(Call);
	}

	FProcessResult FGitRepository::RunWithPathspecs(ECallKind Kind, TArray<FString> Args, const TArray<FString>& Paths) const
	{
		FGitCall Call;
		Call.Kind = Kind;
		Call.Args = MoveTemp(Args);
		Call.Args.Add(TEXT("--pathspec-from-file=-"));
		Call.Args.Add(TEXT("--pathspec-file-nul"));
		for (const FString& Path : Paths)
		{
			AppendUtf8(Call.Stdin, Path);
			Call.Stdin.Add(0);
		}
		return Run(Call);
	}

	FString FGitRepository::ToRelative(const FString& AbsolutePath) const
	{
		FString Normalized = FPaths::ConvertRelativePathToFull(AbsolutePath);
		FPaths::NormalizeFilename(Normalized);
		FString Relative;
		if (StripRoot(Normalized, Root, Relative) || StripRoot(Normalized, AliasRoot, Relative))
		{
			return Relative;
		}
		return FString();
	}

	FString FGitRepository::ToAbsolute(const FString& RelativePath) const
	{
		// The editor's spelling, so paths handed back to it match its own package filenames.
		const FString& Base = AliasRoot.IsEmpty() ? Root : AliasRoot;
		return RelativePath.IsEmpty() ? Base : Base + TEXT("/") + RelativePath;
	}

	FString FGitRepository::DescribeFailure(const FProcessResult& Result, const FString& What)
	{
		switch (Result.End)
		{
		case EProcessEnd::SpawnFailed:
			return FString::Printf(TEXT("%s failed: could not run Git. %s"), *What, *Result.Diagnostic);
		case EProcessEnd::Cancelled:
			return FString::Printf(TEXT("%s was cancelled."), *What);
		case EProcessEnd::IdleTimeout:
		case EProcessEnd::TotalTimeout:
			return FString::Printf(TEXT("%s timed out. %s"), *What, *Result.Diagnostic);
		case EProcessEnd::OutputLimit:
			return FString::Printf(TEXT("%s produced more output than expected. %s"), *What, *Result.Diagnostic);
		case EProcessEnd::Signaled:
			return FString::Printf(TEXT("%s stopped unexpectedly. %s"), *What, *Result.Diagnostic);
		case EProcessEnd::Exited:
			break;
		}
		const FString Errors = Result.ErrorText();
		auto Contains = [&Errors](const TCHAR* Text) { return Errors.Contains(Text, ESearchCase::IgnoreCase); };
		if (Contains(TEXT("terminal prompts disabled")) || Contains(TEXT("could not read Username")) || Contains(TEXT("Authentication failed")) ||
			Contains(TEXT("Permission denied (publickey")) || Contains(TEXT("HTTP Basic: Access denied")) || Contains(TEXT("Host key verification failed")))
		{
			return FString::Printf(TEXT("%s failed: Git could not sign in to the remote. Set up a credential helper (such as Git Credential Manager) or an SSH key, then try again."), *What);
		}
		if (Contains(TEXT("index.lock")))
		{
			return FString::Printf(TEXT("%s failed: another Git process is using this repository. Wait for it to finish; if none is running, delete .git/index.lock."), *What);
		}
		if (Contains(TEXT("Could not resolve host")) || Contains(TEXT("Connection refused")) || Contains(TEXT("Network is unreachable")) ||
			Contains(TEXT("Connection timed out")) || Contains(TEXT("Operation timed out")))
		{
			return FString::Printf(TEXT("%s failed: the remote could not be reached. Check your network connection."), *What);
		}
		FString FirstLines = Errors.Left(400);
		if (FirstLines.IsEmpty())
		{
			FirstLines = FString::Printf(TEXT("git exited with code %d."), Result.ExitCode);
		}
		return FString::Printf(TEXT("%s failed: %s"), *What, *FirstLines);
	}
}
