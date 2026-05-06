#include "Operations/CortexBPComponentOps.h"
#include "Operations/CortexBPAssetOps.h"
#include "Operations/CortexBPSCSDiagnostics.h"
#include "CortexAssetFingerprint.h"
#include "CortexBatchMutation.h"
#include "CortexBlueprintModule.h"
#include "CortexSerializer.h"
#include "Components/ActorComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Dom/JsonValue.h"
#include "Engine/Blueprint.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "Engine/SCS_Node.h"
#include "Engine/StaticMesh.h"
#include "Engine/SimpleConstructionScript.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "GameFramework/Actor.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Misc/PackageName.h"
#include "ScopedTransaction.h"
#include "UObject/SavePackage.h"
#include "UObject/UnrealType.h"

namespace CortexBPComponentOpsPrivate
{
	UObject* FindOwnedSCSComponentTemplate(UBlueprint* Blueprint, const FString& ComponentName);

	TSharedPtr<FJsonObject> CopyJsonObject(const TSharedPtr<FJsonObject>& Source)
	{
		TSharedPtr<FJsonObject> Copy = MakeShared<FJsonObject>();
		if (!Source.IsValid())
		{
			return Copy;
		}

		for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : Source->Values)
		{
			Copy->SetField(Pair.Key, Pair.Value);
		}

