#include "Operations/CortexReflectOps.h"
#include "CortexReflectModule.h"
#include "UObject/UObjectIterator.h"
#include "Engine/Blueprint.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "Engine/SimpleConstructionScript.h"
#include "Engine/SCS_Node.h"
#include "GameFramework/Actor.h"
#include "Components/ActorComponent.h"
#include "Misc/PackageName.h"
#include "K2Node_Variable.h"
#include "K2Node_VariableGet.h"
#include "K2Node_VariableSet.h"
#include "K2Node_CallFunction.h"
#include "K2Node_DynamicCast.h"
#include "K2Node_SpawnActorFromClass.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "Modules/ModuleManager.h"
#include "Interfaces/IPluginManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Misc/App.h"
#include "Serialization/JsonSerializer.h"

namespace
{
TMap<FString, ECortexModuleOrigin> BuildPluginModuleOrigins()
{
	TMap<FString, ECortexModuleOrigin> ModuleOrigins;

	const FString ProjectDir = FPaths::ConvertRelativePathToFull(FPaths::ProjectDir());

	for (const TSharedRef<IPlugin>& Plugin : IPluginManager::Get().GetDiscoveredPlugins())
	{
		ECortexModuleOrigin Origin = ECortexModuleOrigin::Engine;
		if (Plugin->GetLoadedFrom() == EPluginLoadedFrom::Project)
		{
			Origin = ECortexModuleOrigin::Project;
		}
		else
		{
			FString PluginBaseDir = FPaths::ConvertRelativePathToFull(Plugin->GetBaseDir());
			FPaths::NormalizeFilename(PluginBaseDir);
			FString NormalizedProjectDir = ProjectDir;
			FPaths::NormalizeFilename(NormalizedProjectDir);

			if (PluginBaseDir.StartsWith(NormalizedProjectDir))
			{
				Origin = ECortexModuleOrigin::Project;
			}
		}

		for (const FModuleDescriptor& Module : Plugin->GetDescriptor().Modules)
		{
			ModuleOrigins.FindOrAdd(Module.Name.ToString()) = Origin;
		}
	}

	return ModuleOrigins;
}

const TMap<FString, ECortexModuleOrigin>& GetPluginModuleOrigins()
{
	static const TMap<FString, ECortexModuleOrigin> ModuleOrigins = BuildPluginModuleOrigins();
	return ModuleOrigins;
}
}

// Returns the full C++ name of a class (e.g. "AActor" not "Actor").
// UClass::GetName() strips the A/U prefix; GetPrefixCPP() gives it back.
FString FCortexReflectOps::GetCppClassName(const UClass* Class)
{
	if (!Class)
	{
		return FString();
	}
	return FString(Class->GetPrefixCPP()) + Class->GetName();
}

UClass* FCortexReflectOps::FindClassByName(const FString& ClassName, FCortexCommandResult& OutError)
{
	if (ClassName.IsEmpty())
	{
		OutError = FCortexCommandRouter::Error(
			CortexErrorCodes::InvalidField,
			TEXT("class_name is required")
		);
		return nullptr;
	}

	// 1. Try asset path (starts with /)
	if (ClassName.StartsWith(TEXT("/")))
	{
		FString PkgName = FPackageName::ObjectPathToPackageName(ClassName);
		if (!FindPackage(nullptr, *PkgName) && !FPackageName::DoesPackageExist(PkgName))
		{
			OutError = FCortexCommandRouter::Error(
				CortexErrorCodes::ClassNotFound,
				FString::Printf(TEXT("Blueprint asset not found: %s"), *ClassName)
			);
			return nullptr;
		}

		// Build full object path: /Game/Blueprints/BP_Foo -> /Game/Blueprints/BP_Foo.BP_Foo
		FString ObjectPath = ClassName;
		if (!ObjectPath.Contains(TEXT(".")))
		{
			FString AssetName = FPackageName::GetShortName(ObjectPath);
			ObjectPath = ObjectPath + TEXT(".") + AssetName;
		}

		UBlueprint* BP = LoadObject<UBlueprint>(nullptr, *ObjectPath);
		if (BP && BP->GeneratedClass)
		{
			return BP->GeneratedClass;
		}

		OutError = FCortexCommandRouter::Error(
			CortexErrorCodes::ClassNotFound,
			FString::Printf(TEXT("Blueprint class not found: %s"), *ClassName)
		);
		return nullptr;
	}

	// 2. Resolve by name using TObjectIterator<UClass>.
	//
	//    UClass::GetName() returns the stripped name (e.g. "Actor" for AActor,
	//    "Character" for ACharacter, "Object" for UObject).
	//    GetPrefixCPP() returns the single-char prefix ("A" or "U").
	//
	//    We build a list of stripped names to try, in priority order:
	//      - If input starts with "A" or "U" (C++ prefix), strip it and try the stripped name first.
	//      - Always fall back to the input as-is (for classes that use the prefix in their actual name).
	//    NOTE: F-prefixed types are UStruct, never UClass — intentionally excluded.

	TArray<FString> StrippedCandidates;  // short names to compare against GetName()

	static const TCHAR* KnownPrefixes[] = { TEXT("A"), TEXT("U") };
	for (const TCHAR* Prefix : KnownPrefixes)
	{
		if (ClassName.StartsWith(Prefix) && ClassName.Len() > 1)
		{
			FString Stripped = ClassName.Mid(1);
			StrippedCandidates.Add(Stripped);
			break;
		}
	}

	// The bare name is always a candidate (either the already-stripped form or the original)
	StrippedCandidates.Add(ClassName);

	// Single pass through all registered UClasses
	TMap<FString, UClass*> Found;
	for (TObjectIterator<UClass> It; It; ++It)
	{
		UClass* Candidate = *It;
		if (!IsValid(Candidate))
		{
			continue;
		}

		const FString& ShortName = Candidate->GetName();
		if (StrippedCandidates.Contains(ShortName) && !Found.Contains(ShortName))
		{
			Found.Add(ShortName, Candidate);
			if (Found.Num() == StrippedCandidates.Num())
			{
				break;
			}
		}
	}

	// Return first match in priority order
	for (const FString& Name : StrippedCandidates)
	{
		if (UClass** Result = Found.Find(Name))
		{
			return *Result;
		}
	}

	OutError = FCortexCommandRouter::Error(
		CortexErrorCodes::ClassNotFound,
		FString::Printf(TEXT("Class not found: %s"), *ClassName)
	);
	return nullptr;
}

bool FCortexReflectOps::IsProjectClass(const UClass* Class)
{
	if (!Class)
	{
		return false;
	}

	// Blueprint in /Game/ is a project class
	if (const UBlueprintGeneratedClass* BPGC = Cast<UBlueprintGeneratedClass>(Class))
	{
		UBlueprint* BP = Cast<UBlueprint>(BPGC->ClassGeneratedBy);
		return BP && BP->GetPathName().StartsWith(TEXT("/Game/"));
	}

	// C++ class: resolve the owning module binary first. This distinguishes
	// engine modules from project modules and project-local plugins reliably.
	const UPackage* ClassPackage = Class->GetOuterUPackage();
	FString ModuleName;
	if (ClassPackage)
	{
		ModuleName = ClassPackage->GetName();
		static const FString ScriptPrefix(TEXT("/Script/"));
		if (ModuleName.StartsWith(ScriptPrefix))
		{
			ModuleName.RightChopInline(ScriptPrefix.Len(), EAllowShrinking::No);
		}
#if !IS_MONOLITHIC
		FString ModuleFilename;
		if (FModuleManager::Get().ModuleExists(*ModuleName, &ModuleFilename))
		{
			if (!ModuleFilename.IsEmpty())
			{
				FPaths::NormalizeFilename(ModuleFilename);

				FString ProjectDir = FPaths::ConvertRelativePathToFull(FPaths::ProjectDir());
				FPaths::NormalizeFilename(ProjectDir);
				if (ModuleFilename.StartsWith(ProjectDir))
				{
					return true;
				}

				FString EngineDir = FPaths::ConvertRelativePathToFull(FPaths::EngineDir());
				FPaths::NormalizeFilename(EngineDir);
				if (ModuleFilename.StartsWith(EngineDir))
				{
					return false;
				}
			}
		}
#endif

		FModuleStatus ModuleStatus;
		if (FModuleManager::Get().QueryModule(*ModuleName, ModuleStatus) && ModuleStatus.bIsGameModule)
		{
			return true;
		}
	}

	// Fallback to metadata for modules that do not resolve to a binary path.
	FString ModulePath = Class->GetMetaData(TEXT("ModuleRelativePath"));
	if (!ModulePath.IsEmpty())
	{
		const ECortexModuleOrigin ModuleOrigin = ClassifyNativeModuleFromMetadata(
			ModuleName,
			ModulePath,
			GetPluginModuleOrigins()
		);

		if (ModuleOrigin == ECortexModuleOrigin::Project)
		{
			return true;
		}

		if (ModuleOrigin == ECortexModuleOrigin::Engine)
		{
			return false;
		}

		return false;
	}

	return false;
}

