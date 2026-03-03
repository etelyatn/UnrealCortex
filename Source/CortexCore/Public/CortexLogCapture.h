#pragma once

#include "CoreMinimal.h"
#include "Misc/OutputDevice.h"

/**
 * Scoped log listener that captures FInstancedStruct deserialization warnings.
 *
 * Install on the stack around a load call. Warnings that match
 * "Unable to find serialized UScriptStruct" are captured and can be returned
 * to tool callers.
 */
class CORTEXCORE_API FCortexLogCapture final : public FOutputDevice
{
public:
	FCortexLogCapture()
	{
		GLog->AddOutputDevice(this);
	}

	~FCortexLogCapture()
	{
		GLog->RemoveOutputDevice(this);
	}

	virtual void Serialize(const TCHAR* V, ELogVerbosity::Type Verbosity, const FName& Category) override
	{
		(void)Category;

		const ELogVerbosity::Type VerbosityLevel =
			static_cast<ELogVerbosity::Type>(Verbosity & ELogVerbosity::VerbosityMask);
		if (V == nullptr || VerbosityLevel != ELogVerbosity::Warning)
		{
			return;
		}

		if (FCString::Strstr(V, TEXT("Unable to find serialized UScriptStruct")) != nullptr)
		{
			FScopeLock Lock(&CriticalSection);
			CapturedWarnings.Add(FString(V));
		}
	}

	virtual bool CanBeUsedOnAnyThread() const override
	{
		return true;
	}

	virtual bool CanBeUsedOnMultipleThreads() const override
	{
		return true;
	}

	TArray<FString> GetWarnings() const
	{
		FScopeLock Lock(&CriticalSection);
		return CapturedWarnings;
	}

	TArray<FString> GetWarnings(const FString& AssetPathFilter) const
	{
		FScopeLock Lock(&CriticalSection);
		TArray<FString> FilteredWarnings;
		FilteredWarnings.Reserve(CapturedWarnings.Num());

		for (const FString& Warning : CapturedWarnings)
		{
			if (Warning.Contains(AssetPathFilter))
			{
				FilteredWarnings.Add(Warning);
			}
		}

		return FilteredWarnings;
	}

	bool HasWarnings() const
	{
		FScopeLock Lock(&CriticalSection);
		return CapturedWarnings.Num() > 0;
	}

private:
	mutable FCriticalSection CriticalSection;
	TArray<FString> CapturedWarnings;
};