		return Copy;
	}

	TArray<TSharedPtr<FJsonValue>> ToJsonStringArray(const TArray<FString>& Values)
	{
		TArray<TSharedPtr<FJsonValue>> Out;
		Out.Reserve(Values.Num());
		for (const FString& Value : Values)
		{
			Out.Add(MakeShared<FJsonValueString>(Value));
		}
		return Out;
	}

	FCortexAssetFingerprint MakeBlueprintFingerprint(const UBlueprint* Blueprint)
	{
		if (!Blueprint)
		{
			return MakeObjectAssetFingerprint(nullptr);
		}

		return MakeObjectAssetFingerprint(Blueprint, GetTypeHash(static_cast<uint32>(Blueprint->Status)));
	}

	FString ResolveReferenceForm(UBlueprint* Blueprint, const FString& ComponentName)
	{
		const FCortexBPSCSDiagnostics::FResolveResult ResolveResult =
			FCortexBPSCSDiagnostics::ResolveComponentTemplateByName(Blueprint, ComponentName);
		if (ResolveResult.Component)
		{
			return ResolveResult.Component->GetPathName();
		}

		if (Blueprint && Blueprint->GeneratedClass)
		{
			return FString::Printf(
				TEXT("%s:%s_GEN_VARIABLE"),
				*Blueprint->GeneratedClass->GetPathName(),
				*ComponentName);
		}

		return ComponentName;
	}

	TSharedPtr<FJsonObject> BuildBatchResponseData(const FCortexBatchMutationResult& BatchResult)
	{
		TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
		Data->SetStringField(TEXT("status"), BatchResult.Status);
		Data->SetArrayField(TEXT("written_targets"), ToJsonStringArray(BatchResult.WrittenTargets));
		Data->SetArrayField(TEXT("unwritten_targets"), ToJsonStringArray(BatchResult.UnwrittenTargets));

		TArray<TSharedPtr<FJsonValue>> PerItem;
		PerItem.Reserve(BatchResult.PerItem.Num());
		for (const FCortexBatchMutationItemResult& ItemResult : BatchResult.PerItem)
		{
			TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
			Entry->SetStringField(TEXT("target"), ItemResult.Target);
			Entry->SetBoolField(TEXT("success"), ItemResult.Result.bSuccess);
			if (ItemResult.Result.Data.IsValid())
			{
				Entry->SetObjectField(TEXT("data"), ItemResult.Result.Data);
			}
			if (!ItemResult.Result.ErrorCode.IsEmpty())
			{
				Entry->SetStringField(TEXT("error_code"), ItemResult.Result.ErrorCode);
			}
			if (!ItemResult.Result.ErrorMessage.IsEmpty())
			{
				Entry->SetStringField(TEXT("error_message"), ItemResult.Result.ErrorMessage);
			}
			if (ItemResult.Result.ErrorDetails.IsValid())
			{
				Entry->SetObjectField(TEXT("error_details"), ItemResult.Result.ErrorDetails);
			}
			if (ItemResult.Result.Warnings.Num() > 0)
			{
				Entry->SetArrayField(TEXT("warnings"), ToJsonStringArray(ItemResult.Result.Warnings));
			}
			PerItem.Add(MakeShared<FJsonValueObject>(Entry));
		}

		Data->SetArrayField(TEXT("per_item"), PerItem);
		return Data;
	}

	FCortexCommandResult MakeBatchCommandResult(const FCortexBatchMutationResult& BatchResult)
	{
		TSharedPtr<FJsonObject> Data = BuildBatchResponseData(BatchResult);
		if (BatchResult.Status == TEXT("committed"))
		{
			return FCortexCommandRouter::Success(Data);
		}

		return FCortexCommandRouter::Error(BatchResult.ErrorCode, BatchResult.ErrorMessage, Data);
	}

	FCortexBatchPreflightResult PreflightSetComponentDefaults(const FCortexBatchMutationItem& Item)
	{
		FString ValidationError;
		if (!FCortexBPAssetOps::ValidateWritableBlueprintAssetPath(Item.Target, ValidationError))
		{
			return FCortexBatchPreflightResult::Error(CortexErrorCodes::InvalidField, ValidationError);
		}

		FString LoadError;
		UBlueprint* Blueprint = FCortexBPAssetOps::LoadBlueprint(Item.Target, LoadError);
		if (!Blueprint)
		{
			return FCortexBatchPreflightResult::Error(CortexErrorCodes::BlueprintNotFound, LoadError);
		}

		FString ComponentName;
		if (!Item.Params.IsValid()
			|| !Item.Params->TryGetStringField(TEXT("component_name"), ComponentName)
			|| ComponentName.IsEmpty())
		{
			return FCortexBatchPreflightResult::Error(
				CortexErrorCodes::InvalidField,
				TEXT("Missing required params: component_name"));
		}

		if (!CortexBPComponentOpsPrivate::FindOwnedSCSComponentTemplate(Blueprint, ComponentName))
		{
			return FCortexBatchPreflightResult::Error(
				CortexErrorCodes::ComponentNotFound,
				FString::Printf(
					TEXT("Owned SCS component template not found: %s. set_component_defaults only mutates components owned by this Blueprint's SimpleConstructionScript."),
					*ComponentName));
		}

		const TSharedPtr<FJsonObject>* PropertiesObj = nullptr;
		if (!Item.Params->TryGetObjectField(TEXT("properties"), PropertiesObj)
			|| PropertiesObj == nullptr
			|| !(*PropertiesObj).IsValid())
		{
			return FCortexBatchPreflightResult::Error(
				CortexErrorCodes::InvalidField,
				TEXT("Missing required param: properties"));
		}

		return FCortexBatchPreflightResult::Success(MakeBlueprintFingerprint(Blueprint).ToJson());
	}

	FCortexCommandResult CommitSetComponentDefaults(const FCortexBatchMutationItem& Item)
	{
		TSharedPtr<FJsonObject> ItemParams = CopyJsonObject(Item.Params);
		ItemParams->SetStringField(TEXT("asset_path"), Item.Target);
		return FCortexBPComponentOps::SetComponentDefaults(ItemParams);
	}

	struct FResolvedPropertyPath
	{
		FString BasePropertyName;
		bool bHasArrayIndex = false;
		int32 ArrayIndex = INDEX_NONE;
	};

	bool ParsePropertyPath(const FString& PropertyPath, FResolvedPropertyPath& OutPath)
	{
		OutPath = {};
		OutPath.BasePropertyName = PropertyPath;

		int32 OpenBracketIndex = INDEX_NONE;
		if (!PropertyPath.FindChar(TEXT('['), OpenBracketIndex))
		{
			return !PropertyPath.IsEmpty();
		}

		int32 CloseBracketIndex = INDEX_NONE;
		if (!PropertyPath.FindLastChar(TEXT(']'), CloseBracketIndex)
			|| CloseBracketIndex <= OpenBracketIndex
			|| CloseBracketIndex != PropertyPath.Len() - 1)
		{
			return false;
		}

		OutPath.BasePropertyName = PropertyPath.Left(OpenBracketIndex);
		const FString IndexText = PropertyPath.Mid(OpenBracketIndex + 1, CloseBracketIndex - OpenBracketIndex - 1);
		if (OutPath.BasePropertyName.IsEmpty() || IndexText.IsEmpty())
		{
			return false;
		}

		int32 ParsedIndex = INDEX_NONE;
		if (!LexTryParseString(ParsedIndex, *IndexText) || ParsedIndex < 0)
		{
			return false;
		}

		OutPath.bHasArrayIndex = true;
		OutPath.ArrayIndex = ParsedIndex;
		return true;
	}

	bool IsBlueprintCompiled(UBlueprint* Blueprint)
	{
		return Blueprint != nullptr
			&& (Blueprint->Status == BS_UpToDate || Blueprint->Status == BS_UpToDateWithWarnings);
	}

	TSharedPtr<FJsonObject> BuildBlueprintCompileDetails(UBlueprint* Blueprint)
	{
		TArray<TSharedPtr<FJsonValue>> ErrorsArray;
		TArray<TSharedPtr<FJsonValue>> WarningsArray;

		if (Blueprint == nullptr)
		{
			TSharedPtr<FJsonObject> Details = MakeShared<FJsonObject>();
			Details->SetArrayField(TEXT("errors"), ErrorsArray);
			Details->SetArrayField(TEXT("warnings"), WarningsArray);
			return Details;
		}

		TArray<UEdGraph*> AllGraphs;
		Blueprint->GetAllGraphs(AllGraphs);
		for (UEdGraph* Graph : AllGraphs)
		{
			if (Graph == nullptr)
			{
				continue;
			}

			for (UEdGraphNode* Node : Graph->Nodes)
			{
				if (Node == nullptr || !Node->bHasCompilerMessage)
				{
					continue;
				}

				TSharedPtr<FJsonObject> MsgObj = MakeShared<FJsonObject>();
				MsgObj->SetStringField(TEXT("node"), Node->GetName());
				MsgObj->SetStringField(TEXT("message"), Node->ErrorMsg);
				if (Node->ErrorType <= EMessageSeverity::Error)
				{
					ErrorsArray.Add(MakeShared<FJsonValueObject>(MsgObj));
				}
				else
				{
					WarningsArray.Add(MakeShared<FJsonValueObject>(MsgObj));
				}
			}
		}

		TSharedPtr<FJsonObject> Details = MakeShared<FJsonObject>();
		Details->SetArrayField(TEXT("errors"), ErrorsArray);
		Details->SetArrayField(TEXT("warnings"), WarningsArray);
		return Details;
	}

	UObject* FindOwnedSCSComponentTemplate(UBlueprint* Blueprint, const FString& ComponentName)
	{
		if (Blueprint == nullptr || ComponentName.IsEmpty())
		{
			return nullptr;
		}

		if (USimpleConstructionScript* SCS = Blueprint->SimpleConstructionScript)
		{
			for (USCS_Node* Node : SCS->GetAllNodes())
			{
				if (Node == nullptr || Node->ComponentTemplate == nullptr)
				{
					continue;
				}

				if (Node->GetVariableName().ToString() == ComponentName
					|| Node->ComponentTemplate->GetName() == ComponentName)
				{
					return Node->ComponentTemplate;
				}
			}
		}

		return nullptr;
	}

	bool IsEditableComponentTemplateProperty(const FProperty* Property)
	{
		return Property != nullptr
			&& Property->HasAnyPropertyFlags(CPF_Edit)
			&& !Property->HasAnyPropertyFlags(CPF_Deprecated | CPF_DisableEditOnTemplate | CPF_EditConst);
	}

	bool ContainsInstancedReferenceProperty(const FProperty* Property)
	{
		if (Property == nullptr)
		{
			return false;
		}

		if (Property->HasAnyPropertyFlags(CPF_InstancedReference | CPF_ContainsInstancedReference))
		{
			return true;
		}

		if (const FArrayProperty* ArrayProperty = CastField<FArrayProperty>(Property))
		{
			return ContainsInstancedReferenceProperty(ArrayProperty->Inner);
		}

		if (const FSetProperty* SetProperty = CastField<FSetProperty>(Property))
		{
			return ContainsInstancedReferenceProperty(SetProperty->ElementProp);
		}

		if (const FMapProperty* MapProperty = CastField<FMapProperty>(Property))
		{
			return ContainsInstancedReferenceProperty(MapProperty->KeyProp)
				|| ContainsInstancedReferenceProperty(MapProperty->ValueProp);
		}

		return false;
	}

	void AddPropertyError(
		TArray<TSharedPtr<FJsonValue>>& ErrorsArray,
		const FString& PropertyName,
		const FString& Message)
	{
		ErrorsArray.Add(MakeShared<FJsonValueString>(
			FString::Printf(TEXT("%s: %s"), *PropertyName, *Message)));
	}

	bool LoadObjectPathForProperty(
		const FString& PropertyName,
		const TSharedPtr<FJsonValue>& JsonValue,
		UClass* ExpectedClass,
		UObject*& OutObject,
		FString& OutError)
	{
		OutObject = nullptr;
		if (!JsonValue.IsValid() || JsonValue->Type != EJson::String)
		{
			OutError = TEXT("Expected object path string");
			return false;
		}

		const FString ObjectPath = JsonValue->AsString();
		if (ObjectPath.IsEmpty())
		{
			return true;
		}

		FText PathReason;
		if (!FPackageName::IsValidObjectPath(ObjectPath, &PathReason))
		{
			OutError = FString::Printf(
				TEXT("Invalid object path '%s': %s"),
				*ObjectPath,
				*PathReason.ToString());
			return false;
		}

		const FString PackageName = FPackageName::ObjectPathToPackageName(ObjectPath);
		if (!FPackageName::IsValidLongPackageName(PackageName)
			|| (!FindPackage(nullptr, *PackageName) && !FPackageName::DoesPackageExist(PackageName)))
		{
			OutError = FString::Printf(TEXT("Asset package not found for '%s'"), *ObjectPath);
			return false;
		}

		OutObject = StaticLoadObject(ExpectedClass ? ExpectedClass : UObject::StaticClass(), nullptr, *ObjectPath);
		if (OutObject == nullptr)
		{
			OutError = FString::Printf(
				TEXT("Failed to load '%s' as %s"),
				*ObjectPath,
				ExpectedClass ? *ExpectedClass->GetName() : TEXT("UObject"));
			return false;
		}

		return true;
	}

	bool IsIntegralJsonNumber(double Number)
	{
		return FMath::IsFinite(Number) && FMath::IsNearlyEqual(Number, FMath::RoundToDouble(Number));
	}

	bool ValidateIntegerJsonValue(
		const FNumericProperty* NumericProperty,
		const TSharedPtr<FJsonValue>& JsonValue,
		const FString& PropertyPath,
		TArray<FString>& OutWarnings)
	{
		if (!JsonValue.IsValid() || JsonValue->Type != EJson::Number)
		{
			OutWarnings.Add(FString::Printf(TEXT("%s expected a numeric JSON value"), *PropertyPath));
			return false;
		}

		const double Number = JsonValue->AsNumber();
		if (!IsIntegralJsonNumber(Number))
		{
			OutWarnings.Add(FString::Printf(TEXT("%s expected an integer JSON value"), *PropertyPath));
			return false;
		}

		bool bInRange = true;
		if (CastField<FInt8Property>(NumericProperty))
		{
			bInRange = Number >= TNumericLimits<int8>::Min() && Number <= TNumericLimits<int8>::Max();
		}
		else if (CastField<FInt16Property>(NumericProperty))
		{
			bInRange = Number >= TNumericLimits<int16>::Min() && Number <= TNumericLimits<int16>::Max();
		}
		else if (CastField<FIntProperty>(NumericProperty))
		{
			bInRange = Number >= TNumericLimits<int32>::Min() && Number <= TNumericLimits<int32>::Max();
		}
		else if (CastField<FInt64Property>(NumericProperty))
		{
			bInRange = Number >= static_cast<double>(TNumericLimits<int64>::Min())
				&& Number <= static_cast<double>(TNumericLimits<int64>::Max());
		}
		else if (CastField<FByteProperty>(NumericProperty))
		{
			bInRange = Number >= 0.0 && Number <= static_cast<double>(TNumericLimits<uint8>::Max());
		}
		else if (CastField<FUInt16Property>(NumericProperty))
		{
			bInRange = Number >= 0.0 && Number <= static_cast<double>(TNumericLimits<uint16>::Max());
		}
		else if (CastField<FUInt32Property>(NumericProperty))
		{
			bInRange = Number >= 0.0 && Number <= static_cast<double>(TNumericLimits<uint32>::Max());
		}
		else if (CastField<FUInt64Property>(NumericProperty))
		{
			bInRange = Number >= 0.0 && Number <= static_cast<double>(TNumericLimits<uint64>::Max());
		}

		if (!bInRange)
		{
			OutWarnings.Add(FString::Printf(TEXT("%s numeric value is out of range"), *PropertyPath));
			return false;
		}

		return true;
	}

	bool ValidateEnumJsonValue(
		const UEnum* Enum,
		const TSharedPtr<FJsonValue>& JsonValue,
		const FString& PropertyPath,
		TArray<FString>& OutWarnings)
	{
		if (Enum == nullptr || !JsonValue.IsValid())
		{
			OutWarnings.Add(FString::Printf(TEXT("%s has no enum definition"), *PropertyPath));
			return false;
		}

		int64 EnumValue = INDEX_NONE;
		FString InputValue;
		if (JsonValue->Type == EJson::Number)
		{
			const double Number = JsonValue->AsNumber();
			if (!IsIntegralJsonNumber(Number))
			{
				OutWarnings.Add(FString::Printf(TEXT("%s expected an integer enum value"), *PropertyPath));
				return false;
			}
			EnumValue = static_cast<int64>(Number);
			InputValue = FString::Printf(TEXT("%lld"), static_cast<long long>(EnumValue));
		}
		else if (JsonValue->Type == EJson::String)
		{
			InputValue = JsonValue->AsString();
			EnumValue = Enum->GetValueByNameString(InputValue);
		}
		else
		{
			OutWarnings.Add(FString::Printf(TEXT("%s expected an enum string or numeric value"), *PropertyPath));
			return false;
		}

		const int32 EnumIndex = Enum->GetIndexByValue(EnumValue);
		if (EnumValue == INDEX_NONE
			|| EnumIndex == INDEX_NONE
			|| Enum->HasMetaData(TEXT("Hidden"), EnumIndex))
		{
			OutWarnings.Add(FString::Printf(
				TEXT("%s unknown enum value '%s' for %s"),
				*PropertyPath,
				*InputValue,
				*Enum->GetName()));
			return false;
		}

		return true;
	}

	bool ValidateStrictComponentJsonValue(
		const FProperty* Property,
		const TSharedPtr<FJsonValue>& JsonValue,
		const FString& PropertyPath,
		TArray<FString>& OutWarnings)
	{
		if (const FBoolProperty* BoolProperty = CastField<FBoolProperty>(Property))
		{
			if (!JsonValue.IsValid() || JsonValue->Type != EJson::Boolean)
			{
				OutWarnings.Add(FString::Printf(TEXT("%s expected a boolean JSON value"), *PropertyPath));
				return false;
			}
			return true;
		}

		if (const FEnumProperty* EnumProperty = CastField<FEnumProperty>(Property))
		{
			return ValidateEnumJsonValue(EnumProperty->GetEnum(), JsonValue, PropertyPath, OutWarnings);
		}

		if (const FByteProperty* ByteProperty = CastField<FByteProperty>(Property))
		{
			if (const UEnum* Enum = ByteProperty->GetIntPropertyEnum())
			{
				return ValidateEnumJsonValue(Enum, JsonValue, PropertyPath, OutWarnings);
			}
		}

		if (const FNumericProperty* NumericProperty = CastField<FNumericProperty>(Property))
		{
			if (NumericProperty->IsInteger())
			{
				return ValidateIntegerJsonValue(NumericProperty, JsonValue, PropertyPath, OutWarnings);
			}

			if (!JsonValue.IsValid() || JsonValue->Type != EJson::Number || !FMath::IsFinite(JsonValue->AsNumber()))
			{
				OutWarnings.Add(FString::Printf(TEXT("%s expected a finite numeric JSON value"), *PropertyPath));
				return false;
			}
			return true;
		}

		if (CastField<FArrayProperty>(Property) || CastField<FSetProperty>(Property) || CastField<FMapProperty>(Property))
		{
			OutWarnings.Add(FString::Printf(
				TEXT("%s container properties are not supported by set_component_defaults; use supported indexed object-reference paths instead"),
				*PropertyPath));
			return false;
		}

		if (const FStructProperty* StructProperty = CastField<FStructProperty>(Property))
		{
			const TSharedPtr<FJsonObject>* ObjectValue = nullptr;
			if (!JsonValue.IsValid() || !JsonValue->TryGetObject(ObjectValue) || ObjectValue == nullptr || !(*ObjectValue).IsValid())
			{
				OutWarnings.Add(FString::Printf(TEXT("%s expected a JSON object"), *PropertyPath));
				return false;
			}

			bool bValid = true;
			for (const TPair<FString, TSharedPtr<FJsonValue>>& FieldPair : (*ObjectValue)->Values)
			{
				FProperty* StructField = StructProperty->Struct
					? StructProperty->Struct->FindPropertyByName(FName(*FieldPair.Key))
					: nullptr;
				if (StructField == nullptr)
				{
					OutWarnings.Add(FString::Printf(
						TEXT("%s.%s is not a field on %s"),
						*PropertyPath,
						*FieldPair.Key,
						StructProperty->Struct ? *StructProperty->Struct->GetName() : TEXT("struct")));
					bValid = false;
					continue;
				}

				bValid &= ValidateStrictComponentJsonValue(
					StructField,
					FieldPair.Value,
					FString::Printf(TEXT("%s.%s"), *PropertyPath, *FieldPair.Key),
					OutWarnings);
			}

			return bValid;
		}

		return true;
	}

	bool ValidateGenericPropertyJson(
		UObject* ComponentTemplate,
		FProperty* Property,
		const FString& PropertyName,
		const TSharedPtr<FJsonValue>& JsonValue,
		TArray<FString>& OutWarnings)
	{
		if (!ValidateStrictComponentJsonValue(Property, JsonValue, PropertyName, OutWarnings))
		{
			return false;
		}

		void* ValuePtr = Property->ContainerPtrToValuePtr<void>(ComponentTemplate);
		void* TempValuePtr = FMemory::Malloc(Property->GetSize(), Property->GetMinAlignment());
		Property->InitializeValue(TempValuePtr);
		Property->CopyCompleteValue(TempValuePtr, ValuePtr);

		const bool bSuccess = FCortexSerializer::JsonToProperty(
			JsonValue,
			Property,
			TempValuePtr,
			ComponentTemplate,
			OutWarnings);

		Property->DestroyValue(TempValuePtr);
		FMemory::Free(TempValuePtr);
		return bSuccess && OutWarnings.Num() == 0;
	}

	bool SaveBlueprintPackage(UBlueprint* Blueprint, FString& OutError)
	{
		if (Blueprint == nullptr)
		{
			OutError = TEXT("Blueprint is null");
			return false;
		}

		UPackage* Package = Blueprint->GetOutermost();
		if (Package == nullptr)
		{
			OutError = TEXT("Blueprint package is null");
			return false;
		}

		FString PackageFilename;
		if (!FPackageName::TryConvertLongPackageNameToFilename(
				Package->GetName(),
				PackageFilename,
				FPackageName::GetAssetPackageExtension()))
		{
			OutError = FString::Printf(TEXT("Failed to resolve package filename for: %s"), *Package->GetName());
			return false;
		}

		FSavePackageArgs SaveArgs;
		SaveArgs.TopLevelFlags = RF_Public | RF_Standalone;
		if (!UPackage::SavePackage(Package, Blueprint, *PackageFilename, SaveArgs))
		{
			OutError = FString::Printf(TEXT("Failed to save Blueprint package: %s"), *Package->GetName());
			return false;
		}

		return true;
	}
}

