#include "Operations/CortexDataTypeReplacementOps.h"

#include "AssetRegistry/AssetData.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "CortexSafeFileContract.h"
#include "CortexTypes.h"
#include "Dom/JsonValue.h"
#include "EdGraphSchema_K2.h"
#include "Engine/Blueprint.h"
#include "Engine/DataAsset.h"
#include "Engine/DataTable.h"
#include "Engine/UserDefinedEnum.h"
#include "Engine/World.h"
#include "Kismet2/StructureEditorUtils.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "Operations/CortexDataExportOps.h"
#include "StructUtils/UserDefinedStruct.h"
#include "UObject/CoreRedirects.h"
#include "UObject/UObjectGlobals.h"
#include "UserDefinedStructure/UserDefinedStructEditorData.h"

namespace
{
	const TCHAR* const ScriptStructClass = TEXT("/Script/CoreUObject.ScriptStruct");
	const TCHAR* const EnumClass = TEXT("/Script/CoreUObject.Enum");

	struct FProblem
	{
		FString Code;
		FString Subject;
		FString Message;
	};

	TSharedPtr<FJsonObject> ProblemDetails(const TArray<FProblem>& Problems)
	{
		TArray<TSharedPtr<FJsonValue>> Values;
		for (const FProblem& Problem : Problems)
		{
			TSharedRef<FJsonObject> Json = MakeShared<FJsonObject>();
			Json->SetStringField(TEXT("code"), Problem.Code);
			Json->SetStringField(TEXT("subject"), Problem.Subject);
			Json->SetStringField(TEXT("message"), Problem.Message);
			Values.Add(MakeShared<FJsonValueObject>(Json));
		}
		TSharedPtr<FJsonObject> Details = MakeShared<FJsonObject>();
		Details->SetNumberField(TEXT("problem_count"), Problems.Num());
		Details->SetArrayField(TEXT("problems"), Values);
		return Details;
	}

	FCortexCommandResult InvalidField(const FString& Message)
	{
		return FCortexCommandRouter::Error(CortexErrorCodes::InvalidField, Message);
	}

