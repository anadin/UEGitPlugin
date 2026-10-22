// Copyright UEGitPlugin contributors. Distributed under the MIT license.
#if WITH_DEV_AUTOMATION_TESTS

#include "GitTestFixtures.h"
#include "Async/Async.h"
#include "Misc/SecureHash.h"

namespace
{
	constexpr EAutomationTestFlags ProcessTestFlags = EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter;

	/** git's own shell quoting for --sq-quote, used to check arguments arrive unchanged. */
	FString SqQuote(const FString& Argument)
	{
		return TEXT("'") + Argument.Replace(TEXT("'"), TEXT("'\\''")) + TEXT("'");
	}

	GitCore::FProcessSpec GitSpec(TArray<FString> Arguments)
	{
		GitCore::FProcessSpec Spec;
		Spec.Executable = GitTest::GitBinary();
		Spec.Arguments = MoveTemp(Arguments);
		Spec.TotalTimeoutSeconds = 60.0;
		return Spec;
	}

	/** A command that stays silent for Seconds. */
	GitCore::FProcessSpec SilentSpec(int32 Seconds)
	{
		GitCore::FProcessSpec Spec;
#if PLATFORM_WINDOWS
		// Git for Windows brings a shell with sleep. cmd.exe cannot take its /c command quoted.
		Spec.Executable = GitTest::GitBinary();
		Spec.Arguments = {TEXT("-c"), FString::Printf(TEXT("alias.workspace-test-sleep=!sleep %d"), Seconds), TEXT("workspace-test-sleep")};
#else
		Spec.Executable = TEXT("/bin/sleep");
		Spec.Arguments = {FString::FromInt(Seconds)};
#endif
		return Spec;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGitProcessArgumentsTest, "GitWorkspace.Process.ArgumentsAndStreams", ProcessTestFlags)
bool FGitProcessArgumentsTest::RunTest(const FString&)
{
	if (!TestFalse(TEXT("Git is installed"), GitTest::GitBinary().IsEmpty()))
	{
		return false;
	}
	const TArray<FString> Arguments = {
		TEXT("plain"), TEXT("two  spaces"), TEXT("it's"), TEXT("\"quoted\""), TEXT("trailing\\"), TEXT("back\\\\slashes\\\""),
		TEXT("-leading-dash"), TEXT("brackets[0]*?"), TEXT("unicodé ✓"), TEXT("")};
	TArray<FString> GitArgs = {TEXT("rev-parse"), TEXT("--sq-quote")};
	GitArgs.Append(Arguments);
	const GitCore::FProcessResult Quoted = GitCore::RunProcess(GitSpec(GitArgs));
	FString Expected;
	for (const FString& Argument : Arguments)
	{
		Expected += TEXT(" ") + SqQuote(Argument);
	}
	TestTrue(TEXT("Quoting succeeded"), Quoted.Succeeded());
	TestEqual(TEXT("Every argument arrives exactly as given"), Quoted.StdoutText().TrimEnd(), Expected);

	// stdin in, large binary-safe stdout back out.
	GitTest::FFixture Fixture;
	const FString Repo = Fixture.InitRepo();
	TArray<uint8> Data;
	Data.SetNumUninitialized(3 * 1024 * 1024 + 17);
	for (int32 Index = 0; Index < Data.Num(); ++Index)
	{
		Data[Index] = static_cast<uint8>((Index * 31) ^ (Index >> 7));
	}
	GitCore::FProcessSpec Hash = GitSpec({TEXT("-C"), Repo, TEXT("hash-object"), TEXT("-w"), TEXT("--stdin")});
	Hash.Stdin = Data;
	const GitCore::FProcessResult Written = GitCore::RunProcess(Hash);
	FSHA1 Sha;
	const FString Header = FString::Printf(TEXT("blob %d"), Data.Num());
	Sha.Update(reinterpret_cast<const uint8*>(TCHAR_TO_ANSI(*Header)), Header.Len());
	const uint8 Zero = 0;
	Sha.Update(&Zero, 1);
	Sha.Update(Data.GetData(), Data.Num());
	Sha.Final();
	uint8 Digest[20];
	Sha.GetHash(Digest);
	const FString Object = BytesToHex(Digest, 20).ToLower();
	TestEqual(TEXT("All stdin bytes reached git"), Written.StdoutText().TrimEnd(), Object);
	const GitCore::FProcessResult ReadBack = GitCore::RunProcess(GitSpec({TEXT("-C"), Repo, TEXT("cat-file"), TEXT("blob"), Object}));
	TestTrue(TEXT("Large binary stdout is exact"), ReadBack.Succeeded() && ReadBack.Stdout == Data);

	const GitCore::FProcessResult Failure = GitCore::RunProcess(GitSpec({TEXT("-C"), Repo, TEXT("rev-parse"), TEXT("--verify"), TEXT("no-such-ref")}));
	TestTrue(TEXT("Failure reports a non-zero exit"), Failure.Exited() && Failure.ExitCode != 0);
	TestTrue(TEXT("Errors stay on stderr"), Failure.Stdout.IsEmpty() && !Failure.Stderr.IsEmpty());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGitProcessStopTest, "GitWorkspace.Process.TimeoutsCancelAndLimits", ProcessTestFlags)
bool FGitProcessStopTest::RunTest(const FString&)
{
	GitCore::FProcessSpec Idle = SilentSpec(20);
	Idle.IdleTimeoutSeconds = 0.5;
	const GitCore::FProcessResult IdleResult = GitCore::RunProcess(Idle);
	TestEqual(TEXT("Silence triggers the idle timeout"), IdleResult.End, GitCore::EProcessEnd::IdleTimeout);
	TestTrue(TEXT("The process is stopped promptly"), IdleResult.Seconds < 8.0);

	GitCore::FProcessSpec Total = SilentSpec(20);
	Total.TotalTimeoutSeconds = 0.5;
	TestEqual(TEXT("The total limit applies"), GitCore::RunProcess(Total).End, GitCore::EProcessEnd::TotalTimeout);

	GitCore::FProcessSpec Cancellable = SilentSpec(30);
	Cancellable.Cancel = MakeShared<GitCore::FCancelToken, ESPMode::ThreadSafe>();
	GitCore::FCancelTokenPtr Token = Cancellable.Cancel;
	TFuture<GitCore::FProcessResult> Running = Async(EAsyncExecution::Thread, [Cancellable]() { return GitCore::RunProcess(Cancellable); });
	FPlatformProcess::Sleep(0.3f);
	Token->Cancel();
	const GitCore::FProcessResult Cancelled = Running.Get();
	TestEqual(TEXT("Cancellation stops the process"), Cancelled.End, GitCore::EProcessEnd::Cancelled);
	TestTrue(TEXT("Cancellation is prompt"), Cancelled.Seconds < 8.0);

	// A network-class call has no timeout: silence alone must not stop it.
	GitCore::FProcessSpec Patient = SilentSpec(2);
	const GitCore::FProcessResult Finished = GitCore::RunProcess(Patient);
	TestTrue(TEXT("A silent command without limits runs to completion"), Finished.Succeeded());

	GitTest::FFixture Fixture;
	const FString Repo = Fixture.InitRepo();
	TArray<uint8> Big;
	Big.SetNumZeroed(2 * 1024 * 1024);
	GitCore::FProcessSpec Hash = GitSpec({TEXT("-C"), Repo, TEXT("hash-object"), TEXT("-w"), TEXT("--stdin")});
	Hash.Stdin = Big;
	const FString Object = GitCore::RunProcess(Hash).StdoutText().TrimEnd();
	GitCore::FProcessSpec Limited = GitSpec({TEXT("-C"), Repo, TEXT("cat-file"), TEXT("blob"), Object});
	Limited.MaxStdoutBytes = 64 * 1024;
	TestEqual(TEXT("Output beyond the cap stops the process"), GitCore::RunProcess(Limited).End, GitCore::EProcessEnd::OutputLimit);

	int64 Streamed = 0;
	GitCore::FProcessSpec Sink = GitSpec({TEXT("-C"), Repo, TEXT("cat-file"), TEXT("blob"), Object});
	Sink.MaxStdoutBytes = 1;
	Sink.StdoutSink = [&Streamed](const uint8*, int32 Size)
	{
		Streamed += Size;
		return true;
	};
	const GitCore::FProcessResult SinkResult = GitCore::RunProcess(Sink);
	TestTrue(TEXT("A streaming consumer receives everything without the cap"), SinkResult.Succeeded() && Streamed == Big.Num() && SinkResult.Stdout.IsEmpty());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGitProcessEnvironmentTest, "GitWorkspace.Process.Environment", ProcessTestFlags)
bool FGitProcessEnvironmentTest::RunTest(const FString&)
{
	GitTest::FFixture Fixture;
	const FString Repo = Fixture.InitRepo();
	GitTest::Git(Repo, {TEXT("config"), TEXT("alias.showenv"), TEXT("!env")});

	// A variable of our own: Git for Windows sets HOME for its children when it is missing.
	const TCHAR* const Inherited = TEXT("GIT_WORKSPACE_TEST_INHERITED");
	FPlatformMisc::SetEnvironmentVar(Inherited, TEXT("from the editor"));
	GitCore::FProcessSpec Spec = GitSpec({TEXT("-C"), Repo, TEXT("showenv")});
	const FString Before = GitCore::RunProcess(Spec).StdoutText();
	Spec.SetEnvironment.Add(TEXT("GIT_WORKSPACE_TEST_VALUE"), TEXT("set by test"));
	Spec.UnsetEnvironment.Add(Inherited);
	const GitCore::FProcessResult Result = GitCore::RunProcess(Spec);
	FPlatformMisc::SetEnvironmentVar(Inherited, TEXT(""));
	const FString Environment = Result.StdoutText();
	TestTrue(TEXT("The child runs"), Result.Succeeded());
	TestTrue(TEXT("The child inherits the editor's variables"), Before.Contains(TEXT("GIT_WORKSPACE_TEST_INHERITED=from the editor")));
	TestTrue(TEXT("Overrides reach the child"), Environment.Contains(TEXT("GIT_WORKSPACE_TEST_VALUE=set by test")));
	TestFalse(TEXT("Removed variables are absent"), Environment.Contains(TEXT("GIT_WORKSPACE_TEST_INHERITED=")));

	// The repository layer scrubs variables that would redirect git elsewhere.
	GitTest::WriteFile(Repo, TEXT("file.txt"), TEXT("text\n"));
	FPlatformMisc::SetEnvironmentVar(TEXT("GIT_INDEX_FILE"), TEXT("/nonexistent/index"));
	const GitCore::FGitRepositoryPtr Repository = GitTest::OpenRepository(*this, Repo);
	const bool bStaged = Repository.IsValid() && Repository->Stage({TEXT("file.txt")}).bOk;
	FPlatformMisc::SetEnvironmentVar(TEXT("GIT_INDEX_FILE"), TEXT(""));
	TestTrue(TEXT("A stray GIT_INDEX_FILE does not redirect the repository"), bStaged);
	TestTrue(TEXT("The real index received the file"), GitTest::Output(GitTest::Git(Repo, {TEXT("ls-files"), TEXT("--stage"), TEXT("file.txt")})).Contains(TEXT("file.txt")));
	return true;
}

#endif
