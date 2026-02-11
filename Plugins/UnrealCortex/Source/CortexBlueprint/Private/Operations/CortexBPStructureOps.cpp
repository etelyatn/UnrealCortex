#include "Operations/CortexBPStructureOps.h"
#include "Engine/Blueprint.h"
#include "EdGraph/EdGraph.h"
#include "EdGraphSchema_K2.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Misc/ScopedTransaction.h"

DEFINE_LOG_CATEGORY_STATIC(LogCortexBlueprintStructure, Log, All);

namespace
{
	/** Helper: load a Blueprint by path (reuses pattern from BPAssetOps) */
	UBlueprint* LoadBlueprintForStructure(const FString& AssetPath, FCortexCommandResult& OutError)
	{
		UObject* LoadedObj = StaticLoadObject(UBlueprint::StaticClass(), nullptr, *AssetPath);
		UBlueprint* Blueprint = Cast<UBlueprint>(LoadedObj);
		if (Blueprint == nullptr)
		{
			OutError = FCortexCommandRouter::Error(
				CortexErrorCodes::BlueprintNotFound,
				FString::Printf(TEXT("Blueprint not found: %s"), *AssetPath)
			);
		}
		return Blueprint;
	}

	/** Helper: resolve a type string to FEdGraphPinType */
	FEdGraphPinType ResolveVariableType(const FString& TypeStr)
	{
		FEdGraphPinType PinType;

		if (TypeStr == TEXT("bool"))
		{
			PinType.PinCategory = UEdGraphSchema_K2::PC_Boolean;
		}
		else if (TypeStr == TEXT("int") || TypeStr == TEXT("int32"))
		{
			PinType.PinCategory = UEdGraphSchema_K2::PC_Int;
		}
		else if (TypeStr == TEXT("float"))
		{
			PinType.PinCategory = UEdGraphSchema_K2::PC_Real;
			PinType.PinSubCategory = UEdGraphSchema_K2::PC_Float;
		}
		else if (TypeStr == TEXT("double"))
		{
			PinType.PinCategory = UEdGraphSchema_K2::PC_Real;
			PinType.PinSubCategory = UEdGraphSchema_K2::PC_Double;
		}
		else if (TypeStr == TEXT("FString") || TypeStr == TEXT("string"))
		{
			PinType.PinCategory = UEdGraphSchema_K2::PC_String;
		}
		else if (TypeStr == TEXT("FName") || TypeStr == TEXT("name"))
		{
			PinType.PinCategory = UEdGraphSchema_K2::PC_Name;
		}
		else if (TypeStr == TEXT("FText") || TypeStr == TEXT("text"))
		{
			PinType.PinCategory = UEdGraphSchema_K2::PC_Text;
		}
		else if (TypeStr == TEXT("FVector") || TypeStr == TEXT("vector"))
		{
			PinType.PinCategory = UEdGraphSchema_K2::PC_Struct;
			PinType.PinSubCategoryObject = TBaseStructure<FVector>::Get();
		}
		else if (TypeStr == TEXT("FRotator") || TypeStr == TEXT("rotator"))
		{
			PinType.PinCategory = UEdGraphSchema_K2::PC_Struct;
			PinType.PinSubCategoryObject = TBaseStructure<FRotator>::Get();
		}
		else if (TypeStr == TEXT("FLinearColor"))
		{
			PinType.PinCategory = UEdGraphSchema_K2::PC_Struct;
			PinType.PinSubCategoryObject = TBaseStructure<FLinearColor>::Get();
		}
		else
		{
			// Try to resolve as a class path (object reference)
			UClass* FoundClass = FindObject<UClass>(ANY_PACKAGE, *TypeStr);
			if (FoundClass != nullptr)
			{
				PinType.PinCategory = UEdGraphSchema_K2::PC_Object;
				PinType.PinSubCategoryObject = FoundClass;
			}
			else
			{
				// Fallback: try as a struct
				UScriptStruct* FoundStruct = FindObject<UScriptStruct>(ANY_PACKAGE, *TypeStr);
				if (FoundStruct != nullptr)
				{
					PinType.PinCategory = UEdGraphSchema_K2::PC_Struct;
					PinType.PinSubCategoryObject = FoundStruct;
				}
				else
				{
					// Default to wildcard if we can't resolve
					PinType.PinCategory = UEdGraphSchema_K2::PC_Wildcard;
				}
			}
		}

		return PinType;
	}
}

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

	FCortexCommandResult LoadError;
	UBlueprint* Blueprint = LoadBlueprintForStructure(AssetPath, LoadError);
	if (Blueprint == nullptr)
	{
		return LoadError;
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

	UE_LOG(LogCortexBlueprintStructure, Log, TEXT("Added variable '%s' (%s) to %s"),
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

	FCortexCommandResult LoadError;
	UBlueprint* Blueprint = LoadBlueprintForStructure(AssetPath, LoadError);
	if (Blueprint == nullptr)
	{
		return LoadError;
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

	UE_LOG(LogCortexBlueprintStructure, Log, TEXT("Removed variable '%s' from %s"), *VarName, *AssetPath);

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

	FCortexCommandResult LoadError;
	UBlueprint* Blueprint = LoadBlueprintForStructure(AssetPath, LoadError);
	if (Blueprint == nullptr)
	{
		return LoadError;
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

	bool bIsPure = false;
	Params->TryGetBoolField(TEXT("is_pure"), bIsPure);

	FString Access = TEXT("Public");
	Params->TryGetStringField(TEXT("access"), Access);

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

	FBlueprintEditorUtils::AddFunctionGraph(Blueprint, NewGraph, false);

	// Set access level
	if (Access == TEXT("Private"))
	{
		NewGraph->GetSchema()->GetGraphDisplayInformation(*NewGraph);
	}

	FBlueprintEditorUtils::MarkBlueprintAsModified(Blueprint);

	// Build response
	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetBoolField(TEXT("added"), true);
	Data->SetStringField(TEXT("name"), FuncName);
	Data->SetStringField(TEXT("graph_name"), NewGraph->GetName());

	// Include inputs/outputs if specified
	const TArray<TSharedPtr<FJsonValue>>* InputsArray = nullptr;
	if (Params->TryGetArrayField(TEXT("inputs"), InputsArray) && InputsArray != nullptr)
	{
		Data->SetArrayField(TEXT("inputs"), *InputsArray);
	}

	const TArray<TSharedPtr<FJsonValue>>* OutputsArray = nullptr;
	if (Params->TryGetArrayField(TEXT("outputs"), OutputsArray) && OutputsArray != nullptr)
	{
		Data->SetArrayField(TEXT("outputs"), *OutputsArray);
	}

	UE_LOG(LogCortexBlueprintStructure, Log, TEXT("Added function '%s' to %s"), *FuncName, *AssetPath);

	return FCortexCommandRouter::Success(Data);
}
