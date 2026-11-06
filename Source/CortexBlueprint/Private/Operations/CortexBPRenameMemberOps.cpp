#include "Operations/CortexBPRenameMemberOps.h"

#include "CortexBlueprintModule.h"
#include "CortexCommandRouter.h"
#include "CortexGraphMemberReferences.h"
#include "CortexTypes.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "EdGraph/EdGraph.h"
#include "EdGraphSchema_K2.h"
#include "Engine/Blueprint.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "Engine/SCS_Node.h"
#include "Engine/SimpleConstructionScript.h"
#include "K2Node_ActorBoundEvent.h"
#include "K2Node_BaseMCDelegate.h"
#include "K2Node_CallFunction.h"
#include "K2Node_ComponentBoundEvent.h"
#include "K2Node_FunctionEntry.h"
#include "K2Node_Variable.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/Kismet2NameValidators.h"
#include "Misc/SecureHash.h"
#include "Operations/CortexBPAssetOps.h"
#include "Operations/CortexBPCommandParams.h"
#include "Operations/CortexBPDependentScan.h"
#include "Operations/CortexBPGuardedRun.h"
#include "Operations/CortexBPMigrationGuard.h"
#include "Operations/CortexBPSaveGameGuard.h"
#include "UObject/UnrealType.h"


namespace CortexBPRenameMemberPrivate
{
#if WITH_DEV_AUTOMATION_TESTS
	TFunction<void(UBlueprint*)> GPostApplyHookForTest;
#endif

	enum class EKind : uint8
	{
		Variable,
		Function,
		Dispatcher,
		Local
	};

	const TCHAR* KindName(const EKind Kind)
	{
		switch (Kind)
		{
		case EKind::Variable: return TEXT("variable");
		case EKind::Function: return TEXT("function");
		case EKind::Dispatcher: return TEXT("dispatcher");
		default: return TEXT("local");
		}
	}

	struct FRequest
	{
		EKind Kind = EKind::Variable;
		FName Old;
		FName New;
		/** local only: the function graph that owns the variable. */
		FName Function;
	};

	/** A reason the rename cannot be applied; reported by a dry run and refused by apply. */
	struct FBlocker
	{
		FString Code;
		FString Message;
		TArray<TSharedPtr<FJsonValue>> Items;

		TSharedRef<FJsonObject> ToJson() const
		{
			TSharedRef<FJsonObject> Json = MakeShared<FJsonObject>();
			Json->SetStringField(TEXT("code"), Code);
			Json->SetStringField(TEXT("message"), Message);
			Json->SetArrayField(TEXT("items"), Items);
			return Json;
		}
	};

	struct FPlan
	{
		TArray<FBlocker> Blockers;
		/** Every reference to the old name in the Blueprint itself (before the rename). */
		TArray<FCortexMemberReference> References;
		TArray<FCortexBPDependentHit> Dependents;
		TArray<FCortexBPSaveGameGuard::FFinding> SaveGameFindings;
		TArray<FString> RequiredAcknowledgment;
		/** Nodes of this Blueprint that the engine retargets and that must therefore carry the new name afterwards. */
		TSet<FGuid> FollowNodes;
		/** local only: member variable nodes that share the local's name inside the function (the engine would retarget them). */
		TArray<FCortexMemberReference> ShadowedMemberNodes;

		void AppendTo(const UBlueprint* Blueprint, const FRequest& Request, const TSharedRef<FJsonObject>& Target) const
		{
			TArray<TSharedPtr<FJsonValue>> BlockerValues;
			for (const FBlocker& Blocker : Blockers)
			{
				BlockerValues.Add(MakeShared<FJsonValueObject>(Blocker.ToJson()));
			}
			Target->SetArrayField(TEXT("blocking"), BlockerValues);

			TArray<TSharedPtr<FJsonValue>> DependentValues;
			for (const FCortexBPDependentHit& Hit : Dependents)
			{
				DependentValues.Add(MakeShared<FJsonValueObject>(Hit.ToJson()));
			}
			Target->SetArrayField(TEXT("dependents"), DependentValues);

			TArray<TSharedPtr<FJsonValue>> FindingValues;
			for (const FCortexBPSaveGameGuard::FFinding& Finding : SaveGameFindings)
			{
				FindingValues.Add(MakeShared<FJsonValueObject>(Finding.ToJson()));
			}
			Target->SetArrayField(TEXT("save_game_findings"), FindingValues);

			TArray<TSharedPtr<FJsonValue>> AckValues;
			for (const FString& Key : RequiredAcknowledgment)
			{
				AckValues.Add(MakeShared<FJsonValueString>(Key));
			}
			Target->SetArrayField(TEXT("required_acknowledgment"), AckValues);

			const TSharedRef<FJsonObject> ReferencesJson = MakeShared<FJsonObject>();
			ReferencesJson->SetObjectField(TEXT("before"), FCortexGraphMemberReferences::ToJson(Blueprint, Request.Old, References).ToSharedRef());
			Target->SetObjectField(TEXT("references"), ReferencesJson);
		}
	};

	using CortexBPParams::ReadOptionalBool;
	using CortexBPParams::ReadRequiredString;
	using CortexBPParams::ToJsonStrings;

	void CollectGraphTree(UEdGraph* Graph, TArray<UEdGraph*>& OutGraphs)
	{
		if (Graph == nullptr)
		{
			return;
		}
		OutGraphs.Add(Graph);
		for (UEdGraph* Sub : Graph->SubGraphs)
		{
			CollectGraphTree(Sub, OutGraphs);
		}
	}

	UEdGraph* FindFunctionGraph(const UBlueprint* Blueprint, const FName Name)
	{
		for (UEdGraph* Graph : Blueprint->FunctionGraphs)
		{
			if (Graph != nullptr && Graph->GetFName() == Name)
			{
				return Graph;
			}
		}
		return nullptr;
	}

	UK2Node_FunctionEntry* FindEntryNode(const UEdGraph* FunctionGraph)
	{
		for (UEdGraphNode* Node : FunctionGraph->Nodes)
		{
			if (UK2Node_FunctionEntry* Entry = Cast<UK2Node_FunctionEntry>(Node))
			{
				return Entry;
			}
		}
		return nullptr;
	}

	UEdGraphNode* FindNodeByGuid(const UBlueprint* Blueprint, const FGuid& Guid)
	{
		TArray<UEdGraph*> Graphs;
		Blueprint->GetAllGraphs(Graphs);
		for (const UEdGraph* Graph : Graphs)
		{
			for (UEdGraphNode* Node : Graph->Nodes)
			{
				if (Node != nullptr && Node->NodeGuid == Guid)
				{
					return Node;
				}
			}
		}
		return nullptr;
	}

	bool TargetsOwner(const UClass* Class, const UBlueprint* Owner)
	{
		return Class != nullptr && UBlueprint::GetBlueprintFromClass(Class) == Owner;
	}

