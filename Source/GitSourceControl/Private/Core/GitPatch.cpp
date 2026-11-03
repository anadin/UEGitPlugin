// Copyright UEGitPlugin contributors. Distributed under the MIT license.
#include "GitPatch.h"

#include "Misc/FileHelper.h"

namespace GitCore
{
	namespace
	{
		struct FPatchLine
		{
			/** ' ' (context), '-' or '+'. */
			TCHAR Kind = TEXT(' ');
			/** The line after its marker, with any carriage return. */
			FString Text;
			/** Followed by "\ No newline at end of file". */
			bool bNoNewline = false;
			/** A removed line's number in the old version, an added line's in the new one. */
			int32 Number = 0;
		};

		struct FPatchHunk
		{
			int32 OldStart = 0;
			int32 OldCount = 0;
			int32 NewStart = 0;
			int32 NewCount = 0;
			TArray<FPatchLine> Lines;
		};

		bool ReadNumber(const FString& Text, int32& Out)
		{
			if (Text.IsEmpty() || Text.Len() > 9)
			{
				return false;
			}
			for (const TCHAR Character : Text)
			{
				if (!FChar::IsDigit(Character))
				{
					return false;
				}
			}
			Out = FCString::Atoi(*Text);
			return true;
		}

		/** "-12,4" or "+7" (a count of one is left out). */
		bool ReadRange(const FString& Field, TCHAR Sign, int32& OutStart, int32& OutCount)
		{
			if (Field.IsEmpty() || Field[0] != Sign)
			{
				return false;
			}
			FString Start = Field.Mid(1);
			FString Count = TEXT("1");
			int32 Comma = INDEX_NONE;
			if (Start.FindChar(TEXT(','), Comma))
			{
				Count = Start.Mid(Comma + 1);
				Start.LeftInline(Comma);
			}
			return ReadNumber(Start, OutStart) && ReadNumber(Count, OutCount);
		}

		/** "@@ -12,4 +12,5 @@ Section". */
		bool ReadHunkHeader(const FString& Line, FPatchHunk& Out)
		{
			if (!Line.StartsWith(TEXT("@@ -")))
			{
				return false;
			}
			const int32 End = Line.Find(TEXT(" @@"), ESearchCase::CaseSensitive, ESearchDir::FromStart, 3);
			FString Old;
			FString New;
			return End != INDEX_NONE && Line.Mid(3, End - 3).Split(TEXT(" "), &Old, &New) &&
				ReadRange(Old, TEXT('-'), Out.OldStart, Out.OldCount) && ReadRange(New, TEXT('+'), Out.NewStart, Out.NewCount);
		}

		/** "+++ b/Config/Game.ini" as the other side's line, "--- a/Config/Game.ini". Quoted names stay quoted. */
		FString MirrorFileLine(const FString& Line)
		{
			const bool bNewSide = Line.StartsWith(TEXT("+++ "));
			FString Name = Line.Mid(4);
			const bool bQuoted = Name.StartsWith(TEXT("\""));
			if (bQuoted)
			{
				Name.RightChopInline(1);
			}
			if (!Name.StartsWith(bNewSide ? TEXT("b/") : TEXT("a/")))
			{
				return FString();
			}
			return FString(bNewSide ? TEXT("--- ") : TEXT("+++ ")) + (bQuoted ? TEXT("\"") : TEXT("")) + (bNewSide ? TEXT("a/") : TEXT("b/")) + Name.Mid(2);
		}

		/**
		 * Only the last line of a version may lack a newline. Cutting a diff can leave lines after one that did, on
		 * one side only: that side gets the newline, so a context line becomes a removed line and an added line.
		 */
		TArray<FPatchLine> FixNewlines(const TArray<FPatchLine>& Lines)
		{
			int32 LastOld = INDEX_NONE;
			int32 LastNew = INDEX_NONE;
			for (int32 Index = 0; Index < Lines.Num(); ++Index)
			{
				LastOld = Lines[Index].Kind != TEXT('+') ? Index : LastOld;
				LastNew = Lines[Index].Kind != TEXT('-') ? Index : LastNew;
			}
			TArray<FPatchLine> Fixed;
			for (int32 Index = 0; Index < Lines.Num(); ++Index)
			{
				FPatchLine Line = Lines[Index];
				if (!Line.bNoNewline)
				{
					Fixed.Add(MoveTemp(Line));
					continue;
				}
				const bool bEndsOld = Line.Kind == TEXT('+') || Index == LastOld;
				const bool bEndsNew = Line.Kind == TEXT('-') || Index == LastNew;
				if (Line.Kind == TEXT(' ') && bEndsOld != bEndsNew)
				{
					FPatchLine Added = Line;
					Added.Kind = TEXT('+');
					Added.bNoNewline = bEndsNew;
					Line.Kind = TEXT('-');
					Line.bNoNewline = bEndsOld;
					Fixed.Add(MoveTemp(Line));
					Fixed.Add(MoveTemp(Added));
					continue;
				}
				Line.bNoNewline = bEndsOld && bEndsNew;
				Fixed.Add(MoveTemp(Line));
			}
			return Fixed;
		}
	}

