#include "Misc/AutomationTest.h"
#include "CortexEngineCompat.h"

#include "CortexBPTestLiftActor.h"
#include "Operations/CortexBPClassDefaultsOps.h"
#include "Operations/CortexBPComponentOps.h"
#include "Containers/Ticker.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Engine/Blueprint.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "Engine/SCS_Node.h"
#include "Engine/SimpleConstructionScript.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Misc/Guid.h"
#include "Misc/PackageName.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Misc/Paths.h"
#include "UObject/GarbageCollection.h"
#include "UObject/Package.h"
#include "UObject/UObjectIterator.h"

namespace
{
	bool IsDiscoveryPackageUnderRoot(const FString& PackageName, const FString& Root)
	{
		return PackageName == Root || PackageName.StartsWith(Root + TEXT("/"));
	}

	struct FScopedDiscoveryReadOnlyMountedRoot
	{
		FString Root;
		FString PhysicalDir;

		FScopedDiscoveryReadOnlyMountedRoot()
		{
			Root = FString::Printf(
				TEXT("/CortexReadOnlyDiscovery%s"),
				*FGuid::NewGuid().ToString(EGuidFormats::Digits).Right(8));
			PhysicalDir = FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir() / TEXT("CortexReadOnlyBlueprintTests") / Root.RightChop(1));
			IFileManager::Get().MakeDirectory(*PhysicalDir, true);
			FPackageName::RegisterMountPoint(Root + TEXT("/"), PhysicalDir / TEXT(""));
		}