	/** The class that declares the delegate a bound-event node listens to; null for other nodes. */
	const UClass* BoundEventDelegateOwner(const UEdGraphNode* Node)
	{
		if (const UK2Node_ActorBoundEvent* Actor = Cast<UK2Node_ActorBoundEvent>(Node))
		{
			return Actor->DelegateOwnerClass;
		}
		if (const UK2Node_ComponentBoundEvent* Component = Cast<UK2Node_ComponentBoundEvent>(Node))
		{
			return Component->DelegateOwnerClass;
		}
		return nullptr;
	}

	/** Dispatcher nodes the engine's variable rename does not reach: bind/call nodes that target the Blueprint from outside the self context and bound events. */
	void RetargetDispatcherNodes(UBlueprint* Target, const FName Old, const FName New)
	{
		TArray<UEdGraph*> Graphs;
		Target->GetAllGraphs(Graphs);
		for (const UEdGraph* Graph : Graphs)
		{
			for (UEdGraphNode* GraphNode : Graph->Nodes)
			{
				if (UK2Node_BaseMCDelegate* Delegate = Cast<UK2Node_BaseMCDelegate>(GraphNode))
				{
					if (!Delegate->DelegateReference.IsSelfContext() && Delegate->DelegateReference.GetMemberName() == Old
						&& TargetsOwner(Delegate->DelegateReference.GetMemberParentClass(), Target))
					{
						Delegate->Modify();
						Delegate->DelegateReference.SetExternalMember(New, Delegate->DelegateReference.GetMemberParentClass());
					}
				}
				else if (UK2Node_ActorBoundEvent* Actor = Cast<UK2Node_ActorBoundEvent>(GraphNode))
				{
					if (Actor->DelegatePropertyName == Old && TargetsOwner(Actor->DelegateOwnerClass, Target))
					{
						Actor->Modify();
						Actor->DelegatePropertyName = New;
					}
				}
				else if (UK2Node_ComponentBoundEvent* Component = Cast<UK2Node_ComponentBoundEvent>(GraphNode))
				{
					if (Component->DelegatePropertyName == Old && TargetsOwner(Component->DelegateOwnerClass, Target))
					{
						Component->Modify();
						Component->DelegatePropertyName = New;
					}
				}
			}
		}
	}

	/** True when the engine retargets the node together with the member (nodes of unrelated classes sharing the name stay). */
	bool FollowsRename(const UEdGraphNode* Node, const UBlueprint* Owner, const EKind Kind)
	{
		if (Kind == EKind::Variable || Kind == EKind::Dispatcher)
		{
			if (const UK2Node_Variable* Variable = Cast<UK2Node_Variable>(Node))
			{
				return !Variable->VariableReference.IsLocalScope()
					&& (Variable->VariableReference.IsSelfContext() || TargetsOwner(Variable->VariableReference.GetMemberParentClass(), Owner));
			}
			if (const UK2Node_BaseMCDelegate* Delegate = Cast<UK2Node_BaseMCDelegate>(Node))
			{
				return Delegate->DelegateReference.IsSelfContext() || TargetsOwner(Delegate->DelegateReference.GetMemberParentClass(), Owner);
			}
			if (Cast<UK2Node_ActorBoundEvent>(Node) != nullptr || Cast<UK2Node_ComponentBoundEvent>(Node) != nullptr)
			{
				// Binding nodes name the dispatcher only; the command retargets those that bind this Blueprint's own dispatcher.
				return Kind == EKind::Dispatcher && TargetsOwner(BoundEventDelegateOwner(Node), Owner);
			}
			return true;
		}
		if (Kind == EKind::Function)
		{
			if (const UK2Node_CallFunction* Call = Cast<UK2Node_CallFunction>(Node))
			{
				return Call->FunctionReference.IsSelfContext()
					|| UBlueprint::GetBlueprintFromClass(Call->FunctionReference.GetMemberParentClass()) == Owner;
			}
			return true;
		}
		return true;
	}

	TSet<FGuid> FollowSet(const UBlueprint* Blueprint, const TArray<FCortexMemberReference>& References, const EKind Kind)
	{
		TSet<FGuid> Out;
		for (const FCortexMemberReference& Reference : References)
		{
			const UEdGraphNode* Node = FindNodeByGuid(Blueprint, Reference.NodeGuid);
			if (Node != nullptr && FollowsRename(Node, Blueprint, Kind))
			{
				Out.Add(Reference.NodeGuid);
			}
		}
		return Out;
	}

	FCortexMemberReference MakeReference(const UEdGraph* Graph, const UK2Node* Node, const bool bSelfContext)
	{
		FCortexMemberReference Reference;
		Reference.GraphName = Graph->GetName();
		Reference.GraphGuid = Graph->GraphGuid;
		Reference.NodeName = Node->GetName();
		Reference.NodeGuid = Node->NodeGuid;
		Reference.NodeClass = Node->GetClass()->GetName();
		Reference.Kind = TEXT("k2_variable_node");
		Reference.bSelfContext = bSelfContext;
		return Reference;
	}

	/** Local variable nodes of the function (subgraphs included) and member variable nodes that share the name. */
	void CollectLocalNodes(
		UEdGraph* FunctionGraph, const FName Name, TArray<UK2Node_Variable*>& OutLocalNodes, TArray<FCortexMemberReference>& OutMemberNodes)
	{
		TArray<UEdGraph*> Graphs;
		CollectGraphTree(FunctionGraph, Graphs);
		for (UEdGraph* Graph : Graphs)
		{
			for (UEdGraphNode* GraphNode : Graph->Nodes)
			{
				UK2Node_Variable* Node = Cast<UK2Node_Variable>(GraphNode);
				if (Node == nullptr || Node->GetVarName() != Name)
				{
					continue;
				}
				if (Node->VariableReference.IsLocalScope())
				{
					if (Node->VariableReference.GetMemberScopeName() == FunctionGraph->GetName())
					{
						OutLocalNodes.Add(Node);
					}
				}
				else
				{
					OutMemberNodes.Add(MakeReference(Graph, Node, Node->VariableReference.IsSelfContext()));
				}
			}
		}
	}

	void CollectReferences(UBlueprint* Blueprint, const FRequest& Request, TArray<FCortexMemberReference>& OutReferences, TArray<FCortexMemberReference>* OutShadowedMemberNodes)
	{
		switch (Request.Kind)
		{
		case EKind::Variable:
		case EKind::Dispatcher:
			FCortexGraphMemberReferences::FindVariableReferences(Blueprint, Request.Old, OutReferences);
			break;
		case EKind::Function:
			FCortexGraphMemberReferences::FindFunctionReferences(Blueprint, Request.Old, OutReferences);
			break;
		case EKind::Local:
			if (UEdGraph* Graph = FindFunctionGraph(Blueprint, Request.Function))
			{
				TArray<UK2Node_Variable*> LocalNodes;
				TArray<FCortexMemberReference> MemberNodes;
				CollectLocalNodes(Graph, Request.Old, LocalNodes, MemberNodes);
				for (UK2Node_Variable* Node : LocalNodes)
				{
					OutReferences.Add(MakeReference(Cast<UEdGraph>(Node->GetOuter()), Node, /*bSelfContext=*/false));
				}
				if (OutShadowedMemberNodes != nullptr)
				{
					*OutShadowedMemberNodes = MoveTemp(MemberNodes);
				}
			}
			break;
		}
	}

