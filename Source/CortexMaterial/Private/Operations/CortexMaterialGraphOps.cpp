#include "Operations/CortexMaterialGraphOps.h"
#include "Operations/CortexMaterialAssetOps.h"
#include "Operations/CortexMaterialInputUtils.h"
#include "CortexMaterialModule.h"
#include "CortexPropertyUtils.h"
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

	// Optional editable-property selection (token-efficiency TE05). Absent/empty keeps the
	// current full read of every editable property; malformed selectors refuse instead of being
	// ignored and unknown/unreadable names refuse instead of falling back to the full read.
	TArray<FString> SelectedProperties;
	if (Params->HasField(TEXT("properties")))
	{
		const TArray<TSharedPtr<FJsonValue>>* PropertiesArray = nullptr;
		if (!Params->TryGetArrayField(TEXT("properties"), PropertiesArray) || PropertiesArray == nullptr)
		{
			return FCortexCommandRouter::Error(
				CortexErrorCodes::InvalidField,
				TEXT("'properties' must be an array of editable property names"));
		}

		SelectedProperties.Reserve(PropertiesArray->Num());
		for (const TSharedPtr<FJsonValue>& PropertyValue : *PropertiesArray)
		{
			if (!PropertyValue.IsValid() || PropertyValue->Type != EJson::String || PropertyValue->AsString().IsEmpty())
			{
				return FCortexCommandRouter::Error(
					CortexErrorCodes::InvalidField,
					TEXT("'properties' entries must be non-empty property names"));
			}
			SelectedProperties.Add(PropertyValue->AsString());
		}
	}

	if (SelectedProperties.Num() == 0)
	{
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
	}
	else
	{
		for (const FString& PropertyName : SelectedProperties)
		{
			// A dotted selector must not cross a root the full read excludes: validate the root
			// segment on the expression with the SAME editable admission before traversing, so a
			// non-editable root (for example the expression's plain `Material` pointer,
			// MaterialExpression.h:179-184) cannot expose the owning material's editable fields
			// under this key. The terminal admission below is retained for the resolved leaf.
			int32 RootDotIndex = INDEX_NONE;
			const FString RootName = PropertyName.FindChar(TEXT('.'), RootDotIndex)
				? PropertyName.Left(RootDotIndex)
				: PropertyName;
			FProperty* RootProperty = Expression->GetClass()->FindPropertyByName(FName(*RootName));
			if (RootProperty == nullptr
				|| !RootProperty->HasAnyPropertyFlags(CPF_Edit)
				|| RootProperty->HasAnyPropertyFlags(CPF_Transient | CPF_Deprecated | CPF_DisableEditOnInstance))
			{
				return FCortexCommandRouter::Error(
					CortexErrorCodes::PropertyNotFound,
					FString::Printf(TEXT("Editable node property not found: %s"), *PropertyName));
			}

			FProperty* Property = nullptr;
			void* ValuePtr = nullptr;
			const bool bResolved = FCortexPropertyUtils::ResolvePropertyPath(
				Expression, PropertyName, Property, ValuePtr);
			if (!bResolved
				|| Property == nullptr
				|| ValuePtr == nullptr
				|| !Property->HasAnyPropertyFlags(CPF_Edit)
				|| Property->HasAnyPropertyFlags(CPF_Transient | CPF_Deprecated | CPF_DisableEditOnInstance))
			{
				return FCortexCommandRouter::Error(
					CortexErrorCodes::PropertyNotFound,
					FString::Printf(TEXT("Editable node property not found: %s"), *PropertyName));
			}

			const TSharedPtr<FJsonValue> SerializedValue = FCortexSerializer::PropertyToJson(Property, ValuePtr);
			if (SerializedValue.IsValid())
			{
				Properties->SetField(PropertyName, SerializedValue);
			}
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
			Entry->SetStringField(TEXT("target_input"), CortexMaterialInputUtils::InputName(Property));
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
	bool bIncludeInputState = false;
	if (Params->HasField(TEXT("include_input_state")) && !Params->TryGetBoolField(TEXT("include_input_state"), bIncludeInputState))
	{
		return FCortexCommandRouter::Error(CortexErrorCodes::InvalidField, TEXT("include_input_state must be a boolean"));
	}
	if (bIncludeInputState)
	{
		Data->SetArrayField(TEXT("root_inputs"), CortexMaterialInputUtils::SerializeInputs(Material));
	}

	return FCortexCommandRouter::Success(Data);
}

FCortexCommandResult FCortexMaterialGraphOps::Connect(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath, SourceNode, TargetNode, TargetInput;
	if (!Params.IsValid() || !Params->TryGetStringField(TEXT("asset_path"), AssetPath))
	{
		return FCortexCommandRouter::Error(CortexErrorCodes::InvalidField, TEXT("Missing required param: asset_path"));
	}
	if (!Params->TryGetStringField(TEXT("source_node"), SourceNode))
	{
		Params->TryGetStringField(TEXT("from_node"), SourceNode);
	}
	if (!Params->TryGetStringField(TEXT("target_node"), TargetNode))
	{
		Params->TryGetStringField(TEXT("to_node"), TargetNode);
	}
	if (!Params->TryGetStringField(TEXT("target_input"), TargetInput))
	{
		Params->TryGetStringField(TEXT("to_input"), TargetInput);
	}
	if (SourceNode.IsEmpty() || TargetNode.IsEmpty() || TargetInput.IsEmpty())
	{
		return FCortexCommandRouter::Error(CortexErrorCodes::InvalidField, TEXT("Missing source_node, target_node or target_input"));
	}
	FCortexCommandResult LoadError;
	UMaterial* Material = FCortexMaterialAssetOps::LoadMaterial(AssetPath, LoadError);
	if (!Material)
	{
		return LoadError;
	}
	UMaterialExpression* Source = FindExpression(Material, SourceNode);
	if (!Source)
	{
		return FCortexCommandRouter::Error(CortexErrorCodes::NodeNotFound, TEXT("Source material node not found"));
	}
	const auto& Outputs = Source->GetOutputs();
	int32 OutputIndex = INDEX_NONE;
	FString OutputName;
	double OutputNumber;
	if (Params->TryGetStringField(TEXT("source_output"), OutputName))
	{
		for (int32 Index = 0; Index < Outputs.Num(); ++Index)
		{
			if (Outputs[Index].OutputName.ToString() == OutputName || FString::FromInt(Index) == OutputName)
			{
				OutputIndex = Index;
				break;
			}
		}
	}
	else if (Params->TryGetNumberField(TEXT("source_output"), OutputNumber)
		&& FMath::IsFinite(OutputNumber) && OutputNumber >= 0 && OutputNumber <= MAX_int32
		&& OutputNumber == FMath::FloorToDouble(OutputNumber))
	{
		OutputIndex = static_cast<int32>(OutputNumber);
	}
	if (!Outputs.IsValidIndex(OutputIndex))
	{
		return FCortexCommandRouter::Error(CortexErrorCodes::InvalidField, TEXT("source_output must select an existing output"));
	}
	FExpressionInput* Input = nullptr;
	EMaterialProperty Property = MP_MAX;
	UMaterialExpression* Target = nullptr;
	const bool bRoot = TargetNode == TEXT("MaterialResult") || TargetNode == TEXT("Material");
	if (bRoot)
	{
		if (!CortexMaterialInputUtils::ResolveInput(Material, TargetInput, Property, Input) || !Material->IsPropertySupported(Property))
		{
			return FCortexCommandRouter::Error(CortexErrorCodes::InvalidConnection, TEXT("Unknown or unsupported MaterialResult input"));
		}
	}
	else
	{
		Target = FindExpression(Material, TargetNode);
		if (!Target)
		{
			return FCortexCommandRouter::Error(CortexErrorCodes::NodeNotFound, TEXT("Target material node not found"));
		}
		for (FExpressionInputIterator It(Target); It; ++It)
		{
			if (Target->GetInputName(It.Index).ToString() == TargetInput)
			{
				Input = It.Input;
				break;
			}
		}
		if (!Input)
		{
			return FCortexCommandRouter::Error(CortexErrorCodes::InvalidConnection, TEXT("Target input not found"));
		}
	}
	FScopedTransaction Transaction(FText::FromString(TEXT("Cortex: Connect Material Nodes")), !FCortexCommandRouter::IsInBatch());
	CortexMaterialInputUtils::BeginEdit(Material);
	if (Target)
	{
		Target->Modify();
	}
	Input->Connect(OutputIndex, Source);
	if (bRoot)
	{
		CortexMaterialInputUtils::UpdateUseConstant(Material, Property, false);
	}
	CortexMaterialInputUtils::EndEdit(Material);
	auto Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("source_node"), SourceNode);
	Data->SetNumberField(TEXT("source_output"), OutputIndex);
	Data->SetStringField(TEXT("target_node"), TargetNode);
	Data->SetStringField(TEXT("target_input"), TargetInput);
	Data->SetBoolField(TEXT("connected"), true);
	return FCortexCommandRouter::Success(Data);
}

