#include "Operations/CortexMaterialGraphOps.h"
#include "Operations/CortexMaterialAssetOps.h"
#include "CortexMaterialModule.h"
#include "Misc/EngineVersionComparison.h"
#include "Materials/Material.h"
#include "Materials/MaterialExpression.h"
#include "MaterialDomain.h"

// UE 5.4 compatibility: FExpressionInputIterator was added in UE 5.5
#if UE_VERSION_OLDER_THAN(5, 5, 0)
struct FExpressionInputIterator
{
	FExpressionInputIterator(UMaterialExpression* InExpr)
		: Inputs(InExpr ? InExpr->GetInputs() : TArray<FExpressionInput*>())
		, Index(0)
	{
		Input = Inputs.IsValidIndex(0) ? Inputs[0] : nullptr;
	}

	explicit operator bool() const { return Inputs.IsValidIndex(Index); }

	FExpressionInputIterator& operator++()
	{
		++Index;
		Input = Inputs.IsValidIndex(Index) ? Inputs[Index] : nullptr;
		return *this;
	}

	TArray<FExpressionInput*> Inputs;
	FExpressionInput* Input = nullptr;
	int32 Index = 0;
};
#endif
#include "Materials/MaterialExpressionCollectionParameter.h"
#include "Materials/MaterialParameterCollection.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "ScopedTransaction.h"
#include "CortexBatchScope.h"
#include "CortexCommandRouter.h"
#include "CortexSerializer.h"
#include "CortexGraphLayoutOps.h"
#include "UObject/UnrealType.h"

namespace
{
FString NormalizeMaterialPropertyToken(const FString& InName)
{
	FString Out = InName;
	Out = Out.Replace(TEXT("_"), TEXT(""));
	Out = Out.Replace(TEXT(" "), TEXT(""));
	Out = Out.ToLower();
	return Out;
}

FString MaterialPropertyToInputName(const EMaterialProperty Property)
{
	const UEnum* PropertyEnum = StaticEnum<EMaterialProperty>();
	if (PropertyEnum == nullptr)
	{
		return TEXT("");
	}

	FString EnumName = PropertyEnum->GetNameStringByValue(static_cast<int64>(Property));
	EnumName.RemoveFromStart(TEXT("MP_"));
	return EnumName;
}

bool ResolveMaterialPropertyByInputName(const FString& InputName, EMaterialProperty& OutProperty)
{
	const UEnum* PropertyEnum = StaticEnum<EMaterialProperty>();
	if (PropertyEnum == nullptr)
	{
		return false;
	}

	const FString NormalizedInput = NormalizeMaterialPropertyToken(InputName);
	for (int32 Prop = 0; Prop < MP_MAX; ++Prop)
	{
		const EMaterialProperty Property = static_cast<EMaterialProperty>(Prop);
		const FString EnumDerivedName = MaterialPropertyToInputName(Property);
		const FString NormalizedProperty = NormalizeMaterialPropertyToken(EnumDerivedName);
		if (NormalizedInput == NormalizedProperty)
		{
			OutProperty = Property;
			return true;
		}

		// Accept "CustomizedUV0" alias for "CustomizedUVs0".
		const FString CustomizedAlias = NormalizedProperty.Replace(TEXT("customizeduvs"), TEXT("customizeduv"));
		if (NormalizedInput == CustomizedAlias)
		{
			OutProperty = Property;
			return true;
		}
	}

	return false;
}
}

UMaterialExpression* FCortexMaterialGraphOps::FindExpression(UMaterial* Material, const FString& NodeId)
{
	if (Material == nullptr || !Material->GetEditorOnlyData())
	{
		return nullptr;
	}

	for (UMaterialExpression* Expression : Material->GetEditorOnlyData()->ExpressionCollection.Expressions)
	{
		if (Expression && Expression->GetName() == NodeId)
		{
			return Expression;
		}
	}

	return nullptr;
}

FCortexCommandResult FCortexMaterialGraphOps::ListNodes(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath;
	if (!Params.IsValid() || !Params->TryGetStringField(TEXT("asset_path"), AssetPath))
	{
		return FCortexCommandRouter::Error(
			CortexErrorCodes::InvalidField, TEXT("Missing required param: asset_path"));
	}

	FCortexCommandResult LoadError;
	UMaterial* Material = FCortexMaterialAssetOps::LoadMaterial(AssetPath, LoadError);
	if (Material == nullptr)
	{
		return LoadError;
	}

	TArray<TSharedPtr<FJsonValue>> NodesArray;

	if (Material->GetEditorOnlyData())
	{
		for (UMaterialExpression* Expression : Material->GetEditorOnlyData()->ExpressionCollection.Expressions)
		{
			if (Expression)
			{
				TSharedRef<FJsonObject> Entry = MakeShared<FJsonObject>();
				Entry->SetStringField(TEXT("node_id"), Expression->GetName());
				Entry->SetStringField(TEXT("expression_class"), Expression->GetClass()->GetName());

				TSharedPtr<FJsonObject> Position = MakeShared<FJsonObject>();
				Position->SetNumberField(TEXT("x"), Expression->MaterialExpressionEditorX);
				Position->SetNumberField(TEXT("y"), Expression->MaterialExpressionEditorY);
				Entry->SetObjectField(TEXT("position"), Position);

				NodesArray.Add(MakeShared<FJsonValueObject>(Entry));
			}
		}
	}

	// Add virtual MaterialResult node
	TSharedRef<FJsonObject> MaterialResultNode = MakeShared<FJsonObject>();
	MaterialResultNode->SetStringField(TEXT("node_id"), TEXT("MaterialResult"));
	MaterialResultNode->SetStringField(TEXT("expression_class"), TEXT("MaterialResult"));
	NodesArray.Add(MakeShared<FJsonValueObject>(MaterialResultNode));

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetArrayField(TEXT("nodes"), NodesArray);
	Data->SetNumberField(TEXT("count"), NodesArray.Num());

	return FCortexCommandRouter::Success(Data);
}

