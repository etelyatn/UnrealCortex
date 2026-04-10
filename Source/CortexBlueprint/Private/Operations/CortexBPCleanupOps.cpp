#include "Operations/CortexBPCleanupOps.h"
#include "Operations/CortexBPAssetOps.h"
#include "Operations/CortexBPSCSDiagnostics.h"
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
#include "Components/TimelineComponent.h"
#include "GameFramework/Actor.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"
#include "UObject/UnrealType.h"
#include "Misc/PackageName.h"
#include "ScopedTransaction.h"

#if WITH_DEV_AUTOMATION_TESTS
namespace
{
	TFunction<void(USCS_Node*, UBlueprint*)> GRemoveSCSComponentMidflightTestHook;
}
#endif

namespace
{
	USCS_Node* FindOwnedSCSNodeByName(USimpleConstructionScript* SCS, const FName Name)
	{
		if (!SCS)
		{
			return nullptr;
		}

		for (USCS_Node* Node : SCS->GetAllNodes())
		{
			if (Node && Node->GetVariableName() == Name)
			{
				return Node;
			}
		}

		return nullptr;
	}

	USCS_Node* FindInheritedSCSNodeByName(UBlueprint* Blueprint, const FName ComponentFName, UClass*& OutOwnerClass)
	{
		OutOwnerClass = nullptr;
		if (!Blueprint)
		{
			return nullptr;
		}

		for (UClass* ClassCursor = Blueprint->ParentClass.Get(); ClassCursor; ClassCursor = ClassCursor->GetSuperClass())
		{
			UBlueprintGeneratedClass* ParentBPGC = Cast<UBlueprintGeneratedClass>(ClassCursor);
			if (!ParentBPGC || !ParentBPGC->SimpleConstructionScript)
			{
				continue;
			}

			if (USCS_Node* ParentNode = ParentBPGC->SimpleConstructionScript->FindSCSNode(ComponentFName))
			{
				OutOwnerClass = ClassCursor;
				return ParentNode;
			}
		}

		return nullptr;
	}

	bool SaveBlueprintIfPersistent(UBlueprint* Blueprint, FString& OutError)
	{
		OutError.Reset();
		if (!Blueprint)
		{
			OutError = TEXT("Blueprint is null");
			return false;
		}

		if (Blueprint->GetPackage()->GetName().StartsWith(TEXT("/Engine/Transient")))
		{
			return true;
		}

		FString PackageFilename;
		if (!FPackageName::TryConvertLongPackageNameToFilename(
				Blueprint->GetPackage()->GetName(),
				PackageFilename,
				TEXT(".uasset")))
		{
			OutError = FString::Printf(
				TEXT("Failed to resolve package filename for: %s"),
				*Blueprint->GetPackage()->GetName());
			return false;
		}

		FSavePackageArgs SaveArgs;
		SaveArgs.TopLevelFlags = RF_Public | RF_Standalone;
		if (!UPackage::SavePackage(Blueprint->GetPackage(), Blueprint, *PackageFilename, SaveArgs))
		{
			OutError = FString::Printf(
				TEXT("Failed to save Blueprint package: %s"),
				*Blueprint->GetPackage()->GetName());
			return false;
		}

		return true;
	}

	bool IsBlueprintUpToDate(const UBlueprint* Blueprint)
	{
		return Blueprint
			&& (Blueprint->Status == BS_UpToDate || Blueprint->Status == BS_UpToDateWithWarnings);
	}

	TArray<UTimelineComponent*> CollectOwnedTimelineTemplates(UBlueprint* BP)
	{
		TArray<UTimelineComponent*> TimelineTemplates;
		if (!BP || !BP->GeneratedClass || !BP->SimpleConstructionScript)
		{
			return TimelineTemplates;
		}

		UBlueprintGeneratedClass* BPGC = Cast<UBlueprintGeneratedClass>(BP->GeneratedClass);
		if (!BPGC)
		{
			return TimelineTemplates;
		}

		for (USCS_Node* Node : BP->SimpleConstructionScript->GetAllNodes())
		{
			if (!Node || !Node->ComponentClass || !Node->ComponentClass->IsChildOf(UTimelineComponent::StaticClass()))
			{
				continue;
			}

			if (UTimelineComponent* TimelineTemplate = Cast<UTimelineComponent>(Node->GetActualComponentTemplate(BPGC)))
			{
				TimelineTemplates.Add(TimelineTemplate);
			}
		}

		return TimelineTemplates;
	}