FCortexCommandResult FCortexMaterialGraphOps::Disconnect(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath, TargetNode, TargetInput;
	if (!Params.IsValid() || !Params->TryGetStringField(TEXT("asset_path"), AssetPath)
		|| !Params->TryGetStringField(TEXT("target_node"), TargetNode)
		|| !Params->TryGetStringField(TEXT("target_input"), TargetInput))
	{
		return FCortexCommandRouter::Error(CortexErrorCodes::InvalidField, TEXT("Missing asset_path, target_node or target_input"));
	}
	FCortexCommandResult LoadError;
	UMaterial* Material = FCortexMaterialAssetOps::LoadMaterial(AssetPath, LoadError);
	if (!Material)
	{
		return LoadError;
	}
	if (TargetNode != TEXT("MaterialResult") && TargetNode != TEXT("Material"))
	{
		return FCortexCommandRouter::Error(CortexErrorCodes::InvalidConnection, TEXT("Expression-to-expression disconnections not yet implemented"));
	}
	EMaterialProperty Property = MP_MAX;
	FExpressionInput* Input = nullptr;
	if (!CortexMaterialInputUtils::ResolveInput(Material, TargetInput, Property, Input))
	{
		return FCortexCommandRouter::Error(CortexErrorCodes::InvalidConnection, TEXT("Unknown or unavailable MaterialResult input"));
	}
	FScopedTransaction Transaction(FText::FromString(TEXT("Cortex: Disconnect Material Nodes")), !FCortexCommandRouter::IsInBatch());
	CortexMaterialInputUtils::BeginEdit(Material);
	Input->Expression = nullptr;
	Input->OutputIndex = 0;
	CortexMaterialInputUtils::UpdateUseConstant(Material, Property, true);
	CortexMaterialInputUtils::EndEdit(Material);
	auto Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("target_node"), TargetNode);
	Data->SetStringField(TEXT("target_input"), TargetInput);
	Data->SetBoolField(TEXT("disconnected"), true);
	return FCortexCommandRouter::Success(Data);
}

FCortexCommandResult FCortexMaterialGraphOps::SetMaterialInput(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath, Name;
	if (!Params.IsValid() || !Params->TryGetStringField(TEXT("asset_path"), AssetPath)
		|| !Params->TryGetStringField(TEXT("input"), Name))
	{
		return FCortexCommandRouter::Error(CortexErrorCodes::InvalidField, TEXT("Missing asset_path or input"));
	}
	FCortexCommandResult LoadError;
	UMaterial* Material = FCortexMaterialAssetOps::LoadMaterial(AssetPath, LoadError);
	if (!Material)
	{
		return LoadError;
	}
	EMaterialProperty Property = MP_MAX;
	FExpressionInput* Input = nullptr;
	if (!CortexMaterialInputUtils::ResolveInput(Material, Name, Property, Input))
	{
		return FCortexCommandRouter::Error(CortexErrorCodes::InvalidField, TEXT("Unknown or unavailable material input"));
	}
	return CortexMaterialInputUtils::ApplyPatch(Material, Property, Params);
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
		FString OutputName = Outputs[i].OutputName.IsNone() ? FString() : Outputs[i].OutputName.ToString();
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