	bool ReadStringMap(const TSharedPtr<FJsonObject>& Params, const TCHAR* Field, TMap<FString, FString>& Out, FString& OutError)
	{
		const TSharedPtr<FJsonObject>* Object = nullptr;
		if (!Params->HasField(Field))
		{
			return true;
		}
		if (!Params->TryGetObjectField(Field, Object) || Object == nullptr)
		{
			OutError = FString::Printf(TEXT("%s must be an object of strings"), Field);
			return false;
		}
		for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : (*Object)->Values)
		{
			FString Value;
			if (!Pair.Value.IsValid() || !Pair.Value->TryGetString(Value) || Value.IsEmpty())
			{
				OutError = FString::Printf(TEXT("%s.%s must be a non-empty string"), Field, *Pair.Key);
				return false;
			}
			Out.Add(Pair.Key, Value);
		}
		return true;
	}

	bool SameObject(const TWeakObjectPtr<UObject>& A, const TWeakObjectPtr<UObject>& B)
	{
		return A.Get() == B.Get();
	}

	bool SamePinType(const FEdGraphPinType& A, const FEdGraphPinType& B)
	{
		return A.PinCategory == B.PinCategory
			&& A.PinSubCategory == B.PinSubCategory
			&& SameObject(A.PinSubCategoryObject, B.PinSubCategoryObject)
			&& A.ContainerType == B.ContainerType
			&& A.PinValueType.TerminalCategory == B.PinValueType.TerminalCategory
			&& A.PinValueType.TerminalSubCategory == B.PinValueType.TerminalSubCategory
			&& SameObject(A.PinValueType.TerminalSubCategoryObject, B.PinValueType.TerminalSubCategoryObject);
	}

	/**
	 * A member (or a container's element) typed with another user-defined struct or enum is only convertible when that type is already
	 * redirected to the native type the native property uses; otherwise the inner type has to be replaced first (leaf types first).
	 * The slot is replaced by the native object either way so the remaining comparison sees only genuine type differences.
	 */
	void ReconcileUserTypeSlot(TWeakObjectPtr<UObject>& UserSlot, const TWeakObjectPtr<UObject>& NativeSlot, TArray<FString>& OutNested)
	{
		UObject* UserObject = UserSlot.Get();
		UObject* NativeObject = NativeSlot.Get();
		if (UserObject == nullptr || NativeObject == nullptr || UserObject == NativeObject
			|| !(UserObject->IsA<UUserDefinedStruct>() || UserObject->IsA<UUserDefinedEnum>()))
		{
			return;
		}
		const ECoreRedirectFlags Flags = UserObject->IsA<UUserDefinedEnum>() ? ECoreRedirectFlags::Type_Enum : ECoreRedirectFlags::Type_Object;
		const FCoreRedirectObjectName Old(UserObject);
		const FCoreRedirectObjectName New = FCoreRedirects::GetRedirectedName(Flags, Old);
		if (New == Old || New.ToString() != NativeObject->GetPathName())
		{
			OutNested.Add(UserObject->GetPathName());
		}
		UserSlot = NativeObject;
	}

	FString PinTypeLabel(const FEdGraphPinType& Type)
	{
		FString Label = Type.PinCategory.ToString();
		if (!Type.PinSubCategory.IsNone())
		{
			Label += TEXT("/") + Type.PinSubCategory.ToString();
		}
		if (const UObject* Object = Type.PinSubCategoryObject.Get())
		{
			Label += TEXT("(") + Object->GetPathName() + TEXT(")");
		}
		if (Type.ContainerType != EPinContainerType::None)
		{
			Label += FString::Printf(TEXT(" container=%d"), static_cast<int32>(Type.ContainerType));
		}
		return Label;
	}

	void MatchStructMembers(
		const UUserDefinedStruct* User,
		const UScriptStruct* Native,
		const TMap<FString, FString>& Explicit,
		FCortexDataTypeReplacementPlan& Plan,
		TArray<FProblem>& Problems)
	{
		if (User->Status != UDSS_UpToDate)
		{
			Problems.Add({ TEXT("user_type_not_compiled"), User->GetPathName(), TEXT("The user-defined struct is not compiled (status is not UpToDate); fix its errors first") });
			return;
		}

		const UEdGraphSchema_K2* Schema = GetDefault<UEdGraphSchema_K2>();
		TSet<FString> UsedEntries;
		TMap<const FProperty*, FString> Claimed;
		for (const FStructVariableDescription& Variable : FStructureEditorUtils::GetVarDesc(User))
		{
			const FString UserName = Variable.VarName.ToString();
			FString NativeName = Variable.FriendlyName;
			if (const FString* ByFriendly = Explicit.Find(Variable.FriendlyName))
			{
				NativeName = *ByFriendly;
				UsedEntries.Add(Variable.FriendlyName);
			}
			else if (const FString* ByUser = Explicit.Find(UserName))
			{
				NativeName = *ByUser;
				UsedEntries.Add(UserName);
			}

			const FProperty* NativeProperty = Native->FindPropertyByName(FName(*NativeName));
			const FProperty* UserProperty = User->FindPropertyByName(Variable.VarName);
			if (NativeProperty == nullptr || UserProperty == nullptr)
			{
				Problems.Add({ TEXT("unmatched_member"), Variable.FriendlyName,
					FString::Printf(TEXT("Member '%s' has no native property '%s' on %s; pass member_map {\"%s\":\"<native property>\"}"),
						*Variable.FriendlyName, *NativeName, *Native->GetPathName(), *Variable.FriendlyName) });
				continue;
			}

			FEdGraphPinType UserType;
			FEdGraphPinType NativeType;
			if (!Schema->ConvertPropertyToPinType(UserProperty, UserType) || !Schema->ConvertPropertyToPinType(NativeProperty, NativeType))
			{
				Problems.Add({ TEXT("type_incompatible"), Variable.FriendlyName,
					FString::Printf(TEXT("Member '%s' or native property '%s' has a type Blueprint cannot express"), *Variable.FriendlyName, *NativeName) });
				continue;
			}
			TArray<FString> Nested;
			ReconcileUserTypeSlot(UserType.PinSubCategoryObject, NativeType.PinSubCategoryObject, Nested);
			ReconcileUserTypeSlot(UserType.PinValueType.TerminalSubCategoryObject, NativeType.PinValueType.TerminalSubCategoryObject, Nested);
			if (Nested.Num() > 0)
			{
				Problems.Add({ TEXT("nested_user_type"), Variable.FriendlyName,
					FString::Printf(TEXT("Member '%s' is typed with the user-defined type %s, which is not redirected to the native type of '%s' yet; replace that type first (leaf types first)"),
						*Variable.FriendlyName, *FString::Join(Nested, TEXT(", ")), *NativeName) });
				continue;
			}
			if (!SamePinType(UserType, NativeType))
			{
				Problems.Add({ TEXT("type_incompatible"), Variable.FriendlyName,
					FString::Printf(TEXT("Member '%s' (%s) is not type-compatible with native property '%s' (%s)"),
						*Variable.FriendlyName, *PinTypeLabel(UserType), *NativeName, *PinTypeLabel(NativeType)) });
				continue;
			}
			if (const FString* Other = Claimed.Find(NativeProperty))
			{
				Problems.Add({ TEXT("duplicate_target"), Variable.FriendlyName,
					FString::Printf(TEXT("Members '%s' and '%s' both map to native property '%s'"), **Other, *Variable.FriendlyName, *NativeName) });
				continue;
			}
			Claimed.Add(NativeProperty, Variable.FriendlyName);
			Plan.Members.Add({ UserName, Variable.FriendlyName, NativeName });
		}

		TArray<FString> Unknown;
		for (const TPair<FString, FString>& Entry : Explicit)
		{
			if (!UsedEntries.Contains(Entry.Key))
			{
				Unknown.Add(Entry.Key);
			}
		}
		Unknown.Sort();
		for (const FString& Key : Unknown)
		{
			Problems.Add({ TEXT("unknown_member_map_entry"), Key, FString::Printf(TEXT("member_map names '%s', which is not a member of the user-defined struct"), *Key) });
		}

		for (TFieldIterator<FProperty> It(Native); It; ++It)
		{
			if (!Claimed.Contains(*It))
			{
				Plan.NativeOnly.Add(It->GetName());
			}
		}
	}

	int32 EnumValueCount(const UEnum* Enum)
	{
		int32 Count = Enum->NumEnums();
		if (Count > 0 && Enum->GetNameStringByIndex(Count - 1).EndsWith(TEXT("_MAX")))
		{
			--Count;
		}
		return Count;
	}

	void MatchEnumValues(
		const UUserDefinedEnum* User,
		const UEnum* Native,
		const TMap<FString, FString>& Explicit,
		FCortexDataTypeReplacementPlan& Plan,
		TArray<FProblem>& Problems)
	{
		const int32 UserCount = EnumValueCount(User);
		const int32 NativeCount = EnumValueCount(Native);
		if (UserCount == 0)
		{
			Problems.Add({ TEXT("no_values"), User->GetPathName(), TEXT("The user-defined enum has no values") });
			return;
		}

		auto FindNative = [Native, NativeCount](const FString& Target) -> int32
		{
			for (int32 Index = 0; Index < NativeCount; ++Index)
			{
				if (Native->GetNameStringByIndex(Index) == Target || Native->GetDisplayNameTextByIndex(Index).ToString() == Target)
				{
					return Index;
				}
			}
			return INDEX_NONE;
		};

		TSet<FString> UsedEntries;
		TMap<int32, FString> Claimed;
		for (int32 Index = 0; Index < UserCount; ++Index)
		{
			const FString Token = User->GetNameStringByIndex(Index);
			const FString Display = User->GetDisplayNameTextByIndex(Index).ToString();
			FString Target = Display;
			if (const FString* ByToken = Explicit.Find(Token))
			{
				Target = *ByToken;
				UsedEntries.Add(Token);
			}
			else if (const FString* ByDisplay = Explicit.Find(Display))
			{
				Target = *ByDisplay;
				UsedEntries.Add(Display);
			}

			const int32 NativeIndex = FindNative(Target);
			if (NativeIndex == INDEX_NONE)
			{
				Problems.Add({ TEXT("unmatched_value"), Token,
					FString::Printf(TEXT("Value '%s' (display name '%s') has no native enumerator '%s' on %s; pass value_map {\"%s\":\"<native enumerator>\"}"),
						*Token, *Display, *Target, *Native->GetPathName(), *Token) });
				continue;
			}
			const int64 UserValue = User->GetValueByIndex(Index);
			const int64 NativeValue = Native->GetValueByIndex(NativeIndex);
			if (UserValue != NativeValue)
			{
				Problems.Add({ TEXT("value_mismatch"), Token,
					FString::Printf(TEXT("Value '%s' is %lld but native enumerator '%s' is %lld; stored data would change meaning"),
						*Token, UserValue, *Native->GetNameStringByIndex(NativeIndex), NativeValue) });
				continue;
			}
			if (const FString* Other = Claimed.Find(NativeIndex))
			{
				Problems.Add({ TEXT("duplicate_target"), Token,
					FString::Printf(TEXT("Values '%s' and '%s' both map to native enumerator '%s'"), **Other, *Token, *Native->GetNameStringByIndex(NativeIndex)) });
				continue;
			}
			Claimed.Add(NativeIndex, Token);
			Plan.Values.Emplace(Token, Native->GetNameStringByIndex(NativeIndex));
		}

		TArray<FString> Unknown;
		for (const TPair<FString, FString>& Entry : Explicit)
		{
			if (!UsedEntries.Contains(Entry.Key))
			{
				Unknown.Add(Entry.Key);
			}
		}
		Unknown.Sort();
		for (const FString& Key : Unknown)
		{
			Problems.Add({ TEXT("unknown_value_map_entry"), Key, FString::Printf(TEXT("value_map names '%s', which is not a value of the user-defined enum"), *Key) });
		}

		for (int32 Index = 0; Index < NativeCount; ++Index)
		{
			if (!Claimed.Contains(Index))
			{
				Plan.NativeOnly.Add(Native->GetNameStringByIndex(Index));
			}
		}
	}

	void AddExistingRedirectProblems(const FString& UserPath, const TArray<ECoreRedirectFlags>& Flags, const FString& NativePath, TArray<FProblem>& Problems)
	{
		for (const ECoreRedirectFlags Flag : Flags)
		{
			const FCoreRedirectObjectName Old(UserPath);
			const FCoreRedirectObjectName New = FCoreRedirects::GetRedirectedName(Flag, Old);
			if (New != Old)
			{
				const bool bSameTarget = New.ToString() == NativePath;
				Problems.Add({ bSameTarget ? TEXT("redirect_present") : TEXT("redirect_conflict"), UserPath,
					FString::Printf(TEXT("A redirect for %s already exists (to %s); %s"), *UserPath, *New.ToString(),
						bSameTarget ? TEXT("the plan is already in place, run verify_type_replacement") : TEXT("remove it from the ini before planning another target")) });
				return;
			}
		}
	}

	void BuildStructRedirects(FCortexDataTypeReplacementPlan& Plan)
	{
		Plan.RedirectLines.Add(FString::Printf(TEXT("+ObjectRedirects=(OldName=\"%s\",NewName=\"%s\",OverrideClassName=\"%s\")"), *Plan.UserType, *Plan.NativeType, ScriptStructClass));
		Plan.RedirectLines.Add(FString::Printf(TEXT("+StructRedirects=(OldName=\"%s\",NewName=\"%s\")"), *Plan.UserType, *Plan.NativeType));
		for (const FCortexDataTypeReplacementMember& Member : Plan.Members)
		{
			Plan.RedirectLines.Add(FString::Printf(TEXT("+PropertyRedirects=(OldName=\"%s.%s\",NewName=\"%s\")"), *Plan.NativeType, *Member.UserName, *Member.NativeName));
		}
	}

	void BuildEnumRedirects(FCortexDataTypeReplacementPlan& Plan)
	{
		TArray<FString> Changes;
		for (const TPair<FString, FString>& Value : Plan.Values)
		{
			Changes.Add(FString::Printf(TEXT("(\"%s\",\"%s\")"), *Value.Key, *Value.Value));
		}
		Plan.RedirectLines.Add(FString::Printf(TEXT("+EnumRedirects=(OldName=\"%s\",NewName=\"%s\",OverrideClassName=\"%s\",ValueChanges=(%s))"),
			*Plan.UserType, *Plan.NativeType, EnumClass, *FString::Join(Changes, TEXT(","))));
	}

	TSharedRef<FJsonObject> CountsByKind(const TArray<FCortexDataTypeReplacementAsset>& Assets)
	{
		TSharedRef<FJsonObject> Counts = MakeShared<FJsonObject>();
		for (const TCHAR* Kind : { TEXT("blueprint"), TEXT("datatable"), TEXT("data_asset"), TEXT("struct"), TEXT("enum"), TEXT("world"), TEXT("other") })
		{
			Counts->SetNumberField(Kind, 0);
		}
		for (const FCortexDataTypeReplacementAsset& Asset : Assets)
		{
			Counts->SetNumberField(Asset.Kind, Counts->GetNumberField(Asset.Kind) + 1);
		}
		return Counts;
	}

	constexpr int32 MaxResponseAssets = 50;
}

