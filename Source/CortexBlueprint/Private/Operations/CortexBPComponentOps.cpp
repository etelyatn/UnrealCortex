#include "Operations/CortexBPComponentOps.h"
#include "Operations/CortexBPAssetOps.h"
#include "Operations/CortexBPSCSDiagnostics.h"
#include "CortexAssetFingerprint.h"
#include "CortexBatchMutation.h"
#include "CortexBlueprintModule.h"
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
#include "UObject/UnrealType.h"

namespace CortexBPComponentOpsPrivate
{
	UObject* FindComponentTemplate(UBlueprint* Blueprint, const FString& ComponentName);

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

		if (!CortexBPComponentOpsPrivate::FindComponentTemplate(Blueprint, ComponentName))
		{
			return FCortexBatchPreflightResult::Error(
				CortexErrorCodes::ComponentNotFound,
				FString::Printf(TEXT("Component not found: %s"), *ComponentName));
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

	UObject* FindComponentTemplate(UBlueprint* Blueprint, const FString& ComponentName)
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

		for (UActorComponent* Template : Blueprint->ComponentTemplates)
		{
			if (Template != nullptr && Template->GetName() == ComponentName)
			{
				return Template;
			}
		}

		if (UBlueprintGeneratedClass* BPGC = Cast<UBlueprintGeneratedClass>(Blueprint->GeneratedClass))
		{
			for (UActorComponent* Template : BPGC->ComponentTemplates)
			{
				if (Template != nullptr && Template->GetName() == ComponentName)
				{
					return Template;
				}
			}

			if (AActor* CDO = Cast<AActor>(BPGC->GetDefaultObject()))
			{
				TInlineComponentArray<UActorComponent*> Components;
				CDO->GetComponents(Components);
				for (UActorComponent* Component : Components)
				{
					if (Component != nullptr && Component->GetName() == ComponentName)
					{
						return Component;
					}
				}
			}
		}

		return nullptr;
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

	FKismetEditorUtilities::CompileBlueprint(Blueprint);

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

	FString LoadError;
	UBlueprint* Blueprint = FCortexBPAssetOps::LoadBlueprint(AssetPath, LoadError);
	if (Blueprint == nullptr)
	{
		return FCortexCommandRouter::Error(CortexErrorCodes::BlueprintNotFound, LoadError);
	}

	UObject* ComponentTemplate = CortexBPComponentOpsPrivate::FindComponentTemplate(Blueprint, ComponentName);
	if (ComponentTemplate == nullptr)
	{
		return FCortexCommandRouter::Error(
			CortexErrorCodes::ComponentNotFound,
			FString::Printf(TEXT("Component not found: %s"), *ComponentName)
		);
	}

	FScopedTransaction Transaction(FText::FromString(
		FString::Printf(TEXT("Cortex: Set Component Defaults %s"), *ComponentName)));
	ComponentTemplate->Modify();
	Blueprint->Modify();

	int32 PropertiesSet = 0;
	TArray<TSharedPtr<FJsonValue>> ErrorsArray;

	for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : (*PropertiesObj)->Values)
	{
		const FString& PropName = Pair.Key;
		const TSharedPtr<FJsonValue>& PropValueJson = Pair.Value;
		const FString PropValue = PropValueJson.IsValid() ? PropValueJson->AsString() : FString();
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
			ErrorsArray.Add(MakeShared<FJsonValueString>(
				FString::Printf(TEXT("Property not found: %s"), *PropertyPath.BasePropertyName)));
			continue;
		}

		const FString PackageName = FPackageName::ObjectPathToPackageName(PropValue);
		if (PackageName.IsEmpty() || (!FindPackage(nullptr, *PackageName) && !FPackageName::DoesPackageExist(PackageName)))
		{
			ErrorsArray.Add(MakeShared<FJsonValueString>(
				FString::Printf(TEXT("Asset not found: %s"), *PropValue)));
			continue;
		}