ECortexModuleOrigin FCortexReflectOps::ClassifyNativeModuleFromMetadata(
	const FString& ModuleName,
	const FString& ModuleRelativePath,
	const TMap<FString, ECortexModuleOrigin>& PluginModuleOrigins
)
{
	if (const ECortexModuleOrigin* PluginOrigin = PluginModuleOrigins.Find(ModuleName))
	{
		return *PluginOrigin;
	}

	if (ModuleName.Equals(FApp::GetProjectName(), ESearchCase::IgnoreCase))
	{
		return ECortexModuleOrigin::Project;
	}

	if (ModuleRelativePath.StartsWith(TEXT("Runtime/"))
		|| ModuleRelativePath.StartsWith(TEXT("Editor/"))
		|| ModuleRelativePath.StartsWith(TEXT("Developer/"))
		|| ModuleRelativePath.StartsWith(TEXT("Programs/"))
		|| ModuleRelativePath.StartsWith(TEXT("Plugins/")))
	{
		return ECortexModuleOrigin::Engine;
	}

	return ECortexModuleOrigin::Unknown;
}

TArray<FString> FCortexReflectOps::GetPropertyFlags(const FProperty* Property)
{
	TArray<FString> Flags;
	if (Property->HasAnyPropertyFlags(CPF_BlueprintVisible))
	{
		if (Property->HasAnyPropertyFlags(CPF_BlueprintReadOnly))
		{
			Flags.Add(TEXT("BlueprintReadOnly"));
		}
		else
		{
			Flags.Add(TEXT("BlueprintReadWrite"));
		}
	}
	if (Property->HasAnyPropertyFlags(CPF_Edit))
	{
		if (Property->HasAnyPropertyFlags(CPF_DisableEditOnInstance))
		{
			Flags.Add(TEXT("EditDefaultsOnly"));
		}
		else if (Property->HasAnyPropertyFlags(CPF_DisableEditOnTemplate))
		{
			Flags.Add(TEXT("EditInstanceOnly"));
		}
		else
		{
			Flags.Add(TEXT("EditAnywhere"));
		}
	}
	if (Property->HasAnyPropertyFlags(CPF_Net))
	{
		Flags.Add(TEXT("Replicated"));
	}
	if (Property->HasAnyPropertyFlags(CPF_Config))
	{
		Flags.Add(TEXT("Config"));
	}
	if (Property->HasAnyPropertyFlags(CPF_Transient))
	{
		Flags.Add(TEXT("Transient"));
	}
	return Flags;
}

FString FCortexReflectOps::GetPropertyTypeName(const FProperty* Property)
{
	return Property->GetCPPType();
}

TSharedPtr<FJsonObject> FCortexReflectOps::SerializeProperty(const FProperty* Property, const UObject* CDO)
{
	TSharedPtr<FJsonObject> PropObj = MakeShared<FJsonObject>();
	PropObj->SetStringField(TEXT("name"), Property->GetName());
	PropObj->SetStringField(TEXT("type"), GetPropertyTypeName(Property));

	TArray<FString> Flags = GetPropertyFlags(Property);
	TArray<TSharedPtr<FJsonValue>> FlagsArray;
	for (const FString& Flag : Flags)
	{
		FlagsArray.Add(MakeShared<FJsonValueString>(Flag));
	}
	PropObj->SetArrayField(TEXT("flags"), FlagsArray);

	if (Property->HasMetaData(TEXT("Category")))
	{
		PropObj->SetStringField(TEXT("category"), Property->GetMetaData(TEXT("Category")));
	}

	if (CDO)
	{
		FString DefaultValue;
		const void* ValuePtr = Property->ContainerPtrToValuePtr<void>(CDO);
		if (ValuePtr)
		{
			Property->ExportText_Direct(DefaultValue, ValuePtr, nullptr, nullptr, PPF_None);
			if (!DefaultValue.IsEmpty())
			{
				PropObj->SetStringField(TEXT("default_value"), DefaultValue);
			}
		}
	}

	return PropObj;
}

TArray<FString> FCortexReflectOps::GetFunctionFlags(const UFunction* Function)
{
	TArray<FString> Flags;
	if (Function->HasAnyFunctionFlags(FUNC_BlueprintCallable))
	{
		Flags.Add(TEXT("BlueprintCallable"));
	}
	if (Function->HasAnyFunctionFlags(FUNC_BlueprintEvent))
	{
		Flags.Add(TEXT("BlueprintImplementableEvent"));
	}
	if (Function->HasAnyFunctionFlags(FUNC_Native) && Function->HasAnyFunctionFlags(FUNC_Event))
	{
		Flags.Add(TEXT("BlueprintNativeEvent"));
	}
	if (Function->HasAnyFunctionFlags(FUNC_Const))
	{
		Flags.Add(TEXT("Const"));
	}
	if (Function->HasAnyFunctionFlags(FUNC_Static))
	{
		Flags.Add(TEXT("Static"));
	}
	if (Function->HasAnyFunctionFlags(FUNC_Net))
	{
		Flags.Add(TEXT("Replicated"));
	}
	return Flags;
}

TSharedPtr<FJsonObject> FCortexReflectOps::SerializeFunction(const UFunction* Function, const UClass* QueryClass)
{
	TSharedPtr<FJsonObject> FuncObj = MakeShared<FJsonObject>();
	FuncObj->SetStringField(TEXT("name"), Function->GetName());

	const FProperty* ReturnProp = Function->GetReturnProperty();
	FuncObj->SetStringField(TEXT("return_type"),
		ReturnProp ? GetPropertyTypeName(ReturnProp) : TEXT("void"));

	TArray<TSharedPtr<FJsonValue>> ParamsArray;
	for (TFieldIterator<FProperty> It(Function); It; ++It)
	{
		if (It->HasAnyPropertyFlags(CPF_Parm) && !It->HasAnyPropertyFlags(CPF_ReturnParm))
		{
			TSharedPtr<FJsonObject> ParamObj = MakeShared<FJsonObject>();
			ParamObj->SetStringField(TEXT("name"), It->GetName());
			ParamObj->SetStringField(TEXT("type"), GetPropertyTypeName(*It));
			if (It->HasAnyPropertyFlags(CPF_OutParm))
			{
				ParamObj->SetBoolField(TEXT("is_out"), true);
			}
			ParamsArray.Add(MakeShared<FJsonValueObject>(ParamObj));
		}
	}
	FuncObj->SetArrayField(TEXT("params"), ParamsArray);

	TArray<FString> Flags = GetFunctionFlags(Function);
	TArray<TSharedPtr<FJsonValue>> FlagsArray;
	for (const FString& Flag : Flags)
	{
		FlagsArray.Add(MakeShared<FJsonValueString>(Flag));
	}
	FuncObj->SetArrayField(TEXT("flags"), FlagsArray);

	bool bIsOverride = false;
	if (QueryClass && QueryClass->GetSuperClass())
	{
		bIsOverride = QueryClass->GetSuperClass()->FindFunctionByName(Function->GetFName()) != nullptr;
	}
	FuncObj->SetBoolField(TEXT("is_override"), bIsOverride);

	return FuncObj;
}

