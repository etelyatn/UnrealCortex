#include "CortexGraphMemberReferences.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "EdGraph/EdGraph.h"
#include "Engine/Blueprint.h"
#include "K2Node.h"
#include "K2Node_ActorBoundEvent.h"
#include "K2Node_BaseMCDelegate.h"
#include "K2Node_ComponentBoundEvent.h"
#include "K2Node_CallFunction.h"
#include "K2Node_CreateDelegate.h"
#include "K2Node_Variable.h"
#include "UObject/Class.h"
#include "UObject/UObjectGlobals.h"

namespace CortexGraphMemberReferencesPrivate
{
	enum class EMemberQuery : uint8
	{
		Variable,
		Function
	};

	UClass* FindOptionalClass(const TCHAR* Path)
	{
		return FindObject<UClass>(nullptr, Path);
	}

	void Collect(const UBlueprint* Blueprint, FName Member, EMemberQuery Query, TArray<FCortexMemberReference>& OutReferences)
	{
		if (Blueprint == nullptr || Member.IsNone())
		{
			return;
		}

		const UClass* AnimNodeBase = FindOptionalClass(TEXT("/Script/AnimGraph.AnimGraphNode_Base"));
		const UClass* PropertyAccessNode = FindOptionalClass(TEXT("/Script/PropertyAccessNode.K2Node_PropertyAccess"));

		TArray<UEdGraph*> Graphs;
		Blueprint->GetAllGraphs(Graphs);

		for (const UEdGraph* Graph : Graphs)
		{
			if (Graph == nullptr)
			{
				continue;
			}

			for (const UEdGraphNode* GraphNode : Graph->Nodes)
			{
				const UK2Node* Node = Cast<UK2Node>(GraphNode);
				if (Node == nullptr)
				{
					continue;
				}

				FCortexMemberReference Reference;
				const UK2Node_Variable* VariableNode = Cast<UK2Node_Variable>(Node);
				const bool bIsAnimNode = AnimNodeBase != nullptr && Node->IsA(AnimNodeBase);
				const bool bIsPropertyAccess = PropertyAccessNode != nullptr && Node->IsA(PropertyAccessNode);

				bool bReferences = false;
				if (Query == EMemberQuery::Variable)
				{
					bReferences = Node->ReferencesVariable(Member, nullptr);
					if (bReferences && VariableNode != nullptr && VariableNode->VariableReference.IsLocalScope())
					{
						// Same-named local variable: not a member reference.
						bReferences = false;
					}
					if (!bReferences)
					{
						// Dispatcher (multicast delegate variable) nodes do not override ReferencesVariable.
						if (const UK2Node_BaseMCDelegate* DelegateNode = Cast<UK2Node_BaseMCDelegate>(Node))
						{
							bReferences = DelegateNode->DelegateReference.GetMemberName() == Member;
						}
						else if (const UK2Node_ActorBoundEvent* ActorEvent = Cast<UK2Node_ActorBoundEvent>(Node))
						{
							// Bound events store the delegate by name and override neither hook.
							bReferences = ActorEvent->DelegatePropertyName == Member;
						}
						else if (const UK2Node_ComponentBoundEvent* ComponentEvent = Cast<UK2Node_ComponentBoundEvent>(Node))
						{
							bReferences = ComponentEvent->DelegatePropertyName == Member;
						}
					}
				}
				else
				{
					bReferences = Node->ReferencesFunction(Member, nullptr);
					if (!bReferences)
					{
						if (const UK2Node_CallFunction* CallNode = Cast<UK2Node_CallFunction>(Node))
						{
							bReferences = CallNode->FunctionReference.GetMemberName() == Member;
						}
						else if (const UK2Node_CreateDelegate* DelegateNode = Cast<UK2Node_CreateDelegate>(Node))
						{
							bReferences = DelegateNode->GetFunctionName() == Member;
						}
					}
				}

				if (!bReferences)
				{
					continue;
				}

				if (VariableNode != nullptr)
				{
					Reference.Kind = TEXT("k2_variable_node");
					Reference.bSelfContext = VariableNode->VariableReference.IsSelfContext();
				}
				else if (bIsAnimNode)
				{
					Reference.Kind = Query == EMemberQuery::Function ? TEXT("anim_node_function") : TEXT("anim_node_binding");
				}
				else if (bIsPropertyAccess)
				{
					Reference.Kind = TEXT("property_access_node");
				}
				else
				{
					Reference.Kind = TEXT("other_k2_node");
				}

				Reference.GraphName = Graph->GetName();
				Reference.GraphGuid = Graph->GraphGuid;
				Reference.NodeName = Node->GetName();
				Reference.NodeGuid = Node->NodeGuid;
				Reference.NodeClass = Node->GetClass()->GetName();
				OutReferences.Add(MoveTemp(Reference));
			}
		}
	}
}

