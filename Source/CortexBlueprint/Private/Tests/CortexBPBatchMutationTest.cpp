#include "Misc/AutomationTest.h"

#include "Operations/CortexBPClassDefaultsOps.h"
#include "Operations/CortexBPComponentOps.h"
#include "CortexAssetMutationGuard.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Engine/Blueprint.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "GameFramework/Actor.h"
#include "Misc/Guid.h"

namespace
{
UBlueprint* CreateBatchMutationBlueprint(const TCHAR* Name)
{
	UBlueprint* Blueprint = FKismetEditorUtilities::CreateBlueprint(
		AActor::StaticClass(),
		CreatePackage(*FString::Printf(
			TEXT("/Game/Temp/%s_%s"),
			Name,
			*FGuid::NewGuid().ToString(EGuidFormats::Digits).Left(8))),
		FName(Name),
		BPTYPE_Normal,
		UBlueprint::StaticClass(),
		UBlueprintGeneratedClass::StaticClass());

	if (!Blueprint)
	{
		return nullptr;
	}

	FKismetEditorUtilities::CompileBlueprint(Blueprint);
	return Blueprint;
}

TSharedPtr<FJsonObject> MakeBatchSetClassDefaultsParams(UBlueprint* BlueprintA, UBlueprint* BlueprintB)
{
	TSharedPtr<FJsonObject> FirstItem = MakeShared<FJsonObject>();
	FirstItem->SetStringField(TEXT("target"), BlueprintA->GetPathName());
	TSharedPtr<FJsonObject> FirstProperties = MakeShared<FJsonObject>();
	FirstProperties->SetNumberField(TEXT("InitialLifeSpan"), 12.5);
	FirstItem->SetObjectField(TEXT("properties"), FirstProperties);
	FirstItem->SetBoolField(TEXT("compile"), false);
	FirstItem->SetBoolField(TEXT("save"), false);

	TSharedPtr<FJsonObject> SecondItem = MakeShared<FJsonObject>();
	SecondItem->SetStringField(TEXT("target"), BlueprintB->GetPathName());
	TSharedPtr<FJsonObject> SecondProperties = MakeShared<FJsonObject>();
	SecondProperties->SetNumberField(TEXT("InitialLifeSpan"), 20.0);
	SecondItem->SetObjectField(TEXT("properties"), SecondProperties);
	SecondItem->SetBoolField(TEXT("compile"), false);
	SecondItem->SetBoolField(TEXT("save"), false);

	TArray<TSharedPtr<FJsonValue>> Items;
	Items.Add(MakeShared<FJsonValueObject>(FirstItem));
	Items.Add(MakeShared<FJsonValueObject>(SecondItem));

	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetArrayField(TEXT("items"), Items);
	return Params;
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexBPSetClassDefaultsBatchTest,
	"Cortex.Blueprint.Batch.SetClassDefaults",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexBPSetClassDefaultsBatchTest::RunTest(const FString& Parameters)
{
	(void)Parameters;

	UBlueprint* BlueprintA = CreateBatchMutationBlueprint(TEXT("BP_BatchSetDefaultsA"));
	UBlueprint* BlueprintB = CreateBatchMutationBlueprint(TEXT("BP_BatchSetDefaultsB"));
	TestNotNull(TEXT("First blueprint created"), BlueprintA);
	TestNotNull(TEXT("Second blueprint created"), BlueprintB);
	if (!BlueprintA || !BlueprintB)
	{
		if (BlueprintA)
		{
			BlueprintA->MarkAsGarbage();
		}
		if (BlueprintB)
		{
			BlueprintB->MarkAsGarbage();
		}
		return false;
	}

	const FCortexCommandResult Result =
		FCortexBPClassDefaultsOps::SetClassDefaults(MakeBatchSetClassDefaultsParams(BlueprintA, BlueprintB));

	TestTrue(TEXT("batch succeeds"), Result.bSuccess);
	TestTrue(TEXT("result data exists"), Result.Data.IsValid());
	if (Result.Data.IsValid())
	{
		TestEqual(TEXT("status"), Result.Data->GetStringField(TEXT("status")), TEXT("committed"));

		const TArray<TSharedPtr<FJsonValue>>* PerItem = nullptr;
		TestTrue(TEXT("per_item exists"), Result.Data->TryGetArrayField(TEXT("per_item"), PerItem) && PerItem != nullptr);
		if (PerItem != nullptr)
		{
			TestEqual(TEXT("per_item count"), PerItem->Num(), 2);
		}
	}

	BlueprintA->MarkAsGarbage();
	BlueprintB->MarkAsGarbage();
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexBPBlockedDefaultsBatchTest,
	"Cortex.Blueprint.Batch.BlockedDefaults",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexBPBlockedDefaultsBatchTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	UBlueprint* Blueprint = CreateBatchMutationBlueprint(TEXT("BP_BatchBlockedDefaults"));
	TestNotNull(TEXT("blocked defaults blueprint created"), Blueprint);
	if (!Blueprint) return false;
	Blueprint->GetOutermost()->SetDirtyFlag(false);
	FCortexAssetMutationGuard::Block(Blueprint, TEXT("forced recovery verification failure"));

	const FCortexCommandResult ClassDefaults = FCortexBPClassDefaultsOps::SetClassDefaults(
		MakeBatchSetClassDefaultsParams(Blueprint, Blueprint));
	TestFalse(TEXT("blocked set_class_defaults batch item refuses before side effects"), ClassDefaults.bSuccess);
	TestFalse(TEXT("blocked class-defaults batch leaves package clean"), Blueprint->GetOutermost()->IsDirty());

	TSharedPtr<FJsonObject> Item = MakeShared<FJsonObject>();
	Item->SetStringField(TEXT("target"), Blueprint->GetPathName());
	Item->SetStringField(TEXT("component_name"), TEXT("NeverLoaded"));
	Item->SetObjectField(TEXT("properties"), MakeShared<FJsonObject>());
	TSharedPtr<FJsonObject> ComponentParams = MakeShared<FJsonObject>();
	ComponentParams->SetArrayField(TEXT("items"), { MakeShared<FJsonValueObject>(Item) });
	const FCortexCommandResult ComponentDefaults = FCortexBPComponentOps::SetComponentDefaults(ComponentParams);
	TestFalse(TEXT("blocked set_component_defaults batch item refuses before component lookup"), ComponentDefaults.bSuccess);
	TestFalse(TEXT("blocked component-defaults batch leaves package clean"), Blueprint->GetOutermost()->IsDirty());
	Blueprint->MarkAsGarbage();
	return true;
}
