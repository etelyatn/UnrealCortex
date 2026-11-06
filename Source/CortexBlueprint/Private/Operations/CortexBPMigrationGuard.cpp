#include "Operations/CortexBPMigrationGuard.h"

#include "CortexCommandRouter.h"
#include "CortexEngineCompat.h"
#include "CortexGraphFingerprint.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Engine/Blueprint.h"
#include "Operations/CortexBPSnapshotOps.h"
#include "Operations/CortexBPTypeUtils.h"
#include "Policies/CondensedJsonPrintPolicy.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "IO/IoHash.h"

namespace
{
	const TCHAR* const MigrationStateHashField = TEXT("migration_state_hash");
	const TCHAR* const GraphHashField = TEXT("graph_authoring_hash");
	const TCHAR* const GraphVersionField = TEXT("graph_authoring_version");
	const TCHAR* const StateVersionField = TEXT("migration_state_version");

	FString SerializeMigrationCompact(const TSharedRef<FJsonObject>& Object)
	{
		FString Text;
		const TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> Writer =
			TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Text);
		FJsonSerializer::Serialize(Object, Writer);
		return Text;
	}

	FString ComputeMigrationStateHash(UBlueprint* Blueprint)
	{
		FString Buffer;
		Buffer.Reserve(16384);
		Buffer += FString::Printf(TEXT("Parent: %s\n"), Blueprint->ParentClass ? *Blueprint->ParentClass->GetPathName() : TEXT("None"));

		for (const FBPVariableDescription& Variable : Blueprint->NewVariables)
		{
			TArray<FString> Metadata;
			for (const FBPVariableMetaDataEntry& Entry : Variable.MetaDataArray)
			{
				Metadata.Add(FString::Printf(TEXT("%s=%s"), *Entry.DataKey.ToString(), *Entry.DataValue));
			}
			Metadata.Sort();
			Buffer += FString::Printf(TEXT("Var: %s|%s|%s|%s|%llu|%s|%s|%s\n"),
				*Variable.VarName.ToString(),
				*Variable.VarGuid.ToString(),
				*CortexBPTypeUtils::FriendlyTypeName(Variable.VarType),
				*Variable.DefaultValue,
				static_cast<uint64>(Variable.PropertyFlags),
				*Variable.RepNotifyFunc.ToString(),
				*Variable.Category.ToString(),
				*FString::Join(Metadata, TEXT(";")));
		}

		// Compiled state migrations read and write: variable defaults and CDO values, component template
		// overrides and implemented interfaces.
		const TSet<FString> Sections = {TEXT("variables"), TEXT("cdo"), TEXT("components"), TEXT("interfaces")};
		FString SnapshotError;
		const TSharedPtr<FJsonObject> Snapshot = FCortexBPSnapshotOps::BuildSnapshot(Blueprint, Sections, SnapshotError);
		if (Snapshot.IsValid())
		{
			Buffer += SerializeMigrationCompact(Snapshot.ToSharedRef());
		}
		else
		{
			Buffer += FString::Printf(TEXT("SnapshotError: %s\n"), *SnapshotError);
		}

		FTCHARToUTF8 Utf8Buffer(*Buffer);
		const FIoHash Hash = FIoHash::HashBuffer(reinterpret_cast<const uint8*>(Utf8Buffer.Get()), Utf8Buffer.Length());
		return LexToString(Hash);
	}
}

TSharedPtr<FJsonObject> FCortexBPMigrationGuard::ComputeFingerprint(UBlueprint* Blueprint)
{
	TSharedPtr<FJsonObject> Fingerprint = MakeShared<FJsonObject>();
	const TSharedPtr<FJsonObject> Graph = FCortexGraphFingerprint::Compute(Blueprint);
	Fingerprint->SetNumberField(GraphVersionField, 1);
	Fingerprint->SetNumberField(StateVersionField, 1);
	Fingerprint->SetStringField(GraphHashField, Graph.IsValid() ? Graph->GetStringField(GraphHashField) : FString());
	Fingerprint->SetStringField(MigrationStateHashField, Blueprint ? ComputeMigrationStateHash(Blueprint) : FString());
	return Fingerprint;
}

bool FCortexBPMigrationGuard::CheckExpected(UBlueprint* Blueprint, const TSharedPtr<FJsonObject>& Expected, FCortexCommandResult& OutError)
{
	if (!Expected.IsValid())
	{
		OutError = FCortexCommandRouter::Error(CortexErrorCodes::InvalidField, TEXT("expected_fingerprint must be an object"));
		return false;
	}

	for (const auto& Pair : Expected->Values)
	{
		const FString Name = CortexEngineCompat::JsonKeyToString(Pair.Key);
		if (Name != MigrationStateHashField && Name != GraphHashField && Name != GraphVersionField && Name != StateVersionField)
		{
			OutError = FCortexCommandRouter::Error(CortexErrorCodes::InvalidField,
				FString::Printf(TEXT("Unknown field '%s' in expected_fingerprint"), *Name));
			return false;
		}
	}

	const TSharedPtr<FJsonObject> Current = ComputeFingerprint(Blueprint);
	auto Stale = [&Current, &OutError](const FString& Message)
	{
		TSharedPtr<FJsonObject> Details = MakeShared<FJsonObject>();
		Details->SetObjectField(TEXT("current_fingerprint"), Current);
		OutError = FCortexCommandRouter::Error(CortexErrorCodes::StalePrecondition, Message, Details);
		return false;
	};

	FString ExpectedState;
	if (!Expected->TryGetStringField(MigrationStateHashField, ExpectedState) || ExpectedState.IsEmpty())
	{
		return Stale(TEXT("expected_fingerprint needs migration_state_hash (read the current fingerprint from a dry run)"));
	}
	if (ExpectedState != Current->GetStringField(MigrationStateHashField))
	{
		return Stale(TEXT("migration_state_hash does not match the current Blueprint state"));
	}

	// Graphs are not part of migration_state_hash, so the graph hash and both versions are required too.
	FString ExpectedGraph;
	if (!Expected->TryGetStringField(GraphHashField, ExpectedGraph) || ExpectedGraph.IsEmpty())
	{
		return Stale(TEXT("expected_fingerprint needs graph_authoring_hash (read the current fingerprint from a dry run)"));
	}
	if (ExpectedGraph != Current->GetStringField(GraphHashField))
	{
		return Stale(TEXT("graph_authoring_hash does not match the current Blueprint graphs"));
	}

	double ExpectedGraphVersion = 0.0;
	if (!Expected->TryGetNumberField(GraphVersionField, ExpectedGraphVersion) || static_cast<int32>(ExpectedGraphVersion) != 1)
	{
		return Stale(TEXT("graph_authoring_version is missing or not supported (expected 1)"));
	}
	double ExpectedStateVersion = 0.0;
	if (!Expected->TryGetNumberField(StateVersionField, ExpectedStateVersion) || static_cast<int32>(ExpectedStateVersion) != 1)
	{
		return Stale(TEXT("migration_state_version is missing or not supported (expected 1)"));
	}
	return true;
}