		UClass* AssetClass = nullptr;
		if (PropertyPath.bHasArrayIndex)
		{
			FArrayProperty* ArrayProp = CastField<FArrayProperty>(Prop);
			if (ArrayProp == nullptr)
			{
				ErrorsArray.Add(MakeShared<FJsonValueString>(
					FString::Printf(TEXT("Property '%s' is not an array"), *PropertyPath.BasePropertyName)));
				continue;
			}

			FObjectProperty* InnerObjectProp = CastField<FObjectProperty>(ArrayProp->Inner);
			if (InnerObjectProp == nullptr)
			{
				ErrorsArray.Add(MakeShared<FJsonValueString>(
					FString::Printf(TEXT("Array property '%s' does not store object references"), *PropertyPath.BasePropertyName)));
				continue;
			}

			AssetClass = InnerObjectProp->PropertyClass;
		}
		else
		{
			FObjectProperty* ObjectProp = CastField<FObjectProperty>(Prop);
			if (ObjectProp == nullptr)
			{
				ErrorsArray.Add(MakeShared<FJsonValueString>(
					FString::Printf(TEXT("Property '%s' is not an object reference"), *PropertyPath.BasePropertyName)));
				continue;
			}

			AssetClass = ObjectProp->PropertyClass;
		}

		UObject* Asset = StaticLoadObject(AssetClass, nullptr, *PropValue);
		if (Asset == nullptr)
		{
			ErrorsArray.Add(MakeShared<FJsonValueString>(
				FString::Printf(TEXT("Failed to load asset: %s"), *PropValue)));
			continue;
		}

		// Some component properties require setter side-effects to stay engine-consistent.
		if (!PropertyPath.bHasArrayIndex && PropertyPath.BasePropertyName == TEXT("StaticMesh"))
		{
			if (UStaticMeshComponent* StaticMeshComponent = Cast<UStaticMeshComponent>(ComponentTemplate))
			{
				UStaticMesh* StaticMesh = Cast<UStaticMesh>(Asset);
				if (StaticMesh == nullptr)
				{
					ErrorsArray.Add(MakeShared<FJsonValueString>(
						FString::Printf(TEXT("Asset is not a StaticMesh: %s"), *PropValue)));
					continue;
				}

				StaticMeshComponent->SetStaticMesh(StaticMesh);
				++PropertiesSet;
				continue;
			}
		}

		if (PropertyPath.bHasArrayIndex)
		{
			FArrayProperty* ArrayProp = CastFieldChecked<FArrayProperty>(Prop);
			FObjectProperty* InnerObjectProp = CastFieldChecked<FObjectProperty>(ArrayProp->Inner);
			FScriptArrayHelper ArrayHelper(ArrayProp, ArrayProp->ContainerPtrToValuePtr<void>(ComponentTemplate));
			while (ArrayHelper.Num() <= PropertyPath.ArrayIndex)
			{
				ArrayHelper.AddValue();
			}
			InnerObjectProp->SetObjectPropertyValue(ArrayHelper.GetRawPtr(PropertyPath.ArrayIndex), Asset);
		}
		else
		{
			FObjectProperty* ObjectProp = CastFieldChecked<FObjectProperty>(Prop);
			ObjectProp->SetObjectPropertyValue_InContainer(ComponentTemplate, Asset);
		}
		++PropertiesSet;
	}

	FBlueprintEditorUtils::MarkBlueprintAsModified(Blueprint);
	FKismetEditorUtilities::CompileBlueprint(Blueprint);
	if (!CortexBPComponentOpsPrivate::IsBlueprintCompiled(Blueprint))
	{
		return FCortexCommandRouter::Error(
			CortexErrorCodes::CompileFailed,
			FString::Printf(TEXT("Blueprint compilation failed after setting component defaults: %s"), *AssetPath),
			CortexBPComponentOpsPrivate::BuildBlueprintCompileDetails(Blueprint));
	}

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("component_name"), ComponentName);
	Data->SetNumberField(TEXT("properties_set"), PropertiesSet);
	if (ErrorsArray.Num() > 0)
	{
		Data->SetArrayField(TEXT("errors"), ErrorsArray);
	}

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
