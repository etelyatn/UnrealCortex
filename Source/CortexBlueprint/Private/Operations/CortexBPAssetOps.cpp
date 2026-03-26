#include "CortexBPAssetOps.h"
#include "Operations/CortexBPTypeUtils.h"
#include "CortexBlueprintModule.h"
#include "Engine/Blueprint.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetToolsModule.h"
#include "IAssetTools.h"
#include "CoreGlobals.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Blueprint/BlueprintSupport.h"
#include "UObject/SavePackage.h"
#include "Misc/PackageName.h"
#include "GameFramework/Actor.h"
#include "Components/ActorComponent.h"
#include "Kismet/BlueprintFunctionLibrary.h"
#include "Kismet2/CompilerResultsLog.h"
#include "K2Node_CallFunction.h"
#include "K2Node_Variable.h"
#include "K2Node_Event.h"
#include "K2Node_DynamicCast.h"
#include "K2Node_FunctionEntry.h"
#include "K2Node_FunctionResult.h"
#include "ScopedTransaction.h"
#include "UObject/UObjectGlobals.h"
#include "ObjectTools.h"
#include "Misc/TextBuffer.h"
#include "Editor.h"
#include "Engine/World.h"
#include "Engine/LevelScriptBlueprint.h"

UBlueprint* FCortexBPAssetOps::LoadBlueprint(const FString& AssetPath, FString& OutError)
{
	// Level Script Blueprint: synthetic path __level_bp__:/Game/Maps/MapName
	static const FString LevelBPPrefix = TEXT("__level_bp__:");
	if (AssetPath.StartsWith(LevelBPPrefix))
	{
		const FString MapPath = AssetPath.Mid(LevelBPPrefix.Len());

		// Use current editor world if it matches — avoids double-load
		UWorld* World = nullptr;
		if (GEditor)
		{
			UWorld* EditorWorld = GEditor->GetEditorWorldContext().World();
			if (EditorWorld && EditorWorld->GetOutermost()->GetName() == MapPath)
			{
				World = EditorWorld;
			}
		}

		if (!World)
		{
			UPackage* MapPackage = LoadPackage(nullptr, *MapPath, LOAD_None);
			if (!MapPackage)
			{
				OutError = FString::Printf(TEXT("Map package not found: %s"), *MapPath);
				return nullptr;
			}
			World = UWorld::FindWorldInPackage(MapPackage);
		}

		if (!World)
		{
			OutError = FString::Printf(TEXT("No world found in map package: %s"), *MapPath);
			return nullptr;
		}

		ULevelScriptBlueprint* LSB = World->PersistentLevel->GetLevelScriptBlueprint(/*bDontCreate=*/false);
		if (!LSB)
		{
			OutError = FString::Printf(TEXT("Failed to get Level Script Blueprint for: %s"), *MapPath);
			return nullptr;
		}

		return LSB;
	}

	// Check if the asset is already in memory at the original path (e.g. transient BPs in tests)
	// before applying /Game/ normalization, which would corrupt non-/Game/ paths.
	if (AssetPath.StartsWith(TEXT("/")))
	{
		const FString OrigPkgName = FPackageName::ObjectPathToPackageName(AssetPath);
		if (FindPackage(nullptr, *OrigPkgName))
		{
			UBlueprint* InMemoryBP = FindObject<UBlueprint>(nullptr, *AssetPath);
			if (InMemoryBP)
			{
				return InMemoryBP;
			}
		}
	}

	// Normalize path (ensure it starts with /Game/)
	FString NormalizedPath = AssetPath;
	if (!NormalizedPath.StartsWith(TEXT("/")))
	{
		NormalizedPath = TEXT("/Game/") + NormalizedPath;
	}
	else if (!NormalizedPath.StartsWith(TEXT("/Game/")))
	{
		NormalizedPath = TEXT("/Game") + NormalizedPath;
	}

	// Check if package exists before LoadObject to avoid SkipPackage warnings
	FString PkgName = FPackageName::ObjectPathToPackageName(NormalizedPath);
	if (!FindPackage(nullptr, *PkgName) && !FPackageName::DoesPackageExist(PkgName))
	{
		OutError = FString::Printf(TEXT("Blueprint not found at path: %s"), *NormalizedPath);
		return nullptr;
	}

	UObject* LoadedAsset = LoadObject<UBlueprint>(nullptr, *NormalizedPath);
	if (!LoadedAsset)
	{
		OutError = FString::Printf(TEXT("Blueprint not found at path: %s"), *NormalizedPath);
		return nullptr;
	}

	UBlueprint* BP = Cast<UBlueprint>(LoadedAsset);
	if (!BP)
	{
		OutError = FString::Printf(TEXT("Asset at path %s is not a Blueprint"), *NormalizedPath);
		return nullptr;
	}

	return BP;
}

FString FCortexBPAssetOps::DetermineBlueprintType(const UBlueprint* BP)
{
	if (!BP || !BP->ParentClass)
	{
		return TEXT("Unknown");
	}

	if (BP->BlueprintType == BPTYPE_Interface)
	{
		return TEXT("Interface");
	}

	if (BP->BlueprintType == BPTYPE_FunctionLibrary)
	{
		return TEXT("FunctionLibrary");
	}

	// Check for Widget using dynamic resolution to avoid compile-time dependency on UMG
	static UClass* UserWidgetClass = FindObject<UClass>(nullptr, TEXT("/Script/UMG.UserWidget"));
	if (UserWidgetClass && BP->ParentClass->IsChildOf(UserWidgetClass))
	{
		return TEXT("Widget");
	}

	if (BP->ParentClass->IsChildOf(UActorComponent::StaticClass()))
	{
		return TEXT("Component");
	}

	if (BP->ParentClass->IsChildOf(AActor::StaticClass()))
	{
		return TEXT("Actor");
	}

	return TEXT("Unknown");
}

namespace
{

