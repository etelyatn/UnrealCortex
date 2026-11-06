#include "Misc/AutomationTest.h"
#include "Misc/Guid.h"
#include "Misc/PackageName.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Engine/Blueprint.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "GameFramework/Actor.h"
#include "GameplayTagsManager.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "UObject/Class.h"
#include "UObject/Package.h"
#include "UObject/UnrealType.h"
#include "UObject/UObjectGlobals.h"
#include "UObject/UObjectHash.h"
#include "CortexBPCommandHandler.h"
#include "CortexBPEchoContractFixture.h"
#include "CortexTypes.h"

namespace
{
	FString CreateTestBlueprint(FCortexBPCommandHandler& Handler, const FString& Suffix)
	{
		const FString Name = FString::Printf(TEXT("BP_CDOTest_%s"), *Suffix);
		const FString Dir = FString::Printf(TEXT("/Game/Temp/CortexBPCDOTest_%s"), *Suffix);

		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("name"), Name);
		Params->SetStringField(TEXT("path"), Dir);
		Params->SetStringField(TEXT("type"), TEXT("Actor"));

		const FCortexCommandResult Result = Handler.Execute(TEXT("create"), Params);
		if (!Result.bSuccess || !Result.Data.IsValid())
		{
			return FString();
		}

		FString AssetPath;
		Result.Data->TryGetStringField(TEXT("asset_path"), AssetPath);
		return AssetPath;
	}

	void CleanupTestBlueprint(const FString& AssetPath)
	{
		UBlueprint* Blueprint = LoadObject<UBlueprint>(nullptr, *AssetPath);
		if (Blueprint)
		{
			Blueprint->GetOutermost()->MarkAsGarbage();
		}
	}

	// ============================================================================
	// Task 14 (class-default mutation echo) fixture helpers.
	// Every helper name is module-unique because the supported unity test build merges the
	// anonymous namespaces of every translation unit in this module.
	// ============================================================================
	FString EchoFixtureUniquePath(const TCHAR* Purpose)
	{
		return FString::Printf(TEXT("/Game/Temp/CortexBPEcho_%s_%s"),
			Purpose,
			*FGuid::NewGuid().ToString(EGuidFormats::Digits).Right(8));
	}

	UBlueprint* CreateEchoFixtureBlueprint(UClass* ParentClass, const FString& UniquePath)
	{
		UPackage* Package = CreatePackage(*UniquePath);
		if (!Package)
		{
			return nullptr;
		}

		const FName BlueprintName(*FPackageName::GetShortName(UniquePath));
		UBlueprint* Blueprint = FKismetEditorUtilities::CreateBlueprint(
			ParentClass,
			Package,
			BlueprintName,
			BPTYPE_Normal,
			UBlueprint::StaticClass(),
			UBlueprintGeneratedClass::StaticClass());
		if (!Blueprint)
		{
			Package->MarkAsGarbage();
			return nullptr;
		}

		FKismetEditorUtilities::CompileBlueprint(Blueprint);
		return Blueprint;
	}

	// Retires ONLY the dedicated package created for an echo fixture. Generated classes and CDOs keep
	// their reflection metadata for the engine's own later GC pass; only public/standalone flags and
	// non-metadata objects are released. No global GC is requested here.
	void RetireEchoFixture(UBlueprint* Blueprint)
	{
		if (!Blueprint)
		{
			return;
		}

		UPackage* Package = Blueprint->GetOutermost();
		if (!Package)
		{
			return;
		}

		ForEachObjectWithOuter(Package, [](UObject* Object)
		{
			Object->ClearFlags(RF_Public | RF_Standalone);
			if (!Object->IsA<UField>() || Object->IsA<UClass>())
			{
				Object->MarkAsGarbage();
			}
		});
		Package->MarkAsGarbage();
	}

	struct FEchoScopedFixture
	{
		UBlueprint* Blueprint = nullptr;
		FString AssetPath;

		FEchoScopedFixture() = default;
		FEchoScopedFixture(const FEchoScopedFixture&) = delete;
		FEchoScopedFixture& operator=(const FEchoScopedFixture&) = delete;

		~FEchoScopedFixture()
		{
			RetireEchoFixture(Blueprint);
		}

		// Creates the dedicated Blueprint and returns its CDO, which is the object the production
		// setter mutates.
		UObject* Create(UClass* ParentClass, const FString& UniquePath)
		{
			Blueprint = CreateEchoFixtureBlueprint(ParentClass, UniquePath);
			if (Blueprint)
			{
				AssetPath = Blueprint->GetPathName();
			}
			return Blueprint && Blueprint->GeneratedClass
				? Blueprint->GeneratedClass->GetDefaultObject()
				: nullptr;
		}
	};

	// Builds a set_class_defaults request that never compiles or saves, so the fixture package stays
	// in memory and the assertions observe only the mutation contract.
	TSharedPtr<FJsonObject> EchoSetParams(
		const FString& AssetPath,
		const TSharedPtr<FJsonObject>& Properties,
		const TCHAR* EchoMode)
	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("asset_path"), AssetPath);
		Params->SetObjectField(TEXT("properties"), Properties);
		Params->SetBoolField(TEXT("compile"), false);
		Params->SetBoolField(TEXT("save"), false);
		if (EchoMode != nullptr)
		{
			Params->SetStringField(TEXT("echo"), EchoMode);
		}
		return Params;
	}

	TSharedPtr<FJsonObject> EchoResultRecord(const FCortexCommandResult& Result, const TCHAR* PropertyName)
	{
		const TSharedPtr<FJsonObject>* Results = nullptr;
		if (!Result.Data.IsValid()
			|| !Result.Data->TryGetObjectField(TEXT("results"), Results)
			|| Results == nullptr)
		{
			return nullptr;
		}

		const TSharedPtr<FJsonObject>* Record = nullptr;
		if (!(*Results)->TryGetObjectField(PropertyName, Record) || Record == nullptr)
		{
			return nullptr;
		}
		return *Record;
	}

	// Reads an integer leaf through native reflection. This is the precision oracle: the legacy
	// PropertyToJson getter rounds wide integers and may never decide an expected value.
	int64 EchoReadInt64(UObject* Owner, FProperty* Property, void* ValuePtr = nullptr)
	{
		const FInt64Property* Int64Property = CastField<FInt64Property>(Property);
		if (!Int64Property || !Owner)
		{
			return 0;
		}
		const void* Target = ValuePtr != nullptr
			? ValuePtr
			: Int64Property->ContainerPtrToValuePtr<void>(Owner);
		return Int64Property->GetPropertyValue(Target);
	}

	// Native oracle for the FInstancedStruct fixture field: the runtime payload pointer, or nullptr when
	// the field is invalid or holds a different struct type. The legacy rounded getter is never used.
	const FCortexBPEchoInstancePayload* EchoReadInstancePayload(UObject* Owner, FStructProperty* InstanceProperty)
	{
		if (!Owner || !InstanceProperty)
		{
			return nullptr;
		}
		const FInstancedStruct* Instance =
			static_cast<const FInstancedStruct*>(InstanceProperty->ContainerPtrToValuePtr<void>(Owner));
		return Instance ? Instance->GetPtr<FCortexBPEchoInstancePayload>() : nullptr;
	}

	int64 EchoReadInstanceBig(UObject* Owner, FStructProperty* InstanceProperty)
	{
		const FCortexBPEchoInstancePayload* Payload = EchoReadInstancePayload(Owner, InstanceProperty);
		return Payload ? Payload->Big : 0;
	}

	// Seeds the FInstancedStruct fixture field with a runtime payload of the expected type, directly
	// through native reflection.
	void EchoSeedInstancePayload(UObject* Owner, FStructProperty* InstanceProperty, int64 Big)
	{
		if (!Owner || !InstanceProperty)
		{
			return;
		}
		FInstancedStruct* Instance =
			static_cast<FInstancedStruct*>(InstanceProperty->ContainerPtrToValuePtr<void>(Owner));
		if (!Instance)
		{
			return;
		}
		Instance->InitializeAs(FCortexBPEchoInstancePayload::StaticStruct());
		if (FCortexBPEchoInstancePayload* Payload = Instance->GetMutablePtr<FCortexBPEchoInstancePayload>())
		{
			Payload->Big = Big;
		}
	}
}

