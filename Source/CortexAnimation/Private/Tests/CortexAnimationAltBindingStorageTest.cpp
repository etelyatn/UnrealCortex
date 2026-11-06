#include "Misc/AutomationTest.h"

#include "Animation/AnimBlueprint.h"
#include "AnimGraphNode_SequencePlayer.h"
#include "CortexAnimationRebindTestSupport.h"
#include "CortexAnimationTestAltBinding.h"
#include "CortexAnimationTestAnimBlueprint.h"
#include "CortexAnimationTestNativeInstance.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexAnimationAlternateBindingStorageTest,
	"Cortex.Animation.AnimBlueprint.AlternateBindingStorage",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexAnimationAlternateBindingStorageTest::RunTest(const FString& Parameters)
{
	using namespace CortexAnimTest;

	UAnimBlueprint* AnimBlueprint = MakeAnimBlueprint(MakeFixtureSuffix(), UCortexAnimTestNativeInstance::StaticClass());
	if (!TestNotNull(TEXT("Anim Blueprint fixture"), AnimBlueprint))
	{
		return false;
	}
	FFixtureGuard Guard(AnimBlueprint);

	if (!TestNotNull(TEXT("Blueprint getter"), AddBlueprintFloatGetter(AnimBlueprint, TEXT("GetDir"))))
	{
		return false;
	}
	CompileAndAssertClean(*this, AnimBlueprint);

	// One node uses the engine's default binding storage; the other uses a different implementation of the same engine interface.
	UAnimGraphNode_SequencePlayer* Default = AddSequencePlayerNode(AnimBlueprint);
	UAnimGraphNode_SequencePlayer* Alternate = AddSequencePlayerNode(AnimBlueprint);
	if (!TestTrue(TEXT("sequence players"), Default != nullptr && Alternate != nullptr))
	{
		return false;
	}
	TestTrue(TEXT("default storage binding"), BindPropertyByReflection(Default, TEXT("PlayRate"), { TEXT("NativeSpeed") }));

	const FObjectProperty* BindingProperty = FindFProperty<FObjectProperty>(Alternate->GetClass(), TEXT("Binding"));
	UCortexAnimTestAltBinding* AltBinding = NewObject<UCortexAnimTestAltBinding>(Alternate, NAME_None, RF_Transactional);
	if (!TestTrue(TEXT("the alternate storage object is created"), BindingProperty != nullptr && AltBinding != nullptr))
	{
		return false;
	}
	BindingProperty->SetObjectPropertyValue_InContainer(Alternate, AltBinding);
	TestTrue(TEXT("the node holds the alternate storage (an engine UAnimGraphNodeBinding, not the default class)"),
		Alternate->GetBinding() == AltBinding && AltBinding->GetClass() != BindingProperty->PropertyClass);
	// A slot naming the Blueprint getter: an owned occurrence the rebind would have to journal.
	Alternate->UpdateFunction.SetSelfMember(TEXT("GetDir"));
	AnimBlueprint->GetPackage()->SetDirtyFlag(false);

	FCortexCommandRouter Router = MakeAnimGraphRouter();

	// ── The report is per node: the default node's bindings are read, the alternate node is unsupported and silent ──
	{
		TSharedPtr<FJsonObject> Params = MakeAssetParams(AnimBlueprint);
		Params->SetArrayField(TEXT("include"), { MakeShared<FJsonValueString>(TEXT("bindings")) });
		const FCortexCommandResult Info = Router.Execute(TEXT("anim.get_animbp_info"), Params);
		if (TestTrue(TEXT("the report succeeds with an alternate storage node present"), Info.bSuccess) && TestTrue(TEXT("data"), Info.Data.IsValid()))
		{
			TestTrue(TEXT("the default node's binding is reported"), ReportedBindingPath(Router, AnimBlueprint, Default->NodeGuid, TEXT("PlayRate")) == TArray<FString>{ TEXT("NativeSpeed") });
			TestTrue(TEXT("the alternate node reports no bindings"), ReportedBindingPath(Router, AnimBlueprint, Alternate->NodeGuid, TEXT("PlayRate")) == TArray<FString>{ TEXT("<missing>") });

			const TArray<TSharedPtr<FJsonObject>> Unsupported = JsonObjectArray(Info.Data, TEXT("unsupported_binding_nodes"));
			if (TestEqual(TEXT("exactly one node is unsupported"), Unsupported.Num(), 1) && Unsupported.Num() == 1)
			{
				TestTrue(TEXT("it is the alternate node"), SameGuid(Unsupported[0]->GetStringField(TEXT("node_guid")), Alternate->NodeGuid));
				TestEqual(TEXT("binding_storage"), Unsupported[0]->GetStringField(TEXT("binding_storage")), FString(TEXT("unsupported")));
			}
		}
	}

	// ── A rebind that would have to journal the alternate node refuses before it changes anything ──
	{
		FCompileCounter Compiles(AnimBlueprint);
		const FCortexCommandResult Refused = Rebind(Router, AnimBlueprint, TEXT("GetDir"), TEXT("GetNativeDirection"), false);
		TestFalse(TEXT("the rebind refuses"), Refused.bSuccess);
		TestEqual(TEXT("UNSUPPORTED_OPERATION"), Refused.ErrorCode, CortexErrorCodes::UnsupportedOperation);
		FString NodeGuidText;
		FString Storage;
		if (Refused.ErrorDetails.IsValid())
		{
			Refused.ErrorDetails->TryGetStringField(TEXT("node_guid"), NodeGuidText);
			Refused.ErrorDetails->TryGetStringField(TEXT("binding_storage"), Storage);
		}
		TestTrue(TEXT("the refusal names the node"), SameGuid(NodeGuidText, Alternate->NodeGuid));
		TestEqual(TEXT("and its storage"), Storage, FString(TEXT("unsupported")));
		TestEqual(TEXT("nothing was compiled"), Compiles.Count, 0);
		TestFalse(TEXT("nothing was dirtied"), AnimBlueprint->GetPackage()->IsDirty());
		TestTrue(TEXT("the slot is unchanged"), Alternate->UpdateFunction.GetMemberName() == FName(TEXT("GetDir")));
		TestTrue(TEXT("the default node is unchanged"), ReportedBindingPath(Router, AnimBlueprint, Default->NodeGuid, TEXT("PlayRate")) == TArray<FString>{ TEXT("NativeSpeed") });
	}

	return true;
}
