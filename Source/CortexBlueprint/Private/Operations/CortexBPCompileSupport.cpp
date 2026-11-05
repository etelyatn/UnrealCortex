#include "Operations/CortexBPCompileSupport.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "Engine/Blueprint.h"
#include "K2Node_CallFunction.h"
#include "K2Node_DynamicCast.h"
#include "K2Node_Event.h"
#include "K2Node_Variable.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/CompilerResultsLog.h"

#if WITH_DEV_AUTOMATION_TESTS
namespace
{
	TFunction<void(UBlueprint*, EBlueprintCompileOptions)> GCompileObserverForTest;
}
#endif

namespace
{
	FString ToSeverity(const UEdGraphNode* Node)
	{
		if (!Node)
		{
			return TEXT("error");
		}

		return (Node->ErrorType <= EMessageSeverity::Error) ? TEXT("error") : TEXT("warning");
	}

	void AddReferencedMetadata(const UEdGraphNode* Node, const TSharedPtr<FJsonObject>& Diagnostic)
	{
		if (!Node || !Diagnostic.IsValid())
		{
			return;
		}

		const UBlueprint* Blueprint = FBlueprintEditorUtils::FindBlueprintForNode(Node);
		UClass* SelfScope = nullptr;
		if (Blueprint)
		{
			SelfScope = Blueprint->GeneratedClass ? Blueprint->GeneratedClass : Blueprint->SkeletonGeneratedClass;
		}

		if (const UK2Node_CallFunction* CallNode = Cast<UK2Node_CallFunction>(Node))
		{
			UClass* ParentClass = CallNode->FunctionReference.GetMemberParentClass(SelfScope);
			if (!ParentClass)
			{
				ParentClass = CallNode->FunctionReference.GetMemberParentClass();
			}
			if (ParentClass)
			{
				Diagnostic->SetStringField(TEXT("referenced_class"), ParentClass->GetName());
			}

			const FName MemberName = CallNode->FunctionReference.GetMemberName();
			if (!MemberName.IsNone())
			{
				Diagnostic->SetStringField(TEXT("referenced_member"), MemberName.ToString());
			}
			return;
		}

		if (const UK2Node_Variable* VariableNode = Cast<UK2Node_Variable>(Node))
		{
			UClass* ParentClass = VariableNode->VariableReference.GetMemberParentClass(SelfScope);
			if (!ParentClass)
			{
				ParentClass = VariableNode->VariableReference.GetMemberParentClass();
			}
			if (ParentClass)
			{
				Diagnostic->SetStringField(TEXT("referenced_class"), ParentClass->GetName());
			}

			const FName MemberName = VariableNode->VariableReference.GetMemberName();
			if (!MemberName.IsNone())
			{
				Diagnostic->SetStringField(TEXT("referenced_member"), MemberName.ToString());
			}
			return;
		}

		if (const UK2Node_Event* EventNode = Cast<UK2Node_Event>(Node))
		{
			UClass* ParentClass = EventNode->EventReference.GetMemberParentClass(SelfScope);
			if (!ParentClass)
			{
				ParentClass = EventNode->EventReference.GetMemberParentClass();
			}
			if (ParentClass)
			{
				Diagnostic->SetStringField(TEXT("referenced_class"), ParentClass->GetName());
			}

			const FName MemberName = EventNode->EventReference.GetMemberName();
			if (!MemberName.IsNone())
			{
				Diagnostic->SetStringField(TEXT("referenced_member"), MemberName.ToString());
			}
			return;
		}

		if (const UK2Node_DynamicCast* DynamicCastNode = Cast<UK2Node_DynamicCast>(Node))
		{
			if (const UClass* TargetClass = DynamicCastNode->TargetType)
			{
				Diagnostic->SetStringField(TEXT("referenced_class"), TargetClass->GetName());
			}
		}
	}

