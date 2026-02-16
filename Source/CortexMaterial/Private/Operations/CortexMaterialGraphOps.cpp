#include "Operations/CortexMaterialGraphOps.h"
#include "Operations/CortexMaterialAssetOps.h"
#include "CortexMaterialModule.h"
#include "Materials/Material.h"
#include "Materials/MaterialExpression.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Misc/PackageName.h"
#include "ScopedTransaction.h"
#include "CortexBatchScope.h"
#include "CortexCommandRouter.h"
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

	// Find expression class
	UClass* ExpClass = FindFirstObject<UClass>(*ExpressionClass, EFindFirstObjectOptions::NativeFirst);
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

	CheckMaterialInput(EditorData->BaseColor, TEXT("BaseColor"));
	CheckMaterialInput(EditorData->Metallic, TEXT("Metallic"));
	CheckMaterialInput(EditorData->Specular, TEXT("Specular"));
	CheckMaterialInput(EditorData->Roughness, TEXT("Roughness"));
	CheckMaterialInput(EditorData->Normal, TEXT("Normal"));
	CheckMaterialInput(EditorData->EmissiveColor, TEXT("EmissiveColor"));
	CheckMaterialInput(EditorData->Opacity, TEXT("Opacity"));
	CheckMaterialInput(EditorData->OpacityMask, TEXT("OpacityMask"));

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

	if (!Params.IsValid()
		|| !Params->TryGetStringField(TEXT("asset_path"), AssetPath)
		|| !Params->TryGetStringField(TEXT("source_node"), SourceNode)
		|| !Params->TryGetStringField(TEXT("target_node"), TargetNode)
		|| !Params->TryGetStringField(TEXT("target_input"), TargetInput))
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

	// Connect to MaterialResult or expression
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

	// Check which expressions connect to MaterialResult
	UMaterialEditorOnlyData* EditorData = Material->GetEditorOnlyData();
	TSet<FString> MaterialResultInputIds;
	auto CheckResultInput = [&](const FExpressionInput& Input)
	{
		if (Input.Expression)
		{
			const FString* Id = ExprToId.Find(Input.Expression);
			if (Id) MaterialResultInputIds.Add(*Id);
		}
	};
	CheckResultInput(EditorData->BaseColor);
	CheckResultInput(EditorData->Metallic);
	CheckResultInput(EditorData->Specular);
	CheckResultInput(EditorData->Roughness);
	CheckResultInput(EditorData->Normal);
	CheckResultInput(EditorData->EmissiveColor);
	CheckResultInput(EditorData->Opacity);
	CheckResultInput(EditorData->OpacityMask);

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

	// Build layout nodes with connectivity
	TArray<FCortexLayoutNode> LayoutNodes;
	for (UMaterialExpression* Expr : Expressions)
	{
		if (!Expr) continue;
		FCortexLayoutNode LayoutNode;
		LayoutNode.Id = ExprToId[Expr];
		LayoutNode.bIsEntryPoint = MaterialResultInputIds.Contains(LayoutNode.Id);
		int32 InputCount = 0;
		for (FExpressionInputIterator It(Expr); It; ++It) InputCount++;
		int32 OutputCount = Expr->GetOutputs().Num();
		LayoutNode.Width = 150;
		LayoutNode.Height = FMath::Max(80, 40 + FMath::Max(InputCount, OutputCount) * 26);
		const TArray<FString>* Outputs = ForwardEdges.Find(LayoutNode.Id);
		if (Outputs) LayoutNode.DataOutputs = *Outputs;
		LayoutNodes.Add(LayoutNode);
	}

	// Run shared layout engine
	FCortexLayoutConfig Config;
	Config.Direction = ECortexLayoutDirection::RightToLeft;
	double HSpacingVal = 0, VSpacingVal = 0;
	if (Params->TryGetNumberField(TEXT("horizontal_spacing"), HSpacingVal) && HSpacingVal > 0)
		Config.HorizontalSpacing = static_cast<int32>(HSpacingVal);
	if (Params->TryGetNumberField(TEXT("vertical_spacing"), VSpacingVal) && VSpacingVal > 0)
		Config.VerticalSpacing = static_cast<int32>(VSpacingVal);

	FCortexLayoutResult LayoutResult = FCortexGraphLayoutOps::CalculateLayout(LayoutNodes, Config);

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
	Data->SetNumberField(TEXT("node_count"), LayoutResult.Positions.Num());
	return FCortexCommandRouter::Success(Data);
}