FString FCortexDataTypeReplacementOps::DefaultPlanPath(const FString& UserType)
{
	return FString::Printf(TEXT("Saved/CortexTypeReplacement/%s.plan.json"), *FPackageName::ObjectPathToObjectName(UserType));
}

FString FCortexDataTypeReplacementOps::NormalizeObjectPath(const FString& Path)
{
	FString Normalized = Path.TrimStartAndEnd();
	if (!Normalized.Contains(TEXT(".")))
	{
		Normalized += TEXT(".") + FPackageName::GetShortName(Normalized);
	}
	return Normalized;
}

UObject* FCortexDataTypeReplacementOps::LoadAssetObject(const FString& ObjectPath)
{
	const FString PackageName = FPackageName::ObjectPathToPackageName(ObjectPath);
	if (!FPackageName::IsValidLongPackageName(PackageName))
	{
		return nullptr;
	}
	if (FindPackage(nullptr, *PackageName) == nullptr && !FPackageName::DoesPackageExist(PackageName))
	{
		return nullptr;
	}
	return LoadObject<UObject>(nullptr, *ObjectPath);
}

UScriptStruct* FCortexDataTypeReplacementOps::FindNativeStruct(const FString& Name)
{
	auto IsNative = [](const UScriptStruct* Struct)
	{
		return Struct != nullptr && !Struct->IsA<UUserDefinedStruct>() && Struct->GetOutermost()->GetName().StartsWith(TEXT("/Script/"));
	};
	if (Name.StartsWith(TEXT("/")))
	{
		UScriptStruct* Found = FindObject<UScriptStruct>(nullptr, *Name);
		return IsNative(Found) ? Found : nullptr;
	}
	UScriptStruct* Found = FindFirstObject<UScriptStruct>(*Name, EFindFirstObjectOptions::NativeFirst);
	if (!IsNative(Found) && Name.Len() > 1 && Name[0] == TEXT('F'))
	{
		Found = FindFirstObject<UScriptStruct>(*Name.RightChop(1), EFindFirstObjectOptions::NativeFirst);
	}
	return IsNative(Found) ? Found : nullptr;
}

