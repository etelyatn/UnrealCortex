#include "Misc/AutomationTest.h"
#include "CortexAnimationCommandHandler.h"
#include "CortexCommandRouter.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

namespace
{
FCortexCommandRouter CreateAnimInspectRouter()
{
	FCortexCommandRouter Router;
	Router.RegisterDomain(TEXT("anim"), TEXT("Cortex Animation"), TEXT("1.0.0"), MakeShared<FCortexAnimationCommandHandler>());
	return Router;
}

TSharedPtr<FJsonObject> AssetParams(const FString& AssetPath)
{
	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("asset_path"), AssetPath);
	return Params;
}

bool HasCollection(const TSharedPtr<FJsonObject>& Data, const TCHAR* FieldName)
{
	const TSharedPtr<FJsonObject>* Collection = nullptr;
	if (!Data.IsValid() || !Data->TryGetObjectField(FieldName, Collection) || Collection == nullptr)
	{
		return false;
	}

	return (*Collection)->HasField(TEXT("count"))
		&& (*Collection)->HasField(TEXT("returned"))
		&& (*Collection)->HasField(TEXT("truncated"))
		&& (*Collection)->HasTypedField<EJson::Array>(TEXT("items"));
}

bool CollectionReturnedAtMost(const TSharedPtr<FJsonObject>& Collection, int32 Limit)
{
	int32 Returned = 0;
	return Collection.IsValid()
		&& Collection->TryGetNumberField(TEXT("returned"), Returned)
		&& Returned <= Limit;
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexAnimationSequenceInfoTest,
	"Cortex.Animation.Sequence.Info",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexAnimationSequenceInfoTest::RunTest(const FString& Parameters)
{
	FCortexCommandRouter Router = CreateAnimInspectRouter();
	FCortexCommandResult Result = Router.Execute(TEXT("anim.get_sequence_info"), AssetParams(TEXT("/Game/Characters/Mannequins/Anims/Pistol/MM_Pistol_Fire")));
	TestTrue(TEXT("sequence info should succeed"), Result.bSuccess);
	TestTrue(TEXT("data should be valid"), Result.Data.IsValid());
	TestEqual(TEXT("asset_type"), Result.Data->GetStringField(TEXT("asset_type")), FString(TEXT("AnimSequence")));
	TestTrue(TEXT("length_seconds exists"), Result.Data->HasField(TEXT("length_seconds")));
	TestTrue(TEXT("skeleton exists"), Result.Data->HasField(TEXT("skeleton")));
	TestTrue(TEXT("sampling_frame_rate exists"), Result.Data->HasField(TEXT("sampling_frame_rate")));
	TestTrue(TEXT("notifies collection exists"), HasCollection(Result.Data, TEXT("notifies")));
	TestTrue(TEXT("curves collection exists"), HasCollection(Result.Data, TEXT("curves")));
	TestTrue(TEXT("sync_markers collection exists"), HasCollection(Result.Data, TEXT("sync_markers")));
	TestTrue(TEXT("fingerprint exists"), Result.Data->HasTypedField<EJson::Object>(TEXT("fingerprint")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexAnimationMontageInfoTest,
	"Cortex.Animation.Montage.Info",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexAnimationMontageInfoTest::RunTest(const FString& Parameters)
{
	FCortexCommandRouter Router = CreateAnimInspectRouter();
	TSharedPtr<FJsonObject> Params = AssetParams(TEXT("/Game/Characters/Mannequins/Anims/Pistol/MM_Pistol_Fire_Montage"));
	Params->SetNumberField(TEXT("segment_limit"), 1);
	FCortexCommandResult Result = Router.Execute(TEXT("anim.get_montage_info"), Params);
	TestTrue(TEXT("montage info should succeed"), Result.bSuccess);
	TestTrue(TEXT("data should be valid"), Result.Data.IsValid());
	TestEqual(TEXT("asset_type"), Result.Data->GetStringField(TEXT("asset_type")), FString(TEXT("AnimMontage")));
	TestTrue(TEXT("length_seconds exists"), Result.Data->HasField(TEXT("length_seconds")));
	TestTrue(TEXT("sections collection exists"), HasCollection(Result.Data, TEXT("sections")));
	TestTrue(TEXT("slots collection exists"), HasCollection(Result.Data, TEXT("slots")));
	TestTrue(TEXT("notifies collection exists"), HasCollection(Result.Data, TEXT("notifies")));
	TestTrue(TEXT("branching_points collection exists"), HasCollection(Result.Data, TEXT("branching_points")));
	TestTrue(TEXT("fingerprint exists"), Result.Data->HasTypedField<EJson::Object>(TEXT("fingerprint")));

	const TSharedPtr<FJsonObject>* Slots = nullptr;
	if (Result.Data->TryGetObjectField(TEXT("slots"), Slots) && Slots != nullptr)
	{
		const TArray<TSharedPtr<FJsonValue>>* SlotItems = nullptr;
		if ((*Slots)->TryGetArrayField(TEXT("items"), SlotItems) && SlotItems != nullptr && SlotItems->Num() > 0)
		{
			const TSharedPtr<FJsonObject> FirstSlot = (*SlotItems)[0]->AsObject();
			const TSharedPtr<FJsonObject>* Segments = nullptr;
			TestTrue(TEXT("slot segments collection exists"), FirstSlot.IsValid() && FirstSlot->TryGetObjectField(TEXT("segments"), Segments));
			TestTrue(TEXT("segment_limit caps returned segments"), Segments != nullptr && CollectionReturnedAtMost(*Segments, 1));
		}
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexAnimationSkeletonInfoTest,
	"Cortex.Animation.Skeleton.Info",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexAnimationSkeletonInfoTest::RunTest(const FString& Parameters)
{
	FCortexCommandRouter Router = CreateAnimInspectRouter();
	FCortexCommandResult Result = Router.Execute(TEXT("anim.get_skeleton_info"), AssetParams(TEXT("/Game/Characters/Mannequins/Meshes/SK_Mannequin")));
	TestTrue(TEXT("skeleton info should succeed"), Result.bSuccess);
	TestTrue(TEXT("data should be valid"), Result.Data.IsValid());
	TestEqual(TEXT("asset_type"), Result.Data->GetStringField(TEXT("asset_type")), FString(TEXT("Skeleton")));
	TestTrue(TEXT("bones collection exists"), HasCollection(Result.Data, TEXT("bones")));
	TestTrue(TEXT("sockets collection exists"), HasCollection(Result.Data, TEXT("sockets")));
	TestTrue(TEXT("virtual_bones collection exists"), HasCollection(Result.Data, TEXT("virtual_bones")));
	TestTrue(TEXT("fingerprint exists"), Result.Data->HasTypedField<EJson::Object>(TEXT("fingerprint")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexAnimationAnimBlueprintInfoTest,
	"Cortex.Animation.AnimBlueprint.Info",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexAnimationAnimBlueprintInfoTest::RunTest(const FString& Parameters)
{
	FCortexCommandRouter Router = CreateAnimInspectRouter();
	FCortexCommandResult Result = Router.Execute(TEXT("anim.get_animbp_info"), AssetParams(TEXT("/Game/Characters/Mannequins/Anims/Unarmed/ABP_Unarmed")));
	TestTrue(TEXT("AnimBP info should succeed"), Result.bSuccess);
	TestTrue(TEXT("data should be valid"), Result.Data.IsValid());
	TestEqual(TEXT("asset_type"), Result.Data->GetStringField(TEXT("asset_type")), FString(TEXT("AnimBlueprint")));
	TestTrue(TEXT("target_skeleton exists"), Result.Data->HasField(TEXT("target_skeleton")));
	TestTrue(TEXT("parent_class exists"), Result.Data->HasField(TEXT("parent_class")));
	TestTrue(TEXT("generated_class exists"), Result.Data->HasField(TEXT("generated_class")));
	TestTrue(TEXT("state_machines collection exists"), HasCollection(Result.Data, TEXT("state_machines")));
	TestTrue(TEXT("fingerprint exists"), Result.Data->HasTypedField<EJson::Object>(TEXT("fingerprint")));
	return true;
}
