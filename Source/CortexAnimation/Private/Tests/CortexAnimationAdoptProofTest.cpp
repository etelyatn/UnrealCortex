#include "Misc/AutomationTest.h"

#include "Animation/AnimBlueprint.h"
#include "AnimGraphNode_SequencePlayer.h"
#include "CortexAnimationTestAnimBlueprint.h"
#include "CortexAnimationTestNativeInstance.h"
#include "Serialization/JsonSerializer.h"
#include "CortexCommandRouter.h"
#include "CortexCoreModule.h"
#include "CortexGraphFingerprint.h"
#include "CortexGraphMemberReferences.h"
#include "EdGraph/EdGraph.h"
#include "K2Node_VariableGet.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Modules/ModuleManager.h"
#include "UObject/CoreRedirects.h"

namespace
{
using namespace CortexAnimTest;

/** Registers the property redirect a renamed native member needs while the adoption resolves it (the #121 test pattern). */
struct FAdoptProofRedirectScope
{
	FAdoptProofRedirectScope(const UClass* Owner, const TCHAR* OldName, const TCHAR* NewName)
	{
		const FName Package = Owner->GetOutermost()->GetFName();
		const FName OuterName = Owner->GetFName();
		Redirects.Add(FCoreRedirect(ECoreRedirectFlags::Type_Property,
			FCoreRedirectObjectName(FName(OldName), OuterName, Package),
			FCoreRedirectObjectName(FName(NewName), OuterName, Package)));
		FCoreRedirects::AddRedirectList(Redirects, TEXT("CortexAnimationAdoptProofTest"));
	}
	~FAdoptProofRedirectScope() { FCoreRedirects::RemoveRedirectList(Redirects, TEXT("CortexAnimationAdoptProofTest")); }

