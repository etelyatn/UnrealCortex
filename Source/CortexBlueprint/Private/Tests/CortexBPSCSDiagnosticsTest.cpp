#include "Misc/AutomationTest.h"
#include "Operations/CortexBPSCSDiagnostics.h"
#include "CortexBPTestLiftActor.h"
#include "Engine/Blueprint.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "Engine/SimpleConstructionScript.h"
#include "Engine/SCS_Node.h"
#include "Kismet2/KismetEditorUtilities.h"

namespace
{
	UBlueprint* CreateLiftBP(const TCHAR* Name)
	{
		return FKismetEditorUtilities::CreateBlueprint(
			ACortexBPTestLiftActor::StaticClass(),
			GetTransientPackage(),
			FName(Name),
			BPTYPE_Normal,
			UBlueprint::StaticClass(),
			UBlueprintGeneratedClass::StaticClass());
	}

	USCS_Node* AddSCSNode(UBlueprint* BP, UClass* ComponentClass, const TCHAR* VarName)
	{
		USCS_Node* Node = BP->SimpleConstructionScript->CreateNode(ComponentClass, FName(VarName));
		BP->SimpleConstructionScript->AddNode(Node);
		FKismetEditorUtilities::CompileBlueprint(BP);
		return BP->SimpleConstructionScript->FindSCSNode(FName(VarName));
	}