	bool ReferencesOwnedTimelineTemplate(UBlueprint* BP, USCS_Node* Node)
	{
		if (!BP || !Node || !BP->GeneratedClass)
		{
			return false;
		}

		UBlueprintGeneratedClass* BPGC = Cast<UBlueprintGeneratedClass>(BP->GeneratedClass);
		UActorComponent* Template = BPGC ? Node->GetActualComponentTemplate(BPGC) : nullptr;
		if (!Template)
		{
			return false;
		}

		const TArray<UTimelineComponent*> TimelineTemplates = CollectOwnedTimelineTemplates(BP);
		if (TimelineTemplates.IsEmpty())
		{
			return false;
		}

		for (TFieldIterator<FObjectPropertyBase> It(Template->GetClass()); It; ++It)
		{
			const FObjectPropertyBase* ObjectProperty = *It;
			if (!ObjectProperty)
			{
				continue;
			}

			UObject* ReferencedObject = ObjectProperty->GetObjectPropertyValue_InContainer(Template);
			if (!ReferencedObject)
			{
				continue;
			}

			for (UTimelineComponent* TimelineTemplate : TimelineTemplates)
			{
				if (ReferencedObject == TimelineTemplate)
				{
					return true;
				}
			}
		}

		return false;
	}
}

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

FCortexCommandResult FCortexBPCleanupOps::RecompileDependents(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath;
	if (!Params.IsValid() || !Params->TryGetStringField(TEXT("asset_path"), AssetPath) || AssetPath.IsEmpty())
	{
		return FCortexCommandRouter::Error(
			CortexErrorCodes::InvalidField,
			TEXT("Missing required param: asset_path"));
	}

	FString LoadError;
	UBlueprint* TargetBlueprint = FCortexBPAssetOps::LoadBlueprint(AssetPath, LoadError);
	if (!TargetBlueprint)
	{
		return FCortexCommandRouter::Error(CortexErrorCodes::BlueprintNotFound, LoadError);
	}

	FScopedTransaction Transaction(FText::FromString(
		FString::Printf(TEXT("Cortex: Recompile Dependents of %s"), *TargetBlueprint->GetName())
	));

	TArray<UBlueprint*> DependentBlueprints;
	FBlueprintEditorUtils::GetDependentBlueprints(TargetBlueprint, DependentBlueprints);

	TArray<TSharedPtr<FJsonValue>> ResultsArray;
	ResultsArray.Reserve(DependentBlueprints.Num());
	int32 SuccessCount = 0;

	for (UBlueprint* DependentBlueprint : DependentBlueprints)
	{
		if (!DependentBlueprint)
		{
			continue;
		}

		FBlueprintEditorUtils::RefreshAllNodes(DependentBlueprint);
		FKismetEditorUtilities::CompileBlueprint(DependentBlueprint);

		const bool bCompiled =
			DependentBlueprint->Status == BS_UpToDate ||
			DependentBlueprint->Status == BS_UpToDateWithWarnings;
		SuccessCount += bCompiled ? 1 : 0;

		TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
		Entry->SetStringField(TEXT("blueprint"), DependentBlueprint->GetPathName());
		Entry->SetStringField(TEXT("status"), bCompiled ? TEXT("success") : TEXT("error"));

		TArray<TSharedPtr<FJsonValue>> Errors;
		if (!bCompiled)
		{
			Errors.Add(MakeShared<FJsonValueString>(TEXT("Blueprint did not compile to UpToDate status")));
		}
		Entry->SetArrayField(TEXT("errors"), Errors);
		ResultsArray.Add(MakeShared<FJsonValueObject>(Entry));
	}

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("asset_path"), AssetPath);
	Data->SetNumberField(TEXT("dependent_count"), DependentBlueprints.Num());
	Data->SetNumberField(TEXT("recompiled_count"), SuccessCount);
	Data->SetArrayField(TEXT("results"), ResultsArray);

	return FCortexCommandRouter::Success(Data);
}