FCortexCommandResult FCortexMaterialGraphOps::GetNode(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath, NodeId;
	if (!Params.IsValid()
		|| !Params->TryGetStringField(TEXT("asset_path"), AssetPath)
		|| !Params->TryGetStringField(TEXT("node_id"), NodeId))
	{
		return FCortexCommandRouter::Error(
			CortexErrorCodes::InvalidField, TEXT("Missing required params: asset_path and node_id"));
	}

	FCortexCommandResult LoadError;
	UMaterial* Material = FCortexMaterialAssetOps::LoadMaterial(AssetPath, LoadError);
	if (Material == nullptr)
	{
		return LoadError;
	}

	UMaterialExpression* Expression = FindExpression(Material, NodeId);
	if (Expression == nullptr)
	{
		return FCortexCommandRouter::Error(
			CortexErrorCodes::NodeNotFound,
			FString::Printf(TEXT("Node not found: %s"), *NodeId)
		);
	}

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("node_id"), Expression->GetName());
	Data->SetStringField(TEXT("expression_class"), Expression->GetClass()->GetName());

	TSharedPtr<FJsonObject> Position = MakeShared<FJsonObject>();
	Position->SetNumberField(TEXT("x"), Expression->MaterialExpressionEditorX);
	Position->SetNumberField(TEXT("y"), Expression->MaterialExpressionEditorY);
	Data->SetObjectField(TEXT("position"), Position);

	TSharedPtr<FJsonObject> Properties = MakeShared<FJsonObject>();
	for (TFieldIterator<FProperty> It(Expression->GetClass()); It; ++It)
	{
		FProperty* Property = *It;
		if (!Property)
		{
			continue;
		}

		if (!Property->HasAnyPropertyFlags(CPF_Edit)
			|| Property->HasAnyPropertyFlags(CPF_Transient | CPF_Deprecated | CPF_DisableEditOnInstance))
		{
			continue;
		}

		const void* ValuePtr = Property->ContainerPtrToValuePtr<void>(Expression);
		if (!ValuePtr)
		{
			continue;
		}

		const TSharedPtr<FJsonValue> SerializedValue = FCortexSerializer::PropertyToJson(Property, ValuePtr);
		if (SerializedValue.IsValid())
		{
			Properties->SetField(Property->GetName(), SerializedValue);
		}
	}
	Data->SetObjectField(TEXT("properties"), Properties);

	if (const UMaterialExpressionCollectionParameter* CollectionParam =
		Cast<UMaterialExpressionCollectionParameter>(Expression))
	{
		if (CollectionParam->Collection)
		{
			Data->SetStringField(TEXT("collection_path"), CollectionParam->Collection->GetPathName());
			Data->SetStringField(TEXT("parameter_name"), CollectionParam->ParameterName.ToString());
		}
	}

	return FCortexCommandRouter::Success(Data);
}

