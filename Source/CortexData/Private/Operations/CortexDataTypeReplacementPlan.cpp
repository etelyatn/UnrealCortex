#include "Operations/CortexDataTypeReplacementPlan.h"

#include "CortexSafeFileContract.h"
#include "Dom/JsonValue.h"
#include "Misc/SecureHash.h"

namespace
{
	TArray<TSharedPtr<FJsonValue>> StringValues(const TArray<FString>& Strings)
	{
		TArray<TSharedPtr<FJsonValue>> Values;
		Values.Reserve(Strings.Num());
		for (const FString& String : Strings)
		{
			Values.Add(MakeShared<FJsonValueString>(String));
		}
		return Values;
	}

	bool ReadStringArray(const TSharedPtr<FJsonObject>& Json, const TCHAR* Field, TArray<FString>& Out)
	{
		const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
		if (!Json->TryGetArrayField(Field, Values) || Values == nullptr)
		{
			return false;
		}
		for (const TSharedPtr<FJsonValue>& Value : *Values)
		{
			FString String;
			if (!Value.IsValid() || !Value->TryGetString(String))
			{
				return false;
			}
			Out.Add(String);
		}
		return true;
	}

	bool ReadObjects(const TSharedPtr<FJsonObject>& Json, const TCHAR* Field, TArray<TSharedPtr<FJsonObject>>& Out)
	{
		const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
		if (!Json->TryGetArrayField(Field, Values) || Values == nullptr)
		{
			return false;
		}
		for (const TSharedPtr<FJsonValue>& Value : *Values)
		{
			const TSharedPtr<FJsonObject> Object = Value.IsValid() ? Value->AsObject() : nullptr;
			if (!Object.IsValid())
			{
				return false;
			}
			Out.Add(Object);
		}
		return true;
	}
}

TMap<FString, FString> FCortexDataTypeReplacementPlan::MemberMap() const
{
	TMap<FString, FString> Map;
	for (const FCortexDataTypeReplacementMember& Member : Members)
	{
		Map.Add(Member.UserName, Member.NativeName);
	}
	return Map;
}

TMap<FString, FString> FCortexDataTypeReplacementPlan::ValueMap() const
{
	TMap<FString, FString> Map;
	for (const TPair<FString, FString>& Value : Values)
	{
		Map.Add(Value.Key, Value.Value);
	}
	return Map;
}

