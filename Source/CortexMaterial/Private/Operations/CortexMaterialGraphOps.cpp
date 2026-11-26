#include "Operations/CortexMaterialGraphOps.h"
#include "Operations/CortexMaterialAssetOps.h"
#include "Operations/CortexMaterialGraphTarget.h"
#include "Operations/CortexMaterialInputUtils.h"
#include "CortexMaterialModule.h"
#include "CortexPropertyUtils.h"
#include "Misc/EngineVersionComparison.h"
#include "Materials/Material.h"
#include "Materials/MaterialFunction.h"
#include "Materials/MaterialFunctionInstance.h"
#include "Materials/MaterialExpression.h"
#include "Materials/MaterialExpressionFunctionInput.h"
#include "Materials/MaterialExpressionFunctionOutput.h"
#include "Materials/MaterialParameters.h"
#include "Engine/Font.h"
#include "Engine/TextureCollection.h"
#include "MaterialDomain.h"
#include "Misc/MemStack.h"
#include "Misc/ScopeExit.h"
#include "UObject/UObjectHash.h"

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
/** Pin name for an expression input; falls back to the reflected FExpressionInput property name.
 *  Function outputs override GetInputName with NAME_None, while their single input is A. */
FName CortexMaterialInputPinName(const UMaterialExpression* Expression, int32 InputIndex)
{
	if (Expression == nullptr)
	{
		return NAME_None;
	}
	const FName InputName = Expression->GetInputName(InputIndex);
	if (!InputName.IsNone())
	{
		return InputName;
	}

	int32 Index = 0;
	for (TFieldIterator<FStructProperty> It(Expression->GetClass(),
		EFieldIteratorFlags::IncludeSuper, EFieldIteratorFlags::ExcludeDeprecated); It; ++It)
	{
		FStructProperty* StructProperty = *It;
		if (StructProperty->Struct->GetFName() != NAME_ExpressionInput)
		{
			continue;
		}
		for (int32 ArrayIndex = 0; ArrayIndex < StructProperty->ArrayDim; ++ArrayIndex)
		{
			if (Index == InputIndex)
			{
				return StructProperty->GetFName();
			}
			++Index;
		}
	}
	return NAME_None;
}

const TCHAR* CortexMaterialParameterTypeName(EMaterialParameterType Type)
{
	switch (Type)
	{
	case EMaterialParameterType::Scalar: return TEXT("scalar");
	case EMaterialParameterType::Vector: return TEXT("vector");
	case EMaterialParameterType::DoubleVector: return TEXT("double_vector");
	case EMaterialParameterType::Texture: return TEXT("texture");
	case EMaterialParameterType::TextureCollection: return TEXT("texture_collection");
	case EMaterialParameterType::Font: return TEXT("font");
	case EMaterialParameterType::RuntimeVirtualTexture: return TEXT("runtime_virtual_texture");
	case EMaterialParameterType::SparseVolumeTexture: return TEXT("sparse_volume_texture");
	case EMaterialParameterType::StaticSwitch: return TEXT("static_switch");
	case EMaterialParameterType::ParameterCollection: return TEXT("parameter_collection");
	case EMaterialParameterType::StaticComponentMask: return TEXT("static_component_mask");
	default: return TEXT("unknown");
	}
}