	/** Helper: Determine parent class and blueprint class from type string */
	bool ResolveTypeToClasses(
		const FString& TypeStr,
		UClass*& OutParentClass,
		TSubclassOf<UBlueprint>& OutBlueprintClass,
		FString& OutError)
	{
		if (TypeStr == TEXT("Actor"))
		{
			OutParentClass = AActor::StaticClass();
			OutBlueprintClass = UBlueprint::StaticClass();
			return true;
		}

		if (TypeStr == TEXT("Component"))
		{
			OutParentClass = UActorComponent::StaticClass();
			OutBlueprintClass = UBlueprint::StaticClass();
			return true;
		}

		if (TypeStr == TEXT("Widget"))
		{
			// Use dynamic class resolution to avoid compile-time dependency on UMG
			static UClass* UserWidgetClass = FindObject<UClass>(nullptr, TEXT("/Script/UMG.UserWidget"));
			static UClass* WidgetBlueprintClass = FindObject<UClass>(nullptr, TEXT("/Script/UMGEditor.WidgetBlueprint"));
			if (!UserWidgetClass || !WidgetBlueprintClass)
			{
				OutError = TEXT("Widget Blueprint classes not available (UMG module not loaded)");
				return false;
			}
			OutParentClass = UserWidgetClass;
			OutBlueprintClass = WidgetBlueprintClass;
			return true;
		}

		if (TypeStr == TEXT("Interface"))
		{
			OutParentClass = UInterface::StaticClass();
			OutBlueprintClass = UBlueprint::StaticClass();
			return true;
		}

		if (TypeStr == TEXT("FunctionLibrary"))
		{
			OutParentClass = UBlueprintFunctionLibrary::StaticClass();
			OutBlueprintClass = UBlueprint::StaticClass();
			return true;
		}

		OutError = FString::Printf(TEXT("Invalid Blueprint type: %s (supported: Actor, Component, Widget, Interface, FunctionLibrary)"), *TypeStr);
		return false;
	}

	/** Helper: Determine Blueprint type string from AssetRegistry tags (without loading the asset). */
	FString DetermineTypeFromAssetData(const FAssetData& AssetData)
	{
		const FString BlueprintTypeTag = AssetData.GetTagValueRef<FString>(FBlueprintTags::BlueprintType);
		if (BlueprintTypeTag == TEXT("BPType_Interface"))
		{
			return TEXT("Interface");
		}
		if (BlueprintTypeTag == TEXT("BPType_FunctionLibrary"))
		{
			return TEXT("FunctionLibrary");
		}

		FString ParentClassName;
		if (!AssetData.GetTagValue(FBlueprintTags::NativeParentClassPath, ParentClassName))
		{
			AssetData.GetTagValue(FBlueprintTags::ParentClassPath, ParentClassName);
		}

		if (!ParentClassName.IsEmpty())
		{
			const FString ParentClassPath = FPackageName::ExportTextPathToObjectPath(ParentClassName);

			// Resolve to UClass* — native classes are always loaded in editor, no disk I/O
			UClass* ParentClass = FindObject<UClass>(nullptr, *ParentClassPath);
			if (ParentClass)
			{
				static UClass* UserWidgetClass = FindObject<UClass>(nullptr, TEXT("/Script/UMG.UserWidget"));
				if (UserWidgetClass && ParentClass->IsChildOf(UserWidgetClass))
				{
					return TEXT("Widget");
				}
				if (ParentClass->IsChildOf(UActorComponent::StaticClass()))
				{
					return TEXT("Component");
				}
				if (ParentClass->IsChildOf(AActor::StaticClass()))
				{
					return TEXT("Actor");
				}
				if (ParentClass->IsChildOf(UBlueprintFunctionLibrary::StaticClass()))
				{
					return TEXT("FunctionLibrary");
				}
				if (ParentClass->IsChildOf(UInterface::StaticClass()))
				{
					return TEXT("Interface");
				}
				return TEXT("Unknown");
			}

			// Fallback: string matching for unloaded Blueprint-only parents
			if (ParentClassPath.Contains(TEXT("UserWidget")))
			{
				return TEXT("Widget");
			}
			if (ParentClassPath.Contains(TEXT("BlueprintFunctionLibrary")))
			{
				return TEXT("FunctionLibrary");
			}
			if (ParentClassPath.Contains(TEXT("ActorComponent")))
			{
				return TEXT("Component");
			}
			if (ParentClassPath.Contains(TEXT("Interface")))
			{
				return TEXT("Interface");
			}
			if (ParentClassPath.Contains(TEXT("Actor")))
			{
				return TEXT("Actor");
			}
		}

		return TEXT("Unknown");
	}

	FString ToSeverity(const UEdGraphNode* Node)
	{
		if (!Node)
		{
			return TEXT("error");
		}

		return (Node->ErrorType <= EMessageSeverity::Error) ? TEXT("error") : TEXT("warning");
	}

	void AddReferencedMetadata(const UEdGraphNode* Node, const TSharedPtr<FJsonObject>& Diagnostic)
	{
		if (!Node || !Diagnostic.IsValid())
		{
			return;
		}

		const UBlueprint* Blueprint = FBlueprintEditorUtils::FindBlueprintForNode(Node);
		UClass* SelfScope = nullptr;
		if (Blueprint)
		{
			SelfScope = Blueprint->GeneratedClass ? Blueprint->GeneratedClass : Blueprint->SkeletonGeneratedClass;
		}

		if (const UK2Node_CallFunction* CallNode = Cast<UK2Node_CallFunction>(Node))
		{
			UClass* ParentClass = CallNode->FunctionReference.GetMemberParentClass(SelfScope);
			if (!ParentClass)
			{
				ParentClass = CallNode->FunctionReference.GetMemberParentClass();
			}
			if (ParentClass)
			{
				Diagnostic->SetStringField(TEXT("referenced_class"), ParentClass->GetName());
			}

			const FName MemberName = CallNode->FunctionReference.GetMemberName();
			if (!MemberName.IsNone())
			{
				Diagnostic->SetStringField(TEXT("referenced_member"), MemberName.ToString());
			}
			return;
		}

		if (const UK2Node_Variable* VariableNode = Cast<UK2Node_Variable>(Node))
		{
			UClass* ParentClass = VariableNode->VariableReference.GetMemberParentClass(SelfScope);
			if (!ParentClass)
			{
				ParentClass = VariableNode->VariableReference.GetMemberParentClass();
			}
			if (ParentClass)
			{
				Diagnostic->SetStringField(TEXT("referenced_class"), ParentClass->GetName());
			}

			const FName MemberName = VariableNode->VariableReference.GetMemberName();
			if (!MemberName.IsNone())
			{
				Diagnostic->SetStringField(TEXT("referenced_member"), MemberName.ToString());
			}
			return;
		}

		if (const UK2Node_Event* EventNode = Cast<UK2Node_Event>(Node))
		{
			UClass* ParentClass = EventNode->EventReference.GetMemberParentClass(SelfScope);
			if (!ParentClass)
			{
				ParentClass = EventNode->EventReference.GetMemberParentClass();
			}
			if (ParentClass)
			{
				Diagnostic->SetStringField(TEXT("referenced_class"), ParentClass->GetName());
			}

			const FName MemberName = EventNode->EventReference.GetMemberName();
			if (!MemberName.IsNone())
			{
				Diagnostic->SetStringField(TEXT("referenced_member"), MemberName.ToString());
			}
			return;
		}

		if (const UK2Node_DynamicCast* DynamicCastNode = Cast<UK2Node_DynamicCast>(Node))
		{
			if (const UClass* TargetClass = DynamicCastNode->TargetType)
			{
				Diagnostic->SetStringField(TEXT("referenced_class"), TargetClass->GetName());
			}
		}
	}

