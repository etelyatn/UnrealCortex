#pragma once

#include "CoreMinimal.h"
#include "Misc/OutputDevice.h"

struct FCortexEditorLogEntry
{
	int32 Cursor = 0;
	double Timestamp = 0.0;
	ELogVerbosity::Type Verbosity = ELogVerbosity::Log;
	FString Category;
	FString Message;
};

struct FCortexEditorLogResult
{
	TArray<FCortexEditorLogEntry> Entries;
	/** Cursor of the last retained row in this page; the continuation start. */
	int32 NextCursor = -1;
	/** Cursor of the newest retained row (may be past a pinned window end). */
	int32 LatestCursor = -1;
	/** Inclusive end of the window scanned for this page. */
	int32 WindowEndCursor = -1;
	/** Oldest retained cursor; anything below it was evicted from the ring buffer. */
	int32 OldestAvailableCursor = -1;
	/** Matching rows in the window, whether or not they fit the page. */
	int32 MatchedCount = 0;
	/** True when matching rows remain beyond the page. */
	bool bHasMore = false;
	/** True when rows before the requested window start were evicted. */
	bool bLostBeforeCursor = false;
};

class FCortexEditorLogCapture : public FOutputDevice
{
public:
	explicit FCortexEditorLogCapture(int32 InMaxEntries = 5000);
	virtual ~FCortexEditorLogCapture() override;

	void StartCapture();
	void StopCapture();

	virtual void Serialize(const TCHAR* Message, ELogVerbosity::Type Verbosity, const FName& Category) override;

	void AddEntry(
		ELogVerbosity::Type Verbosity,
		const FString& Category,
		const FString& Message,
		double Timestamp,
		int32 ForcedCursor = INDEX_NONE);

	void EnsureCapturing();

	FCortexEditorLogResult GetRecentLogs(
		ELogVerbosity::Type MinSeverity,
		double SinceSeconds,
		int32 SinceCursor,
		const FString& CategoryFilter,
		int32 Limit = 200,
		int32 UntilCursor = INDEX_NONE,
		bool bCountOnly = false) const;

private:
	bool PassesSeverity(ELogVerbosity::Type Value, ELogVerbosity::Type MinSeverity) const;

	mutable FCriticalSection BufferCS;
	TArray<FCortexEditorLogEntry> Entries;
	int32 MaxEntries = 5000;
	int32 NextCursor = 1;
	int32 EvictedThroughCursor = 0;
	bool bCapturing = false;
};