	/** A hash of every function's local variables and of every dispatcher node, for the restoration check. */
	FString OperationStateDigest(UBlueprint* Blueprint)
	{
		FString Buffer;
		TArray<UK2Node_FunctionEntry*> Entries;
		FBlueprintEditorUtils::GetAllNodesOfClass(Blueprint, Entries);
		for (const UK2Node_FunctionEntry* Entry : Entries)
		{
			Buffer += FString::Printf(TEXT("Entry %s\n"), *Entry->GetOuter()->GetName());
			for (const FBPVariableDescription& Variable : Entry->LocalVariables)
			{
				Buffer += FString::Printf(TEXT("  Local %s %s %s %s %llu %s\n"),
					*Variable.VarName.ToString(), *Variable.VarGuid.ToString(), *Variable.VarType.PinCategory.ToString(),
					*Variable.FriendlyName, static_cast<uint64>(Variable.PropertyFlags), *Variable.DefaultValue);
			}
		}
		TArray<UK2Node_BaseMCDelegate*> DelegateNodes;
		FBlueprintEditorUtils::GetAllNodesOfClass(Blueprint, DelegateNodes);
		for (const UK2Node_BaseMCDelegate* Node : DelegateNodes)
		{
			Buffer += FString::Printf(TEXT("Delegate %s %s %d\n"),
				*Node->NodeGuid.ToString(), *Node->DelegateReference.GetMemberName().ToString(), Node->DelegateReference.IsSelfContext() ? 1 : 0);
		}
		return FMD5::HashAnsiString(*Buffer);
	}

	/** Where a name is already taken in the Blueprint, as text. */
	FString DescribeExisting(UBlueprint* Blueprint, const FName Name)
	{
		if (FBlueprintEditorUtils::FindNewVariableIndex(Blueprint, Name) != INDEX_NONE)
		{
			return TEXT("a variable of this Blueprint");
		}
		if (Blueprint->ParentClass != nullptr)
		{
			if (const FProperty* Property = FindFProperty<FProperty>(Blueprint->ParentClass, Name))
			{
				return FString::Printf(TEXT("a property declared by the parent class %s"), *Property->GetOwnerClass()->GetName());
			}
			if (const UFunction* Function = Blueprint->ParentClass->FindFunctionByName(Name))
			{
				return FString::Printf(TEXT("a function declared by the parent class %s"), *Function->GetOwnerClass()->GetName());
			}
		}
		TArray<UEdGraph*> Graphs;
		Blueprint->GetAllGraphs(Graphs);
		for (const UEdGraph* Graph : Graphs)
		{
			if (Graph->GetFName() == Name)
			{
				return TEXT("a graph of this Blueprint");
			}
		}
		if (Blueprint->SimpleConstructionScript != nullptr)
		{
			for (const USCS_Node* Node : Blueprint->SimpleConstructionScript->GetAllNodes())
			{
				if (Node->GetVariableName() == Name)
				{
					return TEXT("a component of this Blueprint");
				}
			}
		}
		return TEXT("already in use in this Blueprint");
	}

	/** Where a Blueprint (a child) declares Name itself; empty when it does not. */
	FString ChildDeclares(const UBlueprint* Child, const FName Name)
	{
		if (FBlueprintEditorUtils::FindNewVariableIndex(Child, Name) != INDEX_NONE)
		{
			return TEXT("a variable");
		}
		TArray<UEdGraph*> Graphs;
		Child->GetAllGraphs(Graphs);
		for (const UEdGraph* Graph : Graphs)
		{
			if (Graph->GetFName() == Name)
			{
				return TEXT("a graph");
			}
		}
		if (Child->SimpleConstructionScript != nullptr)
		{
			for (const USCS_Node* Node : Child->SimpleConstructionScript->GetAllNodes())
			{
				if (Node->GetVariableName() == Name)
				{
					return TEXT("a component");
				}
			}
		}
		// Members the child's own classes declare; inherited ones are the owner's and are checked against the owner itself.
		for (const UClass* Class : {Child->SkeletonGeneratedClass.Get(), Child->GeneratedClass.Get()})
		{
			if (Class == nullptr)
			{
				continue;
			}
			const FProperty* Property = Class->FindPropertyByName(Name);
			if ((Property != nullptr && Property->GetOwnerClass() == Class)
				|| Class->FindFunctionByName(Name, EIncludeSuperFlag::ExcludeSuper) != nullptr)
			{
				return TEXT("a class member");
			}
		}
		return FString();
	}

	void AddBlocker(FPlan& Plan, const FString& Code, const FString& Message, const TArray<FString>& Items = TArray<FString>())
	{
		FBlocker& Blocker = Plan.Blockers.AddDefaulted_GetRef();
		Blocker.Code = Code;
		Blocker.Message = Message;
		Blocker.Items = ToJsonStrings(Items);
	}