FCortexCommandResult FCortexMaterialGraphOps::AddNode(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath, ExpressionClass;
	if (!Params.IsValid() || !Params->TryGetStringField(TEXT("asset_path"), AssetPath))
	{
		return FCortexCommandRouter::Error(
			CortexErrorCodes::InvalidField,
			TEXT("Missing required params: asset_path and expression_class"));
	}
	// Accept node_class as alias for expression_class
	if (!Params->TryGetStringField(TEXT("expression_class"), ExpressionClass))
	{
		if (!Params->TryGetStringField(TEXT("node_class"), ExpressionClass))
		{
			return FCortexCommandRouter::Error(
				CortexErrorCodes::InvalidField,
				TEXT("Missing required params: asset_path and expression_class (alias: node_class)"));
		}
	}

	FCortexCommandResult LoadError;
	UMaterial* Material = FCortexMaterialAssetOps::LoadMaterial(AssetPath, LoadError);
	if (Material == nullptr)
	{
		return LoadError;
	}

	if (!Material->GetEditorOnlyData())
	{
		return FCortexCommandRouter::Error(
			CortexErrorCodes::SerializationError,
			TEXT("Material has no editor data"));
	}

	// Find expression class — accept short names (e.g. "VectorParameter" → "MaterialExpressionVectorParameter")
	UClass* ExpClass = FindFirstObject<UClass>(*ExpressionClass, EFindFirstObjectOptions::NativeFirst);
	if (ExpClass == nullptr || !ExpClass->IsChildOf(UMaterialExpression::StaticClass()))
	{
		const FString PrefixedName = TEXT("MaterialExpression") + ExpressionClass;
		ExpClass = FindFirstObject<UClass>(*PrefixedName, EFindFirstObjectOptions::NativeFirst);
	}
	if (ExpClass == nullptr || !ExpClass->IsChildOf(UMaterialExpression::StaticClass()))
	{
		return FCortexCommandRouter::Error(
			CortexErrorCodes::InvalidField,
			FString::Printf(TEXT("Invalid expression class: %s"), *ExpressionClass));
	}

	if (!FCortexCommandRouter::IsInBatch())
	{
		FScopedTransaction Transaction(FText::FromString(
			FString::Printf(TEXT("Cortex: Add Material Expression Node"))
		));
		Material->PreEditChange(nullptr);
	}

	UMaterialExpression* NewExpression = NewObject<UMaterialExpression>(Material, ExpClass);
	NewExpression->bCollapsed = false;

	// Set position if provided
	const TSharedPtr<FJsonObject>* PositionObj = nullptr;
	if (Params->TryGetObjectField(TEXT("position"), PositionObj) && (*PositionObj).IsValid())
	{
		double X = 0, Y = 0;
		(*PositionObj)->TryGetNumberField(TEXT("x"), X);
		(*PositionObj)->TryGetNumberField(TEXT("y"), Y);
		NewExpression->MaterialExpressionEditorX = static_cast<int32>(X);
		NewExpression->MaterialExpressionEditorY = static_cast<int32>(Y);
	}

	Material->GetEditorOnlyData()->ExpressionCollection.Expressions.Add(NewExpression);

	if (!FCortexCommandRouter::IsInBatch())
	{
		Material->PostEditChange();
	}
	else
	{
		FCortexBatchScope::MarkMaterialDirty(Material);
	}
	Material->MarkPackageDirty();

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("node_id"), NewExpression->GetName());
	Data->SetStringField(TEXT("expression_class"), NewExpression->GetClass()->GetName());
	Data->SetStringField(TEXT("asset_path"), AssetPath);

	return FCortexCommandRouter::Success(Data);
}

FCortexCommandResult FCortexMaterialGraphOps::RemoveNode(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath, NodeId;
	if (!Params.IsValid()
		|| !Params->TryGetStringField(TEXT("asset_path"), AssetPath)
		|| !Params->TryGetStringField(TEXT("node_id"), NodeId))
	{
		return FCortexCommandRouter::Error(
			CortexErrorCodes::InvalidField,
			TEXT("Missing required params: asset_path and node_id"));
	}

	FCortexCommandResult LoadError;
	UMaterial* Material = FCortexMaterialAssetOps::LoadMaterial(AssetPath, LoadError);
	if (Material == nullptr)
	{
		return LoadError;
	}

	if (!Material->GetEditorOnlyData())
	{
		return FCortexCommandRouter::Error(
			CortexErrorCodes::SerializationError,
			TEXT("Material has no editor data"));
	}

	UMaterialExpression* Expression = FindExpression(Material, NodeId);
	if (Expression == nullptr)
	{
		return FCortexCommandRouter::Error(
			CortexErrorCodes::NodeNotFound,
			FString::Printf(TEXT("Node not found: %s"), *NodeId)
		);
	}

	if (!FCortexCommandRouter::IsInBatch())
	{
		FScopedTransaction Transaction(FText::FromString(
			FString::Printf(TEXT("Cortex: Remove Material Expression Node %s"), *NodeId)
		));
		Material->PreEditChange(nullptr);
	}

	Material->GetEditorOnlyData()->ExpressionCollection.Expressions.Remove(Expression);

	if (!FCortexCommandRouter::IsInBatch())
	{
		Material->PostEditChange();
	}
	else
	{
		FCortexBatchScope::MarkMaterialDirty(Material);
	}
	Material->MarkPackageDirty();

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("node_id"), NodeId);
	Data->SetBoolField(TEXT("removed"), true);

	return FCortexCommandRouter::Success(Data);
}