TSharedRef<FJsonObject> FCortexDataTypeReplacementPlan::ToJson(const bool bIncludeHash) const
{
	TSharedRef<FJsonObject> Json = MakeShared<FJsonObject>();
	Json->SetStringField(TEXT("kind"), Kind);
	Json->SetStringField(TEXT("user_type"), UserType);
	Json->SetStringField(TEXT("native_type"), NativeType);

	TArray<TSharedPtr<FJsonValue>> MemberValues;
	TSharedRef<FJsonObject> MemberMapJson = MakeShared<FJsonObject>();
	for (const FCortexDataTypeReplacementMember& Member : Members)
	{
		TSharedRef<FJsonObject> MemberJson = MakeShared<FJsonObject>();
		MemberJson->SetStringField(TEXT("user_name"), Member.UserName);
		MemberJson->SetStringField(TEXT("friendly_name"), Member.FriendlyName);
		MemberJson->SetStringField(TEXT("native_name"), Member.NativeName);
		MemberValues.Add(MakeShared<FJsonValueObject>(MemberJson));
		MemberMapJson->SetStringField(Member.UserName, Member.NativeName);
	}
	Json->SetArrayField(TEXT("members"), MemberValues);
	Json->SetObjectField(TEXT("member_map"), MemberMapJson);
	Json->SetArrayField(TEXT("native_only"), StringValues(NativeOnly));

	TArray<TSharedPtr<FJsonValue>> ValueValues;
	TSharedRef<FJsonObject> ValueMapJson = MakeShared<FJsonObject>();
	for (const TPair<FString, FString>& Value : Values)
	{
		TSharedRef<FJsonObject> ValueJson = MakeShared<FJsonObject>();
		ValueJson->SetStringField(TEXT("from"), Value.Key);
		ValueJson->SetStringField(TEXT("to"), Value.Value);
		ValueValues.Add(MakeShared<FJsonValueObject>(ValueJson));
		ValueMapJson->SetStringField(Value.Key, Value.Value);
	}
	Json->SetArrayField(TEXT("values"), ValueValues);
	Json->SetObjectField(TEXT("value_map"), ValueMapJson);

	Json->SetArrayField(TEXT("redirects"), StringValues(RedirectLines));

	TArray<TSharedPtr<FJsonValue>> AssetValues;
	for (const FCortexDataTypeReplacementAsset& Asset : AffectedAssets)
	{
		TSharedRef<FJsonObject> AssetJson = MakeShared<FJsonObject>();
		AssetJson->SetStringField(TEXT("path"), Asset.Path);
		AssetJson->SetStringField(TEXT("kind"), Asset.Kind);
		AssetJson->SetBoolField(TEXT("row_struct"), Asset.bRowStruct);
		AssetValues.Add(MakeShared<FJsonValueObject>(AssetJson));
	}
	Json->SetArrayField(TEXT("affected_assets"), AssetValues);

	TArray<TSharedPtr<FJsonValue>> BaselineValues;
	for (const FCortexDataTypeReplacementBaseline& Entry : Baseline)
	{
		TSharedRef<FJsonObject> EntryJson = MakeShared<FJsonObject>();
		EntryJson->SetStringField(TEXT("asset_path"), Entry.AssetPath);
		EntryJson->SetStringField(TEXT("kind"), Entry.Kind);
		EntryJson->SetStringField(TEXT("file"), Entry.File);
		BaselineValues.Add(MakeShared<FJsonValueObject>(EntryJson));
	}
	Json->SetArrayField(TEXT("baseline"), BaselineValues);

	TArray<TSharedPtr<FJsonValue>> PrerequisiteValues;
	for (const FCortexDataTypeReplacementPrerequisite& Prerequisite : Prerequisites)
	{
		TSharedRef<FJsonObject> PrerequisiteJson = MakeShared<FJsonObject>();
		PrerequisiteJson->SetStringField(TEXT("kind"), Prerequisite.Kind);
		PrerequisiteJson->SetStringField(TEXT("user_type"), Prerequisite.UserType);
		PrerequisiteJson->SetStringField(TEXT("native_type"), Prerequisite.NativeType);
		TArray<TSharedPtr<FJsonValue>> InnerMembers;
		for (const FCortexDataTypeReplacementMember& Member : Prerequisite.Members)
		{
			TSharedRef<FJsonObject> MemberJson = MakeShared<FJsonObject>();
			MemberJson->SetStringField(TEXT("user_name"), Member.UserName);
			MemberJson->SetStringField(TEXT("friendly_name"), Member.FriendlyName);
			MemberJson->SetStringField(TEXT("native_name"), Member.NativeName);
			InnerMembers.Add(MakeShared<FJsonValueObject>(MemberJson));
		}
		PrerequisiteJson->SetArrayField(TEXT("members"), InnerMembers);
		TArray<TSharedPtr<FJsonValue>> InnerValues;
		for (const TPair<FString, FString>& Value : Prerequisite.Values)
		{
			TSharedRef<FJsonObject> ValueJson = MakeShared<FJsonObject>();
			ValueJson->SetStringField(TEXT("from"), Value.Key);
			ValueJson->SetStringField(TEXT("to"), Value.Value);
			InnerValues.Add(MakeShared<FJsonValueObject>(ValueJson));
		}
		PrerequisiteJson->SetArrayField(TEXT("values"), InnerValues);
		PrerequisiteValues.Add(MakeShared<FJsonValueObject>(PrerequisiteJson));
	}
	Json->SetArrayField(TEXT("prerequisites"), PrerequisiteValues);

	if (bIncludeHash)
	{
		Json->SetStringField(TEXT("plan_hash"), PlanHash);
	}
	return Json;
}