void FCortexReflectOps::BuildHierarchyTree(
	UClass* Root,
	TSharedPtr<FJsonObject>& OutNode,
	int32 CurrentDepth,
	int32 MaxDepth,
	bool bIncludeBlueprint,
	bool bIncludeEngine,
	int32 MaxResults,
	int32& OutTotalCount,
	int32& OutCppCount,
	int32& OutBPCount,
	int32& OutProjectCppCount,
	int32& OutProjectBPCount)
{
	OutNode = MakeShared<FJsonObject>();
	OutNode->SetStringField(TEXT("name"), GetCppClassName(Root));

	UBlueprintGeneratedClass* BPGC = Cast<UBlueprintGeneratedClass>(Root);
	if (BPGC)
	{
		OutNode->SetStringField(TEXT("type"), TEXT("blueprint"));
		OutBPCount++;
		if (IsProjectClass(Root))
		{
			OutProjectBPCount++;
		}
		UBlueprint* BP = Cast<UBlueprint>(BPGC->ClassGeneratedBy);
		if (BP)
		{
			OutNode->SetStringField(TEXT("asset_path"), BP->GetPathName());
		}
	}
	else
	{
		OutNode->SetStringField(TEXT("type"), TEXT("cpp"));
		OutCppCount++;
		if (IsProjectClass(Root))
		{
			OutProjectCppCount++;
		}
		const FString* ModName = Root->FindMetaData(TEXT("ModuleName"));
		if (ModName)
		{
			OutNode->SetStringField(TEXT("module"), *ModName);
		}
		FString SourcePath = Root->GetMetaData(TEXT("ModuleRelativePath"));
		if (!SourcePath.IsEmpty())
		{
			OutNode->SetStringField(TEXT("source_path"), SourcePath);
		}
	}
	OutTotalCount++;

	// Build children
	TArray<TSharedPtr<FJsonValue>> ChildrenArray;
	if (CurrentDepth < MaxDepth && OutTotalCount < MaxResults)
	{
		TArray<UClass*> DirectChildren;
		GetDerivedClasses(Root, DirectChildren, false);

		for (UClass* Child : DirectChildren)
		{
			if (OutTotalCount >= MaxResults)
			{
				break;
			}

			if (!bIncludeBlueprint && Cast<UBlueprintGeneratedClass>(Child))
			{
				continue;
			}

			if (!bIncludeEngine && !IsProjectClass(Child))
			{
				continue;
			}

			TSharedPtr<FJsonObject> ChildNode;
			BuildHierarchyTree(
				Child, ChildNode,
				CurrentDepth + 1, MaxDepth,
				bIncludeBlueprint, bIncludeEngine,
				MaxResults,
				OutTotalCount, OutCppCount, OutBPCount,
				OutProjectCppCount, OutProjectBPCount
			);
			ChildrenArray.Add(MakeShared<FJsonValueObject>(ChildNode));
		}
	}
	OutNode->SetArrayField(TEXT("children"), ChildrenArray);
}

FCortexCommandResult FCortexReflectOps::ClassHierarchy(const TSharedPtr<FJsonObject>& Params)
{
	FString RootName;
	if (!Params.IsValid()
		|| (!Params->TryGetStringField(TEXT("root"), RootName)
			&& !Params->TryGetStringField(TEXT("class_name"), RootName)))
	{
		return FCortexCommandRouter::Error(
			CortexErrorCodes::InvalidField,
			TEXT("root parameter is required (alias: class_name)")
		);
	}

	FCortexCommandResult FindError;
	UClass* RootClass = FindClassByName(RootName, FindError);
	if (!RootClass)
	{
		return FindError;
	}

	// Parse parameters with safe defaults from design doc
	int32 Depth = 2;
	Params->TryGetNumberField(TEXT("depth"), Depth);

	int32 MaxResults = 100;
	Params->TryGetNumberField(TEXT("max_results"), MaxResults);

	bool bIncludeBlueprint = true;
	Params->TryGetBoolField(TEXT("include_blueprint"), bIncludeBlueprint);

	bool bIncludeEngine = false;
	Params->TryGetBoolField(TEXT("include_engine"), bIncludeEngine);

	int32 TotalCount = 0;
	int32 CppCount = 0;
	int32 BPCount = 0;
	int32 ProjectCppCount = 0;
	int32 ProjectBPCount = 0;
	TSharedPtr<FJsonObject> TreeNode;
	BuildHierarchyTree(
		RootClass, TreeNode,
		0, Depth,
		bIncludeBlueprint, bIncludeEngine,
		MaxResults,
		TotalCount, CppCount, BPCount,
		ProjectCppCount, ProjectBPCount
	);

	// Keep the root node in the hierarchy for context, but in project-only mode
	// exclude an engine root class from aggregate counts.
	if (!bIncludeEngine && !IsProjectClass(RootClass))
	{
		TotalCount = FMath::Max(0, TotalCount - 1);
		if (Cast<UBlueprintGeneratedClass>(RootClass))
		{
			BPCount = FMath::Max(0, BPCount - 1);
		}
		else
		{
			CppCount = FMath::Max(0, CppCount - 1);
		}
	}

	TreeNode->SetNumberField(TEXT("total_classes"), TotalCount);
	TreeNode->SetNumberField(TEXT("cpp_count"), CppCount);
	TreeNode->SetNumberField(TEXT("blueprint_count"), BPCount);
	TreeNode->SetNumberField(TEXT("project_cpp_count"), ProjectCppCount);
	TreeNode->SetNumberField(TEXT("engine_cpp_count"), FMath::Max(0, CppCount - ProjectCppCount));
	TreeNode->SetNumberField(TEXT("project_blueprint_count"), ProjectBPCount);

	// Build flat classes list for simple access
	TArray<TSharedPtr<FJsonValue>> FlatClassesArray;
	TFunction<void(const TSharedPtr<FJsonObject>&)> FlattenTree = [&](const TSharedPtr<FJsonObject>& Node)
	{
		if (!Node.IsValid()) return;
		FString Name;
		if (Node->TryGetStringField(TEXT("name"), Name))
		{
			FlatClassesArray.Add(MakeShared<FJsonValueString>(Name));
		}
		const TArray<TSharedPtr<FJsonValue>>* Children = nullptr;
		if (Node->TryGetArrayField(TEXT("children"), Children))
		{
			for (const TSharedPtr<FJsonValue>& Child : *Children)
			{
				FlattenTree(Child->AsObject());
			}
		}
	};
	FlattenTree(TreeNode);
	TreeNode->SetArrayField(TEXT("classes"), FlatClassesArray);

	TSharedPtr<FJsonObject> CacheParams = MakeShared<FJsonObject>();
	CacheParams->SetStringField(TEXT("root"), RootName);
	CacheParams->SetNumberField(TEXT("depth"), Depth);
	CacheParams->SetNumberField(TEXT("max_results"), MaxResults);
	CacheParams->SetBoolField(TEXT("include_engine"), bIncludeEngine);
	WriteReflectCache(TreeNode, CacheParams);

	return FCortexCommandRouter::Success(TreeNode);
}