	/** Everything the rename must not run into, computed read-only. */
	void BuildPlan(UBlueprint* Blueprint, const FRequest& Request, FPlan& Plan)
	{
		CollectReferences(Blueprint, Request, Plan.References, &Plan.ShadowedMemberNodes);
		Plan.FollowNodes = FollowSet(Blueprint, Plan.References, Request.Kind);

		const FString OldString = Request.Old.ToString();
		const FString NewString = Request.New.ToString();

		if (Request.Kind == EKind::Variable || Request.Kind == EKind::Dispatcher)
		{
			const int32 Index = FBlueprintEditorUtils::FindNewVariableIndex(Blueprint, Request.Old);
			if (Index != INDEX_NONE)
			{
				const FBPVariableDescription& Variable = Blueprint->NewVariables[Index];
				if (Variable.RepNotifyFunc != NAME_None)
				{
					AddBlocker(Plan, CortexErrorCodes::InvalidOperation,
						FString::Printf(TEXT("Variable '%s' has the RepNotify function '%s'; the engine would ask a question in a modal dialog and break the association. Clear the RepNotify first."),
							*OldString, *Variable.RepNotifyFunc.ToString()));
				}
				if (Variable.HasMetaData(FBlueprintMetadata::MD_FieldNotify))
				{
					AddBlocker(Plan, CortexErrorCodes::InvalidOperation,
						FString::Printf(TEXT("Variable '%s' is a FieldNotify variable; the engine would not move its FieldNotify registration with the name."), *OldString));
				}
			}
		}
		else if (Request.Kind == EKind::Function)
		{
			if (const UEdGraph* Graph = FindFunctionGraph(Blueprint, Request.Old))
			{
				if (const UK2Node_FunctionEntry* Entry = FindEntryNode(Graph); Entry != nullptr && Entry->MetaData.HasMetaData(FBlueprintMetadata::MD_FieldNotify))
				{
					AddBlocker(Plan, CortexErrorCodes::InvalidOperation,
						FString::Printf(TEXT("Function '%s' is a FieldNotify function; the engine would not move its FieldNotify registration with the name."), *OldString));
				}
			}
		}
		else
		{
			// The engine's local rename retargets the member variable nodes of the function graph itself whose member class is this
			// Blueprint's (self context included); nodes of unrelated classes and nodes in sub graphs are not touched.
			UClass* Generated = Blueprint->GeneratedClass;
			const UEdGraph* FunctionGraph = FindFunctionGraph(Blueprint, Request.Function);
			for (const FCortexMemberReference& Shadowed : Plan.ShadowedMemberNodes)
			{
				const UK2Node_Variable* Node = Cast<UK2Node_Variable>(FindNodeByGuid(Blueprint, Shadowed.NodeGuid));
				const UClass* MemberClass = Node != nullptr ? Node->VariableReference.GetMemberParentClass(Generated) : nullptr;
				if (Node != nullptr && Node->GetGraph() == FunctionGraph && Generated != nullptr && MemberClass != nullptr && MemberClass->IsChildOf(Generated))
				{
					AddBlocker(Plan, CortexErrorCodes::InvalidOperation,
						FString::Printf(TEXT("Function '%s' contains member variable nodes named '%s'; the engine's local rename would retarget them too. Rename or remove those nodes first."),
							*Request.Function.ToString(), *OldString),
						{FString::Printf(TEXT("%s/%s"), *Shadowed.GraphName, *Shadowed.NodeName)});
					break;
				}
			}
		}

		// The new name: the editor's own validator for the Blueprint (variables, functions of the parent and interfaces, graphs, SCS, locals).
		{
			const UStruct* Scope = nullptr;
			if (Request.Kind == EKind::Local && Blueprint->SkeletonGeneratedClass != nullptr)
			{
				Scope = Blueprint->SkeletonGeneratedClass->FindFunctionByName(Request.Function);
			}
			FKismetNameValidator Validator(Blueprint, NAME_None, Scope);
			const EValidatorResult Result = Validator.IsValid(NewString);
			const bool bParentDeclares = Request.Kind != EKind::Local && Blueprint->ParentClass != nullptr
				&& (FindFProperty<FProperty>(Blueprint->ParentClass, Request.New) != nullptr
					|| Blueprint->ParentClass->FindFunctionByName(Request.New) != nullptr);
			if (Result == EValidatorResult::AlreadyInUse || Result == EValidatorResult::LocallyInUse || bParentDeclares)
			{
				AddBlocker(Plan, CortexErrorCodes::NameCollision,
					FString::Printf(TEXT("'%s' is already taken: %s."), *NewString, *DescribeExisting(Blueprint, Request.New)));
			}
			else if (Request.Kind == EKind::Local)
			{
				// Same function: another local or a parameter of that name.
				if (UEdGraph* Graph = FindFunctionGraph(Blueprint, Request.Function))
				{
					if (FBlueprintEditorUtils::FindLocalVariable(Blueprint, Graph, Request.New) != nullptr)
					{
						AddBlocker(Plan, CortexErrorCodes::NameCollision,
							FString::Printf(TEXT("Function '%s' already has a local variable '%s'."), *Request.Function.ToString(), *NewString));
					}
				}
			}
		}


		// Dependents: derived classes (a child that declares the new name, or overrides the old function) and every
		// Blueprint that references the member (the engine would rewrite only the loaded ones).
		FCortexBPDependentScan::Find(Blueprint, Request.Old,
			Request.Kind == EKind::Function ? FCortexBPDependentScan::EMemberKind::Function : FCortexBPDependentScan::EMemberKind::Variable,
			Plan.Dependents,
			// A local variable cannot be referenced from another asset, so nothing outside the child hierarchy can name it: skip the
			// registry referencers (and their maps) entirely instead of loading them to find nothing.
			Request.Kind == EKind::Local ? FCortexBPDependentScan::EScanScope::ChildrenOnly : FCortexBPDependentScan::EScanScope::All);
		if (Request.Kind == EKind::Local)
		{
			// A local variable cannot be referenced from another asset; only children (whose members ValidateBlueprintChildVariables
			// would rename) and uninspectable dependents matter.
			Plan.Dependents.RemoveAll([](const FCortexBPDependentHit& Hit) { return Hit.Role != TEXT("child") && !Hit.bUnscanned; });
		}

		TArray<FString> ChildCollisions;
		TArray<FString> ChildOverrides;
		TArray<FString> Referencing;
		TArray<FString> Unscanned;
		for (const FCortexBPDependentHit& Hit : Plan.Dependents)
		{
			if (Hit.bUnscanned)
			{
				Unscanned.Add(Hit.AssetPath);
				continue;
			}
			if (Request.Kind != EKind::Local && Hit.References.Num() > 0)
			{
				Referencing.Add(FString::Printf(TEXT("%s (%d references)"), *Hit.AssetPath, Hit.References.Num()));
			}
			if (Hit.Role == TEXT("child"))
			{
				const UBlueprint* Child = LoadObject<UBlueprint>(nullptr, *Hit.AssetPath);
				if (Child == nullptr)
				{
					Unscanned.Add(Hit.AssetPath);
					continue;
				}
				const FString Declares = ChildDeclares(Child, Request.New);
				if (!Declares.IsEmpty())
				{
					ChildCollisions.Add(FString::Printf(TEXT("%s declares '%s' as %s"), *Hit.AssetPath, *NewString, *Declares));
				}
				if (Request.Kind == EKind::Function && FindFunctionGraph(Child, Request.Old) != nullptr)
				{
					ChildOverrides.Add(Hit.AssetPath);
				}
			}
		}

		if (!ChildCollisions.IsEmpty())
		{
			AddBlocker(Plan, CortexErrorCodes::NameCollision,
				FString::Printf(TEXT("A child Blueprint already declares '%s'; the engine would silently rename the child's member."), *NewString), ChildCollisions);
		}
		if (!ChildOverrides.IsEmpty())
		{
			AddBlocker(Plan, CortexErrorCodes::InvalidOperation,
				FString::Printf(TEXT("A child Blueprint has a function graph '%s' (an override); the engine would rename it too."), *OldString), ChildOverrides);
		}
		if (!Referencing.IsEmpty())
		{
			AddBlocker(Plan, CortexErrorCodes::InvalidOperation,
				FString::Printf(TEXT("Other Blueprints reference '%s'; the engine rewrites only the loaded ones and leaves the rest stale. Retarget them first."), *OldString), Referencing);
		}
		if (!Unscanned.IsEmpty())
		{
			AddBlocker(Plan, CortexErrorCodes::InvalidOperation,
				TEXT("Some dependent Blueprints could not be inspected, so the rename cannot be proven safe."), Unscanned);
		}

		if (Request.Kind == EKind::Variable || Request.Kind == EKind::Dispatcher)
		{
			const int32 Index = FBlueprintEditorUtils::FindNewVariableIndex(Blueprint, Request.Old);
			if (Index != INDEX_NONE)
			{
				FCortexBPSaveGameGuard::EvaluateRename(Blueprint->NewVariables[Index], Request.New, Plan.SaveGameFindings);
				Plan.RequiredAcknowledgment = FCortexBPSaveGameGuard::RequiredAcknowledgment(Plan.SaveGameFindings);
			}
		}
	}

