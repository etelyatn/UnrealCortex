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

void FCortexEditorLogCapture::EnsureCapturing()
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
	// Set bCapturing = false inside the lock, then release before calling GLog.
	// Holding BufferCS while calling GLog->RemoveOutputDevice() can deadlock:
	// another thread may hold GLog's internal lock while trying to acquire BufferCS
	// via Serialize() -> AddEntry(), creating an ABBA lock-order cycle.
	{
		FScopeLock Lock(&BufferCS);
		if (!bCapturing || GLog == nullptr)
		{
			return;
		}
		bCapturing = false;
	}
	GLog->RemoveOutputDevice(this);
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
		EvictedThroughCursor = Entries[0].Cursor;
		Entries.RemoveAt(0);
	}
}

FCortexEditorLogResult FCortexEditorLogCapture::GetRecentLogs(
	ELogVerbosity::Type MinSeverity,
	double SinceSeconds,
	int32 SinceCursor,
	const FString& CategoryFilter,
	int32 Limit,
	int32 UntilCursor,
	bool bCountOnly) const
{
	FScopeLock Lock(&BufferCS);

	FCortexEditorLogResult Result;
	if (Entries.Num() == 0)
	{
		return Result;
	}

	const int32 LatestCursor = Entries.Last().Cursor;
	// The window is fixed by the caller's watermark, or by the newest retained row. Rows past a
	// pinned end are never attributed to this window.
	const int32 WindowEndCursor = (UntilCursor != INDEX_NONE) ? FMath::Min(UntilCursor, LatestCursor) : LatestCursor;

	Result.LatestCursor = LatestCursor;
	Result.WindowEndCursor = WindowEndCursor;
	Result.OldestAvailableCursor = Entries[0].Cursor;

	// A negative start cursor means "from the beginning"; loss is only real when the row at the
	// requested start was actually evicted from the front of the ring buffer.
	const int32 RequestedStartCursor = (SinceCursor >= 0) ? (SinceCursor + 1) : 1;
	Result.bLostBeforeCursor = RequestedStartCursor <= EvictedThroughCursor;

	// Time filtering anchors to the retained end of this window; -1 disables it entirely.
	double CutoffTimestamp = 0.0;
	bool bUseTimeFilter = false;
	if (SinceSeconds >= 0.0)
	{
		bUseTimeFilter = true;
		for (int32 Index = Entries.Num() - 1; Index >= 0; --Index)
		{
			if (Entries[Index].Cursor <= WindowEndCursor)
			{
				CutoffTimestamp = Entries[Index].Timestamp - SinceSeconds;
				break;
			}
		}
	}

	// A page always retains at least one row so a capped scan can never fail to advance.
	const int32 RetainLimit = bCountOnly ? 0 : FMath::Max(1, Limit);
	int32 LastConsumedCursor = SinceCursor;
	bool bWindowHasMore = false;

	// One pass over the retained rows: count matches and retain at most the page, without copying
	// the whole list and trimming it.
	for (const FCortexEditorLogEntry& Entry : Entries)
	{
		if (Entry.Cursor <= SinceCursor || Entry.Cursor > WindowEndCursor)
		{
			continue;
		}
		if (!CategoryFilter.IsEmpty() && Entry.Category != CategoryFilter)
		{
			continue;
		}
		if (bUseTimeFilter && Entry.Timestamp < CutoffTimestamp)
		{
			continue;
		}
		if (!PassesSeverity(Entry.Verbosity, MinSeverity))
		{
			continue;
		}

		++Result.MatchedCount;
		if (Result.Entries.Num() < RetainLimit)
		{
			Result.Entries.Add(Entry);
			LastConsumedCursor = Entry.Cursor;
		}
		else
		{
			bWindowHasMore = true;
		}
	}

	if (bCountOnly)
	{
		// A count-only watermark never carries a page.
		Result.NextCursor = WindowEndCursor;
		Result.bHasMore = false;
	}
	else
	{
		// A finished scan advances through any nonmatching tail; a capped page stops at the last
		// consumed match so the omitted match is never consumed.
		Result.NextCursor = bWindowHasMore ? LastConsumedCursor : WindowEndCursor;
		Result.bHasMore = bWindowHasMore;
	}

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
