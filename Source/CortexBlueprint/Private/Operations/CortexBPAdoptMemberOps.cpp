#include "Operations/CortexBPAdoptMemberOps.h"

#include "CortexBlueprintModule.h"
#include "CortexCommandRouter.h"
#include "CortexGraphBindingStorage.h"
#include "CortexGraphMemberReferences.h"
#include "CortexTypes.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "EdGraph/EdGraph.h"
#include "EdGraphSchema_K2.h"
#include "Engine/Blueprint.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "K2Node.h"
#include "K2Node_ActorBoundEvent.h"
#include "K2Node_ComponentBoundEvent.h"
#include "K2Node_Variable.h"
#include "K2Node_VariableSet.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Algo/AnyOf.h"
#include "Misc/SecureHash.h"
#include "Operations/CortexBPAssetOps.h"
#include "Operations/CortexBPCommandParams.h"
#include "Operations/CortexBPGuardedRun.h"
#include "Operations/CortexBPMigrationGuard.h"
#include "Operations/CortexBPSCSSupport.h"
#include "Operations/CortexBPSnapshotOps.h"
#include "UObject/CoreRedirects.h"
#include "UObject/Package.h"
#include "UObject/UObjectHash.h"
#include "UObject/UnrealType.h"

#if WITH_DEV_AUTOMATION_TESTS
namespace CortexBPAdoptMemberPrivate
{
	TFunction<void(UBlueprint*)> GPostApplyHookForTest;
	TFunction<void(UBlueprint*)> GPostWriteHookForTest;
}
#endif

namespace
{
	using FOps = FCortexBPAdoptMemberOps;
	using FValueSnap = FCortexBPAdoptMemberOps::FValueSnap;

	using CortexBPParams::ReadOptionalBool;
	using CortexBPParams::ReadRequiredString;
	using CortexBPParams::ToJsonStrings;

	bool IsFloatingPoint(const FProperty* Property)
	{
		return Property != nullptr && (Property->IsA<FFloatProperty>() || Property->IsA<FDoubleProperty>());
	}

	//////////////////////////////////////////////////////////////////////////
	// Values

	FValueSnap CaptureValue(const FProperty* Property, const UObject* Container)
	{
		FValueSnap Snap;
		if (Property == nullptr || Container == nullptr)
		{
			return Snap;
		}
		if (IsFloatingPoint(Property))
		{
			const FNumericProperty* Numeric = CastField<FNumericProperty>(Property);
			Snap.bNumeric = true;
			Snap.Number = Numeric->GetFloatingPointPropertyValue(Property->ContainerPtrToValuePtr<void>(Container));
			Snap.Text = FString::Printf(TEXT("%.17g"), Snap.Number);
			Snap.bValid = true;
			return Snap;
		}
		Snap.bValid = FBlueprintEditorUtils::PropertyValueToString(Property, reinterpret_cast<const uint8*>(Container), Snap.Text);
		return Snap;
	}

	bool WriteValue(const FProperty* Property, UObject* Container, const FValueSnap& Snap)
	{
		if (Property == nullptr || Container == nullptr || !Snap.bValid)
		{
			return false;
		}
		if (Snap.bNumeric && IsFloatingPoint(Property))
		{
			CastField<FNumericProperty>(Property)->SetFloatingPointPropertyValue(Property->ContainerPtrToValuePtr<void>(Container), Snap.Number);
			return true;
		}
		return FBlueprintEditorUtils::PropertyValueFromString(Property, Snap.Text, reinterpret_cast<uint8*>(Container), Container);
	}

	bool ValueEquals(const FProperty* Property, const UObject* Container, const FValueSnap& Snap)
	{
		const FValueSnap Now = CaptureValue(Property, Container);
		if (!Now.bValid || !Snap.bValid)
		{
			return false;
		}
		if (Snap.bNumeric && Now.bNumeric)
		{
			return Property->IsA<FFloatProperty>() ? static_cast<float>(Snap.Number) == static_cast<float>(Now.Number) : Snap.Number == Now.Number;
		}
		return Snap.Text == Now.Text;
	}

	//////////////////////////////////////////////////////////////////////////
	// Lookup

	using CortexBPSCSSupport::AllNodeGuids;
	using CortexBPSCSSupport::FindNodeByGuid;

	/** True when RenameVariableReferences retargets the node together with the variable. */
	bool NodeFollows(const UBlueprint* Blueprint, const UEdGraphNode* Node)
	{
		// A bound event keeps its delegate binding; it is not a member reference the readback can resolve to a property.
		if (Node->IsA<UK2Node_ComponentBoundEvent>() || Node->IsA<UK2Node_ActorBoundEvent>())
		{
			return false;
		}
		if (const UK2Node_Variable* Variable = Cast<UK2Node_Variable>(Node))
		{
			if (Variable->VariableReference.IsLocalScope())
			{
				return false;
			}
			if (Variable->VariableReference.IsSelfContext())
			{
				return true;
			}
			const UClass* Referenced = Variable->VariableReference.GetMemberParentClass(Blueprint->GeneratedClass);
			return Referenced != nullptr && Blueprint->GeneratedClass != nullptr && Referenced->IsChildOf(Blueprint->GeneratedClass);
		}
		return true;
	}

	/** The property the generated class (the last compile) declares for a Blueprint variable. */
	const FProperty* FindCompiledProperty(const UBlueprint* Blueprint, const FName Name)
	{
		const UClass* Generated = Blueprint->GeneratedClass;
		const UObject* Cdo = Generated != nullptr ? Generated->GetDefaultObject(false) : nullptr;
		return Cdo != nullptr ? FindFProperty<FProperty>(Cdo->GetClass(), Name) : nullptr;
	}

	UObject* GeneratedCdo(const UBlueprint* Blueprint)
	{
		return Blueprint->GeneratedClass != nullptr ? Blueprint->GeneratedClass->GetDefaultObject(false) : nullptr;
	}

	bool IsNativeProperty(const FProperty* Property)
	{
		const UClass* Owner = Property != nullptr ? Property->GetOwnerClass() : nullptr;
		return Owner != nullptr && Owner->HasAnyClassFlags(CLASS_Native);
	}

	/** An engine-renamed leftover of the variable (`<name>_<n>`), left behind when the compiler could not adopt it. */
	FString FindEngineRenamedVariable(const UBlueprint* Blueprint, const FString& Name)
	{
		const FString Prefix = Name + TEXT("_");
		for (const FBPVariableDescription& Variable : Blueprint->NewVariables)
		{
			const FString Other = Variable.VarName.ToString();
			if (Other.StartsWith(Prefix) && Other.Mid(Prefix.Len()).IsNumeric())
			{
				return Other;
			}
		}
		return FString();
	}

	FString DescribeBlueprintType(const FBPVariableDescription& Variable)
	{
		FString Text = Variable.VarType.PinCategory.ToString();
		if (!Variable.VarType.PinSubCategory.IsNone())
		{
			Text += TEXT("/") + Variable.VarType.PinSubCategory.ToString();
		}
		if (Variable.VarType.PinSubCategoryObject.IsValid())
		{
			Text += TEXT(" ") + Variable.VarType.PinSubCategoryObject->GetName();
		}
		return Text;
	}

	/** exact | numeric_convertible; empty (with Why) when the types are not interchangeable. */
	FString ClassifyType(const FBPVariableDescription& Variable, const FProperty* Native, FString& OutWhy)
	{
		if (FCortexBPSaveGameGuard::SameType(Variable, Native))
		{
			return TEXT("exact");
		}
		FEdGraphPinType NativePin;
		if (GetDefault<UEdGraphSchema_K2>()->ConvertPropertyToPinType(Native, NativePin)
			&& Variable.VarType.PinCategory == UEdGraphSchema_K2::PC_Real && NativePin.PinCategory == UEdGraphSchema_K2::PC_Real
			&& !Variable.VarType.IsContainer() && !NativePin.IsContainer())
		{
			return TEXT("numeric_convertible");
		}
		OutWhy = FString::Printf(TEXT("Blueprint type %s does not match native %s"), *DescribeBlueprintType(Variable), *Native->GetCPPType());
		return FString();
	}

	bool RedirectPresent(const UClass* Parent, const FName OldName, const FName NewName)
	{
		for (const UStruct* Struct = Parent; Struct != nullptr; Struct = Struct->GetSuperStruct())
		{
			if (FProperty::FindRedirectedPropertyName(Struct, OldName) == NewName)
			{
				return true;
			}
		}
		return false;
	}

	FString BuildRedirectLine(const FProperty* Native, const FString& OldName)
	{
		const FString ClassPath = Native->GetOwnerClass()->GetPathName();
		return FString::Printf(TEXT("+PropertyRedirects=(OldName=\"%s.%s\",NewName=\"%s.%s\")"),
			*ClassPath, *OldName, *ClassPath, *Native->GetName());
	}

