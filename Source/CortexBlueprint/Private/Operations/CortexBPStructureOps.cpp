#include "Operations/CortexBPStructureOps.h"
#include "Operations/CortexBPRemoveGraphOps.h"
#include "Operations/CortexBPAssetOps.h"
#include "Operations/CortexBPCommandParams.h"
#include "Operations/CortexBPGuardedRun.h"
#include "Operations/CortexBPSaveGameGuard.h"
#include "Operations/CortexBPSCSSupport.h"
#include "Operations/CortexBPTypeUtils.h"
#include "CortexBlueprintModule.h"
#include "CortexGraphMemberReferences.h"
#include "CortexTypes.h"
#include "Engine/Blueprint.h"
#include "EdGraph/EdGraph.h"
#include "EdGraphSchema_K2.h"
#include "K2Node_FunctionEntry.h"
#include "K2Node_FunctionResult.h"
#include "K2Node_CustomEvent.h"
#include "K2Node_Knot.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "ScopedTransaction.h"
#include "UObject/SavePackage.h"
#include "Misc/PackageName.h"

using CortexBPTypeUtils::ResolveVariableType;
using CortexBPTypeUtils::FriendlyTypeName;

namespace
{
	using CortexBPParams::ReadOptionalBool;
	using CortexBPParams::ToJsonStrings;
	using CortexBPSCSSupport::FindNodeByGuid;

	TArray<TSharedPtr<FJsonValue>> ReferencesToJson(const TArray<FCortexMemberReference>& References)
	{
		TArray<TSharedPtr<FJsonValue>> Array;
		for (const FCortexMemberReference& Reference : References)
		{
			Array.Add(MakeShared<FJsonValueObject>(FCortexGraphMemberReferences::ReferenceToJson(Reference)));
		}
		return Array;
	}

	TArray<TSharedPtr<FJsonValue>> FindingsToJson(const TArray<FCortexBPSaveGameGuard::FFinding>& Findings)
	{
		TArray<TSharedPtr<FJsonValue>> Array;
		for (const FCortexBPSaveGameGuard::FFinding& Finding : Findings)
		{
			Array.Add(MakeShared<FJsonValueObject>(Finding.ToJson()));
		}
		return Array;
	}

	/** The CDO value of a Blueprint variable's compiled property, in the export format. */
	FString ReadCdoText(UBlueprint* Blueprint, const FName Name, bool& bOutValid)
	{
		bOutValid = false;
		UObject* Cdo = Blueprint->GeneratedClass != nullptr ? Blueprint->GeneratedClass->GetDefaultObject(false) : nullptr;
		const FProperty* Property = Cdo != nullptr ? FindFProperty<FProperty>(Cdo->GetClass(), Name) : nullptr;
		if (Property == nullptr)
		{
			return FString();
		}
		FString Text;
		bOutValid = FBlueprintEditorUtils::PropertyValueToString(Property, reinterpret_cast<const uint8*>(Cdo), Text);
		return Text;
	}

	bool WriteCdoText(UBlueprint* Blueprint, const FName Name, const FString& Text)
	{
		UObject* Cdo = Blueprint->GeneratedClass != nullptr ? Blueprint->GeneratedClass->GetDefaultObject(false) : nullptr;
		const FProperty* Property = Cdo != nullptr ? FindFProperty<FProperty>(Cdo->GetClass(), Name) : nullptr;
		return Property != nullptr
			&& FBlueprintEditorUtils::PropertyValueFromString(Property, Text, reinterpret_cast<uint8*>(Cdo), Cdo);
	}
}

#if WITH_DEV_AUTOMATION_TESTS
namespace
{
	TFunction<void(UBlueprint*)> GRemoveVariablePostApplyHookForTest;
}
#endif