bool FCortexReflectOps::WriteReflectCache(
	const TSharedPtr<FJsonObject>& HierarchyData,
	const TSharedPtr<FJsonObject>& Params)
{
	if (!HierarchyData.IsValid())
	{
		return false;
	}

	const FString CacheDir = FPaths::ProjectSavedDir() / TEXT("Cortex");
	IFileManager::Get().MakeDirectory(*CacheDir, true);

	TSharedPtr<FJsonObject> Envelope = MakeShared<FJsonObject>();
	Envelope->SetStringField(TEXT("timestamp"), FDateTime::UtcNow().ToIso8601());
	Envelope->SetObjectField(TEXT("data"), HierarchyData);

	if (Params.IsValid())
	{
		Envelope->SetObjectField(TEXT("params"), Params);
	}
	else
	{
		TSharedPtr<FJsonObject> DefaultParams = MakeShared<FJsonObject>();
		DefaultParams->SetStringField(TEXT("root"), TEXT("AActor"));
		DefaultParams->SetNumberField(TEXT("depth"), 5);
		DefaultParams->SetNumberField(TEXT("max_results"), 1000);
		DefaultParams->SetBoolField(TEXT("include_engine"), false);
		Envelope->SetObjectField(TEXT("params"), DefaultParams);
	}

	FString JsonString;
	TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> Writer =
		TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&JsonString);
	FJsonSerializer::Serialize(Envelope.ToSharedRef(), Writer);

	const FString CachePath = CacheDir / TEXT("reflect-cache.json");
	const FString TempPath = CachePath + TEXT(".tmp");
	if (!FFileHelper::SaveStringToFile(JsonString, *TempPath))
	{
		UE_LOG(LogCortexReflect, Warning, TEXT("Failed to write reflect cache temp file: %s"), *TempPath);
		return false;
	}

	if (!IFileManager::Get().Move(*CachePath, *TempPath, true, true))
	{
		UE_LOG(LogCortexReflect, Warning, TEXT("Failed to move reflect cache temp file: %s"), *TempPath);
		return false;
	}

	UE_LOG(LogCortexReflect, Log, TEXT("Wrote reflect cache: %s"), *CachePath);
	return true;
}

FCortexCommandResult FCortexReflectOps::ClassDetail(const TSharedPtr<FJsonObject>& Params)
{
	FString ClassName;
	if (!Params.IsValid()
		|| (!Params->TryGetStringField(TEXT("class_name"), ClassName)
			&& !Params->TryGetStringField(TEXT("root"), ClassName)))
	{
		return FCortexCommandRouter::Error(
			CortexErrorCodes::InvalidField,
			TEXT("class_name parameter is required (alias: root)")
		);
	}

	bool bIncludeInherited = false;
	FString DetailLevel = TEXT("full");
	if (Params.IsValid())
	{
		Params->TryGetBoolField(TEXT("include_inherited"), bIncludeInherited);
		Params->TryGetStringField(TEXT("detail"), DetailLevel);
	}
	const bool bSummaryOnly = DetailLevel.Equals(TEXT("summary"), ESearchCase::IgnoreCase);

	FCortexCommandResult FindError;
	UClass* Class = FindClassByName(ClassName, FindError);
	if (!Class)
	{
		return FindError;
	}

	TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("name"), GetCppClassName(Class));

	// Type and asset info
	UBlueprintGeneratedClass* BPGC = Cast<UBlueprintGeneratedClass>(Class);
	if (BPGC)
	{
		Result->SetStringField(TEXT("type"), TEXT("blueprint"));
		UBlueprint* BP = Cast<UBlueprint>(BPGC->ClassGeneratedBy);
		if (BP)
		{
			Result->SetStringField(TEXT("asset_path"), BP->GetPathName());
		}
	}
	else
	{
		Result->SetStringField(TEXT("type"), TEXT("cpp"));
		FString SourcePath = Class->GetMetaData(TEXT("ModuleRelativePath"));
		if (!SourcePath.IsEmpty())
		{
			Result->SetStringField(TEXT("source_path"), SourcePath);
		}
	}

	// Parent
	if (Class->GetSuperClass())
	{
		Result->SetStringField(TEXT("parent"), GetCppClassName(Class->GetSuperClass()));
	}

	// Module
	const FString* ModuleName = Class->FindMetaData(TEXT("ModuleName"));
	if (ModuleName)
	{
		Result->SetStringField(TEXT("module"), *ModuleName);
	}

	// Interfaces
	TArray<TSharedPtr<FJsonValue>> InterfacesArray;
	for (const FImplementedInterface& Interface : Class->Interfaces)
	{
		if (Interface.Class)
		{
			InterfacesArray.Add(MakeShared<FJsonValueString>(GetCppClassName(Interface.Class)));
		}
	}
	Result->SetArrayField(TEXT("interfaces"), InterfacesArray);

	if (!bSummaryOnly)
	{
		// CDO for default values and component discovery
		UObject* CDO = Class->GetDefaultObject(false);

		// Properties
		TArray<TSharedPtr<FJsonValue>> PropertiesArray;
		for (TFieldIterator<FProperty> It(Class); It; ++It)
		{
			if (It->HasAnyPropertyFlags(CPF_Parm))
			{
				continue;
			}
			if (!bIncludeInherited && It->GetOwnerClass() != Class)
			{
				continue;
			}
			PropertiesArray.Add(MakeShared<FJsonValueObject>(SerializeProperty(*It, CDO)));
		}
		Result->SetArrayField(TEXT("properties"), PropertiesArray);

		// Functions
		TArray<TSharedPtr<FJsonValue>> FunctionsArray;
		for (TFieldIterator<UFunction> It(Class); It; ++It)
		{
			if (It->HasAnyFunctionFlags(FUNC_Delegate))
			{
				continue;
			}
			if (!bIncludeInherited && It->GetOwnerClass() != Class)
			{
				continue;
			}
			FunctionsArray.Add(MakeShared<FJsonValueObject>(SerializeFunction(*It, Class)));
		}
		Result->SetArrayField(TEXT("functions"), FunctionsArray);

		// Components (from CDO default subobjects)
		TArray<TSharedPtr<FJsonValue>> ComponentsArray;
		if (CDO)
		{
			TArray<UObject*> DefaultSubobjects;
			CDO->GetDefaultSubobjects(DefaultSubobjects);
			for (UObject* Subobj : DefaultSubobjects)
			{
				if (UActorComponent* Comp = Cast<UActorComponent>(Subobj))
				{
					TSharedPtr<FJsonObject> CompObj = MakeShared<FJsonObject>();
					CompObj->SetStringField(TEXT("name"), Comp->GetName());
					CompObj->SetStringField(TEXT("type"), GetCppClassName(Comp->GetClass()));
					ComponentsArray.Add(MakeShared<FJsonValueObject>(CompObj));
				}
			}
		}
		else
		{
			Result->SetBoolField(TEXT("cdo_unavailable"), true);
		}

		// For BPs, also walk SCS nodes for BP-added components
		if (BPGC)
		{
			UBlueprint* BP = Cast<UBlueprint>(BPGC->ClassGeneratedBy);
			if (BP && BP->SimpleConstructionScript)
			{
				const TArray<USCS_Node*>& AllNodes = BP->SimpleConstructionScript->GetAllNodes();
				for (const USCS_Node* Node : AllNodes)
				{
					if (Node && Node->ComponentClass)
					{
						TSharedPtr<FJsonObject> CompObj = MakeShared<FJsonObject>();
						CompObj->SetStringField(TEXT("name"), Node->GetVariableName().ToString());
						CompObj->SetStringField(TEXT("type"), GetCppClassName(Node->ComponentClass));
						ComponentsArray.Add(MakeShared<FJsonValueObject>(CompObj));
					}
				}
			}
		}
		Result->SetArrayField(TEXT("components"), ComponentsArray);
	}

	// Blueprint children count
	TArray<UClass*> DerivedClasses;
	GetDerivedClasses(Class, DerivedClasses, false);
	int32 BPChildCount = 0;
	for (UClass* Derived : DerivedClasses)
	{
		if (Cast<UBlueprintGeneratedClass>(Derived))
		{
			BPChildCount++;
		}
	}
	Result->SetNumberField(TEXT("blueprint_children_count"), BPChildCount);

	return FCortexCommandRouter::Success(Result);
}