FCortexCommandResult FCortexMaterialGraphOps::ListConnections(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath;
	if (!Params.IsValid() || !Params->TryGetStringField(TEXT("asset_path"), AssetPath))
	{
		return FCortexCommandRouter::Error(
			CortexErrorCodes::InvalidField, TEXT("Missing required param: asset_path"));
	}

	FCortexCommandResult LoadError;
	UMaterial* Material = FCortexMaterialAssetOps::LoadMaterial(AssetPath, LoadError);
	if (Material == nullptr)
	{
		return LoadError;
	}

	if (!Material->GetEditorOnlyData())
	{
		return FCortexCommandRouter::Error(
			CortexErrorCodes::SerializationError,
			TEXT("Material has no editor data"));
	}

	TArray<TSharedPtr<FJsonValue>> ConnectionsArray;

	UMaterialEditorOnlyData* EditorData = Material->GetEditorOnlyData();

	// Enumerate all MaterialResult inputs via property API.
	for (int32 Prop = 0; Prop < MP_MAX; ++Prop)
	{
		const EMaterialProperty Property = static_cast<EMaterialProperty>(Prop);
		FExpressionInput* Input = Material->GetExpressionInputForProperty(Property);
		if (Input && Input->Expression != nullptr)
		{
			TSharedRef<FJsonObject> Entry = MakeShared<FJsonObject>();
			Entry->SetStringField(TEXT("source_node"), Input->Expression->GetName());
			Entry->SetNumberField(TEXT("source_output"), Input->OutputIndex);
			Entry->SetStringField(TEXT("target_node"), TEXT("MaterialResult"));
			Entry->SetStringField(TEXT("target_input"), MaterialPropertyToInputName(Property));
			ConnectionsArray.Add(MakeShared<FJsonValueObject>(Entry));
		}
	}

	// Check expression-to-expression connections
	for (UMaterialExpression* Expression : EditorData->ExpressionCollection.Expressions)
	{
		if (Expression == nullptr)
		{
			continue;
		}

		for (FExpressionInputIterator It(Expression); It; ++It)
		{
			if (It.Input && It.Input->Expression)
			{
				TSharedRef<FJsonObject> Entry = MakeShared<FJsonObject>();
				Entry->SetStringField(TEXT("source_node"), It.Input->Expression->GetName());
				Entry->SetNumberField(TEXT("source_output"), It.Input->OutputIndex);
				Entry->SetStringField(TEXT("target_node"), Expression->GetName());
				Entry->SetStringField(TEXT("target_input"), Expression->GetInputName(It.Index).ToString());
				ConnectionsArray.Add(MakeShared<FJsonValueObject>(Entry));
			}
		}
	}

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetArrayField(TEXT("connections"), ConnectionsArray);
	Data->SetNumberField(TEXT("count"), ConnectionsArray.Num());

	return FCortexCommandRouter::Success(Data);
}

FCortexCommandResult FCortexMaterialGraphOps::Connect(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath, SourceNode, TargetNode, TargetInput;

	if (!Params.IsValid() || !Params->TryGetStringField(TEXT("asset_path"), AssetPath))
	{
		return FCortexCommandRouter::Error(
			CortexErrorCodes::InvalidField,
			TEXT("Missing required params: asset_path, source_node, source_output, target_node, target_input"));
	}
	// Accept from_node/to_node as aliases
	if (!Params->TryGetStringField(TEXT("source_node"), SourceNode))
		Params->TryGetStringField(TEXT("from_node"), SourceNode);
	if (!Params->TryGetStringField(TEXT("target_node"), TargetNode))
		Params->TryGetStringField(TEXT("to_node"), TargetNode);
	if (!Params->TryGetStringField(TEXT("target_input"), TargetInput))
		Params->TryGetStringField(TEXT("to_input"), TargetInput);

	if (SourceNode.IsEmpty() || TargetNode.IsEmpty() || TargetInput.IsEmpty())
	{
		return FCortexCommandRouter::Error(
			CortexErrorCodes::InvalidField,
			TEXT("Missing required params: asset_path, source_node, source_output, target_node, target_input"));
	}

	// Parse source_output: try string first (name-based), then number (index-based)
	FString SourceOutputStr;
	double SourceOutputDouble = 0;
	bool bSourceOutputIsString = Params->TryGetStringField(TEXT("source_output"), SourceOutputStr);
	bool bSourceOutputIsNumber = Params->TryGetNumberField(TEXT("source_output"), SourceOutputDouble);

	if (!bSourceOutputIsString && !bSourceOutputIsNumber)
	{
		return FCortexCommandRouter::Error(
			CortexErrorCodes::InvalidField,
			TEXT("Missing required param: source_output (string or number)"));
	}

	FCortexCommandResult LoadError;
	UMaterial* Material = FCortexMaterialAssetOps::LoadMaterial(AssetPath, LoadError);
	if (Material == nullptr)
	{
		return LoadError;
	}

	if (!Material->GetEditorOnlyData())
	{
		return FCortexCommandRouter::Error(
			CortexErrorCodes::SerializationError,
			TEXT("Material has no editor data"));
	}

	UMaterialExpression* SourceExpr = FindExpression(Material, SourceNode);
	if (SourceExpr == nullptr)
	{
		return FCortexCommandRouter::Error(
			CortexErrorCodes::NodeNotFound,
			FString::Printf(TEXT("Source node not found: %s"), *SourceNode)
		);
	}

	// Resolve source output index
	int32 SourceOutputIndex = 0;
	if (bSourceOutputIsString)
	{
		// Match by output name or index string
		const TArray<FExpressionOutput>& Outputs = SourceExpr->GetOutputs();
		bool bFound = false;
		for (int32 i = 0; i < Outputs.Num(); ++i)
		{
			FString OutputName = Outputs[i].OutputName.ToString();
			if (OutputName == SourceOutputStr || FString::Printf(TEXT("%d"), i) == SourceOutputStr)
			{
				SourceOutputIndex = i;
				bFound = true;
				break;
			}
		}
		if (!bFound)
		{
			return FCortexCommandRouter::Error(
				CortexErrorCodes::InvalidField,
				FString::Printf(TEXT("Source output not found: %s"), *SourceOutputStr)
			);
		}
	}
	else
	{
		SourceOutputIndex = static_cast<int32>(SourceOutputDouble);
	}

	if (!FCortexCommandRouter::IsInBatch())
	{
		FScopedTransaction Transaction(FText::FromString(
			FString::Printf(TEXT("Cortex: Connect Material Nodes"))
		));
		Material->PreEditChange(nullptr);
	}

	// Connect to MaterialResult or expression — accept "Material" as alias
	if (TargetNode == TEXT("MaterialResult") || TargetNode == TEXT("Material"))
	{
		EMaterialProperty Property = MP_MAX;
		if (!ResolveMaterialPropertyByInputName(TargetInput, Property))
		{
			return FCortexCommandRouter::Error(
				CortexErrorCodes::InvalidConnection,
				FString::Printf(TEXT("Unknown MaterialResult input: %s"), *TargetInput)
			);
		}

		if (!Material->IsPropertySupported(Property))
		{
			return FCortexCommandRouter::Error(
				CortexErrorCodes::InvalidConnection,
				FString::Printf(TEXT("MaterialResult input not supported: %s"), *TargetInput)
			);
		}

		FExpressionInput* Input = Material->GetExpressionInputForProperty(Property);
		if (Input == nullptr)
		{
			return FCortexCommandRouter::Error(
				CortexErrorCodes::InvalidConnection,
				FString::Printf(TEXT("MaterialResult input unavailable: %s"), *TargetInput)
			);
		}

		Input->Expression = SourceExpr;
		Input->OutputIndex = SourceOutputIndex;
	}
	else
	{
		// Expression-to-expression connection
		UMaterialExpression* TargetExpr = FindExpression(Material, TargetNode);
		if (TargetExpr == nullptr)
		{
			return FCortexCommandRouter::Error(
				CortexErrorCodes::NodeNotFound,
				FString::Printf(TEXT("Target node not found: %s"), *TargetNode)
			);
		}

		// Find target input by name
		FExpressionInput* TargetInputPtr = nullptr;
		for (FExpressionInputIterator It(TargetExpr); It; ++It)
		{
			FString InputName = TargetExpr->GetInputName(It.Index).ToString();
			if (InputName == TargetInput)
			{
				TargetInputPtr = It.Input;
				break;
			}
		}

		if (TargetInputPtr == nullptr)
		{
			return FCortexCommandRouter::Error(
				CortexErrorCodes::InvalidConnection,
				FString::Printf(TEXT("Target input not found: %s on node %s"), *TargetInput, *TargetNode)
			);
		}

		TargetInputPtr->Expression = SourceExpr;
		TargetInputPtr->OutputIndex = SourceOutputIndex;
	}

	if (!FCortexCommandRouter::IsInBatch())
	{
		Material->PostEditChange();
	}
	else
	{
		FCortexBatchScope::MarkMaterialDirty(Material);
	}
	Material->MarkPackageDirty();

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("source_node"), SourceNode);
	Data->SetNumberField(TEXT("source_output"), SourceOutputIndex);
	Data->SetStringField(TEXT("target_node"), TargetNode);
	Data->SetStringField(TEXT("target_input"), TargetInput);
	Data->SetBoolField(TEXT("connected"), true);

	return FCortexCommandRouter::Success(Data);
}