	/** Loaded objects that carry a value of the member: placed instances and child Blueprint CDOs. */
	void CollectLoadedValues(const UBlueprint* Blueprint, const FName MemberName, const FName NativeName, TArray<FOps::FLoadedValue>& OutValues)
	{
		const UClass* Generated = Blueprint->GeneratedClass;
		UObject* Cdo = GeneratedCdo(Blueprint);
		if (Generated == nullptr || Cdo == nullptr)
		{
			return;
		}

		TArray<UObject*> Objects;
		GetObjectsOfClass(Generated, Objects, /*bIncludeDerivedClasses=*/true, RF_NoFlags);
		for (UObject* Object : Objects)
		{
			if (Object == Cdo || !IsValid(Object))
			{
				continue;
			}
			const UClass* ObjectClass = Object->GetClass();
			const bool bIsCdo = Object->HasAnyFlags(RF_ClassDefaultObject);
			if (ObjectClass->HasAnyClassFlags(CLASS_NewerVersionExists) || ObjectClass->GetName().StartsWith(TEXT("REINST_"))
				|| (!bIsCdo && Object->HasAnyFlags(RF_ArchetypeObject))
				|| Object->GetOutermost()->HasAnyPackageFlags(PKG_PlayInEditor))
			{
				continue;
			}
			const FProperty* Property = FindFProperty<FProperty>(ObjectClass, MemberName);
			if (Property == nullptr)
			{
				continue;
			}
			FOps::FLoadedValue& Entry = OutValues.AddDefaulted_GetRef();
			Entry.Path = Object->GetPathName();
			Entry.Kind = bIsCdo ? TEXT("child_cdo") : TEXT("instance");
			Entry.Value = CaptureValue(Property, Object);
			if (NativeName != MemberName)
			{
				Entry.NativeBefore = CaptureValue(FindFProperty<FProperty>(ObjectClass, NativeName), Object);
			}
			Entry.bIsOverride = !Property->Identical_InContainer(Object, Cdo);
		}
		OutValues.Sort([](const FOps::FLoadedValue& A, const FOps::FLoadedValue& B) { return A.Path < B.Path; });
	}

	//////////////////////////////////////////////////////////////////////////
	// AnimGraph bindings (reached by reflection; CortexBlueprint does not depend on the AnimGraph module)

	/**
	 * Fails closed when an animation node's binding storage cannot be read: a restore the adoption could not verify would silently drop
	 * bindings. The reflected fields belong to FCortexGraphBindingStorage, which names the missing one.
	 */
	bool CheckAnimBindingStorage(UBlueprint* Blueprint, FString& OutError)
	{
		TArray<UEdGraph*> Graphs;
		Blueprint->GetAllGraphs(Graphs);
		for (const UEdGraph* Graph : Graphs)
		{
			for (UEdGraphNode* Node : Graph->Nodes)
			{
				FString Missing;
				if (FCortexGraphBindingStorage::IsAnimNode(Node) && !FCortexGraphBindingStorage::CheckBindingStorage(Missing))
				{
					OutError = FString::Printf(TEXT("This engine build does not expose the reflected field '%s' needed to journal anim-node bindings"), *Missing);
					return false;
				}
			}
		}
		return true;
	}

	/** Exported property bindings of every node whose binding storage the shared owner can read. */
	TMap<FGuid, FString> CollectBindingTexts(UBlueprint* Blueprint)
	{
		TMap<FGuid, FString> Out;
		TArray<UEdGraph*> Graphs;
		Blueprint->GetAllGraphs(Graphs);
		for (const UEdGraph* Graph : Graphs)
		{
			for (UEdGraphNode* Node : Graph->Nodes)
			{
				FString Text;
				if (FCortexGraphBindingStorage::ExportBindingText(Cast<UK2Node>(Node), Text))
				{
					Out.Add(Node->NodeGuid, Text);
				}
			}
		}
		return Out;
	}

	bool RestoreBindingTexts(UBlueprint* Blueprint, const TMap<FGuid, FString>& Before, FString& OutError)
	{
		const TMap<FGuid, FString> Now = CollectBindingTexts(Blueprint);
		for (const TPair<FGuid, FString>& Pair : Before)
		{
			const FString* Current = Now.Find(Pair.Key);
			if (Current != nullptr && *Current == Pair.Value)
			{
				continue;
			}
			UK2Node* Node = Cast<UK2Node>(FindNodeByGuid(Blueprint, Pair.Key));
			FString Unused;
			if (!FCortexGraphBindingStorage::ExportBindingText(Node, Unused))
			{
				OutError = FString::Printf(TEXT("Node %s lost its property bindings"), *Pair.Key.ToString());
				return false;
			}
			if (!FCortexGraphBindingStorage::ImportBindingText(Node, Pair.Value))
			{
				OutError = FString::Printf(TEXT("Could not restore the property bindings of node %s"), *Pair.Key.ToString());
				return false;
			}
		}
		return true;
	}

	//////////////////////////////////////////////////////////////////////////
	// Plan

	void AddRefusal(FOps::FAdoptPlan& Plan, const FString& Code, const FString& Message, const FString& Variable, const TSharedPtr<FJsonObject>& Extra = nullptr)
	{
		FOps::FRefusal& Refusal = Plan.Refusals.AddDefaulted_GetRef();
		Refusal.Code = Code;
		Refusal.Message = Message;
		Refusal.Details = Extra.IsValid() ? Extra : MakeShared<FJsonObject>();
		Refusal.Details->SetStringField(TEXT("bp_variable"), Variable);
	}

	/** State the capture, apply, verify and restore callbacks share. */
	struct FAdoptMemberApplyState
	{
		TSet<FGuid> NodeGuidsBefore;
		TMap<FGuid, FString> BindingsBefore;
		TSharedPtr<FJsonObject> SnapshotBefore;
		TArray<TSharedPtr<FJsonValue>> Differences;
		TSharedPtr<FJsonObject> Counts;
		/** Expected native value per mapping index. */
		TArray<FValueSnap> Expected;
		bool bCompiled = false;
		/** The super class the last compile was built against; differs from the parent in state A (parent assigned, never compiled). */
		UClass* OldCompiledSuper = nullptr;
		UClass* NativeParent = nullptr;
		bool bParentReverted = false;
	};

	FString OperationDigest(UBlueprint* Blueprint, const FOps::FAdoptPlan& Plan)
	{
		FString Buffer;
		TMap<FGuid, FString> Bindings = CollectBindingTexts(Blueprint);
		Bindings.KeySort([](const FGuid& A, const FGuid& B) { return A < B; });
		for (const TPair<FGuid, FString>& Pair : Bindings)
		{
			Buffer += FString::Printf(TEXT("Binding %s %s\n"), *Pair.Key.ToString(), *Pair.Value);
		}
		for (const FOps::FMappingPlan& Mapping : Plan.Mappings)
		{
			const FName Name(*Mapping.BpVariable);
			if (const UObject* Cdo = GeneratedCdo(Blueprint))
			{
				const FValueSnap Value = CaptureValue(FindFProperty<FProperty>(Cdo->GetClass(), Name), Cdo);
				Buffer += FString::Printf(TEXT("Cdo %s %d %s\n"), *Mapping.BpVariable, Value.bValid ? 1 : 0, *Value.Text);
			}
			if (const UObject* NativeCdo = GeneratedCdo(Blueprint); NativeCdo != nullptr && Mapping.NativeCdoBefore.bValid)
			{
				const FValueSnap Native = CaptureValue(FindFProperty<FProperty>(NativeCdo->GetClass(), FName(*Mapping.NativeProperty)), NativeCdo);
				Buffer += FString::Printf(TEXT("NativeCdo %s %d %s\n"), *Mapping.NativeProperty, Native.bValid ? 1 : 0, *Native.Text);
			}
			for (const FOps::FLoadedValue& Loaded : Mapping.LoadedValues)
			{
				const UObject* Object = FindObject<UObject>(nullptr, *Loaded.Path);
				const FValueSnap Value = Object != nullptr ? CaptureValue(FindFProperty<FProperty>(Object->GetClass(), Name), Object) : FValueSnap();
				Buffer += FString::Printf(TEXT("Loaded %s %s %d %s\n"), *Loaded.Path, *Mapping.BpVariable, Value.bValid ? 1 : 0, *Value.Text);
				if (Loaded.NativeBefore.bValid && Object != nullptr)
				{
					const FValueSnap Native = CaptureValue(FindFProperty<FProperty>(Object->GetClass(), FName(*Mapping.NativeProperty)), Object);
					Buffer += FString::Printf(TEXT("NativeLoaded %s %s %d %s\n"), *Loaded.Path, *Mapping.NativeProperty, Native.bValid ? 1 : 0, *Native.Text);
				}
			}
		}
		return FMD5::HashAnsiString(*Buffer);
	}
}