	TSharedPtr<FJsonObject> BuildDiagnosticObject(const UEdGraphNode* Node)
	{
		TSharedPtr<FJsonObject> Diagnostic = MakeShared<FJsonObject>();
		Diagnostic->SetStringField(TEXT("graph"), Node && Node->GetGraph() ? Node->GetGraph()->GetName() : TEXT(""));
		Diagnostic->SetStringField(TEXT("node_id"), Node ? Node->NodeGuid.ToString() : TEXT(""));
		Diagnostic->SetStringField(TEXT("node_class"), Node ? Node->GetClass()->GetName() : TEXT(""));
		Diagnostic->SetStringField(TEXT("node_name"),
			Node ? Node->GetNodeTitle(ENodeTitleType::ListView).ToString() : TEXT(""));
		Diagnostic->SetStringField(TEXT("severity"), ToSeverity(Node));
		Diagnostic->SetStringField(TEXT("message"), Node ? Node->ErrorMsg : TEXT(""));
		AddReferencedMetadata(Node, Diagnostic);
		return Diagnostic;
	}
}

FCortexCommandResult FCortexBPAssetOps::Create(const TSharedPtr<FJsonObject>& Params)
{
	FCortexCommandResult Result;

	// Validate params
	if (!Params.IsValid())
	{
		Result.bSuccess = false;
		Result.ErrorCode = CortexErrorCodes::InvalidField;
		Result.ErrorMessage = TEXT("Missing params object");
		return Result;
	}

	FString Name;
	if (!Params->TryGetStringField(TEXT("name"), Name) || Name.IsEmpty())
	{
		Result.bSuccess = false;
		Result.ErrorCode = CortexErrorCodes::InvalidField;
		Result.ErrorMessage = TEXT("Missing or empty 'name' field");
		return Result;
	}

	FString Path;
	if (!Params->TryGetStringField(TEXT("path"), Path) || Path.IsEmpty())
	{
		Result.bSuccess = false;
		Result.ErrorCode = CortexErrorCodes::InvalidField;
		Result.ErrorMessage = TEXT("Missing or empty 'path' field");
		return Result;
	}

	FString TypeStr;
	Params->TryGetStringField(TEXT("type"), TypeStr);

	FString ParentClassStr;
	Params->TryGetStringField(TEXT("parent_class"), ParentClassStr);

	UClass* ParentClass = nullptr;
	TSubclassOf<UBlueprint> BlueprintClass;

	if (!ParentClassStr.IsEmpty())
	{
		// Resolve custom parent class
		// Try full path first (e.g., "/Script/CortexSandbox.CortexBenchmarkActor")
		ParentClass = FindObject<UClass>(nullptr, *ParentClassStr);

		// Try short name lookup if full path didn't work
		if (!ParentClass)
		{
			ParentClass = FindFirstObject<UClass>(*ParentClassStr, EFindFirstObjectOptions::NativeFirst);
		}

		if (!ParentClass)
		{
			Result.bSuccess = false;
			Result.ErrorCode = CortexErrorCodes::InvalidParentClass;
			Result.ErrorMessage = FString::Printf(TEXT("Could not find class: %s"), *ParentClassStr);
			return Result;
		}

		// Determine correct BlueprintClass based on parent hierarchy
		static UClass* UserWidgetClass = FindObject<UClass>(nullptr, TEXT("/Script/UMG.UserWidget"));
		static UClass* WidgetBlueprintClass = FindObject<UClass>(nullptr, TEXT("/Script/UMGEditor.WidgetBlueprint"));
		if (UserWidgetClass && WidgetBlueprintClass && ParentClass->IsChildOf(UserWidgetClass))
		{
			BlueprintClass = WidgetBlueprintClass;
		}
		else
		{
			BlueprintClass = UBlueprint::StaticClass();
		}
	}
	else
	{
		// Existing behavior: require type param
		if (TypeStr.IsEmpty())
		{
			Result.bSuccess = false;
			Result.ErrorCode = CortexErrorCodes::InvalidField;
			Result.ErrorMessage = TEXT("Missing 'type' or 'parent_class' field");
			return Result;
		}

		FString TypeError;
		if (!ResolveTypeToClasses(TypeStr, ParentClass, BlueprintClass, TypeError))
		{
			Result.bSuccess = false;
			Result.ErrorCode = CortexErrorCodes::InvalidBlueprintType;
			Result.ErrorMessage = TypeError;
			return Result;
		}
	}

	// Combine name and path to form package path (strip trailing slash to avoid double slashes)
	FString NormalizedPath = Path.TrimEnd();
	while (NormalizedPath.EndsWith(TEXT("/")))
	{
		NormalizedPath.LeftChopInline(1);
	}
	FString PackagePath = FString::Printf(TEXT("%s/%s"), *NormalizedPath, *Name);

	// Normalize path (ensure it starts with /Game/)
	if (!PackagePath.StartsWith(TEXT("/")))
	{
		PackagePath = TEXT("/Game/") + PackagePath;
	}
	else if (!PackagePath.StartsWith(TEXT("/Game/")))
	{
		PackagePath = TEXT("/Game") + PackagePath;
	}

	// Check if asset already exists (guard with FindPackage/DoesPackageExist to avoid warnings)
	FString ExistingPkgName = FPackageName::ObjectPathToPackageName(PackagePath);
	bool bPackageExists = FindPackage(nullptr, *ExistingPkgName) || FPackageName::DoesPackageExist(ExistingPkgName);
	UObject* ExistingAsset = bPackageExists ? LoadObject<UBlueprint>(nullptr, *PackagePath) : nullptr;
	if (ExistingAsset)
	{
		Result.bSuccess = false;
		Result.ErrorCode = CortexErrorCodes::BlueprintAlreadyExists;
		Result.ErrorMessage = FString::Printf(TEXT("Blueprint already exists at path: %s"), *PackagePath);
		return Result;
	}

	// Create the Blueprint with undo/redo support
	FScopedTransaction Transaction(FText::FromString(
		FString::Printf(TEXT("Cortex: Create Blueprint %s"), *Name)
	));

	UPackage* Package = CreatePackage(*PackagePath);
	if (!Package)
	{
		Result.bSuccess = false;
		Result.ErrorCode = CortexErrorCodes::SerializationError;
		Result.ErrorMessage = TEXT("Failed to create package");
		return Result;
	}

	EBlueprintType BPType = BPTYPE_Normal;
	if (!ParentClassStr.IsEmpty())
	{
		// When using custom parent_class, infer BPType from hierarchy
		if (ParentClass->IsChildOf(UInterface::StaticClass()))
		{
			BPType = BPTYPE_Interface;
		}
		else if (ParentClass->IsChildOf(UBlueprintFunctionLibrary::StaticClass()))
		{
			BPType = BPTYPE_FunctionLibrary;
		}
	}
	else
	{
		if (TypeStr == TEXT("Interface"))
		{
			BPType = BPTYPE_Interface;
		}
		else if (TypeStr == TEXT("FunctionLibrary"))
		{
			BPType = BPTYPE_FunctionLibrary;
		}
	}

	UBlueprint* NewBP = FKismetEditorUtilities::CreateBlueprint(
		ParentClass,
		Package,
		FName(*Name),
		BPType,
		BlueprintClass,
		UBlueprintGeneratedClass::StaticClass(),
		NAME_None
	);

	if (!NewBP)
	{
		Result.bSuccess = false;
		Result.ErrorCode = CortexErrorCodes::SerializationError;
		Result.ErrorMessage = TEXT("Failed to create Blueprint");
		return Result;
	}

	// Save the Blueprint to disk
	Package->MarkPackageDirty();
	FAssetRegistryModule::AssetCreated(NewBP);

	// Save the package
	const FString PackageFilename =
		FPackageName::LongPackageNameToFilename(PackagePath, FPackageName::GetAssetPackageExtension());
	FSavePackageArgs SaveArgs;
	SaveArgs.TopLevelFlags = RF_Standalone;
	if (!UPackage::SavePackage(Package, NewBP, *PackageFilename, SaveArgs))
	{
		Result.bSuccess = false;
		Result.ErrorCode = CortexErrorCodes::SerializationError;
		Result.ErrorMessage = FString::Printf(TEXT("Failed to save Blueprint package: %s"), *PackagePath);
		return Result;
	}

	// Return success with asset path
	Result.bSuccess = true;
	Result.Data = MakeShared<FJsonObject>();
	Result.Data->SetStringField(TEXT("asset_path"), PackagePath);
	// Infer type from the created Blueprint when type wasn't explicitly provided
	FString ResponseType = TypeStr;
	if (ResponseType.IsEmpty())
	{
		ResponseType = FCortexBPAssetOps::DetermineBlueprintType(NewBP);
	}
	Result.Data->SetStringField(TEXT("type"), ResponseType);
	Result.Data->SetStringField(TEXT("parent_class"), ParentClass ? ParentClass->GetName() : TEXT(""));
	Result.Data->SetBoolField(TEXT("created"), true);

	return Result;
}

