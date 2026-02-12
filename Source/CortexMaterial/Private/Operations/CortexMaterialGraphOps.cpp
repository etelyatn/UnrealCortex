#include "Operations/CortexMaterialGraphOps.h"
#include "CortexMaterialModule.h"
#include "Materials/Material.h"
#include "Materials/MaterialExpression.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Misc/PackageName.h"
#include "ScopedTransaction.h"

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

static UMaterial* LoadMaterial(const FString& AssetPath, FCortexCommandResult& OutError)
{
	FString PkgName = FPackageName::ObjectPathToPackageName(AssetPath);
	if (!FindPackage(nullptr, *PkgName) && !FPackageName::DoesPackageExist(PkgName))
	{
		OutError = FCortexCommandRouter::Error(
			CortexErrorCodes::MaterialNotFound,
			FString::Printf(TEXT("Material not found: %s"), *AssetPath)
		);
		return nullptr;
	}

	UMaterial* Material = LoadObject<UMaterial>(nullptr, *AssetPath);
	if (Material == nullptr)
	{
		OutError = FCortexCommandRouter::Error(
			CortexErrorCodes::MaterialNotFound,
			FString::Printf(TEXT("Material not found: %s"), *AssetPath)
		);
	}
	return Material;
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
	UMaterial* Material = LoadMaterial(AssetPath, LoadError);
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
	UMaterial* Material = LoadMaterial(AssetPath, LoadError);
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

	return FCortexCommandRouter::Success(Data);
}

FCortexCommandResult FCortexMaterialGraphOps::AddNode(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath, ExpressionClass;
	if (!Params.IsValid()
		|| !Params->TryGetStringField(TEXT("asset_path"), AssetPath)
		|| !Params->TryGetStringField(TEXT("expression_class"), ExpressionClass))
	{
		return FCortexCommandRouter::Error(
			CortexErrorCodes::InvalidField,
			TEXT("Missing required params: asset_path and expression_class"));
	}

	FCortexCommandResult LoadError;
	UMaterial* Material = LoadMaterial(AssetPath, LoadError);
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

	// Find expression class
	UClass* ExpClass = FindFirstObject<UClass>(*ExpressionClass, EFindFirstObjectOptions::NativeFirst);
	if (ExpClass == nullptr || !ExpClass->IsChildOf(UMaterialExpression::StaticClass()))
	{
		return FCortexCommandRouter::Error(
			CortexErrorCodes::InvalidField,
			FString::Printf(TEXT("Invalid expression class: %s"), *ExpressionClass));
	}

	FScopedTransaction Transaction(FText::FromString(
		FString::Printf(TEXT("Cortex: Add Material Expression Node"))
	));

	Material->PreEditChange(nullptr);

	UMaterialExpression* NewExpression = NewObject<UMaterialExpression>(Material, ExpClass);

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

	Material->PostEditChange();
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
	UMaterial* Material = LoadMaterial(AssetPath, LoadError);
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

	FScopedTransaction Transaction(FText::FromString(
		FString::Printf(TEXT("Cortex: Remove Material Expression Node %s"), *NodeId)
	));

	Material->PreEditChange(nullptr);

	Material->GetEditorOnlyData()->ExpressionCollection.Expressions.Remove(Expression);

	Material->PostEditChange();
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
	UMaterial* Material = LoadMaterial(AssetPath, LoadError);
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

	// Check material result inputs
	auto CheckMaterialInput = [&](const FExpressionInput& Input, const FString& InputName)
	{
		if (Input.Expression != nullptr)
		{
			TSharedRef<FJsonObject> Entry = MakeShared<FJsonObject>();
			Entry->SetStringField(TEXT("source_node"), Input.Expression->GetName());
			Entry->SetNumberField(TEXT("source_output"), Input.OutputIndex);
			Entry->SetStringField(TEXT("target_node"), TEXT("MaterialResult"));
			Entry->SetStringField(TEXT("target_input"), InputName);
			ConnectionsArray.Add(MakeShared<FJsonValueObject>(Entry));
		}
	};

	UMaterialEditorOnlyData* EditorData = Material->GetEditorOnlyData();
	CheckMaterialInput(EditorData->BaseColor, TEXT("BaseColor"));
	CheckMaterialInput(EditorData->Metallic, TEXT("Metallic"));
	CheckMaterialInput(EditorData->Specular, TEXT("Specular"));
	CheckMaterialInput(EditorData->Roughness, TEXT("Roughness"));
	CheckMaterialInput(EditorData->Normal, TEXT("Normal"));
	CheckMaterialInput(EditorData->EmissiveColor, TEXT("EmissiveColor"));
	CheckMaterialInput(EditorData->Opacity, TEXT("Opacity"));
	CheckMaterialInput(EditorData->OpacityMask, TEXT("OpacityMask"));

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetArrayField(TEXT("connections"), ConnectionsArray);
	Data->SetNumberField(TEXT("count"), ConnectionsArray.Num());

	return FCortexCommandRouter::Success(Data);
}

