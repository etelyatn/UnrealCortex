#include "Operations/CortexBPCleanupOps.h"
#include "Operations/CortexBPAssetOps.h"
#include "CortexBlueprintModule.h"
#include "CortexCommandRouter.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Engine/Blueprint.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "Engine/SimpleConstructionScript.h"
#include "Engine/SCS_Node.h"
#include "EdGraph/EdGraph.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Components/ActorComponent.h"
#include "Components/SceneComponent.h"
#include "GameFramework/Actor.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"
#include "Misc/PackageName.h"
#include "ScopedTransaction.h"

FCortexCommandResult FCortexBPCleanupOps::CleanupMigration(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath;
	if (!Params.IsValid() || !Params->TryGetStringField(TEXT("asset_path"), AssetPath) || AssetPath.IsEmpty())
	{
		return FCortexCommandRouter::Error(
			CortexErrorCodes::InvalidField,
			TEXT("Missing required param: asset_path"));
	}

	FString LoadError;
	UBlueprint* BP = FCortexBPAssetOps::LoadBlueprint(AssetPath, LoadError);
	if (!BP)
	{
		return FCortexCommandRouter::Error(CortexErrorCodes::BlueprintNotFound, LoadError);
	}

	const bool bCompile = Params->HasField(TEXT("compile")) ? Params->GetBoolField(TEXT("compile")) : true;

	TSharedPtr<FJsonObject> ResponseData = MakeShared<FJsonObject>();
	TArray<TSharedPtr<FJsonValue>> Warnings;
	TArray<TSharedPtr<FJsonValue>> RemovedVars;
	TArray<TSharedPtr<FJsonValue>> RemovedFuncs;
	bool bReparented = false;

	// --- Validate reparent target before opening transaction ---
	FString NewParentClassPath;
	UClass* NewParentClass = nullptr;
	if (Params->TryGetStringField(TEXT("new_parent_class"), NewParentClassPath) && !NewParentClassPath.IsEmpty())
	{
		NewParentClass = FindObject<UClass>(nullptr, *NewParentClassPath);
		if (!NewParentClass)
		{
			NewParentClass = LoadClass<UObject>(nullptr, *NewParentClassPath);
		}
		if (!NewParentClass)
		{
			return FCortexCommandRouter::Error(
				CortexErrorCodes::InvalidField,
				FString::Printf(TEXT("Parent class not found: %s"), *NewParentClassPath));
		}

		// Type compatibility check: allow same-family reparent only.
		static UClass* UserWidgetClass = FindObject<UClass>(nullptr, TEXT("/Script/UMG.UserWidget"));
		const UClass* CurrentParent = BP->ParentClass.Get();
		const bool bCurrentIsActor = CurrentParent && CurrentParent->IsChildOf(AActor::StaticClass());
		const bool bCurrentIsComponent = CurrentParent && CurrentParent->IsChildOf(UActorComponent::StaticClass());
		const bool bCurrentIsWidget = UserWidgetClass && CurrentParent && CurrentParent->IsChildOf(UserWidgetClass);
		const bool bNewIsActor = NewParentClass->IsChildOf(AActor::StaticClass());
		const bool bNewIsComponent = NewParentClass->IsChildOf(UActorComponent::StaticClass());
		const bool bNewIsWidget = UserWidgetClass && NewParentClass->IsChildOf(UserWidgetClass);

		if (!((bCurrentIsActor && bNewIsActor) ||
			  (bCurrentIsComponent && bNewIsComponent) ||
			  (bCurrentIsWidget && bNewIsWidget)))
		{
			return FCortexCommandRouter::Error(
				CortexErrorCodes::InvalidField,
				TEXT("Type mismatch: reparenting is only supported within the same type family (Actor->Actor, Component->Component, Widget->Widget)"));
		}
	}

	// Wrap all mutations in a single transaction (constructed after all validation)
	FScopedTransaction Transaction(FText::FromString(
		FString::Printf(TEXT("Cortex: Cleanup Migration %s"), *BP->GetName())));

	// --- Reparent ---
	if (NewParentClass)
	{
		// SCS root component warning
		if (BP->SimpleConstructionScript && NewParentClass->IsChildOf(AActor::StaticClass()))
		{
			const USCS_Node* BPRoot = BP->SimpleConstructionScript->GetDefaultSceneRootNode();
			const AActor* NewParentCDO = Cast<AActor>(NewParentClass->GetDefaultObject(false));
			if (BPRoot && NewParentCDO && NewParentCDO->GetRootComponent())
			{
				Warnings.Add(MakeShared<FJsonValueString>(
					TEXT("SCS root component conflict detected - verify component hierarchy")));
			}
		}

		BP->Modify();
		if (BP->SimpleConstructionScript)
		{
			BP->SimpleConstructionScript->Modify();
		}
		// FBlueprintEditorUtils::ReparentBlueprint does not exist in UE 5.6.
		// Direct ParentClass assignment is the correct approach here, matching
		// what the Blueprint editor's own reparent dialog uses internally.
		BP->ParentClass = NewParentClass;
		FBlueprintEditorUtils::RefreshAllNodes(BP);
		FBlueprintEditorUtils::MarkBlueprintAsModified(BP);
		bReparented = true;
		ResponseData->SetStringField(TEXT("new_parent"), NewParentClass->GetName());
	}
	ResponseData->SetBoolField(TEXT("reparented"), bReparented);

	// --- Remove variables ---
	const TArray<TSharedPtr<FJsonValue>>* VarsArray;
	if (Params->TryGetArrayField(TEXT("remove_variables"), VarsArray))
	{
		for (const TSharedPtr<FJsonValue>& VarVal : *VarsArray)
		{
			const FString VarName = VarVal->AsString();
			if (VarName.IsEmpty()) { continue; }

			// Check variable exists
			const bool bExists = BP->NewVariables.ContainsByPredicate(
				[&VarName](const FBPVariableDescription& V) {
					return V.VarName.ToString() == VarName;
				});

			if (bExists)
			{
				FBlueprintEditorUtils::RemoveMemberVariable(BP, FName(*VarName));
				RemovedVars.Add(MakeShared<FJsonValueString>(VarName));
			}
			else
			{
				Warnings.Add(MakeShared<FJsonValueString>(
					FString::Printf(TEXT("Variable '%s' not found, skipped"), *VarName)));
			}
		}
	}
	ResponseData->SetArrayField(TEXT("removed_variables"), RemovedVars);

	// --- Remove functions ---
	const TArray<TSharedPtr<FJsonValue>>* FuncsArray;
	if (Params->TryGetArrayField(TEXT("remove_functions"), FuncsArray))
	{
		for (const TSharedPtr<FJsonValue>& FuncVal : *FuncsArray)
		{
			const FString FuncName = FuncVal->AsString();
			if (FuncName.IsEmpty()) { continue; }

			UEdGraph* GraphToRemove = nullptr;
			for (UEdGraph* Graph : BP->FunctionGraphs)
			{
				if (Graph && Graph->GetName() == FuncName)
				{
					GraphToRemove = Graph;
					break;
				}
			}

			if (GraphToRemove)
			{
				FBlueprintEditorUtils::RemoveGraph(BP, GraphToRemove);
				RemovedFuncs.Add(MakeShared<FJsonValueString>(FuncName));
			}
			else
			{
				Warnings.Add(MakeShared<FJsonValueString>(
					FString::Printf(TEXT("Function '%s' not found, skipped"), *FuncName)));
			}
		}
	}
	ResponseData->SetArrayField(TEXT("removed_functions"), RemovedFuncs);

	// --- Finalize ---
	// Note: BP->Modify() is omitted here — RemoveMemberVariable and RemoveGraph
	// call Modify() internally, and the reparent block calls it explicitly.

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
					FString::Printf(TEXT("Failed to save cleaned Blueprint package: %s"), *BP->GetPackage()->GetName()));
			}
		}
		else
		{
			return FCortexCommandRouter::Error(
				CortexErrorCodes::SaveFailed,
				FString::Printf(TEXT("Failed to resolve package filename for: %s"), *BP->GetPackage()->GetName()));
		}
	}

	ResponseData->SetArrayField(TEXT("warnings"), Warnings);

	UE_LOG(LogCortexBlueprint, Log, TEXT("Cleanup migration: %s — reparented=%d, removed %d vars, %d funcs"),
		*BP->GetName(), bReparented, RemovedVars.Num(), RemovedFuncs.Num());

	return FCortexCommandRouter::Success(ResponseData);
}

