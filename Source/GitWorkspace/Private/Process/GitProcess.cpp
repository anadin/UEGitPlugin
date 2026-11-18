// Copyright UEGitPlugin contributors. Distributed under the MIT license.
#include "GitProcess.h"

namespace GitCore
{
	FString DecodeUtf8(const uint8* Data, int32 Size)
	{
		if (Size <= 0)
		{
			return FString();
		}
		const FUTF8ToTCHAR Converted(reinterpret_cast<const ANSICHAR*>(Data), Size);
		return FString(Converted.Length(), Converted.Get());
	}

	void AppendUtf8(TArray<uint8>& Bytes, FStringView Text)
	{
		const FTCHARToUTF8 Converted(Text.GetData(), Text.Len());
		Bytes.Append(reinterpret_cast<const uint8*>(Converted.Get()), Converted.Length());
	}

	FString FProcessResult::StdoutText() const
	{
		return DecodeUtf8(Stdout);
	}

	FString FProcessResult::StderrText() const
	{
		return DecodeUtf8(Stderr);
	}

	FString FProcessResult::ErrorText() const
	{
		FString Text = StderrText().TrimStartAndEnd();
		if (!Diagnostic.IsEmpty())
		{
			Text = Text.IsEmpty() ? Diagnostic : Diagnostic + TEXT("\n") + Text;
		}
		if (Text.IsEmpty() && End == EProcessEnd::Exited && ExitCode != 0)
		{
			Text = StdoutText().TrimStartAndEnd();
		}
		return Text;
	}

	namespace ProcessDetail
	{
		static bool SameVariable(FStringView A, FStringView B)
		{
#if PLATFORM_WINDOWS
			return A.Equals(B, ESearchCase::IgnoreCase);
#else
			return A.Equals(B, ESearchCase::CaseSensitive);
#endif
		}

		TArray<FString> BuildEnvironment(const TArray<FString>& Inherited, const FProcessSpec& Spec)
		{
			TArray<FString> Result;
			Result.Reserve(Inherited.Num() + Spec.SetEnvironment.Num());
			for (const FString& Entry : Inherited)
			{
				int32 Equals = INDEX_NONE;
				// Windows keeps per-drive entries such as "=C:=C:\\" that start with '='.
				if (!Entry.FindChar(TEXT('='), Equals) || Equals == 0)
				{
					Result.Add(Entry);
					continue;
				}
				const FStringView Name = FStringView(Entry).Left(Equals);
				bool bDrop = false;
				for (const FString& Unset : Spec.UnsetEnvironment)
				{
					bDrop |= SameVariable(Name, Unset);
				}
				for (const TPair<FString, FString>& Set : Spec.SetEnvironment)
				{
					bDrop |= SameVariable(Name, Set.Key);
				}
				if (!bDrop)
				{
					Result.Add(Entry);
				}
			}
			for (const TPair<FString, FString>& Set : Spec.SetEnvironment)
			{
				Result.Add(Set.Key + TEXT("=") + Set.Value);
			}
			return Result;
		}

		FString QuoteWindowsArgument(const FString& Argument)
		{
			// CRT rules: backslashes are literal unless they precede a quote, where
			// they must be doubled; the closing quote doubles any trailing run.
			FString Quoted = TEXT("\"");
			int32 Backslashes = 0;
			for (const TCHAR Character : Argument)
			{
				if (Character == TEXT('\\'))
				{
					++Backslashes;
					continue;
				}
				if (Character == TEXT('"'))
				{
					Quoted += FString::ChrN(Backslashes * 2 + 1, TEXT('\\'));
				}
				else
				{
					Quoted += FString::ChrN(Backslashes, TEXT('\\'));
				}
				Backslashes = 0;
				Quoted.AppendChar(Character);
			}
			Quoted += FString::ChrN(Backslashes * 2, TEXT('\\'));
			Quoted += TEXT("\"");
			return Quoted;
		}
	}
}