UEnum* FCortexDataTypeReplacementOps::FindNativeEnum(const FString& Name)
{
	auto IsNative = [](const UEnum* Enum)
	{
		return Enum != nullptr && !Enum->IsA<UUserDefinedEnum>() && Enum->GetOutermost()->GetName().StartsWith(TEXT("/Script/"));
	};
	UEnum* Found = Name.StartsWith(TEXT("/")) ? FindObject<UEnum>(nullptr, *Name) : FindFirstObject<UEnum>(*Name, EFindFirstObjectOptions::NativeFirst);
	return IsNative(Found) ? Found : nullptr;
}

TArray<FCortexDataTypeReplacementAsset> FCortexDataTypeReplacementOps::FindAffectedAssets(const FString& UserTypePath)
{
	IAssetRegistry& Registry = IAssetRegistry::GetChecked();
	const FString UserPackage = FPackageName::ObjectPathToPackageName(UserTypePath);

	TSet<FTopLevelAssetPath> BlueprintClasses;
	TSet<FTopLevelAssetPath> DataAssetClasses;
	Registry.GetDerivedClassNames({ UBlueprint::StaticClass()->GetClassPathName() }, TSet<FTopLevelAssetPath>(), BlueprintClasses);
	Registry.GetDerivedClassNames({ UDataAsset::StaticClass()->GetClassPathName() }, TSet<FTopLevelAssetPath>(), DataAssetClasses);
	BlueprintClasses.Add(UBlueprint::StaticClass()->GetClassPathName());
	DataAssetClasses.Add(UDataAsset::StaticClass()->GetClassPathName());

	TArray<FName> Referencers;
	Registry.GetReferencers(FName(*UserPackage), Referencers, UE::AssetRegistry::EDependencyCategory::Package, UE::AssetRegistry::EDependencyQuery::Hard);

	TArray<FCortexDataTypeReplacementAsset> Out;
	static const FName RowStructureTag(TEXT("RowStructure"));
	for (const FName& Referencer : Referencers)
	{
		if (Referencer.ToString() == UserPackage)
		{
			continue;
		}
		TArray<FAssetData> Assets;
		Registry.GetAssetsByPackageName(Referencer, Assets, /*bIncludeOnlyOnDiskAssets=*/true);
		for (const FAssetData& Asset : Assets)
		{
			FCortexDataTypeReplacementAsset Entry;
			Entry.Path = Asset.GetObjectPathString();
			const FTopLevelAssetPath ClassPath = Asset.AssetClassPath;
			if (ClassPath == UDataTable::StaticClass()->GetClassPathName())
			{
				Entry.Kind = TEXT("datatable");
				FString RowStruct;
				Entry.bRowStruct = Asset.GetTagValue(RowStructureTag, RowStruct) && RowStruct == UserTypePath;
			}
			else if (BlueprintClasses.Contains(ClassPath))
			{
				Entry.Kind = TEXT("blueprint");
			}
			else if (ClassPath == UUserDefinedStruct::StaticClass()->GetClassPathName())
			{
				Entry.Kind = TEXT("struct");
			}
			else if (ClassPath == UUserDefinedEnum::StaticClass()->GetClassPathName())
			{
				Entry.Kind = TEXT("enum");
			}
			else if (ClassPath == UWorld::StaticClass()->GetClassPathName())
			{
				Entry.Kind = TEXT("world");
			}
			else if (DataAssetClasses.Contains(ClassPath))
			{
				Entry.Kind = TEXT("data_asset");
			}
			else
			{
				Entry.Kind = TEXT("other");
			}
			Out.Add(Entry);
		}
	}
	Out.Sort([](const FCortexDataTypeReplacementAsset& A, const FCortexDataTypeReplacementAsset& B) { return A.Path < B.Path; });
	return Out;
}