FCortexCommandResult FCortexReflectOps::FindOverrides(const TSharedPtr<FJsonObject>& Params)
{
	FString ClassName;
	if (!Params.IsValid()
		|| (!Params->TryGetStringField(TEXT("class_name"), ClassName)
			&& !Params->TryGetStringField(TEXT("root"), ClassName)))
	{
		return FCortexCommandRouter::Error(
			CortexErrorCodes::InvalidField,
			TEXT("class_name parameter is required (alias: root)")
		);
	}

	FCortexCommandResult FindError;
	UClass* ParentClass = FindClassByName(ClassName, FindError);
	if (!ParentClass)
	{
		return FindError;
	}

	int32 Depth = 2;
	Params->TryGetNumberField(TEXT("depth"), Depth);

	int32 Limit = 20;
	Params->TryGetNumberField(TEXT("limit"), Limit);

	// Collect parent's function names for override detection
	TSet<FName> ParentFunctions;
	for (TFieldIterator<UFunction> It(ParentClass); It; ++It)
	{
		ParentFunctions.Add(It->GetFName());
	}

	TArray<UClass*> AllDerived;
	GetDerivedClasses(ParentClass, AllDerived, true);

	TArray<TSharedPtr<FJsonValue>> ChildrenArray;
	int32 TotalOverrides = 0;
	TMap<FString, int32> OverrideCount;

	for (UClass* DerivedClass : AllDerived)
	{
		if (ChildrenArray.Num() >= Limit)
		{
			break;
		}

		UBlueprintGeneratedClass* BPGC = Cast<UBlueprintGeneratedClass>(DerivedClass);
		if (!BPGC)
		{
			continue;
		}

		// Filter out skeleton classes (SKEL_ prefix) — only compiled Blueprint classes
		if (DerivedClass->GetName().StartsWith(TEXT("SKEL_")))
		{
			continue;
		}

		// Check depth
		int32 ClassDepth = 0;
		UClass* Walker = DerivedClass;
		while (Walker && Walker != ParentClass)
		{
			ClassDepth++;
			Walker = Walker->GetSuperClass();
		}
		if (ClassDepth > Depth)
		{
			continue;
		}

		UBlueprint* BP = Cast<UBlueprint>(BPGC->ClassGeneratedBy);
		if (!BP)
		{
			continue;
		}

		TSharedPtr<FJsonObject> ChildObj = MakeShared<FJsonObject>();
		ChildObj->SetStringField(TEXT("name"), BP->GetName());
		ChildObj->SetStringField(TEXT("type"), TEXT("blueprint"));

		TArray<TSharedPtr<FJsonValue>> OverriddenFuncs;
		TArray<TSharedPtr<FJsonValue>> OverriddenEvents;
		TArray<TSharedPtr<FJsonValue>> CustomFuncs;
		TArray<TSharedPtr<FJsonValue>> CustomVars;

		// Use GetOwnerClass() filter for own-class functions
		for (TFieldIterator<UFunction> It(DerivedClass); It; ++It)
		{
			const UFunction* Func = *It;
			if (Func->GetOwnerClass() != DerivedClass)
			{
				continue;
			}

			if (ParentFunctions.Contains(Func->GetFName()))
			{
				// Find which ancestor declares this function via GetOwnerClass()
				UFunction* DeclaredFunc = ParentClass->FindFunctionByName(Func->GetFName());
				FString DefinedIn = DeclaredFunc
					? DeclaredFunc->GetOwnerClass()->GetName()
					: ParentClass->GetName();

				TSharedPtr<FJsonObject> OverrideEntry = MakeShared<FJsonObject>();
				OverrideEntry->SetStringField(TEXT("name"), Func->GetName());
				OverrideEntry->SetStringField(TEXT("defined_in"), DefinedIn);

				if (Func->HasAnyFunctionFlags(FUNC_BlueprintEvent))
				{
					OverriddenEvents.Add(MakeShared<FJsonValueObject>(OverrideEntry));
				}
				else
				{
					OverriddenFuncs.Add(MakeShared<FJsonValueObject>(OverrideEntry));
				}
				TotalOverrides++;

				int32& Count = OverrideCount.FindOrAdd(Func->GetName());
				Count++;
			}
			else
			{
				CustomFuncs.Add(MakeShared<FJsonValueString>(Func->GetName()));
			}
		}

		// Custom variables (own class only)
		for (TFieldIterator<FProperty> It(DerivedClass); It; ++It)
		{
			if (It->GetOwnerClass() == DerivedClass && !It->HasAnyPropertyFlags(CPF_Parm))
			{
				CustomVars.Add(MakeShared<FJsonValueString>(It->GetName()));
			}
		}

		ChildObj->SetArrayField(TEXT("overridden_functions"), OverriddenFuncs);
		ChildObj->SetArrayField(TEXT("overridden_events"), OverriddenEvents);
		ChildObj->SetArrayField(TEXT("custom_functions"), CustomFuncs);
		ChildObj->SetArrayField(TEXT("custom_variables"), CustomVars);

		ChildrenArray.Add(MakeShared<FJsonValueObject>(ChildObj));
	}

	// Find most overridden function
	FString MostOverridden;
	int32 MaxOverrides = 0;
	for (const auto& Pair : OverrideCount)
	{
		if (Pair.Value > MaxOverrides)
		{
			MaxOverrides = Pair.Value;
			MostOverridden = Pair.Key;
		}
	}

	TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("class_name"), GetCppClassName(ParentClass));
	Result->SetArrayField(TEXT("children"), ChildrenArray);
	Result->SetNumberField(TEXT("total_overrides"), TotalOverrides);
	if (!MostOverridden.IsEmpty())
	{
		Result->SetStringField(TEXT("most_overridden"), MostOverridden);
	}

	return FCortexCommandRouter::Success(Result);
}

