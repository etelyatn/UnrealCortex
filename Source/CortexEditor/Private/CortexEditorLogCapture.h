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
	int32 Cursor = -1;
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

	FCortexEditorLogResult GetRecentLogs(
		ELogVerbosity::Type MinSeverity,
		double SinceSeconds,
		int32 SinceCursor,
		const FString& CategoryFilter) const;

private:
	bool PassesSeverity(ELogVerbosity::Type Value, ELogVerbosity::Type MinSeverity) const;

	mutable FCriticalSection BufferCS;
	TArray<FCortexEditorLogEntry> Entries;
	int32 MaxEntries = 5000;
	int32 NextCursor = 1;
	bool bCapturing = false;
};