TSharedRef<FJsonObject> FCortexBPAdoptMemberOps::FMappingPlan::ToJson() const
{
	TSharedRef<FJsonObject> Json = MakeShared<FJsonObject>();
	Json->SetStringField(TEXT("bp_variable"), BpVariable);
	Json->SetStringField(TEXT("native_property"), NativeProperty);
	Json->SetStringField(TEXT("state"), State);
	Json->SetStringField(TEXT("type_match"), TypeMatch);
	Json->SetStringField(TEXT("old_default"), OldDefaultText);
	Json->SetStringField(TEXT("old_default_source"), OldDefaultSource);
	Json->SetBoolField(TEXT("default_verified"), !bDefaultUnverified);

	TArray<TSharedPtr<FJsonValue>> NodeValues;
	for (const FCortexMemberReference& Node : Nodes)
	{
		const TSharedRef<FJsonObject> NodeJson = FCortexGraphMemberReferences::ReferenceToJson(Node);
		NodeJson->SetBoolField(TEXT("follows_member"), FollowNodes.Contains(Node.NodeGuid));
		if (const bool* Retargeted = RetargetedNodes.Find(Node.NodeGuid))
		{
			NodeJson->SetBoolField(TEXT("retargeted"), *Retargeted);
		}
		if (const bool* Resolves = ResolvedNodes.Find(Node.NodeGuid))
		{
			NodeJson->SetBoolField(TEXT("resolves_to_native"), *Resolves);
		}
		NodeValues.Add(MakeShared<FJsonValueObject>(NodeJson));
	}
	Json->SetArrayField(TEXT("referencing_nodes"), NodeValues);

	TArray<TSharedPtr<FJsonValue>> FindingValues;
	for (const FCortexBPSaveGameGuard::FFinding& Finding : SaveGameFindings)
	{
		FindingValues.Add(MakeShared<FJsonValueObject>(Finding.ToJson()));
	}
	Json->SetArrayField(TEXT("save_game_findings"), FindingValues);

	TArray<TSharedPtr<FJsonValue>> LoadedJson;
	for (const FLoadedValue& Loaded : LoadedValues)
	{
		TSharedRef<FJsonObject> Item = MakeShared<FJsonObject>();
		Item->SetStringField(TEXT("path"), Loaded.Path);
		Item->SetStringField(TEXT("kind"), Loaded.Kind);
		Item->SetStringField(TEXT("value"), Loaded.Value.Text);
		Item->SetBoolField(TEXT("is_override"), Loaded.bIsOverride);
		LoadedJson.Add(MakeShared<FJsonValueObject>(Item));
	}
	Json->SetArrayField(TEXT("loaded_instances"), LoadedJson);

	TArray<TSharedPtr<FJsonValue>> DependentJson;
	for (const FCortexBPDependentHit& Hit : Dependents)
	{
		DependentJson.Add(MakeShared<FJsonValueObject>(Hit.ToJson()));
	}
	Json->SetArrayField(TEXT("dependents"), DependentJson);
	Json->SetArrayField(TEXT("warnings"), ToJsonStrings(Warnings));

	Json->SetBoolField(TEXT("rename"), bRename);
	Json->SetBoolField(TEXT("redirect_required"), bRedirectRequired);
	Json->SetBoolField(TEXT("redirect_present"), bRedirectPresent);
	if (!RedirectLine.IsEmpty())
	{
		Json->SetStringField(TEXT("redirect_line"), RedirectLine);
	}
	Json->SetBoolField(TEXT("instance_values_acknowledged"), bInstanceValuesAcknowledged);
	Json->SetBoolField(TEXT("unloaded_instance_values_lost"), bInstanceValuesAcknowledged && !bRedirectPresent);
	return Json;
}

void FCortexBPAdoptMemberOps::FAdoptPlan::AppendTo(const TSharedRef<FJsonObject>& Target) const
{
	TArray<TSharedPtr<FJsonValue>> MappingValues;
	for (const FMappingPlan& Mapping : Mappings)
	{
		MappingValues.Add(MakeShared<FJsonValueObject>(Mapping.ToJson()));
	}
	Target->SetArrayField(TEXT("mappings"), MappingValues);
	Target->SetArrayField(TEXT("required_acknowledgment"), ToJsonStrings(RequiredAcknowledgment));
	Target->SetBoolField(TEXT("transfer_default"), bTransferDefault);
	Target->SetBoolField(TEXT("retarget_nodes"), bRetargetNodes);

	TArray<TSharedPtr<FJsonValue>> RefusalValues;
	for (const FRefusal& Refusal : Refusals)
	{
		TSharedRef<FJsonObject> Item = MakeShared<FJsonObject>();
		Item->SetStringField(TEXT("code"), Refusal.Code);
		Item->SetStringField(TEXT("message"), Refusal.Message);
		if (Refusal.Details.IsValid())
		{
			Item->SetObjectField(TEXT("details"), Refusal.Details);
		}
		RefusalValues.Add(MakeShared<FJsonValueObject>(Item));
	}
	Target->SetArrayField(TEXT("refusals"), RefusalValues);
}