FCortexCommandResult FCortexBPComponentOps::ListSCSComponents(const TSharedPtr<FJsonObject>& Params)
{
	if (!Params.IsValid())
	{
		return FCortexCommandRouter::Error(CortexErrorCodes::InvalidField, TEXT("Missing params object"));
	}

	FString BlueprintPath;
	if (!Params->TryGetStringField(TEXT("asset_path"), BlueprintPath) || BlueprintPath.IsEmpty())
	{
		return FCortexCommandRouter::Error(
			CortexErrorCodes::InvalidField,
			TEXT("Missing or empty 'asset_path' field"));
	}

	FString LoadError;
	UBlueprint* Blueprint = FCortexBPAssetOps::LoadBlueprint(BlueprintPath, LoadError);
	if (!Blueprint)
	{
		return FCortexCommandRouter::Error(CortexErrorCodes::BlueprintNotFound, LoadError);
	}

	TArray<TSharedPtr<FJsonValue>> ComponentsArray;
	if (USimpleConstructionScript* SCS = Blueprint->SimpleConstructionScript)
	{
		for (USCS_Node* Node : SCS->GetAllNodes())
		{
			if (!Node)
			{
				continue;
			}

			TSharedPtr<FJsonObject> ComponentJson = MakeShared<FJsonObject>();
			const FString ComponentName = Node->GetVariableName().ToString();
			ComponentJson->SetStringField(TEXT("name"), ComponentName);

			UClass* ComponentClass = Node->ComponentClass;
			if (!ComponentClass && Node->ComponentTemplate)
			{
				ComponentClass = Node->ComponentTemplate->GetClass();
			}
			ComponentJson->SetStringField(TEXT("class"), ComponentClass ? ComponentClass->GetName() : TEXT(""));
			ComponentJson->SetStringField(
				TEXT("reference_form"),
				CortexBPComponentOpsPrivate::ResolveReferenceForm(Blueprint, ComponentName));
			ComponentsArray.Add(MakeShared<FJsonValueObject>(ComponentJson));
		}
	}

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("asset_path"), BlueprintPath);
	Data->SetArrayField(TEXT("components"), ComponentsArray);
	Data->SetNumberField(TEXT("count"), ComponentsArray.Num());
	return FCortexCommandRouter::Success(Data);
}