FCortexCommandResult FCortexMaterialGraphOps::Connect(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath, SourceNode, TargetNode, TargetInput;
	double SourceOutputDouble = 0;

	if (!Params.IsValid()
		|| !Params->TryGetStringField(TEXT("asset_path"), AssetPath)
		|| !Params->TryGetStringField(TEXT("source_node"), SourceNode)
		|| !Params->TryGetNumberField(TEXT("source_output"), SourceOutputDouble)
		|| !Params->TryGetStringField(TEXT("target_node"), TargetNode)
		|| !Params->TryGetStringField(TEXT("target_input"), TargetInput))
	{
		return FCortexCommandRouter::Error(
			CortexErrorCodes::InvalidField,
			TEXT("Missing required params: asset_path, source_node, source_output, target_node, target_input"));
	}

	int32 SourceOutput = static_cast<int32>(SourceOutputDouble);

	FCortexCommandResult LoadError;
	UMaterial* Material = LoadMaterial(AssetPath, LoadError);
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

	FScopedTransaction Transaction(FText::FromString(
		FString::Printf(TEXT("Cortex: Connect Material Nodes"))
	));

	Material->PreEditChange(nullptr);

	// Connect to MaterialResult
	if (TargetNode == TEXT("MaterialResult"))
	{
		UMaterialEditorOnlyData* EditorData = Material->GetEditorOnlyData();
		FExpressionInput* Input = nullptr;

		if (TargetInput == TEXT("BaseColor"))
			Input = &EditorData->BaseColor;
		else if (TargetInput == TEXT("Metallic"))
			Input = &EditorData->Metallic;
		else if (TargetInput == TEXT("Specular"))
			Input = &EditorData->Specular;
		else if (TargetInput == TEXT("Roughness"))
			Input = &EditorData->Roughness;
		else if (TargetInput == TEXT("Normal"))
			Input = &EditorData->Normal;
		else if (TargetInput == TEXT("EmissiveColor"))
			Input = &EditorData->EmissiveColor;
		else if (TargetInput == TEXT("Opacity"))
			Input = &EditorData->Opacity;
		else if (TargetInput == TEXT("OpacityMask"))
			Input = &EditorData->OpacityMask;

		if (Input == nullptr)
		{
			return FCortexCommandRouter::Error(
				CortexErrorCodes::InvalidConnection,
				FString::Printf(TEXT("Unknown MaterialResult input: %s"), *TargetInput)
			);
		}

		Input->Expression = SourceExpr;
		Input->OutputIndex = SourceOutput;
	}
	else
	{
		return FCortexCommandRouter::Error(
			CortexErrorCodes::InvalidConnection,
			TEXT("Expression-to-expression connections not yet implemented")
		);
	}

	Material->PostEditChange();
	Material->MarkPackageDirty();

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("source_node"), SourceNode);
	Data->SetNumberField(TEXT("source_output"), SourceOutput);
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
	UMaterial* Material = LoadMaterial(AssetPath, LoadError);
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

	FScopedTransaction Transaction(FText::FromString(
		FString::Printf(TEXT("Cortex: Disconnect Material Nodes"))
	));

	Material->PreEditChange(nullptr);

	// Disconnect from MaterialResult
	if (TargetNode == TEXT("MaterialResult"))
	{
		UMaterialEditorOnlyData* EditorData = Material->GetEditorOnlyData();
		FExpressionInput* Input = nullptr;

		if (TargetInput == TEXT("BaseColor"))
			Input = &EditorData->BaseColor;
		else if (TargetInput == TEXT("Metallic"))
			Input = &EditorData->Metallic;
		else if (TargetInput == TEXT("Specular"))
			Input = &EditorData->Specular;
		else if (TargetInput == TEXT("Roughness"))
			Input = &EditorData->Roughness;
		else if (TargetInput == TEXT("Normal"))
			Input = &EditorData->Normal;
		else if (TargetInput == TEXT("EmissiveColor"))
			Input = &EditorData->EmissiveColor;
		else if (TargetInput == TEXT("Opacity"))
			Input = &EditorData->Opacity;
		else if (TargetInput == TEXT("OpacityMask"))
			Input = &EditorData->OpacityMask;

		if (Input == nullptr)
		{
			return FCortexCommandRouter::Error(
				CortexErrorCodes::InvalidConnection,
				FString::Printf(TEXT("Unknown MaterialResult input: %s"), *TargetInput)
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

	Material->PostEditChange();
	Material->MarkPackageDirty();

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("target_node"), TargetNode);
	Data->SetStringField(TEXT("target_input"), TargetInput);
	Data->SetBoolField(TEXT("disconnected"), true);

	return FCortexCommandRouter::Success(Data);
}