FCortexCommandResult FCortexBPAssetOps::List(const TSharedPtr<FJsonObject>& Params)
{
	FCortexCommandResult Result;

	// Get optional path filter
	FString PathFilter;
	if (Params.IsValid())
	{
		Params->TryGetStringField(TEXT("path"), PathFilter);
	}

	// Query AssetRegistry for all Blueprint assets
	FAssetRegistryModule& AssetRegistryModule = FModuleManager::LoadModuleChecked<FAssetRegistryModule>("AssetRegistry");
	IAssetRegistry& AssetRegistry = AssetRegistryModule.Get();

	TArray<FAssetData> BlueprintAssets;
	FARFilter Filter;
	Filter.ClassPaths.Add(UBlueprint::StaticClass()->GetClassPathName());
	Filter.bRecursiveClasses = true;

	// Apply path filter if provided
	if (!PathFilter.IsEmpty())
	{
		// Normalize path (ensure it starts with /Game/)
		if (!PathFilter.StartsWith(TEXT("/")))
		{
			PathFilter = TEXT("/Game/") + PathFilter;
		}
		else if (!PathFilter.StartsWith(TEXT("/Game/")))
		{
			PathFilter = TEXT("/Game") + PathFilter;
		}

		Filter.PackagePaths.Add(FName(*PathFilter));
		Filter.bRecursivePaths = true;
	}

	AssetRegistry.GetAssets(Filter, BlueprintAssets);

	// Build result array
	TArray<TSharedPtr<FJsonValue>> BlueprintsArray;
	for (const FAssetData& AssetData : BlueprintAssets)
	{
		TSharedPtr<FJsonObject> BPObj = MakeShared<FJsonObject>();

		// Asset path
		BPObj->SetStringField(TEXT("asset_path"), AssetData.GetObjectPathString());

		// Asset name
		BPObj->SetStringField(TEXT("name"), AssetData.AssetName.ToString());

		// Resolve type from AssetRegistry tags to avoid loading every Blueprint asset.
		BPObj->SetStringField(TEXT("type"), DetermineTypeFromAssetData(AssetData));

		// Parent class (extracted from AssetRegistry tags — no load required)
		FString ParentClassName;
		if (!AssetData.GetTagValue(FBlueprintTags::NativeParentClassPath, ParentClassName))
		{
			AssetData.GetTagValue(FBlueprintTags::ParentClassPath, ParentClassName);
		}
		if (!ParentClassName.IsEmpty())
		{
			const FString ClassPath = FPackageName::ExportTextPathToObjectPath(ParentClassName);
			UClass* ParentClass = FindObject<UClass>(nullptr, *ClassPath);
			BPObj->SetStringField(TEXT("parent_class"),
				ParentClass ? ParentClass->GetName() : FPackageName::GetShortName(ClassPath));
		}
		else
		{
			BPObj->SetStringField(TEXT("parent_class"), TEXT(""));
		}

		BlueprintsArray.Add(MakeShared<FJsonValueObject>(BPObj));
	}

	// Return success with blueprints array
	Result.bSuccess = true;
	Result.Data = MakeShared<FJsonObject>();
	Result.Data->SetArrayField(TEXT("blueprints"), BlueprintsArray);
	Result.Data->SetNumberField(TEXT("count"), BlueprintsArray.Num());

	return Result;
}