FCortexCommandResult FCortexBPCleanupOps::RenameSCSComponent(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath;
	FString OldName;
	FString NewName;
	if (!Params.IsValid()
		|| !Params->TryGetStringField(TEXT("asset_path"), AssetPath) || AssetPath.IsEmpty()
		|| !Params->TryGetStringField(TEXT("old_name"), OldName) || OldName.IsEmpty()
		|| !Params->TryGetStringField(TEXT("new_name"), NewName) || NewName.IsEmpty())
	{
		return FCortexCommandRouter::Error(
			CortexErrorCodes::InvalidField,
			TEXT("Missing required params: asset_path, old_name, new_name"));
	}

	bool bCompile = true;
	Params->TryGetBoolField(TEXT("compile"), bCompile);

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

	const FName OldFName(*OldName);
	const FName NewFName(*NewName);

	USCS_Node* TargetNode = FindOwnedSCSNodeByName(SCS, OldFName);
	if (!TargetNode)
	{
		UClass* InheritedOwnerClass = nullptr;
		if (FindInheritedSCSNodeByName(BP, OldFName, InheritedOwnerClass))
		{
			const FString OwnerName = InheritedOwnerClass ? InheritedOwnerClass->GetName() : TEXT("parent class");
			return FCortexCommandRouter::Error(
				CortexErrorCodes::InvalidField,
				FString::Printf(
					TEXT("SCS component '%s' is inherited from %s; rename on the parent Blueprint"),
					*OldName,
					*OwnerName));
		}

		return FCortexCommandRouter::Error(
			CortexErrorCodes::ComponentNotFound,
			FString::Printf(TEXT("SCS component not found: %s"), *OldName));
	}

	if (TargetNode->ComponentClass && TargetNode->ComponentClass->IsChildOf(UTimelineComponent::StaticClass()))
	{
		return FCortexCommandRouter::Error(
			CortexErrorCodes::InvalidField,
			FString::Printf(TEXT("Component '%s' is a UTimelineComponent. Use rename_timeline."), *OldName));
	}

	if (ReferencesOwnedTimelineTemplate(BP, TargetNode))
	{
		return FCortexCommandRouter::Error(
			CortexErrorCodes::InvalidField,
			FString::Printf(
				TEXT("Component '%s' references a local timeline template. Rename the timeline or remove the reference first."),
				*OldName));
	}

	if (USCS_Node* ExistingNode = FindOwnedSCSNodeByName(SCS, NewFName))
	{
		if (ExistingNode != TargetNode)
		{
			return FCortexCommandRouter::Error(
				CortexErrorCodes::InvalidField,
				FString::Printf(TEXT("new_name '%s' collides with an existing SCS node"), *NewName));
		}
	}

	const bool bVariableCollision = BP->NewVariables.ContainsByPredicate(
		[&NewFName](const FBPVariableDescription& Variable)
		{
			return Variable.VarName == NewFName;
		});
	if (bVariableCollision)
	{
		return FCortexCommandRouter::Error(
			CortexErrorCodes::InvalidField,
			FString::Printf(TEXT("new_name '%s' collides with an existing Blueprint variable"), *NewName));
	}

	if (BP->ParentClass && FindFProperty<FProperty>(BP->ParentClass, NewFName))
	{
		return FCortexCommandRouter::Error(
			CortexErrorCodes::InvalidField,
			FString::Printf(TEXT("new_name '%s' collides with an inherited UPROPERTY"), *NewName));
	}

	UClass* InheritedCollisionOwner = nullptr;
	if (FindInheritedSCSNodeByName(BP, NewFName, InheritedCollisionOwner))
	{
		const FString OwnerName = InheritedCollisionOwner ? InheritedCollisionOwner->GetName() : TEXT("parent class");
		return FCortexCommandRouter::Error(
			CortexErrorCodes::InvalidField,
			FString::Printf(
				TEXT("new_name '%s' collides with inherited SCS node on %s"),
				*NewName,
				*OwnerName));
	}

	TArray<UBlueprint*> DependentBlueprints;
	FBlueprintEditorUtils::GetDependentBlueprints(BP, DependentBlueprints);
	for (UBlueprint* DependentBP : DependentBlueprints)
	{
		if (!DependentBP || DependentBP == BP)
		{
			continue;
		}

		const bool bDependentVariableCollision = DependentBP->NewVariables.ContainsByPredicate(
			[&NewFName](const FBPVariableDescription& Variable)
			{
				return Variable.VarName == NewFName;
			});
		if (bDependentVariableCollision)
		{
			return FCortexCommandRouter::Error(
				CortexErrorCodes::InvalidField,
				FString::Printf(
					TEXT("new_name '%s' would shadow a variable on dependent Blueprint %s"),
					*NewName,
					*DependentBP->GetPathName()));
		}

		if (DependentBP->SimpleConstructionScript
			&& FindOwnedSCSNodeByName(DependentBP->SimpleConstructionScript, NewFName))
		{
			return FCortexCommandRouter::Error(
				CortexErrorCodes::InvalidField,
				FString::Printf(
					TEXT("new_name '%s' would shadow an SCS node on dependent Blueprint %s"),
					*NewName,
					*DependentBP->GetPathName()));
		}
	}

	BP->Modify();
	SCS->Modify();
	TargetNode->Modify();
	FBlueprintEditorUtils::RenameMemberVariable(BP, OldFName, NewFName);

	USCS_Node* RenamedNode = FindOwnedSCSNodeByName(SCS, NewFName);
	if (!RenamedNode)
	{
		// Some engine paths do not remap SCS member variables through RenameMemberVariable.
		FBlueprintEditorUtils::RenameComponentMemberVariable(BP, TargetNode, NewFName);
		RenamedNode = FindOwnedSCSNodeByName(SCS, NewFName);
	}

	if (!RenamedNode)
	{
		return FCortexCommandRouter::Error(
			CortexErrorCodes::InvalidOperation,
			FString::Printf(TEXT("Failed to rename SCS component '%s' to '%s'"), *OldName, *NewName));
	}

	FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(BP);

	FString CompileStatus = TEXT("Skipped");
	bool bCompiled = false;
	TArray<TSharedPtr<FJsonValue>> DependentBlueprintEntries;
	if (bCompile)
	{
		FKismetEditorUtilities::CompileBlueprint(BP);
		bCompiled = IsBlueprintUpToDate(BP);
		CompileStatus = bCompiled ? TEXT("UpToDate") : TEXT("Error");
		if (!bCompiled)
		{
			return FCortexCommandRouter::Error(
				CortexErrorCodes::CompileFailed,
				FString::Printf(TEXT("Blueprint compile failed after SCS rename: %s"), *BP->GetPathName()));
		}

		for (UBlueprint* DependentBP : DependentBlueprints)
		{
			if (!DependentBP || DependentBP == BP)
			{
				continue;
			}

			DependentBP->Modify();
			FKismetEditorUtilities::CompileBlueprint(DependentBP);
			const bool bDependentCompiled = IsBlueprintUpToDate(DependentBP);
			TSharedPtr<FJsonObject> DependentEntry = MakeShared<FJsonObject>();
			DependentEntry->SetStringField(TEXT("path"), DependentBP->GetPathName());
			DependentEntry->SetStringField(TEXT("compile_status"), bDependentCompiled ? TEXT("UpToDate") : TEXT("Error"));
			DependentEntry->SetBoolField(TEXT("saved"), false);
			DependentBlueprintEntries.Add(MakeShared<FJsonValueObject>(DependentEntry));
			if (!bDependentCompiled)
			{
				return FCortexCommandRouter::Error(
					CortexErrorCodes::CompileFailed,
					FString::Printf(
						TEXT("Dependent Blueprint compile failed after SCS rename: %s"),
						*DependentBP->GetPathName()));
			}
		}
	}

	FString SaveError;
	if (!SaveBlueprintIfPersistent(BP, SaveError))
	{
		return FCortexCommandRouter::Error(CortexErrorCodes::SaveFailed, SaveError);
	}

	for (UBlueprint* DependentBP : DependentBlueprints)
	{
		if (!DependentBP || DependentBP == BP)
		{
			continue;
		}

		if (!SaveBlueprintIfPersistent(DependentBP, SaveError))
		{
			return FCortexCommandRouter::Error(CortexErrorCodes::SaveFailed, SaveError);
		}

		for (const TSharedPtr<FJsonValue>& DependentValue : DependentBlueprintEntries)
		{
			const TSharedPtr<FJsonObject>* DependentObject = nullptr;
			if (!DependentValue.IsValid() || !DependentValue->TryGetObject(DependentObject) || !DependentObject || !DependentObject->IsValid())
			{
				continue;
			}

			FString Path;
			if ((*DependentObject)->TryGetStringField(TEXT("path"), Path) && Path == DependentBP->GetPathName())
			{
				(*DependentObject)->SetBoolField(TEXT("saved"), true);
				break;
			}
		}
	}

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("asset_path"), AssetPath);
	Data->SetStringField(TEXT("old_name"), OldName);
	Data->SetStringField(TEXT("new_name"), NewName);
	Data->SetBoolField(TEXT("compiled"), bCompiled);
	Data->SetStringField(TEXT("compile_status"), CompileStatus);
	if (!bCompile)
	{
		for (UBlueprint* DependentBP : DependentBlueprints)
		{
			if (!DependentBP || DependentBP == BP)
			{
				continue;
			}

			TSharedPtr<FJsonObject> DependentEntry = MakeShared<FJsonObject>();
			DependentEntry->SetStringField(TEXT("path"), DependentBP->GetPathName());
			DependentEntry->SetStringField(TEXT("compile_status"), TEXT("Skipped"));
			DependentEntry->SetBoolField(TEXT("saved"), true);
			DependentBlueprintEntries.Add(MakeShared<FJsonValueObject>(DependentEntry));
		}
	}
	Data->SetArrayField(TEXT("dependent_blueprints"), DependentBlueprintEntries);

	return FCortexCommandRouter::Success(Data);
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

	const FName ComponentFName(*ComponentName);
	USCS_Node* TargetNode = SCS->FindSCSNode(ComponentFName);

	if (!TargetNode)
	{
		UClass* InheritedOwnerClass = nullptr;
		if (FindInheritedSCSNodeByName(BP, ComponentFName, InheritedOwnerClass))
		{
			const FString OwnerName = InheritedOwnerClass ? InheritedOwnerClass->GetName() : TEXT("parent class");
			return FCortexCommandRouter::Error(
				CortexErrorCodes::InvalidField,
				FString::Printf(TEXT("SCS component '%s' is inherited from %s; rename/remove it on the parent Blueprint"),
					*ComponentName,
					*OwnerName));
		}

		return FCortexCommandRouter::Error(
			CortexErrorCodes::ComponentNotFound,
			FString::Printf(TEXT("SCS component not found: %s"), *ComponentName));
	}

	if (TargetNode == SCS->GetDefaultSceneRootNode())
	{
		return FCortexCommandRouter::Error(
			CortexErrorCodes::InvalidField,
			TEXT("DefaultSceneRoot cannot be removed"));
	}

	const bool bCompile = Params->HasField(TEXT("compile")) ? Params->GetBoolField(TEXT("compile")) : true;
	const bool bForce = Params->HasField(TEXT("force")) ? Params->GetBoolField(TEXT("force")) : false;

	TArray<FString> AcknowledgedLosses;
	const TArray<TSharedPtr<FJsonValue>>* AcknowledgedLossesArray = nullptr;
	if (Params->TryGetArrayField(TEXT("acknowledged_losses"), AcknowledgedLossesArray) && AcknowledgedLossesArray)
	{
		for (const TSharedPtr<FJsonValue>& Value : *AcknowledgedLossesArray)
		{
			if (!Value.IsValid() || Value->Type != EJson::String)
			{
				return FCortexCommandRouter::Error(
					CortexErrorCodes::InvalidField,
					TEXT("'acknowledged_losses' must be an array of strings"));
			}
			AcknowledgedLosses.Add(Value->AsString());
		}
		AcknowledgedLosses.Sort();
	}

	const FCortexBPSCSDiagnostics::FDirtyReport PreReport =
		FCortexBPSCSDiagnostics::DetectSCSNodeDirtyState(TargetNode, BP);
	const bool bHasSubObjectLoss = PreReport.HasSubObjectLoss();
	const bool bAcknowledgmentMatches = (AcknowledgedLosses == PreReport.AcknowledgmentKeys);
	if (bHasSubObjectLoss && !bForce && !bAcknowledgmentMatches)
	{
		return FCortexCommandRouter::Error(
			CortexErrorCodes::PotentialDataLoss,
			FString::Printf(
				TEXT("Component '%s' has dirty sub-objects that would be lost. Echo required_acknowledgment as acknowledged_losses to proceed."),
				*ComponentName),
			PreReport.ToRefusalJson());
	}

	FScopedTransaction Transaction(FText::FromString(
		FString::Printf(TEXT("Cortex: Remove SCS Component %s from %s"), *ComponentName, *BP->GetName())));

	BP->Modify();
	SCS->Modify();
	TargetNode->Modify();