FCortexCommandResult FCortexBPComponentOps::SetComponentDefaults(const TSharedPtr<FJsonObject>& Params)
{
	if (Params.IsValid() && Params->HasField(TEXT("items")))
	{
		FCortexBatchMutationRequest Request;
		FCortexCommandResult ParseError;
		if (!FCortexBatchMutation::ParseRequest(Params, TEXT("asset_path"), Request, ParseError))
		{
			return ParseError;
		}

		return CortexBPComponentOpsPrivate::MakeBatchCommandResult(FCortexBatchMutation::Run(
			Request,
			CortexBPComponentOpsPrivate::PreflightSetComponentDefaults,
			CortexBPComponentOpsPrivate::CommitSetComponentDefaults));
	}

	if (Params.IsValid() && Params->HasField(TEXT("expected_fingerprint")))
	{
		FCortexBatchMutationRequest Request;
		FCortexCommandResult ParseError;
		if (!FCortexBatchMutation::ParseRequest(Params, TEXT("asset_path"), Request, ParseError))
		{
			return ParseError;
		}

		const FCortexBatchMutationResult BatchResult = FCortexBatchMutation::Run(
			Request,
			CortexBPComponentOpsPrivate::PreflightSetComponentDefaults,
			CortexBPComponentOpsPrivate::CommitSetComponentDefaults);
		if (BatchResult.PerItem.Num() > 0)
		{
			return BatchResult.PerItem[0].Result;
		}

		return FCortexCommandRouter::Error(
			CortexErrorCodes::InvalidField,
			TEXT("No set_component_defaults target was parsed"));
	}

	FString AssetPath;
	FString ComponentName;
	if (!Params.IsValid()
		|| !Params->TryGetStringField(TEXT("asset_path"), AssetPath)
		|| !Params->TryGetStringField(TEXT("component_name"), ComponentName))
	{
		return FCortexCommandRouter::Error(
			CortexErrorCodes::InvalidField,
			TEXT("Missing required params: asset_path, component_name")
		);
	}

	const TSharedPtr<FJsonObject>* PropertiesObj = nullptr;
	if (!Params->TryGetObjectField(TEXT("properties"), PropertiesObj)
		|| PropertiesObj == nullptr
		|| !(*PropertiesObj).IsValid())
	{
		return FCortexCommandRouter::Error(
			CortexErrorCodes::InvalidField,
			TEXT("Missing required param: properties")
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

	UObject* ComponentTemplate = CortexBPComponentOpsPrivate::FindOwnedSCSComponentTemplate(Blueprint, ComponentName);
	if (ComponentTemplate == nullptr)
	{
		return FCortexCommandRouter::Error(
			CortexErrorCodes::ComponentNotFound,
			FString::Printf(
				TEXT("Owned SCS component template not found: %s. set_component_defaults only mutates components owned by this Blueprint's SimpleConstructionScript."),
				*ComponentName)
		);
	}

	bool bCompile = true;
	Params->TryGetBoolField(TEXT("compile"), bCompile);

	bool bSave = false;
	Params->TryGetBoolField(TEXT("save"), bSave);

	TUniquePtr<FScopedTransaction> Transaction;
	const auto EnsureMutationStarted = [&Transaction, ComponentTemplate, Blueprint, &ComponentName]()
	{
		if (Transaction.IsValid())
		{
			return;
		}

		Transaction = MakeUnique<FScopedTransaction>(FText::FromString(
			FString::Printf(TEXT("Cortex: Set Component Defaults %s"), *ComponentName)));
		ComponentTemplate->Modify();
		Blueprint->Modify();
	};

	int32 PropertiesSet = 0;
	TArray<TSharedPtr<FJsonValue>> ErrorsArray;

	for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : (*PropertiesObj)->Values)
	{
		const FString& PropName = Pair.Key;
		const TSharedPtr<FJsonValue>& PropValueJson = Pair.Value;
		CortexBPComponentOpsPrivate::FResolvedPropertyPath PropertyPath;
		if (!CortexBPComponentOpsPrivate::ParsePropertyPath(PropName, PropertyPath))
		{
			ErrorsArray.Add(MakeShared<FJsonValueString>(
				FString::Printf(TEXT("Invalid property path: %s"), *PropName)));
			continue;
		}

		FProperty* Prop = FindFProperty<FProperty>(ComponentTemplate->GetClass(), *PropertyPath.BasePropertyName);
		if (Prop == nullptr)
		{
			CortexBPComponentOpsPrivate::AddPropertyError(
				ErrorsArray,
				PropName,
				FString::Printf(TEXT("Property not found: %s"), *PropertyPath.BasePropertyName));
			continue;
		}

		if (!CortexBPComponentOpsPrivate::IsEditableComponentTemplateProperty(Prop))
		{
			CortexBPComponentOpsPrivate::AddPropertyError(
				ErrorsArray,
				PropName,
				TEXT("Property is not editable on component templates"));
			continue;
		}

		if (CortexBPComponentOpsPrivate::ContainsInstancedReferenceProperty(Prop))
		{
			CortexBPComponentOpsPrivate::AddPropertyError(
				ErrorsArray,
				PropName,
				TEXT("instanced reference properties are not supported by set_component_defaults"));
			continue;
		}

		bool bApplied = false;
		FString ApplyError;
		if (PropertyPath.bHasArrayIndex)
		{
			FArrayProperty* ArrayProp = CastField<FArrayProperty>(Prop);
			if (ArrayProp == nullptr)
			{
				CortexBPComponentOpsPrivate::AddPropertyError(
					ErrorsArray,
					PropName,
					FString::Printf(TEXT("Property '%s' is not an array"), *PropertyPath.BasePropertyName));
				continue;
			}

			FObjectPropertyBase* InnerObjectProp = CastField<FObjectPropertyBase>(ArrayProp->Inner);
			if (InnerObjectProp == nullptr)
			{
				CortexBPComponentOpsPrivate::AddPropertyError(
					ErrorsArray,
					PropName,
					FString::Printf(TEXT("Array property '%s' does not store object references"), *PropertyPath.BasePropertyName));
				continue;
			}

			UObject* LoadedObject = nullptr;
			if (!CortexBPComponentOpsPrivate::LoadObjectPathForProperty(
					PropName,
					PropValueJson,
					InnerObjectProp->PropertyClass,
					LoadedObject,
					ApplyError))
			{
				CortexBPComponentOpsPrivate::AddPropertyError(
					ErrorsArray,
					PropName,
					ApplyError.IsEmpty() ? TEXT("Failed to load object reference") : ApplyError);
				continue;
			}

			EnsureMutationStarted();
			ComponentTemplate->PreEditChange(Prop);
			FScriptArrayHelper ArrayHelper(ArrayProp, ArrayProp->ContainerPtrToValuePtr<void>(ComponentTemplate));
			while (ArrayHelper.Num() <= PropertyPath.ArrayIndex)
			{
				ArrayHelper.AddValue();
			}
			InnerObjectProp->SetObjectPropertyValue(ArrayHelper.GetRawPtr(PropertyPath.ArrayIndex), LoadedObject);
			bApplied = true;
		}
		else if (PropertyPath.BasePropertyName == TEXT("StaticMesh"))
		{
			UStaticMeshComponent* StaticMeshComponent = Cast<UStaticMeshComponent>(ComponentTemplate);
			if (StaticMeshComponent == nullptr)
			{
				CortexBPComponentOpsPrivate::AddPropertyError(
					ErrorsArray,
					PropName,
					TEXT("StaticMesh can only be set on UStaticMeshComponent templates"));
				continue;
			}

			UObject* LoadedObject = nullptr;
			if (!CortexBPComponentOpsPrivate::LoadObjectPathForProperty(
					PropName,
					PropValueJson,
					UStaticMesh::StaticClass(),
					LoadedObject,
					ApplyError))
			{
				CortexBPComponentOpsPrivate::AddPropertyError(
					ErrorsArray,
					PropName,
					ApplyError.IsEmpty() ? TEXT("Failed to load StaticMesh") : ApplyError);
				continue;
			}

			UStaticMesh* StaticMesh = Cast<UStaticMesh>(LoadedObject);
			if (LoadedObject != nullptr && StaticMesh == nullptr)
			{
				CortexBPComponentOpsPrivate::AddPropertyError(
					ErrorsArray,
					PropName,
					TEXT("Loaded asset is not a UStaticMesh"));
				continue;
			}

			EnsureMutationStarted();
			ComponentTemplate->PreEditChange(Prop);
			StaticMeshComponent->SetStaticMesh(StaticMesh);
			bApplied = true;
		}
		else
		{
			void* ValuePtr = Prop->ContainerPtrToValuePtr<void>(ComponentTemplate);
			TArray<FString> SetWarnings;
			if (!CortexBPComponentOpsPrivate::ValidateGenericPropertyJson(
					ComponentTemplate,
					Prop,
					PropName,
					PropValueJson,
					SetWarnings))
			{
				ApplyError = SetWarnings.Num() > 0
					? FString::Join(SetWarnings, TEXT("; "))
					: FString::Printf(TEXT("Failed to deserialize JSON value as %s"), *Prop->GetCPPType());
				CortexBPComponentOpsPrivate::AddPropertyError(ErrorsArray, PropName, ApplyError);
				continue;
			}

			const TSharedPtr<FJsonValue> PreviousValue = FCortexSerializer::PropertyToJson(Prop, ValuePtr);
			EnsureMutationStarted();
			ComponentTemplate->PreEditChange(Prop);
			SetWarnings.Reset();
			const bool bSerializerSucceeded = FCortexSerializer::JsonToProperty(
				PropValueJson,
				Prop,
				ValuePtr,
				ComponentTemplate,
				SetWarnings);
			bApplied = bSerializerSucceeded && SetWarnings.Num() == 0;
			if (!bApplied)
			{
				ApplyError = SetWarnings.Num() > 0
					? FString::Join(SetWarnings, TEXT("; "))
					: FString::Printf(TEXT("Failed to deserialize JSON value as %s"), *Prop->GetCPPType());
				if (PreviousValue.IsValid())
				{
					TArray<FString> RestoreWarnings;
					FCortexSerializer::JsonToProperty(
						PreviousValue,
						Prop,
						ValuePtr,
						ComponentTemplate,
						RestoreWarnings);
				}
			}
		}

		if (!bApplied)
		{
			CortexBPComponentOpsPrivate::AddPropertyError(
				ErrorsArray,
				PropName,
				ApplyError.IsEmpty() ? TEXT("Failed to apply property") : ApplyError);
			continue;
		}

		FPropertyChangedEvent ChangedEvent(Prop, EPropertyChangeType::ValueSet);
		ComponentTemplate->PostEditChangeProperty(ChangedEvent);

		++PropertiesSet;
	}

	const bool bPartialFailure = ErrorsArray.Num() > 0;
	bool bDidCompile = false;
	bool bDidSave = false;

	if (PropertiesSet > 0)
	{
		FBlueprintEditorUtils::MarkBlueprintAsModified(Blueprint);

		if (bCompile)
		{
			FKismetEditorUtilities::CompileBlueprint(Blueprint);
			bDidCompile = CortexBPComponentOpsPrivate::IsBlueprintCompiled(Blueprint);
			if (!bDidCompile)
			{
				return FCortexCommandRouter::Error(
					CortexErrorCodes::CompileFailed,
					FString::Printf(TEXT("Blueprint compilation failed after setting component defaults: %s"), *AssetPath),
					CortexBPComponentOpsPrivate::BuildBlueprintCompileDetails(Blueprint));
			}
		}

		if (bSave && !bPartialFailure)
		{
			FString SaveError;
			if (!CortexBPComponentOpsPrivate::SaveBlueprintPackage(Blueprint, SaveError))
			{
				return FCortexCommandRouter::Error(CortexErrorCodes::SaveFailed, SaveError);
			}
			bDidSave = true;
		}
	}

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("component_name"), ComponentName);
	Data->SetNumberField(TEXT("properties_set"), PropertiesSet);
	Data->SetBoolField(TEXT("partial_failure"), bPartialFailure);
	Data->SetArrayField(TEXT("errors"), ErrorsArray);
	Data->SetBoolField(TEXT("compiled"), bCompile && bDidCompile);
	Data->SetBoolField(TEXT("saved"), bSave && bDidSave);

	UE_LOG(LogCortexBlueprint, Log, TEXT("Set %d component default properties on '%s' in %s"),
		PropertiesSet, *ComponentName, *AssetPath);

	return FCortexCommandRouter::Success(Data);
}