FCortexCommandResult FCortexBPAssetOps::GetInfo(const TSharedPtr<FJsonObject>& Params)
{
	FCortexCommandResult Result;

	// Validate params
	if (!Params.IsValid())
	{
		Result.bSuccess = false;
		Result.ErrorCode = CortexErrorCodes::InvalidField;
		Result.ErrorMessage = TEXT("Missing params object");
		return Result;
	}

	FString AssetPath;
	if (!Params->TryGetStringField(TEXT("asset_path"), AssetPath) || AssetPath.IsEmpty())
	{
		Result.bSuccess = false;
		Result.ErrorCode = CortexErrorCodes::InvalidField;
		Result.ErrorMessage = TEXT("Missing or empty 'asset_path' field");
		return Result;
	}

	// Load Blueprint
	FString LoadError;
	UBlueprint* BP = LoadBlueprint(AssetPath, LoadError);
	if (!BP)
	{
		return FCortexCommandRouter::Error(CortexErrorCodes::BlueprintNotFound, LoadError);
	}

	// Build info object
	TSharedPtr<FJsonObject> InfoObj = MakeShared<FJsonObject>();

	// Basic info
	InfoObj->SetStringField(TEXT("name"), BP->GetName());
	InfoObj->SetStringField(TEXT("asset_path"), AssetPath);

	// Determine type using shared helper
	FString Type = FCortexBPAssetOps::DetermineBlueprintType(BP);
	InfoObj->SetStringField(TEXT("type"), Type);

	// Parent class
	if (BP->ParentClass)
	{
		InfoObj->SetStringField(TEXT("parent_class"), BP->ParentClass->GetName());
	}

	// Compilation status
	bool bIsCompiled = (BP->Status == BS_UpToDate || BP->Status == BS_UpToDateWithWarnings);
	InfoObj->SetBoolField(TEXT("is_compiled"), bIsCompiled);

	// Variables
	TArray<TSharedPtr<FJsonValue>> VariablesArray;
	for (FBPVariableDescription& Variable : BP->NewVariables)
	{
		TSharedPtr<FJsonObject> VarObj = MakeShared<FJsonObject>();
		VarObj->SetStringField(TEXT("name"), Variable.VarName.ToString());
		VarObj->SetStringField(TEXT("type"), CortexBPTypeUtils::FriendlyTypeName(Variable.VarType));

		// Add default value if available
		if (!Variable.DefaultValue.IsEmpty())
		{
			VarObj->SetStringField(TEXT("default_value"), Variable.DefaultValue);
		}

		// Add exposure status
		bool bIsExposed = (Variable.PropertyFlags & CPF_BlueprintVisible) != 0;
		VarObj->SetBoolField(TEXT("is_exposed"), bIsExposed);

		// Add category if set
		if (!Variable.Category.IsEmpty())
		{
			VarObj->SetStringField(TEXT("category"), Variable.Category.ToString());
		}

		VariablesArray.Add(MakeShared<FJsonValueObject>(VarObj));
	}
	InfoObj->SetArrayField(TEXT("variables"), VariablesArray);

	// Functions (use Blueprint->FunctionGraphs instead of GetAllGraphs)
	TArray<TSharedPtr<FJsonValue>> FunctionsArray;
	for (UEdGraph* Graph : BP->FunctionGraphs)
	{
		if (!Graph)
		{
			continue;
		}

		TSharedPtr<FJsonObject> FuncObj = MakeShared<FJsonObject>();
		FuncObj->SetStringField(TEXT("name"), Graph->GetName());

		// Serialize inputs from entry node's UserDefinedPins
		TArray<TSharedPtr<FJsonValue>> InputsArr;
		TArray<TSharedPtr<FJsonValue>> OutputsArr;

		for (UEdGraphNode* Node : Graph->Nodes)
		{
			if (UK2Node_FunctionEntry* Entry = Cast<UK2Node_FunctionEntry>(Node))
			{
				for (const TSharedPtr<FUserPinInfo>& Pin : Entry->UserDefinedPins)
				{
					TSharedPtr<FJsonObject> P = MakeShared<FJsonObject>();
					P->SetStringField(TEXT("name"), Pin->PinName.ToString());
					P->SetStringField(TEXT("type"), CortexBPTypeUtils::FriendlyTypeName(Pin->PinType));
					InputsArr.Add(MakeShared<FJsonValueObject>(P));
				}
			}
			else if (UK2Node_FunctionResult* ResultNode = Cast<UK2Node_FunctionResult>(Node))
			{
				for (const TSharedPtr<FUserPinInfo>& Pin : ResultNode->UserDefinedPins)
				{
					TSharedPtr<FJsonObject> P = MakeShared<FJsonObject>();
					P->SetStringField(TEXT("name"), Pin->PinName.ToString());
					P->SetStringField(TEXT("type"), CortexBPTypeUtils::FriendlyTypeName(Pin->PinType));
					OutputsArr.Add(MakeShared<FJsonValueObject>(P));
				}
			}
		}

		FuncObj->SetArrayField(TEXT("inputs"), InputsArr);
		FuncObj->SetArrayField(TEXT("outputs"), OutputsArr);

		FunctionsArray.Add(MakeShared<FJsonValueObject>(FuncObj));
	}
	InfoObj->SetArrayField(TEXT("functions"), FunctionsArray);

	// Graphs (all graphs with node counts)
	TArray<TSharedPtr<FJsonValue>> GraphsArray;
	TArray<UEdGraph*> AllGraphs;
	BP->GetAllGraphs(AllGraphs);

	for (UEdGraph* Graph : AllGraphs)
	{
		if (Graph)
		{
			TSharedPtr<FJsonObject> GraphObj = MakeShared<FJsonObject>();
			GraphObj->SetStringField(TEXT("name"), Graph->GetName());
			GraphObj->SetNumberField(TEXT("node_count"), Graph->Nodes.Num());
			GraphsArray.Add(MakeShared<FJsonValueObject>(GraphObj));
		}
	}
	InfoObj->SetArrayField(TEXT("graphs"), GraphsArray);

	// Return success
	Result.bSuccess = true;
	Result.Data = InfoObj;

	return Result;
}