	bool BuildLinePatch(const FString& Diff, const FLineSelection& Selection, bool bReverse, FString& OutPatch, FString& OutError)
	{
		OutPatch.Reset();
		TArray<FString> Raw;
		Diff.ParseIntoArray(Raw, TEXT("\n"), false);
		// The last line ends with a newline too: nothing follows it.
		if (!Raw.IsEmpty() && Raw.Last().IsEmpty())
		{
			Raw.Pop();
		}
		if (Raw.IsEmpty() || !Raw[0].StartsWith(TEXT("diff --git ")))
		{
			OutError = Raw.IsEmpty() || !Raw[0].StartsWith(TEXT("diff --"))
				? TEXT("Git's diff of this file could not be read.") : TEXT("Resolve the conflicts in this file before staging its lines.");
			return false;
		}

		// File header: kept, except what would change more than the picked lines.
		TArray<FString> Header = {Raw[0]};
		FString OldFileLine;
		FString NewFileLine;
		bool bNewFile = false;
		bool bDeletedFile = false;
		int32 Index = 1;
		for (; Index < Raw.Num() && !Raw[Index].StartsWith(TEXT("@@")); ++Index)
		{
			const FString& Line = Raw[Index];
			const bool bModeLine = Line.StartsWith(TEXT("new file mode ")) || Line.StartsWith(TEXT("deleted file mode ")) ||
				Line.StartsWith(TEXT("old mode ")) || Line.StartsWith(TEXT("new mode ")) || Line.StartsWith(TEXT("index "));
			if (bModeLine && Line.EndsWith(TEXT(" 160000")))
			{
				OutError = TEXT("A submodule is staged as a whole.");
				return false;
			}
			if (Line.StartsWith(TEXT("Binary files ")) || Line.StartsWith(TEXT("GIT binary patch")))
			{
				OutError = TEXT("A binary file cannot be staged line by line.");
				return false;
			}
			if (Line.StartsWith(TEXT("new file mode ")) || Line.StartsWith(TEXT("deleted file mode ")))
			{
				bNewFile |= Line.StartsWith(TEXT("new"));
				bDeletedFile |= Line.StartsWith(TEXT("deleted"));
				Header.Add(Line);
			}
			else if (Line.StartsWith(TEXT("--- ")) || Line.StartsWith(TEXT("+++ ")))
			{
				(Line.StartsWith(TEXT("-")) ? OldFileLine : NewFileLine) = Line;
				Header.Add(Line);
			}
			else if (!bModeLine)
			{
				// Renames, copies, or a second file.
				OutError = Line.StartsWith(TEXT("rename ")) || Line.StartsWith(TEXT("copy ")) || Line.Contains(TEXT("similarity index"))
					? TEXT("A renamed file is staged as a whole.") : TEXT("Git's diff of this file could not be read.");
				return false;
			}
			// Picked lines change the content only: the index line goes, and so does a change of mode.
		}

		// Hunks, read by their line counts.
		TArray<FPatchHunk> Hunks;
		while (Index < Raw.Num())
		{
			FPatchHunk& Hunk = Hunks.AddDefaulted_GetRef();
			if (!ReadHunkHeader(Raw[Index++], Hunk))
			{
				OutError = TEXT("Git's diff of this file could not be read.");
				return false;
			}
			int32 OldLeft = Hunk.OldCount;
			int32 NewLeft = Hunk.NewCount;
			int32 OldNumber = Hunk.OldStart;
			int32 NewNumber = Hunk.NewStart;
			while (OldLeft > 0 || NewLeft > 0)
			{
				if (Index >= Raw.Num())
				{
					OutError = TEXT("Git's diff of this file ended early.");
					return false;
				}
				const FString& Text = Raw[Index++];
				FPatchLine& Line = Hunk.Lines.AddDefaulted_GetRef();
				// Git prints an empty context line without its space when diff.suppressBlankEmpty is set.
				Line.Kind = Text.IsEmpty() ? TEXT(' ') : Text[0];
				Line.Text = Text.Mid(1);
				const bool bOld = Line.Kind == TEXT(' ') || Line.Kind == TEXT('-');
				const bool bNew = Line.Kind == TEXT(' ') || Line.Kind == TEXT('+');
				if ((!bOld && !bNew) || (bOld && OldLeft == 0) || (bNew && NewLeft == 0))
				{
					OutError = TEXT("Git's diff of this file could not be read.");
					return false;
				}
				OldLeft -= bOld ? 1 : 0;
				NewLeft -= bNew ? 1 : 0;
				Line.Number = Line.Kind == TEXT('+') ? NewNumber : OldNumber;
				OldNumber += bOld ? 1 : 0;
				NewNumber += bNew ? 1 : 0;
				// "\ No newline at end of file" belongs to the line before it.
				if (Index < Raw.Num() && Raw[Index].StartsWith(TEXT("\\")))
				{
					Line.bNoNewline = true;
					++Index;
				}
			}
		}

		// Applied forwards the patch starts from the old version, in reverse from the new one. Changes left out
		// keep that version as it is: forwards, a removed line stays as context and an added line is dropped;
		// in reverse, an added line stays as context and a removed line is dropped.
		const TCHAR KeptAsContext = bReverse ? TEXT('+') : TEXT('-');
		bool bAnyPicked = false;
		bool bAllPicked = true;
		for (FPatchHunk& Hunk : Hunks)
		{
			TArray<FPatchLine> Lines;
			for (int32 Start = 0; Start < Hunk.Lines.Num();)
			{
				if (Hunk.Lines[Start].Kind == TEXT(' '))
				{
					Lines.Add(Hunk.Lines[Start++]);
					continue;
				}
				// A run of changes: removed lines, then the added lines that replace them. The n-th removed line and
				// the n-th added one are taken as an edit and kept together, so a change left out stays beside its
				// partner instead of moving past every added line.
				TArray<FPatchLine*> Removed;
				TArray<FPatchLine*> Added;
				for (; Start < Hunk.Lines.Num() && Hunk.Lines[Start].Kind != TEXT(' '); ++Start)
				{
					(Hunk.Lines[Start].Kind == TEXT('-') ? Removed : Added).Add(&Hunk.Lines[Start]);
				}
				for (int32 Pair = 0; Pair < FMath::Max(Removed.Num(), Added.Num()); ++Pair)
				{
					for (FPatchLine* Line : {Removed.IsValidIndex(Pair) ? Removed[Pair] : nullptr, Added.IsValidIndex(Pair) ? Added[Pair] : nullptr})
					{
						if (!Line)
						{
							continue;
						}
						const bool bPicked = (Line->Kind == TEXT('-') ? Selection.Removed : Selection.Added).Contains(Line->Number);
						bAnyPicked |= bPicked;
						bAllPicked &= bPicked;
						if (!bPicked && Line->Kind == KeptAsContext)
						{
							Line->Kind = TEXT(' ');
						}
						if (bPicked || Line->Kind == TEXT(' '))
						{
							Lines.Add(*Line);
						}
					}
				}
			}
			Hunk.Lines = FixNewlines(Lines);
		}
		if (!bAnyPicked)
		{
			return true;
		}

		// A file created or deleted whole stays so. Cut down, it is changed in place instead, and both sides name it:
		// forwards, a new file is created with the picked lines only; in reverse, a deleted file comes back with them.
		const bool bKeepNew = bNewFile && (bAllPicked || !bReverse);
		const bool bKeepDeleted = bDeletedFile && (bAllPicked || bReverse);
		for (const FString& Line : Header)
		{
			if ((Line.StartsWith(TEXT("new file mode ")) && !bKeepNew) || (Line.StartsWith(TEXT("deleted file mode ")) && !bKeepDeleted))
			{
				continue;
			}
			FString Out = Line;
			if (Line == TEXT("--- /dev/null") && bNewFile && !bKeepNew)
			{
				Out = MirrorFileLine(NewFileLine);
			}
			else if (Line == TEXT("+++ /dev/null") && bDeletedFile && !bKeepDeleted)
			{
				Out = MirrorFileLine(OldFileLine);
			}
			if (Out.IsEmpty())
			{
				OutPatch.Reset();
				OutError = TEXT("Git's diff of this file could not be read.");
				return false;
			}
			OutPatch += Out + TEXT("\n");
		}

		// Each hunk starts where the hunks before it left the version being written.
		int32 Shift = 0;
		for (const FPatchHunk& Hunk : Hunks)
		{
			int32 OldCount = 0;
			int32 NewCount = 0;
			for (const FPatchLine& Line : Hunk.Lines)
			{
				OldCount += Line.Kind != TEXT('+') ? 1 : 0;
				NewCount += Line.Kind != TEXT('-') ? 1 : 0;
			}
			if (!Hunk.Lines.ContainsByPredicate([](const FPatchLine& Line) { return Line.Kind != TEXT(' '); }))
			{
				continue;
			}
			// A side without lines names the line before the hunk.
			int32 OldStart = Hunk.OldStart;
			int32 NewStart = Hunk.NewStart;
			if (bReverse)
			{
				const int32 First = (Hunk.NewCount > 0 ? Hunk.NewStart : Hunk.NewStart + 1) + Shift;
				OldStart = OldCount > 0 ? First : First - 1;
				Shift += OldCount - NewCount;
			}
			else
			{
				const int32 First = (Hunk.OldCount > 0 ? Hunk.OldStart : Hunk.OldStart + 1) + Shift;
				NewStart = NewCount > 0 ? First : First - 1;
				Shift += NewCount - OldCount;
			}
			OutPatch += FString::Printf(TEXT("@@ -%d,%d +%d,%d @@\n"), OldStart, OldCount, NewStart, NewCount);
			for (const FPatchLine& Line : Hunk.Lines)
			{
				OutPatch.AppendChar(Line.Kind);
				OutPatch += Line.Text;
				OutPatch += Line.bNoNewline ? TEXT("\n\\ No newline at end of file\n") : TEXT("\n");
			}
		}
		return true;
	}