FCortexCommandResult FCortexReflectOps::FindUsages(const TSharedPtr<FJsonObject>& Params)
{
	FString SymbolName;
	if (!Params.IsValid() || !Params->TryGetStringField(TEXT("symbol"), SymbolName))
	{
		return FCortexCommandRouter::Error(
			CortexErrorCodes::InvalidField,
			TEXT("symbol parameter is required")
		);
	}

	FString ClassName;
	if (!Params->TryGetStringField(TEXT("class_name"), ClassName))
	{
		return FCortexCommandRouter::Error(
			CortexErrorCodes::InvalidField,
			TEXT("class_name parameter is required")
		);
	}

	FCortexCommandResult FindError;
	UClass* OwnerClass = FindClassByName(ClassName, FindError);
	if (!OwnerClass)
	{
		return FindError;
	}

	// Verify symbol exists on the class
	FName SymbolFName(*SymbolName);
	bool bIsProperty = OwnerClass->FindPropertyByName(SymbolFName) != nullptr;
	bool bIsFunction = OwnerClass->FindFunctionByName(SymbolFName) != nullptr;

	if (!bIsProperty && !bIsFunction)
	{
		return FCortexCommandRouter::Error(
			CortexErrorCodes::SymbolNotFound,
			FString::Printf(TEXT("Symbol '%s' not found on class '%s'"),
				*SymbolName, *OwnerClass->GetName())
		);
	}

	FString Scope = TEXT("all");
	Params->TryGetStringField(TEXT("scope"), Scope);
	if (Scope != TEXT("all") && Scope != TEXT("derived"))
	{
		Scope = TEXT("all");
	}

	bool bDeepScan = false;
	Params->TryGetBoolField(TEXT("deep_scan"), bDeepScan);

	FString PathFilter;
	Params->TryGetStringField(TEXT("path_filter"), PathFilter);

	int32 Limit = 20;
	Params->TryGetNumberField(TEXT("limit"), Limit);

	int32 MaxBlueprints = 50;
	Params->TryGetNumberField(TEXT("max_blueprints"), MaxBlueprints);
	if (MaxBlueprints <= 0)
	{
		MaxBlueprints = 50;
	}

	IAssetRegistry& AssetRegistry =
		FModuleManager::LoadModuleChecked<FAssetRegistryModule>("AssetRegistry").Get();

	TMap<FString, UBlueprint*> LoadedBlueprintsByPath;
	TMap<FString, FString> UnloadedBlueprintObjectPaths;

	auto AddLoadedBlueprint = [&LoadedBlueprintsByPath, &PathFilter](UBlueprint* Blueprint)
	{
		if (!Blueprint)
		{
			return;
		}

		const FString AssetPath = Blueprint->GetPathName();
		if (!PathFilter.IsEmpty() && !AssetPath.StartsWith(PathFilter))
		{
			return;
		}

		LoadedBlueprintsByPath.FindOrAdd(AssetPath, Blueprint);
	};

	auto AddBlueprintAsset = [
		&LoadedBlueprintsByPath,
		&UnloadedBlueprintObjectPaths,
		&PathFilter
	](const FAssetData& Asset)
	{
		if (!Asset.IsValid())
		{
			return;
		}

		if (Asset.AssetClassPath != UBlueprint::StaticClass()->GetClassPathName())
		{
			return;
		}

		const FString PackagePath = Asset.PackageName.ToString();
		if (!PathFilter.IsEmpty() && !PackagePath.StartsWith(PathFilter))
		{
			return;
		}

		const FString ObjectPath = Asset.GetSoftObjectPath().ToString();
		UBlueprint* LoadedBlueprint = FindObject<UBlueprint>(nullptr, *ObjectPath);
		if (LoadedBlueprint)
		{
			LoadedBlueprintsByPath.FindOrAdd(LoadedBlueprint->GetPathName(), LoadedBlueprint);
			return;
		}

		UnloadedBlueprintObjectPaths.FindOrAdd(PackagePath, ObjectPath);
	};

	if (Scope == TEXT("derived"))
	{
		// Existing behavior: derive from owner class then enrich with Asset Registry candidates.
		TArray<UClass*> DerivedClasses;
		GetDerivedClasses(OwnerClass, DerivedClasses, true);
		for (UClass* DerivedClass : DerivedClasses)
		{
			if (UBlueprintGeneratedClass* BlueprintClass = Cast<UBlueprintGeneratedClass>(DerivedClass))
			{
				AddLoadedBlueprint(Cast<UBlueprint>(BlueprintClass->ClassGeneratedBy));
			}
		}

		FARFilter Filter;
		Filter.ClassPaths.Add(UBlueprint::StaticClass()->GetClassPathName());
		Filter.bRecursiveClasses = true;
		if (!PathFilter.IsEmpty())
		{
			Filter.PackagePaths.Add(FName(*PathFilter));
			Filter.bRecursivePaths = true;
		}

		TArray<FAssetData> Assets;
		AssetRegistry.GetAssets(Filter, Assets);
		for (const FAssetData& Asset : Assets)
		{
			const FAssetTagValueRef ParentTag = Asset.TagsAndValues.FindTag("NativeParentClass");
			if (!ParentTag.IsSet())
			{
				continue;
			}

			const FString NativeParentPath = ParentTag.AsString();
			UClass* NativeParent = FindObject<UClass>(nullptr, *NativeParentPath);
			if (NativeParent && NativeParent->IsChildOf(OwnerClass))
			{
				AddBlueprintAsset(Asset);
			}
		}
	}
	else
	{
		// Expanded behavior: scan all Blueprint packages that reference the owner class package.
		const FString OwnerPackagePath = OwnerClass->GetOutermost()->GetName();
		TArray<FAssetDependency> Referencers;
		AssetRegistry.GetReferencers(
			FAssetIdentifier(FName(*OwnerPackagePath)),
			Referencers,
			UE::AssetRegistry::EDependencyCategory::Package
		);

		for (const FAssetDependency& Referencer : Referencers)
		{
			const FString ReferencerPackagePath = Referencer.AssetId.PackageName.ToString();
			if (ReferencerPackagePath.IsEmpty())
			{
				continue;
			}

			TArray<FAssetData> AssetsInPackage;
			AssetRegistry.GetAssetsByPackageName(FName(*ReferencerPackagePath), AssetsInPackage, true);
			for (const FAssetData& PackageAsset : AssetsInPackage)
			{
				AddBlueprintAsset(PackageAsset);
			}
		}

		// Keep loaded child blueprints in candidate set even if referencer tags are incomplete.
		TArray<UClass*> DerivedClasses;
		GetDerivedClasses(OwnerClass, DerivedClasses, true);
		for (UClass* DerivedClass : DerivedClasses)
		{
			if (UBlueprintGeneratedClass* BlueprintClass = Cast<UBlueprintGeneratedClass>(DerivedClass))
			{
				AddLoadedBlueprint(Cast<UBlueprint>(BlueprintClass->ClassGeneratedBy));
			}
		}
	}

	TArray<TSharedPtr<FJsonValue>> UsagesArray;
	int32 TotalUsages = 0;
	int32 BlueprintsScanned = 0;
	TSet<FString> NotScannedPathSet;

	auto AddNotScannedPath = [&NotScannedPathSet](const FString& AssetPath)
	{
		if (!AssetPath.IsEmpty())
		{
			NotScannedPathSet.Add(AssetPath);
		}
	};

	auto ScanBlueprintForUsages = [
		OwnerClass,
		bIsProperty,
		bIsFunction,
		SymbolFName
	](UBlueprint* Blueprint)
	{
		TArray<TSharedPtr<FJsonValue>> ReferenceResults;
		if (!Blueprint)
		{
			return ReferenceResults;
		}

		TArray<UEdGraph*> AllGraphs;
		Blueprint->GetAllGraphs(AllGraphs);

		for (UEdGraph* Graph : AllGraphs)
		{
			if (!Graph)
			{
				continue;
			}

			for (UEdGraphNode* Node : Graph->Nodes)
			{
				if (!Node)
				{
					continue;
				}

				if (bIsProperty)
				{
					UK2Node_Variable* VarNode = Cast<UK2Node_Variable>(Node);
					if (VarNode && VarNode->VariableReference.GetMemberName() == SymbolFName)
					{
						TSharedPtr<FJsonObject> RefObj = MakeShared<FJsonObject>();
						RefObj->SetStringField(TEXT("context"), Graph->GetName());

						if (VarNode->IsA<UK2Node_VariableGet>())
						{
							RefObj->SetStringField(TEXT("type"), TEXT("read"));
							RefObj->SetStringField(TEXT("node_class"), TEXT("UK2Node_VariableGet"));
							ReferenceResults.Add(MakeShared<FJsonValueObject>(RefObj));
						}
						else if (VarNode->IsA<UK2Node_VariableSet>())
						{
							RefObj->SetStringField(TEXT("type"), TEXT("write"));
							RefObj->SetStringField(TEXT("node_class"), TEXT("UK2Node_VariableSet"));
							ReferenceResults.Add(MakeShared<FJsonValueObject>(RefObj));
						}
					}
				}

				if (bIsFunction)
				{
					UK2Node_CallFunction* CallNode = Cast<UK2Node_CallFunction>(Node);
					if (CallNode && CallNode->FunctionReference.GetMemberName() == SymbolFName)
					{
						TSharedPtr<FJsonObject> RefObj = MakeShared<FJsonObject>();
						RefObj->SetStringField(TEXT("context"), Graph->GetName());
						RefObj->SetStringField(TEXT("type"), TEXT("call"));
						RefObj->SetStringField(TEXT("node_class"), TEXT("UK2Node_CallFunction"));
						ReferenceResults.Add(MakeShared<FJsonValueObject>(RefObj));
					}
				}

				UK2Node_DynamicCast* CastNode = Cast<UK2Node_DynamicCast>(Node);
				if (CastNode && CastNode->TargetType && CastNode->TargetType->IsChildOf(OwnerClass))
				{
					TSharedPtr<FJsonObject> RefObj = MakeShared<FJsonObject>();
					RefObj->SetStringField(TEXT("context"), Graph->GetName());
					RefObj->SetStringField(TEXT("type"), TEXT("cast"));
					RefObj->SetStringField(TEXT("node_class"), TEXT("UK2Node_DynamicCast"));
					ReferenceResults.Add(MakeShared<FJsonValueObject>(RefObj));
				}

				UK2Node_SpawnActorFromClass* SpawnNode = Cast<UK2Node_SpawnActorFromClass>(Node);
				if (SpawnNode)
				{
					UEdGraphPin* ClassPin = SpawnNode->GetClassPin();
					if (ClassPin && ClassPin->DefaultObject)
					{
						UClass* SpawnClass = Cast<UClass>(ClassPin->DefaultObject);
						if (SpawnClass && SpawnClass->IsChildOf(OwnerClass))
						{
							TSharedPtr<FJsonObject> RefObj = MakeShared<FJsonObject>();
							RefObj->SetStringField(TEXT("context"), Graph->GetName());
							RefObj->SetStringField(TEXT("type"), TEXT("spawn"));
							RefObj->SetStringField(TEXT("node_class"), TEXT("UK2Node_SpawnActorFromClass"));
							ReferenceResults.Add(MakeShared<FJsonValueObject>(RefObj));
						}
					}
				}
			}
		}

		if (USimpleConstructionScript* SCS = Blueprint->SimpleConstructionScript)
		{
			for (USCS_Node* SCSNode : SCS->GetAllNodes())
			{
				if (!SCSNode || !SCSNode->ComponentClass)
				{
					continue;
				}

				if (SCSNode->ComponentClass->IsChildOf(OwnerClass))
				{
					TSharedPtr<FJsonObject> RefObj = MakeShared<FJsonObject>();
					RefObj->SetStringField(TEXT("context"), TEXT("SimpleConstructionScript"));
					RefObj->SetStringField(TEXT("type"), TEXT("component"));
					RefObj->SetStringField(TEXT("node_class"), TEXT("USCS_Node"));
					ReferenceResults.Add(MakeShared<FJsonValueObject>(RefObj));
				}
			}
		}

		return ReferenceResults;
	};

	TArray<FString> LoadedPaths;
	LoadedBlueprintsByPath.GetKeys(LoadedPaths);
	LoadedPaths.Sort();

	for (const FString& LoadedPath : LoadedPaths)
	{
		if (BlueprintsScanned >= MaxBlueprints)
		{
			AddNotScannedPath(LoadedPath);
			continue;
		}

		UBlueprint* Blueprint = nullptr;
		if (UBlueprint** FoundBlueprint = LoadedBlueprintsByPath.Find(LoadedPath))
		{
			Blueprint = *FoundBlueprint;
		}

		if (!Blueprint)
		{
			AddNotScannedPath(LoadedPath);
			continue;
		}

		TArray<TSharedPtr<FJsonValue>> ReferencesArray = ScanBlueprintForUsages(Blueprint);
		BlueprintsScanned++;

		if (ReferencesArray.Num() > 0 && UsagesArray.Num() < Limit)
		{
			TSharedPtr<FJsonObject> UsageObj = MakeShared<FJsonObject>();
			UsageObj->SetStringField(TEXT("class_name"), Blueprint->GetName());
			UsageObj->SetStringField(TEXT("asset_path"), Blueprint->GetPathName());
			UsageObj->SetArrayField(TEXT("references"), ReferencesArray);
			UsageObj->SetNumberField(TEXT("total_count"), ReferencesArray.Num());
			UsagesArray.Add(MakeShared<FJsonValueObject>(UsageObj));
			TotalUsages += ReferencesArray.Num();
		}
	}

	TArray<FString> UnloadedPackagePaths;
	UnloadedBlueprintObjectPaths.GetKeys(UnloadedPackagePaths);
	UnloadedPackagePaths.Sort();

	for (const FString& PackagePath : UnloadedPackagePaths)
	{
		if (BlueprintsScanned >= MaxBlueprints)
		{
			AddNotScannedPath(PackagePath);
			continue;
		}

		const FString* ObjectPath = UnloadedBlueprintObjectPaths.Find(PackagePath);
		if (!ObjectPath)
		{
			AddNotScannedPath(PackagePath);
			continue;
		}

		if (!bDeepScan)
		{
			AddNotScannedPath(PackagePath);
			continue;
		}

		const FString LoadPackagePath = FPackageName::ObjectPathToPackageName(*ObjectPath);
		if (!FindPackage(nullptr, *LoadPackagePath) && !FPackageName::DoesPackageExist(LoadPackagePath))
		{
			AddNotScannedPath(PackagePath);
			continue;
		}

		UBlueprint* LoadedBlueprint = LoadObject<UBlueprint>(nullptr, *(*ObjectPath));
		if (!LoadedBlueprint)
		{
			AddNotScannedPath(PackagePath);
			continue;
		}

		TArray<TSharedPtr<FJsonValue>> ReferencesArray = ScanBlueprintForUsages(LoadedBlueprint);
		BlueprintsScanned++;

		if (ReferencesArray.Num() > 0 && UsagesArray.Num() < Limit)
		{
			TSharedPtr<FJsonObject> UsageObj = MakeShared<FJsonObject>();
			UsageObj->SetStringField(TEXT("class_name"), LoadedBlueprint->GetName());
			UsageObj->SetStringField(TEXT("asset_path"), LoadedBlueprint->GetPathName());
			UsageObj->SetArrayField(TEXT("references"), ReferencesArray);
			UsageObj->SetNumberField(TEXT("total_count"), ReferencesArray.Num());

			UsagesArray.Add(MakeShared<FJsonValueObject>(UsageObj));
			TotalUsages += ReferencesArray.Num();
		}
	}

	TArray<TSharedPtr<FJsonValue>> NotScannedPaths;
	for (const FString& NotScannedPath : NotScannedPathSet)
	{
		NotScannedPaths.Add(MakeShared<FJsonValueString>(NotScannedPath));
	}

	TSharedPtr<FJsonObject> NotScannedObj = MakeShared<FJsonObject>();
	NotScannedObj->SetNumberField(TEXT("count"), NotScannedPaths.Num());
	NotScannedObj->SetArrayField(TEXT("paths"), NotScannedPaths);

	TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("symbol"), SymbolName);
	Result->SetStringField(TEXT("defined_in"), OwnerClass->GetName());
	FString SymbolType = (bIsProperty && bIsFunction) ? TEXT("both")
		: bIsProperty ? TEXT("property") : TEXT("function");
	Result->SetStringField(TEXT("symbol_type"), SymbolType);
	Result->SetStringField(TEXT("scope"), Scope);
	Result->SetNumberField(TEXT("blueprints_scanned"), BlueprintsScanned);
	Result->SetObjectField(TEXT("not_scanned"), NotScannedObj);
	Result->SetArrayField(TEXT("usages"), UsagesArray);
	Result->SetNumberField(TEXT("total_usages"), TotalUsages);
	Result->SetNumberField(TEXT("total_classes"), UsagesArray.Num());

	return FCortexCommandRouter::Success(Result);
}