FCortexCommandResult FCortexBPComponentOps::AddSCSComponent(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath;
	FString ComponentClassName;
	if (!Params.IsValid()
		|| !Params->TryGetStringField(TEXT("asset_path"), AssetPath) || AssetPath.IsEmpty()
		|| !Params->TryGetStringField(TEXT("component_class"), ComponentClassName) || ComponentClassName.IsEmpty())
	{
		return FCortexCommandRouter::Error(
			CortexErrorCodes::InvalidField,
			TEXT("Missing required params: asset_path, component_class"));
	}

	FString ValidationError;
	if (!FCortexBPAssetOps::ValidateWritableBlueprintAssetPath(AssetPath, ValidationError))
	{
		return FCortexCommandRouter::Error(CortexErrorCodes::InvalidField, ValidationError);
	}

	FString LoadError;
	UBlueprint* BP = FCortexBPAssetOps::LoadBlueprint(AssetPath, LoadError);
	if (!BP)
	{
		return FCortexCommandRouter::Error(CortexErrorCodes::BlueprintNotFound, LoadError);
	}

	USimpleConstructionScript* SCS = BP->SimpleConstructionScript;
	if (!SCS)
	{
		return FCortexCommandRouter::Error(
			CortexErrorCodes::InvalidField,
			TEXT("Blueprint has no SimpleConstructionScript"));
	}

	// Resolve component class — try direct path first, then /Script/Engine. prefix
	UClass* ComponentClass = FindObject<UClass>(nullptr, *ComponentClassName);
	if (!ComponentClass && !ComponentClassName.StartsWith(TEXT("/")))
	{
		const FString EnginePath = FString::Printf(TEXT("/Script/Engine.%s"), *ComponentClassName);
		ComponentClass = FindObject<UClass>(nullptr, *EnginePath);
	}
	if (!ComponentClass || !ComponentClass->IsChildOf(UActorComponent::StaticClass()))
	{
		return FCortexCommandRouter::Error(
			CortexErrorCodes::InvalidField,
			FString::Printf(TEXT("Invalid component class: %s (must be a UActorComponent subclass)"), *ComponentClassName));
	}

	const bool bIsSceneComponent = ComponentClass->IsChildOf(USceneComponent::StaticClass());

	// Determine component variable name
	FString ComponentName;
	if (!Params->TryGetStringField(TEXT("component_name"), ComponentName) || ComponentName.IsEmpty())
	{
		ComponentName = ComponentClass->GetName();
	}

	// Resolve optional parent node
	FString ParentComponentName;
	USCS_Node* ParentNode = nullptr;
	if (Params->TryGetStringField(TEXT("parent_component"), ParentComponentName) && !ParentComponentName.IsEmpty())
	{
		// Only scene components can be attached to a parent
		if (!bIsSceneComponent)
		{
			return FCortexCommandRouter::Error(
				CortexErrorCodes::InvalidField,
				FString::Printf(TEXT("Only SceneComponent subclasses can be attached to a parent. %s is not a SceneComponent."),
					*ComponentClassName));
		}

		for (USCS_Node* Node : SCS->GetAllNodes())
		{
			if (Node && Node->GetVariableName() == FName(*ParentComponentName))
			{
				ParentNode = Node;
				break;
			}
		}

		if (!ParentNode)
		{
			return FCortexCommandRouter::Error(
				CortexErrorCodes::ComponentNotFound,
				FString::Printf(TEXT("Parent SCS component not found: %s"), *ParentComponentName));
		}
	}

	const bool bCompile = Params->HasField(TEXT("compile")) ? Params->GetBoolField(TEXT("compile")) : true;

	FScopedTransaction Transaction(FText::FromString(
		FString::Printf(TEXT("Cortex: Add SCS Component %s to %s"), *ComponentName, *BP->GetName())));

	BP->Modify();
	SCS->Modify();

	USCS_Node* NewNode = SCS->CreateNode(ComponentClass, FName(*ComponentName));
	if (!NewNode)
	{
		return FCortexCommandRouter::Error(
			CortexErrorCodes::InvalidField,
			FString::Printf(TEXT("Failed to create SCS node for class: %s"), *ComponentClassName));
	}

	// Scene components without an explicit parent are attached to DefaultSceneRoot,
	// matching UE editor drag-and-drop behavior. Non-scene components are added as roots.
	if (!ParentNode && bIsSceneComponent)
	{
		ParentNode = SCS->GetDefaultSceneRootNode();
	}

	if (ParentNode)
	{
		ParentNode->AddChildNode(NewNode);
	}
	else
	{
		SCS->AddNode(NewNode);
	}

	FBlueprintEditorUtils::MarkBlueprintAsModified(BP);

	TSharedPtr<FJsonObject> ResponseData = MakeShared<FJsonObject>();
	ResponseData->SetStringField(TEXT("variable_name"), NewNode->GetVariableName().ToString());
	ResponseData->SetStringField(TEXT("component_class"), ComponentClass->GetName());
	ResponseData->SetBoolField(TEXT("is_scene_component"), bIsSceneComponent);

	if (ParentNode)
	{
		ResponseData->SetStringField(TEXT("parent_component"), ParentNode->GetVariableName().ToString());
	}

	if (bCompile)
	{
		FKismetEditorUtilities::CompileBlueprint(BP);
		ResponseData->SetBoolField(TEXT("compiled"), true);
		ResponseData->SetStringField(TEXT("compile_status"),
			(BP->Status == BS_UpToDate || BP->Status == BS_UpToDateWithWarnings)
				? TEXT("UpToDate") : TEXT("Error"));
	}
	else
	{
		ResponseData->SetBoolField(TEXT("compiled"), false);
	}

	// Persist to disk (skip transient packages used by tests)
	if (!BP->GetPackage()->GetName().StartsWith(TEXT("/Engine/Transient")))
	{
		FString PackageFilename;
		if (FPackageName::TryConvertLongPackageNameToFilename(
				BP->GetPackage()->GetName(), PackageFilename, TEXT(".uasset")))
		{
			FSavePackageArgs SaveArgs;
			SaveArgs.TopLevelFlags = RF_Public | RF_Standalone;
			const bool bSaved = UPackage::SavePackage(BP->GetPackage(), BP, *PackageFilename, SaveArgs);
			if (!bSaved)
			{
				return FCortexCommandRouter::Error(
					CortexErrorCodes::SaveFailed,
					FString::Printf(TEXT("Failed to save Blueprint after adding SCS component: %s"),
						*BP->GetPackage()->GetName()));
			}
		}
		else
		{
			return FCortexCommandRouter::Error(
				CortexErrorCodes::SaveFailed,
				FString::Printf(TEXT("Failed to resolve package filename for: %s"),
					*BP->GetPackage()->GetName()));
		}
	}

	UE_LOG(LogCortexBlueprint, Log, TEXT("Added SCS component '%s' (%s) to %s"),
		*NewNode->GetVariableName().ToString(), *ComponentClass->GetName(), *BP->GetName());

	return FCortexCommandRouter::Success(ResponseData);
}