FCortexCommandResult FCortexBPStructureOps::AddVariable(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath;
	FString VarName;
	FString VarType;

	if (!Params.IsValid()
		|| !Params->TryGetStringField(TEXT("asset_path"), AssetPath)
		|| !Params->TryGetStringField(TEXT("name"), VarName)
		|| !Params->TryGetStringField(TEXT("type"), VarType))
	{
		return FCortexCommandRouter::Error(
			CortexErrorCodes::InvalidField,
			TEXT("Missing required params: asset_path, name, type")
		);
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

	const FName VarFName(*VarName);

	// Check if variable already exists
	for (const FBPVariableDescription& Var : Blueprint->NewVariables)
	{
		if (Var.VarName == VarFName)
		{
			return FCortexCommandRouter::Error(
				CortexErrorCodes::VariableExists,
				FString::Printf(TEXT("Variable '%s' already exists"), *VarName)
			);
		}
	}

	if (Blueprint->ParentClass && Blueprint->ParentClass->FindPropertyByName(VarFName) != nullptr)
	{
		return FCortexCommandRouter::Error(
			CortexErrorCodes::InvalidValue,
			FString::Printf(TEXT("Variable '%s' collides with an inherited UPROPERTY"), *VarName)
		);
	}

	FEdGraphPinType PinType = ResolveVariableType(VarType);

	FScopedTransaction Transaction(FText::FromString(
		FString::Printf(TEXT("Cortex:Add Variable %s"), *VarName)
	));

	bool bSuccess = FBlueprintEditorUtils::AddMemberVariable(
		Blueprint, VarFName, PinType);

	if (!bSuccess)
	{
		return FCortexCommandRouter::Error(
			CortexErrorCodes::InvalidValue,
			FString::Printf(TEXT("Failed to add variable '%s'"), *VarName)
		);
	}

	// Set default value and flags
	FString DefaultValue;
	Params->TryGetStringField(TEXT("default_value"), DefaultValue);

	bool bIsExposed = false;
	Params->TryGetBoolField(TEXT("is_exposed"), bIsExposed);

	FString Category;
	Params->TryGetStringField(TEXT("category"), Category);

	for (FBPVariableDescription& Var : Blueprint->NewVariables)
	{
		if (Var.VarName == VarFName)
		{
			if (!DefaultValue.IsEmpty())
			{
				Var.DefaultValue = DefaultValue;
			}
			if (bIsExposed)
			{
				Var.PropertyFlags |= CPF_BlueprintVisible;
			}
			if (!Category.IsEmpty())
			{
				Var.Category = FText::FromString(Category);
			}
			break;
		}
	}

	// For event dispatchers, create the signature graph so parameters can be added later
	if (PinType.PinCategory == UEdGraphSchema_K2::PC_MCDelegate)
	{
		const UEdGraphSchema_K2* K2Schema = GetDefault<UEdGraphSchema_K2>();
		UEdGraph* SignatureGraph = FBlueprintEditorUtils::CreateNewGraph(
			Blueprint, VarFName, UEdGraph::StaticClass(), UEdGraphSchema_K2::StaticClass());

		if (SignatureGraph == nullptr)
		{
			FBlueprintEditorUtils::RemoveMemberVariable(Blueprint, VarFName);
			return FCortexCommandRouter::Error(
				CortexErrorCodes::InvalidValue,
				FString::Printf(TEXT("Failed to create delegate signature graph for '%s'"), *VarName)
			);
		}

		SignatureGraph->bEditable = false;
		K2Schema->CreateDefaultNodesForGraph(*SignatureGraph);
		K2Schema->CreateFunctionGraphTerminators(*SignatureGraph, (UClass*)nullptr);
		K2Schema->AddExtraFunctionFlags(SignatureGraph, FUNC_BlueprintCallable | FUNC_BlueprintEvent | FUNC_Public);
		K2Schema->MarkFunctionEntryAsEditable(SignatureGraph, true);
		Blueprint->DelegateSignatureGraphs.Add(SignatureGraph);
		FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);
	}
	else
	{
		FBlueprintEditorUtils::MarkBlueprintAsModified(Blueprint);
	}

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetBoolField(TEXT("added"), true);
	Data->SetStringField(TEXT("name"), VarName);
	Data->SetStringField(TEXT("type"), FriendlyTypeName(PinType));
	if (!DefaultValue.IsEmpty())
	{
		Data->SetStringField(TEXT("default_value"), DefaultValue);
	}

	UE_LOG(LogCortexBlueprint, Log, TEXT("Added variable '%s' (%s) to %s"),
		*VarName, *VarType, *AssetPath);

	return FCortexCommandRouter::Success(Data);
}