FCortexCommandResult FCortexBPAssetOps::Delete(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath;
	if (!Params.IsValid() || !Params->TryGetStringField(TEXT("asset_path"), AssetPath))
	{
		return FCortexCommandRouter::Error(
			CortexErrorCodes::InvalidField,
			TEXT("Missing required param: asset_path")
		);
	}

	bool bForce = false;
	Params->TryGetBoolField(TEXT("force"), bForce);

	// Load blueprint
	FString LoadError;
	UBlueprint* Blueprint = LoadBlueprint(AssetPath, LoadError);
	if (Blueprint == nullptr)
	{
		return FCortexCommandRouter::Error(
			CortexErrorCodes::BlueprintNotFound,
			LoadError
		);
	}

	// Check for references unless force is true
	if (!bForce)
	{
		IAssetRegistry& AssetRegistry =
			FModuleManager::LoadModuleChecked<FAssetRegistryModule>("AssetRegistry").Get();

		TArray<FAssetIdentifier> Referencers;
		AssetRegistry.GetReferencers(
			FAssetIdentifier(Blueprint->GetOutermost()->GetFName()),
			Referencers
		);

		if (Referencers.Num() > 0)
		{
			TArray<TSharedPtr<FJsonValue>> RefArray;
			for (const FAssetIdentifier& Ref : Referencers)
			{
				RefArray.Add(MakeShared<FJsonValueString>(Ref.ToString()));
			}
			TSharedPtr<FJsonObject> Details = MakeShared<FJsonObject>();
			Details->SetArrayField(TEXT("references"), RefArray);

			return FCortexCommandRouter::Error(
				CortexErrorCodes::HasReferences,
				FString::Printf(TEXT("Blueprint has %d references. Use force=true to delete anyway."),
					Referencers.Num()),
				Details
			);
		}
	}

	FScopedTransaction Transaction(FText::FromString(
		FString::Printf(TEXT("Cortex: Delete Blueprint %s"), *Blueprint->GetName())
	));

	// Get package file path BEFORE destroying
	UPackage* Package = Blueprint->GetOutermost();
	FString PackageFilename = FPackageName::LongPackageNameToFilename(
		Package->GetName(), FPackageName::GetAssetPackageExtension());

	// Delete the .uasset file from disk first.
	if (FPlatformFileManager::Get().GetPlatformFile().FileExists(*PackageFilename))
	{
		IFileManager::Get().Delete(*PackageFilename);
	}

	// Prevent a LogAssetRegistry warning caused by a race between ForceDeleteObjects and
	// the file watcher. ForceDeleteObjects calls AssetDeleted, which calls AddEmptyPackage
	// only when UPackage::IsEmptyPackage returns true (i.e. no other RF_Public assets
	// remain in the package). A pending FCA_Added event — queued by SavePackage when the
	// asset was originally created — fires on the next engine tick; if the package is in
	// CachedEmptyPackages at that point, the warning fires.
	//
	// Fix: place a temporary guard object (RF_Public, not an asset in the registry) in
	// the package so IsEmptyPackage returns false, skipping AddEmptyPackage entirely.
	// The guard is cleared and marked as garbage immediately after ForceDeleteObjects;
	// the subsequent FCA_Removed event (from the file deletion above) cleans up any
	// remaining registry state.
	UTextBuffer* DeleteGuard = nullptr;
	if (IsValid(Package))
	{
		DeleteGuard = NewObject<UTextBuffer>(Package, TEXT("__CortexDeleteGuard__"), RF_Public);
	}

	// Use ObjectTools for proper asset deletion (handles GC coordination, references)
	TArray<UObject*> ObjectsToDelete;
	ObjectsToDelete.Add(Blueprint);
	int32 DeletedCount = ObjectTools::ForceDeleteObjects(ObjectsToDelete, false);

	// Remove the guard — strip asset flags so it is invisible to IsAsset() queries.
	// ForceDeleteObjects purges the undo buffer which may temporarily root objects,
	// so we avoid MarkAsGarbage() to prevent an IsRooted() assertion. The guard has
	// no external references and no RF_Standalone, so GC collects it naturally when
	// the package is unloaded after the file watcher processes FCA_Removed.
	if (DeleteGuard)
	{
		DeleteGuard->ClearFlags(RF_Public | RF_Standalone);
	}

	if (DeletedCount == 0)
	{
		return FCortexCommandRouter::Error(
			CortexErrorCodes::SerializationError,
			FString::Printf(TEXT("Failed to delete Blueprint: %s (may have references)"), *AssetPath)
		);
	}

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetBoolField(TEXT("deleted"), true);
	Data->SetStringField(TEXT("asset_path"), AssetPath);

	UE_LOG(LogCortexBlueprint, Log, TEXT("Deleted Blueprint: %s"), *AssetPath);

	return FCortexCommandRouter::Success(Data);
}

FCortexCommandResult FCortexBPAssetOps::Duplicate(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath;
	FString NewName;

	if (!Params.IsValid()
		|| !Params->TryGetStringField(TEXT("asset_path"), AssetPath)
		|| !Params->TryGetStringField(TEXT("new_name"), NewName))
	{
		return FCortexCommandRouter::Error(
			CortexErrorCodes::InvalidField,
			TEXT("Missing required params: asset_path, new_name")
		);
	}

	// Load source blueprint
	FString LoadError;
	UBlueprint* SourceBP = LoadBlueprint(AssetPath, LoadError);
	if (SourceBP == nullptr)
	{
		return FCortexCommandRouter::Error(
			CortexErrorCodes::BlueprintNotFound,
			LoadError
		);
	}

	// Determine destination path
	FString NewPath;
	Params->TryGetStringField(TEXT("new_path"), NewPath);
	if (NewPath.IsEmpty())
	{
		// Default to same directory as source
		NewPath = FPackageName::GetLongPackagePath(SourceBP->GetOutermost()->GetName());
	}

	while (NewPath.EndsWith(TEXT("/"))) { NewPath.LeftChopInline(1); }
	FString NewPackagePath = FString::Printf(TEXT("%s/%s"), *NewPath, *NewName);

	// Check if destination already exists
	UPackage* ExistingPackage = FindPackage(nullptr, *NewPackagePath);
	if (ExistingPackage != nullptr)
	{
		UObject* ExistingAsset = StaticFindObject(UBlueprint::StaticClass(), ExistingPackage, *NewName);
		if (ExistingAsset != nullptr)
		{
			return FCortexCommandRouter::Error(
				CortexErrorCodes::BlueprintAlreadyExists,
				FString::Printf(TEXT("Asset already exists: %s"), *NewPackagePath)
			);
		}
	}

	FScopedTransaction Transaction(FText::FromString(
		FString::Printf(TEXT("Cortex:Duplicate Blueprint %s"), *NewName)
	));

	// Create destination package
	UPackage* NewPackage = CreatePackage(*NewPackagePath);

	// Duplicate the object
	UBlueprint* NewBP = Cast<UBlueprint>(
		StaticDuplicateObject(SourceBP, NewPackage, FName(*NewName))
	);

	if (NewBP == nullptr)
	{
		return FCortexCommandRouter::Error(
			CortexErrorCodes::AssetNotFound,
			TEXT("Failed to duplicate Blueprint")
		);
	}

	FAssetRegistryModule::AssetCreated(NewBP);
	NewPackage->MarkPackageDirty();

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("source"), AssetPath);
	Data->SetStringField(TEXT("new_asset_path"), NewPackagePath);
	Data->SetBoolField(TEXT("duplicated"), true);

	UE_LOG(LogCortexBlueprint, Log, TEXT("Duplicated %s -> %s"), *AssetPath, *NewPackagePath);

	return FCortexCommandRouter::Success(Data);
}