FCortexCommandResult FCortexMaterialGraphOps::Disconnect(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath, TargetNode, TargetInput;

	if (!Params.IsValid()
		|| !Params->TryGetStringField(TEXT("asset_path"), AssetPath)
		|| !Params->TryGetStringField(TEXT("target_node"), TargetNode)
		|| !Params->TryGetStringField(TEXT("target_input"), TargetInput))
	{
		return FCortexCommandRouter::Error(
			CortexErrorCodes::InvalidField,
			TEXT("Missing required params: asset_path, target_node, target_input"));
	}

	FCortexCommandResult LoadError;
	UMaterial* Material = FCortexMaterialAssetOps::LoadMaterial(AssetPath, LoadError);
	if (Material == nullptr)
	{
		return LoadError;
	}

	if (!Material->GetEditorOnlyData())
	{
		return FCortexCommandRouter::Error(
			CortexErrorCodes::SerializationError,
			TEXT("Material has no editor data"));
	}

	if (!FCortexCommandRouter::IsInBatch())
	{
		FScopedTransaction Transaction(FText::FromString(
			FString::Printf(TEXT("Cortex: Disconnect Material Nodes"))
		));
		Material->PreEditChange(nullptr);
	}

	// Disconnect from MaterialResult
	if (TargetNode == TEXT("MaterialResult"))
	{
		EMaterialProperty Property = MP_MAX;
		if (!ResolveMaterialPropertyByInputName(TargetInput, Property))
		{
			return FCortexCommandRouter::Error(
				CortexErrorCodes::InvalidConnection,
				FString::Printf(TEXT("Unknown MaterialResult input: %s"), *TargetInput)
			);
		}

		FExpressionInput* Input = Material->GetExpressionInputForProperty(Property);
		if (Input == nullptr)
		{
			return FCortexCommandRouter::Error(
				CortexErrorCodes::InvalidConnection,
				FString::Printf(TEXT("MaterialResult input unavailable: %s"), *TargetInput)
			);
		}

		Input->Expression = nullptr;
		Input->OutputIndex = 0;
	}
	else
	{
		return FCortexCommandRouter::Error(
			CortexErrorCodes::InvalidConnection,
			TEXT("Expression-to-expression disconnections not yet implemented")
		);
	}

	if (!FCortexCommandRouter::IsInBatch())
	{
		Material->PostEditChange();
	}
	else
	{
		FCortexBatchScope::MarkMaterialDirty(Material);
	}
	Material->MarkPackageDirty();

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("target_node"), TargetNode);
	Data->SetStringField(TEXT("target_input"), TargetInput);
	Data->SetBoolField(TEXT("disconnected"), true);

	return FCortexCommandRouter::Success(Data);
}