TSharedPtr<FJsonValue> CortexMaterialParameterValueJson(const FMaterialParameterValue& Value)
{
	auto ObjectPath = [](const UObject* Object)
	{
		return MakeShared<FJsonValueString>(Object ? Object->GetPathName() : FString());
	};

	switch (Value.Type)
	{
	case EMaterialParameterType::Scalar:
		return MakeShared<FJsonValueNumber>(Value.AsScalar());
	case EMaterialParameterType::Vector:
	{
		const FLinearColor Color = Value.AsLinearColor();
		auto Result = MakeShared<FJsonObject>();
		Result->SetNumberField(TEXT("r"), Color.R);
		Result->SetNumberField(TEXT("g"), Color.G);
		Result->SetNumberField(TEXT("b"), Color.B);
		Result->SetNumberField(TEXT("a"), Color.A);
		return MakeShared<FJsonValueObject>(Result);
	}
	case EMaterialParameterType::DoubleVector:
	{
		const FVector4d Vector = Value.AsVector4d();
		auto Result = MakeShared<FJsonObject>();
		Result->SetNumberField(TEXT("x"), Vector.X);
		Result->SetNumberField(TEXT("y"), Vector.Y);
		Result->SetNumberField(TEXT("z"), Vector.Z);
		Result->SetNumberField(TEXT("w"), Vector.W);
		return MakeShared<FJsonValueObject>(Result);
	}
	case EMaterialParameterType::Texture:
		return ObjectPath(Value.Texture);
	case EMaterialParameterType::TextureCollection:
		return ObjectPath(Value.TextureCollection);
	case EMaterialParameterType::RuntimeVirtualTexture:
		return ObjectPath(Value.AsTextureObject());
	case EMaterialParameterType::SparseVolumeTexture:
		return ObjectPath(Value.AsTextureObject());
	case EMaterialParameterType::ParameterCollection:
		return ObjectPath(Value.ParameterCollection);
	case EMaterialParameterType::StaticSwitch:
		return MakeShared<FJsonValueBoolean>(Value.AsStaticSwitch());
	case EMaterialParameterType::StaticComponentMask:
	{
		const FStaticComponentMaskValue Mask = Value.AsStaticComponentMask();
		auto Result = MakeShared<FJsonObject>();
		Result->SetBoolField(TEXT("r"), Mask.R);
		Result->SetBoolField(TEXT("g"), Mask.G);
		Result->SetBoolField(TEXT("b"), Mask.B);
		Result->SetBoolField(TEXT("a"), Mask.A);
		return MakeShared<FJsonValueObject>(Result);
	}
	case EMaterialParameterType::Font:
	{
		auto Result = MakeShared<FJsonObject>();
		Result->SetStringField(TEXT("font"), Value.Font.Value ? Value.Font.Value->GetPathName() : FString());
		Result->SetNumberField(TEXT("page"), Value.Font.Page);
		return MakeShared<FJsonValueObject>(Result);
	}
	default:
		return MakeShared<FJsonValueNull>();
	}
}

void CortexAddParameterOverride(const UMaterialFunctionInstance* Instance, UMaterialExpression* Expression,
	const TSharedPtr<FJsonObject>& Data)
{
	if (Instance == nullptr || Expression == nullptr || !Data.IsValid())
	{
		return;
	}
	FMaterialParameterMetadata BaseMetadata;
	if (!Expression->HasAParameterName() || !Expression->GetParameterValue(BaseMetadata)
		|| BaseMetadata.Value.Type == EMaterialParameterType::None)
	{
		return;
	}

	const FName ParameterName = Expression->GetParameterName();
	FMaterialParameterMetadata OverrideMetadata;
	if (!Instance->GetParameterOverrideValue(BaseMetadata.Value.Type, ParameterName, OverrideMetadata))
	{
		return;
	}

	auto Override = MakeShared<FJsonObject>();
	Override->SetStringField(TEXT("name"), ParameterName.ToString());
	Override->SetStringField(TEXT("type"), CortexMaterialParameterTypeName(BaseMetadata.Value.Type));
	Override->SetField(TEXT("value"), CortexMaterialParameterValueJson(OverrideMetadata.Value));
	Data->SetObjectField(TEXT("parameter_override"), Override);
}

FCortexCommandResult MaterialFunctionReadOnly(const FCortexMaterialGraphTarget& Target)
{
	return FCortexCommandRouter::Error(
		CortexErrorCodes::InvalidOperation,
		FString::Printf(
			TEXT("Material function instances are read-only; edit the base function: %s"),
			*Target.GetBaseFunctionPath()));
}

bool CortexFunctionSignatureNameTaken(const FCortexMaterialGraphTarget& Target,
	const UMaterialExpression* Exclude, bool bInput, FName Name)
{
	for (UMaterialExpression* Expression : Target.GetExpressions())
	{
		if (Expression == nullptr || Expression == Exclude)
		{
			continue;
		}
		if (bInput)
		{
			const UMaterialExpressionFunctionInput* Other = Cast<UMaterialExpressionFunctionInput>(Expression);
			if (Other && Other->InputName == Name)
			{
				return true;
			}
		}
		else
		{
			const UMaterialExpressionFunctionOutput* Other = Cast<UMaterialExpressionFunctionOutput>(Expression);
			if (Other && Other->OutputName == Name)
			{
				return true;
			}
		}
	}
	return false;
}