// The echo fixture constructor seeds a deterministic baseline for every value field, so a change can
// be expressed as a real mutation against a known previous value.
ACortexBPEchoContractActor::ACortexBPEchoContractActor()
{
	WideCounter = 0;
	TokenVector = FVector(1.0, 2.0, 3.0);

	WidePayload.Big = 0;
	WidePayload.Small = 7;
	WidePayload.bFlag = true;
	WidePayload.Payload = UObject::StaticClass();
	WidePayload.Tag = TEXT("abc");
	WidePayload.Label = FText::FromString(TEXT("abc"));

	WideArray = { 0 };
	AtomicInts = { 1, 2, 3 };
	CaseText = TEXT("abc");
	EchoText = FText::FromString(TEXT("abc"));
	EchoObject = UObject::StaticClass();

	EchoSoftPath = FSoftObjectPath(TEXT("/Game/A.A"));
	// EchoTags and EchoInstance start empty/invalid; the special-encoding regressions seed them.
	MixedPayload.Big = 0;
	MixedPayload.Vector = FVector(1.0, 2.0, 3.0);
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexBPGetClassDefaultsMissingPathTest,
	"Cortex.Blueprint.ClassDefaults.Get.MissingPath",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexBPGetClassDefaultsMissingPathTest::RunTest(const FString& Parameters)
{
	FCortexBPCommandHandler Handler;
	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();

	const FCortexCommandResult Result = Handler.Execute(TEXT("get_class_defaults"), Params);

	TestFalse(TEXT("Should fail without asset_path"), Result.bSuccess);
	TestEqual(TEXT("Error code should be INVALID_FIELD"),
		Result.ErrorCode, CortexErrorCodes::InvalidField);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexBPGetClassDefaultsNotFoundTest,
	"Cortex.Blueprint.ClassDefaults.Get.NotFound",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexBPGetClassDefaultsNotFoundTest::RunTest(const FString& Parameters)
{
	FCortexBPCommandHandler Handler;
	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("blueprint_path"), TEXT("/Game/NonExistent/BP_Nope"));

	const FCortexCommandResult Result = Handler.Execute(TEXT("get_class_defaults"), Params);

	TestFalse(TEXT("Should fail for non-existent Blueprint"), Result.bSuccess);
	TestEqual(TEXT("Error code should be BLUEPRINT_NOT_FOUND"),
		Result.ErrorCode, CortexErrorCodes::BlueprintNotFound);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexBPGetClassDefaultsDiscoveryTest,
	"Cortex.Blueprint.ClassDefaults.Get.Discovery",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexBPGetClassDefaultsDiscoveryTest::RunTest(const FString& Parameters)
{
	const FString Suffix = FGuid::NewGuid().ToString(EGuidFormats::Digits).Right(8);
	FCortexBPCommandHandler Handler;
	const FString AssetPath = CreateTestBlueprint(Handler, Suffix);
	TestFalse(TEXT("Test Blueprint should be created"), AssetPath.IsEmpty());
	if (AssetPath.IsEmpty())
	{
		return true;
	}

	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("blueprint_path"), AssetPath);

	const FCortexCommandResult Result = Handler.Execute(TEXT("get_class_defaults"), Params);

	TestTrue(TEXT("Discovery should succeed"), Result.bSuccess);
	if (Result.Data.IsValid())
	{
		const TSharedPtr<FJsonObject>* PropsObj = nullptr;
		TestTrue(TEXT("Should have properties object"),
			Result.Data->TryGetObjectField(TEXT("properties"), PropsObj));

		double Count = 0;
		Result.Data->TryGetNumberField(TEXT("count"), Count);
		TestTrue(TEXT("Should discover at least one property"), Count > 0);

		FString ParentClass;
		Result.Data->TryGetStringField(TEXT("parent_class"), ParentClass);
		TestEqual(TEXT("parent_class should be Actor"), ParentClass, TEXT("Actor"));
	}

	CleanupTestBlueprint(AssetPath);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexBPGetClassDefaultsAssetPathTest,
	"Cortex.Blueprint.ClassDefaults.Get.AssetPath",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexBPGetClassDefaultsAssetPathTest::RunTest(const FString& Parameters)
{
	const FString Suffix = FGuid::NewGuid().ToString(EGuidFormats::Digits).Right(8);
	FCortexBPCommandHandler Handler;
	const FString AssetPath = CreateTestBlueprint(Handler, Suffix);
	TestFalse(TEXT("Test Blueprint should be created"), AssetPath.IsEmpty());
	if (AssetPath.IsEmpty())
	{
		return true;
	}

	// Use asset_path (primary field, not the deprecated blueprint_path)
	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("asset_path"), AssetPath);

	const FCortexCommandResult Result = Handler.Execute(TEXT("get_class_defaults"), Params);

	TestTrue(TEXT("get_class_defaults via asset_path should succeed"), Result.bSuccess);
	if (Result.Data.IsValid())
	{
		const TSharedPtr<FJsonObject>* PropsObj = nullptr;
		TestTrue(TEXT("Should have properties object"),
			Result.Data->TryGetObjectField(TEXT("properties"), PropsObj));
	}

	CleanupTestBlueprint(AssetPath);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexBPGetClassDefaultsSpecificPropertyTest,
	"Cortex.Blueprint.ClassDefaults.Get.SpecificProperty",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexBPGetClassDefaultsSpecificPropertyTest::RunTest(const FString& Parameters)
{
	const FString Suffix = FGuid::NewGuid().ToString(EGuidFormats::Digits).Right(8);
	FCortexBPCommandHandler Handler;
	const FString AssetPath = CreateTestBlueprint(Handler, Suffix);
	TestFalse(TEXT("Test Blueprint should be created"), AssetPath.IsEmpty());
	if (AssetPath.IsEmpty())
	{
		return true;
	}

	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("blueprint_path"), AssetPath);
	TArray<TSharedPtr<FJsonValue>> PropNames;
	PropNames.Add(MakeShared<FJsonValueString>(TEXT("bReplicates")));
	Params->SetArrayField(TEXT("properties"), PropNames);

	const FCortexCommandResult Result = Handler.Execute(TEXT("get_class_defaults"), Params);

	TestTrue(TEXT("Get specific property should succeed"), Result.bSuccess);
	if (Result.Data.IsValid())
	{
		const TSharedPtr<FJsonObject>* PropsObj = nullptr;
		if (Result.Data->TryGetObjectField(TEXT("properties"), PropsObj) && PropsObj)
		{
			const TSharedPtr<FJsonObject>* PropDetail = nullptr;
			TestTrue(TEXT("Should have bReplicates in results"),
				(*PropsObj)->TryGetObjectField(TEXT("bReplicates"), PropDetail));

			if (PropDetail && *PropDetail)
			{
				FString Type;
				(*PropDetail)->TryGetStringField(TEXT("type"), Type);
				TestFalse(TEXT("type should not be empty"), Type.IsEmpty());

				TestTrue(TEXT("value should be present"),
					(*PropDetail)->HasField(TEXT("value")));
			}
		}
	}

	CleanupTestBlueprint(AssetPath);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexBPGetClassDefaultsPropertyNotFoundTest,
	"Cortex.Blueprint.ClassDefaults.Get.PropertyNotFound",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexBPGetClassDefaultsPropertyNotFoundTest::RunTest(const FString& Parameters)
{
	const FString Suffix = FGuid::NewGuid().ToString(EGuidFormats::Digits).Right(8);
	FCortexBPCommandHandler Handler;
	const FString AssetPath = CreateTestBlueprint(Handler, Suffix);
	TestFalse(TEXT("Test Blueprint should be created"), AssetPath.IsEmpty());
	if (AssetPath.IsEmpty())
	{
		return true;
	}

	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("blueprint_path"), AssetPath);
	TArray<TSharedPtr<FJsonValue>> PropNames;
	PropNames.Add(MakeShared<FJsonValueString>(TEXT("bReplicatess")));
	Params->SetArrayField(TEXT("properties"), PropNames);

	const FCortexCommandResult Result = Handler.Execute(TEXT("get_class_defaults"), Params);

	TestFalse(TEXT("Should fail for misspelled property"), Result.bSuccess);
	TestEqual(TEXT("Error code should be PROPERTY_NOT_FOUND"),
		Result.ErrorCode, CortexErrorCodes::PropertyNotFound);

	if (Result.ErrorDetails.IsValid())
	{
		const TArray<TSharedPtr<FJsonValue>>* Suggestions = nullptr;
		TestTrue(TEXT("Should have suggestions array"),
			Result.ErrorDetails->TryGetArrayField(TEXT("suggestions"), Suggestions));
	}

	CleanupTestBlueprint(AssetPath);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexBPSetClassDefaultsMissingPropertiesTest,
	"Cortex.Blueprint.ClassDefaults.Set.MissingProperties",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexBPSetClassDefaultsMissingPropertiesTest::RunTest(const FString& Parameters)
{
	FCortexBPCommandHandler Handler;
	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("blueprint_path"), TEXT("/Game/Any/BP_Test"));

	const FCortexCommandResult Result = Handler.Execute(TEXT("set_class_defaults"), Params);

	TestFalse(TEXT("Should fail without properties"), Result.bSuccess);
	TestEqual(TEXT("Error code should be INVALID_FIELD"),
		Result.ErrorCode, CortexErrorCodes::InvalidField);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexBPSetClassDefaultsPropertyNotFoundTest,
	"Cortex.Blueprint.ClassDefaults.Set.PropertyNotFound",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexBPSetClassDefaultsPropertyNotFoundTest::RunTest(const FString& Parameters)
{
	const FString Suffix = FGuid::NewGuid().ToString(EGuidFormats::Digits).Right(8);
	FCortexBPCommandHandler Handler;
	const FString AssetPath = CreateTestBlueprint(Handler, Suffix);
	TestFalse(TEXT("Test Blueprint should be created"), AssetPath.IsEmpty());
	if (AssetPath.IsEmpty())
	{
		return true;
	}

	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("blueprint_path"), AssetPath);

	TSharedPtr<FJsonObject> Properties = MakeShared<FJsonObject>();
	Properties->SetBoolField(TEXT("NopeProperty"), true);
	Params->SetObjectField(TEXT("properties"), Properties);
	Params->SetBoolField(TEXT("compile"), false);
	Params->SetBoolField(TEXT("save"), false);

	const FCortexCommandResult Result = Handler.Execute(TEXT("set_class_defaults"), Params);

	TestFalse(TEXT("Should fail for non-existent property"), Result.bSuccess);
	TestEqual(TEXT("Error code should be PROPERTY_NOT_FOUND"),
		Result.ErrorCode, CortexErrorCodes::PropertyNotFound);

	CleanupTestBlueprint(AssetPath);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexBPSetClassDefaultsTypeMismatchTest,
	"Cortex.Blueprint.ClassDefaults.Set.TypeMismatch",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexBPSetClassDefaultsTypeMismatchTest::RunTest(const FString& Parameters)
{
	const FString Suffix = FGuid::NewGuid().ToString(EGuidFormats::Digits).Right(8);
	FCortexBPCommandHandler Handler;
	const FString AssetPath = CreateTestBlueprint(Handler, Suffix);
	TestFalse(TEXT("Test Blueprint should be created"), AssetPath.IsEmpty());
	if (AssetPath.IsEmpty())
	{
		return true;
	}

	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("blueprint_path"), AssetPath);

	TSharedPtr<FJsonObject> Properties = MakeShared<FJsonObject>();
	Properties->SetNumberField(TEXT("RootComponent"), 123.0);
	Params->SetObjectField(TEXT("properties"), Properties);
	Params->SetBoolField(TEXT("compile"), false);
	Params->SetBoolField(TEXT("save"), false);

	const FCortexCommandResult Result = Handler.Execute(TEXT("set_class_defaults"), Params);

	TestFalse(TEXT("Should fail with type mismatch"), Result.bSuccess);
	TestEqual(TEXT("Error code should be TYPE_MISMATCH"),
		Result.ErrorCode, CortexErrorCodes::TypeMismatch);

	CleanupTestBlueprint(AssetPath);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexBPSetClassDefaultsSuccessTest,
	"Cortex.Blueprint.ClassDefaults.Set.Success",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexBPSetClassDefaultsSuccessTest::RunTest(const FString& Parameters)
{
	const FString Suffix = FGuid::NewGuid().ToString(EGuidFormats::Digits).Right(8);
	FCortexBPCommandHandler Handler;
	const FString AssetPath = CreateTestBlueprint(Handler, Suffix);
	TestFalse(TEXT("Test Blueprint should be created"), AssetPath.IsEmpty());
	if (AssetPath.IsEmpty())
	{
		return true;
	}

	TSharedPtr<FJsonObject> SetParams = MakeShared<FJsonObject>();
	SetParams->SetStringField(TEXT("blueprint_path"), AssetPath);

	TSharedPtr<FJsonObject> Properties = MakeShared<FJsonObject>();
	Properties->SetBoolField(TEXT("bReplicates"), true);
	SetParams->SetObjectField(TEXT("properties"), Properties);
	SetParams->SetBoolField(TEXT("compile"), false);
	SetParams->SetBoolField(TEXT("save"), false);

	const FCortexCommandResult SetResult = Handler.Execute(TEXT("set_class_defaults"), SetParams);

	TestTrue(TEXT("Setting class defaults should succeed"), SetResult.bSuccess);
	if (SetResult.Data.IsValid())
	{
		bool bCompiled = true;
		SetResult.Data->TryGetBoolField(TEXT("compiled"), bCompiled);
		TestFalse(TEXT("compiled should be false when compile=false"), bCompiled);

		bool bSaved = true;
		SetResult.Data->TryGetBoolField(TEXT("saved"), bSaved);
		TestFalse(TEXT("saved should be false when save=false"), bSaved);
	}

	TSharedPtr<FJsonObject> GetParams = MakeShared<FJsonObject>();
	GetParams->SetStringField(TEXT("blueprint_path"), AssetPath);
	TArray<TSharedPtr<FJsonValue>> PropNames;
	PropNames.Add(MakeShared<FJsonValueString>(TEXT("bReplicates")));
	GetParams->SetArrayField(TEXT("properties"), PropNames);

	const FCortexCommandResult GetResult = Handler.Execute(TEXT("get_class_defaults"), GetParams);
	TestTrue(TEXT("Get after set should succeed"), GetResult.bSuccess);

	CleanupTestBlueprint(AssetPath);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexBPSetClassDefaultsSingleStaleFingerprintTest,
	"Cortex.Blueprint.ClassDefaults.Set.SingleStaleFingerprint",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexBPSetClassDefaultsSingleStaleFingerprintTest::RunTest(const FString& Parameters)
{
	const FString Suffix = FGuid::NewGuid().ToString(EGuidFormats::Digits).Right(8);
	FCortexBPCommandHandler Handler;
	const FString AssetPath = CreateTestBlueprint(Handler, Suffix);
	TestFalse(TEXT("Test Blueprint should be created"), AssetPath.IsEmpty());
	if (AssetPath.IsEmpty())
	{
		return true;
	}

	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("asset_path"), AssetPath);
	TSharedPtr<FJsonObject> Properties = MakeShared<FJsonObject>();
	Properties->SetBoolField(TEXT("bReplicates"), true);
	Params->SetObjectField(TEXT("properties"), Properties);
	Params->SetObjectField(TEXT("expected_fingerprint"), MakeShared<FJsonObject>());
	Params->SetBoolField(TEXT("compile"), false);
	Params->SetBoolField(TEXT("save"), false);

	const FCortexCommandResult Result = Handler.Execute(TEXT("set_class_defaults"), Params);
	TestFalse(TEXT("single stale fingerprint rejects class defaults write"), Result.bSuccess);
	TestEqual(TEXT("single stale fingerprint error code"),
		Result.ErrorCode, CortexErrorCodes::StalePrecondition);

	UBlueprint* Blueprint = LoadObject<UBlueprint>(nullptr, *AssetPath);
	AActor* CDO = Blueprint && Blueprint->GeneratedClass
		? Cast<AActor>(Blueprint->GeneratedClass->GetDefaultObject())
		: nullptr;
	TestNotNull(TEXT("Actor CDO exists"), CDO);
	TestFalse(TEXT("stale class-default write does not mutate CDO"), CDO ? CDO->GetIsReplicated() : true);

	CleanupTestBlueprint(AssetPath);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexBPSetClassDefaultsSingleStaleFingerprintBlueprintPathAliasTest,
	"Cortex.Blueprint.ClassDefaults.Set.SingleStaleFingerprintBlueprintPathAlias",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexBPSetClassDefaultsSingleStaleFingerprintBlueprintPathAliasTest::RunTest(const FString& Parameters)
{
	const FString Suffix = FGuid::NewGuid().ToString(EGuidFormats::Digits).Right(8);
	FCortexBPCommandHandler Handler;
	const FString AssetPath = CreateTestBlueprint(Handler, Suffix);
	TestFalse(TEXT("Test Blueprint should be created"), AssetPath.IsEmpty());
	if (AssetPath.IsEmpty())
	{
		return true;
	}

	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("blueprint_path"), AssetPath);
	TSharedPtr<FJsonObject> Properties = MakeShared<FJsonObject>();
	Properties->SetBoolField(TEXT("bReplicates"), true);
	Params->SetObjectField(TEXT("properties"), Properties);
	Params->SetObjectField(TEXT("expected_fingerprint"), MakeShared<FJsonObject>());
	Params->SetBoolField(TEXT("compile"), false);
	Params->SetBoolField(TEXT("save"), false);

	const FCortexCommandResult Result = Handler.Execute(TEXT("set_class_defaults"), Params);
	TestFalse(TEXT("blueprint_path alias stale guard rejects class defaults write"), Result.bSuccess);
	TestEqual(TEXT("blueprint_path alias stale guard error code"),
		Result.ErrorCode, CortexErrorCodes::StalePrecondition);

	UBlueprint* Blueprint = LoadObject<UBlueprint>(nullptr, *AssetPath);
	AActor* CDO = Blueprint && Blueprint->GeneratedClass
		? Cast<AActor>(Blueprint->GeneratedClass->GetDefaultObject())
		: nullptr;
	TestNotNull(TEXT("Actor CDO exists"), CDO);
	TestFalse(TEXT("stale alias class-default write does not mutate CDO"), CDO ? CDO->GetIsReplicated() : true);

	CleanupTestBlueprint(AssetPath);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexBPClassDefaultsNestedDiscoveryTest,
	"Cortex.Blueprint.ClassDefaults.Get.NestedDiscovery",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexBPClassDefaultsNestedDiscoveryTest::RunTest(const FString& Parameters)
{
	const FString Suffix = FGuid::NewGuid().ToString(EGuidFormats::Digits).Right(8);
	FCortexBPCommandHandler Handler;
	const FString AssetPath = CreateTestBlueprint(Handler, Suffix);
	TestFalse(TEXT("Test Blueprint should be created"), AssetPath.IsEmpty());
	if (AssetPath.IsEmpty())
	{
		return true;
	}

	// Struct member expansion is opt-in, so this consumer requests values and members explicitly.
	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("blueprint_path"), AssetPath);
	Params->SetBoolField(TEXT("include_values"), true);
	Params->SetBoolField(TEXT("include_members"), true);

	const FCortexCommandResult Result = Handler.Execute(TEXT("get_class_defaults"), Params);
	TestTrue(TEXT("Discovery should succeed"), Result.bSuccess);

	if (Result.Data.IsValid())
	{
		bool bValuesIncluded = false;
		TestTrue(TEXT("explicit value read declares value inclusion"),
			Result.Data->TryGetBoolField(TEXT("values_included"), bValuesIncluded));
		TestTrue(TEXT("explicit value read includes values"), bValuesIncluded);

		const TSharedPtr<FJsonObject>* PropsObj = nullptr;
		if (Result.Data->TryGetObjectField(TEXT("properties"), PropsObj) && PropsObj)
		{
			// Look for PrimaryActorTick — it's an FTickFunction struct on AActor
			const TSharedPtr<FJsonObject>* TickObj = nullptr;
			if ((*PropsObj)->TryGetObjectField(TEXT("PrimaryActorTick"), TickObj) && TickObj)
			{
				// It should have a "members" object
				const TSharedPtr<FJsonObject>* MembersObj = nullptr;
				TestTrue(TEXT("PrimaryActorTick should have 'members' field"),
					(*TickObj)->TryGetObjectField(TEXT("members"), MembersObj));

				if (MembersObj && *MembersObj)
				{
					// Check that members has entries with path info
					TestTrue(TEXT("members should have at least one entry"),
						(*MembersObj)->Values.Num() > 0);

					// Check a specific known member
					const TSharedPtr<FJsonObject>* CanTickObj = nullptr;
					if ((*MembersObj)->TryGetObjectField(TEXT("bCanEverTick"), CanTickObj) && CanTickObj)
					{
						FString Path;
						(*CanTickObj)->TryGetStringField(TEXT("path"), Path);
						TestEqual(TEXT("path should be dot-notation"),
							Path, TEXT("PrimaryActorTick.bCanEverTick"));
					}
				}
			}
		}
	}

	CleanupTestBlueprint(AssetPath);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexBPClassDefaultsNestedFuzzySearchTest,
	"Cortex.Blueprint.ClassDefaults.Get.NestedFuzzySearch",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexBPClassDefaultsNestedFuzzySearchTest::RunTest(const FString& Parameters)
{
	const FString Suffix = FGuid::NewGuid().ToString(EGuidFormats::Digits).Right(8);
	FCortexBPCommandHandler Handler;
	const FString AssetPath = CreateTestBlueprint(Handler, Suffix);
	TestFalse(TEXT("Test Blueprint should be created"), AssetPath.IsEmpty());
	if (AssetPath.IsEmpty())
	{
		return true;
	}

	// Request a nested property by its leaf name only — should get suggestion with dot path
	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("blueprint_path"), AssetPath);
	TArray<TSharedPtr<FJsonValue>> PropNames;
	PropNames.Add(MakeShared<FJsonValueString>(TEXT("bCanEverTick")));
	Params->SetArrayField(TEXT("properties"), PropNames);

	const FCortexCommandResult Result = Handler.Execute(TEXT("get_class_defaults"), Params);

	// This should fail with PROPERTY_NOT_FOUND since bCanEverTick is not a top-level property
	// But the suggestions should include the dot-notation path
	TestFalse(TEXT("Should fail for nested-only property name"), Result.bSuccess);
	TestEqual(TEXT("Error code should be PROPERTY_NOT_FOUND"),
		Result.ErrorCode, CortexErrorCodes::PropertyNotFound);

	if (Result.ErrorDetails.IsValid())
	{
		const TArray<TSharedPtr<FJsonValue>>* Suggestions = nullptr;
		if (Result.ErrorDetails->TryGetArrayField(TEXT("suggestions"), Suggestions) && Suggestions)
		{
			bool bFoundDotPath = false;
			for (const auto& Sug : *Suggestions)
			{
				if (Sug->AsString().Contains(TEXT(".")))
				{
					bFoundDotPath = true;
					break;
				}
			}
			TestTrue(TEXT("Suggestions should include dot-notation paths"), bFoundDotPath);
		}
	}

	CleanupTestBlueprint(AssetPath);
	return true;
}

// ============================================================================
// Task 14 — changed-leaf class-default echoes.
// These regressions pin the mutation echo contract before the private echo/snapshot helper exists:
// every assertion is a consumer-visible response shape or a native post-state, never a private API.
// ============================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexBPClassDefaultsEchoDiffOnlyChangedLeavesTest,
	"Cortex.Blueprint.ClassDefaults.Set.EchoDiffOnlyChangedLeaves",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexBPClassDefaultsEchoDiffOnlyChangedLeavesTest::RunTest(const FString&)
{
	FEchoScopedFixture Fixture;
	FCortexBPCommandHandler Handler;
	UObject* CDO = Fixture.Create(
		ACortexBPEchoContractActor::StaticClass(), EchoFixtureUniquePath(TEXT("DiffLeaf")));
	if (!TestNotNull(TEXT("echo fixture CDO should exist"), CDO))
	{
		return true;
	}

	// Default echo is diff. AtomicInts is requested with its current value so an unchanged root must
	// drop its value echo, and only the changed struct leaf may survive in the diff.
	TSharedPtr<FJsonObject> ChangedVector = MakeShared<FJsonObject>();
	ChangedVector->SetNumberField(TEXT("X"), 4);
	TSharedPtr<FJsonObject> WriteValues = MakeShared<FJsonObject>();
	WriteValues->SetObjectField(TEXT("TokenVector"), ChangedVector);
	WriteValues->SetArrayField(TEXT("AtomicInts"), {
		MakeShared<FJsonValueNumber>(1),
		MakeShared<FJsonValueNumber>(2),
		MakeShared<FJsonValueNumber>(3)});

	const FCortexCommandResult Set = Handler.Execute(
		TEXT("set_class_defaults"), EchoSetParams(Fixture.AssetPath, WriteValues, nullptr));
	if (!TestTrue(TEXT("diff set should succeed"), Set.bSuccess) || !Set.Data.IsValid())
	{
		return true;
	}

	bool bCompiled = true;
	bool bSaved = true;
	TestTrue(TEXT("diff set reports compile status"), Set.Data->TryGetBoolField(TEXT("compiled"), bCompiled));
	TestFalse(TEXT("compile=false keeps compiled false"), bCompiled);
	TestTrue(TEXT("diff set reports save status"), Set.Data->TryGetBoolField(TEXT("saved"), bSaved));
	TestFalse(TEXT("save=false keeps saved false"), bSaved);

	TSharedPtr<FJsonObject> VectorRecord = EchoResultRecord(Set, TEXT("TokenVector"));
	if (!TestNotNull(TEXT("changed struct record should exist"), VectorRecord.Get()))
	{
		return true;
	}

	FString Type;
	TestTrue(TEXT("struct record keeps its declared type"), VectorRecord->TryGetStringField(TEXT("type"), Type));
	TestEqual(TEXT("struct record type is the struct"), Type, FString(TEXT("FVector")));
	bool bSuccess = false;
	TestTrue(TEXT("struct record keeps success"), VectorRecord->TryGetBoolField(TEXT("success"), bSuccess));
	TestTrue(TEXT("struct record success is true"), bSuccess);
	bool bChanged = false;
	TestTrue(TEXT("struct record declares changed"), VectorRecord->TryGetBoolField(TEXT("changed"), bChanged));
	TestTrue(TEXT("one changed leaf is a real change"), bChanged);

	// A struct diff is leaf-only: it must not carry a complete previous/new object.
	TestFalse(TEXT("struct diff drops the complete previous value"), VectorRecord->HasField(TEXT("previous_value")));
	TestFalse(TEXT("struct diff drops the complete new value"), VectorRecord->HasField(TEXT("new_value")));

	const TSharedPtr<FJsonObject>* Changes = nullptr;
	if (TestTrue(TEXT("struct diff exposes changed leaf paths"),
		VectorRecord->TryGetObjectField(TEXT("changes"), Changes) && Changes != nullptr))
	{
		TestEqual(TEXT("only the changed leaf is reported"), (*Changes)->Values.Num(), 1);
		const TSharedPtr<FJsonObject>* XChange = nullptr;
		if (TestTrue(TEXT("the changed leaf uses its relative path"),
			(*Changes)->TryGetObjectField(TEXT("X"), XChange) && XChange != nullptr))
		{
			double Before = -1.0;
			double After = -1.0;
			TestTrue(TEXT("the changed leaf reports its previous value"),
				(*XChange)->TryGetNumberField(TEXT("previous_value"), Before));
			TestEqual(TEXT("the changed leaf previous value is 1"), static_cast<int32>(Before), 1);
			TestTrue(TEXT("the changed leaf reports its new value"),
				(*XChange)->TryGetNumberField(TEXT("new_value"), After));
			TestEqual(TEXT("the changed leaf new value is 4"), static_cast<int32>(After), 4);
		}
		TestFalse(TEXT("unchanged sibling Y stays out of the diff"), (*Changes)->HasField(TEXT("Y")));
		TestFalse(TEXT("unchanged sibling Z stays out of the diff"), (*Changes)->HasField(TEXT("Z")));
	}

	TSharedPtr<FJsonObject> IntsRecord = EchoResultRecord(Set, TEXT("AtomicInts"));
	if (TestNotNull(TEXT("unchanged collection record should exist"), IntsRecord.Get()))
	{
		bool bIntsChanged = true;
		TestTrue(TEXT("unchanged collection declares changed"),
			IntsRecord->TryGetBoolField(TEXT("changed"), bIntsChanged));
		TestFalse(TEXT("an identical atomic collection is not a change"), bIntsChanged);
		TestFalse(TEXT("unchanged collection drops its previous value"),
			IntsRecord->HasField(TEXT("previous_value")));
		TestFalse(TEXT("unchanged collection drops its new value"), IntsRecord->HasField(TEXT("new_value")));
	}

	// Readback through the production read: only X moved.
	TSharedPtr<FJsonObject> ReadParams = MakeShared<FJsonObject>();
	ReadParams->SetStringField(TEXT("asset_path"), Fixture.AssetPath);
	ReadParams->SetArrayField(TEXT("properties"), {
		MakeShared<FJsonValueString>(TEXT("TokenVector"))});
	const FCortexCommandResult Read = Handler.Execute(TEXT("get_class_defaults"), ReadParams);
	if (TestTrue(TEXT("struct readback should succeed"), Read.bSuccess) && Read.Data.IsValid())
	{
		const TSharedPtr<FJsonObject>* ReadProperties = nullptr;
		const TSharedPtr<FJsonObject>* ReadVector = nullptr;
		const TSharedPtr<FJsonObject>* ReadValue = nullptr;
		if (TestTrue(TEXT("readback exposes properties"),
				Read.Data->TryGetObjectField(TEXT("properties"), ReadProperties) && ReadProperties != nullptr)
			&& TestTrue(TEXT("readback exposes TokenVector"),
				(*ReadProperties)->TryGetObjectField(TEXT("TokenVector"), ReadVector) && ReadVector != nullptr)
			&& TestTrue(TEXT("readback exposes the struct value"),
				(*ReadVector)->TryGetObjectField(TEXT("value"), ReadValue) && ReadValue != nullptr))
		{
			double X = -1.0;
			double Y = -1.0;
			double Z = -1.0;
			TestTrue(TEXT("readback exposes X"), (*ReadValue)->TryGetNumberField(TEXT("X"), X));
			TestEqual(TEXT("X applied"), static_cast<int32>(X), 4);
			TestTrue(TEXT("readback exposes Y"), (*ReadValue)->TryGetNumberField(TEXT("Y"), Y));
			TestEqual(TEXT("Y untouched"), static_cast<int32>(Y), 2);
			TestTrue(TEXT("readback exposes Z"), (*ReadValue)->TryGetNumberField(TEXT("Z"), Z));
			TestEqual(TEXT("Z untouched"), static_cast<int32>(Z), 3);
		}
	}

	// No-op: applying the already-current value must report changed=false and echo no value.
	TSharedPtr<FJsonObject> NoOpVector = MakeShared<FJsonObject>();
	NoOpVector->SetNumberField(TEXT("X"), 4);
	TSharedPtr<FJsonObject> NoOpValues = MakeShared<FJsonObject>();
	NoOpValues->SetObjectField(TEXT("TokenVector"), NoOpVector);
	const FCortexCommandResult NoOp = Handler.Execute(
		TEXT("set_class_defaults"), EchoSetParams(Fixture.AssetPath, NoOpValues, nullptr));
	if (TestTrue(TEXT("no-op set should still succeed"), NoOp.bSuccess) && NoOp.Data.IsValid())
	{
		TSharedPtr<FJsonObject> NoOpRecord = EchoResultRecord(NoOp, TEXT("TokenVector"));
		if (TestNotNull(TEXT("no-op struct record should exist"), NoOpRecord.Get()))
		{
			bool bNoOpChanged = true;
			TestTrue(TEXT("no-op record declares changed"),
				NoOpRecord->TryGetBoolField(TEXT("changed"), bNoOpChanged));
			TestFalse(TEXT("re-applying the same leaf is not a change"), bNoOpChanged);
			TestFalse(TEXT("no-op drops the leaf changes"), NoOpRecord->HasField(TEXT("changes")));
			TestFalse(TEXT("no-op drops the previous value"), NoOpRecord->HasField(TEXT("previous_value")));
		}
	}

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexBPClassDefaultsEchoDiffCaseSensitiveFTextNullAtomicTest,
	"Cortex.Blueprint.ClassDefaults.Set.EchoDiffCaseSensitiveFTextNullAndAtomic",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexBPClassDefaultsEchoDiffCaseSensitiveFTextNullAtomicTest::RunTest(const FString&)
{
	FEchoScopedFixture Fixture;
	FCortexBPCommandHandler Handler;
	UObject* CDO = Fixture.Create(
		ACortexBPEchoContractActor::StaticClass(), EchoFixtureUniquePath(TEXT("Scalars")));
	if (!TestNotNull(TEXT("echo fixture CDO should exist"), CDO))
	{
		return true;
	}

	// A case-only string change is a real mutation; FText compares through descriptors; clearing a
	// non-null object to null and growing an atomic collection are explicit changed values. The helper
	// must not confuse representational equality with an actual mutation.
	TSharedPtr<FJsonObject> WriteValues = MakeShared<FJsonObject>();
	WriteValues->SetStringField(TEXT("CaseText"), TEXT("ABC"));
	WriteValues->SetStringField(TEXT("EchoText"), TEXT("ABC"));
	WriteValues->SetField(TEXT("EchoObject"), MakeShared<FJsonValueNull>());
	WriteValues->SetArrayField(TEXT("AtomicInts"), {
		MakeShared<FJsonValueNumber>(1),
		MakeShared<FJsonValueNumber>(2),
		MakeShared<FJsonValueNumber>(3),
		MakeShared<FJsonValueNumber>(4)});

	const FCortexCommandResult Set = Handler.Execute(
		TEXT("set_class_defaults"), EchoSetParams(Fixture.AssetPath, WriteValues, nullptr));
	if (!TestTrue(TEXT("scalar diff set should succeed"), Set.bSuccess) || !Set.Data.IsValid())
	{
		return true;
	}

	// Case-only string change.
	TSharedPtr<FJsonObject> CaseRecord = EchoResultRecord(Set, TEXT("CaseText"));
	if (TestNotNull(TEXT("case record should exist"), CaseRecord.Get()))
	{
		bool bChanged = false;
		TestTrue(TEXT("case record declares changed"), CaseRecord->TryGetBoolField(TEXT("changed"), bChanged));
		TestTrue(TEXT("a case-only string change is a real mutation"), bChanged);
		FString Before;
		FString After;
		TestTrue(TEXT("case record reports the previous string"),
			CaseRecord->TryGetStringField(TEXT("previous_value"), Before));
		TestEqual(TEXT("case record previous string is the old casing"), Before, FString(TEXT("abc")));
		TestTrue(TEXT("case record reports the new string"),
			CaseRecord->TryGetStringField(TEXT("new_value"), After));
		TestEqual(TEXT("case record new string is the new casing"), After, FString(TEXT("ABC")));
	}

	// FText change reported through its descriptor.
	TSharedPtr<FJsonObject> TextRecord = EchoResultRecord(Set, TEXT("EchoText"));
	if (TestNotNull(TEXT("FText record should exist"), TextRecord.Get()))
	{
		bool bChanged = false;
		TestTrue(TEXT("FText record declares changed"), TextRecord->TryGetBoolField(TEXT("changed"), bChanged));
		TestTrue(TEXT("a changed FText is a real mutation"), bChanged);
		const TSharedPtr<FJsonObject>* PreviousDescriptor = nullptr;
		const TSharedPtr<FJsonObject>* NewDescriptor = nullptr;
		if (TestTrue(TEXT("FText previous value is a descriptor"),
				TextRecord->TryGetObjectField(TEXT("previous_value"), PreviousDescriptor) && PreviousDescriptor != nullptr)
			&& TestTrue(TEXT("FText new value is a descriptor"),
				TextRecord->TryGetObjectField(TEXT("new_value"), NewDescriptor) && NewDescriptor != nullptr))
		{
			FString PreviousLiteral;
			FString NewLiteral;
			TestTrue(TEXT("FText previous descriptor exposes its text"),
				(*PreviousDescriptor)->TryGetStringField(TEXT("value"), PreviousLiteral));
			TestEqual(TEXT("FText previous descriptor keeps the old text"),
				PreviousLiteral, FString(TEXT("abc")));
			TestTrue(TEXT("FText new descriptor exposes its text"),
				(*NewDescriptor)->TryGetStringField(TEXT("value"), NewLiteral));
			TestEqual(TEXT("FText new descriptor keeps the new text"),
				NewLiteral, FString(TEXT("ABC")));
		}
	}

	// Explicit null clear of a previously non-null object.
	TSharedPtr<FJsonObject> ObjectRecord = EchoResultRecord(Set, TEXT("EchoObject"));
	if (TestNotNull(TEXT("object record should exist"), ObjectRecord.Get()))
	{
		bool bChanged = false;
		TestTrue(TEXT("object record declares changed"), ObjectRecord->TryGetBoolField(TEXT("changed"), bChanged));
		TestTrue(TEXT("a non-null to null clear is a real mutation"), bChanged);
		FString PreviousPath;
		TestTrue(TEXT("object record reports the previous object path"),
			ObjectRecord->TryGetStringField(TEXT("previous_value"), PreviousPath));
		TestFalse(TEXT("the previous object path is a real object"), PreviousPath.IsEmpty());
		const TSharedPtr<FJsonValue> NewValue = ObjectRecord->TryGetField(TEXT("new_value"));
		TestTrue(TEXT("the null clear is an explicit null, not an omission"),
			NewValue.IsValid() && NewValue->IsNull());
	}

	// Atomic collection: the whole array is one value and a growth is a real change.
	TSharedPtr<FJsonObject> IntsRecord = EchoResultRecord(Set, TEXT("AtomicInts"));
	if (TestNotNull(TEXT("collection record should exist"), IntsRecord.Get()))
	{
		bool bChanged = false;
		TestTrue(TEXT("collection record declares changed"), IntsRecord->TryGetBoolField(TEXT("changed"), bChanged));
		TestTrue(TEXT("an added collection element is a real mutation"), bChanged);
		const TArray<TSharedPtr<FJsonValue>>* PreviousArray = nullptr;
		const TArray<TSharedPtr<FJsonValue>>* NewArray = nullptr;
		if (TestTrue(TEXT("collection record reports the atomic previous value"),
				IntsRecord->TryGetArrayField(TEXT("previous_value"), PreviousArray) && PreviousArray != nullptr)
			&& TestTrue(TEXT("collection record reports the atomic new value"),
				IntsRecord->TryGetArrayField(TEXT("new_value"), NewArray) && NewArray != nullptr))
		{
			TestEqual(TEXT("the previous collection is the atomic old array"), PreviousArray->Num(), 3);
			TestEqual(TEXT("the new collection is the atomic new array"), NewArray->Num(), 4);
		}
	}

	// The case-only change really landed.
	TSharedPtr<FJsonObject> ReadParams = MakeShared<FJsonObject>();
	ReadParams->SetStringField(TEXT("asset_path"), Fixture.AssetPath);
	ReadParams->SetArrayField(TEXT("properties"), {
		MakeShared<FJsonValueString>(TEXT("CaseText"))});
	const FCortexCommandResult Read = Handler.Execute(TEXT("get_class_defaults"), ReadParams);
	if (TestTrue(TEXT("scalar readback should succeed"), Read.bSuccess) && Read.Data.IsValid())
	{
		const TSharedPtr<FJsonObject>* ReadProperties = nullptr;
		const TSharedPtr<FJsonObject>* ReadCase = nullptr;
		if (TestTrue(TEXT("readback exposes properties"),
				Read.Data->TryGetObjectField(TEXT("properties"), ReadProperties) && ReadProperties != nullptr)
			&& TestTrue(TEXT("readback exposes CaseText"),
				(*ReadProperties)->TryGetObjectField(TEXT("CaseText"), ReadCase) && ReadCase != nullptr))
		{
			FString Applied;
			TestTrue(TEXT("readback exposes the string value"),
				(*ReadCase)->TryGetStringField(TEXT("value"), Applied));
			TestEqual(TEXT("the case-only change was applied"), Applied, FString(TEXT("ABC")));
		}
	}

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexBPClassDefaultsEchoDiffStructFalseAndNullClearTest,
	"Cortex.Blueprint.ClassDefaults.Set.EchoDiffStructFalseAndNullClear",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexBPClassDefaultsEchoDiffStructFalseAndNullClearTest::RunTest(const FString&)
{
	FEchoScopedFixture Fixture;
	FCortexBPCommandHandler Handler;
	UObject* CDO = Fixture.Create(
		ACortexBPEchoContractActor::StaticClass(), EchoFixtureUniquePath(TEXT("StructClear")));
	if (!TestNotNull(TEXT("echo fixture CDO should exist"), CDO))
	{
		return true;
	}

	// Only a false bool leaf and a null object leaf change; every other declared struct member must
	// stay out of the diff.
	TSharedPtr<FJsonObject> PayloadWrite = MakeShared<FJsonObject>();
	PayloadWrite->SetBoolField(TEXT("bFlag"), false);
	PayloadWrite->SetField(TEXT("Payload"), MakeShared<FJsonValueNull>());
	TSharedPtr<FJsonObject> WriteValues = MakeShared<FJsonObject>();
	WriteValues->SetObjectField(TEXT("WidePayload"), PayloadWrite);

	const FCortexCommandResult Set = Handler.Execute(
		TEXT("set_class_defaults"), EchoSetParams(Fixture.AssetPath, WriteValues, nullptr));
	if (!TestTrue(TEXT("struct clear set should succeed"), Set.bSuccess) || !Set.Data.IsValid())
	{
		return true;
	}

	TSharedPtr<FJsonObject> Record = EchoResultRecord(Set, TEXT("WidePayload"));
	if (!TestNotNull(TEXT("struct clear record should exist"), Record.Get()))
	{
		return true;
	}

	FString Type;
	TestTrue(TEXT("struct clear record keeps its declared type"), Record->TryGetStringField(TEXT("type"), Type));
	TestEqual(TEXT("struct clear record type is the payload"), Type, FString(TEXT("FCortexBPEchoPayload")));
	bool bChanged = false;
	TestTrue(TEXT("struct clear record declares changed"), Record->TryGetBoolField(TEXT("changed"), bChanged));
	TestTrue(TEXT("a false/null clear is a real mutation"), bChanged);
	TestFalse(TEXT("struct clear diff drops the complete previous value"), Record->HasField(TEXT("previous_value")));
	TestFalse(TEXT("struct clear diff drops the complete new value"), Record->HasField(TEXT("new_value")));

	const TSharedPtr<FJsonObject>* Changes = nullptr;
	if (TestTrue(TEXT("struct clear diff exposes changed leaf paths"),
		Record->TryGetObjectField(TEXT("changes"), Changes) && Changes != nullptr))
	{
		TestEqual(TEXT("only the two cleared leaves are reported"), (*Changes)->Values.Num(), 2);

		const TSharedPtr<FJsonObject>* FlagChange = nullptr;
		if (TestTrue(TEXT("the false bool leaf uses its relative path"),
			(*Changes)->TryGetObjectField(TEXT("bFlag"), FlagChange) && FlagChange != nullptr))
		{
			bool bBefore = true;
			bool bAfter = true;
			TestTrue(TEXT("the bool leaf reports its previous value"),
				(*FlagChange)->TryGetBoolField(TEXT("previous_value"), bBefore));
			TestTrue(TEXT("the bool leaf previous value is true"), bBefore);
			TestTrue(TEXT("the bool leaf reports its new value"),
				(*FlagChange)->TryGetBoolField(TEXT("new_value"), bAfter));
			TestFalse(TEXT("the bool leaf new value is false, an explicit clear"), bAfter);
		}

		const TSharedPtr<FJsonObject>* ObjectChange = nullptr;
		if (TestTrue(TEXT("the object leaf uses its relative path"),
			(*Changes)->TryGetObjectField(TEXT("Payload"), ObjectChange) && ObjectChange != nullptr))
		{
			FString PreviousPath;
			TestTrue(TEXT("the object leaf reports its previous object"),
				(*ObjectChange)->TryGetStringField(TEXT("previous_value"), PreviousPath));
			TestFalse(TEXT("the previous object path is a real object"), PreviousPath.IsEmpty());
			const TSharedPtr<FJsonValue> NewValue = (*ObjectChange)->TryGetField(TEXT("new_value"));
			TestTrue(TEXT("the object clear is an explicit null"),
				NewValue.IsValid() && NewValue->IsNull());
		}

		TestFalse(TEXT("the unchanged int64 leaf stays out of the diff"), (*Changes)->HasField(TEXT("Big")));
		TestFalse(TEXT("the unchanged int leaf stays out of the diff"), (*Changes)->HasField(TEXT("Small")));
		TestFalse(TEXT("the unchanged string leaf stays out of the diff"), (*Changes)->HasField(TEXT("Tag")));
		TestFalse(TEXT("the unchanged text leaf stays out of the diff"), (*Changes)->HasField(TEXT("Label")));
	}

	// The cleared leaves really landed while the siblings kept their baseline.
	TSharedPtr<FJsonObject> ReadParams = MakeShared<FJsonObject>();
	ReadParams->SetStringField(TEXT("asset_path"), Fixture.AssetPath);
	ReadParams->SetArrayField(TEXT("properties"), {
		MakeShared<FJsonValueString>(TEXT("WidePayload"))});
	const FCortexCommandResult Read = Handler.Execute(TEXT("get_class_defaults"), ReadParams);
	if (TestTrue(TEXT("struct clear readback should succeed"), Read.bSuccess) && Read.Data.IsValid())
	{
		const TSharedPtr<FJsonObject>* ReadProperties = nullptr;
		const TSharedPtr<FJsonObject>* ReadPayload = nullptr;
		const TSharedPtr<FJsonObject>* ReadValue = nullptr;
		if (TestTrue(TEXT("readback exposes properties"),
				Read.Data->TryGetObjectField(TEXT("properties"), ReadProperties) && ReadProperties != nullptr)
			&& TestTrue(TEXT("readback exposes WidePayload"),
				(*ReadProperties)->TryGetObjectField(TEXT("WidePayload"), ReadPayload) && ReadPayload != nullptr)
			&& TestTrue(TEXT("readback exposes the struct value"),
				(*ReadPayload)->TryGetObjectField(TEXT("value"), ReadValue) && ReadValue != nullptr))
		{
			bool bFlag = true;
			TestTrue(TEXT("readback exposes the bool leaf"), (*ReadValue)->TryGetBoolField(TEXT("bFlag"), bFlag));
			TestFalse(TEXT("the bool leaf is false after the clear"), bFlag);
			const TSharedPtr<FJsonValue> PayloadValue = (*ReadValue)->TryGetField(TEXT("Payload"));
			TestTrue(TEXT("the object leaf is null after the clear"),
				PayloadValue.IsValid() && PayloadValue->IsNull());
			double Small = -1.0;
			TestTrue(TEXT("readback exposes the int leaf"), (*ReadValue)->TryGetNumberField(TEXT("Small"), Small));
			TestEqual(TEXT("the int sibling kept its baseline"), static_cast<int32>(Small), 7);
		}
	}

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexBPClassDefaultsEchoFullNoneAndModeIdenticalApplicationTest,
	"Cortex.Blueprint.ClassDefaults.Set.EchoFullNoneAndModeIdenticalApplication",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexBPClassDefaultsEchoFullNoneAndModeIdenticalApplicationTest::RunTest(const FString&)
{
	FEchoScopedFixture Fixture;
	FCortexBPCommandHandler Handler;
	UObject* CDO = Fixture.Create(
		ACortexBPEchoContractActor::StaticClass(), EchoFixtureUniquePath(TEXT("Modes")));
	if (!TestNotNull(TEXT("echo fixture CDO should exist"), CDO))
	{
		return true;
	}

	FStructProperty* VectorProperty = FindFProperty<FStructProperty>(CDO->GetClass(), TEXT("TokenVector"));
	if (!TestNotNull(TEXT("the fixture should expose TokenVector as a struct property"), VectorProperty))
	{
		return true;
	}

	// Resets the native struct root to its baseline between runs so each mode is compared on the same
	// previous state.
	const auto ResetVector = [&]()
	{
		FVector* Vector = static_cast<FVector*>(VectorProperty->ContainerPtrToValuePtr<void>(CDO));
		if (Vector)
		{
			*Vector = FVector(1.0, 2.0, 3.0);
		}
	};

	const auto ReadVector = [&]() -> FVector
	{
		const FVector* Vector = static_cast<const FVector*>(VectorProperty->ContainerPtrToValuePtr<void>(CDO));
		return Vector ? *Vector : FVector::ZeroVector;
	};

	// Full echo preserves the complete previous/new representation for a changed struct root.
	ResetVector();
	TSharedPtr<FJsonObject> VectorWrite = MakeShared<FJsonObject>();
	VectorWrite->SetNumberField(TEXT("X"), 7);
	TSharedPtr<FJsonObject> FullValues = MakeShared<FJsonObject>();
	FullValues->SetObjectField(TEXT("TokenVector"), VectorWrite);
	const FCortexCommandResult Full = Handler.Execute(
		TEXT("set_class_defaults"), EchoSetParams(Fixture.AssetPath, FullValues, TEXT("full")));
	if (TestTrue(TEXT("full set should succeed"), Full.bSuccess) && Full.Data.IsValid())
	{
		TSharedPtr<FJsonObject> Record = EchoResultRecord(Full, TEXT("TokenVector"));
		if (TestNotNull(TEXT("full struct record should exist"), Record.Get()))
		{
			bool bChanged = false;
			TestTrue(TEXT("full struct record declares changed"),
				Record->TryGetBoolField(TEXT("changed"), bChanged));
			TestTrue(TEXT("full struct record reports a real change"), bChanged);
			const TSharedPtr<FJsonObject>* Previous = nullptr;
			const TSharedPtr<FJsonObject>* New = nullptr;
			if (TestTrue(TEXT("full struct record keeps the complete previous value"),
					Record->TryGetObjectField(TEXT("previous_value"), Previous) && Previous != nullptr)
				&& TestTrue(TEXT("full struct record keeps the complete new value"),
					Record->TryGetObjectField(TEXT("new_value"), New) && New != nullptr))
			{
				double PreviousX = -1.0;
				double PreviousY = -1.0;
				double PreviousZ = -1.0;
				double NewX = -1.0;
				double NewY = -1.0;
				double NewZ = -1.0;
				TestTrue(TEXT("full previous exposes X"), (*Previous)->TryGetNumberField(TEXT("X"), PreviousX));
				TestTrue(TEXT("full previous exposes Y"), (*Previous)->TryGetNumberField(TEXT("Y"), PreviousY));
				TestTrue(TEXT("full previous exposes Z"), (*Previous)->TryGetNumberField(TEXT("Z"), PreviousZ));
				TestTrue(TEXT("full new exposes X"), (*New)->TryGetNumberField(TEXT("X"), NewX));
				TestTrue(TEXT("full new exposes Y"), (*New)->TryGetNumberField(TEXT("Y"), NewY));
				TestTrue(TEXT("full new exposes Z"), (*New)->TryGetNumberField(TEXT("Z"), NewZ));
				TestEqual(TEXT("full previous X is complete"), static_cast<int32>(PreviousX), 1);
				TestEqual(TEXT("full previous Y is complete"), static_cast<int32>(PreviousY), 2);
				TestEqual(TEXT("full previous Z is complete"), static_cast<int32>(PreviousZ), 3);
				TestEqual(TEXT("full new X is complete"), static_cast<int32>(NewX), 7);
				TestEqual(TEXT("full new Y is complete"), static_cast<int32>(NewY), 2);
				TestEqual(TEXT("full new Z is complete"), static_cast<int32>(NewZ), 3);
			}
		}
	}

	// None omits the value echo entirely while the authoritative outcome survives.
	ResetVector();
	const FCortexCommandResult None = Handler.Execute(
		TEXT("set_class_defaults"), EchoSetParams(Fixture.AssetPath, FullValues, TEXT("none")));
	if (TestTrue(TEXT("none set should succeed"), None.bSuccess) && None.Data.IsValid())
	{
		bool bNoneCompiled = true;
		bool bNoneSaved = true;
		TestTrue(TEXT("none set reports compile status"),
			None.Data->TryGetBoolField(TEXT("compiled"), bNoneCompiled));
		TestFalse(TEXT("none compile=false keeps compiled false"), bNoneCompiled);
		TestTrue(TEXT("none set reports save status"),
			None.Data->TryGetBoolField(TEXT("saved"), bNoneSaved));
		TestFalse(TEXT("none save=false keeps saved false"), bNoneSaved);

		TSharedPtr<FJsonObject> Record = EchoResultRecord(None, TEXT("TokenVector"));
		if (TestNotNull(TEXT("none struct record should exist"), Record.Get()))
		{
			FString Type;
			TestTrue(TEXT("none struct record keeps its type"), Record->TryGetStringField(TEXT("type"), Type));
			TestEqual(TEXT("none struct record type is the struct"), Type, FString(TEXT("FVector")));
			bool bSuccess = false;
			TestTrue(TEXT("none struct record keeps success"), Record->TryGetBoolField(TEXT("success"), bSuccess));
			TestTrue(TEXT("none struct record keeps the authoritative outcome"), bSuccess);
			bool bChanged = false;
			TestTrue(TEXT("none struct record keeps changed"), Record->TryGetBoolField(TEXT("changed"), bChanged));
			TestTrue(TEXT("none struct record reports the real change"), bChanged);
			TestFalse(TEXT("none omits the previous value"), Record->HasField(TEXT("previous_value")));
			TestFalse(TEXT("none omits the new value"), Record->HasField(TEXT("new_value")));
			TestFalse(TEXT("none omits the changed leaf paths"), Record->HasField(TEXT("changes")));
		}
	}

	// Every mode applies the same value, the same compile/save status and the same failure recovery.
	for (const TCHAR* Mode : { TEXT("diff"), TEXT("full"), TEXT("none") })
	{
		ResetVector();
		TSharedPtr<FJsonObject> VectorValue = MakeShared<FJsonObject>();
		VectorValue->SetNumberField(TEXT("X"), 9);
		TSharedPtr<FJsonObject> Values = MakeShared<FJsonObject>();
		Values->SetObjectField(TEXT("TokenVector"), VectorValue);
		const FCortexCommandResult Applied = Handler.Execute(
			TEXT("set_class_defaults"), EchoSetParams(Fixture.AssetPath, Values, Mode));
		TestTrue(*FString::Printf(TEXT("%s mode applies the value"), Mode), Applied.bSuccess);
		if (Applied.Data.IsValid())
		{
			bool bAppliedCompiled = true;
			bool bAppliedSaved = true;
			TestTrue(*FString::Printf(TEXT("%s mode reports compile status"), Mode),
				Applied.Data->TryGetBoolField(TEXT("compiled"), bAppliedCompiled));
			TestFalse(*FString::Printf(TEXT("%s mode keeps compiled false"), Mode), bAppliedCompiled);
			TestTrue(*FString::Printf(TEXT("%s mode reports save status"), Mode),
				Applied.Data->TryGetBoolField(TEXT("saved"), bAppliedSaved));
			TestFalse(*FString::Printf(TEXT("%s mode keeps saved false"), Mode), bAppliedSaved);
		}
		TestEqual(*FString::Printf(TEXT("%s mode applied X"), Mode),
			static_cast<int32>(ReadVector().X), 9);
		TestEqual(*FString::Printf(TEXT("%s mode kept Y"), Mode),
			static_cast<int32>(ReadVector().Y), 2);
		TestEqual(*FString::Printf(TEXT("%s mode kept Z"), Mode),
			static_cast<int32>(ReadVector().Z), 3);

		// The same later-property failure rolls the applied value back identically in every mode.
		ResetVector();
		TSharedPtr<FJsonObject> FailValues = MakeShared<FJsonObject>();
		FailValues->SetObjectField(TEXT("TokenVector"), VectorValue);
		FailValues->SetBoolField(TEXT("CortexEchoAbsentProperty"), true);
		const FCortexCommandResult Failed = Handler.Execute(
			TEXT("set_class_defaults"), EchoSetParams(Fixture.AssetPath, FailValues, Mode));
		TestFalse(*FString::Printf(TEXT("%s mode fails on the later property"), Mode), Failed.bSuccess);
		TestEqual(*FString::Printf(TEXT("%s mode reports PROPERTY_NOT_FOUND"), Mode),
			Failed.ErrorCode, CortexErrorCodes::PropertyNotFound);
		TestEqual(*FString::Printf(TEXT("%s mode rolled X back"), Mode),
			static_cast<int32>(ReadVector().X), 1);
		TestEqual(*FString::Printf(TEXT("%s mode kept Y during rollback"), Mode),
			static_cast<int32>(ReadVector().Y), 2);
		TestEqual(*FString::Printf(TEXT("%s mode kept Z during rollback"), Mode),
			static_cast<int32>(ReadVector().Z), 3);
	}

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexBPClassDefaultsEchoPrecisionInt64AndRollbackTest,
	"Cortex.Blueprint.ClassDefaults.Set.EchoPrecisionInt64ExactDecimalAndRollback",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexBPClassDefaultsEchoPrecisionInt64AndRollbackTest::RunTest(const FString&)
{
	FEchoScopedFixture Fixture;
	FCortexBPCommandHandler Handler;
	UObject* CDO = Fixture.Create(
		ACortexBPEchoContractActor::StaticClass(), EchoFixtureUniquePath(TEXT("Precision")));
	if (!TestNotNull(TEXT("echo fixture CDO should exist"), CDO))
	{
		return true;
	}

	UClass* CDOClass = CDO->GetClass();
	FInt64Property* WideCounterProperty = FindFProperty<FInt64Property>(CDOClass, TEXT("WideCounter"));
	FStructProperty* PayloadProperty = FindFProperty<FStructProperty>(CDOClass, TEXT("WidePayload"));
	FInt64Property* PayloadBigProperty = PayloadProperty
		? FindFProperty<FInt64Property>(PayloadProperty->Struct, TEXT("Big"))
		: nullptr;
	FArrayProperty* WideArrayProperty = FindFProperty<FArrayProperty>(CDOClass, TEXT("WideArray"));
	FInt64Property* WideArrayInnerProperty = WideArrayProperty
		? CastField<FInt64Property>(WideArrayProperty->Inner)
		: nullptr;
	if (!TestNotNull(TEXT("the fixture should expose the scalar int64 root"), WideCounterProperty)
		|| !TestNotNull(TEXT("the fixture should expose the struct root"), PayloadProperty)
		|| !TestNotNull(TEXT("the struct root should expose its int64 leaf"), PayloadBigProperty)
		|| !TestNotNull(TEXT("the fixture should expose the array root"), WideArrayProperty)
		|| !TestNotNull(TEXT("the array root should contain int64"), WideArrayInnerProperty))
	{
		return true;
	}

	// 9007199254740993 is NOT exactly representable as a double; 9007199254740992 is.
	constexpr int64 SeedValue = 9007199254740993LL;
	constexpr int64 RequestedValue = 9007199254740992LL;

	// Seeds every precision-sensitive root directly through FInt64Property. The legacy JSON getter is
	// never used to establish the expected previous value.
	const auto SeedPrecisionLeaves = [&]()
	{
		WideCounterProperty->SetPropertyValue(
			WideCounterProperty->ContainerPtrToValuePtr<int64>(CDO), SeedValue);

		void* PayloadPtr = PayloadProperty->ContainerPtrToValuePtr<void>(CDO);
		PayloadBigProperty->SetPropertyValue(
			PayloadBigProperty->ContainerPtrToValuePtr<int64>(PayloadPtr), SeedValue);

		FScriptArrayHelper ArrayHelper(
			WideArrayProperty, WideArrayProperty->ContainerPtrToValuePtr<void>(CDO));
		ArrayHelper.EmptyValues();
		const int32 Index = ArrayHelper.AddValue();
		WideArrayInnerProperty->SetPropertyValue(ArrayHelper.GetRawPtr(Index), SeedValue);
	};

	SeedPrecisionLeaves();

	// The setter can only carry an exactly representable double, so the requested 9007199254740992 is
	// written while the seeded 9007199254740993 is not representable. A rounded JSON comparison would
	// claim "unchanged"; the exact typed comparison must report a real change with decimal strings.
	TSharedPtr<FJsonObject> WriteValues = MakeShared<FJsonObject>();
	WriteValues->SetNumberField(TEXT("WideCounter"), static_cast<double>(RequestedValue));
	TSharedPtr<FJsonObject> RequestedPayload = MakeShared<FJsonObject>();
	RequestedPayload->SetNumberField(TEXT("Big"), static_cast<double>(RequestedValue));
	WriteValues->SetObjectField(TEXT("WidePayload"), RequestedPayload);
	WriteValues->SetArrayField(TEXT("WideArray"), {
		MakeShared<FJsonValueNumber>(static_cast<double>(RequestedValue))});

	const FCortexCommandResult Set = Handler.Execute(
		TEXT("set_class_defaults"), EchoSetParams(Fixture.AssetPath, WriteValues, TEXT("diff")));
	if (!TestTrue(TEXT("precision set should succeed"), Set.bSuccess) || !Set.Data.IsValid())
	{
		return true;
	}

	// Scalar precision root.
	{
		TSharedPtr<FJsonObject> Record = EchoResultRecord(Set, TEXT("WideCounter"));
		if (TestNotNull(TEXT("scalar precision record should exist"), Record.Get()))
		{
			bool bChanged = false;
			TestTrue(TEXT("scalar precision record declares changed"),
				Record->TryGetBoolField(TEXT("changed"), bChanged));
			TestTrue(TEXT("the exact typed comparison reports a real change"), bChanged);
			FString Encoding;
			TestTrue(TEXT("the scalar precision record declares its value encoding"),
				Record->TryGetStringField(TEXT("value_encoding"), Encoding));
			TestEqual(TEXT("the scalar precision record uses decimal strings"),
				Encoding, FString(TEXT("decimal_string")));
			FString Before;
			FString After;
			TestTrue(TEXT("the scalar precision record reports an exact previous value"),
				Record->TryGetStringField(TEXT("previous_value"), Before));
			TestEqual(TEXT("the scalar previous value is the exact seeded integer"),
				Before, FString(TEXT("9007199254740993")));
			TestTrue(TEXT("the scalar precision record reports an exact new value"),
				Record->TryGetStringField(TEXT("new_value"), After));
			TestEqual(TEXT("the scalar new value is the exact requested integer"),
				After, FString(TEXT("9007199254740992")));
		}
	}

	// Struct root containing an int64 leaf.
	{
		TSharedPtr<FJsonObject> Record = EchoResultRecord(Set, TEXT("WidePayload"));
		if (TestNotNull(TEXT("struct precision record should exist"), Record.Get()))
		{
			bool bChanged = false;
			TestTrue(TEXT("struct precision record declares changed"),
				Record->TryGetBoolField(TEXT("changed"), bChanged));
			TestTrue(TEXT("the struct int64 leaf change is a real mutation"), bChanged);
			TestFalse(TEXT("struct precision diff drops the complete previous value"),
				Record->HasField(TEXT("previous_value")));

			const TSharedPtr<FJsonObject>* Changes = nullptr;
			if (TestTrue(TEXT("struct precision diff exposes changed leaf paths"),
				Record->TryGetObjectField(TEXT("changes"), Changes) && Changes != nullptr))
			{
				const TSharedPtr<FJsonObject>* BigChange = nullptr;
				if (TestTrue(TEXT("the struct int64 leaf uses its relative path"),
					(*Changes)->TryGetObjectField(TEXT("Big"), BigChange) && BigChange != nullptr))
				{
					FString Before;
					FString After;
					TestTrue(TEXT("the struct int64 leaf reports an exact previous value"),
						(*BigChange)->TryGetStringField(TEXT("previous_value"), Before));
					TestEqual(TEXT("the struct int64 previous value is the exact seeded integer"),
						Before, FString(TEXT("9007199254740993")));
					TestTrue(TEXT("the struct int64 leaf reports an exact new value"),
						(*BigChange)->TryGetStringField(TEXT("new_value"), After));
					TestEqual(TEXT("the struct int64 new value is the exact requested integer"),
						After, FString(TEXT("9007199254740992")));
				}
				TestFalse(TEXT("the unchanged int leaf stays out of the precision diff"),
					(*Changes)->HasField(TEXT("Small")));
			}

			const TSharedPtr<FJsonObject>* Encodings = nullptr;
			if (TestTrue(TEXT("the struct precision record declares per-path encodings"),
				Record->TryGetObjectField(TEXT("value_encodings"), Encodings) && Encodings != nullptr))
			{
				FString BigEncoding;
				TestTrue(TEXT("the struct encoding names the int64 leaf path"),
					(*Encodings)->TryGetStringField(TEXT("Big"), BigEncoding));
				TestEqual(TEXT("the struct int64 leaf uses decimal strings"),
					BigEncoding, FString(TEXT("decimal_string")));
			}
		}
	}

	// Atomic collection root containing an int64 element: the collection stays atomic, but the unsafe
	// element is still reported as an encoding instead of being labelled unchanged.
	{
		TSharedPtr<FJsonObject> Record = EchoResultRecord(Set, TEXT("WideArray"));
		if (TestNotNull(TEXT("collection precision record should exist"), Record.Get()))
		{
			bool bChanged = false;
			TestTrue(TEXT("collection precision record declares changed"),
				Record->TryGetBoolField(TEXT("changed"), bChanged));
			TestTrue(TEXT("an int64 element change is not rounded-equal"), bChanged);

			const TArray<TSharedPtr<FJsonValue>>* PreviousArray = nullptr;
			const TArray<TSharedPtr<FJsonValue>>* NewArray = nullptr;
			if (TestTrue(TEXT("the collection reports its atomic previous value"),
					Record->TryGetArrayField(TEXT("previous_value"), PreviousArray) && PreviousArray != nullptr)
				&& TestTrue(TEXT("the collection reports its atomic new value"),
					Record->TryGetArrayField(TEXT("new_value"), NewArray) && NewArray != nullptr))
			{
				TestEqual(TEXT("the collection previous value keeps its element count"), PreviousArray->Num(), 1);
				TestEqual(TEXT("the collection new value keeps its element count"), NewArray->Num(), 1);
				FString PreviousElement;
				FString NewElement;
				TestTrue(TEXT("the collection previous element is an exact decimal string"),
					PreviousArray->Num() == 1 && (*PreviousArray)[0].IsValid() && (*PreviousArray)[0]->TryGetString(PreviousElement));
				TestEqual(TEXT("the collection previous element is the exact seeded integer"),
					PreviousElement, FString(TEXT("9007199254740993")));
				TestTrue(TEXT("the collection new element is an exact decimal string"),
					NewArray->Num() == 1 && (*NewArray)[0].IsValid() && (*NewArray)[0]->TryGetString(NewElement));
				TestEqual(TEXT("the collection new element is the exact requested integer"),
					NewElement, FString(TEXT("9007199254740992")));
			}

			const TSharedPtr<FJsonObject>* Encodings = nullptr;
			if (TestTrue(TEXT("the collection precision record declares per-path encodings"),
				Record->TryGetObjectField(TEXT("value_encodings"), Encodings) && Encodings != nullptr))
			{
				TestEqual(TEXT("the collection reports exactly one encoded element path"),
					(*Encodings)->Values.Num(), 1);
				FString ElementEncoding;
				TestTrue(TEXT("the encoded element path is the array index"),
					(*Encodings)->TryGetStringField(TEXT("0"), ElementEncoding));
				TestEqual(TEXT("the collection element uses decimal strings"),
					ElementEncoding, FString(TEXT("decimal_string")));
			}
		}
	}

	// Native reflection verifies the actual post-state; the legacy lossy getter is never the oracle.
	{
		void* PayloadPtr = PayloadProperty->ContainerPtrToValuePtr<void>(CDO);
		FScriptArrayHelper ArrayHelper(
			WideArrayProperty, WideArrayProperty->ContainerPtrToValuePtr<void>(CDO));
		TestTrue(TEXT("the scalar post-state is the requested exactly representable value"),
			EchoReadInt64(CDO, WideCounterProperty) == RequestedValue);
		TestTrue(TEXT("the struct int64 post-state is the requested value"),
			EchoReadInt64(CDO, PayloadBigProperty,
				PayloadBigProperty->ContainerPtrToValuePtr<void>(PayloadPtr)) == RequestedValue);
		TestTrue(TEXT("the collection int64 post-state is the requested value"),
			ArrayHelper.Num() == 1
				&& EchoReadInt64(CDO, WideArrayInnerProperty, ArrayHelper.GetRawPtr(0)) == RequestedValue);
	}

	// A later property failure (observed through the existing property-change event, exactly as the
	// production setter broadcasts it) must restore every exact previous integer in every echo mode,
	// including the struct/array container shapes, and keep the ordinary journal value too.
	for (const TCHAR* Mode : { TEXT("diff"), TEXT("full"), TEXT("none") })
	{
		SeedPrecisionLeaves();

		bool bObservedAssignment = false;
		const FDelegateHandle Handle = FCoreUObjectDelegates::OnObjectPropertyChanged.AddLambda(
			[CDO, WideCounterProperty, &bObservedAssignment](UObject* Object, FPropertyChangedEvent& Event)
			{
				if (Object == CDO && Event.MemberProperty == WideCounterProperty)
				{
					bObservedAssignment = true;
				}
			});

		TSharedPtr<FJsonObject> FailedValues = MakeShared<FJsonObject>();
		FailedValues->SetNumberField(TEXT("WideCounter"), static_cast<double>(RequestedValue));
		TSharedPtr<FJsonObject> FailedPayload = MakeShared<FJsonObject>();
		FailedPayload->SetNumberField(TEXT("Big"), static_cast<double>(RequestedValue));
		FailedValues->SetObjectField(TEXT("WidePayload"), FailedPayload);
		FailedValues->SetArrayField(TEXT("WideArray"), {
			MakeShared<FJsonValueNumber>(static_cast<double>(RequestedValue))});
		FailedValues->SetStringField(TEXT("CaseText"), TEXT("ABC"));
		FailedValues->SetBoolField(TEXT("CortexEchoAbsentProperty"), true);

		const FCortexCommandResult Failed = Handler.Execute(
			TEXT("set_class_defaults"), EchoSetParams(Fixture.AssetPath, FailedValues, Mode));

		FCoreUObjectDelegates::OnObjectPropertyChanged.Remove(Handle);

		TestFalse(*FString::Printf(TEXT("%s mode fails on the later property"), Mode), Failed.bSuccess);
		TestEqual(*FString::Printf(TEXT("%s mode reports PROPERTY_NOT_FOUND"), Mode),
			Failed.ErrorCode, CortexErrorCodes::PropertyNotFound);
		TestTrue(*FString::Printf(TEXT("%s mode observed the first assignment before the failure"), Mode),
			bObservedAssignment);

		void* PayloadPtr = PayloadProperty->ContainerPtrToValuePtr<void>(CDO);
		FScriptArrayHelper ArrayHelper(
			WideArrayProperty, WideArrayProperty->ContainerPtrToValuePtr<void>(CDO));
		TestTrue(*FString::Printf(TEXT("%s mode restores the scalar previous integer exactly"), Mode),
			EchoReadInt64(CDO, WideCounterProperty) == SeedValue);
		TestTrue(*FString::Printf(TEXT("%s mode restores the struct int64 previous value exactly"), Mode),
			EchoReadInt64(CDO, PayloadBigProperty,
				PayloadBigProperty->ContainerPtrToValuePtr<void>(PayloadPtr)) == SeedValue);
		TestTrue(*FString::Printf(TEXT("%s mode restores the collection int64 previous value exactly"), Mode),
			ArrayHelper.Num() == 1
				&& EchoReadInt64(CDO, WideArrayInnerProperty, ArrayHelper.GetRawPtr(0)) == SeedValue);

		// The ordinary (non-precision) journal value is still rolled back through the read contract.
		TSharedPtr<FJsonObject> ReadParams = MakeShared<FJsonObject>();
		ReadParams->SetStringField(TEXT("asset_path"), Fixture.AssetPath);
		ReadParams->SetArrayField(TEXT("properties"), {
			MakeShared<FJsonValueString>(TEXT("CaseText"))});
		const FCortexCommandResult Read = Handler.Execute(TEXT("get_class_defaults"), ReadParams);
		if (TestTrue(*FString::Printf(TEXT("%s mode rollback readback should succeed"), Mode),
				Read.bSuccess) && Read.Data.IsValid())
		{
			const TSharedPtr<FJsonObject>* ReadProperties = nullptr;
			const TSharedPtr<FJsonObject>* ReadCase = nullptr;
			if (TestTrue(*FString::Printf(TEXT("%s mode rollback readback exposes properties"), Mode),
					Read.Data->TryGetObjectField(TEXT("properties"), ReadProperties) && ReadProperties != nullptr)
				&& TestTrue(*FString::Printf(TEXT("%s mode rollback readback exposes CaseText"), Mode),
					(*ReadProperties)->TryGetObjectField(TEXT("CaseText"), ReadCase) && ReadCase != nullptr))
			{
				FString Restored;
				TestTrue(*FString::Printf(TEXT("%s mode rollback exposes the restored string"), Mode),
					(*ReadCase)->TryGetStringField(TEXT("value"), Restored));
				TestEqual(*FString::Printf(TEXT("%s mode restored the journal previous string"), Mode),
					Restored, FString(TEXT("abc")));
			}
		}
	}

	return true;
}

// ============================================================================
// Task 14 review fix F1 — a special-encoded struct root (FSoftObjectPath / FGameplayTag /
// FGameplayTagContainer / FInstancedStruct) has no plain member object, so a real change must be
// echoed as one atomic before/after value and never as an empty complete diff.
// ============================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexBPClassDefaultsEchoSpecialEncodedAtomicTest,
	"Cortex.Blueprint.ClassDefaults.Set.EchoSpecialEncodedAtomic",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexBPClassDefaultsEchoSpecialEncodedAtomicTest::RunTest(const FString&)
{
	FEchoScopedFixture Fixture;
	FCortexBPCommandHandler Handler;
	UObject* CDO = Fixture.Create(
		ACortexBPEchoContractActor::StaticClass(), EchoFixtureUniquePath(TEXT("Special")));
	if (!TestNotNull(TEXT("special-encoding fixture CDO should exist"), CDO))
	{
		return true;
	}

	UClass* CDOClass = CDO->GetClass();
	FStructProperty* SoftPathProperty = FindFProperty<FStructProperty>(CDOClass, TEXT("EchoSoftPath"));
	FStructProperty* TagsProperty = FindFProperty<FStructProperty>(CDOClass, TEXT("EchoTags"));
	FStructProperty* InstanceProperty = FindFProperty<FStructProperty>(CDOClass, TEXT("EchoInstance"));
	if (!TestNotNull(TEXT("fixture exposes the soft object path root"), SoftPathProperty)
		|| !TestNotNull(TEXT("fixture exposes the gameplay tag container root"), TagsProperty)
		|| !TestNotNull(TEXT("fixture exposes the instanced struct root"), InstanceProperty))
	{
		return true;
	}

	// A real registered tag so the container write is unambiguous. Registered at most once per process
	// so a repeated run never duplicates the native tag registration.
	static const FGameplayTag EchoTag = UGameplayTagsManager::Get().AddNativeGameplayTag(
		FName(TEXT("Cortex.Echo.Tag")), TEXT("Cortex echo regression tag"));
	TestTrue(TEXT("the regression gameplay tag is registered"), EchoTag.IsValid());
	TestTrue(TEXT("the regression tag resolves by name"),
		FGameplayTag::RequestGameplayTag(FName(TEXT("Cortex.Echo.Tag")), false).IsValid());

	// UHT strips the F/U/A prefix from the reflected name, so _struct_type carries the struct's
	// reflected name (CortexBPEchoInstancePayload), exactly what the serializer emits.
	const FString InstanceStructName = FCortexBPEchoInstancePayload::StaticStruct()->GetName();

	TSharedPtr<FJsonObject> WriteValues = MakeShared<FJsonObject>();
	WriteValues->SetStringField(TEXT("EchoSoftPath"), TEXT("/Game/B.B"));
	WriteValues->SetArrayField(TEXT("EchoTags"), {
		MakeShared<FJsonValueString>(TEXT("Cortex.Echo.Tag"))});
	TSharedPtr<FJsonObject> InstanceWrite = MakeShared<FJsonObject>();
	InstanceWrite->SetStringField(TEXT("_struct_type"), InstanceStructName);
	InstanceWrite->SetNumberField(TEXT("Big"), 5.0);
	WriteValues->SetObjectField(TEXT("EchoInstance"), InstanceWrite);

	const FCortexCommandResult Set = Handler.Execute(
		TEXT("set_class_defaults"), EchoSetParams(Fixture.AssetPath, WriteValues, nullptr));
	if (!TestTrue(*FString::Printf(TEXT("special-encoding set should succeed (error %s: %s)"),
			*Set.ErrorCode, *Set.ErrorMessage), Set.bSuccess)
		|| !Set.Data.IsValid())
	{
		return true;
	}

	// Real post-state, read through native reflection.
	const FSoftObjectPath* StoredSoftPath =
		static_cast<const FSoftObjectPath*>(SoftPathProperty->ContainerPtrToValuePtr<void>(CDO));
	TestEqual(TEXT("the soft object path really moved"),
		StoredSoftPath ? StoredSoftPath->ToString() : FString(), FString(TEXT("/Game/B.B")));
	const FGameplayTagContainer* StoredTags =
		static_cast<const FGameplayTagContainer*>(TagsProperty->ContainerPtrToValuePtr<void>(CDO));
	TestTrue(TEXT("the tag container really gained the tag"), StoredTags && StoredTags->HasTag(EchoTag));
	TestTrue(TEXT("the runtime payload really holds the written value"),
		EchoReadInstanceBig(CDO, InstanceProperty) == 5);

	// Soft object path: one atomic changed value, never an empty leaf diff.
	TSharedPtr<FJsonObject> SoftRecord = EchoResultRecord(Set, TEXT("EchoSoftPath"));
	if (TestNotNull(TEXT("soft object path record should exist"), SoftRecord.Get()))
	{
		bool bChanged = false;
		TestTrue(TEXT("soft object path declares changed"),
			SoftRecord->TryGetBoolField(TEXT("changed"), bChanged));
		TestTrue(TEXT("soft object path is a real change"), bChanged);
		FString Before;
		FString After;
		TestTrue(TEXT("soft object path reports the previous value"),
			SoftRecord->TryGetStringField(TEXT("previous_value"), Before));
		TestEqual(TEXT("soft object path previous value is the seeded path"),
			Before, FString(TEXT("/Game/A.A")));
		TestTrue(TEXT("soft object path reports the new value"),
			SoftRecord->TryGetStringField(TEXT("new_value"), After));
		TestEqual(TEXT("soft object path new value is the requested path"),
			After, FString(TEXT("/Game/B.B")));
		TestFalse(TEXT("soft object path is not published as empty leaf changes"),
			SoftRecord->HasField(TEXT("changes")));
	}

	// Gameplay tag container: one atomic array before/after.
	TSharedPtr<FJsonObject> TagsRecord = EchoResultRecord(Set, TEXT("EchoTags"));
	if (TestNotNull(TEXT("tag container record should exist"), TagsRecord.Get()))
	{
		bool bChanged = false;
		TestTrue(TEXT("tag container declares changed"),
			TagsRecord->TryGetBoolField(TEXT("changed"), bChanged));
		TestTrue(TEXT("tag container is a real change"), bChanged);
		const TArray<TSharedPtr<FJsonValue>>* Before = nullptr;
		const TArray<TSharedPtr<FJsonValue>>* After = nullptr;
		if (TestTrue(TEXT("tag container reports the previous array"),
				TagsRecord->TryGetArrayField(TEXT("previous_value"), Before) && Before != nullptr)
			&& TestTrue(TEXT("tag container reports the new array"),
				TagsRecord->TryGetArrayField(TEXT("new_value"), After) && After != nullptr))
		{
			TestEqual(TEXT("tag container previous array was empty"), Before->Num(), 0);
			TestEqual(TEXT("tag container new array holds the tag"), After->Num(), 1);
			FString TagName;
			TestTrue(TEXT("the new tag array exposes the tag name"),
				After->Num() == 1 && (*After)[0].IsValid() && (*After)[0]->TryGetString(TagName));
			TestEqual(TEXT("the new tag array holds the registered tag"),
				TagName, FString(TEXT("Cortex.Echo.Tag")));
		}
		TestFalse(TEXT("tag container is not published as empty leaf changes"),
			TagsRecord->HasField(TEXT("changes")));
	}

	// Instanced struct: the runtime payload is one atomic before/after object.
	TSharedPtr<FJsonObject> InstanceRecord = EchoResultRecord(Set, TEXT("EchoInstance"));
	if (TestNotNull(TEXT("instanced struct record should exist"), InstanceRecord.Get()))
	{
		bool bChanged = false;
		TestTrue(TEXT("instanced struct declares changed"),
			InstanceRecord->TryGetBoolField(TEXT("changed"), bChanged));
		TestTrue(TEXT("instanced struct is a real change"), bChanged);
		const TSharedPtr<FJsonValue> PreviousValue = InstanceRecord->TryGetField(TEXT("previous_value"));
		TestTrue(TEXT("instanced struct previous value was an invalid payload"),
			PreviousValue.IsValid() && PreviousValue->IsNull());
		const TSharedPtr<FJsonObject>* NewObject = nullptr;
		if (TestTrue(TEXT("instanced struct reports the new payload object"),
			InstanceRecord->TryGetObjectField(TEXT("new_value"), NewObject) && NewObject != nullptr))
		{
			FString StructType;
			TestTrue(TEXT("the payload names its runtime struct"),
				(*NewObject)->TryGetStringField(TEXT("_struct_type"), StructType));
			TestEqual(TEXT("the payload struct uses the reflected runtime struct name"),
				StructType, FString(TEXT("CortexBPEchoInstancePayload")));
			double Big = -1.0;
			TestTrue(TEXT("the payload reports the written big value"),
				(*NewObject)->TryGetNumberField(TEXT("Big"), Big));
			TestEqual(TEXT("the payload big value is the written value"),
				static_cast<int32>(Big), 5);
		}
		TestFalse(TEXT("instanced struct is not published as empty leaf changes"),
			InstanceRecord->HasField(TEXT("changes")));
	}

	return true;
}

// ============================================================================
// Task 14 review fix F2 — inside a precision-sensitive root, an ordinary nested struct must still be
// diffed by relative leaf path; an unrelated int64 sibling may not change the diff granularity.
// ============================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexBPClassDefaultsEchoNestedStructRelativeLeavesTest,
	"Cortex.Blueprint.ClassDefaults.Set.EchoNestedStructRelativeLeaves",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexBPClassDefaultsEchoNestedStructRelativeLeavesTest::RunTest(const FString&)
{
	FEchoScopedFixture Fixture;
	FCortexBPCommandHandler Handler;
	UObject* CDO = Fixture.Create(
		ACortexBPEchoContractActor::StaticClass(), EchoFixtureUniquePath(TEXT("NestedLeaves")));
	if (!TestNotNull(TEXT("nested-leaf fixture CDO should exist"), CDO))
	{
		return true;
	}

	FStructProperty* MixedProperty = FindFProperty<FStructProperty>(CDO->GetClass(), TEXT("MixedPayload"));
	if (!TestNotNull(TEXT("fixture exposes the mixed payload root"), MixedProperty))
	{
		return true;
	}

	// Only the nested Vector.X changes; the unrelated wide int64 sibling must not collapse the diff.
	TSharedPtr<FJsonObject> VectorWrite = MakeShared<FJsonObject>();
	VectorWrite->SetNumberField(TEXT("X"), 4);
	TSharedPtr<FJsonObject> MixedWrite = MakeShared<FJsonObject>();
	MixedWrite->SetObjectField(TEXT("Vector"), VectorWrite);
	TSharedPtr<FJsonObject> WriteValues = MakeShared<FJsonObject>();
	WriteValues->SetObjectField(TEXT("MixedPayload"), MixedWrite);

	const FCortexCommandResult Set = Handler.Execute(
		TEXT("set_class_defaults"), EchoSetParams(Fixture.AssetPath, WriteValues, nullptr));
	if (!TestTrue(*FString::Printf(TEXT("nested-leaf set should succeed (error %s: %s)"),
			*Set.ErrorCode, *Set.ErrorMessage), Set.bSuccess)
		|| !Set.Data.IsValid())
	{
		return true;
	}

	TSharedPtr<FJsonObject> Record = EchoResultRecord(Set, TEXT("MixedPayload"));
	if (!TestNotNull(TEXT("nested-leaf record should exist"), Record.Get()))
	{
		return true;
	}

	bool bChanged = false;
	TestTrue(TEXT("nested-leaf record declares changed"), Record->TryGetBoolField(TEXT("changed"), bChanged));
	TestTrue(TEXT("a nested leaf change is a real change"), bChanged);
	TestFalse(TEXT("nested-leaf diff drops the complete previous value"), Record->HasField(TEXT("previous_value")));
	TestFalse(TEXT("nested-leaf diff drops the complete new value"), Record->HasField(TEXT("new_value")));

	const TSharedPtr<FJsonObject>* Changes = nullptr;
	if (TestTrue(TEXT("nested-leaf diff exposes changed leaf paths"),
		Record->TryGetObjectField(TEXT("changes"), Changes) && Changes != nullptr))
	{
		TestEqual(TEXT("only the nested leaf is reported"), (*Changes)->Values.Num(), 1);
		const TSharedPtr<FJsonObject>* VectorXChange = nullptr;
		if (TestTrue(TEXT("the nested struct leaf uses its relative path"),
			(*Changes)->TryGetObjectField(TEXT("Vector.X"), VectorXChange) && VectorXChange != nullptr))
		{
			double Before = -1.0;
			double After = -1.0;
			TestTrue(TEXT("the nested leaf reports its previous value"),
				(*VectorXChange)->TryGetNumberField(TEXT("previous_value"), Before));
			TestEqual(TEXT("the nested leaf previous value is 1"), static_cast<int32>(Before), 1);
			TestTrue(TEXT("the nested leaf reports its new value"),
				(*VectorXChange)->TryGetNumberField(TEXT("new_value"), After));
			TestEqual(TEXT("the nested leaf new value is 4"), static_cast<int32>(After), 4);
		}
		TestFalse(TEXT("the whole nested struct is not echoed"),
			(*Changes)->HasField(TEXT("Vector")));
		TestFalse(TEXT("the unchanged wide sibling stays out"),
			(*Changes)->HasField(TEXT("Big")));
	}

	// The applied state really landed while the wide sibling kept its baseline.
	const FCortexBPEchoMixedPayload* Mixed =
		static_cast<const FCortexBPEchoMixedPayload*>(MixedProperty->ContainerPtrToValuePtr<void>(CDO));
	if (TestNotNull(TEXT("mixed payload should be readable"), Mixed))
	{
		TestEqual(TEXT("the nested X applied"), static_cast<int32>(Mixed->Vector.X), 4);
		TestEqual(TEXT("the nested Y kept its baseline"), static_cast<int32>(Mixed->Vector.Y), 2);
		TestEqual(TEXT("the nested Z kept its baseline"), static_cast<int32>(Mixed->Vector.Z), 3);
		TestTrue(TEXT("the wide sibling kept its baseline"), Mixed->Big == 0);
	}

	return true;
}

// ============================================================================
// Task 14 review fix F3 — an FInstancedStruct root carrying a runtime int64 payload must take the exact
// snapshot path in every mode: an exactly representable neighbour write reports changed with exact
// decimal strings, and a later property failure restores the exact native payload.
// ============================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexBPClassDefaultsEchoInstancedStructPrecisionTest,
	"Cortex.Blueprint.ClassDefaults.Set.EchoInstancedStructPrecisionAndRollback",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexBPClassDefaultsEchoInstancedStructPrecisionTest::RunTest(const FString&)
{
	FEchoScopedFixture Fixture;
	FCortexBPCommandHandler Handler;
	UObject* CDO = Fixture.Create(
		ACortexBPEchoContractActor::StaticClass(), EchoFixtureUniquePath(TEXT("InstancePrecision")));
	if (!TestNotNull(TEXT("instanced precision fixture CDO should exist"), CDO))
	{
		return true;
	}

	FStructProperty* InstanceProperty = FindFProperty<FStructProperty>(CDO->GetClass(), TEXT("EchoInstance"));
	if (!TestNotNull(TEXT("fixture exposes the instanced struct root"), InstanceProperty))
	{
		return true;
	}

	// 9007199254740993 is NOT exactly representable as a double; 9007199254740992 is.
	constexpr int64 SeedValue = 9007199254740993LL;
	constexpr int64 RequestedValue = 9007199254740992LL;

	// UHT strips the F prefix from the reflected name; use the runtime reflected name exactly like the
	// serializer's `_struct_type`.
	const FString InstanceStructName = FCortexBPEchoInstancePayload::StaticStruct()->GetName();

	const auto MakeInstanceWrite = [&InstanceStructName](int64 Big)
	{
		TSharedPtr<FJsonObject> InstanceWrite = MakeShared<FJsonObject>();
		InstanceWrite->SetStringField(TEXT("_struct_type"), InstanceStructName);
		InstanceWrite->SetNumberField(TEXT("Big"), static_cast<double>(Big));
		TSharedPtr<FJsonObject> Values = MakeShared<FJsonObject>();
		Values->SetObjectField(TEXT("EchoInstance"), InstanceWrite);
		return Values;
	};

	// diff consolidates the runtime payload with exact decimal strings.
	EchoSeedInstancePayload(CDO, InstanceProperty, SeedValue);
	const FCortexCommandResult Set = Handler.Execute(
		TEXT("set_class_defaults"),
		EchoSetParams(Fixture.AssetPath, MakeInstanceWrite(RequestedValue), TEXT("diff")));
	if (!TestTrue(*FString::Printf(TEXT("instanced precision set should succeed (error %s: %s)"),
			*Set.ErrorCode, *Set.ErrorMessage), Set.bSuccess)
		|| !Set.Data.IsValid())
	{
		return true;
	}

	TSharedPtr<FJsonObject> Record = EchoResultRecord(Set, TEXT("EchoInstance"));
	if (!TestNotNull(TEXT("instanced precision record should exist"), Record.Get()))
	{
		return true;
	}

	bool bChanged = false;
	TestTrue(TEXT("instanced precision record declares changed"),
		Record->TryGetBoolField(TEXT("changed"), bChanged));
	TestTrue(TEXT("the exact typed comparison reports a real change"), bChanged);
	TestFalse(TEXT("instanced precision diff drops the leaf changes"), Record->HasField(TEXT("changes")));

	const TSharedPtr<FJsonObject>* PreviousObject = nullptr;
	const TSharedPtr<FJsonObject>* NewObject = nullptr;
	if (TestTrue(TEXT("instanced precision diff reports the previous payload"),
			Record->TryGetObjectField(TEXT("previous_value"), PreviousObject) && PreviousObject != nullptr)
		&& TestTrue(TEXT("instanced precision diff reports the new payload"),
			Record->TryGetObjectField(TEXT("new_value"), NewObject) && NewObject != nullptr))
	{
		FString PreviousBig;
		FString NewBig;
		TestTrue(TEXT("the previous payload reports an exact decimal string"),
			(*PreviousObject)->TryGetStringField(TEXT("Big"), PreviousBig));
		TestEqual(TEXT("the previous payload value is the exact seeded integer"),
			PreviousBig, FString(TEXT("9007199254740993")));
		TestTrue(TEXT("the new payload reports an exact decimal string"),
			(*NewObject)->TryGetStringField(TEXT("Big"), NewBig));
		TestEqual(TEXT("the new payload value is the exact requested integer"),
			NewBig, FString(TEXT("9007199254740992")));
	}

	const TSharedPtr<FJsonObject>* Encodings = nullptr;
	if (TestTrue(TEXT("instanced precision diff declares per-path encodings"),
		Record->TryGetObjectField(TEXT("value_encodings"), Encodings) && Encodings != nullptr))
	{
		FString BigEncoding;
		TestTrue(TEXT("the encoding names the payload int64 path"),
			(*Encodings)->TryGetStringField(TEXT("Big"), BigEncoding));
		TestEqual(TEXT("the payload int64 leaf uses decimal strings"),
			BigEncoding, FString(TEXT("decimal_string")));
	}

	TestTrue(TEXT("the payload post-state is the requested exactly representable value"),
		EchoReadInstanceBig(CDO, InstanceProperty) == RequestedValue);

	// Every mode keeps an authoritative changed outcome; diff/full carry exact decimal strings and none
	// omits the values.
	for (const TCHAR* Mode : { TEXT("diff"), TEXT("full"), TEXT("none") })
	{
		EchoSeedInstancePayload(CDO, InstanceProperty, SeedValue);
		const FCortexCommandResult ModeSet = Handler.Execute(
			TEXT("set_class_defaults"), EchoSetParams(Fixture.AssetPath, MakeInstanceWrite(RequestedValue), Mode));
		TestTrue(*FString::Printf(TEXT("%s mode applies the payload (error %s: %s)"),
			Mode, *ModeSet.ErrorCode, *ModeSet.ErrorMessage), ModeSet.bSuccess);

		TSharedPtr<FJsonObject> ModeRecord = EchoResultRecord(ModeSet, TEXT("EchoInstance"));
		if (TestNotNull(*FString::Printf(TEXT("%s mode exposes the payload record"), Mode), ModeRecord.Get()))
		{
			bool bModeChanged = false;
			TestTrue(*FString::Printf(TEXT("%s mode declares changed"), Mode),
				ModeRecord->TryGetBoolField(TEXT("changed"), bModeChanged));
			TestTrue(*FString::Printf(TEXT("%s mode reports a real change"), Mode), bModeChanged);

			if (FCString::Strcmp(Mode, TEXT("none")) == 0)
			{
				TestFalse(*FString::Printf(TEXT("%s mode omits the previous value"), Mode),
					ModeRecord->HasField(TEXT("previous_value")));
				TestFalse(*FString::Printf(TEXT("%s mode omits the new value"), Mode),
					ModeRecord->HasField(TEXT("new_value")));
				TestFalse(*FString::Printf(TEXT("%s mode omits the changed leaves"), Mode),
					ModeRecord->HasField(TEXT("changes")));
			}
			else
			{
				const TSharedPtr<FJsonObject>* ModePrevious = nullptr;
				const TSharedPtr<FJsonObject>* ModeNew = nullptr;
				if (TestTrue(*FString::Printf(TEXT("%s mode reports the previous payload"), Mode),
						ModeRecord->TryGetObjectField(TEXT("previous_value"), ModePrevious) && ModePrevious != nullptr)
					&& TestTrue(*FString::Printf(TEXT("%s mode reports the new payload"), Mode),
						ModeRecord->TryGetObjectField(TEXT("new_value"), ModeNew) && ModeNew != nullptr))
				{
					FString ModePreviousBig;
					FString ModeNewBig;
					TestTrue(*FString::Printf(TEXT("%s mode reports the exact previous integer"), Mode),
						(*ModePrevious)->TryGetStringField(TEXT("Big"), ModePreviousBig));
					TestEqual(*FString::Printf(TEXT("%s mode previous value is the exact seed"), Mode),
						ModePreviousBig, FString(TEXT("9007199254740993")));
					TestTrue(*FString::Printf(TEXT("%s mode reports the exact new integer"), Mode),
						(*ModeNew)->TryGetStringField(TEXT("Big"), ModeNewBig));
					TestEqual(*FString::Printf(TEXT("%s mode new value is the exact request"), Mode),
						ModeNewBig, FString(TEXT("9007199254740992")));
				}
			}
		}

		TestTrue(*FString::Printf(TEXT("%s mode applied the requested payload"), Mode),
			EchoReadInstanceBig(CDO, InstanceProperty) == RequestedValue);
	}

	// A later property failure restores the exact native payload in every mode.
	for (const TCHAR* Mode : { TEXT("diff"), TEXT("full"), TEXT("none") })
	{
		EchoSeedInstancePayload(CDO, InstanceProperty, SeedValue);

		TSharedPtr<FJsonObject> FailedValues = MakeInstanceWrite(RequestedValue);
		FailedValues->SetBoolField(TEXT("CortexEchoAbsentProperty"), true);
		const FCortexCommandResult Failed = Handler.Execute(
			TEXT("set_class_defaults"), EchoSetParams(Fixture.AssetPath, FailedValues, Mode));

		TestFalse(*FString::Printf(TEXT("%s mode fails on the later property"), Mode), Failed.bSuccess);
		TestEqual(*FString::Printf(TEXT("%s mode reports PROPERTY_NOT_FOUND"), Mode),
			Failed.ErrorCode, CortexErrorCodes::PropertyNotFound);
		TestTrue(*FString::Printf(TEXT("%s mode restores the payload type"), Mode),
			EchoReadInstancePayload(CDO, InstanceProperty) != nullptr);
		TestTrue(*FString::Printf(TEXT("%s mode restores the exact payload integer"), Mode),
			EchoReadInstanceBig(CDO, InstanceProperty) == SeedValue);
	}

	return true;
}