FCortexBPAdoptMemberOps::FAdoptPlan FCortexBPAdoptMemberOps::BuildPlan(
	UBlueprint* Blueprint,
	const TArray<FMappingRequest>& Mappings,
	const bool bTransferDefault,
	const bool bRetargetNodes,
	const TArray<FString>& Acknowledged)
{
	FAdoptPlan Plan;
	Plan.bTransferDefault = bTransferDefault;
	Plan.bRetargetNodes = bRetargetNodes;

	UClass* Parent = Blueprint->ParentClass;
	if (Parent == nullptr)
	{
		AddRefusal(Plan, CortexErrorCodes::InvalidOperation, TEXT("The Blueprint has no parent class"), FString());
		return Plan;
	}

	TSet<FString> SeenVariables;
	TSet<FString> SeenNative;
	TArray<FCortexBPSaveGameGuard::FFinding> AllFindings;

	for (const FMappingRequest& Request : Mappings)
	{
		FMappingPlan& Mapping = Plan.Mappings.AddDefaulted_GetRef();
		Mapping.BpVariable = Request.BpVariable;
		Mapping.NativeProperty = Request.NativeProperty;
		Mapping.bRename = Request.BpVariable != Request.NativeProperty;
		Mapping.OldDefaultSource = TEXT("none");
		const FName VariableName(*Request.BpVariable);
		const FName NativeName(*Request.NativeProperty);
		const auto Refuse = [&](const FString& Code, const FString& Message)
		{
			AddRefusal(Plan, Code, Message, Request.BpVariable);
		};

		bool bVariableRepeated = false;
		bool bNativeRepeated = false;
		SeenVariables.Add(Request.BpVariable, &bVariableRepeated);
		SeenNative.Add(Request.NativeProperty, &bNativeRepeated);
		if (bVariableRepeated || bNativeRepeated)
		{
			Refuse(CortexErrorCodes::InvalidField, FString::Printf(TEXT("'%s' -> '%s' repeats a variable or native property of another mapping"), *Request.BpVariable, *Request.NativeProperty));
			continue;
		}

		const FProperty* Native = FindFProperty<FProperty>(Parent, NativeName);
		if (Native == nullptr)
		{
			Refuse(CortexErrorCodes::InvalidField, FString::Printf(TEXT("Parent class %s declares no property '%s'; reparent first"), *Parent->GetName(), *Request.NativeProperty));
			continue;
		}
		if (!IsNativeProperty(Native))
		{
			Refuse(CortexErrorCodes::InvalidField, FString::Printf(TEXT("'%s' is declared by %s, which is not a native class"), *Request.NativeProperty, *Native->GetOwnerClass()->GetName()));
			continue;
		}
		if (!Native->HasAnyPropertyFlags(CPF_BlueprintVisible))
		{
			Refuse(CortexErrorCodes::InvalidField, FString::Printf(TEXT("Native property '%s' is not Blueprint visible; nodes could not resolve to it"), *Request.NativeProperty));
			continue;
		}

		const int32 Index = FBlueprintEditorUtils::FindNewVariableIndex(Blueprint, VariableName);
		if (Index == INDEX_NONE)
		{
			if (Mapping.bRename)
			{
				Refuse(CortexErrorCodes::VariableNotFound, FString::Printf(TEXT("Variable '%s' is not declared by the Blueprint (a rename cannot have been adopted by the engine)"), *Request.BpVariable));
				continue;
			}
			const FString Renamed = FindEngineRenamedVariable(Blueprint, Request.BpVariable);
			if (!Renamed.IsEmpty())
			{
				Refuse(CortexErrorCodes::InvalidField, FString::Printf(TEXT("The engine could not adopt '%s' (its type differs from the native property) and renamed it '%s'; it is not adoptable"), *Request.BpVariable, *Renamed));
				continue;
			}
			Mapping.State = TEXT("engine_already_adopted");
			Mapping.TypeMatch = TEXT("verified");
			const UObject* Cdo = GeneratedCdo(Blueprint);
			const FProperty* Current = Cdo != nullptr ? FindFProperty<FProperty>(Cdo->GetClass(), NativeName) : nullptr;
			if (Request.bHasDefault && bTransferDefault)
			{
				Mapping.OldDefault.Text = Request.DefaultText;
				Mapping.OldDefault.bValid = true;
				if (IsFloatingPoint(Native))
				{
					Mapping.OldDefault.bNumeric = true;
					Mapping.OldDefault.Number = FCString::Atod(*Request.DefaultText);
				}
				Mapping.OldDefaultSource = TEXT("default_param");
			}
			else if (bTransferDefault)
			{
				// The engine conformed the variable before this call: its Blueprint default is gone and the current CDO value is only the
				// native one. Kept as a reference, never as verified evidence.
				Mapping.bDefaultUnverified = true;
				if (Current != nullptr)
				{
					Mapping.OldDefault = CaptureValue(Current, Cdo);
					Mapping.OldDefaultSource = TEXT("current_cdo_unverified");
				}
				Mapping.Warnings.Add(FString::Printf(
					TEXT("The engine conformed '%s' before this call, so its original Blueprint default cannot be recovered. Pass 'default' (from a pre-change snapshot or blueprint.get_class_defaults) or acknowledge default_unverified:%s to keep the native value"),
					*Request.BpVariable, *Request.BpVariable));
			}
			else if (Current != nullptr)
			{
				Mapping.OldDefault = CaptureValue(Current, Cdo);
				Mapping.OldDefaultSource = TEXT("current_cdo");
			}
		}
		else
		{
			const FBPVariableDescription& Variable = Blueprint->NewVariables[Index];
			Mapping.State = TEXT("variable_present");
			if (Request.bHasDefault)
			{
				Refuse(CortexErrorCodes::InvalidField, FString::Printf(TEXT("'default' only applies when the engine already adopted '%s'; the Blueprint default is transferred"), *Request.BpVariable));
				continue;
			}
			if (Variable.RepNotifyFunc != NAME_None)
			{
				Refuse(CortexErrorCodes::InvalidField, FString::Printf(TEXT("Variable '%s' has the RepNotify function '%s'; clear it before adopting"), *Request.BpVariable, *Variable.RepNotifyFunc.ToString()));
				continue;
			}
			if (Variable.VarType.PinCategory == UEdGraphSchema_K2::PC_MCDelegate)
			{
				Refuse(CortexErrorCodes::InvalidField, FString::Printf(TEXT("'%s' is an event dispatcher; adoption handles variables only"), *Request.BpVariable));
				continue;
			}
			if (Mapping.bRename && Variable.HasMetaData(FBlueprintMetadata::MD_FieldNotify))
			{
				Refuse(CortexErrorCodes::InvalidField, FString::Printf(TEXT("Variable '%s' is a FieldNotify variable; a rename would not move its FieldNotify registration"), *Request.BpVariable));
				continue;
			}
			FString Why;
			Mapping.TypeMatch = ClassifyType(Variable, Native, Why);
			if (Mapping.TypeMatch.IsEmpty())
			{
				Refuse(CortexErrorCodes::InvalidField, FString::Printf(TEXT("'%s' cannot be adopted into '%s': %s"), *Request.BpVariable, *Request.NativeProperty, *Why));
				continue;
			}
			Mapping.bNumericPair = Mapping.TypeMatch == TEXT("numeric_convertible");
			if (Mapping.bNumericPair)
			{
				Mapping.Warnings.Add(FString::Printf(TEXT("'%s' converts between double and float; precision beyond the native type is lost"), *Request.BpVariable));
			}
			if (Blueprint->Status != BS_UpToDate && Blueprint->Status != BS_UpToDateWithWarnings)
			{
				Refuse(CortexErrorCodes::StalePrecondition, TEXT("The Blueprint is not up to date; compile it first so the old default can be read from the last compiled CDO"));
				continue;
			}

			const UObject* Cdo = GeneratedCdo(Blueprint);
			const FProperty* Own = Cdo != nullptr ? FindFProperty<FProperty>(Cdo->GetClass(), VariableName) : nullptr;
			if (Own != nullptr)
			{
				Mapping.OldDefault = CaptureValue(Own, Cdo);
				Mapping.OldDefaultSource = TEXT("compiled_cdo");
			}
			else if (!Variable.DefaultValue.IsEmpty())
			{
				Mapping.OldDefault.Text = Variable.DefaultValue;
				Mapping.OldDefault.bValid = true;
				Mapping.OldDefaultSource = TEXT("variable_default");
			}
			FCortexBPSaveGameGuard::Evaluate(Variable, Native, Mapping.SaveGameFindings);
			AllFindings.Append(Mapping.SaveGameFindings);
		}
		Mapping.OldDefaultText = Mapping.OldDefault.Text;

		FCortexGraphMemberReferences::FindVariableReferences(Blueprint, VariableName, Mapping.Nodes);
		for (const FCortexMemberReference& Reference : Mapping.Nodes)
		{
			const UEdGraphNode* Node = FindNodeByGuid(Blueprint, Reference.NodeGuid);
			if (Node != nullptr && NodeFollows(Blueprint, Node))
			{
				Mapping.FollowNodes.Add(Reference.NodeGuid);
			}
		}
		if (Native->HasAnyPropertyFlags(CPF_BlueprintReadOnly))
		{
			for (const FGuid& Guid : Mapping.FollowNodes)
			{
				if (Cast<UK2Node_VariableSet>(FindNodeByGuid(Blueprint, Guid)) != nullptr)
				{
					Refuse(CortexErrorCodes::InvalidField, FString::Printf(TEXT("Native property '%s' is read-only in Blueprints but a set node writes '%s'"), *Request.NativeProperty, *Request.BpVariable));
					break;
				}
			}
		}
		if (!bRetargetNodes && Mapping.bRename && !Mapping.FollowNodes.IsEmpty())
		{
			Refuse(CortexErrorCodes::InvalidOperation, FString::Printf(TEXT("'%s' is renamed to '%s' and %d node(s) name it; retarget_nodes=false would orphan them"), *Request.BpVariable, *Request.NativeProperty, Mapping.FollowNodes.Num()));
		}

		{
			const UObject* PlanCdo = GeneratedCdo(Blueprint);
			if (Mapping.bRename && PlanCdo != nullptr)
			{
				Mapping.NativeCdoBefore = CaptureValue(FindFProperty<FProperty>(PlanCdo->GetClass(), NativeName), PlanCdo);
			}
		}
		CollectLoadedValues(Blueprint, Mapping.State == TEXT("variable_present") ? VariableName : NativeName, NativeName, Mapping.LoadedValues);

		FCortexBPDependentScan::Find(Blueprint, VariableName, FCortexBPDependentScan::EMemberKind::Variable, Mapping.Dependents);
		for (const FCortexBPDependentHit& Hit : Mapping.Dependents)
		{
			if (Hit.bUnscanned)
			{
				Refuse(CortexErrorCodes::InvalidOperation, FString::Printf(TEXT("Dependent '%s' could not be inspected (%s); the adoption refuses conservatively"), *Hit.AssetPath, *Hit.Error));
			}
			else if (Mapping.bRename && Hit.References.Num() > 0)
			{
				const TSharedRef<FJsonObject> Extra = MakeShared<FJsonObject>();
				Extra->SetObjectField(TEXT("dependent"), Hit.ToJson());
				AddRefusal(Plan, CortexErrorCodes::InvalidOperation,
					FString::Printf(TEXT("Renaming '%s' to '%s' would leave %d reference(s) in '%s' stale; this command does not rewrite other assets"), *Request.BpVariable, *Request.NativeProperty, Hit.References.Num(), *Hit.AssetPath),
					Request.BpVariable, Extra);
			}
		}

		if (Mapping.bRename)
		{
			Mapping.bRedirectRequired = true;
			Mapping.bRedirectPresent = RedirectPresent(Parent, VariableName, NativeName);
			Mapping.RedirectLine = BuildRedirectLine(Native, Request.BpVariable);
			Mapping.bInstanceValuesAcknowledged = Acknowledged.Contains(FString::Printf(TEXT("instance_values:%s"), *Request.BpVariable));
			if (!Mapping.bRedirectPresent && !Mapping.bInstanceValuesAcknowledged)
			{
				const TSharedRef<FJsonObject> Extra = MakeShared<FJsonObject>();
				Extra->SetStringField(TEXT("redirect_line"), Mapping.RedirectLine);
				Extra->SetStringField(TEXT("config_file"), TEXT("Config/DefaultEngine.ini ([CoreRedirects])"));
				Extra->SetStringField(TEXT("acknowledgment"), FString::Printf(TEXT("instance_values:%s"), *Request.BpVariable));
				AddRefusal(Plan, CortexErrorCodes::MissingPropertyRedirect,
					FString::Printf(TEXT("'%s' is renamed to '%s' but no property redirect maps them; persisted instance values would not load. Add %s or acknowledge instance_values:%s"),
						*Request.BpVariable, *Request.NativeProperty, *Mapping.RedirectLine, *Request.BpVariable),
					Request.BpVariable, Extra);
			}
		}
	}

	// The engine replaces or renames every variable the parent already declares at the next skeleton regeneration; leaving
	// such a variable out of the mappings would change it behind the caller's back.
	for (const FBPVariableDescription& Variable : Blueprint->NewVariables)
	{
		if (!SeenVariables.Contains(Variable.VarName.ToString()) && FindFProperty<FProperty>(Parent, Variable.VarName) != nullptr)
		{
			AddRefusal(Plan, CortexErrorCodes::NameCollision,
				FString::Printf(TEXT("Variable '%s' is not mapped but the parent already declares a property of that name; the engine would replace or rename it at the next compile. Map it too or reparent with acknowledged_collisions"), *Variable.VarName.ToString()),
				Variable.VarName.ToString());
		}
	}

	TArray<FString> AllRequired = FCortexBPSaveGameGuard::RequiredAcknowledgment(AllFindings);
	for (const FMappingPlan& Mapping : Plan.Mappings)
	{
		if (Mapping.bDefaultUnverified)
		{
			AllRequired.Add(FString::Printf(TEXT("default_unverified:%s"), *Mapping.BpVariable));
		}
	}
	AllRequired.Sort();
	Plan.RequiredAcknowledgment = MoveTemp(AllRequired);
	return Plan;
}