FCortexCommandResult FCortexBPAssetOps::Compile(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath;
	if (!Params.IsValid() || !Params->TryGetStringField(TEXT("asset_path"), AssetPath))
	{
		return FCortexCommandRouter::Error(
			CortexErrorCodes::InvalidField,
			TEXT("Missing required param: asset_path")
		);
	}

	// Load blueprint
	FString LoadError;
	UBlueprint* Blueprint = LoadBlueprint(AssetPath, LoadError);
	if (Blueprint == nullptr)
	{
		return FCortexCommandRouter::Error(
			CortexErrorCodes::BlueprintNotFound,
			LoadError
		);
	}

	FCompilerResultsLog CompilerResults;
	CompilerResults.bSilentMode = true;
	CompilerResults.bAnnotateMentionedNodes = true;
	FKismetEditorUtilities::CompileBlueprint(Blueprint, EBlueprintCompileOptions::None, &CompilerResults);

	const int32 ErrorCount = CompilerResults.NumErrors;
	const int32 WarningCount = CompilerResults.NumWarnings;

	TArray<TSharedPtr<FJsonValue>> DiagnosticsArray;
	DiagnosticsArray.Reserve(CompilerResults.AnnotatedNodes.Num());
	for (const TWeakObjectPtr<UEdGraphNode>& WeakNode : CompilerResults.AnnotatedNodes)
	{
		UEdGraphNode* Node = WeakNode.Get();
		if (!Node || !Node->bHasCompilerMessage)
		{
			continue;
		}

		DiagnosticsArray.Add(MakeShared<FJsonValueObject>(BuildDiagnosticObject(Node)));
	}

	auto BuildCompilePayload = [&]()
	{
		TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
		Payload->SetStringField(TEXT("asset_path"), AssetPath);
		Payload->SetStringField(
			TEXT("compile_status"),
			ErrorCount > 0 ? TEXT("error") : (WarningCount > 0 ? TEXT("warning") : TEXT("success")));
		Payload->SetNumberField(TEXT("error_count"), ErrorCount);
		Payload->SetNumberField(TEXT("warning_count"), WarningCount);
		Payload->SetArrayField(TEXT("diagnostics"), DiagnosticsArray);
		return Payload;
	};

	if (ErrorCount > 0)
	{
		return FCortexCommandRouter::Error(
			CortexErrorCodes::CompileFailed,
			FString::Printf(TEXT("Blueprint compilation failed with %d errors"), ErrorCount),
			BuildCompilePayload());
	}

	UE_LOG(LogCortexBlueprint, Log, TEXT("Compiled Blueprint: %s"), *AssetPath);

	return FCortexCommandRouter::Success(BuildCompilePayload());
}

FCortexCommandResult FCortexBPAssetOps::Save(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath;
	if (!Params.IsValid() || !Params->TryGetStringField(TEXT("asset_path"), AssetPath))
	{
		return FCortexCommandRouter::Error(
			CortexErrorCodes::InvalidField,
			TEXT("Missing required param: asset_path")
		);
	}

	// Level Script Blueprints are saved via save_level, not bp.save
	if (AssetPath.StartsWith(TEXT("__level_bp__:")))
	{
		const FString MapPath = AssetPath.Mid(FCString::Strlen(TEXT("__level_bp__:")));
		return FCortexCommandRouter::Error(
			TEXT("LevelBlueprintSaveError"),
			FString::Printf(
				TEXT("Use save_level with '%s' to persist Level Blueprint changes — bp.save does not apply to Level Script Blueprints"),
				*MapPath)
		);
	}

	// Load blueprint
	FString LoadError;
	UBlueprint* Blueprint = LoadBlueprint(AssetPath, LoadError);
	if (Blueprint == nullptr)
	{
		return FCortexCommandRouter::Error(
			CortexErrorCodes::BlueprintNotFound,
			LoadError
		);
	}

	UPackage* Package = Blueprint->GetOutermost();
	FString PackageFilename = FPackageName::LongPackageNameToFilename(
		Package->GetName(), FPackageName::GetAssetPackageExtension());

	FSavePackageArgs SaveArgs;
	SaveArgs.TopLevelFlags = RF_Standalone;
	bool bSaved = UPackage::SavePackage(Package, Blueprint, *PackageFilename, SaveArgs);

	if (!bSaved)
	{
		return FCortexCommandRouter::Error(
			CortexErrorCodes::SerializationError,
			FString::Printf(TEXT("Failed to save Blueprint: %s"), *AssetPath)
		);
	}

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetBoolField(TEXT("saved"), true);
	Data->SetStringField(TEXT("asset_path"), AssetPath);

	UE_LOG(LogCortexBlueprint, Log, TEXT("Saved Blueprint: %s"), *AssetPath);

	return FCortexCommandRouter::Success(Data);
}