FString FCortexDataTypeReplacementOps::BaselineFileName(const FString& AssetPath, const FString& Kind)
{
	FString Stem = FPackageName::ObjectPathToPackageName(AssetPath);
	Stem.ReplaceInline(TEXT("/"), TEXT("__"));
	return FString::Printf(TEXT("%s.%s.json"), *Stem.RightChop(Stem.StartsWith(TEXT("__")) ? 2 : 0), *Kind);
}

FCortexCommandResult FCortexDataTypeReplacementOps::PlanTypeReplacement(const TSharedPtr<FJsonObject>& Params)
{
	FString UserTypeParam;
	FString NativeTypeParam;
	if (!Params.IsValid() || !Params->TryGetStringField(TEXT("user_type"), UserTypeParam) || !Params->TryGetStringField(TEXT("native_type"), NativeTypeParam)
		|| UserTypeParam.IsEmpty() || NativeTypeParam.IsEmpty())
	{
		return InvalidField(TEXT("Missing required params: user_type and native_type"));
	}

	TMap<FString, FString> MemberMapParam;
	TMap<FString, FString> ValueMapParam;
	FString ParseError;
	if (!ReadStringMap(Params, TEXT("member_map"), MemberMapParam, ParseError) || !ReadStringMap(Params, TEXT("value_map"), ValueMapParam, ParseError))
	{
		return InvalidField(ParseError);
	}
	FString BaselineDir;
	FString OutPath;
	if ((Params->HasField(TEXT("baseline_dir")) && (!Params->TryGetStringField(TEXT("baseline_dir"), BaselineDir) || BaselineDir.TrimStartAndEnd().IsEmpty()))
		|| (Params->HasField(TEXT("out_path")) && (!Params->TryGetStringField(TEXT("out_path"), OutPath) || OutPath.TrimStartAndEnd().IsEmpty())))
	{
		return InvalidField(TEXT("baseline_dir and out_path must be non-empty strings"));
	}

	const FString UserPath = NormalizeObjectPath(UserTypeParam);
	UObject* UserObject = LoadAssetObject(UserPath);
	if (UserObject == nullptr)
	{
		return FCortexCommandRouter::Error(CortexErrorCodes::AssetNotFound, FString::Printf(TEXT("user_type is not a loadable asset: %s"), *UserPath));
	}
	UUserDefinedStruct* UserStruct = Cast<UUserDefinedStruct>(UserObject);
	UUserDefinedEnum* UserEnum = Cast<UUserDefinedEnum>(UserObject);
	if (UserStruct == nullptr && UserEnum == nullptr)
	{
		return InvalidField(FString::Printf(TEXT("user_type must be a user-defined struct or enum asset, but %s is a %s"), *UserPath, *UserObject->GetClass()->GetName()));
	}
	const bool bStruct = UserStruct != nullptr;
	if (bStruct && !ValueMapParam.IsEmpty())
	{
		return InvalidField(TEXT("value_map only applies to a user-defined enum; use member_map for a struct"));
	}
	if (!bStruct && !MemberMapParam.IsEmpty())
	{
		return InvalidField(TEXT("member_map only applies to a user-defined struct; use value_map for an enum"));
	}

	UScriptStruct* NativeStruct = bStruct ? FindNativeStruct(NativeTypeParam) : nullptr;
	UEnum* NativeEnum = bStruct ? nullptr : FindNativeEnum(NativeTypeParam);
	UObject* NativeObject = bStruct ? static_cast<UObject*>(NativeStruct) : static_cast<UObject*>(NativeEnum);
	if (NativeObject == nullptr)
	{
		return InvalidField(FString::Printf(TEXT("native_type '%s' is not a native %s (a /Script/Module.Name path or a type name)"), *NativeTypeParam, bStruct ? TEXT("struct") : TEXT("enum")));
	}
	if (!CastChecked<UField>(NativeObject)->GetBoolMetaData(TEXT("BlueprintType")))
	{
		return FCortexCommandRouter::Error(CortexErrorCodes::InvalidOperation,
			FString::Printf(TEXT("%s is not BlueprintType, so Blueprint variables and pins cannot use it"), *NativeObject->GetPathName()),
			ProblemDetails({ { TEXT("native_not_blueprint_type"), NativeObject->GetPathName(), TEXT("Mark the native type BlueprintType") } }));
	}

	FCortexDataTypeReplacementPlan Plan;
	Plan.Kind = bStruct ? TEXT("struct") : TEXT("enum");
	Plan.UserType = UserObject->GetPathName();
	Plan.NativeType = NativeObject->GetPathName();

	TArray<FProblem> Problems;
	if (bStruct)
	{
		AddExistingRedirectProblems(Plan.UserType, { ECoreRedirectFlags::Type_Object, ECoreRedirectFlags::Type_Struct }, Plan.NativeType, Problems);
		MatchStructMembers(UserStruct, NativeStruct, MemberMapParam, Plan, Problems);
	}
	else
	{
		AddExistingRedirectProblems(Plan.UserType, { ECoreRedirectFlags::Type_Enum }, Plan.NativeType, Problems);
		MatchEnumValues(UserEnum, NativeEnum, ValueMapParam, Plan, Problems);
	}

	Plan.AffectedAssets = FindAffectedAssets(Plan.UserType);
	if (bStruct && !NativeStruct->IsChildOf(FTableRowBase::StaticStruct()))
	{
		for (const FCortexDataTypeReplacementAsset& Asset : Plan.AffectedAssets)
		{
			if (Asset.bRowStruct)
			{
				Problems.Add({ TEXT("native_not_table_row"), Asset.Path,
					FString::Printf(TEXT("DataTable %s uses the user-defined struct as its row struct, but %s does not derive from FTableRowBase"), *Asset.Path, *Plan.NativeType) });
			}
		}
	}

	if (Problems.Num() > 0)
	{
		return FCortexCommandRouter::Error(
			CortexErrorCodes::InvalidOperation,
			FString::Printf(TEXT("The replacement of %s by %s cannot be planned: %d problem(s)"), *Plan.UserType, *Plan.NativeType, Problems.Num()),
			ProblemDetails(Problems));
	}

	if (bStruct)
	{
		BuildStructRedirects(Plan);
	}
	else
	{
		BuildEnumRedirects(Plan);
	}

	// The baseline is exported with the exporters before any redirect exists and recorded in the plan.
	if (!BaselineDir.IsEmpty())
	{
		FCortexResolvedFilePath Probe;
		FString ErrorCode;
		FString ErrorMessage;
		if (!FCortexSafeFileContract::ResolveRelativeChildWritePath(BaselineDir, TEXT("baseline.json"), Probe, ErrorCode, ErrorMessage))
		{
			return FCortexCommandRouter::Error(ErrorCode, ErrorMessage);
		}
		for (const FCortexDataTypeReplacementAsset& Asset : Plan.AffectedAssets)
		{
			if (Asset.Kind != TEXT("datatable") && Asset.Kind != TEXT("data_asset"))
			{
				continue;
			}
			const FString File = FPaths::Combine(BaselineDir, BaselineFileName(Asset.Path, Asset.Kind));
			TSharedPtr<FJsonObject> ExportParams = MakeShared<FJsonObject>();
			ExportParams->SetStringField(TEXT("out_path"), File);
			FCortexCommandResult Exported;
			if (Asset.Kind == TEXT("datatable"))
			{
				ExportParams->SetStringField(TEXT("table_path"), Asset.Path);
				Exported = FCortexDataExportOps::ExportDatatableJson(ExportParams);
			}
			else
			{
				TArray<TSharedPtr<FJsonValue>> AssetPaths;
				AssetPaths.Add(MakeShared<FJsonValueString>(Asset.Path));
				ExportParams->SetArrayField(TEXT("asset_paths"), AssetPaths);
				ExportParams->SetBoolField(TEXT("include_properties"), true);
				Exported = FCortexDataExportOps::ExportDataAssetsJson(ExportParams);
			}
			if (!Exported.bSuccess)
			{
				return FCortexCommandRouter::Error(
					Exported.ErrorCode, FString::Printf(TEXT("Baseline export of %s failed: %s"), *Asset.Path, *Exported.ErrorMessage), Exported.ErrorDetails);
			}
			Plan.Baseline.Add({ Asset.Path, Asset.Kind, File });
		}
	}

	Plan.PlanHash = Plan.ComputeHash();

	const FString PlanPath = OutPath.IsEmpty() ? DefaultPlanPath(Plan.UserType) : OutPath;
	FCortexResolvedFilePath ResolvedPlanPath;
	FString ErrorCode;
	FString ErrorMessage;
	if (!FCortexSafeFileContract::ResolveWritePath(PlanPath, ResolvedPlanPath, ErrorCode, ErrorMessage))
	{
		return FCortexCommandRouter::Error(ErrorCode, ErrorMessage);
	}
	const FCortexJsonFileWriteResult Written = FCortexSafeFileContract::WriteJsonReportAtomic(ResolvedPlanPath, Plan.ToJson());
	if (!Written.bWritten)
	{
		return FCortexCommandRouter::Error(Written.ErrorCode, Written.ErrorMessage);
	}

	TSharedRef<FJsonObject> Data = Plan.ToJson();
	const int32 AssetCount = Plan.AffectedAssets.Num();
	if (AssetCount > MaxResponseAssets)
	{
		TArray<TSharedPtr<FJsonValue>> Shown;
		const TArray<TSharedPtr<FJsonValue>>& All = Data->GetArrayField(TEXT("affected_assets"));
		for (int32 Index = 0; Index < MaxResponseAssets; ++Index)
		{
			Shown.Add(All[Index]);
		}
		Data->SetArrayField(TEXT("affected_assets"), Shown);
	}
	Data->SetBoolField(TEXT("affected_truncated"), AssetCount > MaxResponseAssets);
	Data->SetNumberField(TEXT("affected_count"), AssetCount);
	Data->SetObjectField(TEXT("affected_counts"), CountsByKind(Plan.AffectedAssets));
	Data->SetStringField(TEXT("plan_path"), PlanPath);
	Data->SetStringField(TEXT("plan_file"), ResolvedPlanPath.AbsolutePath);
	Data->SetStringField(TEXT("next_step"), TEXT("Add the redirects lines to the [CoreRedirects] section of Config/DefaultEngine.ini, restart the editor (editor_restart), then run data.verify_type_replacement with plan_path."));
	return FCortexCommandRouter::Success(Data);
}
