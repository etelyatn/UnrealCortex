#include "Misc/AutomationTest.h"
#include "CortexBPCommandHandler.h"
#include "CortexTypes.h"
#include "CortexBPList119TestUtils.h"
#include "Animation/AnimBlueprint.h"
#include "Animation/AnimInstance.h"
#include "AssetToolsModule.h"
#include "Factories/AnimBlueprintFactory.h"
#include "IAssetTools.h"

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

// --------------------------------------------------------------------------
// Extended vocabulary shared by list and get_info
// --------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexBPTypeDetectionExtendedVocabularyTest,
	"Cortex.Blueprint.TypeDetection.ExtendedVocabulary",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexBPTypeDetectionExtendedVocabularyTest::RunTest(const FString& Parameters)
{
	using namespace CortexBPList119Test;

	FCortexBPCommandHandler Handler;
	const FString Dir = MakeUniqueDir(TEXT("ExtendedVocabulary"));
	TArray<FString> Created;

	// Plain UObject parent -> Object (previously Unknown).
	const FString ObjectName = TEXT("BP_EV_Object");
	if (TestTrue(TEXT("create UObject-parent BP"),
		CreateBlueprint(Handler, Dir, ObjectName, FString(), TEXT("/Script/CoreUObject.Object"))))
	{
		Created.Add(ObjectName);
	}

	// AnimBlueprint (template, no skeleton) through its factory.
	const FString AnimName = TEXT("ABP_EV_Anim");
	bool bAnimCreated = false;
	{
		UAnimBlueprintFactory* Factory = NewObject<UAnimBlueprintFactory>();
		Factory->ParentClass = UAnimInstance::StaticClass();
		Factory->bTemplate = true;
		IAssetTools& AssetTools = FModuleManager::LoadModuleChecked<FAssetToolsModule>("AssetTools").Get();
		UObject* Asset = AssetTools.CreateAsset(AnimName, Dir, UAnimBlueprint::StaticClass(), Factory);
		if (Asset)
		{
			TSharedPtr<FJsonObject> SaveParams = MakeShared<FJsonObject>();
			SaveParams->SetStringField(TEXT("asset_path"), FString::Printf(TEXT("%s/%s"), *Dir, *AnimName));
			bAnimCreated = Handler.Execute(TEXT("save"), SaveParams).bSuccess;
			TestTrue(TEXT("save AnimBlueprint"), bAnimCreated);
			Created.Add(AnimName);
		}
		else
		{
			AddInfo(TEXT("AnimBlueprint not creatable without a skeleton; AnimBlueprint classification skipped"));
		}
	}

	// EditorUtility by native parent, only when Blutility classes are loaded.
	const FString UtilName = TEXT("EUB_EV_Util");
	const FString UtilWidgetName = TEXT("EUW_EV_UtilWidget");
	const bool bHasUtilObject = FindObject<UClass>(nullptr, TEXT("/Script/Blutility.EditorUtilityObject")) != nullptr;
	const bool bHasUtilWidget = FindObject<UClass>(nullptr, TEXT("/Script/Blutility.EditorUtilityWidget")) != nullptr;
	bool bUtilCreated = false;
	bool bUtilWidgetCreated = false;
	if (bHasUtilObject)
	{
		bUtilCreated = CreateBlueprint(Handler, Dir, UtilName, FString(), TEXT("/Script/Blutility.EditorUtilityObject"));
		TestTrue(TEXT("create EditorUtilityObject-parent BP"), bUtilCreated);
		if (bUtilCreated)
		{
			Created.Add(UtilName);
		}
	}
	else
	{
		AddInfo(TEXT("Blutility EditorUtilityObject class not loaded; EditorUtility classification skipped"));
	}
	if (bHasUtilWidget)
	{
		bUtilWidgetCreated = CreateBlueprint(Handler, Dir, UtilWidgetName, FString(), TEXT("/Script/Blutility.EditorUtilityWidget"));
		if (bUtilWidgetCreated)
		{
			Created.Add(UtilWidgetName);
		}
		else
		{
			AddInfo(TEXT("EditorUtilityWidget-parent BP not creatable through create; widget exclusion check skipped"));
		}
	}
	else
	{
		AddInfo(TEXT("Blutility EditorUtilityWidget class not loaded; widget exclusion check skipped"));
	}

	const FCortexCommandResult All = RunList(Handler, Dir, FString(), nullptr);
	TestTrue(TEXT("list succeeds"), All.bSuccess);

	TestEqual(TEXT("UObject-parent BP lists as Object"), RowString(FindRow(All, ObjectName), TEXT("type")), TEXT("Object"));
	if (bAnimCreated)
	{
		TestEqual(TEXT("AnimBlueprint lists as AnimBlueprint"), RowString(FindRow(All, AnimName), TEXT("type")), TEXT("AnimBlueprint"));
		TestEqual(TEXT("AnimBlueprint asset_class"), RowString(FindRow(All, AnimName), TEXT("asset_class")), TEXT("AnimBlueprint"));
	}
	if (bUtilCreated)
	{
		TestEqual(TEXT("EditorUtilityObject-parent BP lists as EditorUtility"),
			RowString(FindRow(All, UtilName), TEXT("type")), TEXT("EditorUtility"));
	}
	if (bUtilWidgetCreated)
	{
		TestEqual(TEXT("EditorUtilityWidget-parent BP lists as EditorUtility"),
			RowString(FindRow(All, UtilWidgetName), TEXT("type")), TEXT("EditorUtility"));
		const FCortexCommandResult Widgets = RunList(Handler, Dir, TEXT("Widget"), nullptr);
		TestFalse(TEXT("EditorUtilityWidget BP must not appear under type Widget"),
			RowNames(Widgets).Contains(UtilWidgetName));
	}

	// The loaded-Blueprint classifier (get_info) must agree with the Asset Registry classifier.
	const FCortexCommandResult ObjectFilter = RunList(Handler, Dir, TEXT("object"), nullptr);
	TestTrue(TEXT("type=object includes the UObject-parent BP"), RowNames(ObjectFilter).Contains(ObjectName));
	for (const FString& Name : Created)
	{
		TSharedPtr<FJsonObject> InfoParams = MakeShared<FJsonObject>();
		InfoParams->SetStringField(TEXT("asset_path"), FString::Printf(TEXT("%s/%s"), *Dir, *Name));
		const FCortexCommandResult Info = Handler.Execute(TEXT("get_info"), InfoParams);
		if (TestTrue(*FString::Printf(TEXT("get_info %s succeeds"), *Name), Info.bSuccess) && Info.Data.IsValid())
		{
			FString InfoType;
			Info.Data->TryGetStringField(TEXT("type"), InfoType);
			TestEqual(*FString::Printf(TEXT("get_info type matches list type for %s"), *Name),
				InfoType, RowString(FindRow(All, Name), TEXT("type")));
		}
	}

	for (int32 Index = Created.Num() - 1; Index >= 0; --Index)
	{
		DeleteBlueprint(Handler, Dir, Created[Index]);
	}
	RemoveTestDirectory(Dir);
	return true;
}