FCortexCommandResult FCortexReflectOps::GetDependencies(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath;
	if (!Params.IsValid() || !Params->TryGetStringField(TEXT("asset_path"), AssetPath))
	{
		return FCortexCommandRouter::Error(
			CortexErrorCodes::InvalidField,
			TEXT("asset_path parameter is required")
		);
	}

	AssetPath = FPackageName::ObjectPathToPackageName(AssetPath);

	FString CategoryStr;
	Params->TryGetStringField(TEXT("category"), CategoryStr);

	int32 Limit = 100;
	Params->TryGetNumberField(TEXT("limit"), Limit);
	if (Limit <= 0)
	{
		Limit = 100;
	}

	UE::AssetRegistry::EDependencyCategory Category = UE::AssetRegistry::EDependencyCategory::All;
	if (CategoryStr == TEXT("package"))
	{
		Category = UE::AssetRegistry::EDependencyCategory::Package;
	}

	IAssetRegistry& AssetRegistry =
		FModuleManager::LoadModuleChecked<FAssetRegistryModule>("AssetRegistry").Get();

	TArray<FAssetDependency> Dependencies;
	AssetRegistry.GetDependencies(FAssetIdentifier(FName(*AssetPath)), Dependencies, Category);

	TArray<TSharedPtr<FJsonValue>> DepsArray;
	for (const FAssetDependency& Dependency : Dependencies)
	{
		if (DepsArray.Num() >= Limit)
		{
			break;
		}

		const FString PackageName = Dependency.AssetId.PackageName.ToString();
		if (PackageName.IsEmpty())
		{
			continue;
		}

		TSharedPtr<FJsonObject> DependencyObj = MakeShared<FJsonObject>();
		DependencyObj->SetStringField(TEXT("package"), PackageName);

		const bool bIsCode = PackageName.StartsWith(TEXT("/Script/"));
		DependencyObj->SetBoolField(TEXT("is_code"), bIsCode);

		if (bIsCode)
		{
			const FString ModuleName = FPackageName::GetShortName(FName(*PackageName));
			DependencyObj->SetStringField(TEXT("module"), ModuleName);
		}
		else
		{
			TArray<FAssetData> Assets;
			AssetRegistry.GetAssetsByPackageName(FName(*PackageName), Assets, true);
			if (Assets.Num() > 0)
			{
				DependencyObj->SetStringField(
					TEXT("asset_class"),
					Assets[0].AssetClassPath.GetAssetName().ToString()
				);
			}
		}

		const bool bHard = EnumHasAnyFlags(
			Dependency.Properties,
			UE::AssetRegistry::EDependencyProperty::Hard
		);
		DependencyObj->SetStringField(TEXT("type"), bHard ? TEXT("hard") : TEXT("soft"));

		DepsArray.Add(MakeShared<FJsonValueObject>(DependencyObj));
	}

	TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("asset_path"), AssetPath);
	Result->SetArrayField(TEXT("dependencies"), DepsArray);
	Result->SetNumberField(TEXT("total"), DepsArray.Num());
	Result->SetNumberField(TEXT("total_unfiltered"), Dependencies.Num());

	return FCortexCommandRouter::Success(Result);
}