//////////////////////////////////////////////////////////////////////////
// Execute

namespace
{
	/** A node of the mapping that must resolve to the native property after the adoption. */
	bool NodeResolvesToNative(UBlueprint* Blueprint, UEdGraphNode* Node, const FName NativeName)
	{
		if (UK2Node_Variable* Variable = Cast<UK2Node_Variable>(Node))
		{
			const FProperty* Resolved = Variable->VariableReference.ResolveMember<FProperty>(Blueprint);
			return Resolved != nullptr && Resolved->GetFName() == NativeName && IsNativeProperty(Resolved);
		}
		const UK2Node* K2Node = Cast<UK2Node>(Node);
		return K2Node != nullptr && K2Node->ReferencesVariable(NativeName, nullptr);
	}

	bool ReadMappings(const TSharedPtr<FJsonObject>& Params, TArray<FOps::FMappingRequest>& OutMappings, FCortexCommandResult& OutError)
	{
		const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
		if (!Params->TryGetArrayField(TEXT("mappings"), Values) || Values == nullptr || Values->IsEmpty())
		{
			OutError = FCortexCommandRouter::Error(CortexErrorCodes::InvalidField, TEXT("mappings must be a non-empty array of {bp_variable, native_property, default?}"));
			return false;
		}
		for (const TSharedPtr<FJsonValue>& Value : *Values)
		{
			const TSharedPtr<FJsonObject>* Item = nullptr;
			if (!Value.IsValid() || !Value->TryGetObject(Item) || Item == nullptr || !Item->IsValid())
			{
				OutError = FCortexCommandRouter::Error(CortexErrorCodes::InvalidField, TEXT("Each mapping must be an object {bp_variable, native_property, default?}"));
				return false;
			}
			for (const TPair<FString, TSharedPtr<FJsonValue>>& Field : (*Item)->Values)
			{
				if (Field.Key != TEXT("bp_variable") && Field.Key != TEXT("native_property") && Field.Key != TEXT("default"))
				{
					OutError = FCortexCommandRouter::Error(CortexErrorCodes::InvalidField,
						FString::Printf(TEXT("Unknown mapping field '%s' (allowed: bp_variable, native_property, default)"), *Field.Key));
					return false;
				}
			}
			FOps::FMappingRequest Request;
			if (!ReadRequiredString(*Item, TEXT("bp_variable"), Request.BpVariable, OutError)
				|| !ReadRequiredString(*Item, TEXT("native_property"), Request.NativeProperty, OutError))
			{
				return false;
			}
			if ((*Item)->HasField(TEXT("default")))
			{
				const TSharedPtr<FJsonValue> Default = (*Item)->TryGetField(TEXT("default"));
				Request.bHasDefault = true;
				if (Default->Type == EJson::String)
				{
					Request.DefaultText = Default->AsString();
				}
				else if (Default->Type == EJson::Number)
				{
					Request.DefaultText = FString::Printf(TEXT("%.17g"), Default->AsNumber());
				}
				else if (Default->Type == EJson::Boolean)
				{
					Request.DefaultText = Default->AsBool() ? TEXT("true") : TEXT("false");
				}
				else
				{
					OutError = FCortexCommandRouter::Error(CortexErrorCodes::InvalidField, TEXT("mapping default must be a string, number or boolean"));
					return false;
				}
			}
			OutMappings.Add(MoveTemp(Request));
		}
		return true;
	}

	bool ReadAcknowledged(const TSharedPtr<FJsonObject>& Params, TArray<FString>& OutAcknowledged, FCortexCommandResult& OutError)
	{
		if (!Params->HasField(TEXT("acknowledged_losses")))
		{
			return true;
		}
		const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
		if (!Params->TryGetArrayField(TEXT("acknowledged_losses"), Values) || Values == nullptr)
		{
			OutError = FCortexCommandRouter::Error(CortexErrorCodes::InvalidField, TEXT("acknowledged_losses must be an array of strings"));
			return false;
		}
		for (const TSharedPtr<FJsonValue>& Value : *Values)
		{
			FString Key;
			if (!Value.IsValid() || Value->Type != EJson::String || !Value->TryGetString(Key))
			{
				OutError = FCortexCommandRouter::Error(CortexErrorCodes::InvalidField, TEXT("acknowledged_losses must be an array of strings"));
				return false;
			}
			if (!Key.StartsWith(TEXT("save_game:")) && !Key.StartsWith(TEXT("instance_values:")) && !Key.StartsWith(TEXT("default_unverified:")))
			{
				OutError = FCortexCommandRouter::Error(CortexErrorCodes::InvalidField,
					FString::Printf(TEXT("Unknown acknowledgment '%s' (allowed: save_game:<variable>, instance_values:<variable>, default_unverified:<variable>)"), *Key));
				return false;
			}
			OutAcknowledged.Add(Key);
		}
		OutAcknowledged = TSet<FString>(OutAcknowledged).Array();
		OutAcknowledged.Sort();
		return true;
	}

	FCortexCommandResult RefusalResult(const FOps::FAdoptPlan& Plan)
	{
		const FOps::FRefusal& First = Plan.Refusals[0];
		const TSharedRef<FJsonObject> Details = MakeShared<FJsonObject>();
		Plan.AppendTo(Details);
		Details->SetBoolField(TEXT("adopted"), false);
		Details->SetBoolField(TEXT("can_apply"), false);
		if (First.Details.IsValid())
		{
			for (const TPair<FString, TSharedPtr<FJsonValue>>& Field : First.Details->Values)
			{
				Details->SetField(Field.Key, Field.Value);
			}
		}
		return FCortexCommandRouter::Error(First.Code, First.Message, Details);
	}

	void MarkDirtyObject(UObject* Object)
	{
		if (Object != nullptr)
		{
			Object->Modify();
		}
	}