FCortexCommandResult FCortexBPCleanupOps::RemoveSCSComponent(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath;
	FString ComponentName;
	if (!Params.IsValid()
		|| !Params->TryGetStringField(TEXT("asset_path"), AssetPath) || AssetPath.IsEmpty()
		|| !Params->TryGetStringField(TEXT("component_name"), ComponentName) || ComponentName.IsEmpty())
	{
		return FCortexCommandRouter::Error(
			CortexErrorCodes::InvalidField,
			TEXT("Missing required params: asset_path, component_name"));
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

	// Find the SCS node by variable name
	USCS_Node* TargetNode = nullptr;
	for (USCS_Node* Node : SCS->GetAllNodes())
	{
		if (Node && Node->GetVariableName().ToString() == ComponentName)
		{
			TargetNode = Node;
			break;
		}
	}

	if (!TargetNode)
	{
		return FCortexCommandRouter::Error(
			CortexErrorCodes::ComponentNotFound,
			FString::Printf(TEXT("SCS component not found: %s"), *ComponentName));
	}

	const bool bCompile = Params->HasField(TEXT("compile")) ? Params->GetBoolField(TEXT("compile")) : true;

	FScopedTransaction Transaction(FText::FromString(
		FString::Printf(TEXT("Cortex: Remove SCS Component %s from %s"), *ComponentName, *BP->GetName())));

	BP->Modify();
	SCS->Modify();

	// Detach children from this node and re-attach to its parent before removal
	TArray<USCS_Node*> Children = TargetNode->GetChildNodes();
	USCS_Node* ParentNode = SCS->FindParentNode(TargetNode);
	for (USCS_Node* Child : Children)
	{
		if (Child)
		{
			TargetNode->RemoveChildNode(Child);
			if (ParentNode)
			{
				ParentNode->AddChildNode(Child);
			}
			else
			{
				SCS->AddNode(Child);
			}
		}
	}

	// Remove the node from the SCS
	SCS->RemoveNodeAndPromoteChildren(TargetNode);

	FBlueprintEditorUtils::MarkBlueprintAsModified(BP);

	if (bCompile)
	{
		FKismetEditorUtilities::CompileBlueprint(BP);
	}

	// Persist to disk
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
					FString::Printf(TEXT("Failed to save Blueprint after SCS removal: %s"), *BP->GetPackage()->GetName()));
			}
		}
	}

	UE_LOG(LogCortexBlueprint, Log, TEXT("Removed SCS component '%s' from %s"), *ComponentName, *BP->GetName());

	TSharedPtr<FJsonObject> ResponseData = MakeShared<FJsonObject>();
	ResponseData->SetStringField(TEXT("removed_component"), ComponentName);
	ResponseData->SetBoolField(TEXT("compiled"), bCompile);
	return FCortexCommandRouter::Success(ResponseData);
}