FCortexCommandResult FCortexBPStructureOps::RemoveVariable(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath;
	FString VarName;

	if (!Params.IsValid()
		|| !Params->TryGetStringField(TEXT("asset_path"), AssetPath)
		|| !Params->TryGetStringField(TEXT("name"), VarName)
		|| VarName.IsEmpty())
	{
		return FCortexCommandRouter::Error(
			CortexErrorCodes::InvalidField,
			TEXT("Missing required params: asset_path, name")
		);
	}

	FCortexCommandResult ParamError;
	bool bDryRun = false;
	bool bCompile = false;
	bool bSave = false;
	bool bRemoveReferencingNodes = false;
	if (!ReadOptionalBool(Params, TEXT("dry_run"), false, bDryRun, ParamError)
		|| !ReadOptionalBool(Params, TEXT("compile"), false, bCompile, ParamError)
		|| !ReadOptionalBool(Params, TEXT("save"), false, bSave, ParamError)
		|| !ReadOptionalBool(Params, TEXT("remove_referencing_nodes"), false, bRemoveReferencingNodes, ParamError))
	{
		return ParamError;
	}
	if (bDryRun && bSave)
	{
		return FCortexCommandRouter::Error(CortexErrorCodes::InvalidOperation, TEXT("A dry run must use save=false"));
	}
	if (bSave && !bCompile)
	{
		return FCortexCommandRouter::Error(CortexErrorCodes::InvalidOperation, TEXT("save=true requires compile=true"));
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
			if (!Key.StartsWith(TEXT("save_game:")))
			{
				return FCortexCommandRouter::Error(CortexErrorCodes::InvalidField,
					FString::Printf(TEXT("Unknown acknowledgment '%s' (allowed: save_game:<variable>)"), *Key));
			}
			Acknowledged.Add(Key);
		}
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

	const FName VarFName(*VarName);
	const int32 VariableIndex = FBlueprintEditorUtils::FindNewVariableIndex(Blueprint, VarFName);
	if (VariableIndex == INDEX_NONE)
	{
		return FCortexCommandRouter::Error(
			CortexErrorCodes::VariableNotFound,
			FString::Printf(TEXT("Variable '%s' not found"), *VarName)
		);
	}

	// Read-only preflight: the nodes the engine's RemoveMemberVariable would destroy, and the SaveGame data at risk.
	const FBPVariableDescription Variable = Blueprint->NewVariables[VariableIndex];
	TArray<FCortexMemberReference> References;
	FCortexGraphMemberReferences::FindVariableReferences(Blueprint, VarFName, References);
	TArray<FCortexBPSaveGameGuard::FFinding> Findings;
	FCortexBPSaveGameGuard::Evaluate(Variable, nullptr, Findings);
	const TArray<FString> RequiredAcknowledgment = FCortexBPSaveGameGuard::RequiredAcknowledgment(Findings);
	// A full compile moves the variable's DefaultValue into the CDO and clears it, so the value only exists on the
	// compiled class. The recovery compile rebuilds a class whose default is the property type's zero; the captured
	// value is written back once that class exists again (operation-owned state, design C3).
	bool bValueValid = false;
	const FString ValueBefore = ReadCdoText(Blueprint, VarFName, bValueValid);

	const auto DescribePlan = [&](const TSharedRef<FJsonObject>& Data)
	{
		Data->SetStringField(TEXT("name"), VarName);
		Data->SetNumberField(TEXT("reference_count"), References.Num());
		Data->SetArrayField(TEXT("referencing_nodes"), ReferencesToJson(References));
		Data->SetArrayField(TEXT("save_game_findings"), FindingsToJson(Findings));
		Data->SetArrayField(TEXT("required_acknowledgment"), ToJsonStrings(RequiredAcknowledgment));
	};

	if (bDryRun)
	{
		FCortexBPGuardedRun::FSpec Spec;
		Spec.OpName = TEXT("remove_variable");
		Spec.Blueprint = Blueprint;
		Spec.bDryRun = true;
		Spec.bCompile = bCompile;
		Spec.Expected = Expected;
		FCortexCommandResult Result = FCortexBPGuardedRun::Execute(Spec);
		if (Result.bSuccess && Result.Data.IsValid())
		{
			DescribePlan(Result.Data.ToSharedRef());
			Result.Data->SetBoolField(TEXT("removed"), false);
			Result.Data->SetBoolField(TEXT("can_apply"),
				RequiredAcknowledgment.IsEmpty() && (References.IsEmpty() || bRemoveReferencingNodes));
		}
		return Result;
	}

	if (!References.IsEmpty() && !bRemoveReferencingNodes)
	{
		const TSharedRef<FJsonObject> Details = MakeShared<FJsonObject>();
		DescribePlan(Details);
		Details->SetBoolField(TEXT("removed"), false);
		Details->SetBoolField(TEXT("can_apply"), false);
		return FCortexCommandRouter::Error(CortexErrorCodes::MemberReferenced,
			FString::Printf(TEXT("Variable '%s' is referenced by %d graph node(s); removing it would destroy them. "
				"Pass remove_referencing_nodes=true to remove the nodes together with the variable."),
				*VarName, References.Num()),
			Details);
	}

	FCortexCommandResult AcknowledgmentError;
	if (!FCortexBPSaveGameGuard::Check(Findings, Acknowledged, AcknowledgmentError))
	{
		if (!AcknowledgmentError.ErrorDetails.IsValid())
		{
			AcknowledgmentError.ErrorDetails = MakeShared<FJsonObject>();
		}
		DescribePlan(AcknowledgmentError.ErrorDetails.ToSharedRef());
		AcknowledgmentError.ErrorDetails->SetBoolField(TEXT("removed"), false);
		return AcknowledgmentError;
	}

	TArray<TSharedPtr<FJsonValue>> RemovedNodes;

	FCortexBPGuardedRun::FSpec Spec;
	Spec.OpName = TEXT("remove_variable");
	Spec.Blueprint = Blueprint;
	Spec.bCompile = bCompile;
	Spec.bSave = bSave;
	Spec.Expected = Expected;

	Spec.Apply = [&RemovedNodes, VarFName, References](UBlueprint* Target, FString& OutError)
	{
		RemovedNodes.Reset();
		// RemoveNode breaks the node's links first, so a chain the variable sat in is cut at this node, not silently mangled
		// by FBlueprintEditorUtils::RemoveMemberVariable.
		for (const FCortexMemberReference& Reference : References)
		{
			UEdGraphNode* Node = FindNodeByGuid(Target, Reference.NodeGuid);
			if (Node == nullptr)
			{
				OutError = FString::Printf(TEXT("Referencing node %s vanished before the removal"),
					*Reference.NodeGuid.ToString(EGuidFormats::DigitsWithHyphens));
				return false;
			}
			RemovedNodes.Add(MakeShared<FJsonValueObject>(FCortexGraphMemberReferences::ReferenceToJson(Reference)));
			FBlueprintEditorUtils::RemoveNode(Target, Node, /*bDontRecompile=*/true);
		}

		if (FBlueprintEditorUtils::FindNewVariableIndex(Target, VarFName) == INDEX_NONE)
		{
			OutError = FString::Printf(TEXT("Variable '%s' vanished before the removal"), *VarFName.ToString());
			return false;
		}
		FBlueprintEditorUtils::RemoveMemberVariable(Target, VarFName);
		FBlueprintEditorUtils::MarkBlueprintAsModified(Target);

#if WITH_DEV_AUTOMATION_TESTS
		if (GRemoveVariablePostApplyHookForTest)
		{
			GRemoveVariablePostApplyHookForTest(Target);
		}
#endif
		return true;
	};

	Spec.PostRecovery = [VarFName, ValueBefore, bValueValid](UBlueprint* Target, FString& OutError)
	{
		if (bValueValid && !WriteCdoText(Target, VarFName, ValueBefore))
		{
			OutError = FString::Printf(TEXT("Could not restore the compiled default of '%s'"), *VarFName.ToString());
			return false;
		}
		return true;
	};

	Spec.Verify = [VarFName, References](UBlueprint* Target, bool bCompiled, FString& OutError)
	{
		if (FBlueprintEditorUtils::FindNewVariableIndex(Target, VarFName) != INDEX_NONE)
		{
			OutError = FString::Printf(TEXT("Variable '%s' is still declared by the Blueprint"), *VarFName.ToString());
			return false;
		}
		for (const FCortexMemberReference& Reference : References)
		{
			if (FindNodeByGuid(Target, Reference.NodeGuid) != nullptr)
			{
				OutError = FString::Printf(TEXT("Referencing node %s was not removed"),
					*Reference.NodeGuid.ToString(EGuidFormats::DigitsWithHyphens));
				return false;
			}
		}
		if (bCompiled)
		{
			if (Target->Status != BS_UpToDate && Target->Status != BS_UpToDateWithWarnings)
			{
				OutError = TEXT("The Blueprint is not up to date after the compile");
				return false;
			}
			const FProperty* Property = Target->GeneratedClass != nullptr
				? Target->GeneratedClass->FindPropertyByName(VarFName) : nullptr;
			if (Property != nullptr && Property->GetOwnerClass() == Target->GeneratedClass)
			{
				OutError = FString::Printf(TEXT("The compiled class still declares '%s'"), *VarFName.ToString());
				return false;
			}
		}
		return true;
	};

	FCortexCommandResult Result = FCortexBPGuardedRun::Execute(Spec);
	if (!Result.bSuccess)
	{
		if (!Result.ErrorDetails.IsValid())
		{
			Result.ErrorDetails = MakeShared<FJsonObject>();
		}
		DescribePlan(Result.ErrorDetails.ToSharedRef());
		Result.ErrorDetails->SetBoolField(TEXT("removed"), false);
		Result.ErrorDetails->SetArrayField(TEXT("removed_nodes"), RemovedNodes);
		return Result;
	}

	const TSharedRef<FJsonObject> Data = Result.Data.ToSharedRef();
	DescribePlan(Data);
	Data->SetBoolField(TEXT("removed"), true);
	Data->SetArrayField(TEXT("removed_nodes"), RemovedNodes);

	UE_LOG(LogCortexBlueprint, Log, TEXT("Removed variable '%s' from %s (%d referencing node(s))"), *VarName, *AssetPath, RemovedNodes.Num());

	return Result;
}