#if WITH_DEV_AUTOMATION_TESTS
	if (GRemoveSCSComponentMidflightTestHook)
	{
		GRemoveSCSComponentMidflightTestHook(TargetNode, BP);
	}
#endif

	const FCortexBPSCSDiagnostics::FDirtyReport InsideReport =
		FCortexBPSCSDiagnostics::DetectSCSNodeDirtyState(TargetNode, BP);
	if (InsideReport.AcknowledgmentKeys != PreReport.AcknowledgmentKeys && !bForce)
	{
		Transaction.Cancel();
		return FCortexCommandRouter::Error(
			CortexErrorCodes::PotentialDataLoss,
			TEXT("Dirty state changed between pre-flight and transaction; re-run and re-acknowledge."),
			InsideReport.ToRefusalJson());
	}

	if (InsideReport.HasSubObjectLoss() && !bForce && AcknowledgedLosses != InsideReport.AcknowledgmentKeys)
	{
		Transaction.Cancel();
		return FCortexCommandRouter::Error(
			CortexErrorCodes::PotentialDataLoss,
			TEXT("Acknowledged losses do not match current dirty state; re-run and retry with required_acknowledgment."),
			InsideReport.ToRefusalJson());
	}

	// RemoveNodeAndPromoteChildren re-parents children to the removed node's parent
	// and removes the node from all SCS arrays — it is the canonical API for this operation.
	SCS->RemoveNodeAndPromoteChildren(TargetNode);

	FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(BP);

	TSharedPtr<FJsonObject> ResponseData = MakeShared<FJsonObject>();
	ResponseData->SetStringField(TEXT("removed_component"), ComponentName);
	ResponseData->SetObjectField(TEXT("diff"), InsideReport.ToDiffJson());

	FString GuardResult = TEXT("clean");
	if (InsideReport.HasSubObjectLoss())
	{
		GuardResult = bForce ? TEXT("force_override") : TEXT("acknowledged");
		ResponseData->SetStringField(TEXT("override_used"), bForce ? TEXT("force") : TEXT("acknowledged_losses"));
	}
	else if (!InsideReport.TopLevelKeys.IsEmpty())
	{
		GuardResult = TEXT("top_level_dirty");
	}
	ResponseData->SetStringField(TEXT("guard_result"), GuardResult);

	TArray<TSharedPtr<FJsonValue>> ResponseWarnings;
	if (bForce && InsideReport.HasSubObjectLoss())
	{
		ResponseWarnings.Add(MakeShared<FJsonValueString>(
			TEXT("Data loss override used via force - verify downstream assets.")));
		ResponseData->SetArrayField(TEXT("warnings"), ResponseWarnings);
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
					FString::Printf(TEXT("Failed to save Blueprint after SCS removal: %s"), *BP->GetPackage()->GetName()));
			}
		}
		else
		{
			return FCortexCommandRouter::Error(
				CortexErrorCodes::SaveFailed,
				FString::Printf(TEXT("Failed to resolve package filename for: %s"), *BP->GetPackage()->GetName()));
		}
	}

	UE_LOG(LogCortexBlueprint, Log, TEXT("Removed SCS component '%s' from %s"), *ComponentName, *BP->GetName());

	FCortexCommandResult Result = FCortexCommandRouter::Success(ResponseData);
	if (bForce && InsideReport.HasSubObjectLoss())
	{
		Result.Warnings.Add(TEXT("Data loss override used via force - verify downstream assets."));
	}

	return Result;
}

#if WITH_DEV_AUTOMATION_TESTS
void FCortexBPCleanupOps::SetRemoveSCSComponentMidflightTestHook(
	TFunction<void(USCS_Node*, UBlueprint*)> InHook)
{
	GRemoveSCSComponentMidflightTestHook = MoveTemp(InHook);
}
#endif