	FCortexCommandResult ExecuteImpl(const TSharedPtr<FJsonObject>& Params)
	{
		if (!Params.IsValid())
		{
			return FCortexCommandRouter::Error(CortexErrorCodes::InvalidField, TEXT("Missing params"));
		}

		FCortexCommandResult ParamError;
		FString AssetPath;
		bool bDryRun = false;
		bool bCompile = true;
		bool bSave = false;
		bool bTransferDefault = true;
		bool bRetargetNodes = true;
		if (!ReadRequiredString(Params, TEXT("asset_path"), AssetPath, ParamError)
			|| !ReadOptionalBool(Params, TEXT("dry_run"), false, bDryRun, ParamError)
			|| !ReadOptionalBool(Params, TEXT("compile"), true, bCompile, ParamError)
			|| !ReadOptionalBool(Params, TEXT("save"), false, bSave, ParamError)
			|| !ReadOptionalBool(Params, TEXT("transfer_default"), true, bTransferDefault, ParamError)
			|| !ReadOptionalBool(Params, TEXT("retarget_nodes"), true, bRetargetNodes, ParamError))
		{
			return ParamError;
		}

		TArray<FOps::FMappingRequest> Requests;
		TArray<FString> Acknowledged;
		if (!ReadMappings(Params, Requests, ParamError) || !ReadAcknowledged(Params, Acknowledged, ParamError))
		{
			return ParamError;
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

		if (Expected.IsValid())
		{
			FCortexCommandResult ExpectedError;
			if (!FCortexBPMigrationGuard::CheckExpected(Blueprint, Expected, ExpectedError))
			{
				return ExpectedError;
			}
		}

		// Acknowledgment keys must name a variable of this call; instance_values only applies to renames that lack a redirect.
		{
			TSet<FString> RenamedVariables;
			TSet<FString> AllVariables;
			for (const FOps::FMappingRequest& Request : Requests)
			{
				AllVariables.Add(Request.BpVariable);
				if (Request.BpVariable != Request.NativeProperty)
				{
					RenamedVariables.Add(Request.BpVariable);
				}
			}
			for (const FString& Key : Acknowledged)
			{
				if (!Key.StartsWith(TEXT("instance_values:")))
				{
					continue;
				}
				const FString Variable = Key.Mid(FCString::Strlen(TEXT("instance_values:")));
				if (!AllVariables.Contains(Variable))
				{
					return FCortexCommandRouter::Error(CortexErrorCodes::InvalidField, FString::Printf(TEXT("%s does not name a mapped variable"), *Key));
				}
				if (!RenamedVariables.Contains(Variable))
				{
					return FCortexCommandRouter::Error(CortexErrorCodes::InvalidField, FString::Printf(TEXT("%s is stale: only a renamed mapping holds instance values that need acknowledging"), *Key));
				}
			}
		}

		// Every default write lives in the compiled-state step, so compile=false would drop it while still reporting it as the source.
		if (!bCompile && (!bTransferDefault || Algo::AnyOf(Requests, [](const FOps::FMappingRequest& R)
		{
			return R.BpVariable != R.NativeProperty || R.bHasDefault;
		})))
		{
			return FCortexCommandRouter::Error(CortexErrorCodes::InvalidOperation,
				TEXT("compile=false cannot move defaults and instance values: transfer_default=false, a mapping default and renamed mappings need the compile that writes them"));
		}

		const TSharedRef<FOps::FAdoptPlan> Plan = MakeShared<FOps::FAdoptPlan>(FOps::BuildPlan(Blueprint, Requests, bTransferDefault, bRetargetNodes, Acknowledged));

		// A redirect that already exists makes an instance_values acknowledgment stale.
		for (const FOps::FMappingPlan& Mapping : Plan->Mappings)
		{
			if (Mapping.bInstanceValuesAcknowledged && Mapping.bRedirectPresent)
			{
				return FCortexCommandRouter::Error(CortexErrorCodes::InvalidField,
					FString::Printf(TEXT("instance_values:%s is stale: a property redirect already maps it"), *Mapping.BpVariable));
			}
		}

		if (!Plan->Refusals.IsEmpty())
		{
			return RefusalResult(*Plan);
		}

		// SaveGame findings: dry_run lists them, apply refuses until acknowledged.
		TArray<FString> SaveGameAcknowledged;
		for (const FString& Key : Acknowledged)
		{
			if (Key.StartsWith(TEXT("save_game:")))
			{
				SaveGameAcknowledged.Add(Key);
			}
		}
		TArray<FCortexBPSaveGameGuard::FFinding> AllFindings;
		for (const FOps::FMappingPlan& Mapping : Plan->Mappings)
		{
			AllFindings.Append(Mapping.SaveGameFindings);
		}
		FCortexCommandResult AckError;
		bool bAcknowledged = FCortexBPSaveGameGuard::Check(AllFindings, SaveGameAcknowledged, AckError);

		// default_unverified: an engine-conformed variable without `default` keeps the native value only when the caller says so.
		TArray<FString> DefaultRequired;
		TArray<FString> DefaultAcknowledged;
		for (const FString& Key : Plan->RequiredAcknowledgment)
		{
			if (Key.StartsWith(TEXT("default_unverified:")))
			{
				DefaultRequired.Add(Key);
			}
		}
		for (const FString& Key : Acknowledged)
		{
			if (Key.StartsWith(TEXT("default_unverified:")))
			{
				DefaultAcknowledged.Add(Key);
			}
		}
		if (bAcknowledged && DefaultRequired != DefaultAcknowledged)
		{
			bAcknowledged = false;
			AckError = FCortexCommandRouter::Error(CortexErrorCodes::PotentialDataLoss,
				DefaultRequired.IsEmpty()
					? TEXT("No unverifiable default is at risk; acknowledged_losses must not carry stale default_unverified keys")
					: TEXT("The engine conformed a variable before this call, so its original default cannot be recovered; pass 'default' (from a pre-change snapshot or blueprint.get_class_defaults) or acknowledge exactly the required default_unverified keys to keep the native value"),
				MakeShared<FJsonObject>());
		}
		if (!bAcknowledged)
		{
			if (!AckError.ErrorDetails.IsValid())
			{
				AckError.ErrorDetails = MakeShared<FJsonObject>();
			}
			AckError.ErrorDetails->SetArrayField(TEXT("required_acknowledgment"), ToJsonStrings(Plan->RequiredAcknowledgment));
			AckError.ErrorDetails->SetArrayField(TEXT("acknowledged"), ToJsonStrings(Acknowledged));
		}

		// A conformed-but-uncompiled Blueprint cannot be restored by a recovery compile (the compile would conform it again), so a
		// failure could not be rolled back exactly. State A (variable still declared) is restorable: the apply reverts the parent for the recovery.
		FString ApplyBlocker;
		if (Blueprint->Status != BS_UpToDate && Blueprint->Status != BS_UpToDateWithWarnings
			&& Algo::AnyOf(Plan->Mappings, [](const FOps::FMappingPlan& Mapping) { return Mapping.State == TEXT("engine_already_adopted"); }))
		{
			ApplyBlocker = TEXT("The engine already conformed this Blueprint but it was not compiled afterwards; compile it first so a failed adoption can be restored exactly");
		}

		const auto DescribeInto = [&](const TSharedRef<FJsonObject>& Data)
		{
			Data->SetStringField(TEXT("asset_path"), AssetPath);
			Plan->AppendTo(Data);
		};

		if (bDryRun)
		{
			FCortexBPGuardedRun::FSpec Spec;
			Spec.OpName = TEXT("adopt_native_member");
			Spec.Blueprint = Blueprint;
			Spec.bDryRun = true;
			Spec.bCompile = bCompile;
			Spec.Expected = Expected;
			FCortexCommandResult Result = FCortexBPGuardedRun::Execute(Spec);
			if (Result.bSuccess && Result.Data.IsValid())
			{
				DescribeInto(Result.Data.ToSharedRef());
				Result.Data->SetBoolField(TEXT("adopted"), false);
				Result.Data->SetBoolField(TEXT("can_apply"), bAcknowledged && ApplyBlocker.IsEmpty());
				if (!ApplyBlocker.IsEmpty())
				{
					Result.Data->SetStringField(TEXT("apply_blocker"), ApplyBlocker);
				}
			}
			return Result;
		}

		if (!ApplyBlocker.IsEmpty())
		{
			const TSharedRef<FJsonObject> Details = MakeShared<FJsonObject>();
			Plan->AppendTo(Details);
			Details->SetBoolField(TEXT("adopted"), false);
			return FCortexCommandRouter::Error(CortexErrorCodes::StalePrecondition, ApplyBlocker, Details);
		}

		if (!bAcknowledged)
		{
			if (!AckError.ErrorDetails.IsValid())
			{
				AckError.ErrorDetails = MakeShared<FJsonObject>();
			}
			AckError.ErrorDetails->SetBoolField(TEXT("adopted"), false);
			return AckError;
		}

		const TSharedRef<FAdoptMemberApplyState> State = MakeShared<FAdoptMemberApplyState>();
		UClass* Parent = Blueprint->ParentClass;
		for (const FOps::FMappingPlan& Mapping : Plan->Mappings)
		{
			const FProperty* Native = FindFProperty<FProperty>(Parent, FName(*Mapping.NativeProperty));
			FValueSnap ExpectedValue;
			if (bTransferDefault)
			{
				ExpectedValue = Mapping.OldDefault;
			}
			else
			{
				ExpectedValue = CaptureValue(Native, Parent->GetDefaultObject());
			}
			State->Expected.Add(ExpectedValue);
		}

		FCortexBPGuardedRun::FSpec Spec;
		Spec.OpName = TEXT("adopt_native_member");
		Spec.Blueprint = Blueprint;
		Spec.bCompile = bCompile;
		Spec.bSave = bSave;
		// State A (variable declared next to the native property) cannot be rebuilt by the editor undo; the journal, the binding
		// restore and the value restore recover everything the adoption touches.
		Spec.bUndoOnFailure = false;
		Spec.Expected = Expected;
		Spec.DigestExtra = [Plan](UBlueprint* Target) { return OperationDigest(Target, *Plan); };

		Spec.Capture = [State, Plan](UBlueprint* Target, FString& OutError)
		{
			if (!CheckAnimBindingStorage(Target, OutError))
			{
				return false;
			}
			State->NodeGuidsBefore = AllNodeGuids(Target);
			State->BindingsBefore = CollectBindingTexts(Target);
			State->OldCompiledSuper = Target->GeneratedClass != nullptr ? Target->GeneratedClass->GetSuperClass() : nullptr;
			State->NativeParent = Target->ParentClass;
			State->SnapshotBefore = FCortexBPSnapshotOps::BuildSnapshot(Target, FCortexBPSnapshotOps::MemberDeltaSections(), OutError);
			return State->SnapshotBefore.IsValid();
		};

		Spec.Apply = [Plan, bRetargetNodes](UBlueprint* Target, FString& OutError)
		{
			Target->Modify();
			for (const FOps::FMappingPlan& Mapping : Plan->Mappings)
			{
				// While the variable still exists in the skeleton, the engine retargets every node and binding that names it.
				if (Mapping.State == TEXT("variable_present") && Mapping.bRename && bRetargetNodes)
				{
					FBlueprintEditorUtils::RenameVariableReferences(Target, Target->GeneratedClass, FName(*Mapping.BpVariable), FName(*Mapping.NativeProperty));
				}
				if (bRetargetNodes)
				{
					// A stale member guid would keep pointing at the removed declaration.
					for (const FGuid& Guid : Mapping.FollowNodes)
					{
						UK2Node_Variable* Node = Cast<UK2Node_Variable>(FindNodeByGuid(Target, Guid));
						if (Node != nullptr && Node->VariableReference.IsSelfContext() && !Node->VariableReference.IsLocalScope())
						{
							Node->Modify();
							Node->VariableReference.SetSelfMember(FName(*Mapping.NativeProperty), FGuid());
						}
					}
				}
			}

			bool bRemoved = false;
			for (const FOps::FMappingPlan& Mapping : Plan->Mappings)
			{
				if (Mapping.State != TEXT("variable_present"))
				{
					continue;
				}
				// Never RemoveMemberVariable: it destroys the get/set nodes that name the variable.
				const int32 Index = FBlueprintEditorUtils::FindNewVariableIndex(Target, FName(*Mapping.BpVariable));
				if (Index == INDEX_NONE)
				{
					OutError = FString::Printf(TEXT("Variable '%s' vanished before the adoption"), *Mapping.BpVariable);
					return false;
				}
				Target->NewVariables.RemoveAt(Index);
				bRemoved = true;
			}
			if (bRemoved)
			{
				FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Target);
			}

#if WITH_DEV_AUTOMATION_TESTS
			if (CortexBPAdoptMemberPrivate::GPostApplyHookForTest)
			{
				CortexBPAdoptMemberPrivate::GPostApplyHookForTest(Target);
			}
#endif
			return true;
		};

		Spec.Verify = [State, Plan, bTransferDefault](UBlueprint* Target, bool bCompiled, FString& OutError)
		{
			State->bCompiled = bCompiled;
			if (bCompiled && Target->Status != BS_UpToDate && Target->Status != BS_UpToDateWithWarnings)
			{
				OutError = TEXT("The Blueprint is not up to date after the compile");
				return false;
			}

			UObject* Cdo = GeneratedCdo(Target);
			if (bCompiled && Cdo == nullptr)
			{
				OutError = TEXT("The compiled class has no default object");
				return false;
			}

			// Explicit writes after the compile: the new CDO, then the loaded objects that follow it.
			if (bCompiled)
			{
				for (int32 Index = 0; Index < Plan->Mappings.Num(); ++Index)
				{
					const FOps::FMappingPlan& Mapping = Plan->Mappings[Index];
					const FName NativeName(*Mapping.NativeProperty);
					const FProperty* Native = FindFProperty<FProperty>(Cdo->GetClass(), NativeName);
					if (Native == nullptr || !IsNativeProperty(Native))
					{
						OutError = FString::Printf(TEXT("'%s' does not resolve to the inherited native property"), *Mapping.NativeProperty);
						return false;
					}
					const FValueSnap& ExpectedValue = State->Expected[Index];
					if (ExpectedValue.bValid && !ValueEquals(Native, Cdo, ExpectedValue))
					{
						MarkDirtyObject(Cdo);
						if (!WriteValue(Native, Cdo, ExpectedValue))
						{
							OutError = FString::Printf(TEXT("Could not write the default of '%s' into the native property"), *Mapping.NativeProperty);
							return false;
						}
					}
					for (const FOps::FLoadedValue& Loaded : Mapping.LoadedValues)
					{
						UObject* Object = FindObject<UObject>(nullptr, *Loaded.Path);
						const FProperty* Property = Object != nullptr ? FindFProperty<FProperty>(Object->GetClass(), NativeName) : nullptr;
						if (Property == nullptr)
						{
							FString Present;
							if (Object == nullptr && Target->GeneratedClass != nullptr)
							{
								TArray<UObject*> Now;
								GetObjectsOfClass(Target->GeneratedClass, Now, true, RF_NoFlags);
								for (const UObject* Candidate : Now)
								{
									Present += FString::Printf(TEXT(" [%s%s]"), *Candidate->GetPathName(), IsValid(Candidate) ? TEXT("") : TEXT(" garbage"));
								}
							}
							OutError = Object == nullptr
								? FString::Printf(TEXT("Loaded object %s is gone after the compile; objects of the class now:%s"), *Loaded.Path, *Present)
								: FString::Printf(TEXT("Loaded object %s (class %s) lacks the native property after the compile"), *Loaded.Path, *Object->GetClass()->GetName());
							return false;
						}
						if (Loaded.bIsOverride)
						{
							if (!ValueEquals(Property, Object, Loaded.Value))
							{
								MarkDirtyObject(Object);
								if (!WriteValue(Property, Object, Loaded.Value))
								{
									OutError = FString::Printf(TEXT("Could not move the value of %s"), *Loaded.Path);
									return false;
								}
							}
						}
						else if (!Property->Identical_InContainer(Object, Cdo))
						{
							MarkDirtyObject(Object);
							Property->CopyCompleteValue_InContainer(Object, Cdo);
						}
					}
				}
			}

#if WITH_DEV_AUTOMATION_TESTS
			if (CortexBPAdoptMemberPrivate::GPostWriteHookForTest)
			{
				CortexBPAdoptMemberPrivate::GPostWriteHookForTest(Target);
			}
#endif

			// Readback: nothing below corrects anything.
			const TSet<FGuid> NodesNow = AllNodeGuids(Target);
			if (State->NodeGuidsBefore.Num() != NodesNow.Num() || !State->NodeGuidsBefore.Includes(NodesNow))
			{
				OutError = TEXT("The set of graph nodes changed; the adoption must not destroy or create a node");
				return false;
			}

			for (int32 Index = 0; Index < Plan->Mappings.Num(); ++Index)
			{
				const FOps::FMappingPlan& Mapping = Plan->Mappings[Index];
				const FName OldName(*Mapping.BpVariable);
				const FName NativeName(*Mapping.NativeProperty);
				if (FBlueprintEditorUtils::FindNewVariableIndex(Target, OldName) != INDEX_NONE)
				{
					OutError = FString::Printf(TEXT("Variable '%s' is still declared by the Blueprint"), *Mapping.BpVariable);
					return false;
				}

				// Every captured node resolves to the inherited member; a renamed one carries the new name only.
				TSet<FGuid> StillOld;
				if (Mapping.bRename)
				{
					TArray<FCortexMemberReference> Stale;
					FCortexGraphMemberReferences::FindVariableReferences(Target, OldName, Stale);
					for (const FCortexMemberReference& Reference : Stale)
					{
						if (Mapping.FollowNodes.Contains(Reference.NodeGuid))
						{
							StillOld.Add(Reference.NodeGuid);
						}
					}
					if (!StillOld.IsEmpty())
					{
						OutError = FString::Printf(TEXT("%d node(s) still name the old variable '%s'"), StillOld.Num(), *Mapping.BpVariable);
						return false;
					}
				}
				for (const FGuid& Guid : Mapping.FollowNodes)
				{
					UEdGraphNode* Node = FindNodeByGuid(Target, Guid);
					const bool bResolves = Node != nullptr && NodeResolvesToNative(Target, Node, NativeName);
					Plan->Mappings[Index].ResolvedNodes.Add(Guid, bResolves);
					Plan->Mappings[Index].RetargetedNodes.Add(Guid, bResolves);
					if (!bResolves)
					{
						OutError = FString::Printf(TEXT("Node %s does not resolve to the native property '%s'"), *Guid.ToString(), *Mapping.NativeProperty);
						return false;
					}
				}

				if (bCompiled)
				{
					if (Target->GeneratedClass->FindPropertyByName(OldName) != nullptr && Mapping.bRename)
					{
						OutError = FString::Printf(TEXT("The compiled class still carries '%s'"), *Mapping.BpVariable);
						return false;
					}
					const FProperty* Native = FindFProperty<FProperty>(Cdo->GetClass(), NativeName);
					const FValueSnap& ExpectedValue = State->Expected[Index];
					if (ExpectedValue.bValid && !ValueEquals(Native, Cdo, ExpectedValue))
					{
						OutError = FString::Printf(TEXT("The default of '%s' is '%s' on the CDO, expected '%s'"),
							*Mapping.NativeProperty, *CaptureValue(Native, Cdo).Text, *ExpectedValue.Text);
						return false;
					}
					for (const FOps::FLoadedValue& Loaded : Mapping.LoadedValues)
					{
						const UObject* Object = FindObject<UObject>(nullptr, *Loaded.Path);
						const FProperty* Property = Object != nullptr ? FindFProperty<FProperty>(Object->GetClass(), NativeName) : nullptr;
						const bool bEqual = Property != nullptr
							&& (Loaded.bIsOverride ? ValueEquals(Property, Object, Loaded.Value) : Property->Identical_InContainer(Object, Cdo));
						if (!bEqual)
						{
							OutError = FString::Printf(TEXT("The value of %s does not match after the adoption"), *Loaded.Path);
							return false;
						}
					}
				}
			}

			FString SnapshotError;
			const TSharedPtr<FJsonObject> SnapshotAfter = FCortexBPSnapshotOps::BuildSnapshot(Target, FCortexBPSnapshotOps::MemberDeltaSections(), SnapshotError);
			if (!SnapshotAfter.IsValid())
			{
				OutError = FString::Printf(TEXT("Could not record the post-call snapshot: %s"), *SnapshotError);
				return false;
			}
			State->Differences.Reset();
			const FCortexCommandResult Diff = FCortexBPSnapshotOps::DiffSnapshots(State->SnapshotBefore, SnapshotAfter, FCortexBPSnapshotOps::MemberDeltaSections(), State->Differences, State->Counts);
			if (!Diff.bSuccess)
			{
				OutError = Diff.ErrorMessage;
				return false;
			}
			for (const FOps::FMappingPlan& Mapping : Plan->Mappings)
			{
				if (!bCompiled || Mapping.State != TEXT("variable_present"))
				{
					continue;
				}
				const auto FindDifference = [&](const TCHAR* Section, const FString& Key) -> TSharedPtr<FJsonObject>
				{
					for (const TSharedPtr<FJsonValue>& Value : State->Differences)
					{
						const TSharedPtr<FJsonObject>* Item = nullptr;
						FString ItemSection;
						FString ItemKey;
						if (Value->TryGetObject(Item) && Item != nullptr
							&& (*Item)->TryGetStringField(TEXT("section"), ItemSection) && ItemSection == Section
							&& (*Item)->TryGetStringField(TEXT("key"), ItemKey) && ItemKey == Key)
						{
							return *Item;
						}
					}
					return nullptr;
				};
				const TSharedPtr<FJsonObject> Variable = FindDifference(TEXT("variables"), Mapping.BpVariable);
				if (!Variable.IsValid() || Variable->GetStringField(TEXT("kind")) != TEXT("removed"))
				{
					OutError = FString::Printf(TEXT("The snapshot does not show variable '%s' removed"), *Mapping.BpVariable);
					return false;
				}
				if (!Mapping.bRename && Mapping.TypeMatch == TEXT("exact") && bTransferDefault)
				{
					const TSharedPtr<FJsonObject> Member = FindDifference(TEXT("cdo"), Mapping.NativeProperty);
					if (Member.IsValid() && Member->GetStringField(TEXT("kind")) != TEXT("moved"))
					{
						OutError = FString::Printf(TEXT("The CDO value of '%s' did not survive the move (%s)"), *Mapping.NativeProperty, *Member->GetStringField(TEXT("kind")));
						return false;
					}
				}
			}
			return true;
		};

		// A compile of state A conforms the variable away again (the engine removes a variable the native parent declares), so the
		// recovery builds the class against the parent the last compile used and the native parent is put back afterwards: the
		// restored Blueprint is state A again, not a conformed one.
		Spec.Restore = [State](UBlueprint* Target, FString& OutError)
		{
			if (State->OldCompiledSuper != nullptr && Target->ParentClass != State->OldCompiledSuper)
			{
				Target->Modify();
				Target->ParentClass = State->OldCompiledSuper;
				State->bParentReverted = true;
			}
			return RestoreBindingTexts(Target, State->BindingsBefore, OutError);
		};

		// The recovery compile rebuilds the class from the post-adoption CDO; the old defaults and instance values go back afterwards.
		Spec.PostRecovery = [State, Plan](UBlueprint* Target, FString& OutError)
		{
			bool bRestored = true;
			UObject* Cdo = GeneratedCdo(Target);
			for (const FOps::FMappingPlan& Mapping : Plan->Mappings)
			{
				const FName Name(*Mapping.BpVariable);
				if (Cdo != nullptr && Mapping.OldDefault.bValid)
				{
					const FProperty* Own = FindFProperty<FProperty>(Cdo->GetClass(), Name);
					if (Own != nullptr && !ValueEquals(Own, Cdo, Mapping.OldDefault))
					{
						MarkDirtyObject(Cdo);
						if (!WriteValue(Own, Cdo, Mapping.OldDefault))
						{
							OutError = FString::Printf(TEXT("Could not restore the default of '%s'"), *Mapping.BpVariable);
							bRestored = false;
						}
					}
				}
				if (Cdo != nullptr && Mapping.NativeCdoBefore.bValid)
				{
					const FProperty* NativeProperty = FindFProperty<FProperty>(Cdo->GetClass(), FName(*Mapping.NativeProperty));
					if (NativeProperty != nullptr && !ValueEquals(NativeProperty, Cdo, Mapping.NativeCdoBefore))
					{
						MarkDirtyObject(Cdo);
						if (!WriteValue(NativeProperty, Cdo, Mapping.NativeCdoBefore))
						{
							OutError = FString::Printf(TEXT("Could not restore the native default of '%s'"), *Mapping.NativeProperty);
							bRestored = false;
						}
					}
				}
				for (const FOps::FLoadedValue& Loaded : Mapping.LoadedValues)
				{
					UObject* Object = FindObject<UObject>(nullptr, *Loaded.Path);
					const FProperty* Property = Object != nullptr ? FindFProperty<FProperty>(Object->GetClass(), Name) : nullptr;
					if (Property == nullptr)
					{
						OutError = FString::Printf(TEXT("Could not find %s to restore its value"), *Loaded.Path);
						bRestored = false;
					}
					else if (!ValueEquals(Property, Object, Loaded.Value))
					{
						MarkDirtyObject(Object);
						if (!WriteValue(Property, Object, Loaded.Value))
						{
							OutError = FString::Printf(TEXT("Could not restore the value of %s"), *Loaded.Path);
							bRestored = false;
						}
					}
					if (Object != nullptr && Loaded.NativeBefore.bValid)
					{
						const FProperty* NativeProperty = FindFProperty<FProperty>(Object->GetClass(), FName(*Mapping.NativeProperty));
						if (NativeProperty != nullptr && !ValueEquals(NativeProperty, Object, Loaded.NativeBefore))
						{
							MarkDirtyObject(Object);
							if (!WriteValue(NativeProperty, Object, Loaded.NativeBefore))
							{
								OutError = FString::Printf(TEXT("Could not restore the native value of %s"), *Loaded.Path);
								bRestored = false;
							}
						}
					}
				}
			}
			if (State->bParentReverted)
			{
				// Assigned directly, as in state A: no refresh and no compile.
				Target->ParentClass = State->NativeParent;
				State->bParentReverted = false;
			}
			return bRestored;
		};

		FCortexCommandResult Result = FCortexBPGuardedRun::Execute(Spec);
		if (!Result.bSuccess)
		{
			if (!Result.ErrorDetails.IsValid())
			{
				Result.ErrorDetails = MakeShared<FJsonObject>();
			}
			FString Readback;
			const bool bAdopted = Result.ErrorDetails->TryGetStringField(TEXT("readback_status"), Readback) && Readback == TEXT("matched");
			Result.ErrorDetails->SetBoolField(TEXT("adopted"), bAdopted);
			Plan->AppendTo(Result.ErrorDetails.ToSharedRef());
			return Result;
		}

		const TSharedRef<FJsonObject> Data = Result.Data.ToSharedRef();
		DescribeInto(Data);
		Data->SetBoolField(TEXT("adopted"), true);
		Data->SetBoolField(TEXT("compiled"), bCompile);
		// The readback proved the native property carries the current value, not that it equals the lost Blueprint default.
		FString ReadbackStatus;
		if (Data->TryGetStringField(TEXT("readback_status"), ReadbackStatus) && ReadbackStatus == TEXT("matched")
			&& Algo::AnyOf(Plan->Mappings, [](const FOps::FMappingPlan& Mapping) { return Mapping.bDefaultUnverified; }))
		{
			Data->SetStringField(TEXT("readback_status"), TEXT("matched_acknowledged"));
		}

		const TSharedRef<FJsonObject> Delta = MakeShared<FJsonObject>();
		Delta->SetBoolField(TEXT("compiled"), bCompile);
		if (State->Counts.IsValid())
		{
			Delta->SetObjectField(TEXT("counts"), State->Counts);
		}
		Delta->SetNumberField(TEXT("total_differences"), State->Differences.Num());
		Delta->SetArrayField(TEXT("differences"), State->Differences);
		Data->SetObjectField(TEXT("cdo_delta"), Delta);

		TArray<FString> Warnings;
		for (const FOps::FMappingPlan& Mapping : Plan->Mappings)
		{
			Warnings.Append(Mapping.Warnings);
		}
		Data->SetArrayField(TEXT("warnings"), ToJsonStrings(Warnings));

		UE_LOG(LogCortexBlueprint, Log, TEXT("Adopted %d native member(s) in %s"), Plan->Mappings.Num(), *AssetPath);
		return Result;
	}
}

FCortexCommandResult FCortexBPAdoptMemberOps::Execute(const TSharedPtr<FJsonObject>& Params)
{
	return ExecuteImpl(Params);
}

#if WITH_DEV_AUTOMATION_TESTS
void FCortexBPAdoptMemberOps::SetPostApplyHookForTest(TFunction<void(UBlueprint*)> Hook)
{
	CortexBPAdoptMemberPrivate::GPostApplyHookForTest = MoveTemp(Hook);
}

void FCortexBPAdoptMemberOps::ClearPostApplyHookForTest()
{
	CortexBPAdoptMemberPrivate::GPostApplyHookForTest = nullptr;
}

void FCortexBPAdoptMemberOps::SetPostWriteHookForTest(TFunction<void(UBlueprint*)> Hook)
{
	CortexBPAdoptMemberPrivate::GPostWriteHookForTest = MoveTemp(Hook);
}

void FCortexBPAdoptMemberOps::ClearPostWriteHookForTest()
{
	CortexBPAdoptMemberPrivate::GPostWriteHookForTest = nullptr;
}
#endif
