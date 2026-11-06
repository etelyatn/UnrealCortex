#include "Misc/AutomationTest.h"

#include "Animation/AnimBlueprint.h"
#include "AnimGraphNode_SequencePlayer.h"
#include "CortexAnimationTestAnimBlueprint.h"
#include "CortexAnimationTestNativeInstance.h"
#include "CortexAnimationRebindTestSupport.h"

namespace
{
using namespace CortexAnimTest;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexAnimationRebindOccurrencesTest,
	"Cortex.Animation.RebindMember.Occurrences",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexAnimationRebindOccurrencesTest::RunTest(const FString& Parameters)
{
	UAnimBlueprint* AnimBlueprint = MakeAnimBlueprint(MakeFixtureSuffix(), UCortexAnimTestNativeInstance::StaticClass());
	if (!TestNotNull(TEXT("Anim Blueprint fixture"), AnimBlueprint))
	{
		return false;
	}
	FFixtureGuard Guard(AnimBlueprint);

	if (!TestTrue(TEXT("Blueprint getter GetDir"), AddBlueprintFloatGetter(AnimBlueprint, TEXT("GetDir")) != nullptr))
	{
		return false;
	}
	CompileAndAssertClean(*this, AnimBlueprint);

	// One node holds the source (PlayRate -> GetDir) next to a pre-existing use of the destination (StartPosition -> GetNativeDirection).
	// A second node binds a pin through an unrelated object whose class declares its own GetDir.
	UAnimGraphNode_SequencePlayer* Mixed = AddSequencePlayerNode(AnimBlueprint);
	UAnimGraphNode_SequencePlayer* Foreign = AddSequencePlayerNode(AnimBlueprint);
	if (!TestTrue(TEXT("sequence players"), Mixed != nullptr && Foreign != nullptr))
	{
		return false;
	}
	TestTrue(TEXT("source binding"), BindPropertyByReflection(Mixed, TEXT("PlayRate"), { TEXT("GetDir") }, /*bFunction=*/true));
	TestTrue(TEXT("pre-existing destination binding"), BindPropertyByReflection(Mixed, TEXT("StartPosition"), { TEXT("GetNativeDirection") }, /*bFunction=*/true));
	TestTrue(TEXT("foreign same-name binding"), BindPropertyByReflection(Foreign, TEXT("PlayRate"), { TEXT("ForeignRef"), TEXT("GetDir") }, /*bFunction=*/true));
	AnimBlueprint->GetPackage()->SetDirtyFlag(false);

	FCortexCommandRouter Router = MakeAnimGraphRouter();

	// ── The dry run plans exactly the source-owned occurrence ──
	{
		const FCortexCommandResult Dry = Router.Execute(TEXT("anim.rebind_function"), RebindParams(AnimBlueprint, TEXT("GetDir"), TEXT("GetNativeDirection"), true));
		if (TestTrue(*FString::Printf(TEXT("dry run succeeds (%s: %s)"), *Dry.ErrorCode, *Dry.ErrorMessage), Dry.bSuccess) && Dry.Data.IsValid())
		{
			const TArray<TSharedPtr<FJsonObject>> Planned = JsonObjectArray(Dry.Data, TEXT("would_retarget"));
			if (TestEqual(TEXT("exactly one occurrence is planned"), Planned.Num(), 1) && Planned.Num() == 1)
			{
				TestTrue(TEXT("it is the mixed node"), SameGuid(Planned[0]->GetStringField(TEXT("node_guid")), Mixed->NodeGuid));
				TestEqual(TEXT("it is the PlayRate binding"), Planned[0]->GetStringField(TEXT("slot")), FString(TEXT("PlayRate")));
			}
			bool bForeignListed = false;
			for (const TSharedPtr<FJsonObject>& Unmodified : JsonObjectArray(Dry.Data, TEXT("unmodified_references")))
			{
				FString NodeGuidText;
				if (Unmodified->TryGetStringField(TEXT("node_guid"), NodeGuidText) && SameGuid(NodeGuidText, Foreign->NodeGuid))
				{
					bForeignListed = Unmodified->GetStringField(TEXT("reason")) == TEXT("different_owner");
				}
			}
			TestTrue(TEXT("the foreign same-name binding is reported as unmodified (different owner)"), bForeignListed);
		}
		TestFalse(TEXT("a dry run leaves the package clean"), AnimBlueprint->GetPackage()->IsDirty());
	}

	// ── The apply migrates the owned occurrence and preserves every other binding ──
	const FCortexCommandResult Apply = Router.Execute(TEXT("anim.rebind_function"), RebindParams(AnimBlueprint, TEXT("GetDir"), TEXT("GetNativeDirection"), false));
	TestTrue(*FString::Printf(TEXT("the apply succeeds (%s: %s)"), *Apply.ErrorCode, *Apply.ErrorMessage), Apply.bSuccess);
	TestTrue(TEXT("the source binding now names the native getter"), ReportedBindingPath(Router, AnimBlueprint, Mixed->NodeGuid, TEXT("PlayRate")) == TArray<FString>{ TEXT("GetNativeDirection") });
	TestTrue(TEXT("the pre-existing destination binding is unchanged"), ReportedBindingPath(Router, AnimBlueprint, Mixed->NodeGuid, TEXT("StartPosition")) == TArray<FString>{ TEXT("GetNativeDirection") });
	TestTrue(TEXT("the foreign same-name binding is untouched"), ReportedBindingPath(Router, AnimBlueprint, Foreign->NodeGuid, TEXT("PlayRate")) == TArray<FString>{ TEXT("ForeignRef"), TEXT("GetDir") });

	// A success after the migration did not block the asset: the same asset accepts another guarded call.
	const FCortexCommandResult Again = Router.Execute(TEXT("anim.rebind_function"), RebindParams(AnimBlueprint, TEXT("GetDir"), TEXT("GetNativeDirection"), true));
	TestTrue(TEXT("the asset is not blocked after a valid migration"), Again.bSuccess || Again.ErrorCode != CortexErrorCodes::InvalidOperation);

	return true;
}
