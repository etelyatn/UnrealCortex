#include "CortexEditorLogCapture.h"
#include "Misc/ScopeLock.h"

FCortexEditorLogCapture::FCortexEditorLogCapture(int32 InMaxEntries)
	: MaxEntries(FMath::Max(1, InMaxEntries))
{
}

FCortexEditorLogCapture::~FCortexEditorLogCapture()
{
	StopCapture();
}

void FCortexEditorLogCapture::StartCapture()
{
	FScopeLock Lock(&BufferCS);
	if (!bCapturing && GLog != nullptr)
	{
		GLog->AddOutputDevice(this);
		bCapturing = true;
	}
}

void FCortexEditorLogCapture::StopCapture()
{
	FScopeLock Lock(&BufferCS);
	if (bCapturing && GLog != nullptr)
	{
		GLog->RemoveOutputDevice(this);
		bCapturing = false;
	}
}

void FCortexEditorLogCapture::Serialize(const TCHAR* Message, ELogVerbosity::Type Verbosity, const FName& Category)
{
	AddEntry(Verbosity, Category.ToString(), Message, FPlatformTime::Seconds());
}

void FCortexEditorLogCapture::AddEntry(
	ELogVerbosity::Type Verbosity,
	const FString& Category,
	const FString& Message,
	double Timestamp,
	int32 ForcedCursor)
{
	FScopeLock Lock(&BufferCS);

	FCortexEditorLogEntry Entry;
	Entry.Cursor = (ForcedCursor != INDEX_NONE) ? ForcedCursor : NextCursor++;
	Entry.Timestamp = Timestamp;
	Entry.Verbosity = Verbosity;
	Entry.Category = Category;
	Entry.Message = Message;
	Entries.Add(MoveTemp(Entry));

	if (ForcedCursor != INDEX_NONE)
	{
		NextCursor = FMath::Max(NextCursor, ForcedCursor + 1);
	}

	while (Entries.Num() > MaxEntries)
	{
		Entries.RemoveAt(0);
	}
}

FCortexEditorLogResult FCortexEditorLogCapture::GetRecentLogs(
	ELogVerbosity::Type MinSeverity,
	double SinceSeconds,
	int32 SinceCursor,
	const FString& CategoryFilter) const
{
	FScopeLock Lock(&BufferCS);

	FCortexEditorLogResult Result;
	if (Entries.Num() == 0)
	{
		return Result;
	}

	const double LatestTimestamp = Entries.Last().Timestamp;
	const double CutoffTimestamp = LatestTimestamp - SinceSeconds;

	for (const FCortexEditorLogEntry& Entry : Entries)
	{
		if (Entry.Cursor <= SinceCursor)
		{
			continue;
		}
		if (!CategoryFilter.IsEmpty() && Entry.Category != CategoryFilter)
		{
			continue;
		}
		if (Entry.Timestamp < CutoffTimestamp)
		{
			continue;
		}
		if (!PassesSeverity(Entry.Verbosity, MinSeverity))
		{
			continue;
		}
		Result.Entries.Add(Entry);
	}

	Result.Cursor = Entries.Last().Cursor;
	return Result;
}

bool FCortexEditorLogCapture::PassesSeverity(ELogVerbosity::Type Value, ELogVerbosity::Type MinSeverity) const
{
	if (MinSeverity == ELogVerbosity::Error)
	{
		return Value == ELogVerbosity::Error || Value == ELogVerbosity::Fatal;
	}
	if (MinSeverity == ELogVerbosity::Warning)
	{
		return Value == ELogVerbosity::Warning || Value == ELogVerbosity::Error || Value == ELogVerbosity::Fatal;
	}
	return true;
}