bool FCortexDataTypeReplacementPlan::FromJson(const TSharedPtr<FJsonObject>& Json, FCortexDataTypeReplacementPlan& OutPlan, FString& OutError)
{
	OutPlan = FCortexDataTypeReplacementPlan();
	if (!Json.IsValid())
	{
		OutError = TEXT("the plan is not a JSON object");
		return false;
	}
	if (!Json->TryGetStringField(TEXT("kind"), OutPlan.Kind) || (OutPlan.Kind != TEXT("struct") && OutPlan.Kind != TEXT("enum")))
	{
		OutError = TEXT("the plan has no valid kind (struct or enum)");
		return false;
	}
	if (!Json->TryGetStringField(TEXT("user_type"), OutPlan.UserType) || !Json->TryGetStringField(TEXT("native_type"), OutPlan.NativeType)
		|| !Json->TryGetStringField(TEXT("plan_hash"), OutPlan.PlanHash))
	{
		OutError = TEXT("the plan is missing user_type, native_type or plan_hash");
		return false;
	}

	TArray<TSharedPtr<FJsonObject>> Objects;
	if (!ReadObjects(Json, TEXT("members"), Objects))
	{
		OutError = TEXT("the plan has no members array");
		return false;
	}
	for (const TSharedPtr<FJsonObject>& Object : Objects)
	{
		FCortexDataTypeReplacementMember Member;
		if (!Object->TryGetStringField(TEXT("user_name"), Member.UserName) || !Object->TryGetStringField(TEXT("friendly_name"), Member.FriendlyName)
			|| !Object->TryGetStringField(TEXT("native_name"), Member.NativeName))
		{
			OutError = TEXT("a plan member lacks user_name, friendly_name or native_name");
			return false;
		}
		OutPlan.Members.Add(Member);
	}
	if (!ReadStringArray(Json, TEXT("native_only"), OutPlan.NativeOnly) || !ReadStringArray(Json, TEXT("redirects"), OutPlan.RedirectLines))
	{
		OutError = TEXT("the plan has no native_only or redirects array");
		return false;
	}

	Objects.Reset();
	if (!ReadObjects(Json, TEXT("values"), Objects))
	{
		OutError = TEXT("the plan has no values array");
		return false;
	}
	for (const TSharedPtr<FJsonObject>& Object : Objects)
	{
		FString From;
		FString To;
		if (!Object->TryGetStringField(TEXT("from"), From) || !Object->TryGetStringField(TEXT("to"), To))
		{
			OutError = TEXT("a plan value lacks from or to");
			return false;
		}
		OutPlan.Values.Emplace(From, To);
	}

	Objects.Reset();
	if (!ReadObjects(Json, TEXT("affected_assets"), Objects))
	{
		OutError = TEXT("the plan has no affected_assets array");
		return false;
	}
	for (const TSharedPtr<FJsonObject>& Object : Objects)
	{
		FCortexDataTypeReplacementAsset Asset;
		if (!Object->TryGetStringField(TEXT("path"), Asset.Path) || !Object->TryGetStringField(TEXT("kind"), Asset.Kind)
			|| !Object->TryGetBoolField(TEXT("row_struct"), Asset.bRowStruct))
		{
			OutError = TEXT("an affected asset lacks path, kind or row_struct");
			return false;
		}
		OutPlan.AffectedAssets.Add(Asset);
	}

	Objects.Reset();
	if (!ReadObjects(Json, TEXT("baseline"), Objects))
	{
		OutError = TEXT("the plan has no baseline array");
		return false;
	}
	for (const TSharedPtr<FJsonObject>& Object : Objects)
	{
		FCortexDataTypeReplacementBaseline Entry;
		if (!Object->TryGetStringField(TEXT("asset_path"), Entry.AssetPath) || !Object->TryGetStringField(TEXT("kind"), Entry.Kind)
			|| !Object->TryGetStringField(TEXT("file"), Entry.File))
		{
			OutError = TEXT("a baseline entry lacks asset_path, kind or file");
			return false;
		}
		OutPlan.Baseline.Add(Entry);
	}

	Objects.Reset();
	if (!ReadObjects(Json, TEXT("prerequisites"), Objects))
	{
		OutError = TEXT("the plan has no prerequisites array");
		return false;
	}
	for (const TSharedPtr<FJsonObject>& Object : Objects)
	{
		FCortexDataTypeReplacementPrerequisite Prerequisite;
		if (!Object->TryGetStringField(TEXT("kind"), Prerequisite.Kind) || !Object->TryGetStringField(TEXT("user_type"), Prerequisite.UserType)
			|| !Object->TryGetStringField(TEXT("native_type"), Prerequisite.NativeType))
		{
			OutError = TEXT("a prerequisite lacks kind, user_type or native_type");
			return false;
		}
		TArray<TSharedPtr<FJsonObject>> Inner;
		if (!ReadObjects(Object, TEXT("members"), Inner))
		{
			OutError = TEXT("a prerequisite has no members array");
			return false;
		}
		for (const TSharedPtr<FJsonObject>& MemberJson : Inner)
		{
			FCortexDataTypeReplacementMember Member;
			if (!MemberJson->TryGetStringField(TEXT("user_name"), Member.UserName) || !MemberJson->TryGetStringField(TEXT("friendly_name"), Member.FriendlyName)
				|| !MemberJson->TryGetStringField(TEXT("native_name"), Member.NativeName))
			{
				OutError = TEXT("a prerequisite member lacks user_name, friendly_name or native_name");
				return false;
			}
			Prerequisite.Members.Add(Member);
		}
		Inner.Reset();
		if (!ReadObjects(Object, TEXT("values"), Inner))
		{
			OutError = TEXT("a prerequisite has no values array");
			return false;
		}
		for (const TSharedPtr<FJsonObject>& ValueJson : Inner)
		{
			FString From;
			FString To;
			if (!ValueJson->TryGetStringField(TEXT("from"), From) || !ValueJson->TryGetStringField(TEXT("to"), To))
			{
				OutError = TEXT("a prerequisite value lacks from or to");
				return false;
			}
			Prerequisite.Values.Emplace(From, To);
		}
		OutPlan.Prerequisites.Add(Prerequisite);
	}
	return true;
}

FString FCortexDataTypeReplacementPlan::ComputeHash() const
{
	const FString Canonical = FCortexSafeFileContract::SerializeCanonicalJson(ToJson(/*bIncludeHash=*/false));
	return FMD5::HashAnsiString(*Canonical);
}