FCortexCommandResult FCortexMaterialGraphOps::AutoLayout(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath;
	if (!Params.IsValid() || !Params->TryGetStringField(TEXT("asset_path"), AssetPath))
	{
		return FCortexCommandRouter::Error(
			CortexErrorCodes::InvalidField, TEXT("Missing required param: asset_path"));
	}

	FCortexCommandResult LoadError;
	UMaterial* Material = FCortexMaterialAssetOps::LoadMaterial(AssetPath, LoadError);
	if (Material == nullptr) return LoadError;

	if (!Material->GetEditorOnlyData())
	{
		return FCortexCommandRouter::Error(
			CortexErrorCodes::SerializationError, TEXT("Material has no editor data"));
	}

	const TArray<UMaterialExpression*>& Expressions = Material->GetEditorOnlyData()->ExpressionCollection.Expressions;

	if (Expressions.Num() == 0)
	{
		TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
		Data->SetStringField(TEXT("asset_path"), AssetPath);
		Data->SetNumberField(TEXT("node_count"), 0);
		return FCortexCommandRouter::Success(Data);
	}

	// Convert Material expressions to abstract layout nodes
	TMap<UMaterialExpression*, FString> ExprToId;

	for (UMaterialExpression* Expr : Expressions)
	{
		if (!Expr) continue;
		ExprToId.Add(Expr, Expr->GetPathName());
	}

	// Enumerate all material property inputs via UE API.
	// Replaces hardcoded 8-input check to include all EMaterialProperty values.
	TSet<FString> MaterialResultInputIds;
	for (int32 Prop = 0; Prop < MP_MAX; ++Prop)
	{
		FExpressionInput* Input = Material->GetExpressionInputForProperty(
			static_cast<EMaterialProperty>(Prop));
		if (Input && Input->Expression)
		{
			const FString* Id = ExprToId.Find(Input->Expression);
			if (Id)
			{
				MaterialResultInputIds.Add(*Id);
			}
		}
	}

	// Build forward adjacency map in single O(N*M) pass
	TMap<FString, TArray<FString>> ForwardEdges;
	for (UMaterialExpression* Expr : Expressions)
	{
		if (!Expr) continue;
		for (FExpressionInputIterator It(Expr); It; ++It)
		{
			if (It.Input && It.Input->Expression)
			{
				const FString* SourceId = ExprToId.Find(It.Input->Expression);
				if (SourceId)
					ForwardEdges.FindOrAdd(*SourceId).AddUnique(ExprToId[Expr]);
			}
		}
	}

	// Wire MaterialResult-feeding expressions to a virtual sink node.
	// Without this, expressions that only connect to MaterialResult (not to other expressions)
	// have empty DataOutputs and collapse to layer 0, making all nodes overlap at x=0.
	const FString MaterialResultId = TEXT("__MaterialResult__");
	for (const FString& Id : MaterialResultInputIds)
	{
		ForwardEdges.FindOrAdd(Id).AddUnique(MaterialResultId);
	}

	// Build layout nodes with connectivity
	TArray<FCortexLayoutNode> LayoutNodes;
	for (UMaterialExpression* Expr : Expressions)
	{
		if (!Expr) continue;
		FCortexLayoutNode LayoutNode;
		LayoutNode.Id = ExprToId[Expr];
		// Mark MaterialResult-feeding nodes as entry points. With RightToLeft direction,
		// layer inversion places entry points rightmost — matching MaterialResult's visual position.
		LayoutNode.bIsEntryPoint = MaterialResultInputIds.Contains(LayoutNode.Id);
		LayoutNode.bIsExecNode = false; // Material expressions are always pure data nodes.
		const int32 BaseWidth = FMath::Max(Expr->GetWidth(), CortexMaterialLayout::MinNodeWidth);
		LayoutNode.Width = BaseWidth
			+ (Expr->UsesLeftGutter() ? 32 : 0)
			+ (Expr->UsesRightGutter() ? 32 : 0)
			+ CortexMaterialLayout::NodeChromePaddingX;

		const int32 BaseHeight = FMath::Max(Expr->GetHeight(), CortexMaterialLayout::MinNodeHeight);
		const int32 PreviewAddition =
			(!Expr->bHidePreviewWindow && !Expr->bCollapsed)
			? CortexMaterialLayout::PreviewHeight : 0;
		LayoutNode.Height = CortexMaterialLayout::TitleBarHeight + BaseHeight + PreviewAddition;
		const TArray<FString>* Outputs = ForwardEdges.Find(LayoutNode.Id);
		if (Outputs) LayoutNode.DataOutputs = *Outputs;
		LayoutNodes.Add(LayoutNode);
	}

	// Add virtual MaterialResult sink — gives the layout engine a concrete rightmost anchor.
	// Sized to match visible Material Output pins for more accurate spacing.
	// Removed from positions after layout; not written back to any expression.
	{
		int32 VisiblePinCount = 0;
		for (int32 Prop = 0; Prop < MP_MAX; ++Prop)
		{
			const EMaterialProperty Property = static_cast<EMaterialProperty>(Prop);
			if (Material->GetExpressionInputForProperty(Property)
				&& Material->IsPropertySupported(Property))
			{
				++VisiblePinCount;
			}
		}

		FCortexLayoutNode MaterialResultNode;
		MaterialResultNode.Id = MaterialResultId;
		MaterialResultNode.Width = CortexMaterialLayout::MaterialResultWidth;
		MaterialResultNode.Height = CortexMaterialLayout::TitleBarHeight
			+ FMath::Max(1, VisiblePinCount) * CortexMaterialLayout::PinRowHeight;
		MaterialResultNode.bIsEntryPoint = false;
		MaterialResultNode.bIsExecNode = false;
		LayoutNodes.Add(MaterialResultNode);
	}

	// Run shared layout engine with material-appropriate defaults.
	FCortexLayoutConfig Config;
	Config.Direction = ECortexLayoutDirection::RightToLeft;
	Config.HorizontalSpacing = CortexMaterialLayout::DefaultHorizontalSpacing;
	Config.VerticalSpacing = CortexMaterialLayout::DefaultVerticalSpacing;
	double HSpacingVal = 0, VSpacingVal = 0;
	if (Params->TryGetNumberField(TEXT("horizontal_spacing"), HSpacingVal) && HSpacingVal > 0)
		Config.HorizontalSpacing = static_cast<int32>(HSpacingVal);
	if (Params->TryGetNumberField(TEXT("vertical_spacing"), VSpacingVal) && VSpacingVal > 0)
		Config.VerticalSpacing = static_cast<int32>(VSpacingVal);

	FCortexLayoutResult LayoutResult = FCortexGraphLayoutOps::CalculateLayout(LayoutNodes, Config);

	// Translate positions so MaterialResult lands at x=0.
	// UE material editor convention: MaterialResult is at ~x=0; expressions are at negative x (to the left).
	if (const FIntPoint* MRPos = LayoutResult.Positions.Find(MaterialResultId))
	{
		const int32 OffsetX = MRPos->X;
		for (auto& Pair : LayoutResult.Positions)
		{
			Pair.Value.X -= OffsetX;
		}
		LayoutResult.Positions.Remove(MaterialResultId);
	}

	// Apply positions back to Material expressions
	TUniquePtr<FScopedTransaction> Transaction;
	if (!FCortexCommandRouter::IsInBatch())
	{
		Transaction = MakeUnique<FScopedTransaction>(
			FText::FromString(TEXT("Cortex: Auto-Layout Material Graph")));
		Material->PreEditChange(nullptr);
	}

	TMap<FString, UMaterialExpression*> IdToExpr;
	for (UMaterialExpression* Expr : Expressions)
	{
		if (Expr) IdToExpr.Add(Expr->GetPathName(), Expr);
	}

	for (const auto& Pair : LayoutResult.Positions)
	{
		UMaterialExpression** ExprPtr = IdToExpr.Find(Pair.Key);
		if (ExprPtr && *ExprPtr)
		{
			(*ExprPtr)->MaterialExpressionEditorX = Pair.Value.X;
			(*ExprPtr)->MaterialExpressionEditorY = Pair.Value.Y;
		}
	}

	if (!FCortexCommandRouter::IsInBatch())
	{
		Material->PostEditChange();
	}
	else
	{
		FCortexBatchScope::MarkMaterialDirty(Material);
	}
	Material->MarkPackageDirty();

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("asset_path"), AssetPath);
	Data->SetNumberField(TEXT("node_count"), Expressions.Num());
	return FCortexCommandRouter::Success(Data);
}

