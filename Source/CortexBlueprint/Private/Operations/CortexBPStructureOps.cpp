#include "Operations/CortexBPStructureOps.h"
#include "Operations/CortexBPAssetOps.h"
#include "Operations/CortexBPTypeUtils.h"
#include "CortexBlueprintModule.h"
#include "Engine/Blueprint.h"
#include "EdGraph/EdGraph.h"
#include "EdGraphSchema_K2.h"
#include "K2Node_FunctionEntry.h"
#include "K2Node_FunctionResult.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "ScopedTransaction.h"

using CortexBPTypeUtils::ResolveVariableType;
using CortexBPTypeUtils::FriendlyTypeName;

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

	FString LoadError;
	UBlueprint* Blueprint = FCortexBPAssetOps::LoadBlueprint(AssetPath, LoadError);
	if (Blueprint == nullptr)
	{
		return FCortexCommandRouter::Error(CortexErrorCodes::BlueprintNotFound, LoadError);
	}

	// Check if variable already exists
	for (const FBPVariableDescription& Var : Blueprint->NewVariables)
	{
		if (Var.VarName == FName(*VarName))
		{
			return FCortexCommandRouter::Error(
				CortexErrorCodes::VariableExists,
				FString::Printf(TEXT("Variable '%s' already exists"), *VarName)
			);
		}
	}

	FEdGraphPinType PinType = ResolveVariableType(VarType);

	FScopedTransaction Transaction(FText::FromString(
		FString::Printf(TEXT("Cortex:Add Variable %s"), *VarName)
	));

	bool bSuccess = FBlueprintEditorUtils::AddMemberVariable(
		Blueprint, FName(*VarName), PinType);

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
		if (Var.VarName == FName(*VarName))
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

	FBlueprintEditorUtils::MarkBlueprintAsModified(Blueprint);

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetBoolField(TEXT("added"), true);
	Data->SetStringField(TEXT("name"), VarName);
	Data->SetStringField(TEXT("type"), VarType);
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
		|| !Params->TryGetStringField(TEXT("name"), VarName))
	{
		return FCortexCommandRouter::Error(
			CortexErrorCodes::InvalidField,
			TEXT("Missing required params: asset_path, name")
		);
	}

	FString LoadError;
	UBlueprint* Blueprint = FCortexBPAssetOps::LoadBlueprint(AssetPath, LoadError);
	if (Blueprint == nullptr)
	{
		return FCortexCommandRouter::Error(CortexErrorCodes::BlueprintNotFound, LoadError);
	}

	// Check if variable exists
	bool bFound = false;
	for (const FBPVariableDescription& Var : Blueprint->NewVariables)
	{
		if (Var.VarName == FName(*VarName))
		{
			bFound = true;
			break;
		}
	}

	if (!bFound)
	{
		return FCortexCommandRouter::Error(
			CortexErrorCodes::VariableNotFound,
			FString::Printf(TEXT("Variable '%s' not found"), *VarName)
		);
	}

	FScopedTransaction Transaction(FText::FromString(
		FString::Printf(TEXT("Cortex:Remove Variable %s"), *VarName)
	));

	FBlueprintEditorUtils::RemoveMemberVariable(Blueprint, FName(*VarName));
	FBlueprintEditorUtils::MarkBlueprintAsModified(Blueprint);

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetBoolField(TEXT("removed"), true);
	Data->SetStringField(TEXT("name"), VarName);

	UE_LOG(LogCortexBlueprint, Log, TEXT("Removed variable '%s' from %s"), *VarName, *AssetPath);

	return FCortexCommandRouter::Success(Data);
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

	FString ValidationError;
	if (!ParsePinArray(InputsArray, InputSpecs, ValidationError) ||
		!ParsePinArray(OutputsArray, OutputSpecs, ValidationError))
	{
		return FCortexCommandRouter::Error(CortexErrorCodes::InvalidValue, ValidationError);
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

	FBlueprintEditorUtils::AddFunctionGraph<UClass>(Blueprint, NewGraph, false, nullptr);

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