	UCortexBPTestSubobjComponent* GetTemplateComponent(UBlueprint* BP, USCS_Node* Node)
	{
		if (!BP || !Node)
		{
			return nullptr;
		}

		UBlueprintGeneratedClass* BPGC = Cast<UBlueprintGeneratedClass>(BP->GeneratedClass);
		if (!BPGC)
		{
			return nullptr;
		}

		return Cast<UCortexBPTestSubobjComponent>(Node->GetActualComponentTemplate(BPGC));
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexBPSCSDiagnosticsDirtyCleanTest,
	"Cortex.Blueprint.SCSDiagnostics.Dirty.CleanTemplate",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexBPSCSDiagnosticsDirtyCleanTest::RunTest(const FString& Parameters)
{
	UBlueprint* BP = CreateLiftBP(TEXT("BP_DirtyClean"));
	TestNotNull(TEXT("BP created"), BP);
	if (!BP)
	{
		return false;
	}

	USCS_Node* Node = AddSCSNode(BP, UCortexBPTestSubobjComponent::StaticClass(), TEXT("MyComp"));
	TestNotNull(TEXT("Node added"), Node);
	if (!Node)
	{
		BP->MarkAsGarbage();
		return false;
	}

	const FCortexBPSCSDiagnostics::FDirtyReport Report =
		FCortexBPSCSDiagnostics::DetectSCSNodeDirtyState(Node, BP);
	TestFalse(TEXT("Clean template has no loss"), Report.HasLoss());
	TestFalse(TEXT("Clean template has no sub-object loss"), Report.HasSubObjectLoss());
	TestEqual(TEXT("Clean template has zero ack keys"), Report.AcknowledgmentKeys.Num(), 0);

	BP->MarkAsGarbage();
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexBPSCSDiagnosticsDirtyTopLevelTest,
	"Cortex.Blueprint.SCSDiagnostics.Dirty.TopLevelOverride",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexBPSCSDiagnosticsDirtyTopLevelTest::RunTest(const FString& Parameters)
{
	UBlueprint* BP = CreateLiftBP(TEXT("BP_DirtyTopLevel"));
	TestNotNull(TEXT("BP created"), BP);
	if (!BP)
	{
		return false;
	}

	USCS_Node* Node = AddSCSNode(BP, UCortexBPTestSubobjComponent::StaticClass(), TEXT("MyComp"));
	TestNotNull(TEXT("Node added"), Node);
	UCortexBPTestSubobjComponent* Template = GetTemplateComponent(BP, Node);
	TestNotNull(TEXT("Template exists"), Template);
	if (!Node || !Template)
	{
		BP->MarkAsGarbage();
		return false;
	}

	Template->ComponentTags.Add(TEXT("TopLevelDirty"));

	const FCortexBPSCSDiagnostics::FDirtyReport Report =
		FCortexBPSCSDiagnostics::DetectSCSNodeDirtyState(Node, BP);
	TestFalse(TEXT("Top-level only dirty should not require acknowledgement"), Report.HasLoss());
	TestFalse(TEXT("Top-level only dirty is not sub-object loss"), Report.HasSubObjectLoss());
	TestTrue(TEXT("Top-level keys include ComponentTags"), Report.TopLevelKeys.Contains(TEXT("ComponentTags")));

	BP->MarkAsGarbage();
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexBPSCSDiagnosticsDirtySubObjectTest,
	"Cortex.Blueprint.SCSDiagnostics.Dirty.InstancedSubObject",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexBPSCSDiagnosticsDirtySubObjectTest::RunTest(const FString& Parameters)
{
	UBlueprint* BP = CreateLiftBP(TEXT("BP_DirtySubObject"));
	TestNotNull(TEXT("BP created"), BP);
	if (!BP)
	{
		return false;
	}

	USCS_Node* Node = AddSCSNode(BP, UCortexBPTestSubobjComponent::StaticClass(), TEXT("MyComp"));
	TestNotNull(TEXT("Node added"), Node);
	UCortexBPTestSubobjComponent* Template = GetTemplateComponent(BP, Node);
	TestNotNull(TEXT("Template exists"), Template);
	TestNotNull(TEXT("Instanced payload exists"), Template ? Template->Payload.Get() : nullptr);
	if (!Node || !Template || !Template->Payload)
	{
		BP->MarkAsGarbage();
		return false;
	}

	Template->Payload->Tracks.Add(123);

	const FCortexBPSCSDiagnostics::FDirtyReport Report =
		FCortexBPSCSDiagnostics::DetectSCSNodeDirtyState(Node, BP);
	TestTrue(TEXT("Instanced sub-object dirty requires acknowledgement"), Report.HasLoss());
	TestTrue(TEXT("Instanced sub-object dirty is sub-object loss"), Report.HasSubObjectLoss());
	TestTrue(TEXT("Sub-object keys include Payload"), Report.SubObjectKeys.Contains(TEXT("Payload")));
	TestTrue(TEXT("Acknowledgment keys include Payload"), Report.AcknowledgmentKeys.Contains(TEXT("Payload")));

	BP->MarkAsGarbage();
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexBPSCSDiagnosticsDirtyScopeLimitTest,
	"Cortex.Blueprint.SCSDiagnostics.Dirty.ScopeLimit.NonInstancedPointer",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexBPSCSDiagnosticsDirtyScopeLimitTest::RunTest(const FString& Parameters)
{
	UBlueprint* BP = CreateLiftBP(TEXT("BP_DirtyScopeLimit"));
	TestNotNull(TEXT("BP created"), BP);
	if (!BP)
	{
		return false;
	}

	USCS_Node* Node = AddSCSNode(BP, UCortexBPTestSubobjComponent::StaticClass(), TEXT("MyComp"));
	TestNotNull(TEXT("Node added"), Node);
	UCortexBPTestSubobjComponent* Template = GetTemplateComponent(BP, Node);
	TestNotNull(TEXT("Template exists"), Template);
	UCortexBPTestSubobjComponent* Archetype =
		Template ? Cast<UCortexBPTestSubobjComponent>(Template->GetArchetype()) : nullptr;
	TestNotNull(TEXT("Template archetype exists"), Archetype);
	if (!Node || !Template || !Archetype)
	{
		BP->MarkAsGarbage();
		return false;
	}

	UCortexBPTestSubobjPayload* SharedPayload = NewObject<UCortexBPTestSubobjPayload>(GetTransientPackage());
	TestNotNull(TEXT("Shared payload created"), SharedPayload);
	if (!SharedPayload)
	{
		BP->MarkAsGarbage();
		return false;
	}

	Template->PlainPayload = SharedPayload;
	Archetype->PlainPayload = SharedPayload;
	SharedPayload->Tracks.Add(17);

	const FCortexBPSCSDiagnostics::FDirtyReport Report =
		FCortexBPSCSDiagnostics::DetectSCSNodeDirtyState(Node, BP);
	TestFalse(TEXT("Non-instanced pointer internals are out of scope"), Report.HasLoss());
	TestFalse(TEXT("No sub-object loss for non-instanced pointer internals"), Report.HasSubObjectLoss());
	TestFalse(TEXT("No PlainPayload top-level diff expected"), Report.TopLevelKeys.Contains(TEXT("PlainPayload")));
	TestFalse(TEXT("No PlainPayload ack key expected"), Report.AcknowledgmentKeys.Contains(TEXT("PlainPayload")));

	BP->MarkAsGarbage();
	return true;
}