/** Mirrors the editor's ValidateName suffixing; ValidateName itself only scans a UMaterial's
 *  expressions, so function-owned inputs/outputs must be uniquified against the function. */
FName CortexUniqueFunctionSignatureName(const FCortexMaterialGraphTarget& Target,
	const UMaterialExpression* Exclude, bool bInput, FName Desired)
{
	FName Candidate = Desired;
	for (int32 Index = 1; ; ++Index)
	{
		if (Index != 1)
		{
			Candidate = Desired;
			Candidate.SetNumber(Index);
		}
		if (!CortexFunctionSignatureNameTaken(Target, Exclude, bInput, Candidate))
		{
			return Candidate;
		}
	}
}
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
	FCortexMaterialGraphTarget Target;
	if (!FCortexMaterialGraphTarget::Load(AssetPath, Target, LoadError))
	{
		return LoadError;
	}

	TArray<TSharedPtr<FJsonValue>> NodesArray;

	for (UMaterialExpression* Expression : Target.GetExpressions())
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

	if (Target.IsMaterial())
	{
		// Materials have a virtual MaterialResult sink; a function's outputs are real expressions.
		TSharedRef<FJsonObject> MaterialResultNode = MakeShared<FJsonObject>();
		MaterialResultNode->SetStringField(TEXT("node_id"), TEXT("MaterialResult"));
		MaterialResultNode->SetStringField(TEXT("expression_class"), TEXT("MaterialResult"));
		NodesArray.Add(MakeShared<FJsonValueObject>(MaterialResultNode));
	}

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("asset_kind"), Target.GetKindName());
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
	FCortexMaterialGraphTarget Target;
	if (!FCortexMaterialGraphTarget::Load(AssetPath, Target, LoadError))
	{
		return LoadError;
	}

	UMaterialExpression* Expression = Target.FindExpression(NodeId);
	if (Expression == nullptr)
	{
		return FCortexCommandRouter::Error(
			CortexErrorCodes::NodeNotFound,
			FString::Printf(TEXT("Node not found: %s"), *NodeId)
		);
	}

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("asset_kind"), Target.GetKindName());
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

	if (Target.GetKind() == ECortexMaterialGraphTargetKind::FunctionInstance)
	{
		CortexAddParameterOverride(Cast<UMaterialFunctionInstance>(Target.GetFunction()), Expression, Data);
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
	FCortexMaterialGraphTarget Target;
	if (!FCortexMaterialGraphTarget::Load(AssetPath, Target, LoadError))
	{
		return LoadError;
	}
	if (Target.IsReadOnly())
	{
		return MaterialFunctionReadOnly(Target);
	}
	if (Target.IsMaterial() && !Target.GetMaterial()->GetEditorOnlyData())
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

	const UMaterialExpression* ClassDefault = Cast<UMaterialExpression>(ExpClass->GetDefaultObject());
	if (ClassDefault == nullptr || !ClassDefault->IsAllowedIn(Target.GetAsset()))
	{
		return FCortexCommandRouter::Error(
			CortexErrorCodes::InvalidOperation,
			FString::Printf(TEXT("Expression class %s is not allowed in this graph: %s"),
				*ExpClass->GetName(), *AssetPath));
	}

	FScopedTransaction Transaction(FText::FromString(TEXT("Cortex: Add Material Expression Node")), !FCortexCommandRouter::IsInBatch());
	Target.BeginEdit();

	UMaterialExpression* NewExpression = NewObject<UMaterialExpression>(
		Target.GetAsset(), ExpClass, NAME_None, RF_Transactional);
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

	if (Target.IsMaterial())
	{
		Target.GetMaterial()->GetEditorOnlyData()->ExpressionCollection.Expressions.Add(NewExpression);
	}
	else
	{
		Target.GetBaseFunction()->GetExpressionCollection().AddExpression(NewExpression);
	}

	// The material editor generates expression ids when creating function inputs/outputs; do the same
	// so a new function signature entry keeps a stable identity through later renames. ValidateName
	// only dedupes against a UMaterial, so function-owned names are made unique here.
	if (UMaterialExpressionFunctionInput* FunctionInput = Cast<UMaterialExpressionFunctionInput>(NewExpression))
	{
		FunctionInput->ConditionallyGenerateId(true);
		FunctionInput->InputName = CortexUniqueFunctionSignatureName(
			Target, FunctionInput, true, FunctionInput->InputName);
	}
	if (UMaterialExpressionFunctionOutput* FunctionOutput = Cast<UMaterialExpressionFunctionOutput>(NewExpression))
	{
		FunctionOutput->ConditionallyGenerateId(true);
		FunctionOutput->OutputName = CortexUniqueFunctionSignatureName(
			Target, FunctionOutput, false, FunctionOutput->OutputName);
	}
	Target.EndEdit();

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
	FCortexMaterialGraphTarget Target;
	if (!FCortexMaterialGraphTarget::Load(AssetPath, Target, LoadError))
	{
		return LoadError;
	}
	if (Target.IsReadOnly())
	{
		return MaterialFunctionReadOnly(Target);
	}
	if (Target.IsMaterial() && !Target.GetMaterial()->GetEditorOnlyData())
	{
		return FCortexCommandRouter::Error(
			CortexErrorCodes::SerializationError,
			TEXT("Material has no editor data"));
	}

	UMaterialExpression* Expression = Target.FindExpression(NodeId);
	if (Expression == nullptr)
	{
		return FCortexCommandRouter::Error(
			CortexErrorCodes::NodeNotFound,
			FString::Printf(TEXT("Node not found: %s"), *NodeId)
		);
	}

	FScopedTransaction Transaction(FText::FromString(
		FString::Printf(TEXT("Cortex: Remove Material Expression Node %s"), *NodeId)
	), !FCortexCommandRouter::IsInBatch());
	Target.BeginEdit();

	// Break every link that points at the removed expression, as the material editor does on delete.
	for (UMaterialExpression* Other : Target.GetExpressions())
	{
		if (Other == nullptr || Other == Expression)
		{
			continue;
		}
		for (FExpressionInputIterator It(Other); It; ++It)
		{
			if (It->Expression == Expression)
			{
				CortexMaterialInputUtils::ModifyExpression(Other);
				It->Expression = nullptr;
				It->OutputIndex = 0;
			}
		}
	}

	if (Target.IsMaterial())
	{
		UMaterial* Material = Target.GetMaterial();
		for (int32 Prop = 0; Prop < MP_MAX; ++Prop)
		{
			const EMaterialProperty Property = static_cast<EMaterialProperty>(Prop);
			FExpressionInput* Input = Material->GetExpressionInputForProperty(Property);
			if (Input && Input->Expression == Expression)
			{
				Input->Expression = nullptr;
				Input->OutputIndex = 0;
				CortexMaterialInputUtils::UpdateUseConstant(Material, Property, true);
			}
		}
		Material->RemoveExpressionParameter(Expression);
		Material->GetEditorOnlyData()->ExpressionCollection.Expressions.Remove(Expression);
	}
	else
	{
		Target.GetBaseFunction()->GetExpressionCollection().RemoveExpression(Expression);
	}

	CortexMaterialInputUtils::ModifyExpression(Expression);
	Target.EndEdit();

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
	FCortexMaterialGraphTarget Target;
	if (!FCortexMaterialGraphTarget::Load(AssetPath, Target, LoadError))
	{
		return LoadError;
	}
	if (Target.IsMaterial() && !Target.GetMaterial()->GetEditorOnlyData())
	{
		return FCortexCommandRouter::Error(
			CortexErrorCodes::SerializationError,
			TEXT("Material has no editor data"));
	}

	TArray<TSharedPtr<FJsonValue>> ConnectionsArray;

	if (Target.IsMaterial())
	{
		UMaterial* Material = Target.GetMaterial();
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
	}

	// Check expression-to-expression connections
	for (UMaterialExpression* Expression : Target.GetExpressions())
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
				Entry->SetStringField(TEXT("target_input"), CortexMaterialInputPinName(Expression, It.Index).ToString());
				ConnectionsArray.Add(MakeShared<FJsonValueObject>(Entry));
			}
		}
	}

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("asset_kind"), Target.GetKindName());
	Data->SetArrayField(TEXT("connections"), ConnectionsArray);
	Data->SetNumberField(TEXT("count"), ConnectionsArray.Num());
	bool bIncludeInputState = false;
	if (Params->HasField(TEXT("include_input_state")) && !Params->TryGetBoolField(TEXT("include_input_state"), bIncludeInputState))
	{
		return FCortexCommandRouter::Error(CortexErrorCodes::InvalidField, TEXT("include_input_state must be a boolean"));
	}
	if (bIncludeInputState)
	{
		if (!Target.IsMaterial())
		{
			return FCortexCommandRouter::Error(
				CortexErrorCodes::InvalidOperation,
				TEXT("include_input_state is only available for materials; function inputs are expressions"));
		}
		Data->SetArrayField(TEXT("root_inputs"), CortexMaterialInputUtils::SerializeInputs(Target.GetMaterial()));
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
	FCortexMaterialGraphTarget Target;
	if (!FCortexMaterialGraphTarget::Load(AssetPath, Target, LoadError))
	{
		return LoadError;
	}
	if (Target.IsReadOnly())
	{
		return MaterialFunctionReadOnly(Target);
	}
	UMaterialExpression* Source = Target.FindExpression(SourceNode);
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
	UMaterialExpression* TargetExpression = nullptr;
	const bool bRoot = Target.IsMaterial()
		&& (TargetNode == TEXT("MaterialResult") || TargetNode == TEXT("Material"));
	if (bRoot)
	{
		UMaterial* Material = Target.GetMaterial();
		if (!CortexMaterialInputUtils::ResolveInput(Material, TargetInput, Property, Input) || !Material->IsPropertySupported(Property))
		{
			return FCortexCommandRouter::Error(CortexErrorCodes::InvalidConnection, TEXT("Unknown or unsupported MaterialResult input"));
		}
	}
	else
	{
		TargetExpression = Target.FindExpression(TargetNode);
		if (!TargetExpression)
		{
			return FCortexCommandRouter::Error(CortexErrorCodes::NodeNotFound, TEXT("Target material node not found"));
		}
		for (FExpressionInputIterator It(TargetExpression); It; ++It)
		{
			if (CortexMaterialInputPinName(TargetExpression, It.Index).ToString() == TargetInput)
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
	Target.BeginEdit();
	if (TargetExpression)
	{
		CortexMaterialInputUtils::ModifyExpression(TargetExpression);
	}
	Input->Connect(OutputIndex, Source);
	if (bRoot)
	{
		CortexMaterialInputUtils::UpdateUseConstant(Target.GetMaterial(), Property, false);
	}
	Target.EndEdit();
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
	FCortexMaterialGraphTarget Target;
	if (!FCortexMaterialGraphTarget::Load(AssetPath, Target, LoadError))
	{
		return LoadError;
	}
	if (Target.IsReadOnly())
	{
		return MaterialFunctionReadOnly(Target);
	}

	const bool bRoot = Target.IsMaterial()
		&& (TargetNode == TEXT("MaterialResult") || TargetNode == TEXT("Material"));
	EMaterialProperty Property = MP_MAX;
	FExpressionInput* Input = nullptr;
	UMaterialExpression* TargetExpression = nullptr;
	if (bRoot)
	{
		if (!CortexMaterialInputUtils::ResolveInput(Target.GetMaterial(), TargetInput, Property, Input))
		{
			return FCortexCommandRouter::Error(CortexErrorCodes::InvalidConnection, TEXT("Unknown or unavailable MaterialResult input"));
		}
	}
	else
	{
		TargetExpression = Target.FindExpression(TargetNode);
		if (!TargetExpression)
		{
			return FCortexCommandRouter::Error(CortexErrorCodes::NodeNotFound, TEXT("Target material node not found"));
		}
		for (FExpressionInputIterator It(TargetExpression); It; ++It)
		{
			if (CortexMaterialInputPinName(TargetExpression, It.Index).ToString() == TargetInput)
			{
				Input = It.Input;
				break;
			}
		}
		if (!Input)
		{
			return FCortexCommandRouter::Error(CortexErrorCodes::InvalidConnection, TEXT("Target input not found"));
		}
		if (!Input->Expression)
		{
			return FCortexCommandRouter::Error(CortexErrorCodes::InvalidConnection, TEXT("Target input is not connected"));
		}
	}

	FScopedTransaction Transaction(FText::FromString(TEXT("Cortex: Disconnect Material Nodes")), !FCortexCommandRouter::IsInBatch());
	Target.BeginEdit();
	if (TargetExpression)
	{
		CortexMaterialInputUtils::ModifyExpression(TargetExpression);
	}
	Input->Expression = nullptr;
	Input->OutputIndex = 0;
	if (bRoot)
	{
		CortexMaterialInputUtils::UpdateUseConstant(Target.GetMaterial(), Property, true);
	}
	Target.EndEdit();
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
	FCortexMaterialGraphTarget Target;
	if (!FCortexMaterialGraphTarget::Load(AssetPath, Target, LoadError))
	{
		return LoadError;
	}
	if (!Target.IsMaterial())
	{
		return FCortexCommandRouter::Error(
			CortexErrorCodes::InvalidOperation,
			FString::Printf(TEXT("set_material_input requires a material asset: %s"), *AssetPath));
	}
	UMaterial* Material = Target.GetMaterial();
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
	FCortexMaterialGraphTarget Target;
	if (!FCortexMaterialGraphTarget::Load(AssetPath, Target, LoadError))
	{
		return LoadError;
	}
	if (Target.IsReadOnly())
	{
		return MaterialFunctionReadOnly(Target);
	}
	if (Target.IsMaterial() && !Target.GetMaterial()->GetEditorOnlyData())
	{
		return FCortexCommandRouter::Error(
			CortexErrorCodes::SerializationError, TEXT("Material has no editor data"));
	}

	UMaterial* Material = Target.GetMaterial();
	const bool bMaterial = Target.IsMaterial();
	const TConstArrayView<TObjectPtr<UMaterialExpression>> Expressions = Target.GetExpressions();

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
	if (bMaterial)
	{
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
	if (bMaterial)
	{
		for (const FString& Id : MaterialResultInputIds)
		{
			ForwardEdges.FindOrAdd(Id).AddUnique(MaterialResultId);
		}
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
	if (bMaterial)
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
	FScopedTransaction Transaction(FText::FromString(TEXT("Cortex: Auto-Layout Material Graph")), !FCortexCommandRouter::IsInBatch());
	Target.BeginEdit();

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
			CortexMaterialInputUtils::ModifyExpression(*ExprPtr);
			(*ExprPtr)->MaterialExpressionEditorX = Pair.Value.X;
			(*ExprPtr)->MaterialExpressionEditorY = Pair.Value.Y;
		}
	}
	Target.EndEdit(false);

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
	FCortexMaterialGraphTarget Target;
	if (!FCortexMaterialGraphTarget::Load(AssetPath, Target, LoadError))
	{
		return LoadError;
	}
	if (Target.IsReadOnly())
	{
		return MaterialFunctionReadOnly(Target);
	}
	if (Target.IsMaterial() && !Target.GetMaterial()->GetEditorOnlyData())
	{
		return FCortexCommandRouter::Error(
			CortexErrorCodes::SerializationError,
			TEXT("Material has no editor data"));
	}

	UMaterialExpression* Expression = Target.FindExpression(NodeId);
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

	// The function editor refuses duplicate signature names with a modal dialog. Refuse here instead,
	// before staging, so the asset is untouched and no dialog can block an unattended editor.
	if (!Target.IsMaterial() && Value->Type == EJson::String)
	{
		const FName Requested(*Value->AsString());
		if (Property->GetFName() == GET_MEMBER_NAME_CHECKED(UMaterialExpressionFunctionInput, InputName)
			&& CortexFunctionSignatureNameTaken(Target, Expression, true, Requested))
		{
			return FCortexCommandRouter::Error(
				CortexErrorCodes::InvalidValue,
				FString::Printf(TEXT("Function input name must be unique: %s"), *Requested.ToString()));
		}
		if (Property->GetFName() == GET_MEMBER_NAME_CHECKED(UMaterialExpressionFunctionOutput, OutputName)
			&& CortexFunctionSignatureNameTaken(Target, Expression, false, Requested))
		{
			return FCortexCommandRouter::Error(
				CortexErrorCodes::InvalidValue,
				FString::Printf(TEXT("Function output name must be unique: %s"), *Requested.ToString()));
		}
	}

	// Stage the value first so a refused value never leaves the expression half-edited.
	void* PropertyAddress = Property->ContainerPtrToValuePtr<void>(Expression);
	FMemMark Mark(FMemStack::Get());
	void* Staged = FMemStack::Get().Alloc(Property->GetSize(), Property->GetMinAlignment());
	Property->InitializeValue(Staged);
	ON_SCOPE_EXIT
	{
		Property->DestroyValue(Staged);
	};
	Property->CopyCompleteValue(Staged, PropertyAddress);

	// Staging retires the instanced sub-objects it replaces (marks them as garbage) outside any transaction. Revive
	// them so a refusal changes nothing; on success they are retired again below, where Undo can bring them back.
	TArray<UObject*> SubObjects;
	ForEachObjectWithOuter(Expression, [&SubObjects](UObject* SubObject)
	{
		if (IsValid(SubObject))
		{
			SubObjects.Add(SubObject);
		}
	});

	TArray<FString> Warnings;
	const bool bStaged = FCortexSerializer::JsonToProperty(Value, Property, Staged, Expression, Warnings);
	TArray<UObject*> Retired;
	for (UObject* SubObject : SubObjects)
	{
		if (!IsValid(SubObject))
		{
			SubObject->ClearGarbage();
			Retired.Add(SubObject);
		}
	}
	if (!bStaged)
	{
		FString WarningStr = Warnings.Num() > 0
			? FString::Join(Warnings, TEXT("; "))
			: TEXT("unsupported type");
		return FCortexCommandRouter::Error(
			CortexErrorCodes::InvalidField,
			FString::Printf(TEXT("Failed to set property '%s': %s"), *PropertyName, *WarningStr));
	}

	FScopedTransaction Transaction(FText::FromString(
		FString::Printf(TEXT("Cortex: Set Node Property %s"), *PropertyName)
	), !FCortexCommandRouter::IsInBatch());
	Target.BeginEdit();
	CortexMaterialInputUtils::ModifyExpression(Expression);
	for (UObject* SubObject : Retired)
	{
		SubObject->SetFlags(RF_Transactional);
		SubObject->Modify();
		SubObject->MarkAsGarbage();
	}
	if (!FCortexCommandRouter::IsInBatch())
	{
		Expression->PreEditChange(Property);
	}
	Property->CopyCompleteValue(PropertyAddress, Staged);
	if (!FCortexCommandRouter::IsInBatch())
	{
		FPropertyChangedEvent Event(Property);
		Expression->PostEditChangeProperty(Event);
	}
	Target.EndEdit();

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
	FCortexMaterialGraphTarget Target;
	if (!FCortexMaterialGraphTarget::Load(AssetPath, Target, LoadError))
	{
		return LoadError;
	}

	UMaterialExpression* Expression = Target.FindExpression(NodeId);
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
		Entry->SetStringField(TEXT("name"), CortexMaterialInputPinName(Expression, It.Index).ToString());
		// Report if this input is currently connected
		if (It.Input && It.Input->Expression)
		{
			Entry->SetStringField(TEXT("connected_to"), It.Input->Expression->GetName());
		}
		InputsArray.Add(MakeShared<FJsonValueObject>(Entry));
	}

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("asset_kind"), Target.GetKindName());
	Data->SetStringField(TEXT("node_id"), NodeId);
	Data->SetStringField(TEXT("expression_class"), Expression->GetClass()->GetName());
	Data->SetArrayField(TEXT("inputs"), InputsArray);
	Data->SetArrayField(TEXT("outputs"), OutputsArray);
	Data->SetNumberField(TEXT("input_count"), InputsArray.Num());
	Data->SetNumberField(TEXT("output_count"), OutputsArray.Num());

	return FCortexCommandRouter::Success(Data);
}