		~FScopedDiscoveryReadOnlyMountedRoot()
		{
			for (TObjectIterator<UObject> It; It; ++It)
			{
				UObject* Asset = *It;
				if (!Asset || !Asset->IsAsset())
				{
					continue;
				}

				UPackage* Package = Asset->GetOutermost();
				if (Package && IsDiscoveryPackageUnderRoot(Package->GetName(), Root))
				{
					FAssetRegistryModule::AssetDeleted(Asset);
					Asset->MarkAsGarbage();
				}
			}

			for (TObjectIterator<UPackage> It; It; ++It)
			{
				UPackage* Package = *It;
				if (Package && IsDiscoveryPackageUnderRoot(Package->GetName(), Root))
				{
					Package->SetDirtyFlag(false);
					FAssetRegistryModule::PackageDeleted(Package);
					Package->MarkAsGarbage();
				}
			}
			CollectGarbage(RF_NoFlags);

			IAssetRegistry& AssetRegistry =
				FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();
			AssetRegistry.WaitForCompletion();
			FlushAsyncLoading();
			FTSTicker::GetCoreTicker().Tick(0.0f);

			FPackageName::UnRegisterMountPoint(Root + TEXT("/"), PhysicalDir / TEXT(""));
			IFileManager::Get().DeleteDirectory(*PhysicalDir, false, true);
		}
	};

	UBlueprint* CreateDiscoveryBlueprint(const TCHAR* Name)
	{
		return FKismetEditorUtilities::CreateBlueprint(
			ACortexBPTestLiftActor::StaticClass(),
			GetTransientPackage(),
			FName(Name),
			BPTYPE_Normal,
			UBlueprint::StaticClass(),
			UBlueprintGeneratedClass::StaticClass());
	}

	USCS_Node* AddDiscoveryComponent(UBlueprint* BP, UClass* ComponentClass, const TCHAR* VariableName, bool bCompile = true)
	{
		if (!BP || !BP->SimpleConstructionScript)
		{
			return nullptr;
		}

		USCS_Node* Node = BP->SimpleConstructionScript->CreateNode(ComponentClass, FName(VariableName));
		if (!Node)
		{
			return nullptr;
		}

		BP->SimpleConstructionScript->AddNode(Node);
		Node->SetVariableName(FName(VariableName), false);
		if (bCompile)
		{
			FKismetEditorUtilities::CompileBlueprint(BP);
		}
		return Node;
	}

	const TSharedPtr<FJsonObject>* FindObjectInArrayByStringField(
		const TArray<TSharedPtr<FJsonValue>>* Array,
		const TCHAR* FieldName,
		const TCHAR* ExpectedValue)
	{
		if (!Array)
		{
			return nullptr;
		}

		for (const TSharedPtr<FJsonValue>& Item : *Array)
		{
			if (Item.IsValid() && Item->Type == EJson::Object)
			{
				const TSharedPtr<FJsonObject> Obj = Item->AsObject();
				FString Val;
				if (Obj.IsValid() && Obj->TryGetStringField(FieldName, Val) && Val == ExpectedValue)
				{
					return &Item->AsObject();
				}
			}
		}
		return nullptr;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexBPListSCSComponentsTest,
	"Cortex.Blueprint.Discovery.ListSCSComponents.Basic",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexBPListSCSComponentsTest::RunTest(const FString& Parameters)
{
	UBlueprint* BP = CreateDiscoveryBlueprint(TEXT("BP_DiscoveryListSCS"));
	TestNotNull(TEXT("Discovery Blueprint created"), BP);
	if (!BP)
	{
		return false;
	}

	TestNotNull(
		TEXT("Discovery component added"),
		AddDiscoveryComponent(BP, UCortexBPTestSubobjComponent::StaticClass(), TEXT("LiftComp")));

	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("asset_path"), BP->GetPathName());
	const FCortexCommandResult Result = FCortexBPComponentOps::ListSCSComponents(Params);

	TestTrue(TEXT("list_scs_components succeeds"), Result.bSuccess);
	if (Result.bSuccess && Result.Data.IsValid())
	{
		const TArray<TSharedPtr<FJsonValue>>* Components = nullptr;
		TestTrue(TEXT("components array exists"), Result.Data->TryGetArrayField(TEXT("components"), Components));
		TestNotNull(
			TEXT("LiftComp is returned"),
			FindObjectInArrayByStringField(Components, TEXT("name"), TEXT("LiftComp")));
	}

	BP->MarkAsGarbage();
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexBPListSCSComponentsReadOnlyNoCompileTest,
	"Cortex.Blueprint.Discovery.ListSCSComponents.ReadOnlyNoCompile",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexBPListSCSComponentsReadOnlyNoCompileTest::RunTest(const FString& Parameters)
{
	FScopedDiscoveryReadOnlyMountedRoot ReadOnlyRoot;

	UBlueprint* BP = FKismetEditorUtilities::CreateBlueprint(
		ACortexBPTestLiftActor::StaticClass(),
		CreatePackage(*(ReadOnlyRoot.Root / TEXT("BP_DiscoveryReadOnlyListSCS"))),
		FName(TEXT("BP_DiscoveryReadOnlyListSCS")),
		BPTYPE_Normal,
		UBlueprint::StaticClass(),
		UBlueprintGeneratedClass::StaticClass());
	TestNotNull(TEXT("Read-only mounted Blueprint created"), BP);
	if (!BP)
	{
		return false;
	}

	TestNotNull(
		TEXT("Discovery component added"),
		AddDiscoveryComponent(BP, UCortexBPTestSubobjComponent::StaticClass(), TEXT("ReadOnlyComp"), false));
	BP->Status = BS_Dirty;

	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("asset_path"), BP->GetPathName());
	const FCortexCommandResult Result = FCortexBPComponentOps::ListSCSComponents(Params);

	TestTrue(TEXT("list_scs_components reads non-writable mounted Blueprint"), Result.bSuccess);
	TestEqual(TEXT("Read-only Blueprint was not compiled"), BP->Status, EBlueprintStatus::BS_Dirty);
	if (Result.bSuccess && Result.Data.IsValid())
	{
		const TArray<TSharedPtr<FJsonValue>>* Components = nullptr;
		TestTrue(TEXT("components array exists"), Result.Data->TryGetArrayField(TEXT("components"), Components));
		TestNotNull(
			TEXT("ReadOnlyComp is returned"),
			FindObjectInArrayByStringField(Components, TEXT("name"), TEXT("ReadOnlyComp")));
	}

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexBPListInheritedPropertiesTest,
	"Cortex.Blueprint.Discovery.ListInheritedProperties",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexBPListInheritedPropertiesTest::RunTest(const FString& Parameters)
{
	UBlueprint* BP = CreateDiscoveryBlueprint(TEXT("BP_DiscoveryInheritedProps"));
	TestNotNull(TEXT("Blueprint created"), BP);
	if (!BP)
	{
		return false;
	}

	FKismetEditorUtilities::CompileBlueprint(BP);

	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("asset_path"), BP->GetPathName());
	Params->SetBoolField(TEXT("include_values"), true);
	const FCortexCommandResult Result = FCortexBPClassDefaultsOps::ListInheritedProperties(Params);

	TestTrue(TEXT("list_inherited_properties succeeds"), Result.bSuccess);
	if (Result.bSuccess && Result.Data.IsValid())
	{
		bool bValuesIncluded = false;
		TestTrue(TEXT("explicit value read declares value inclusion"),
			Result.Data->TryGetBoolField(TEXT("values_included"), bValuesIncluded));
		TestTrue(TEXT("explicit value read includes values"), bValuesIncluded);

		const TSharedPtr<FJsonObject>* Properties = nullptr;
		TestTrue(TEXT("properties object exists"), Result.Data->TryGetObjectField(TEXT("properties"), Properties));
		if (Properties && *Properties)
		{
			const TSharedPtr<FJsonObject>* MeshObj = nullptr;
			TestTrue(TEXT("Mesh property returned"), (*Properties)->TryGetObjectField(TEXT("Mesh"), MeshObj));
			if (MeshObj && *MeshObj)
			{
				TestTrue(TEXT("accepted_formats exists"), (*MeshObj)->HasField(TEXT("accepted_formats")));
				TestTrue(TEXT("requested value exists"), (*MeshObj)->HasField(TEXT("value")));
			}
		}
	}

	BP->MarkAsGarbage();
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexBPListSettableDefaultsTest,
	"Cortex.Blueprint.Discovery.ListSettableDefaults",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexBPListSettableDefaultsTest::RunTest(const FString& Parameters)
{
	UBlueprint* BP = CreateDiscoveryBlueprint(TEXT("BP_DiscoverySettableDefaults"));
	TestNotNull(TEXT("Blueprint created"), BP);
	if (!BP)
	{
		return false;
	}

	FKismetEditorUtilities::CompileBlueprint(BP);

	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("asset_path"), BP->GetPathName());
	Params->SetBoolField(TEXT("include_values"), true);
	const FCortexCommandResult Result = FCortexBPClassDefaultsOps::ListSettableDefaults(Params);

	TestTrue(TEXT("list_settable_defaults succeeds"), Result.bSuccess);
	if (Result.bSuccess && Result.Data.IsValid())
	{
		bool bValuesIncluded = false;
		TestTrue(TEXT("explicit value read declares value inclusion"),
			Result.Data->TryGetBoolField(TEXT("values_included"), bValuesIncluded));
		TestTrue(TEXT("explicit value read includes values"), bValuesIncluded);

		const TSharedPtr<FJsonObject>* Properties = nullptr;
		TestTrue(TEXT("properties object exists"), Result.Data->TryGetObjectField(TEXT("properties"), Properties));
		if (Properties && *Properties)
		{
			const TSharedPtr<FJsonObject>* OpenSeqObj = nullptr;
			TestTrue(TEXT("OpenSeq property returned"), (*Properties)->TryGetObjectField(TEXT("OpenSeq"), OpenSeqObj));
			if (OpenSeqObj && *OpenSeqObj)
			{
				TestTrue(TEXT("value exists"), (*OpenSeqObj)->HasField(TEXT("value")));
				TestTrue(TEXT("accepted_formats exists"), (*OpenSeqObj)->HasField(TEXT("accepted_formats")));
			}
		}
	}

	BP->MarkAsGarbage();
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexBPDiscoveryMetadataOnlyTest,
	"Cortex.Blueprint.Discovery.Defaults.MetadataOnlyWithoutValues",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexBPDiscoveryMetadataOnlyTest::RunTest(const FString& Parameters)
{
	UBlueprint* BP = CreateDiscoveryBlueprint(TEXT("BP_DiscoveryMetadataOnly"));
	TestNotNull(TEXT("Blueprint created"), BP);
	if (!BP)
	{
		return false;
	}

	FKismetEditorUtilities::CompileBlueprint(BP);

	const TCHAR* const Commands[] = { TEXT("list_settable_defaults"), TEXT("list_inherited_properties") };
	for (const TCHAR* Command : Commands)
	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("asset_path"), BP->GetPathName());

		const bool bSettableDefaults = FCString::Stricmp(Command, TEXT("list_settable_defaults")) == 0;
		const FCortexCommandResult Result = bSettableDefaults
			? FCortexBPClassDefaultsOps::ListSettableDefaults(Params)
			: FCortexBPClassDefaultsOps::ListInheritedProperties(Params);

		TestTrue(*FString::Printf(TEXT("%s succeeds"), Command), Result.bSuccess);
		if (!Result.bSuccess || !Result.Data.IsValid())
		{
			continue;
		}

		bool bValuesIncluded = true;
		TestTrue(*FString::Printf(TEXT("%s declares value inclusion"), Command),
			Result.Data->TryGetBoolField(TEXT("values_included"), bValuesIncluded));
		TestFalse(*FString::Printf(TEXT("%s metadata does not claim values"), Command), bValuesIncluded);

		const TSharedPtr<FJsonObject>* Properties = nullptr;
		if (!TestTrue(*FString::Printf(TEXT("%s exposes properties"), Command),
			Result.Data->TryGetObjectField(TEXT("properties"), Properties) && Properties != nullptr))
		{
			continue;
		}

		int32 ValueFields = 0;
		int32 MemberFields = 0;
		int32 MissingFormats = 0;
		for (const auto& Pair : (*Properties)->Values)
		{
			const TSharedPtr<FJsonObject> Entry = Pair.Value.IsValid() ? Pair.Value->AsObject() : nullptr;
			if (!Entry.IsValid())
			{
				continue;
			}
			if (Entry->HasField(TEXT("value")))
			{
				++ValueFields;
			}
			if (Entry->HasField(TEXT("members")))
			{
				++MemberFields;
			}
			if (!Entry->HasField(TEXT("accepted_formats")))
			{
				++MissingFormats;
			}
		}
		TestEqual(*FString::Printf(TEXT("%s must not serialize property values"), Command), ValueFields, 0);
		TestEqual(*FString::Printf(TEXT("%s must not expand struct members"), Command), MemberFields, 0);
		TestEqual(*FString::Printf(TEXT("%s must keep accepted formats in metadata"), Command), MissingFormats, 0);

		double TotalProperties = 0.0;
		TestTrue(*FString::Printf(TEXT("%s declares total_properties"), Command),
			Result.Data->TryGetNumberField(TEXT("total_properties"), TotalProperties));
		TestTrue(*FString::Printf(TEXT("%s total_properties covers the returned metadata"), Command),
			TotalProperties > 0.0 && TotalProperties >= (*Properties)->Values.Num());

		const TSharedPtr<FJsonObject>* Pagination = nullptr;
		if (TestTrue(*FString::Printf(TEXT("%s declares discovery_pagination"), Command),
			Result.Data->TryGetObjectField(TEXT("discovery_pagination"), Pagination) && Pagination != nullptr))
		{
			double Offset = -1.0;
			double Limit = -1.0;
			double Returned = -1.0;
			bool bComplete = false;
			TestTrue(*FString::Printf(TEXT("%s pagination declares its offset"), Command),
				(*Pagination)->TryGetNumberField(TEXT("offset"), Offset));
			TestEqual(*FString::Printf(TEXT("%s pagination offset defaults to 0"), Command), Offset, 0.0);
			TestTrue(*FString::Printf(TEXT("%s pagination declares its limit"), Command),
				(*Pagination)->TryGetNumberField(TEXT("limit"), Limit));
			TestEqual(*FString::Printf(TEXT("%s pagination limit defaults to 100"), Command),
				static_cast<int32>(Limit), 100);
			TestTrue(*FString::Printf(TEXT("%s pagination declares its returned count"), Command),
				(*Pagination)->TryGetNumberField(TEXT("returned"), Returned));
			TestEqual(*FString::Printf(TEXT("%s pagination returned count matches the returned metadata"), Command),
				static_cast<int32>(Returned), (*Properties)->Values.Num());
			TestTrue(*FString::Printf(TEXT("%s pagination declares completeness"), Command),
				(*Pagination)->TryGetBoolField(TEXT("is_complete"), bComplete));
			if (static_cast<int32>(TotalProperties) <= 100)
			{
				TestTrue(*FString::Printf(TEXT("%s population inside the default page is complete"), Command), bComplete);
			}
		}
	}

	BP->MarkAsGarbage();
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexBPDiscoveryInvalidReadControlsTest,
	"Cortex.Blueprint.Discovery.Defaults.InvalidReadControlsRefuse",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexBPDiscoveryInvalidReadControlsTest::RunTest(const FString& Parameters)
{
	UBlueprint* BP = CreateDiscoveryBlueprint(TEXT("BP_DiscoveryInvalidReadControls"));
	TestNotNull(TEXT("Blueprint created"), BP);
	if (!BP)
	{
		return false;
	}

	FKismetEditorUtilities::CompileBlueprint(BP);

	// A selector that the read does not admit must refuse instead of silently listing everything.
	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("asset_path"), BP->GetPathName());
		TArray<TSharedPtr<FJsonValue>> Selector;
		Selector.Add(MakeShared<FJsonValueString>(TEXT("NotAnAdmittedDefault")));
		Params->SetArrayField(TEXT("properties"), Selector);
		const FCortexCommandResult Result = FCortexBPClassDefaultsOps::ListSettableDefaults(Params);
		TestFalse(TEXT("list_settable_defaults refuses an unadmitted selected property"), Result.bSuccess);
		TestEqual(TEXT("unadmitted selected property should refuse with PROPERTY_NOT_FOUND"),
			Result.ErrorCode, CortexErrorCodes::PropertyNotFound);
	}

	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("asset_path"), BP->GetPathName());
		Params->SetStringField(TEXT("include_values"), TEXT("yes"));
		const FCortexCommandResult Result = FCortexBPClassDefaultsOps::ListSettableDefaults(Params);
		TestFalse(TEXT("list_settable_defaults refuses a non-boolean include_values"), Result.bSuccess);
		TestEqual(TEXT("non-boolean include_values should refuse with INVALID_FIELD"),
			Result.ErrorCode, CortexErrorCodes::InvalidField);
	}

	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("asset_path"), BP->GetPathName());
		Params->SetNumberField(TEXT("include_members"), 2);
		const FCortexCommandResult Result = FCortexBPClassDefaultsOps::ListSettableDefaults(Params);
		TestFalse(TEXT("list_settable_defaults refuses a numeric include_members"), Result.bSuccess);
		TestEqual(TEXT("numeric include_members should refuse with INVALID_FIELD"),
			Result.ErrorCode, CortexErrorCodes::InvalidField);
	}

	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("asset_path"), BP->GetPathName());
		Params->SetNumberField(TEXT("discovery_limit"), 201);
		const FCortexCommandResult Result = FCortexBPClassDefaultsOps::ListSettableDefaults(Params);
		TestFalse(TEXT("list_settable_defaults refuses an oversized discovery_limit"), Result.bSuccess);
		TestEqual(TEXT("oversized discovery_limit should refuse with INVALID_FIELD"),
			Result.ErrorCode, CortexErrorCodes::InvalidField);
	}

	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("asset_path"), BP->GetPathName());
		Params->SetNumberField(TEXT("discovery_offset"), -1);
		const FCortexCommandResult Result = FCortexBPClassDefaultsOps::ListInheritedProperties(Params);
		TestFalse(TEXT("list_inherited_properties refuses a negative discovery_offset"), Result.bSuccess);
		TestEqual(TEXT("negative discovery_offset should refuse with INVALID_FIELD"),
			Result.ErrorCode, CortexErrorCodes::InvalidField);
	}

	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("asset_path"), BP->GetPathName());
		Params->SetStringField(TEXT("include_members"), TEXT("true"));
		const FCortexCommandResult Result = FCortexBPClassDefaultsOps::ListInheritedProperties(Params);
		TestFalse(TEXT("list_inherited_properties refuses a non-boolean include_members"), Result.bSuccess);
		TestEqual(TEXT("non-boolean include_members should refuse with INVALID_FIELD"),
			Result.ErrorCode, CortexErrorCodes::InvalidField);
	}

	// A present non-array selector must refuse instead of degrading to an unfiltered read.
	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("asset_path"), BP->GetPathName());
		Params->SetStringField(TEXT("properties"), TEXT("Mesh"));
		const FCortexCommandResult Result = FCortexBPClassDefaultsOps::ListSettableDefaults(Params);
		TestFalse(TEXT("list_settable_defaults refuses a string property selector"), Result.bSuccess);
		TestEqual(TEXT("string property selector should refuse with INVALID_FIELD"),
			Result.ErrorCode, CortexErrorCodes::InvalidField);
	}

	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("asset_path"), BP->GetPathName());
		TSharedPtr<FJsonObject> Selector = MakeShared<FJsonObject>();
		Selector->SetBoolField(TEXT("Mesh"), true);
		Params->SetObjectField(TEXT("properties"), Selector);
		Params->SetBoolField(TEXT("include_values"), true);
		const FCortexCommandResult Result = FCortexBPClassDefaultsOps::ListSettableDefaults(Params);
		TestFalse(TEXT("list_settable_defaults refuses an object selector with include_values"), Result.bSuccess);
		TestEqual(TEXT("object property selector should refuse with INVALID_FIELD"),
			Result.ErrorCode, CortexErrorCodes::InvalidField);
	}

	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("asset_path"), BP->GetPathName());
		Params->SetField(TEXT("properties"), MakeShared<FJsonValueNull>());
		const FCortexCommandResult Result = FCortexBPClassDefaultsOps::ListInheritedProperties(Params);
		TestFalse(TEXT("list_inherited_properties refuses a null property selector"), Result.bSuccess);
		TestEqual(TEXT("null property selector should refuse with INVALID_FIELD"),
			Result.ErrorCode, CortexErrorCodes::InvalidField);
	}

	// Controls are validated before any asset lookup: a resolvable path is never resolved or
	// compiled, and a nonexistent path never reports a lookup failure.
	{
		const FString RawPath = FString::Printf(TEXT("/Game/Temp/CortexBPDiscoveryNoClass_%s"),
			*FGuid::NewGuid().ToString(EGuidFormats::Digits).Right(8));
		UPackage* RawPackage = CreatePackage(*RawPath);
		UBlueprint* RawBlueprint = RawPackage
			? NewObject<UBlueprint>(RawPackage, FName(*FPackageName::GetShortName(RawPath)),
				RF_Public | RF_Standalone | RF_Transactional | RF_LoadCompleted)
			: nullptr;
		if (!TestNotNull(TEXT("compile-less discovery Blueprint fixture should be created"), RawBlueprint))
		{
			if (RawPackage)
			{
				RawPackage->MarkAsGarbage();
			}
		}
		else
		{
			TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
			Params->SetStringField(TEXT("asset_path"), RawBlueprint->GetPathName());
			Params->SetStringField(TEXT("include_values"), TEXT("yes"));
			const FCortexCommandResult Result = FCortexBPClassDefaultsOps::ListSettableDefaults(Params);
			TestFalse(TEXT("malformed control should refuse a Blueprint without a generated class"),
				Result.bSuccess);
			TestEqual(TEXT("malformed list control should refuse with INVALID_FIELD"),
				Result.ErrorCode, CortexErrorCodes::InvalidField);
			TestTrue(TEXT("malformed list control must not create a generated class"),
				RawBlueprint->GeneratedClass.Get() == nullptr);

			RawBlueprint->MarkAsGarbage();
			RawPackage->MarkAsGarbage();
		}
	}

	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("asset_path"), FString::Printf(TEXT("/Game/Temp/CortexBPDiscoveryMissing_%s"),
			*FGuid::NewGuid().ToString(EGuidFormats::Digits).Right(8)));
		Params->SetNumberField(TEXT("discovery_limit"), 0);
		const FCortexCommandResult Result = FCortexBPClassDefaultsOps::ListInheritedProperties(Params);
		TestFalse(TEXT("malformed list control should refuse a nonexistent path"), Result.bSuccess);
		TestEqual(TEXT("malformed list control should refuse before lookup with INVALID_FIELD"),
			Result.ErrorCode, CortexErrorCodes::InvalidField);
	}

	BP->MarkAsGarbage();
	return true;
}
