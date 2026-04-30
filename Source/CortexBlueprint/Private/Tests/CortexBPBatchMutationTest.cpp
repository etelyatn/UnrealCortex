#include "Misc/AutomationTest.h"

#include "Operations/CortexBPClassDefaultsOps.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Engine/Blueprint.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "GameFramework/Actor.h"

namespace
{
UBlueprint* CreateBatchMutationBlueprint(const TCHAR* Name)
{
	UBlueprint* Blueprint = FKismetEditorUtilities::CreateBlueprint(
		AActor::StaticClass(),
		GetTransientPackage(),
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