FCortexCommandResult FCortexMaterialGraphOps::SetNodeProperty(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath, NodeId, PropertyName;
	if (!Params.IsValid()
		|| !Params->TryGetStringField(TEXT("asset_path"), AssetPath)
		|| !Params->TryGetStringField(TEXT("node_id"), NodeId))
	{
		return FCortexCommandRouter::Error(
			CortexErrorCodes::InvalidField,
			TEXT("Missing required params: asset_path, node_id, property_name"));
	}
	// Accept "property" as alias for "property_name"
	if (!Params->TryGetStringField(TEXT("property_name"), PropertyName))
	{
		if (!Params->TryGetStringField(TEXT("property"), PropertyName))
		{
			return FCortexCommandRouter::Error(
				CortexErrorCodes::InvalidField,
				TEXT("Missing required param: property_name (or property)"));
		}
	}

	FCortexCommandResult LoadError;
	UMaterial* Material = FCortexMaterialAssetOps::LoadMaterial(AssetPath, LoadError);
	if (Material == nullptr)
	{
		return LoadError;
	}

	if (!Material->GetEditorOnlyData())
	{
		return FCortexCommandRouter::Error(
			CortexErrorCodes::SerializationError,
			TEXT("Material has no editor data"));
	}

	UMaterialExpression* Expression = FindExpression(Material, NodeId);
	if (Expression == nullptr)
	{
		return FCortexCommandRouter::Error(
			CortexErrorCodes::NodeNotFound,
			FString::Printf(TEXT("Node not found: %s"), *NodeId)
		);
	}

	// Find property by name
	FProperty* Property = Expression->GetClass()->FindPropertyByName(FName(*PropertyName));
	if (Property == nullptr)
	{
		return FCortexCommandRouter::Error(
			CortexErrorCodes::InvalidField,
			FString::Printf(TEXT("Property not found: %s"), *PropertyName)
		);
	}

	// Get value from JSON params
	TSharedPtr<FJsonValue> Value = Params->TryGetField(TEXT("value"));
	if (!Value.IsValid())
	{
		return FCortexCommandRouter::Error(
			CortexErrorCodes::InvalidField,
			TEXT("Missing required param: value"));
	}

	if (!FCortexCommandRouter::IsInBatch())
	{
		FScopedTransaction Transaction(FText::FromString(
			FString::Printf(TEXT("Cortex: Set Node Property %s"), *PropertyName)
		));
		Material->PreEditChange(nullptr);
	}

	void* PropertyAddress = Property->ContainerPtrToValuePtr<void>(Expression);

	TArray<FString> Warnings;
	if (!FCortexSerializer::JsonToProperty(Value, Property, PropertyAddress, Expression, Warnings))
	{
		FString WarningStr = Warnings.Num() > 0
			? FString::Join(Warnings, TEXT("; "))
			: TEXT("unsupported type");
		return FCortexCommandRouter::Error(
			CortexErrorCodes::InvalidField,
			FString::Printf(TEXT("Failed to set property '%s': %s"), *PropertyName, *WarningStr));
	}

	if (!FCortexCommandRouter::IsInBatch())
	{
		Material->PostEditChange();
	}
	else
	{
		FCortexBatchScope::MarkMaterialDirty(Material);
	}
	Material->MarkPackageDirty();

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("node_id"), NodeId);
	Data->SetStringField(TEXT("property_name"), PropertyName);
	Data->SetBoolField(TEXT("updated"), true);

	return FCortexCommandRouter::Success(Data);
}