void FCortexGraphMemberReferences::FindVariableReferences(const UBlueprint* Blueprint, FName VariableName, TArray<FCortexMemberReference>& OutReferences)
{
	CortexGraphMemberReferencesPrivate::Collect(Blueprint, VariableName, CortexGraphMemberReferencesPrivate::EMemberQuery::Variable, OutReferences);
}

void FCortexGraphMemberReferences::FindFunctionReferences(const UBlueprint* Blueprint, FName FunctionName, TArray<FCortexMemberReference>& OutReferences)
{
	CortexGraphMemberReferencesPrivate::Collect(Blueprint, FunctionName, CortexGraphMemberReferencesPrivate::EMemberQuery::Function, OutReferences);
}

TSharedRef<FJsonObject> FCortexGraphMemberReferences::ReferenceToJson(const FCortexMemberReference& Reference)
{
	TSharedRef<FJsonObject> Json = MakeShared<FJsonObject>();
	Json->SetStringField(TEXT("graph_name"), Reference.GraphName);
	Json->SetStringField(TEXT("graph_guid"), Reference.GraphGuid.ToString(EGuidFormats::DigitsWithHyphens));
	Json->SetStringField(TEXT("node_name"), Reference.NodeName);
	Json->SetStringField(TEXT("node_guid"), Reference.NodeGuid.ToString(EGuidFormats::DigitsWithHyphens));
	Json->SetStringField(TEXT("node_class"), Reference.NodeClass);
	Json->SetStringField(TEXT("kind"), Reference.Kind);
	Json->SetStringField(TEXT("slot"), Reference.Slot);
	Json->SetBoolField(TEXT("self_context"), Reference.bSelfContext);
	return Json;
}

TSharedPtr<FJsonObject> FCortexGraphMemberReferences::ToJson(const UBlueprint* Blueprint, FName Member, const TArray<FCortexMemberReference>& References)
{
	bool bNativeParentDeclares = false;
	if (Blueprint != nullptr && Blueprint->ParentClass != nullptr)
	{
		const UClass* ParentClass = Blueprint->ParentClass;
		const FProperty* Property = FindFProperty<FProperty>(ParentClass, Member);
		const UFunction* Function = ParentClass->FindFunctionByName(Member);
		const UClass* Owner = Property != nullptr ? Property->GetOwnerClass() : (Function != nullptr ? Function->GetOwnerClass() : nullptr);
		bNativeParentDeclares = Owner != nullptr && !Owner->HasAnyClassFlags(CLASS_CompiledFromBlueprint);
	}

	TSharedPtr<FJsonObject> Json = MakeShared<FJsonObject>();
	Json->SetStringField(TEXT("member"), Member.ToString());
	Json->SetBoolField(TEXT("native_parent_declares"), bNativeParentDeclares);
	Json->SetNumberField(TEXT("reference_count"), References.Num());

	TArray<TSharedPtr<FJsonValue>> Values;
	Values.Reserve(References.Num());
	for (const FCortexMemberReference& Reference : References)
	{
		Values.Add(MakeShared<FJsonValueObject>(ReferenceToJson(Reference)));
	}
	Json->SetArrayField(TEXT("references"), Values);
	return Json;
}