	/** State the capture, apply, verify and restore callbacks share. */
	struct FApplyState
	{
		FBPVariableDescription VariableBefore;
		bool bHaveVariableBefore = false;
		FString OldDefaultText;
		bool bHaveOldDefault = false;
		TArray<FBPVariableDescription> LocalsBefore;
		TArray<FGuid> LocalNodeGuids;
		TArray<FGuid> MemberNodeGuids;
		TArray<FCortexMemberReference> ReferencesAfter;
		bool bCompiled = false;
	};

	FString ReadCdoText(UBlueprint* Blueprint, const FName Name)
	{
		UClass* Class = Blueprint->GeneratedClass;
		UObject* Cdo = Class != nullptr ? Class->GetDefaultObject(false) : nullptr;
		const FProperty* Property = Cdo != nullptr ? FindFProperty<FProperty>(Cdo->GetClass(), Name) : nullptr;
		FString Text;
		if (Property != nullptr && !Property->IsA<FMulticastDelegateProperty>()
			&& FBlueprintEditorUtils::PropertyValueToString(Property, reinterpret_cast<const uint8*>(Cdo), Text))
		{
			return Text;
		}
		return FString();
	}

	bool VerifyUpToDate(const UBlueprint* Target, bool bCompiled, FString& OutError)
	{
		if (bCompiled && Target->Status != BS_UpToDate && Target->Status != BS_UpToDateWithWarnings)
		{
			OutError = TEXT("The Blueprint is not up to date after the compile");
			return false;
		}
		return true;
	}

	bool VerifyMember(UBlueprint* Target, const FRequest& Request, const FPlan& Plan, FApplyState& State, bool bCompiled, FString& OutError)
	{
		if (!VerifyUpToDate(Target, bCompiled, OutError))
		{
			return false;
		}
		const FString OldString = Request.Old.ToString();
		const FString NewString = Request.New.ToString();

		if (Request.Kind == EKind::Local)
		{
			UEdGraph* Graph = FindFunctionGraph(Target, Request.Function);
			if (Graph == nullptr)
			{
				OutError = TEXT("The function graph disappeared");
				return false;
			}
			const FBPVariableDescription* Renamed = FBlueprintEditorUtils::FindLocalVariable(Target, Graph, Request.New);
			if (Renamed == nullptr || FBlueprintEditorUtils::FindLocalVariable(Target, Graph, Request.Old) != nullptr)
			{
				OutError = FString::Printf(TEXT("The local variable declaration is not '%s' only"), *NewString);
				return false;
			}
			if (State.bHaveVariableBefore
				&& (Renamed->VarGuid != State.VariableBefore.VarGuid || Renamed->DefaultValue != State.VariableBefore.DefaultValue
					|| !(Renamed->VarType == State.VariableBefore.VarType) || Renamed->PropertyFlags != State.VariableBefore.PropertyFlags))
			{
				OutError = TEXT("The local variable's guid, type, flags or default changed");
				return false;
			}
			for (const FGuid& Guid : State.LocalNodeGuids)
			{
				const UK2Node_Variable* Node = Cast<UK2Node_Variable>(FindNodeByGuid(Target, Guid));
				if (Node == nullptr || Node->GetVarName() != Request.New || !Node->VariableReference.IsLocalScope()
					|| Node->VariableReference.GetMemberScopeName() != Request.Function.ToString())
				{
					OutError = TEXT("A local variable node does not follow the rename");
					return false;
				}
			}
			for (const FGuid& Guid : State.MemberNodeGuids)
			{
				const UK2Node_Variable* Node = Cast<UK2Node_Variable>(FindNodeByGuid(Target, Guid));
				if (Node == nullptr || Node->GetVarName() != Request.Old || Node->VariableReference.IsLocalScope())
				{
					OutError = TEXT("A member variable node of the same name was changed");
					return false;
				}
			}
			TArray<UK2Node_Variable*> LocalNodes;
			TArray<FCortexMemberReference> MemberNodes;
			CollectLocalNodes(Graph, Request.Old, LocalNodes, MemberNodes);
			if (!LocalNodes.IsEmpty())
			{
				OutError = TEXT("Local variable nodes still name the old variable");
				return false;
			}
			return true;
		}

		TArray<FCortexMemberReference> After;
		TArray<FCortexMemberReference> Stale;
		FRequest NewRequest = Request;
		NewRequest.Old = Request.New;
		CollectReferences(Target, NewRequest, After, nullptr);
		CollectReferences(Target, Request, Stale, nullptr);
		State.ReferencesAfter = After;
		if (!FollowSet(Target, Stale, Request.Kind).IsEmpty())
		{
			OutError = FString::Printf(TEXT("Nodes still reference the old name '%s'"), *OldString);
			return false;
		}
		const TSet<FGuid> Following = FollowSet(Target, After, Request.Kind);
		if (!Following.Includes(Plan.FollowNodes))
		{
			OutError = FString::Printf(TEXT("%d reference node(s) did not follow the rename"), Plan.FollowNodes.Num() - Following.Intersect(Plan.FollowNodes).Num());
			return false;
		}

		if (Request.Kind == EKind::Function)
		{
			if (FindFunctionGraph(Target, Request.New) == nullptr || FindFunctionGraph(Target, Request.Old) != nullptr)
			{
				OutError = TEXT("The function graph is not named exactly the new name");
				return false;
			}
			if (const UK2Node_FunctionEntry* Entry = FindEntryNode(FindFunctionGraph(Target, Request.New));
				Entry != nullptr && Entry->FunctionReference.GetMemberName() != Request.New && Entry->CustomGeneratedFunctionName != Request.New)
			{
				OutError = TEXT("The function entry node does not follow the rename");
				return false;
			}
			if (bCompiled && (Target->GeneratedClass == nullptr || Target->GeneratedClass->FindFunctionByName(Request.New) == nullptr
				|| Target->GeneratedClass->FindFunctionByName(Request.Old) != nullptr))
			{
				OutError = TEXT("The compiled class does not carry the renamed function");
				return false;
			}
			return true;
		}

		const int32 NewIndex = FBlueprintEditorUtils::FindNewVariableIndex(Target, Request.New);
		if (NewIndex == INDEX_NONE || FBlueprintEditorUtils::FindNewVariableIndex(Target, Request.Old) != INDEX_NONE)
		{
			OutError = FString::Printf(TEXT("The variable declaration is not '%s' only"), *NewString);
			return false;
		}
		const FBPVariableDescription& Renamed = Target->NewVariables[NewIndex];
		if (State.bHaveVariableBefore
			&& (Renamed.VarGuid != State.VariableBefore.VarGuid || !(Renamed.VarType == State.VariableBefore.VarType)
				|| Renamed.PropertyFlags != State.VariableBefore.PropertyFlags || !Renamed.Category.EqualTo(State.VariableBefore.Category)))
		{
			OutError = TEXT("The variable's guid, type, flags or category changed");
			return false;
		}
		if (Request.Kind == EKind::Dispatcher)
		{
			if (FBlueprintEditorUtils::GetDelegateSignatureGraphByName(Target, Request.New) == nullptr
				|| FBlueprintEditorUtils::GetDelegateSignatureGraphByName(Target, Request.Old) != nullptr)
			{
				OutError = TEXT("The dispatcher signature graph is not named exactly the new name");
				return false;
			}
		}
		if (bCompiled)
		{
			if (Target->GeneratedClass == nullptr || Target->GeneratedClass->FindPropertyByName(Request.New) == nullptr
				|| Target->GeneratedClass->FindPropertyByName(Request.Old) != nullptr)
			{
				OutError = TEXT("The compiled class does not carry the renamed property");
				return false;
			}
			if (State.bHaveOldDefault && ReadCdoText(Target, Request.New) != State.OldDefaultText)
			{
				OutError = FString::Printf(TEXT("The default value changed ('%s' before, '%s' after)"), *State.OldDefaultText, *ReadCdoText(Target, Request.New));
				return false;
			}
		}
		return true;
	}