FCortexCommandResult FCortexReflectOps::GetReferencers(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath;
	if (!Params.IsValid() || !Params->TryGetStringField(TEXT("asset_path"), AssetPath))
	{
		return FCortexCommandRouter::Error(
			CortexErrorCodes::InvalidField,
			TEXT("asset_path parameter is required")
		);
	}

	AssetPath = FPackageName::ObjectPathToPackageName(AssetPath);

	FString CategoryStr;
	Params->TryGetStringField(TEXT("category"), CategoryStr);

	int32 Limit = 100;
	Params->TryGetNumberField(TEXT("limit"), Limit);
	if (Limit <= 0)
	{
		Limit = 100;
	}

	UE::AssetRegistry::EDependencyCategory Category = UE::AssetRegistry::EDependencyCategory::All;
	if (CategoryStr == TEXT("package"))
	{
		Category = UE::AssetRegistry::EDependencyCategory::Package;
	}

	IAssetRegistry& AssetRegistry =
		FModuleManager::LoadModuleChecked<FAssetRegistryModule>("AssetRegistry").Get();

	TArray<FAssetDependency> Referencers;
	AssetRegistry.GetReferencers(FAssetIdentifier(FName(*AssetPath)), Referencers, Category);

	TArray<TSharedPtr<FJsonValue>> ReferencerArray;
	for (const FAssetDependency& Referencer : Referencers)
	{
		if (ReferencerArray.Num() >= Limit)
		{
			break;
		}

		const FString PackageName = Referencer.AssetId.PackageName.ToString();
		if (PackageName.IsEmpty())
		{
			continue;
		}

		TSharedPtr<FJsonObject> ReferencerObj = MakeShared<FJsonObject>();
		ReferencerObj->SetStringField(TEXT("package"), PackageName);

		const bool bIsCode = PackageName.StartsWith(TEXT("/Script/"));
		ReferencerObj->SetBoolField(TEXT("is_code"), bIsCode);

		if (bIsCode)
		{
			const FString ModuleName = FPackageName::GetShortName(FName(*PackageName));
			ReferencerObj->SetStringField(TEXT("module"), ModuleName);
		}
		else
		{
			TArray<FAssetData> Assets;
			AssetRegistry.GetAssetsByPackageName(FName(*PackageName), Assets, true);
			if (Assets.Num() > 0)
			{
				ReferencerObj->SetStringField(
					TEXT("asset_class"),
					Assets[0].AssetClassPath.GetAssetName().ToString()
				);
			}
		}

		const bool bHard = EnumHasAnyFlags(
			Referencer.Properties,
			UE::AssetRegistry::EDependencyProperty::Hard
		);
		ReferencerObj->SetStringField(TEXT("type"), bHard ? TEXT("hard") : TEXT("soft"));

		ReferencerArray.Add(MakeShared<FJsonValueObject>(ReferencerObj));
	}

	TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("asset_path"), AssetPath);
	Result->SetArrayField(TEXT("referencers"), ReferencerArray);
	Result->SetNumberField(TEXT("total"), ReferencerArray.Num());
	Result->SetNumberField(TEXT("total_unfiltered"), Referencers.Num());

	return FCortexCommandRouter::Success(Result);
}

FCortexCommandResult FCortexReflectOps::Search(const TSharedPtr<FJsonObject>& Params)
{
	FString Pattern;
	if (!Params.IsValid()
		|| (!Params->TryGetStringField(TEXT("pattern"), Pattern)
			&& !Params->TryGetStringField(TEXT("query"), Pattern)))
	{
		return FCortexCommandRouter::Error(
			CortexErrorCodes::InvalidField,
			TEXT("pattern parameter is required (alias: query)")
		);
	}

	FString TypeFilter;
	Params->TryGetStringField(TEXT("type_filter"), TypeFilter);

	FString ModuleFilter;
	Params->TryGetStringField(TEXT("module_filter"), ModuleFilter);

	bool bIncludeEngine = false;
	Params->TryGetBoolField(TEXT("include_engine"), bIncludeEngine);

	int32 Limit = 50;
	Params->TryGetNumberField(TEXT("limit"), Limit);

	UClass* TypeFilterClass = nullptr;
	if (!TypeFilter.IsEmpty())
	{
		FCortexCommandResult FindError;
		TypeFilterClass = FindClassByName(TypeFilter, FindError);
		if (!TypeFilterClass)
		{
			return FindError;
		}
	}

	TArray<TSharedPtr<FJsonValue>> ResultsArray;

	for (TObjectIterator<UClass> It; It; ++It)
	{
		UClass* Class = *It;
		if (!Class)
		{
			continue;
		}

		// Case-insensitive pattern match against stripped name (without A/U prefix)
		if (!Class->GetName().Contains(Pattern, ESearchCase::IgnoreCase))
		{
			continue;
		}

		if (!bIncludeEngine && !IsProjectClass(Class))
		{
			continue;
		}

		if (TypeFilterClass && !Class->IsChildOf(TypeFilterClass))
		{
			continue;
		}

		if (!ModuleFilter.IsEmpty())
		{
			const FString* ModName = Class->FindMetaData(TEXT("ModuleName"));
			if (!ModName || !ModName->Contains(ModuleFilter, ESearchCase::IgnoreCase))
			{
				continue;
			}
		}

		TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
		Entry->SetStringField(TEXT("name"), GetCppClassName(Class));

		if (Cast<UBlueprintGeneratedClass>(Class))
		{
			Entry->SetStringField(TEXT("type"), TEXT("blueprint"));
			UBlueprint* BP = Cast<UBlueprint>(
				Cast<UBlueprintGeneratedClass>(Class)->ClassGeneratedBy);
			if (BP)
			{
				Entry->SetStringField(TEXT("asset_path"), BP->GetPathName());
			}
		}
		else
		{
			Entry->SetStringField(TEXT("type"), TEXT("cpp"));
			const FString* ModName = Class->FindMetaData(TEXT("ModuleName"));
			if (ModName)
			{
				Entry->SetStringField(TEXT("module"), *ModName);
			}
		}

		if (Class->GetSuperClass())
		{
			Entry->SetStringField(TEXT("parent"), GetCppClassName(Class->GetSuperClass()));
		}

		// BP children count (GetDerivedClasses per result — acceptable at limit=50)
		TArray<UClass*> Derived;
		GetDerivedClasses(Class, Derived, false);
		int32 BPChildCount = 0;
		for (UClass* D : Derived)
		{
			if (Cast<UBlueprintGeneratedClass>(D))
			{
				BPChildCount++;
			}
		}
		Entry->SetNumberField(TEXT("blueprint_children_count"), BPChildCount);

		ResultsArray.Add(MakeShared<FJsonValueObject>(Entry));

		if (ResultsArray.Num() >= Limit)
		{
			break;
		}
	}

	TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetArrayField(TEXT("results"), ResultsArray);
	Result->SetNumberField(TEXT("total_results"), ResultsArray.Num());

	return FCortexCommandRouter::Success(Result);
}