FCortexCommandResult FCortexMaterialGraphOps::GetNodePins(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath, NodeId;
	if (!Params.IsValid()
		|| !Params->TryGetStringField(TEXT("asset_path"), AssetPath)
		|| !Params->TryGetStringField(TEXT("node_id"), NodeId))
	{
		return FCortexCommandRouter::Error(
			CortexErrorCodes::InvalidField,
			TEXT("Missing required params: asset_path and node_id"));
	}

	FCortexCommandResult LoadError;
	UMaterial* Material = FCortexMaterialAssetOps::LoadMaterial(AssetPath, LoadError);
	if (Material == nullptr)
	{
		return LoadError;
	}

	UMaterialExpression* Expression = FindExpression(Material, NodeId);
	if (Expression == nullptr)
	{
		return FCortexCommandRouter::Error(
			CortexErrorCodes::NodeNotFound,
			FString::Printf(TEXT("Node not found: %s"), *NodeId)
		);
	}

	// Enumerate output pins
	TArray<TSharedPtr<FJsonValue>> OutputsArray;
	const TArray<FExpressionOutput>& Outputs = Expression->GetOutputs();
	for (int32 i = 0; i < Outputs.Num(); ++i)
	{
		TSharedRef<FJsonObject> Entry = MakeShared<FJsonObject>();
		Entry->SetNumberField(TEXT("index"), i);
		FString OutputName = Outputs[i].OutputName.ToString();
		Entry->SetStringField(TEXT("name"), OutputName.IsEmpty() ? FString::Printf(TEXT("%d"), i) : OutputName);
		OutputsArray.Add(MakeShared<FJsonValueObject>(Entry));
	}

	// Enumerate input pins
	TArray<TSharedPtr<FJsonValue>> InputsArray;
	for (FExpressionInputIterator It(Expression); It; ++It)
	{
		TSharedRef<FJsonObject> Entry = MakeShared<FJsonObject>();
		Entry->SetNumberField(TEXT("index"), It.Index);
		Entry->SetStringField(TEXT("name"), Expression->GetInputName(It.Index).ToString());
		// Report if this input is currently connected
		if (It.Input && It.Input->Expression)
		{
			Entry->SetStringField(TEXT("connected_to"), It.Input->Expression->GetName());
		}
		InputsArray.Add(MakeShared<FJsonValueObject>(Entry));
	}

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("node_id"), NodeId);
	Data->SetStringField(TEXT("expression_class"), Expression->GetClass()->GetName());
	Data->SetArrayField(TEXT("inputs"), InputsArray);
	Data->SetArrayField(TEXT("outputs"), OutputsArray);
	Data->SetNumberField(TEXT("input_count"), InputsArray.Num());
	Data->SetNumberField(TEXT("output_count"), OutputsArray.Num());

	return FCortexCommandRouter::Success(Data);
}
