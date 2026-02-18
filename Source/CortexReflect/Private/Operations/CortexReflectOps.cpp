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

	// C++ class: use ModuleRelativePath metadata.
	// Engine source paths start with Runtime/, Editor/, Developer/, Programs/, or Plugins/.
	// Project source paths are relative to the game module (e.g., "Characters/MyCharacter.h").
	FString ModulePath = Class->GetMetaData(TEXT("ModuleRelativePath"));
	if (!ModulePath.IsEmpty())
	{
		return !ModulePath.StartsWith(TEXT("Runtime/"))
			&& !ModulePath.StartsWith(TEXT("Editor/"))
			&& !ModulePath.StartsWith(TEXT("Developer/"))
			&& !ModulePath.StartsWith(TEXT("Programs/"))
			&& !ModulePath.StartsWith(TEXT("Plugins/"));
	}

	return false;
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
	int32& OutBPCount)
{
	OutNode = MakeShared<FJsonObject>();
	OutNode->SetStringField(TEXT("name"), GetCppClassName(Root));

	UBlueprintGeneratedClass* BPGC = Cast<UBlueprintGeneratedClass>(Root);
	if (BPGC)
	{
		OutNode->SetStringField(TEXT("type"), TEXT("blueprint"));
		OutBPCount++;
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
				OutTotalCount, OutCppCount, OutBPCount
			);
			ChildrenArray.Add(MakeShared<FJsonValueObject>(ChildNode));
		}
	}
	OutNode->SetArrayField(TEXT("children"), ChildrenArray);
}

FCortexCommandResult FCortexReflectOps::ClassHierarchy(const TSharedPtr<FJsonObject>& Params)
{
	FString RootName;
	if (!Params.IsValid() || !Params->TryGetStringField(TEXT("root"), RootName))
	{
		return FCortexCommandRouter::Error(
			CortexErrorCodes::InvalidField,
			TEXT("root parameter is required")
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

	int32 TotalCount = 0, CppCount = 0, BPCount = 0;
	TSharedPtr<FJsonObject> TreeNode;
	BuildHierarchyTree(
		RootClass, TreeNode,
		0, Depth,
		bIncludeBlueprint, bIncludeEngine,
		MaxResults,
		TotalCount, CppCount, BPCount
	);

	TreeNode->SetStringField(TEXT("root"), GetCppClassName(RootClass));
	TreeNode->SetNumberField(TEXT("total_classes"), TotalCount);
	TreeNode->SetNumberField(TEXT("cpp_count"), CppCount);
	TreeNode->SetNumberField(TEXT("blueprint_count"), BPCount);

	return FCortexCommandRouter::Success(TreeNode);
}

FCortexCommandResult FCortexReflectOps::ClassDetail(const TSharedPtr<FJsonObject>& Params)
{
	FString ClassName;
	if (!Params.IsValid() || !Params->TryGetStringField(TEXT("class_name"), ClassName))
	{
		return FCortexCommandRouter::Error(
			CortexErrorCodes::InvalidField,
			TEXT("class_name parameter is required")
		);
	}

	bool bIncludeInherited = false;
	if (Params.IsValid())
	{
		Params->TryGetBoolField(TEXT("include_inherited"), bIncludeInherited);
	}

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
				CompObj->SetStringField(TEXT("type"), Comp->GetClass()->GetName());
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
					CompObj->SetStringField(TEXT("type"), Node->ComponentClass->GetName());
					ComponentsArray.Add(MakeShared<FJsonValueObject>(CompObj));
				}
			}
		}
	}
	Result->SetArrayField(TEXT("components"), ComponentsArray);

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
	return FCortexCommandRouter::Error(CortexErrorCodes::UnknownCommand, TEXT("Not implemented"));
}

FCortexCommandResult FCortexReflectOps::FindUsages(const TSharedPtr<FJsonObject>& Params)
{
	return FCortexCommandRouter::Error(CortexErrorCodes::UnknownCommand, TEXT("Not implemented"));
}

FCortexCommandResult FCortexReflectOps::Search(const TSharedPtr<FJsonObject>& Params)
{
	FString Pattern;
	if (!Params.IsValid() || !Params->TryGetStringField(TEXT("pattern"), Pattern))
	{
		return FCortexCommandRouter::Error(
			CortexErrorCodes::InvalidField,
			TEXT("pattern parameter is required")
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
			if (!ModName || !ModName->Contains(ModuleFilter))
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