FCortexCommandResult FCortexBPStructureOps::AddFunction(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath;
	FString FuncName;

	if (!Params.IsValid()
		|| !Params->TryGetStringField(TEXT("asset_path"), AssetPath)
		|| !Params->TryGetStringField(TEXT("name"), FuncName))
	{
		return FCortexCommandRouter::Error(
			CortexErrorCodes::InvalidField,
			TEXT("Missing required params: asset_path, name")
		);
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

	// Check if function already exists
	for (UEdGraph* Graph : Blueprint->FunctionGraphs)
	{
		if (Graph != nullptr && Graph->GetName() == FuncName)
		{
			return FCortexCommandRouter::Error(
				CortexErrorCodes::FunctionExists,
				FString::Printf(TEXT("Function '%s' already exists"), *FuncName)
			);
		}
	}

	// Parse optional inputs/outputs arrays
	const TArray<TSharedPtr<FJsonValue>>* InputsArray = nullptr;
	Params->TryGetArrayField(TEXT("inputs"), InputsArray);

	const TArray<TSharedPtr<FJsonValue>>* OutputsArray = nullptr;
	Params->TryGetArrayField(TEXT("outputs"), OutputsArray);

	// All-or-nothing validation: resolve all types before creating anything
	struct FPinSpec
	{
		FString Name;
		FEdGraphPinType PinType;
	};

	TArray<FPinSpec> InputSpecs;
	TArray<FPinSpec> OutputSpecs;

	auto ParsePinArray = [](const TArray<TSharedPtr<FJsonValue>>* Array, TArray<FPinSpec>& OutSpecs, FString& OutError) -> bool
	{
		if (!Array)
		{
			return true;
		}
		for (const TSharedPtr<FJsonValue>& Val : *Array)
		{
			const TSharedPtr<FJsonObject>& Obj = Val->AsObject();
			if (!Obj.IsValid())
			{
				continue;
			}

			FString PinName, PinTypeStr;
			if (!Obj->TryGetStringField(TEXT("name"), PinName) || !Obj->TryGetStringField(TEXT("type"), PinTypeStr))
			{
				OutError = TEXT("Each input/output requires 'name' and 'type' fields");
				return false;
			}

			FEdGraphPinType PinType = ResolveVariableType(PinTypeStr);
			if (PinType.PinCategory == UEdGraphSchema_K2::PC_Wildcard)
			{
				OutError = FString::Printf(TEXT("Unknown type '%s' for parameter '%s'"), *PinTypeStr, *PinName);
				return false;
			}

			OutSpecs.Add({ PinName, PinType });
		}
		return true;
	};

	FString PinValidationError;
	if (!ParsePinArray(InputsArray, InputSpecs, PinValidationError) ||
		!ParsePinArray(OutputsArray, OutputSpecs, PinValidationError))
	{
		return FCortexCommandRouter::Error(CortexErrorCodes::InvalidValue, PinValidationError);
	}

	FScopedTransaction Transaction(FText::FromString(
		FString::Printf(TEXT("Cortex:Add Function %s"), *FuncName)
	));

	// Create the function graph
	UEdGraph* NewGraph = FBlueprintEditorUtils::CreateNewGraph(
		Blueprint,
		FName(*FuncName),
		UEdGraph::StaticClass(),
		UEdGraphSchema_K2::StaticClass()
	);

	// A function graph created as a *user* graph is what the engine flags with
	// FUNC_BlueprintCallable | FUNC_BlueprintEvent | FUNC_Public (and marks editable), exactly like the
	// editor's own New Function. Creating it without that flag yields a function that no Kismet graph
	// can call or override, so `graph.apply_patch` rightly refuses a CallFunction node for it — which
	// makes every published way of calling a Blueprint-defined function (including the typed authoring
	// contract's explicit-target call) impossible.
	FBlueprintEditorUtils::AddFunctionGraph<UClass>(Blueprint, NewGraph, true, nullptr);

	// Find entry node
	UK2Node_FunctionEntry* EntryNode = nullptr;
	for (UEdGraphNode* Node : NewGraph->Nodes)
	{
		EntryNode = Cast<UK2Node_FunctionEntry>(Node);
		if (EntryNode)
		{
			break;
		}
	}

	// Add input pins to entry node
	if (EntryNode && InputSpecs.Num() > 0)
	{
		for (const FPinSpec& Spec : InputSpecs)
		{
			EntryNode->CreateUserDefinedPin(FName(*Spec.Name), Spec.PinType, EGPD_Output);
		}
	}

	// Add output pins to result node
	if (OutputSpecs.Num() > 0 && EntryNode)
	{
		UK2Node_FunctionResult* ResultNode =
			FBlueprintEditorUtils::FindOrCreateFunctionResultNode(EntryNode);

		if (ResultNode)
		{
			for (const FPinSpec& Spec : OutputSpecs)
			{
				ResultNode->CreateUserDefinedPin(FName(*Spec.Name), Spec.PinType, EGPD_Input);
			}
		}
	}

	FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);

	// Build response — read back actual created pins, don't echo input
	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetBoolField(TEXT("added"), true);
	Data->SetStringField(TEXT("name"), FuncName);
	Data->SetStringField(TEXT("graph_name"), NewGraph->GetName());

	// Serialize inputs from actual UserDefinedPins on entry node
	TArray<TSharedPtr<FJsonValue>> ResponseInputs;
	if (EntryNode)
	{
		for (const TSharedPtr<FUserPinInfo>& PinInfo : EntryNode->UserDefinedPins)
		{
			TSharedPtr<FJsonObject> PinObj = MakeShared<FJsonObject>();
			PinObj->SetStringField(TEXT("name"), PinInfo->PinName.ToString());
			PinObj->SetStringField(TEXT("type"), FriendlyTypeName(PinInfo->PinType));
			ResponseInputs.Add(MakeShared<FJsonValueObject>(PinObj));
		}
	}
	Data->SetArrayField(TEXT("inputs"), ResponseInputs);

	// Serialize outputs from result node
	TArray<TSharedPtr<FJsonValue>> ResponseOutputs;
	for (UEdGraphNode* Node : NewGraph->Nodes)
	{
		if (UK2Node_FunctionResult* ResultNode = Cast<UK2Node_FunctionResult>(Node))
		{
			for (const TSharedPtr<FUserPinInfo>& PinInfo : ResultNode->UserDefinedPins)
			{
				TSharedPtr<FJsonObject> PinObj = MakeShared<FJsonObject>();
				PinObj->SetStringField(TEXT("name"), PinInfo->PinName.ToString());
				PinObj->SetStringField(TEXT("type"), FriendlyTypeName(PinInfo->PinType));
				ResponseOutputs.Add(MakeShared<FJsonValueObject>(PinObj));
			}
			break;
		}
	}
	Data->SetArrayField(TEXT("outputs"), ResponseOutputs);

	UE_LOG(LogCortexBlueprint, Log, TEXT("Added function '%s' to %s"), *FuncName, *AssetPath);

	return FCortexCommandRouter::Success(Data);
}

FCortexCommandResult FCortexBPStructureOps::RemoveGraph(const TSharedPtr<FJsonObject>& Params)
{
	return FCortexBPRemoveGraphOps::Execute(Params);
}

#if WITH_DEV_AUTOMATION_TESTS
void FCortexBPStructureOps::SetRemoveVariablePostApplyHookForTest(TFunction<void(UBlueprint*)> Hook)
{
	GRemoveVariablePostApplyHookForTest = MoveTemp(Hook);
}

void FCortexBPStructureOps::ClearRemoveVariablePostApplyHookForTest()
{
	GRemoveVariablePostApplyHookForTest = nullptr;
}
#endif
