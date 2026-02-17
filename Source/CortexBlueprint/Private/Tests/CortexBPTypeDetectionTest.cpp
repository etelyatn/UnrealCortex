// Copyright Andrei Sudarikov. All Rights Reserved.

#include "Misc/AutomationTest.h"
#include "CortexBPCommandHandler.h"
#include "CortexTypes.h"

// --------------------------------------------------------------------------
// get_info type detection
// --------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexBPTypeDetectionGetInfoComponentTest,
	"Cortex.Blueprint.TypeDetection.GetInfo.Component",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexBPTypeDetectionGetInfoComponentTest::RunTest(const FString& Parameters)
{
	FCortexBPCommandHandler Handler;

	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("asset_path"), TEXT("/Game/Blueprints/BP_SimpleComponent"));
	FCortexCommandResult Result = Handler.Execute(TEXT("get_info"), Params);

	TestTrue(TEXT("get_info should succeed"), Result.bSuccess);
	if (Result.Data.IsValid())
	{
		FString Type;
		Result.Data->TryGetStringField(TEXT("type"), Type);
		TestEqual(TEXT("BP_SimpleComponent type should be Component"), Type, TEXT("Component"));
	}

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexBPTypeDetectionGetInfoAllTypesTest,
	"Cortex.Blueprint.TypeDetection.GetInfo.AllTypes",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexBPTypeDetectionGetInfoAllTypesTest::RunTest(const FString& Parameters)
{
	FCortexBPCommandHandler Handler;

	struct FFixture
	{
		const TCHAR* Path;
		const TCHAR* ExpectedType;
	};

	const FFixture Fixtures[] =
	{
		{ TEXT("/Game/Blueprints/BP_SimpleActor"),           TEXT("Actor") },
		{ TEXT("/Game/Blueprints/BP_SimpleComponent"),       TEXT("Component") },
		{ TEXT("/Game/Blueprints/BP_SimpleFunctionLibrary"), TEXT("FunctionLibrary") },
		{ TEXT("/Game/Blueprints/BP_SimpleInterface"),       TEXT("Interface") },
	};

	for (const FFixture& Fix : Fixtures)
	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("asset_path"), Fix.Path);
		FCortexCommandResult Result = Handler.Execute(TEXT("get_info"), Params);

		TestTrue(
			*FString::Printf(TEXT("get_info %s should succeed"), Fix.Path),
			Result.bSuccess);

		if (Result.Data.IsValid())
		{
			FString Type;
			Result.Data->TryGetStringField(TEXT("type"), Type);
			TestEqual(
				*FString::Printf(TEXT("%s type"), Fix.Path),
				Type,
				FString(Fix.ExpectedType));
		}
	}

	return true;
}

// --------------------------------------------------------------------------
// list type detection
// --------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexBPTypeDetectionListComponentTest,
	"Cortex.Blueprint.TypeDetection.List.Component",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexBPTypeDetectionListComponentTest::RunTest(const FString& Parameters)
{
	FCortexBPCommandHandler Handler;

	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("path"), TEXT("/Game/Blueprints"));
	FCortexCommandResult Result = Handler.Execute(TEXT("list"), Params);

	TestTrue(TEXT("list should succeed"), Result.bSuccess);

	if (Result.Data.IsValid())
	{
		const TArray<TSharedPtr<FJsonValue>>* BlueprintsArray;
		if (TestTrue(TEXT("blueprints array should exist"),
			Result.Data->TryGetArrayField(TEXT("blueprints"), BlueprintsArray)))
		{
			bool bFound = false;
			for (const TSharedPtr<FJsonValue>& Entry : *BlueprintsArray)
			{
				const TSharedPtr<FJsonObject>& Obj = Entry->AsObject();
				FString Name;
				Obj->TryGetStringField(TEXT("name"), Name);
				if (Name == TEXT("BP_SimpleComponent"))
				{
					bFound = true;
					FString Type;
					Obj->TryGetStringField(TEXT("type"), Type);
					TestEqual(TEXT("BP_SimpleComponent list type should be Component"),
						Type, TEXT("Component"));
					break;
				}
			}
			TestTrue(TEXT("BP_SimpleComponent should appear in list"), bFound);
		}
	}

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexBPTypeDetectionListAllTypesTest,
	"Cortex.Blueprint.TypeDetection.List.AllTypes",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexBPTypeDetectionListAllTypesTest::RunTest(const FString& Parameters)
{
	FCortexBPCommandHandler Handler;

	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("path"), TEXT("/Game/Blueprints"));
	FCortexCommandResult Result = Handler.Execute(TEXT("list"), Params);

	TestTrue(TEXT("list should succeed"), Result.bSuccess);

	if (!Result.Data.IsValid())
	{
		return true;
	}

	const TArray<TSharedPtr<FJsonValue>>* BlueprintsArray;
	if (!TestTrue(TEXT("blueprints array should exist"),
		Result.Data->TryGetArrayField(TEXT("blueprints"), BlueprintsArray)))
	{
		return true;
	}

	// Build a name -> type map from the list response
	TMap<FString, FString> TypeMap;
	for (const TSharedPtr<FJsonValue>& Entry : *BlueprintsArray)
	{
		const TSharedPtr<FJsonObject>& Obj = Entry->AsObject();
		FString Name, Type;
		Obj->TryGetStringField(TEXT("name"), Name);
		Obj->TryGetStringField(TEXT("type"), Type);
		TypeMap.Add(Name, Type);
	}

	struct FExpected
	{
		const TCHAR* Name;
		const TCHAR* Type;
	};

	const FExpected Expected[] =
	{
		{ TEXT("BP_SimpleActor"),           TEXT("Actor") },
		{ TEXT("BP_SimpleComponent"),       TEXT("Component") },
		{ TEXT("BP_SimpleFunctionLibrary"), TEXT("FunctionLibrary") },
		{ TEXT("BP_SimpleInterface"),       TEXT("Interface") },
	};

	for (const FExpected& E : Expected)
	{
		const FString* Found = TypeMap.Find(E.Name);
		if (TestTrue(*FString::Printf(TEXT("%s should appear in list"), E.Name), Found != nullptr))
		{
			TestEqual(
				*FString::Printf(TEXT("%s list type"), E.Name),
				*Found,
				FString(E.Type));
		}
	}

	return true;
}
