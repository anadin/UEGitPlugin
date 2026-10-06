// Copyright UEGitPlugin contributors. Distributed under the MIT license.
#include "GitWorkspaceRepository.h"
#include "Misc/ScopeLock.h"
#include "String/LexFromString.h"

namespace GitWorkspace
{
namespace
{
const TCHAR* HistoryFormat = TEXT("--format=%H%x00%P%x00%an%x00%aI%x00%s");
bool HistoryOid(const FString& Oid)
{
    if (Oid.Len() != 40 && Oid.Len() != 64) return false;
    for (TCHAR C : Oid) if (!FChar::IsHexDigit(C)) return false;
    return true;
}
bool HistoryFields(const TArray<uint8>& Bytes, TArray<FString>& Fields)
{
    int32 Start = 0;
    for (int32 I = 0; I < Bytes.Num(); ++I) if (!Bytes[I])
    {
        FUTF8ToTCHAR Value(reinterpret_cast<const ANSICHAR*>(Bytes.GetData() + Start), I - Start);
        Fields.Emplace(Value.Length(), Value.Get()); Start = I + 1;
    }
    return Start == Bytes.Num();
}
FString HistoryDisplay(FString Value)
{ return Value.Replace(TEXT("\\"), TEXT("\\\\")).Replace(TEXT("\r"), TEXT("\\r")).Replace(TEXT("\n"), TEXT("\\n")).Replace(TEXT("\t"), TEXT("\\t")); }
FResult HistoryFailure(const FString& Error) { FResult R; R.Error = Error; return R; }
FResult HistoryText(const FString& Value)
{
    FResult R; R.Code = 0; FTCHARToUTF8 Bytes(*Value); R.Out.Append(reinterpret_cast<const uint8*>(Bytes.Get()), Bytes.Length()); return R;
}
}
bool ParseHistory(const TArray<uint8>& Bytes, TArray<FHistoryCommit>& Commits, FString& Error)
{
    Commits.Empty(); Error.Empty(); TArray<FString> Fields; TSet<FString> Seen;
    auto Fail = [&]() { Commits.Empty(); Error = TEXT("Cannot read complete commit history. Refresh history or inspect it externally."); return false; };
    if (!HistoryFields(Bytes, Fields) || Fields.Num() % 5) return Fail();
    for (int32 I = 0; I < Fields.Num(); I += 5)
    {
        FHistoryCommit Commit; Commit.Oid = Fields[I]; Commit.Author = Fields[I + 2]; Commit.Date = Fields[I + 3]; Commit.Subject = Fields[I + 4];
        if (!HistoryOid(Commit.Oid) || Seen.Contains(Commit.Oid)) return Fail();
        Fields[I + 1].ParseIntoArray(Commit.Parents, TEXT(" "), false);
        if (Fields[I + 1].IsEmpty()) Commit.Parents.Empty();
        for (const auto& Parent : Commit.Parents) if (!HistoryOid(Parent)) return Fail();
        FDateTime Date; if (!FDateTime::ParseIso8601(*Commit.Date, Date)) return Fail();
        Seen.Add(Commit.Oid); Commits.Add(MoveTemp(Commit));
    }
    return true;
}
FHistoryList FRepository::ListHistory(int32 Limit)
{
    FScopeLock Guard(&Mutex); FHistoryList R;
    if (Limit < 1 || Limit > 1000) { R.Error = TEXT("History supports 1 to 1000 recent commits."); return R; }
    const auto Local = RefreshInternal(); R.Root = Local.Root; R.Branch = Local.Branch; R.Head = Local.Head;
    if (!Local.bValid) { R.Error = Local.Error; return R; }
    if (Local.bUnborn) { R.bValid = true; return R; }
    // Freeze HEAD before walking it. External branch changes cannot change the
    // identity of the commits subsequently selected in this read-only window.
    const auto Log = Git({TEXT("log"), TEXT("-z"), TEXT("--no-notes"), TEXT("--encoding=UTF-8"), FString::Printf(TEXT("--max-count=%d"), Limit + 1), HistoryFormat, R.Head, TEXT("--")});
    if (!Log.Ok() || !ParseHistory(Log.Out, R.Commits, R.Error)) { if (!Log.Ok()) R.Error = Log.Error; return R; }
    if (R.Commits.IsEmpty() || R.Commits[0].Oid != R.Head) { R.Error = TEXT("History did not include the selected HEAD. Refresh history."); R.Commits.Empty(); return R; }
    R.bHasMore = R.Commits.Num() > Limit;
    if (R.bHasMore) R.Commits.SetNum(Limit);
    R.bValid = true; return R;
}
FString FCommitInspection::Text() const
{
    if (!bValid) return Error;
    FString R = TEXT("Commit: ") + Commit.Oid + TEXT("\nAuthor: ") + HistoryDisplay(Commit.Author) + TEXT("\nAuthored: ") + Commit.Date + TEXT("\n");
    R += Commit.Parents.IsEmpty() ? TEXT("Comparison: initial commit against the empty tree.\n") : TEXT("Comparison: parent ") + Commit.Parents[0] + TEXT(" → this commit.\n");
    if (Commit.Parents.Num() > 1) R += FString::Printf(TEXT("Merge with %d parents. Files and differences below compare the first parent only.\n"), Commit.Parents.Num());
    R += TEXT("Renames appear as deleted and added paths.\n\n") + Message;
    return R;
}
FCommitInspection FRepository::InspectCommit(const FString& Oid)
{ FScopeLock Guard(&Mutex); return InspectCommitInternal(Oid); }
FCommitInspection FRepository::InspectCommitInternal(const FString& Oid)
{
    FCommitInspection R;
    if (!HistoryOid(Oid)) { R.Error = TEXT("Select an exact commit ID from history."); return R; }
    const auto Top = Git({TEXT("rev-parse"), TEXT("--show-toplevel")});
    if (!Top.Ok()) { R.Error = Top.Error; return R; } Root = Top.Text().TrimEnd(); R.Root = Root;
    const auto Log = Git({TEXT("log"), TEXT("-z"), TEXT("--no-walk"), TEXT("--no-notes"), TEXT("--encoding=UTF-8"), HistoryFormat, Oid, TEXT("--")});
    TArray<FHistoryCommit> Commits;
    if (!Log.Ok() || !ParseHistory(Log.Out, Commits, R.Error) || Commits.Num() != 1 || Commits[0].Oid != Oid)
    { R.Error = TEXT("Cannot read this exact commit. ") + R.Error + Log.Error; return R; }
    R.Commit = MoveTemp(Commits[0]);
    const auto Message = Git({TEXT("show"), TEXT("--no-patch"), TEXT("--no-notes"), TEXT("--encoding=UTF-8"), TEXT("--format=%B"), Oid, TEXT("--")});
    if (!Message.Ok()) { R.Error = Message.Error; return R; }
    R.Message = Message.Text();
    if (R.Message.Len() > 65536) R.Message = R.Message.Left(65536) + TEXT("\n[Commit message shortened for display.]\n");
    TArray<FString> Args = R.Commit.Parents.IsEmpty() ? TArray<FString>{TEXT("diff-tree"), TEXT("--root"), TEXT("--no-commit-id"), TEXT("-r")} : TArray<FString>{TEXT("diff")};
    Args.Append({TEXT("--raw"), TEXT("--no-ext-diff"), TEXT("--no-textconv"), TEXT("--no-abbrev"), TEXT("--no-renames"), TEXT("-z")});
    if (!R.Commit.Parents.IsEmpty()) Args.Add(R.Commit.Parents[0]);
    Args.Append({Oid, TEXT("--")});
    const auto Diff = Git(Args); TArray<FIncomingChange> Validated; TArray<FString> Fields;
    if (!Diff.Ok() || !ParseIncomingChanges(Diff.Out, Validated, R.Error) || !HistoryFields(Diff.Out, Fields))
    { R.Error = TEXT("Cannot inspect the complete committed paths. ") + R.Error + Diff.Error; return R; }
    for (int32 I = 0; I < Validated.Num(); ++I)
    {
        TArray<FString> Header; Fields[I * 2].ParseIntoArray(Header, TEXT(" "));
        FHistoryChange Change; Change.Path = Validated[I].Path; Change.Status = Validated[I].Status;
        Change.OldMode = Validated[I].OldMode; Change.NewMode = Validated[I].NewMode; Change.OldOid = Header[2]; Change.NewOid = Header[3];
        R.Files.Add(MoveTemp(Change));
    }
    R.bValid = true; return R;
}
FResult FRepository::InspectCommitFile(const FString& Oid, const FString& Path)
{
    FScopeLock Guard(&Mutex);
    const auto Commit = InspectCommitInternal(Oid);
    if (!Commit.bValid) return HistoryFailure(Commit.Error);
    const auto* Change = Commit.Files.FindByPredicate([&](const auto& File) { return File.Path == Path; });
    if (!Change) return HistoryFailure(TEXT("Select a changed file from this exact commit."));
    FString Text = TEXT("COMMITTED FILE\nCommit: ") + Oid + TEXT("\nPath: ") + HistoryDisplay(Path) + TEXT("\nChange: ") + FString::Chr(Change->Status) + TEXT("\n\n");
    bool bMetadataOnly = Path.EndsWith(TEXT(".uasset"), ESearchCase::IgnoreCase) || Path.EndsWith(TEXT(".umap"), ESearchCase::IgnoreCase);
    for (int32 Side = 0; Side < 2; ++Side)
    {
        const FString Mode = Side ? Change->NewMode : Change->OldMode, Blob = Side ? Change->NewOid : Change->OldOid;
        Text += Side ? TEXT("AFTER\n") : TEXT("BEFORE\n");
        if (Mode == TEXT("000000")) { Text += TEXT("Absent\n\n"); continue; }
        Text += TEXT("Mode: ") + Mode + TEXT("\nGit object: ") + Blob + TEXT("\n");
        if (Mode == TEXT("160000")) { bMetadataOnly = true; Text += TEXT("Submodule commit. Inspect its own repository for source changes.\n\n"); continue; }
        if (Mode != TEXT("100644") && Mode != TEXT("100755")) bMetadataOnly = true;
        const auto SizeResult = Git({TEXT("cat-file"), TEXT("-s"), Blob}); int64 Size = -1;
        if (!SizeResult.Ok() || !LexTryParseString(Size, *SizeResult.Text().TrimEnd()) || Size < 0) return HistoryFailure(TEXT("Cannot read the committed object size. ") + SizeResult.Error);
        Text += FString::Printf(TEXT("Stored blob size: %lld bytes\n"), static_cast<long long>(Size));
        if (Size > 1024 * 1024) { bMetadataOnly = true; Text += TEXT("Large revision: metadata only (text preview limit is 1 MiB per version).\n\n"); continue; }
        const auto Data = Git({TEXT("cat-file"), TEXT("blob"), Blob});
        if (!Data.Ok() || Data.Out.Num() != Size) return HistoryFailure(TEXT("Cannot read the complete committed blob. ") + Data.Error);
        if (Data.Out.Contains(0)) bMetadataOnly = true;
        else if (Data.Text().StartsWith(TEXT("version https://git-lfs.github.com/spec/v1\n")))
        { bMetadataOnly = true; Text += TEXT("Stored Git LFS pointer:\n") + Data.Text(); }
        Text += TEXT("\n");
    }
    if (bMetadataOnly) return HistoryText(Text + TEXT("Revision metadata only. Asset visual diffs are not yet available; no LFS object was downloaded or package replaced.\n"));
    TArray<FString> Args = Commit.Commit.Parents.IsEmpty() ? TArray<FString>{TEXT("diff-tree"), TEXT("--root"), TEXT("--no-commit-id"), TEXT("-r")} : TArray<FString>{TEXT("diff")};
    Args.Append({TEXT("-p"), TEXT("--no-ext-diff"), TEXT("--no-textconv"), TEXT("--no-color"), TEXT("--no-renames"), TEXT("--full-index")});
    if (!Commit.Commit.Parents.IsEmpty()) Args.Add(Commit.Commit.Parents[0]);
    Args.Append({Oid, TEXT("--"), Path});
    const auto Diff = Git(Args); if (!Diff.Ok()) return HistoryFailure(Diff.Error);
    FString Patch = Diff.Text();
    if (Patch.Len() > 524288) Patch = Patch.Left(524288) + TEXT("\n[Text diff shortened for display. Inspect the full diff in an external client.]\n");
    return HistoryText(Text + TEXT("COMMITTED TEXT DIFF\n") + Patch);
}
}