	void AddFailureContext(FCortexCommandResult& Result, const FRequest& Request, const FPlan& Plan, UBlueprint* Blueprint)
	{
		if (!Result.ErrorDetails.IsValid())
		{
			Result.ErrorDetails = MakeShared<FJsonObject>();
		}
		FString Readback;
		const bool bRenamed = Result.ErrorDetails->TryGetStringField(TEXT("readback_status"), Readback) && Readback == TEXT("matched");
		Result.ErrorDetails->SetBoolField(TEXT("renamed"), bRenamed);
		Plan.AppendTo(Blueprint, Request, Result.ErrorDetails.ToSharedRef());
	}

FCortexCommandResult ExecuteImpl(const TSharedPtr<FJsonObject>& Params)
{
	if (!Params.IsValid())
	{
		return FCortexCommandRouter::Error(CortexErrorCodes::InvalidField, TEXT("Missing params object"));
	}

	FCortexCommandResult ParamError;
	FString AssetPath;
	FString KindString;
	FString OldString;
	FString NewString;
	bool bDryRun = false;
	bool bCompile = true;
	bool bSave = false;
	if (!ReadRequiredString(Params, TEXT("asset_path"), AssetPath, ParamError)
		|| !ReadRequiredString(Params, TEXT("kind"), KindString, ParamError)
		|| !ReadRequiredString(Params, TEXT("old"), OldString, ParamError)
		|| !ReadRequiredString(Params, TEXT("new"), NewString, ParamError)
		|| !ReadOptionalBool(Params, TEXT("dry_run"), false, bDryRun, ParamError)
		|| !ReadOptionalBool(Params, TEXT("compile"), true, bCompile, ParamError)
		|| !ReadOptionalBool(Params, TEXT("save"), false, bSave, ParamError))
	{
		return ParamError;
	}

	FRequest Request;
	if (KindString == TEXT("variable")) { Request.Kind = EKind::Variable; }
	else if (KindString == TEXT("function")) { Request.Kind = EKind::Function; }
	else if (KindString == TEXT("dispatcher")) { Request.Kind = EKind::Dispatcher; }
	else if (KindString == TEXT("local")) { Request.Kind = EKind::Local; }
	else
	{
		return FCortexCommandRouter::Error(CortexErrorCodes::InvalidField,
			FString::Printf(TEXT("kind must be variable, function, dispatcher or local (got '%s')"), *KindString));
	}

	FString FunctionString;
	if (Params->HasField(TEXT("function")))
	{
		if (!ReadRequiredString(Params, TEXT("function"), FunctionString, ParamError))
		{
			return ParamError;
		}
		if (Request.Kind != EKind::Local)
		{
			return FCortexCommandRouter::Error(CortexErrorCodes::InvalidField, TEXT("function is only valid with kind:local"));
		}
	}
	else if (Request.Kind == EKind::Local)
	{
		return FCortexCommandRouter::Error(CortexErrorCodes::InvalidField, TEXT("kind:local requires the function param (the function graph that owns the variable)"));
	}

	TArray<FString> Acknowledged;
	if (Params->HasField(TEXT("acknowledged_losses")))
	{
		const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
		if (!Params->TryGetArrayField(TEXT("acknowledged_losses"), Values) || Values == nullptr)
		{
			return FCortexCommandRouter::Error(CortexErrorCodes::InvalidField, TEXT("acknowledged_losses must be an array of strings"));
		}
		for (const TSharedPtr<FJsonValue>& Value : *Values)
		{
			FString Key;
			if (!Value.IsValid() || Value->Type != EJson::String || !Value->TryGetString(Key))
			{
				return FCortexCommandRouter::Error(CortexErrorCodes::InvalidField, TEXT("acknowledged_losses must be an array of strings"));
			}
			Acknowledged.Add(Key);
		}
		Acknowledged = TSet<FString>(Acknowledged).Array();
		Acknowledged.Sort();
	}

	TSharedPtr<FJsonObject> Expected;
	if (Params->HasField(TEXT("expected_fingerprint")))
	{
		const TSharedPtr<FJsonObject>* ExpectedField = nullptr;
		if (!Params->TryGetObjectField(TEXT("expected_fingerprint"), ExpectedField) || ExpectedField == nullptr || !ExpectedField->IsValid())
		{
			return FCortexCommandRouter::Error(CortexErrorCodes::InvalidField, TEXT("expected_fingerprint must be an object"));
		}
		Expected = *ExpectedField;
	}

	if (bDryRun && bSave)
	{
		return FCortexCommandRouter::Error(CortexErrorCodes::InvalidOperation, TEXT("A dry run must use save=false"));
	}
	if (bSave && !bCompile)
	{
		return FCortexCommandRouter::Error(CortexErrorCodes::InvalidOperation, TEXT("save=true requires compile=true"));
	}
	if (OldString == NewString)
	{
		return FCortexCommandRouter::Error(CortexErrorCodes::InvalidField, TEXT("old and new are the same name"));
	}

	// The identifier, before anything is loaded.
	if (NewString.Len() >= NAME_SIZE || !FName::IsValidXName(NewString, UE_BLUEPRINT_INVALID_NAME_CHARACTERS))
	{
		return FCortexCommandRouter::Error(CortexErrorCodes::InvalidField,
			FString::Printf(TEXT("'%s' is not a valid Blueprint member name"), *NewString));
	}

	FString ValidationError;
	if (!FCortexBPAssetOps::ValidateWritableBlueprintAssetPath(AssetPath, ValidationError))
	{
		return FCortexCommandRouter::Error(CortexErrorCodes::InvalidField, ValidationError);
	}
	FString LoadError;
	UBlueprint* Blueprint = FCortexBPAssetOps::LoadBlueprint(AssetPath, LoadError);
	if (Blueprint == nullptr)
	{
		return FCortexCommandRouter::Error(CortexErrorCodes::BlueprintNotFound, LoadError);
	}

	Request.Old = FName(*OldString);
	Request.New = FName(*NewString);
	Request.Function = FName(*FunctionString);

	// Resolve the member; these refusals have no useful plan.
	switch (Request.Kind)
	{
	case EKind::Variable:
	case EKind::Dispatcher:
	{
		const int32 Index = FBlueprintEditorUtils::FindNewVariableIndex(Blueprint, Request.Old);
		if (Index == INDEX_NONE)
		{
			if (Blueprint->SimpleConstructionScript != nullptr)
			{
				for (const USCS_Node* Node : Blueprint->SimpleConstructionScript->GetAllNodes())
				{
					if (Node->GetVariableName() == Request.Old)
					{
						return FCortexCommandRouter::Error(CortexErrorCodes::InvalidOperation,
							FString::Printf(TEXT("'%s' is a component variable; use rename_scs_component"), *OldString));
					}
				}
			}
			return FCortexCommandRouter::Error(CortexErrorCodes::VariableNotFound,
				FString::Printf(TEXT("Variable '%s' not found in %s"), *OldString, *AssetPath));
		}
		const bool bIsDelegate = Blueprint->NewVariables[Index].VarType.PinCategory == UEdGraphSchema_K2::PC_MCDelegate;
		if (Request.Kind == EKind::Variable && bIsDelegate)
		{
			return FCortexCommandRouter::Error(CortexErrorCodes::InvalidField,
				FString::Printf(TEXT("'%s' is an event dispatcher; use kind:dispatcher"), *OldString));
		}
		if (Request.Kind == EKind::Dispatcher && !bIsDelegate)
		{
			return FCortexCommandRouter::Error(CortexErrorCodes::InvalidField,
				FString::Printf(TEXT("'%s' is not an event dispatcher; use kind:variable"), *OldString));
		}
		break;
	}
	case EKind::Function:
	{
		if (Blueprint->BlueprintType == BPTYPE_Interface)
		{
			return FCortexCommandRouter::Error(CortexErrorCodes::InvalidOperation,
				TEXT("Interface Blueprint functions cannot be renamed here: every implementing Blueprint would lose its graph"));
		}
		if (FindFunctionGraph(Blueprint, Request.Old) == nullptr)
		{
			for (const FBPInterfaceDescription& Description : Blueprint->ImplementedInterfaces)
			{
				for (const UEdGraph* Graph : Description.Graphs)
				{
					if (Graph != nullptr && Graph->GetFName() == Request.Old)
					{
						return FCortexCommandRouter::Error(CortexErrorCodes::InvalidOperation,
							FString::Printf(TEXT("'%s' implements an interface function and cannot be renamed"), *OldString));
					}
				}
			}
			if (FBlueprintEditorUtils::GetDelegateSignatureGraphByName(Blueprint, Request.Old) != nullptr)
			{
				return FCortexCommandRouter::Error(CortexErrorCodes::InvalidField,
					FString::Printf(TEXT("'%s' is an event dispatcher signature; use kind:dispatcher"), *OldString));
			}
			return FCortexCommandRouter::Error(CortexErrorCodes::GraphNotFound,
				FString::Printf(TEXT("Function graph '%s' not found in %s"), *OldString, *AssetPath));
		}
		if (Blueprint->ParentClass != nullptr && Blueprint->ParentClass->FindFunctionByName(Request.Old) != nullptr)
		{
			return FCortexCommandRouter::Error(CortexErrorCodes::InvalidOperation,
				FString::Printf(TEXT("'%s' overrides a function of the parent class and cannot be renamed"), *OldString));
		}
		break;
	}
	case EKind::Local:
	{
		UEdGraph* FunctionGraph = FindFunctionGraph(Blueprint, Request.Function);
		if (FunctionGraph == nullptr)
		{
			return FCortexCommandRouter::Error(CortexErrorCodes::GraphNotFound,
				FString::Printf(TEXT("Function graph '%s' not found in %s"), *FunctionString, *AssetPath));
		}
		if (FBlueprintEditorUtils::FindLocalVariable(Blueprint, FunctionGraph, Request.Old) == nullptr)
		{
			return FCortexCommandRouter::Error(CortexErrorCodes::VariableNotFound,
				FString::Printf(TEXT("Function '%s' has no local variable '%s'"), *FunctionString, *OldString));
		}
		if (Blueprint->SkeletonGeneratedClass == nullptr || Blueprint->SkeletonGeneratedClass->FindFunctionByName(Request.Function) == nullptr)
		{
			return FCortexCommandRouter::Error(CortexErrorCodes::StalePrecondition,
				FString::Printf(TEXT("The compiled skeleton has no function '%s'; compile the Blueprint first"), *FunctionString));
		}
		break;
	}
	}

	if (Expected.IsValid())
	{
		FCortexCommandResult ExpectedError;
		if (!FCortexBPMigrationGuard::CheckExpected(Blueprint, Expected, ExpectedError))
		{
			return ExpectedError;
		}
	}

	const TSharedRef<FPlan> Plan = MakeShared<FPlan>();
	BuildPlan(Blueprint, Request, *Plan);

	const auto DescribeInto = [&](const TSharedRef<FJsonObject>& Data)
	{
		Data->SetStringField(TEXT("asset_path"), AssetPath);
		Data->SetStringField(TEXT("kind"), KindName(Request.Kind));
		Data->SetStringField(TEXT("old"), OldString);
		Data->SetStringField(TEXT("new"), NewString);
		if (Request.Kind == EKind::Local)
		{
			Data->SetStringField(TEXT("function"), FunctionString);
		}
	};

	if (bDryRun)
	{
		FCortexBPGuardedRun::FSpec Spec;
		Spec.OpName = TEXT("rename_member");
		Spec.Blueprint = Blueprint;
		Spec.bDryRun = true;
		Spec.bCompile = bCompile;
		Spec.Expected = Expected;
		FCortexCommandResult Result = FCortexBPGuardedRun::Execute(Spec);
		if (Result.bSuccess && Result.Data.IsValid())
		{
			DescribeInto(Result.Data.ToSharedRef());
			Plan->AppendTo(Blueprint, Request, Result.Data.ToSharedRef());
			Result.Data->SetBoolField(TEXT("renamed"), false);
			Result.Data->SetBoolField(TEXT("can_apply"), Plan->Blockers.IsEmpty());
		}
		return Result;
	}

	if (!Plan->Blockers.IsEmpty())
	{
		const FBlocker& First = Plan->Blockers[0];
		TSharedPtr<FJsonObject> Details = MakeShared<FJsonObject>();
		Plan->AppendTo(Blueprint, Request, Details.ToSharedRef());
		Details->SetBoolField(TEXT("renamed"), false);
		return FCortexCommandRouter::Error(First.Code,
			FString::Printf(TEXT("Cannot rename %s '%s' to '%s' in %s: %s"), KindName(Request.Kind), *OldString, *NewString, *AssetPath, *First.Message),
			Details);
	}

	{
		FCortexCommandResult AckError;
		if (!FCortexBPSaveGameGuard::Check(Plan->SaveGameFindings, Acknowledged, AckError))
		{
			if (!AckError.ErrorDetails.IsValid())
			{
				AckError.ErrorDetails = MakeShared<FJsonObject>();
			}
			AckError.ErrorDetails->SetBoolField(TEXT("renamed"), false);
			return AckError;
		}
	}

	const TSharedRef<FApplyState> State = MakeShared<FApplyState>();

	FCortexBPGuardedRun::FSpec Spec;
	Spec.OpName = TEXT("rename_member");
	Spec.Blueprint = Blueprint;
	Spec.bCompile = bCompile;
	Spec.bSave = bSave;
	Spec.Expected = Expected;
	Spec.DigestExtra = [](UBlueprint* Target) { return OperationStateDigest(Target); };

	Spec.Capture = [State, Request](UBlueprint* Target, FString& OutError)
	{
		if (Request.Kind == EKind::Local)
		{
			UEdGraph* Graph = FindFunctionGraph(Target, Request.Function);
			UK2Node_FunctionEntry* Entry = Graph != nullptr ? FindEntryNode(Graph) : nullptr;
			const FBPVariableDescription* Local = Graph != nullptr ? FBlueprintEditorUtils::FindLocalVariable(Target, Graph, Request.Old) : nullptr;
			if (Entry == nullptr || Local == nullptr)
			{
				OutError = TEXT("The local variable declaration vanished");
				return false;
			}
			State->LocalsBefore = Entry->LocalVariables;
			State->VariableBefore = *Local;
			State->bHaveVariableBefore = true;

			TArray<UK2Node_Variable*> LocalNodes;
			TArray<FCortexMemberReference> MemberNodes;
			CollectLocalNodes(Graph, Request.Old, LocalNodes, MemberNodes);
			for (const UK2Node_Variable* Node : LocalNodes)
			{
				State->LocalNodeGuids.Add(Node->NodeGuid);
			}
			for (const FCortexMemberReference& Node : MemberNodes)
			{
				State->MemberNodeGuids.Add(Node.NodeGuid);
			}
			return true;
		}
		if (Request.Kind != EKind::Function)
		{
			const int32 Index = FBlueprintEditorUtils::FindNewVariableIndex(Target, Request.Old);
			if (Index == INDEX_NONE)
			{
				OutError = TEXT("The variable declaration vanished");
				return false;
			}
			State->VariableBefore = Target->NewVariables[Index];
			State->bHaveVariableBefore = true;
			State->OldDefaultText = ReadCdoText(Target, Request.Old);
			State->bHaveOldDefault = Target->Status == BS_UpToDate || Target->Status == BS_UpToDateWithWarnings ? !State->OldDefaultText.IsEmpty() : false;
		}
		return true;
	};

	Spec.Apply = [Request](UBlueprint* Target, FString& OutError)
	{
		Target->Modify();
		switch (Request.Kind)
		{
		case EKind::Variable:
		case EKind::Dispatcher:
			if (Request.Kind == EKind::Dispatcher)
			{
				RetargetDispatcherNodes(Target, Request.Old, Request.New);
			}
			FBlueprintEditorUtils::RenameMemberVariable(Target, Request.Old, Request.New);
			break;
		case EKind::Function:
		{
			UEdGraph* Graph = FindFunctionGraph(Target, Request.Old);
			if (Graph == nullptr)
			{
				OutError = TEXT("The function graph vanished before the rename");
				return false;
			}
			FBlueprintEditorUtils::RenameGraph(Graph, Request.New.ToString());
			break;
		}
		case EKind::Local:
		{
			UEdGraph* Graph = FindFunctionGraph(Target, Request.Function);
			const UFunction* Scope = Target->SkeletonGeneratedClass != nullptr ? Target->SkeletonGeneratedClass->FindFunctionByName(Request.Function) : nullptr;
			if (Graph == nullptr || Scope == nullptr)
			{
				OutError = TEXT("The function scope vanished before the rename");
				return false;
			}
			FBlueprintEditorUtils::RenameLocalVariable(Target, Scope, Request.Old, Request.New);

			// The engine retargets the nodes of the function graph itself; nodes in its subgraphs (collapsed graphs) keep the old name.
			TArray<UK2Node_Variable*> LocalNodes;
			TArray<FCortexMemberReference> MemberNodes;
			CollectLocalNodes(Graph, Request.Old, LocalNodes, MemberNodes);
			for (UK2Node_Variable* Node : LocalNodes)
			{
				Node->HandleVariableRenamed(Target, Target->GeneratedClass, Cast<UEdGraph>(Node->GetOuter()), Request.Old, Request.New);
			}
			break;
		}
		}

#if WITH_DEV_AUTOMATION_TESTS
		if (GPostApplyHookForTest)
		{
			GPostApplyHookForTest(Target);
		}
#endif
		return true;
	};

	Spec.Verify = [State, Plan, Request](UBlueprint* Target, bool bCompiled, FString& OutError)
	{
		State->bCompiled = bCompiled;
		return VerifyMember(Target, Request, *Plan, *State, bCompiled, OutError);
	};

	if (Request.Kind == EKind::Local)
	{
		// Local variable declarations live on the function entry node, outside the graph authoring hash and the node journal.
		Spec.Restore = [State, Request](UBlueprint* Target, FString& OutError)
		{
			UEdGraph* Graph = FindFunctionGraph(Target, Request.Function);
			UK2Node_FunctionEntry* Entry = Graph != nullptr ? FindEntryNode(Graph) : nullptr;
			if (Entry == nullptr)
			{
				OutError = TEXT("The function entry node is missing");
				return false;
			}
			Entry->Modify();
			Entry->LocalVariables = State->LocalsBefore;
			return true;
		};
	}

	FCortexCommandResult Result = FCortexBPGuardedRun::Execute(Spec);
	if (!Result.bSuccess)
	{
		AddFailureContext(Result, Request, *Plan, Blueprint);
		return Result;
	}

	const TSharedRef<FJsonObject> Data = Result.Data.ToSharedRef();
	DescribeInto(Data);
	Plan->AppendTo(Blueprint, Request, Data);
	Data->SetBoolField(TEXT("renamed"), true);
	Data->SetBoolField(TEXT("compiled"), bCompile);
	{
		// Before and after must describe the same nodes under the two names.
		TSharedPtr<FJsonObject> References = Data->GetObjectField(TEXT("references"));
		References->SetObjectField(TEXT("after"), FCortexGraphMemberReferences::ToJson(Blueprint, Request.New, State->ReferencesAfter).ToSharedRef());
		References->SetNumberField(TEXT("before_count"), Plan->References.Num());
		References->SetNumberField(TEXT("after_count"), State->ReferencesAfter.Num());
	}

	UE_LOG(LogCortexBlueprint, Log, TEXT("Renamed %s '%s' -> '%s' in %s"), KindName(Request.Kind), *OldString, *NewString, *AssetPath);
	return Result;
}
}

FCortexCommandResult FCortexBPRenameMemberOps::Execute(const TSharedPtr<FJsonObject>& Params)
{
	return CortexBPRenameMemberPrivate::ExecuteImpl(Params);
}

#if WITH_DEV_AUTOMATION_TESTS
void FCortexBPRenameMemberOps::SetPostApplyHookForTest(TFunction<void(UBlueprint*)> Hook)
{
	CortexBPRenameMemberPrivate::GPostApplyHookForTest = MoveTemp(Hook);
}

void FCortexBPRenameMemberOps::ClearPostApplyHookForTest()
{
	CortexBPRenameMemberPrivate::GPostApplyHookForTest = nullptr;
}
#endif