	TSharedPtr<FJsonObject> BuildDiagnosticObject(const UEdGraphNode* Node)
	{
		TSharedPtr<FJsonObject> Diagnostic = MakeShared<FJsonObject>();
		Diagnostic->SetStringField(TEXT("graph"), Node && Node->GetGraph() ? Node->GetGraph()->GetName() : TEXT(""));
		Diagnostic->SetStringField(TEXT("node_id"), Node ? Node->NodeGuid.ToString() : TEXT(""));
		Diagnostic->SetStringField(TEXT("node_class"), Node ? Node->GetClass()->GetName() : TEXT(""));
		Diagnostic->SetStringField(TEXT("node_name"),
			Node ? Node->GetNodeTitle(ENodeTitleType::ListView).ToString() : TEXT(""));
		Diagnostic->SetStringField(TEXT("severity"), ToSeverity(Node));
		Diagnostic->SetStringField(TEXT("message"), Node ? Node->ErrorMsg : TEXT(""));
		AddReferencedMetadata(Node, Diagnostic);
		return Diagnostic;
	}
}

TSharedPtr<FJsonObject> FCortexBPCompileOutcome::ToPayload(const FString& AssetPath) const
{
	TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
	Payload->SetStringField(TEXT("asset_path"), AssetPath);
	Payload->SetStringField(TEXT("compile_status"), Status);
	Payload->SetStringField(TEXT("status"), Status);
	Payload->SetNumberField(TEXT("error_count"), ErrorCount);
	Payload->SetNumberField(TEXT("warning_count"), WarningCount);
	Payload->SetArrayField(TEXT("diagnostics"), Diagnostics);
	return Payload;
}

void FCortexBPCompileSupport::Compile(UBlueprint* Blueprint, EBlueprintCompileOptions Options, FCortexBPCompileOutcome& OutOutcome)
{
	OutOutcome = FCortexBPCompileOutcome();
	if (Blueprint == nullptr)
	{
		OutOutcome.Status = TEXT("error");
		OutOutcome.ErrorCount = 1;
		return;
	}

	// A pending reparent (the generated class still derives from the old parent): compile with the editor's delta
	// flags so a native-parent default that differs from the old CDO wins and restored inherited values survive.
	if (Blueprint->GeneratedClass != nullptr && Blueprint->ParentClass != nullptr
		&& Blueprint->GeneratedClass->GetSuperClass() != Blueprint->ParentClass)
	{
		Options |= EBlueprintCompileOptions::UseDeltaSerializationDuringReinstancing
			| EBlueprintCompileOptions::SkipNewVariableDefaultsDetection;
	}
	OutOutcome.EffectiveOptions = Options;

#if WITH_DEV_AUTOMATION_TESTS
	if (GCompileObserverForTest)
	{
		GCompileObserverForTest(Blueprint, Options);
	}
#endif

	FCompilerResultsLog CompilerResults;
	CompilerResults.bSilentMode = true;
	CompilerResults.bAnnotateMentionedNodes = true;
	FKismetEditorUtilities::CompileBlueprint(Blueprint, Options, &CompilerResults);

	OutOutcome.ErrorCount = CompilerResults.NumErrors;
	OutOutcome.WarningCount = CompilerResults.NumWarnings;
	OutOutcome.Status = OutOutcome.ErrorCount > 0 ? TEXT("error") : (OutOutcome.WarningCount > 0 ? TEXT("warning") : TEXT("success"));

	OutOutcome.Diagnostics.Reserve(CompilerResults.AnnotatedNodes.Num());
	for (const TWeakObjectPtr<UEdGraphNode>& WeakNode : CompilerResults.AnnotatedNodes)
	{
		const UEdGraphNode* Node = WeakNode.Get();
		if (!Node || !Node->bHasCompilerMessage)
		{
			continue;
		}

		OutOutcome.Diagnostics.Add(MakeShared<FJsonValueObject>(BuildDiagnosticObject(Node)));
	}
}

#if WITH_DEV_AUTOMATION_TESTS
void FCortexBPCompileSupport::SetCompileObserverForTest(TFunction<void(UBlueprint*, EBlueprintCompileOptions)> Observer)
{
	GCompileObserverForTest = MoveTemp(Observer);
}

void FCortexBPCompileSupport::ClearCompileObserverForTest()
{
	GCompileObserverForTest = nullptr;
}
#endif