FCortexCommandResult FCortexMaterialGraphOps::SetNodeProperty(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath, NodeId, PropertyName;
	if (!Params.IsValid()
		|| !Params->TryGetStringField(TEXT("asset_path"), AssetPath)
		|| !Params->TryGetStringField(TEXT("node_id"), NodeId)
		|| !Params->TryGetStringField(TEXT("property_name"), PropertyName))
	{
		return FCortexCommandRouter::Error(
			CortexErrorCodes::InvalidField,
			TEXT("Missing required params: asset_path, node_id, property_name"));
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

	// Set property value based on type
	void* PropertyAddress = Property->ContainerPtrToValuePtr<void>(Expression);

	if (FStrProperty* StrProp = CastField<FStrProperty>(Property))
	{
		FString StringValue;
		if (Value->TryGetString(StringValue))
		{
			StrProp->SetPropertyValue(PropertyAddress, StringValue);
		}
		else
		{
			return FCortexCommandRouter::Error(
				CortexErrorCodes::InvalidField,
				TEXT("value must be a string for FStrProperty"));
		}
	}
	else if (FNameProperty* NameProp = CastField<FNameProperty>(Property))
	{
		FString StringValue;
		if (Value->TryGetString(StringValue))
		{
			NameProp->SetPropertyValue(PropertyAddress, FName(*StringValue));
		}
		else
		{
			return FCortexCommandRouter::Error(
				CortexErrorCodes::InvalidField,
				TEXT("value must be a string for FNameProperty"));
		}
	}
	else if (FFloatProperty* FloatProp = CastField<FFloatProperty>(Property))
	{
		double DoubleValue;
		if (Value->TryGetNumber(DoubleValue))
		{
			FloatProp->SetPropertyValue(PropertyAddress, static_cast<float>(DoubleValue));
		}
		else
		{
			return FCortexCommandRouter::Error(
				CortexErrorCodes::InvalidField,
				TEXT("value must be a number for FFloatProperty"));
		}
	}
	else if (FIntProperty* IntProp = CastField<FIntProperty>(Property))
	{
		double DoubleValue;
		if (Value->TryGetNumber(DoubleValue))
		{
			IntProp->SetPropertyValue(PropertyAddress, static_cast<int32>(DoubleValue));
		}
		else
		{
			return FCortexCommandRouter::Error(
				CortexErrorCodes::InvalidField,
				TEXT("value must be a number for FIntProperty"));
		}
	}
	else if (FBoolProperty* BoolProp = CastField<FBoolProperty>(Property))
	{
		bool BoolValue;
		if (Value->TryGetBool(BoolValue))
		{
			BoolProp->SetPropertyValue(PropertyAddress, BoolValue);
		}
		else
		{
			return FCortexCommandRouter::Error(
				CortexErrorCodes::InvalidField,
				TEXT("value must be a boolean for FBoolProperty"));
		}
	}
	else if (FStructProperty* StructProp = CastField<FStructProperty>(Property))
	{
		// FLinearColor - from [R, G, B, A] array
		if (StructProp->Struct == TBaseStructure<FLinearColor>::Get())
		{
			const TArray<TSharedPtr<FJsonValue>>* ColorArray = nullptr;
			if (Value->TryGetArray(ColorArray) && ColorArray->Num() == 4)
			{
				FLinearColor* Color = static_cast<FLinearColor*>(PropertyAddress);
				Color->R = static_cast<float>((*ColorArray)[0]->AsNumber());
				Color->G = static_cast<float>((*ColorArray)[1]->AsNumber());
				Color->B = static_cast<float>((*ColorArray)[2]->AsNumber());
				Color->A = static_cast<float>((*ColorArray)[3]->AsNumber());
			}
			else
			{
				return FCortexCommandRouter::Error(
					CortexErrorCodes::InvalidField,
					TEXT("value must be [R, G, B, A] array for FLinearColor"));
			}
		}
		// FVector - from [X, Y, Z] array
		else if (StructProp->Struct == TBaseStructure<FVector>::Get())
		{
			const TArray<TSharedPtr<FJsonValue>>* VecArray = nullptr;
			if (Value->TryGetArray(VecArray) && VecArray->Num() == 3)
			{
				FVector* Vec = static_cast<FVector*>(PropertyAddress);
				Vec->X = (*VecArray)[0]->AsNumber();
				Vec->Y = (*VecArray)[1]->AsNumber();
				Vec->Z = (*VecArray)[2]->AsNumber();
			}
			else
			{
				return FCortexCommandRouter::Error(
					CortexErrorCodes::InvalidField,
					TEXT("value must be [X, Y, Z] array for FVector"));
			}
		}
		else
		{
			return FCortexCommandRouter::Error(
				CortexErrorCodes::InvalidField,
				FString::Printf(TEXT("Unsupported struct type: %s"), *StructProp->Struct->GetName())
			);
		}
	}
	else if (FSoftObjectProperty* SoftObjProp = CastField<FSoftObjectProperty>(Property))
	{
		FString ObjectPath;
		if (Value->TryGetString(ObjectPath))
		{
			FSoftObjectPath SoftPath(ObjectPath);
			FSoftObjectPtr* SoftPtrAddress = static_cast<FSoftObjectPtr*>(PropertyAddress);
			*SoftPtrAddress = FSoftObjectPtr(SoftPath);
		}
		else
		{
			return FCortexCommandRouter::Error(
				CortexErrorCodes::InvalidField,
				TEXT("value must be a string (object path) for FSoftObjectProperty"));
		}
	}
	else if (FObjectProperty* ObjProp = CastField<FObjectProperty>(Property))
	{
		FString ObjectPath;
		if (Value->TryGetString(ObjectPath))
		{
			// Guard LoadObject to prevent SkipPackage warnings
			FString PkgName = FPackageName::ObjectPathToPackageName(ObjectPath);
			if (!FindPackage(nullptr, *PkgName) && !FPackageName::DoesPackageExist(PkgName))
			{
				return FCortexCommandRouter::Error(
					CortexErrorCodes::AssetNotFound,
					FString::Printf(TEXT("Object not found: %s"), *ObjectPath));
			}

			UObject* Obj = LoadObject<UObject>(nullptr, *ObjectPath);
			if (Obj == nullptr)
			{
				return FCortexCommandRouter::Error(
					CortexErrorCodes::AssetNotFound,
					FString::Printf(TEXT("Failed to load object: %s"), *ObjectPath));
			}
			ObjProp->SetPropertyValue(PropertyAddress, Obj);
		}
		else
		{
			return FCortexCommandRouter::Error(
				CortexErrorCodes::InvalidField,
				TEXT("value must be a string (object path) for FObjectProperty"));
		}
	}
	else
	{
		return FCortexCommandRouter::Error(
			CortexErrorCodes::InvalidField,
			FString::Printf(TEXT("Unsupported property type: %s"), *Property->GetClass()->GetName())
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