	TArray<FCoreRedirect> Redirects;
};

FCortexCommandResult CoreCommand(const TCHAR* Command, const TSharedPtr<FJsonObject>& Params)
{
	FCortexCoreModule& Core = FModuleManager::GetModuleChecked<FCortexCoreModule>(TEXT("CortexCore"));
	return Core.GetCommandRouter().Execute(Command, Params);
}

TArray<FString> VariableReferenceKinds(const UBlueprint* AnimBlueprint, const FName Variable)
{
	TArray<FString> Kinds;
	TArray<FCortexMemberReference> References;
	FCortexGraphMemberReferences::FindVariableReferences(AnimBlueprint, Variable, References);
	for (const FCortexMemberReference& Reference : References)
	{
		Kinds.Add(Reference.Kind);
	}
	Kinds.Sort();
	return Kinds;
}

TArray<FString> ReportedPath(const TSharedPtr<FJsonObject>& Data, const TCHAR* Collection, const FGuid& NodeGuid, const TCHAR* Field, const TCHAR* FieldValue)
{
	for (const TSharedPtr<FJsonObject>& Entry : CollectionItems(Data, Collection))
	{
		FString NodeGuidText;
		FString Value;
		if (!Entry->TryGetStringField(TEXT("node_guid"), NodeGuidText) || !SameGuid(NodeGuidText, NodeGuid))
		{
			continue;
		}
		if (Field != nullptr && (!Entry->TryGetStringField(Field, Value) || Value != FieldValue))
		{
			continue;
		}
		TArray<FString> Path;
		const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
		if (Entry->TryGetArrayField(TEXT("path"), Values) && Values != nullptr)
		{
			for (const TSharedPtr<FJsonValue>& Segment : *Values)
			{
				FString Text;
				if (Segment.IsValid() && Segment->TryGetString(Text))
				{
					Path.Add(Text);
				}
			}
		}
		return Path;
	}
	return {};
}

TSharedPtr<FJsonObject> AnimInfo(const UAnimBlueprint* AnimBlueprint)
{
	FCortexCommandRouter Router = MakeAnimGraphRouter();
	TSharedPtr<FJsonObject> Params = MakeAssetParams(AnimBlueprint);
	Params->SetArrayField(TEXT("include"), { MakeShared<FJsonValueString>(TEXT("bindings")), MakeShared<FJsonValueString>(TEXT("property_access")) });
	const FCortexCommandResult Result = Router.Execute(TEXT("anim.get_animbp_info"), Params);
	return Result.bSuccess ? Result.Data : nullptr;
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexAnimationAdoptNativeMemberAnimBindingsTest,
	"Cortex.Animation.AdoptNativeMember.AnimBindings",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexAnimationAdoptNativeMemberAnimBindingsTest::RunTest(const FString& Parameters)
{
	UAnimBlueprint* AnimBlueprint = MakeAnimBlueprint(MakeFixtureSuffix(), UCortexAnimTestNativeInstance::StaticClass());
	if (!TestNotNull(TEXT("Anim Blueprint fixture"), AnimBlueprint))
	{
		return false;
	}
	FFixtureGuard Guard(AnimBlueprint);

	TestTrue(TEXT("Speed variable"), AddVariable(AnimBlueprint, TEXT("Speed"), FloatPinType(), TEXT("0.0")));
	CompileAndAssertClean(*this, AnimBlueprint);
	UAnimationGraph* AnimGraph = FindRootAnimGraph(AnimBlueprint);
	UAnimGraphNode_SequencePlayer* Player = AddSequencePlayerNode(AnimBlueprint);
	if (!TestTrue(TEXT("fixture nodes"), AnimGraph != nullptr && Player != nullptr))
	{
		return false;
	}
	TestTrue(TEXT("anim binding"), BindPropertyByReflection(Player, TEXT("PlayRate"), { TEXT("Speed") }));
	UEdGraphNode* Access = AddPropertyAccessNode(AnimGraph, { TEXT("Speed") });
	FGraphNodeCreator<UK2Node_VariableGet> GetCreator(*AnimGraph);
	UK2Node_VariableGet* Get = GetCreator.CreateNode();
	Get->VariableReference.SetSelfMember(TEXT("Speed"));
	GetCreator.Finalize();
	if (!TestTrue(TEXT("property access and get node"), Access != nullptr && Get != nullptr))
	{
		return false;
	}

	const TArray<FString> Before = VariableReferenceKinds(AnimBlueprint, TEXT("Speed"));
	TestEqual(TEXT("three Blueprint references before the adoption"), Before.Num(), 3);
	TestTrue(TEXT("the anim binding is one of them"), Before.Contains(TEXT("anim_node_binding")));
	TestTrue(TEXT("the property-access node is one of them"), Before.Contains(TEXT("property_access_node")));

	// #121 refuses the removal while the nodes name the variable.
	{
		TSharedPtr<FJsonObject> DryParams = MakeAssetParams(AnimBlueprint);
		DryParams->SetStringField(TEXT("name"), TEXT("Speed"));
		DryParams->SetBoolField(TEXT("dry_run"), true);
		const FCortexCommandResult Dry = CoreCommand(TEXT("blueprint.remove_variable"), DryParams);
		TestTrue(TEXT("the removal dry run succeeds"), Dry.bSuccess);
		TSharedPtr<FJsonObject> ApplyParams = MakeAssetParams(AnimBlueprint);
		ApplyParams->SetStringField(TEXT("name"), TEXT("Speed"));
		ApplyParams->SetObjectField(TEXT("expected_fingerprint"), FCortexGraphFingerprint::Compute(AnimBlueprint));
		ApplyParams->SetBoolField(TEXT("compile"), true);
		const FCortexCommandResult Apply = CoreCommand(TEXT("blueprint.remove_variable"), ApplyParams);
		TestFalse(TEXT("remove_variable refuses while nodes reference Speed"), Apply.bSuccess);
		TestEqual(TEXT("MEMBER_REFERENCED"), Apply.ErrorCode, FString(TEXT("MEMBER_REFERENCED")));
	}

	// The adoption retargets anim bindings and property-access nodes through the engine hooks.
	{
		const FAdoptProofRedirectScope Redirect(UCortexAnimTestNativeInstance::StaticClass(), TEXT("Speed"), TEXT("NativeSpeed"));
		const TSharedRef<FJsonObject> Mapping = MakeShared<FJsonObject>();
		Mapping->SetStringField(TEXT("bp_variable"), TEXT("Speed"));
		Mapping->SetStringField(TEXT("native_property"), TEXT("NativeSpeed"));
		TSharedPtr<FJsonObject> Params = MakeAssetParams(AnimBlueprint);
		Params->SetArrayField(TEXT("mappings"), { MakeShared<FJsonValueObject>(Mapping) });
		Params->SetBoolField(TEXT("retarget_nodes"), true);
		const FCortexCommandResult Adopt = CoreCommand(TEXT("blueprint.adopt_native_member"), Params);
		if (TestTrue(*FString::Printf(TEXT("the adoption succeeds (%s: %s)"), *Adopt.ErrorCode, *Adopt.ErrorMessage), Adopt.bSuccess) && Adopt.Data.IsValid())
		{
			TestEqual(TEXT("readback matched"), Adopt.Data->GetStringField(TEXT("readback_status")), FString(TEXT("matched")));
		}
	}

	TestTrue(TEXT("the variable is gone"), FBlueprintEditorUtils::FindNewVariableIndex(AnimBlueprint, TEXT("Speed")) == INDEX_NONE);
	TestEqual(TEXT("no reference to Speed remains"), VariableReferenceKinds(AnimBlueprint, TEXT("Speed")).Num(), 0);

	const TSharedPtr<FJsonObject> After = AnimInfo(AnimBlueprint);
	if (TestTrue(TEXT("the AnimBP report is readable after the adoption"), After.IsValid()))
	{
		TestTrue(TEXT("the anim binding follows to the native property"), ReportedPath(After, TEXT("bindings"), Player->NodeGuid, TEXT("property_name"), TEXT("PlayRate")) == TArray<FString>{ TEXT("NativeSpeed") });
		TestTrue(TEXT("the property-access path follows"), ReportedPath(After, TEXT("property_access"), Access->NodeGuid, nullptr, nullptr) == TArray<FString>{ TEXT("NativeSpeed") });

		bool bBindingResolvesNative = false;
		for (const TSharedPtr<FJsonObject>& Entry : CollectionItems(After, TEXT("bindings")))
		{
			FString NodeGuidText;
			if (Entry->TryGetStringField(TEXT("node_guid"), NodeGuidText) && SameGuid(NodeGuidText, Player->NodeGuid))
			{
				bBindingResolvesNative = Entry->GetBoolField(TEXT("resolved"))
					&& Entry->GetStringField(TEXT("root_declared_in")) == TEXT("native");
			}
		}
		TestTrue(TEXT("the binding resolves as a native member"), bBindingResolvesNative);
	}

	TestTrue(TEXT("the retargeted references resolve"), VariableReferenceKinds(AnimBlueprint, TEXT("NativeSpeed")).Num() >= 2);
	TestTrue(TEXT("the Anim Blueprint compiles"), AnimBlueprint->Status != BS_Error);

	return true;
}