FCortexCommandResult FCortexBPAssetOps::Rename(const TSharedPtr<FJsonObject>& Params)
{
	if (!Params.IsValid())
	{
		return FCortexCommandRouter::Error(
			CortexErrorCodes::InvalidField,
			TEXT("Missing params object")
		);
	}

	FString SourcePath;
	FString DestPath;
	if (!Params->TryGetStringField(TEXT("source_path"), SourcePath) || SourcePath.IsEmpty())
	{
		return FCortexCommandRouter::Error(
			CortexErrorCodes::InvalidField,
			TEXT("Missing required param: source_path")
		);
	}
	if (!Params->TryGetStringField(TEXT("dest_path"), DestPath) || DestPath.IsEmpty())
	{
		return FCortexCommandRouter::Error(
			CortexErrorCodes::InvalidField,
			TEXT("Missing required param: dest_path")
		);
	}

	// Normalize destination to object path form (/Game/Path/AssetName)
	FString NormalizedDestPath = DestPath;
	if (!NormalizedDestPath.StartsWith(TEXT("/")))
	{
		NormalizedDestPath = TEXT("/Game/") + NormalizedDestPath;
	}
	else if (!NormalizedDestPath.StartsWith(TEXT("/Game/")))
	{
		NormalizedDestPath = TEXT("/Game") + NormalizedDestPath;
	}

	if (SourcePath == NormalizedDestPath)
	{
		return FCortexCommandRouter::Error(
			CortexErrorCodes::InvalidField,
			TEXT("source_path and dest_path must be different")
		);
	}

	FString LoadError;
	UBlueprint* SourceBlueprint = LoadBlueprint(SourcePath, LoadError);
	if (!SourceBlueprint)
	{
		return FCortexCommandRouter::Error(CortexErrorCodes::BlueprintNotFound, LoadError);
	}

	const FString DestPackagePath = FPackageName::ObjectPathToPackageName(NormalizedDestPath);
	const FString DestAssetName = FPackageName::GetShortName(DestPackagePath);
	const FString DestFolderPath = FPackageName::GetLongPackagePath(DestPackagePath);

	if (DestAssetName.IsEmpty() || DestFolderPath.IsEmpty())
	{
		return FCortexCommandRouter::Error(
			CortexErrorCodes::InvalidField,
			FString::Printf(TEXT("Invalid destination path: %s"), *NormalizedDestPath)
		);
	}

	if (FindPackage(nullptr, *DestPackagePath) || FPackageName::DoesPackageExist(DestPackagePath))
	{
		return FCortexCommandRouter::Error(
			CortexErrorCodes::BlueprintAlreadyExists,
			FString::Printf(TEXT("Blueprint already exists at destination: %s"), *NormalizedDestPath)
		);
	}

	// In MCP context, IAssetTools::RenameAssets shows a blocking dialog when level packages
	// (placed instances) reference the Blueprint. Filter to only world/map package referencers.
	IAssetRegistry& AssetRegistry =
		FModuleManager::LoadModuleChecked<FAssetRegistryModule>("AssetRegistry").Get();
	TArray<FAssetIdentifier> Referencers;
	AssetRegistry.GetReferencers(FAssetIdentifier(SourceBlueprint->GetOutermost()->GetFName()), Referencers);

	TArray<FString> LevelReferencers;
	for (const FAssetIdentifier& Ref : Referencers)
	{
		TArray<FAssetData> RefAssets;
		FARFilter RefFilter;
		RefFilter.PackageNames.Add(Ref.PackageName);
		AssetRegistry.GetAssets(RefFilter, RefAssets);
		for (const FAssetData& RefAsset : RefAssets)
		{
			if (RefAsset.AssetClassPath == UWorld::StaticClass()->GetClassPathName())
			{
				LevelReferencers.Add(Ref.PackageName.ToString());
			}
		}
	}

	if (LevelReferencers.Num() > 0)
	{
		TArray<TSharedPtr<FJsonValue>> RefArray;
		RefArray.Reserve(LevelReferencers.Num());
		for (const FString& Ref : LevelReferencers)
		{
			RefArray.Add(MakeShared<FJsonValueString>(Ref));
		}
		TSharedPtr<FJsonObject> Details = MakeShared<FJsonObject>();
		Details->SetArrayField(TEXT("level_references"), RefArray);
		return FCortexCommandRouter::Error(
			CortexErrorCodes::HasReferences,
			FString::Printf(TEXT("Rename blocked: Blueprint is placed in %d level(s). Fix up level placements before renaming."), LevelReferencers.Num()),
			Details
		);
	}

	FAssetToolsModule& AssetToolsModule =
		FModuleManager::LoadModuleChecked<FAssetToolsModule>("AssetTools");
	TArray<FAssetRenameData> RenameAssets;
	RenameAssets.Emplace(SourceBlueprint, DestFolderPath, DestAssetName);

	FScopedTransaction Transaction(FText::FromString(
		FString::Printf(TEXT("Cortex: Rename Blueprint %s"), *SourceBlueprint->GetName())
	));
	// Suppress editor modal rename dialogs while running MCP operations unattended.
	TGuardValue<bool> UnattendedScriptGuard(GIsRunningUnattendedScript, true);
	const bool bRenamed = AssetToolsModule.Get().RenameAssets(RenameAssets);
	if (!bRenamed)
	{
		return FCortexCommandRouter::Error(
			CortexErrorCodes::SerializationError,
			FString::Printf(TEXT("Failed to rename Blueprint from %s to %s"), *SourcePath, *NormalizedDestPath)
		);
	}

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("old_path"), SourcePath);
	Data->SetStringField(TEXT("new_path"), NormalizedDestPath);
	Data->SetBoolField(TEXT("redirector_created"), true);

	return FCortexCommandRouter::Success(Data);
}

FCortexCommandResult FCortexBPAssetOps::Reparent(const TSharedPtr<FJsonObject>& Params)
{
	if (!Params.IsValid())
	{
		return FCortexCommandRouter::Error(
			CortexErrorCodes::InvalidField,
			TEXT("Missing params object")
		);
	}

	FString AssetPath;
	if (!Params->TryGetStringField(TEXT("asset_path"), AssetPath) || AssetPath.IsEmpty())
	{
		return FCortexCommandRouter::Error(
			CortexErrorCodes::InvalidField,
			TEXT("Missing required param: asset_path")
		);
	}

	FString NewParent;
	if (!Params->TryGetStringField(TEXT("new_parent"), NewParent) || NewParent.IsEmpty())
	{
		return FCortexCommandRouter::Error(
			CortexErrorCodes::InvalidField,
			TEXT("Missing required param: new_parent")
		);
	}

	// Load the Blueprint to reparent
	FString LoadError;
	UBlueprint* Blueprint = LoadBlueprint(AssetPath, LoadError);
	if (!Blueprint)
	{
		return FCortexCommandRouter::Error(CortexErrorCodes::BlueprintNotFound, LoadError);
	}

	// Resolve the new parent class
	UClass* NewParentClass = nullptr;

	// First try as a Blueprint asset path
	FString ParentLoadError;
	UBlueprint* ParentBP = LoadBlueprint(NewParent, ParentLoadError);
	if (ParentBP && ParentBP->GeneratedClass)
	{
		NewParentClass = ParentBP->GeneratedClass;
	}
	else
	{
		// Try as a C++ class name (full path or short name)
		NewParentClass = FindObject<UClass>(nullptr, *NewParent);
		if (!NewParentClass)
		{
			NewParentClass = FindFirstObject<UClass>(*NewParent, EFindFirstObjectOptions::NativeFirst);
		}
	}

	if (!NewParentClass)
	{
		return FCortexCommandRouter::Error(
			CortexErrorCodes::InvalidParentClass,
			FString::Printf(TEXT("Could not resolve new parent: %s (tried as Blueprint path and C++ class name)"), *NewParent)
		);
	}

	// Check that the new parent is different
	FString OldParentName = Blueprint->ParentClass ? Blueprint->ParentClass->GetName() : TEXT("None");
	if (Blueprint->ParentClass == NewParentClass)
	{
		return FCortexCommandRouter::Error(
			CortexErrorCodes::InvalidField,
			FString::Printf(TEXT("Blueprint already has parent class: %s"), *NewParentClass->GetName())
		);
	}

	FScopedTransaction Transaction(FText::FromString(
		FString::Printf(TEXT("Cortex: Reparent Blueprint %s to %s"), *Blueprint->GetName(), *NewParentClass->GetName())
	));

	// Perform the reparent — direct ParentClass assignment is the correct approach,
	// matching what the Blueprint editor's own reparent dialog uses internally.
	Blueprint->ParentClass = NewParentClass;
	FBlueprintEditorUtils::RefreshAllNodes(Blueprint);
	FBlueprintEditorUtils::MarkBlueprintAsModified(Blueprint);

	// Compile the reparented Blueprint
	FKismetEditorUtilities::CompileBlueprint(Blueprint);

	// Mark dirty for save
	Blueprint->GetOutermost()->MarkPackageDirty();

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("asset_path"), AssetPath);
	Data->SetStringField(TEXT("old_parent"), OldParentName);
	Data->SetStringField(TEXT("new_parent"), NewParentClass->GetName());
	Data->SetBoolField(TEXT("reparented"), true);

	UE_LOG(LogCortexBlueprint, Log, TEXT("Reparented Blueprint %s: %s -> %s"),
		*AssetPath, *OldParentName, *NewParentClass->GetName());

	return FCortexCommandRouter::Success(Data);
}