	FString NewFileDiff(const TArray<uint8>& Bytes)
	{
		// Git's own test: a NUL byte near the start means binary.
		const int32 Sniff = FMath::Min(Bytes.Num(), 8000);
		for (int32 Index = 0; Index < Sniff; ++Index)
		{
			if (Bytes[Index] == 0)
			{
				return TEXT("Binary files /dev/null and b/file differ\n");
			}
		}
		FString Content;
		FFileHelper::BufferToString(Content, Bytes.GetData(), Bytes.Num());
		TArray<FString> Lines;
		Content.ParseIntoArray(Lines, TEXT("\n"), false);
		if (!Lines.IsEmpty() && Lines.Last().IsEmpty())
		{
			Lines.Pop();
		}
		FString Diff = FString::Printf(TEXT("@@ -0,0 +1,%d @@\n"), Lines.Num());
		for (const FString& Line : Lines)
		{
			Diff += TEXT("+") + Line + TEXT("\n");
		}
		return Diff;
	}

	bool IsValidUtf8(const uint8* Data, int32 Size)
	{
		for (int32 Index = 0; Index < Size;)
		{
			const uint8 Lead = Data[Index];
			int32 Extra = 0;
			uint32 CodePoint = 0;
			uint32 Smallest = 0;
			if (Lead < 0x80)
			{
				++Index;
				continue;
			}
			if ((Lead & 0xE0) == 0xC0)
			{
				Extra = 1;
				CodePoint = Lead & 0x1F;
				Smallest = 0x80;
			}
			else if ((Lead & 0xF0) == 0xE0)
			{
				Extra = 2;
				CodePoint = Lead & 0x0F;
				Smallest = 0x800;
			}
			else if ((Lead & 0xF8) == 0xF0)
			{
				Extra = 3;
				CodePoint = Lead & 0x07;
				Smallest = 0x10000;
			}
			else
			{
				return false;
			}
			if (Index + Extra >= Size)
			{
				return false;
			}
			for (int32 Offset = 1; Offset <= Extra; ++Offset)
			{
				const uint8 Next = Data[Index + Offset];
				if ((Next & 0xC0) != 0x80)
				{
					return false;
				}
				CodePoint = (CodePoint << 6) | (Next & 0x3F);
			}
			// Overlong forms, surrogates and values past Unicode's last code point do not round-trip.
			if (CodePoint < Smallest || CodePoint > 0x10FFFF || (CodePoint >= 0xD800 && CodePoint <= 0xDFFF))
			{
				return false;
			}
			Index += Extra + 1;
		}
		return true;
	}
}
