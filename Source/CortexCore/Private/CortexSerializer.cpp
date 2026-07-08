
#include "CortexSerializer.h"
#include "CortexCoreModule.h"
#include "UObject/UnrealType.h"
#include "UObject/TextProperty.h"
#include "UObject/EnumProperty.h"
#include "GameplayTagContainer.h"
#include "Misc/EngineVersionComparison.h"
#if UE_VERSION_OLDER_THAN(5, 5, 0)
#include "InstancedStruct.h"
#else
#include "StructUtils/InstancedStruct.h"
#endif
#include "UObject/SoftObjectPath.h"
#include "Dom/JsonValue.h"
#include "Misc/PackageName.h"
#include "Internationalization/Text.h"
#include "UObject/UObjectGlobals.h"

TMap<const UScriptStruct*, TArray<UScriptStruct*>> FCortexSerializer::SubtypeCache;
TMap<const UScriptStruct*, bool> FCortexSerializer::PositionalNumericStructCache;
const FString FCortexSerializer::ChangedMarker = TEXT("<changed>");

namespace
{
	constexpr const TCHAR* CortexTextType = TEXT("FText");
	constexpr const TCHAR* CortexTextSourceLiteral = TEXT("literal");
	constexpr const TCHAR* CortexTextSourceStringTable = TEXT("string_table");

	void AppendSerializationIssues(FCortexPropertySerializationResult& Target, const FCortexPropertySerializationResult& Source)
	{
		Target.Issues.Append(Source.Issues);
		Target.bPartial = Target.bPartial || Source.bPartial;
	}

	FCortexPropertySerializationResult MakeJsonObjectResult(TSharedPtr<FJsonObject> Object)
	{
		FCortexPropertySerializationResult Result;
		Result.JsonValue = MakeShared<FJsonValueObject>(Object.IsValid() ? Object : MakeShared<FJsonObject>());
		return Result;
	}

	FString JoinFieldPath(const FString& Parent, const FString& Child)
	{
		return Parent.IsEmpty() ? Child : Parent + TEXT(".") + Child;
	}

	FString IndexedFieldPath(const FString& Parent, int32 Index)
	{
		return FString::Printf(TEXT("%s[%d]"), *Parent, Index);
	}

	void AddIssue(
		FCortexPropertySerializationResult& Result,
		const FString& Field,
		const FString& Code,
		const FString& Message,
		ECortexSerializationSeverity Severity,
		bool bDegraded,
		bool bOmitted)
	{
		FCortexSerializationIssue Issue;
		Issue.Field = Field;
		Issue.Code = Code;
		Issue.Issue = Message;
		Issue.Severity = Severity;
		Issue.bDegraded = bDegraded;
		Issue.bOmitted = bOmitted;
		Result.Issues.Add(Issue);
		Result.bPartial = Result.bPartial || bDegraded || bOmitted;
	}

	bool IsObjectIdentityProperty(const FProperty* Property)
	{
		return CastField<FObjectProperty>(Property) != nullptr
			|| CastField<FClassProperty>(Property) != nullptr
			|| CastField<FSoftObjectProperty>(Property) != nullptr
			|| CastField<FSoftClassProperty>(Property) != nullptr;
	}

	bool HasObjectIdentityValue(const FProperty* Property, const void* ValuePtr)
	{
		if (const FSoftObjectProperty* SoftObjectProperty = CastField<FSoftObjectProperty>(Property))
		{
			return !SoftObjectProperty->GetPropertyValue(ValuePtr).ToSoftObjectPath().IsNull();
		}

		if (const FSoftClassProperty* SoftClassProperty = CastField<FSoftClassProperty>(Property))
		{
			return !SoftClassProperty->GetPropertyValue(ValuePtr).ToSoftObjectPath().IsNull();
		}

		if (const FObjectProperty* ObjectProperty = CastField<FObjectProperty>(Property))
		{
			return ObjectProperty->GetObjectPropertyValue(ValuePtr) != nullptr;
		}

		if (const FClassProperty* ClassProperty = CastField<FClassProperty>(Property))
		{
			return ClassProperty->GetObjectPropertyValue(ValuePtr) != nullptr;
		}

		return false;
	}

	TSharedPtr<FJsonValue> ObjectIdentityToJson(const FProperty* Property, const void* ValuePtr)
	{
		if (const FSoftObjectProperty* SoftObjectProperty = CastField<FSoftObjectProperty>(Property))
		{
			const FSoftObjectPtr& SoftPtr = SoftObjectProperty->GetPropertyValue(ValuePtr);
			return MakeShared<FJsonValueString>(SoftPtr.ToSoftObjectPath().ToString());
		}

		if (const FSoftClassProperty* SoftClassProperty = CastField<FSoftClassProperty>(Property))
		{
			const FSoftObjectPtr& SoftPtr = SoftClassProperty->GetPropertyValue(ValuePtr);
			return MakeShared<FJsonValueString>(SoftPtr.ToSoftObjectPath().ToString());
		}

		if (const FObjectProperty* ObjectProperty = CastField<FObjectProperty>(Property))
		{
			const UObject* Object = ObjectProperty->GetObjectPropertyValue(ValuePtr);
			if (Object != nullptr)
			{
				TSharedPtr<FJsonValue> JsonValue = MakeShared<FJsonValueString>(Object->GetPathName());
				return JsonValue;
			}

			TSharedPtr<FJsonValue> JsonValue = MakeShared<FJsonValueNull>();
			return JsonValue;
		}

		if (const FClassProperty* ClassProperty = CastField<FClassProperty>(Property))
		{
			const UClass* Class = Cast<UClass>(ClassProperty->GetObjectPropertyValue(ValuePtr));
			if (Class != nullptr)
			{
				TSharedPtr<FJsonValue> JsonValue = MakeShared<FJsonValueString>(Class->GetPathName());
				return JsonValue;
			}

			TSharedPtr<FJsonValue> JsonValue = MakeShared<FJsonValueNull>();
			return JsonValue;
		}

		return MakeShared<FJsonValueNull>();
	}

	FCortexPropertySerializationResult StructToJsonDeepInternal(
		const UStruct* StructType,
		const void* StructData,
		const FCortexSerializationPolicy& Policy,
		const FString& BasePath);

	bool CortexGetTextObject(const TSharedPtr<FJsonValue>& JsonValue, TSharedPtr<FJsonObject>& OutObject, FString& OutLiteral)
	{
		if (!JsonValue.IsValid())
		{
			return false;
		}

		if (JsonValue->TryGetString(OutLiteral))
		{
			OutObject.Reset();
			return true;
		}

		const TSharedPtr<FJsonObject>* Object = nullptr;
		if (!JsonValue->TryGetObject(Object) || Object == nullptr || !(*Object).IsValid())
		{
			return false;
		}

		OutObject = *Object;
		return true;
	}
}

TSharedPtr<FJsonObject> FCortexSerializer::ObjectNonDefaultPropertiesToJson(
	const UObject* Object,
	const UObject* DefaultObject,
	int32 MaxDepth)
{
	if (Object == nullptr)
	{
		return MakeShared<FJsonObject>();
	}

	const UObject* EffectiveDefaultObject = DefaultObject;
	if (EffectiveDefaultObject == nullptr || EffectiveDefaultObject->GetClass() != Object->GetClass())
	{
		EffectiveDefaultObject = Object->GetClass()->GetDefaultObject();
	}

	if (EffectiveDefaultObject == nullptr)
	{
		return MakeShared<FJsonObject>();
	}

	return StructNonDefaultPropertiesToJson(Object->GetClass(), Object, EffectiveDefaultObject, MaxDepth);
}

TSharedPtr<FJsonValue> FCortexSerializer::NonDefaultPropertyToJson(
	const FProperty* Property,
	const void* ValuePtr,
	const void* DefaultValuePtr,
	int32 MaxDepth)
{
	if (Property == nullptr || ValuePtr == nullptr || DefaultValuePtr == nullptr)
	{
		return nullptr;
	}

	if (const FStructProperty* StructProp = CastField<FStructProperty>(Property))
	{
		if (MaxDepth <= 0)
		{
			if (!StructProp->Identical(ValuePtr, DefaultValuePtr, PPF_None))
			{
				return MakeShared<FJsonValueString>(ChangedMarker);
			}
			return nullptr;
		}

		const TSharedPtr<FJsonObject> NestedJson = StructNonDefaultPropertiesToJson(
			StructProp->Struct,
			ValuePtr,
			DefaultValuePtr,
			MaxDepth - 1);

		if (NestedJson->Values.Num() > 0)
		{
			return MakeShared<FJsonValueObject>(NestedJson);
		}

		return nullptr;
	}

	if (const FObjectProperty* ObjectProp = CastField<FObjectProperty>(Property))
	{
		if (Property->HasAllPropertyFlags(CPF_InstancedReference))
		{
			const UObject* CurrentObject = ObjectProp->GetObjectPropertyValue(ValuePtr);
			const UObject* DefaultObj = ObjectProp->GetObjectPropertyValue(DefaultValuePtr);

			if (CurrentObject == nullptr && DefaultObj == nullptr)
			{
				return nullptr;
			}

			if (CurrentObject == nullptr)
			{
				return MakeShared<FJsonValueNull>();
			}

			if (MaxDepth <= 0)
			{
				return MakeShared<FJsonValueString>(ChangedMarker);
			}

			TSharedPtr<FJsonObject> NestedJson = ObjectNonDefaultPropertiesToJson(
				CurrentObject,
				DefaultObj,
				MaxDepth - 1);
			const bool bClassChanged = DefaultObj == nullptr || CurrentObject->GetClass() != DefaultObj->GetClass();
			if (bClassChanged)
			{
				NestedJson->SetStringField(TEXT("class"), CurrentObject->GetClass()->GetName());
			}

			if (NestedJson->Values.Num() > 0)
			{
				return MakeShared<FJsonValueObject>(NestedJson);
			}

			return nullptr;
		}
	}

	if (Property->Identical(ValuePtr, DefaultValuePtr))
	{
		return nullptr;
	}

	if (MaxDepth <= 0)
	{
		return MakeShared<FJsonValueString>(ChangedMarker);
	}

	return PropertyToJson(Property, ValuePtr);
}

TSharedPtr<FJsonObject> FCortexSerializer::StructNonDefaultPropertiesToJson(
	const UStruct* StructType,
	const void* StructData,
	const void* DefaultData,
	int32 MaxDepth)
{
	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	if (StructType == nullptr || StructData == nullptr || DefaultData == nullptr)
	{
		return Result;
	}

	for (TFieldIterator<FProperty> It(StructType); It; ++It)
	{
		const FProperty* Property = *It;
		if (Property->HasAnyPropertyFlags(CPF_Transient))
		{
			continue;
		}

		const void* ValuePtr = Property->ContainerPtrToValuePtr<void>(StructData);
		const void* DefaultValuePtr = Property->ContainerPtrToValuePtr<void>(DefaultData);
		if (const TSharedPtr<FJsonValue> JsonValue = NonDefaultPropertyToJson(
			Property,
			ValuePtr,
			DefaultValuePtr,
			MaxDepth))
		{
			Result->SetField(Property->GetName(), JsonValue);
		}
	}

	return Result;
}

TSharedPtr<FJsonObject> FCortexSerializer::TextToJson(const FText& Text)
{
	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("type"), CortexTextType);
	Result->SetStringField(TEXT("value"), Text.ToString());

	FName TableId;
	FString Key;
	if (FTextInspector::GetTableIdAndKey(Text, TableId, Key))
	{
		Result->SetStringField(TEXT("source_kind"), CortexTextSourceStringTable);
		TSharedPtr<FJsonObject> StringTableObject = MakeShared<FJsonObject>();
		StringTableObject->SetStringField(TEXT("table_id"), TableId.ToString());
		StringTableObject->SetStringField(TEXT("key"), Key);
		Result->SetObjectField(TEXT("string_table"), StringTableObject);
	}
	else
	{
		Result->SetStringField(TEXT("source_kind"), CortexTextSourceLiteral);
	}

	return Result;
}

bool FCortexSerializer::NormalizeTextDescriptor(
	const TSharedPtr<FJsonValue>& JsonValue,
	TSharedPtr<FJsonObject>& OutDescriptor,
	FText* OutText,
	TArray<FString>& OutErrors)
{
	OutDescriptor.Reset();

	TSharedPtr<FJsonObject> InputObject;
	FString LiteralValue;
	if (!CortexGetTextObject(JsonValue, InputObject, LiteralValue))
	{
		OutErrors.Add(TEXT("Expected string or object for FText descriptor"));
		return false;
	}

	TSharedPtr<FJsonObject> Normalized = MakeShared<FJsonObject>();
	Normalized->SetStringField(TEXT("type"), CortexTextType);

	if (!InputObject.IsValid())
	{
		Normalized->SetStringField(TEXT("source_kind"), CortexTextSourceLiteral);
		Normalized->SetStringField(TEXT("value"), LiteralValue);
		OutDescriptor = Normalized;
		if (OutText != nullptr)
		{
			*OutText = FText::FromString(LiteralValue);
		}
		return true;
	}

	FString Type;
	if (InputObject->TryGetStringField(TEXT("type"), Type) && Type != CortexTextType)
	{
		OutErrors.Add(FString::Printf(TEXT("FText descriptor type must be '%s'"), CortexTextType));
		return false;
	}

	if (!InputObject->TryGetStringField(TEXT("value"), LiteralValue))
	{
		OutErrors.Add(TEXT("FText descriptor missing required 'value' string"));
		return false;
	}

	const TSharedPtr<FJsonObject>* StringTableObject = nullptr;
	const bool bHasStringTable = InputObject->TryGetObjectField(TEXT("string_table"), StringTableObject)
		&& StringTableObject != nullptr
		&& (*StringTableObject).IsValid();

	FString SourceKind;
	if (!InputObject->TryGetStringField(TEXT("source_kind"), SourceKind) || SourceKind.IsEmpty())
	{
		SourceKind = bHasStringTable ? CortexTextSourceStringTable : CortexTextSourceLiteral;
	}

	if (SourceKind != CortexTextSourceLiteral && SourceKind != CortexTextSourceStringTable)
	{
		OutErrors.Add(FString::Printf(TEXT("Unsupported FText source_kind '%s'"), *SourceKind));
		return false;
	}

	Normalized->SetStringField(TEXT("source_kind"), SourceKind);
	Normalized->SetStringField(TEXT("value"), LiteralValue);

	if (SourceKind == CortexTextSourceLiteral)
	{
		if (bHasStringTable)
		{
			OutErrors.Add(TEXT("Literal FText descriptor must not include string_table"));
			return false;
		}
		if (OutText != nullptr)
		{
			*OutText = FText::FromString(LiteralValue);
		}
		OutDescriptor = Normalized;
		return true;
	}

	if (!bHasStringTable)
	{
		OutErrors.Add(TEXT("StringTable FText descriptor missing required string_table object"));
		return false;
	}

	FString TableId;
	FString Key;
	if (!(*StringTableObject)->TryGetStringField(TEXT("table_id"), TableId) || TableId.IsEmpty())
	{
		OutErrors.Add(TEXT("FText string_table object missing required 'table_id' string"));
		return false;
	}
	if (!(*StringTableObject)->TryGetStringField(TEXT("key"), Key) || Key.IsEmpty())
	{
		OutErrors.Add(TEXT("FText string_table object missing required 'key' string"));
		return false;
	}

	TSharedPtr<FJsonObject> NormalizedStringTable = MakeShared<FJsonObject>();
	NormalizedStringTable->SetStringField(TEXT("table_id"), TableId);
	NormalizedStringTable->SetStringField(TEXT("key"), Key);
	Normalized->SetObjectField(TEXT("string_table"), NormalizedStringTable);

	if (OutText != nullptr)
	{
		*OutText = FText::FromStringTable(FName(*TableId), Key);
	}

	OutDescriptor = Normalized;
	return true;
}

bool FCortexSerializer::TextFromJson(const TSharedPtr<FJsonValue>& JsonValue, FText& OutText, TArray<FString>& OutWarnings)
{
	TSharedPtr<FJsonObject> Normalized;
	return NormalizeTextDescriptor(JsonValue, Normalized, &OutText, OutWarnings);
}

bool FCortexSerializer::TextDescriptorsEqual(
	const TSharedPtr<FJsonValue>& Left,
	const TSharedPtr<FJsonValue>& Right,
	TArray<FString>& OutErrors)
{
	TSharedPtr<FJsonObject> NormalizedLeft;
	TSharedPtr<FJsonObject> NormalizedRight;
	if (!NormalizeTextDescriptor(Left, NormalizedLeft, nullptr, OutErrors))
	{
		return false;
	}
	if (!NormalizeTextDescriptor(Right, NormalizedRight, nullptr, OutErrors))
	{
		return false;
	}

	const FString LeftSource = NormalizedLeft->GetStringField(TEXT("source_kind"));
	const FString RightSource = NormalizedRight->GetStringField(TEXT("source_kind"));
	if (LeftSource != RightSource)
	{
		return false;
	}
	if (NormalizedLeft->GetStringField(TEXT("value")) != NormalizedRight->GetStringField(TEXT("value")))
	{
		return false;
	}
	if (LeftSource == CortexTextSourceStringTable)
	{
		const TSharedPtr<FJsonObject> LeftTable = NormalizedLeft->GetObjectField(TEXT("string_table"));
		const TSharedPtr<FJsonObject> RightTable = NormalizedRight->GetObjectField(TEXT("string_table"));
		return LeftTable->GetStringField(TEXT("table_id")) == RightTable->GetStringField(TEXT("table_id"))
			&& LeftTable->GetStringField(TEXT("key")) == RightTable->GetStringField(TEXT("key"));
	}

	return true;
}

FCortexPropertySerializationResult FCortexSerializer::ObjectToJsonDeep(const UObject* Object, const FCortexSerializationPolicy& Policy)
{
	check(IsInGameThread());

	if (Object == nullptr)
	{
		return MakeJsonObjectResult(MakeShared<FJsonObject>());
	}

	return StructToJsonDeep(Object->GetClass(), Object, Policy);
}

FCortexPropertySerializationResult FCortexSerializer::StructToJsonDeep(const UStruct* StructType, const void* StructData, const FCortexSerializationPolicy& Policy)
{
	check(IsInGameThread());
	return StructToJsonDeepInternal(StructType, StructData, Policy, TEXT(""));
}

FCortexPropertySerializationResult FCortexSerializer::PropertyToJsonDeep(const FProperty* Property, const void* ValuePtr, const FCortexSerializationPolicy& Policy, const FString& FieldPath)
{
	FCortexPropertySerializationResult Result;
	if (Property == nullptr || ValuePtr == nullptr)
	{
		Result.JsonValue = MakeShared<FJsonValueNull>();
		AddIssue(Result, FieldPath, TEXT("INVALID_PROPERTY"), TEXT("Property or value pointer was null"), ECortexSerializationSeverity::Error, true, true);
		return Result;
	}

	if (Policy.MaxDepth < 0)
	{
		Result.JsonValue = MakeShared<FJsonValueNull>();
		AddIssue(Result, FieldPath, TEXT("MAX_DEPTH_EXCEEDED"), TEXT("Maximum serialization depth exceeded"), ECortexSerializationSeverity::Error, true, true);
		return Result;
	}

	if (const FTextProperty* TextProp = CastField<FTextProperty>(Property))
	{
		const FText& TextValue = TextProp->GetPropertyValue(ValuePtr);
		TSharedPtr<FJsonObject> TextObject = FCortexSerializer::TextToJson(TextValue);

		FName TableId;
		FString Key;
		if (Policy.bIncludeTextMetadata
			&& !TextValue.IsEmpty()
			&& !FTextInspector::GetTableIdAndKey(TextValue, TableId, Key))
		{
			AddIssue(
				Result,
				FieldPath,
				TEXT("PARTIAL_TEXT_METADATA"),
				TEXT("FText namespace/key/string-table/source metadata was unavailable; display text was serialized"),
				ECortexSerializationSeverity::Warning,
				true,
				false);
		}

		Result.JsonValue = MakeShared<FJsonValueObject>(TextObject);
		return Result;
	}

	if (CastField<FDelegateProperty>(Property) != nullptr || CastField<FMulticastDelegateProperty>(Property) != nullptr)
	{
		Result.JsonValue = MakeShared<FJsonValueNull>();
		AddIssue(Result, FieldPath, TEXT("UNSUPPORTED_PROPERTY_TYPE"), TEXT("Delegate properties are not serializable"), ECortexSerializationSeverity::Error, true, true);
		return Result;
	}

	if (CastField<FInterfaceProperty>(Property) != nullptr)
	{
		const FScriptInterface* InterfaceValue = static_cast<const FScriptInterface*>(ValuePtr);
		Result.JsonValue = MakeShared<FJsonValueNull>();
		if (InterfaceValue == nullptr || InterfaceValue->GetObject() == nullptr)
		{
			return Result;
		}

		AddIssue(Result, FieldPath, TEXT("UNSUPPORTED_PROPERTY_TYPE"), TEXT("Interface properties are not serializable"), ECortexSerializationSeverity::Error, true, true);
		return Result;
	}

	if (const FStructProperty* StructProp = CastField<FStructProperty>(Property))
	{
		if (StructProp->Struct == FInstancedStruct::StaticStruct())
		{
			const FInstancedStruct* Instance = static_cast<const FInstancedStruct*>(ValuePtr);
			if (!Instance->IsValid())
			{
				Result.JsonValue = MakeShared<FJsonValueNull>();
				return Result;
			}

			FCortexSerializationPolicy NestedPolicy = Policy;
			--NestedPolicy.MaxDepth;
			FCortexPropertySerializationResult Inner = StructToJsonDeepInternal(Instance->GetScriptStruct(), Instance->GetMemory(), NestedPolicy, FieldPath);
			TSharedPtr<FJsonObject> Object = Inner.JsonValue.IsValid() ? Inner.JsonValue->AsObject() : MakeShared<FJsonObject>();
			Object->SetStringField(TEXT("_struct_type"), Instance->GetScriptStruct()->GetName());
			Result.JsonValue = MakeShared<FJsonValueObject>(Object);
			AppendSerializationIssues(Result, Inner);
			return Result;
		}

		if (StructProp->Struct == FGameplayTag::StaticStruct()
			|| StructProp->Struct == FGameplayTagContainer::StaticStruct()
			|| StructProp->Struct == TBaseStructure<FSoftObjectPath>::Get())
		{
			Result.JsonValue = PropertyToJson(Property, ValuePtr);
			return Result;
		}

		FCortexSerializationPolicy NestedPolicy = Policy;
		--NestedPolicy.MaxDepth;
		FCortexPropertySerializationResult Inner = StructToJsonDeepInternal(StructProp->Struct, ValuePtr, NestedPolicy, FieldPath);
		Result.JsonValue = Inner.JsonValue;
		AppendSerializationIssues(Result, Inner);
		return Result;
	}

	if (const FArrayProperty* ArrayProp = CastField<FArrayProperty>(Property))
	{
		FScriptArrayHelper ArrayHelper(ArrayProp, ValuePtr);
		TArray<TSharedPtr<FJsonValue>> JsonArray;
		JsonArray.Reserve(ArrayHelper.Num());
		FCortexSerializationPolicy NestedPolicy = Policy;
		--NestedPolicy.MaxDepth;
		for (int32 Index = 0; Index < ArrayHelper.Num(); ++Index)
		{
			FCortexPropertySerializationResult Element = PropertyToJsonDeep(ArrayProp->Inner, ArrayHelper.GetRawPtr(Index), NestedPolicy, IndexedFieldPath(FieldPath, Index));
			JsonArray.Add(Element.JsonValue.IsValid() ? Element.JsonValue : MakeShared<FJsonValueNull>());
			AppendSerializationIssues(Result, Element);
		}
		Result.JsonValue = MakeShared<FJsonValueArray>(JsonArray);
		return Result;
	}

	if (const FSetProperty* SetProp = CastField<FSetProperty>(Property))
	{
		FScriptSetHelper SetHelper(SetProp, ValuePtr);
		TArray<TSharedPtr<FJsonValue>> JsonArray;
		FCortexSerializationPolicy NestedPolicy = Policy;
		--NestedPolicy.MaxDepth;
		for (int32 SparseIndex = 0; SparseIndex < SetHelper.GetMaxIndex(); ++SparseIndex)
		{
			if (!SetHelper.IsValidIndex(SparseIndex))
			{
				continue;
			}

			FCortexPropertySerializationResult Element = PropertyToJsonDeep(
				SetProp->ElementProp,
				SetHelper.GetElementPtr(SparseIndex),
				NestedPolicy,
				IndexedFieldPath(FieldPath, JsonArray.Num()));
			JsonArray.Add(Element.JsonValue.IsValid() ? Element.JsonValue : MakeShared<FJsonValueNull>());
			AppendSerializationIssues(Result, Element);
		}
		if (JsonArray.Num() > 0)
		{
			AddIssue(Result, FieldPath, TEXT("NON_DETERMINISTIC_SET_ORDER"), TEXT("Set order follows Unreal set iteration order"), ECortexSerializationSeverity::Warning, true, false);
		}
		Result.JsonValue = MakeShared<FJsonValueArray>(JsonArray);
		return Result;
	}

	if (const FMapProperty* MapProp = CastField<FMapProperty>(Property))
	{
		FScriptMapHelper MapHelper(MapProp, ValuePtr);
		struct FPendingMapEntry
		{
			FString KeyString;
			int32 SparseIndex = INDEX_NONE;
		};

		TArray<FPendingMapEntry> Entries;
		const bool bStringLikeKeys = CastField<FStrProperty>(MapProp->KeyProp) != nullptr || CastField<FNameProperty>(MapProp->KeyProp) != nullptr;
		TSet<FString> SeenKeys;
		bool bHasKeyCollision = false;
		FCortexSerializationPolicy NestedPolicy = Policy;
		--NestedPolicy.MaxDepth;

		for (int32 SparseIndex = 0; SparseIndex < MapHelper.GetMaxIndex(); ++SparseIndex)
		{
			if (!MapHelper.IsValidIndex(SparseIndex))
			{
				continue;
			}

			FString KeyString;
			MapProp->KeyProp->ExportTextItem_Direct(KeyString, MapHelper.GetKeyPtr(SparseIndex), nullptr, nullptr, PPF_None);
			bHasKeyCollision = bHasKeyCollision || SeenKeys.Contains(KeyString);
			SeenKeys.Add(KeyString);
			Entries.Add({KeyString, SparseIndex});
		}

		if (bStringLikeKeys && !bHasKeyCollision)
		{
			TSharedPtr<FJsonObject> ObjectMap = MakeShared<FJsonObject>();
			for (const FPendingMapEntry& Entry : Entries)
			{
				FCortexPropertySerializationResult Value = PropertyToJsonDeep(
					MapProp->ValueProp,
					MapHelper.GetValuePtr(Entry.SparseIndex),
					NestedPolicy,
					JoinFieldPath(FieldPath, Entry.KeyString));
				ObjectMap->SetField(Entry.KeyString, Value.JsonValue.IsValid() ? Value.JsonValue : MakeShared<FJsonValueNull>());
				AppendSerializationIssues(Result, Value);
			}
			Result.JsonValue = MakeShared<FJsonValueObject>(ObjectMap);
		}
		else
		{
			TArray<TSharedPtr<FJsonValue>> EntryArray;
			EntryArray.Reserve(Entries.Num());
			for (int32 EntryIndex = 0; EntryIndex < Entries.Num(); ++EntryIndex)
			{
				const FPendingMapEntry& PendingEntry = Entries[EntryIndex];
				FCortexPropertySerializationResult Key = PropertyToJsonDeep(
					MapProp->KeyProp,
					MapHelper.GetKeyPtr(PendingEntry.SparseIndex),
					NestedPolicy,
					IndexedFieldPath(FieldPath, EntryIndex) + TEXT(".key"));
				FCortexPropertySerializationResult Value = PropertyToJsonDeep(
					MapProp->ValueProp,
					MapHelper.GetValuePtr(PendingEntry.SparseIndex),
					NestedPolicy,
					IndexedFieldPath(FieldPath, EntryIndex) + TEXT(".value"));
				TSharedRef<FJsonObject> EntryObject = MakeShared<FJsonObject>();
				EntryObject->SetField(TEXT("key"), Key.JsonValue.IsValid() ? Key.JsonValue : MakeShared<FJsonValueString>(PendingEntry.KeyString));
				EntryObject->SetField(TEXT("value"), Value.JsonValue.IsValid() ? Value.JsonValue : MakeShared<FJsonValueNull>());
				EntryArray.Add(MakeShared<FJsonValueObject>(EntryObject));
				AppendSerializationIssues(Result, Key);
				AppendSerializationIssues(Result, Value);
			}

			TSharedRef<FJsonObject> EntriesObject = MakeShared<FJsonObject>();
			EntriesObject->SetArrayField(TEXT("entries"), EntryArray);
			Result.JsonValue = MakeShared<FJsonValueObject>(EntriesObject);
		}
		return Result;
	}

	if (IsObjectIdentityProperty(Property))
	{
		if (const FObjectProperty* ObjectProperty = CastField<FObjectProperty>(Property))
		{
			const UObject* Object = ObjectProperty->GetObjectPropertyValue(ValuePtr);
			if (Object != nullptr
				&& Policy.bExpandInstancedSubobjects
				&& Property->HasAllPropertyFlags(CPF_InstancedReference))
			{
				TSharedRef<FJsonObject> SubObject = MakeShared<FJsonObject>();
				SubObject->SetStringField(TEXT("_class"), Object->GetClass()->GetName());

				FCortexSerializationPolicy NestedPolicy = Policy;
				--NestedPolicy.MaxDepth;
				const FCortexPropertySerializationResult Properties = StructToJsonDeepInternal(
					Object->GetClass(),
					Object,
					NestedPolicy,
					FieldPath);
				if (Properties.JsonValue.IsValid() && Properties.JsonValue->AsObject()->Values.Num() > 0)
				{
					SubObject->SetObjectField(TEXT("properties"), Properties.JsonValue->AsObject());
				}
				Result.JsonValue = MakeShared<FJsonValueObject>(SubObject);
				AppendSerializationIssues(Result, Properties);
				return Result;
			}
		}

		Result.JsonValue = ObjectIdentityToJson(Property, ValuePtr);
		if (HasObjectIdentityValue(Property, ValuePtr)
			&& CastField<FSoftObjectProperty>(Property) == nullptr
			&& CastField<FSoftClassProperty>(Property) == nullptr)
		{
			AddIssue(Result, FieldPath, TEXT("OBJECT_REFERENCE_IDENTITY_ONLY"), TEXT("Object reference serialized as identity only"), ECortexSerializationSeverity::Warning, true, false);
		}
		return Result;
	}

	Result.JsonValue = PropertyToJson(Property, ValuePtr);
	if (!Result.JsonValue.IsValid() || Result.JsonValue->IsNull())
	{
		AddIssue(
			Result,
			FieldPath,
			TEXT("UNSUPPORTED_PROPERTY_TYPE"),
			FString::Printf(TEXT("Unsupported property type: %s"), *Property->GetClass()->GetName()),
			ECortexSerializationSeverity::Error,
			true,
			true);
	}
	return Result;
}

TArray<TSharedPtr<FJsonValue>> FCortexSerializer::SerializationIssuesToJson(const TArray<FCortexSerializationIssue>& Issues)
{
	TArray<TSharedPtr<FJsonValue>> JsonIssues;
	JsonIssues.Reserve(Issues.Num());
	for (const FCortexSerializationIssue& Issue : Issues)
	{
		TSharedRef<FJsonObject> IssueObject = MakeShared<FJsonObject>();
		IssueObject->SetStringField(TEXT("field"), Issue.Field);
		IssueObject->SetStringField(TEXT("issue"), Issue.Issue);
		IssueObject->SetStringField(TEXT("code"), Issue.Code);
		IssueObject->SetStringField(TEXT("severity"), Issue.Severity == ECortexSerializationSeverity::Error ? TEXT("error") : TEXT("warning"));
		IssueObject->SetBoolField(TEXT("degraded"), Issue.bDegraded);
		IssueObject->SetBoolField(TEXT("omitted"), Issue.bOmitted);
		JsonIssues.Add(MakeShared<FJsonValueObject>(IssueObject));
	}
	return JsonIssues;
}

namespace
{
	FCortexPropertySerializationResult StructToJsonDeepInternal(
		const UStruct* StructType,
		const void* StructData,
		const FCortexSerializationPolicy& Policy,
		const FString& BasePath)
	{
		check(IsInGameThread());

		TSharedPtr<FJsonObject> JsonObject = MakeShared<FJsonObject>();
		FCortexPropertySerializationResult Result = MakeJsonObjectResult(JsonObject);
		if (StructType == nullptr || StructData == nullptr)
		{
			return Result;
		}

		if (StructType == FInstancedStruct::StaticStruct())
		{
			const FInstancedStruct* Instance = static_cast<const FInstancedStruct*>(StructData);
			if (Instance == nullptr || !Instance->IsValid())
			{
				return Result;
			}

			FCortexSerializationPolicy NestedPolicy = Policy;
			--NestedPolicy.MaxDepth;
			FCortexPropertySerializationResult Inner = StructToJsonDeepInternal(
				Instance->GetScriptStruct(),
				Instance->GetMemory(),
				NestedPolicy,
				BasePath);
			TSharedPtr<FJsonObject> Object = Inner.JsonValue.IsValid() ? Inner.JsonValue->AsObject() : MakeShared<FJsonObject>();
			Object->SetStringField(TEXT("_struct_type"), Instance->GetScriptStruct()->GetName());
			Inner.JsonValue = MakeShared<FJsonValueObject>(Object);
			return Inner;
		}

		for (TFieldIterator<FProperty> It(StructType); It; ++It)
		{
			const FProperty* Property = *It;
			if (!Policy.ShouldAdmitProperty(Property))
			{
				continue;
			}

			const FString FieldName = Property->GetName();
			const FString Path = JoinFieldPath(BasePath, FieldName);
			const void* ValuePtr = Property->ContainerPtrToValuePtr<void>(StructData);
			FCortexPropertySerializationResult PropertyResult = FCortexSerializer::PropertyToJsonDeep(Property, ValuePtr, Policy, Path);
			if (PropertyResult.JsonValue.IsValid())
			{
				JsonObject->SetField(FieldName, PropertyResult.JsonValue);
			}
			AppendSerializationIssues(Result, PropertyResult);
		}

		return Result;
	}
}

TSharedPtr<FJsonObject> FCortexSerializer::StructToJson(const UStruct* StructType, const void* StructData)
{
	TSharedPtr<FJsonObject> JsonObject = MakeShared<FJsonObject>();

	if (StructType == nullptr || StructData == nullptr)
	{
		return JsonObject;
	}

	// Special case: if the top-level struct IS an FInstancedStruct, unwrap it
	if (StructType == FInstancedStruct::StaticStruct())
	{
		const FInstancedStruct* Instance = static_cast<const FInstancedStruct*>(StructData);
		if (Instance->IsValid())
		{
			JsonObject = StructToJson(Instance->GetScriptStruct(), Instance->GetMemory());
			JsonObject->SetStringField(TEXT("_struct_type"), Instance->GetScriptStruct()->GetName());
		}
		return JsonObject;
	}

	for (TFieldIterator<FProperty> It(StructType); It; ++It)
	{
		const FProperty* Property = *It;
		const void* ValuePtr = Property->ContainerPtrToValuePtr<void>(StructData);

		TSharedPtr<FJsonValue> JsonValue = PropertyToJson(Property, ValuePtr);
		if (JsonValue.IsValid())
		{
			JsonObject->SetField(Property->GetName(), JsonValue);
		}
	}

	return JsonObject;
}

TSharedPtr<FJsonObject> FCortexSerializer::StructToJson(const UStruct* StructType, const void* StructData, const TSet<FString>& FieldFilter)
{
	if (FieldFilter.Num() == 0)
	{
		return StructToJson(StructType, StructData);
	}

	TSharedPtr<FJsonObject> JsonObject = MakeShared<FJsonObject>();

	if (StructType == nullptr || StructData == nullptr)
	{
		return JsonObject;
	}

	for (TFieldIterator<FProperty> It(StructType); It; ++It)
	{
		const FProperty* Property = *It;
		if (!FieldFilter.Contains(Property->GetName()))
		{
			continue;
		}

		const void* ValuePtr = Property->ContainerPtrToValuePtr<void>(StructData);

		TSharedPtr<FJsonValue> JsonValue = PropertyToJson(Property, ValuePtr);
		if (JsonValue.IsValid())
		{
			JsonObject->SetField(Property->GetName(), JsonValue);
		}
	}

	return JsonObject;
}

TSharedPtr<FJsonObject> FCortexSerializer::NonDefaultPropertiesToJson(const UObject* Object, int32 MaxDepth)
{
	check(IsInGameThread());

	if (Object == nullptr)
	{
		return MakeShared<FJsonObject>();
	}

	const UObject* DefaultObject = Object->GetClass()->GetDefaultObject();
	if (DefaultObject == nullptr)
	{
		return MakeShared<FJsonObject>();
	}

	return ObjectNonDefaultPropertiesToJson(Object, DefaultObject, MaxDepth);
}

TSharedPtr<FJsonValue> FCortexSerializer::PropertyToJson(const FProperty* Property, const void* ValuePtr)
{
	if (Property == nullptr || ValuePtr == nullptr)
	{
		return nullptr;
	}

	// Bool
	if (const FBoolProperty* BoolProp = CastField<FBoolProperty>(Property))
	{
		return MakeShared<FJsonValueBoolean>(BoolProp->GetPropertyValue(ValuePtr));
	}

	// Int
	if (const FIntProperty* IntProp = CastField<FIntProperty>(Property))
	{
		return MakeShared<FJsonValueNumber>(static_cast<double>(IntProp->GetPropertyValue(ValuePtr)));
	}

	// Int64
	if (const FInt64Property* Int64Prop = CastField<FInt64Property>(Property))
	{
		return MakeShared<FJsonValueNumber>(static_cast<double>(Int64Prop->GetPropertyValue(ValuePtr)));
	}

	// Float
	if (const FFloatProperty* FloatProp = CastField<FFloatProperty>(Property))
	{
		return MakeShared<FJsonValueNumber>(static_cast<double>(FloatProp->GetPropertyValue(ValuePtr)));
	}

	// Double
	if (const FDoubleProperty* DoubleProp = CastField<FDoubleProperty>(Property))
	{
		return MakeShared<FJsonValueNumber>(DoubleProp->GetPropertyValue(ValuePtr));
	}

	// FString
	if (const FStrProperty* StrProp = CastField<FStrProperty>(Property))
	{
		return MakeShared<FJsonValueString>(StrProp->GetPropertyValue(ValuePtr));
	}

	// FName
	if (const FNameProperty* NameProp = CastField<FNameProperty>(Property))
	{
		return MakeShared<FJsonValueString>(NameProp->GetPropertyValue(ValuePtr).ToString());
	}

	// FText
	if (const FTextProperty* TextProp = CastField<FTextProperty>(Property))
	{
		const FText& TextValue = TextProp->GetPropertyValue(ValuePtr);
		return MakeShared<FJsonValueObject>(TextToJson(TextValue));
	}

	// Enum property (enum class)
	if (const FEnumProperty* EnumProp = CastField<FEnumProperty>(Property))
	{
		const UEnum* Enum = EnumProp->GetEnum();
		const FNumericProperty* UnderlyingProp = EnumProp->GetUnderlyingProperty();
		int64 Value = UnderlyingProp->GetSignedIntPropertyValue(ValuePtr);
		FString EnumName = Enum->GetNameStringByIndex(static_cast<int32>(Value));
		return MakeShared<FJsonValueString>(EnumName);
	}

	// Byte property with enum (old-style TEnumAsByte)
	if (const FByteProperty* ByteProp = CastField<FByteProperty>(Property))
	{
		if (const UEnum* Enum = ByteProp->GetIntPropertyEnum())
		{
			int64 Value = static_cast<int64>(ByteProp->GetPropertyValue(ValuePtr));
			FString EnumName = Enum->GetNameStringByIndex(static_cast<int32>(Value));
			return MakeShared<FJsonValueString>(EnumName);
		}
		return MakeShared<FJsonValueNumber>(static_cast<double>(ByteProp->GetPropertyValue(ValuePtr)));
	}

	// Struct property
	if (const FStructProperty* StructProp = CastField<FStructProperty>(Property))
	{
		// FGameplayTag - serialize as tag string
		if (StructProp->Struct == FGameplayTag::StaticStruct())
		{
			const FGameplayTag* Tag = static_cast<const FGameplayTag*>(ValuePtr);
			return MakeShared<FJsonValueString>(Tag->ToString());
		}

		// FGameplayTagContainer - serialize as array of tag strings
		if (StructProp->Struct == FGameplayTagContainer::StaticStruct())
		{
			const FGameplayTagContainer* Container = static_cast<const FGameplayTagContainer*>(ValuePtr);
			TArray<TSharedPtr<FJsonValue>> TagArray;
			for (const FGameplayTag& Tag : *Container)
			{
				TagArray.Add(MakeShared<FJsonValueString>(Tag.ToString()));
			}
			return MakeShared<FJsonValueArray>(TagArray);
		}

		// FInstancedStruct - serialize with _struct_type discriminator
		if (StructProp->Struct == FInstancedStruct::StaticStruct())
		{
			const FInstancedStruct* Instance = static_cast<const FInstancedStruct*>(ValuePtr);
			if (Instance->IsValid())
			{
				TSharedPtr<FJsonObject> Obj = StructToJson(Instance->GetScriptStruct(), Instance->GetMemory());
				Obj->SetStringField(TEXT("_struct_type"), Instance->GetScriptStruct()->GetName());
				return MakeShared<FJsonValueObject>(Obj);
			}
			return MakeShared<FJsonValueNull>();
		}

		// FSoftObjectPath - serialize as string path
		if (StructProp->Struct == TBaseStructure<FSoftObjectPath>::Get())
		{
			const FSoftObjectPath* SoftPath = static_cast<const FSoftObjectPath*>(ValuePtr);
			return MakeShared<FJsonValueString>(SoftPath->ToString());
		}

		// Default: recursive struct serialization
		TSharedPtr<FJsonObject> NestedObj = StructToJson(StructProp->Struct, ValuePtr);
		return MakeShared<FJsonValueObject>(NestedObj);
	}

	// Array property
	if (const FArrayProperty* ArrayProp = CastField<FArrayProperty>(Property))
	{
		FScriptArrayHelper ArrayHelper(ArrayProp, ValuePtr);
		TArray<TSharedPtr<FJsonValue>> JsonArray;
		JsonArray.Reserve(ArrayHelper.Num());

		for (int32 Index = 0; Index < ArrayHelper.Num(); ++Index)
		{
			const void* ElementPtr = ArrayHelper.GetRawPtr(Index);
			TSharedPtr<FJsonValue> ElementValue = PropertyToJson(ArrayProp->Inner, ElementPtr);
			if (ElementValue.IsValid())
			{
				JsonArray.Add(ElementValue);
			}
		}

		return MakeShared<FJsonValueArray>(JsonArray);
	}

	// Map property
	if (const FMapProperty* MapProp = CastField<FMapProperty>(Property))
	{
		FScriptMapHelper MapHelper(MapProp, ValuePtr);
		TSharedPtr<FJsonObject> MapObj = MakeShared<FJsonObject>();

		for (int32 Index = 0; Index < MapHelper.GetMaxIndex(); ++Index)
		{
			if (!MapHelper.IsValidIndex(Index))
			{
				continue;
			}

			// Get key as string
			const void* KeyPtr = MapHelper.GetKeyPtr(Index);
			FString KeyString;
			MapProp->KeyProp->ExportTextItem_Direct(KeyString, KeyPtr, nullptr, nullptr, PPF_None);

			// Get value
			const void* MapValuePtr = MapHelper.GetValuePtr(Index);
			TSharedPtr<FJsonValue> JsonValue = PropertyToJson(MapProp->ValueProp, MapValuePtr);
			if (JsonValue.IsValid())
			{
				MapObj->SetField(KeyString, JsonValue);
			}
		}

		return MakeShared<FJsonValueObject>(MapObj);
	}

	// Set property
	if (const FSetProperty* SetProp = CastField<FSetProperty>(Property))
	{
		FScriptSetHelper SetHelper(SetProp, ValuePtr);
		TArray<TSharedPtr<FJsonValue>> JsonArray;

		for (int32 Index = 0; Index < SetHelper.GetMaxIndex(); ++Index)
		{
			if (!SetHelper.IsValidIndex(Index))
			{
				continue;
			}

			const void* ElementPtr = SetHelper.GetElementPtr(Index);
			TSharedPtr<FJsonValue> ElementValue = PropertyToJson(SetProp->ElementProp, ElementPtr);
			if (ElementValue.IsValid())
			{
				JsonArray.Add(ElementValue);
			}
		}

		return MakeShared<FJsonValueArray>(JsonArray);
	}

	// Object property (hard reference)
	if (const FObjectProperty* ObjProp = CastField<FObjectProperty>(Property))
	{
		const UObject* Object = ObjProp->GetObjectPropertyValue(ValuePtr);
		if (Object != nullptr)
		{
			// Instanced sub-object: serialize as {"_class": "...", "properties": {...}}
			if (Property->HasAllPropertyFlags(CPF_InstancedReference))
			{
				TSharedPtr<FJsonObject> SubObj = MakeShared<FJsonObject>();
				SubObj->SetStringField(TEXT("_class"), Object->GetClass()->GetName());
				TSharedPtr<FJsonObject> Props = StructToJson(Object->GetClass(), static_cast<const void*>(Object));
				if (Props.IsValid() && Props->Values.Num() > 0)
				{
					SubObj->SetObjectField(TEXT("properties"), Props);
				}
				return MakeShared<FJsonValueObject>(SubObj);
			}

			return MakeShared<FJsonValueString>(Object->GetPathName());
		}
		return MakeShared<FJsonValueNull>();
	}

	// Soft object property
	if (const FSoftObjectProperty* SoftObjProp = CastField<FSoftObjectProperty>(Property))
	{
		const FSoftObjectPtr& SoftPtr = SoftObjProp->GetPropertyValue(ValuePtr);
		return MakeShared<FJsonValueString>(SoftPtr.ToSoftObjectPath().ToString());
	}

	UE_LOG(LogCortex, Verbose, TEXT("Unhandled property type: %s (%s)"),
		*Property->GetName(), *Property->GetClass()->GetName());
	return MakeShared<FJsonValueNull>();
}

bool FCortexSerializer::JsonToStruct(const TSharedPtr<FJsonObject>& JsonObject, const UStruct* StructType, void* StructData, UObject* Outer, TArray<FString>& OutWarnings)
{
	if (!JsonObject.IsValid() || StructType == nullptr || StructData == nullptr)
	{
		return false;
	}

	bool bSuccess = true;
	for (const auto& Pair : JsonObject->Values)
	{
		const FString FieldName(Pair.Key.ToView());
		const TSharedPtr<FJsonValue>& JsonValue = Pair.Value;

		// Skip internal metadata fields
		if (FieldName.StartsWith(TEXT("_")))
		{
			continue;
		}

		// Find the matching property
		const FProperty* Property = StructType->FindPropertyByName(FName(*FieldName));
		if (Property == nullptr)
		{
			OutWarnings.Add(FString::Printf(TEXT("Unknown field '%s' in struct '%s'"), *FieldName, *StructType->GetName()));
			continue;
		}

		void* ValuePtr = Property->ContainerPtrToValuePtr<void>(StructData);
		if (!JsonToProperty(JsonValue, Property, ValuePtr, Outer, OutWarnings))
		{
			OutWarnings.Add(FString::Printf(TEXT("Failed to deserialize field '%s'"), *FieldName));
			bSuccess = false;
		}
	}

	return bSuccess;
}

bool FCortexSerializer::JsonToProperty(const TSharedPtr<FJsonValue>& JsonValue, const FProperty* Property, void* ValuePtr, UObject* Outer, TArray<FString>& OutWarnings)
{
	if (!JsonValue.IsValid() || Property == nullptr || ValuePtr == nullptr)
	{
		return false;
	}

	// Handle null JSON values
	if (JsonValue->IsNull())
	{
		// For object properties, set to nullptr
		if (const FObjectProperty* ObjProp = CastField<FObjectProperty>(Property))
		{
			// Clean up existing instanced sub-object before nulling
			if (Property->HasAllPropertyFlags(CPF_InstancedReference))
			{
				UObject* Existing = ObjProp->GetObjectPropertyValue(ValuePtr);
				if (Existing)
				{
					Existing->MarkAsGarbage();
				}
			}
			ObjProp->SetObjectPropertyValue(ValuePtr, nullptr);
			return true;
		}
		return false;
	}

	// Bool
	if (const FBoolProperty* BoolProp = CastField<FBoolProperty>(Property))
	{
		BoolProp->SetPropertyValue(ValuePtr, JsonValue->AsBool());
		return true;
	}

	// Int
	if (const FIntProperty* IntProp = CastField<FIntProperty>(Property))
	{
		IntProp->SetPropertyValue(ValuePtr, static_cast<int32>(JsonValue->AsNumber()));
		return true;
	}

	// Int64
	if (const FInt64Property* Int64Prop = CastField<FInt64Property>(Property))
	{
		Int64Prop->SetPropertyValue(ValuePtr, static_cast<int64>(JsonValue->AsNumber()));
		return true;
	}

	// Float
	if (const FFloatProperty* FloatProp = CastField<FFloatProperty>(Property))
	{
		FloatProp->SetPropertyValue(ValuePtr, static_cast<float>(JsonValue->AsNumber()));
		return true;
	}

	// Double
	if (const FDoubleProperty* DoubleProp = CastField<FDoubleProperty>(Property))
	{
		DoubleProp->SetPropertyValue(ValuePtr, JsonValue->AsNumber());
		return true;
	}

	// FString
	if (const FStrProperty* StrProp = CastField<FStrProperty>(Property))
	{
		StrProp->SetPropertyValue(ValuePtr, JsonValue->AsString());
		return true;
	}

	// FName
	if (const FNameProperty* NameProp = CastField<FNameProperty>(Property))
	{
		NameProp->SetPropertyValue(ValuePtr, FName(*JsonValue->AsString()));
		return true;
	}

	// FText
	if (const FTextProperty* TextProp = CastField<FTextProperty>(Property))
	{
		FText TextValue;
		if (!TextFromJson(JsonValue, TextValue, OutWarnings))
		{
			return false;
		}
		TextProp->SetPropertyValue(ValuePtr, TextValue);
		return true;
	}

	// Enum property (enum class)
	if (const FEnumProperty* EnumProp = CastField<FEnumProperty>(Property))
	{
		const UEnum* Enum = EnumProp->GetEnum();
		int64 EnumValue = INDEX_NONE;
		FString InputValue;

		if (JsonValue->Type == EJson::Number)
		{
			EnumValue = static_cast<int64>(JsonValue->AsNumber());
			InputValue = FString::Printf(TEXT("%lld"), static_cast<long long>(EnumValue));
		}
		else
		{
			InputValue = JsonValue->AsString();
			EnumValue = Enum->GetValueByNameString(InputValue);
		}

		if (EnumValue == INDEX_NONE)
		{
			TArray<FString> ValidValues;
			for (int32 Index = 0; Index < Enum->NumEnums() - 1; ++Index)
			{
				ValidValues.Add(Enum->GetNameStringByIndex(Index));
			}

			OutWarnings.Add(FString::Printf(
				TEXT("Unknown enum value '%s' for %s. Valid: %s"),
				*InputValue, *Enum->GetName(), *FString::Join(ValidValues, TEXT(", "))));
			return false;
		}
		FNumericProperty* UnderlyingProp = EnumProp->GetUnderlyingProperty();
		UnderlyingProp->SetIntPropertyValue(ValuePtr, EnumValue);
		return true;
	}

	// Byte property with enum (old-style TEnumAsByte)
	if (const FByteProperty* ByteProp = CastField<FByteProperty>(Property))
	{
		if (const UEnum* Enum = ByteProp->GetIntPropertyEnum())
		{
			int64 EnumValue = INDEX_NONE;
			FString InputValue;

			if (JsonValue->Type == EJson::Number)
			{
				EnumValue = static_cast<int64>(JsonValue->AsNumber());
				InputValue = FString::Printf(TEXT("%lld"), static_cast<long long>(EnumValue));
			}
			else
			{
				InputValue = JsonValue->AsString();
				EnumValue = Enum->GetValueByNameString(InputValue);
			}

			if (EnumValue == INDEX_NONE)
			{
				TArray<FString> ValidValues;
				for (int32 Index = 0; Index < Enum->NumEnums() - 1; ++Index)
				{
					ValidValues.Add(Enum->GetNameStringByIndex(Index));
				}

				OutWarnings.Add(FString::Printf(
					TEXT("Unknown enum value '%s' for %s. Valid: %s"),
					*InputValue, *Enum->GetName(), *FString::Join(ValidValues, TEXT(", "))));
				return false;
			}
			ByteProp->SetPropertyValue(ValuePtr, static_cast<uint8>(EnumValue));
			return true;
		}
		ByteProp->SetPropertyValue(ValuePtr, static_cast<uint8>(JsonValue->AsNumber()));
		return true;
	}

	// Struct property
	if (const FStructProperty* StructProp = CastField<FStructProperty>(Property))
	{
		// ── Array-to-struct promotion ────────────────────────────────────────────
		// Accepts [v0, v1, v2, v3] for numeric structs like FLinearColor and FVector.
		// Positional order matches C++ declaration order (R,G,B,A for FLinearColor).
		// FColor is excluded: its memory layout is B,G,R,A — see IsPositionalNumericStruct.
		{
			const TArray<TSharedPtr<FJsonValue>>* JsonArray = nullptr;
			if (JsonValue->TryGetArray(JsonArray)
				&& JsonArray != nullptr
				&& IsPositionalNumericStruct(StructProp->Struct))
			{
				TArray<const FProperty*> Fields;
				// ExcludeSuper: positional contract is defined for the struct's own declared fields only.
				// A struct with inherited numeric fields would pass this check but the promotion loop
				// would only write the child's own fields, silently leaving parent fields at defaults.
				// In practice, all UE math structs (FLinearColor, FVector, etc.) have no numeric parents.
				for (TFieldIterator<FProperty> It(StructProp->Struct, EFieldIteratorFlags::ExcludeSuper); It; ++It)
				{
					Fields.Add(*It);
				}

				if (JsonArray->Num() != Fields.Num())
				{
					OutWarnings.Add(FString::Printf(
						TEXT("Array-to-struct: property '%s' (struct '%s') needs %d elements, got %d"),
						*Property->GetName(),
						*StructProp->Struct->GetName(),
						Fields.Num(),
						JsonArray->Num()));
					return false;
				}

				bool bSuccess = true;
				for (int32 i = 0; i < Fields.Num(); ++i)
				{
					void* FieldPtr = Fields[i]->ContainerPtrToValuePtr<void>(ValuePtr);
					if (!JsonToProperty((*JsonArray)[i], Fields[i], FieldPtr, Outer, OutWarnings))
					{
						bSuccess = false;
					}
				}
				return bSuccess;
			}
		}
		// ── End array-to-struct promotion ───────────────────────────────────────

		// FGameplayTag - deserialize from tag string
		if (StructProp->Struct == FGameplayTag::StaticStruct())
		{
			const FString TagString = JsonValue->AsString();
			FGameplayTag* Tag = static_cast<FGameplayTag*>(ValuePtr);
			*Tag = FGameplayTag::RequestGameplayTag(FName(*TagString), false);
			return true;
		}

		// FGameplayTagContainer - deserialize from array of tag strings
		if (StructProp->Struct == FGameplayTagContainer::StaticStruct())
		{
			const TArray<TSharedPtr<FJsonValue>>* JsonArray = nullptr;
			if (!JsonValue->TryGetArray(JsonArray) || JsonArray == nullptr)
			{
				OutWarnings.Add(TEXT("Expected array for FGameplayTagContainer"));
				return false;
			}
			FGameplayTagContainer* Container = static_cast<FGameplayTagContainer*>(ValuePtr);
			Container->Reset();
			for (const TSharedPtr<FJsonValue>& Element : *JsonArray)
			{
				if (Element.IsValid())
				{
					FGameplayTag Tag = FGameplayTag::RequestGameplayTag(FName(*Element->AsString()), false);
					Container->AddTag(Tag);
				}
			}
			return true;
		}

		// FInstancedStruct - deserialize with _struct_type discriminator
		if (StructProp->Struct == FInstancedStruct::StaticStruct())
		{
			const TSharedPtr<FJsonObject>* InnerObj = nullptr;
			if (!JsonValue->TryGetObject(InnerObj) || InnerObj == nullptr || !(*InnerObj).IsValid())
			{
				OutWarnings.Add(TEXT("Expected object for FInstancedStruct"));
				return false;
			}

			FString StructTypeName;
			if (!(*InnerObj)->TryGetStringField(TEXT("_struct_type"), StructTypeName) || StructTypeName.IsEmpty())
			{
				OutWarnings.Add(TEXT("FInstancedStruct missing '_struct_type' field"));
				return false;
			}

			// Find the UScriptStruct by name (O(1) hash lookup)
			UScriptStruct* FoundStruct = FindFirstObjectSafe<UScriptStruct>(*StructTypeName, EFindFirstObjectOptions::NativeFirst);

			if (FoundStruct == nullptr)
			{
				OutWarnings.Add(FString::Printf(TEXT("Could not find struct type '%s' for FInstancedStruct"), *StructTypeName));
				return false;
			}

			FInstancedStruct* Instance = static_cast<FInstancedStruct*>(ValuePtr);
			Instance->InitializeAs(FoundStruct);
			return JsonToStruct(*InnerObj, FoundStruct, Instance->GetMutableMemory(), Outer, OutWarnings);
		}

		// FSoftObjectPath - deserialize from string path
		if (StructProp->Struct == TBaseStructure<FSoftObjectPath>::Get())
		{
			FSoftObjectPath* SoftPath = static_cast<FSoftObjectPath*>(ValuePtr);
			SoftPath->SetPath(JsonValue->AsString());
			return true;
		}

		// Default: recursive struct deserialization from JSON object
		const TSharedPtr<FJsonObject>* NestedObj = nullptr;
		if (!JsonValue->TryGetObject(NestedObj) || NestedObj == nullptr || !(*NestedObj).IsValid())
		{
			OutWarnings.Add(FString::Printf(TEXT("Expected object for struct property '%s'"), *Property->GetName()));
			return false;
		}
		return JsonToStruct(*NestedObj, StructProp->Struct, ValuePtr, Outer, OutWarnings);
	}

	// Array property
	if (const FArrayProperty* ArrayProp = CastField<FArrayProperty>(Property))
	{
		const TArray<TSharedPtr<FJsonValue>>* JsonArray = nullptr;
		if (!JsonValue->TryGetArray(JsonArray) || JsonArray == nullptr)
		{
			OutWarnings.Add(FString::Printf(TEXT("Expected array for property '%s'"), *Property->GetName()));
			return false;
		}

		FScriptArrayHelper ArrayHelper(ArrayProp, ValuePtr);

		// Clean up existing instanced sub-objects before resize
		if (ArrayProp->Inner->HasAllPropertyFlags(CPF_InstancedReference))
		{
			if (const FObjectProperty* InnerObjProp = CastField<FObjectProperty>(ArrayProp->Inner))
			{
				for (int32 i = 0; i < ArrayHelper.Num(); ++i)
				{
					UObject* Existing = InnerObjProp->GetObjectPropertyValue(ArrayHelper.GetRawPtr(i));
					if (Existing)
					{
						Existing->MarkAsGarbage();
					}
				}
			}
		}

		const int32 OriginalNum = ArrayHelper.Num();
		ArrayHelper.Resize(JsonArray->Num());
		bool bSuccess = true;
		for (int32 Index = 0; Index < JsonArray->Num(); ++Index)
		{
			if (!JsonToProperty((*JsonArray)[Index], ArrayProp->Inner, ArrayHelper.GetRawPtr(Index), Outer, OutWarnings))
			{
				OutWarnings.Add(FString::Printf(
					TEXT("Failed to deserialize element %d of array property '%s'"),
					Index,
					*Property->GetName()));
				bSuccess = false;
			}
		}
		if (!bSuccess)
		{
			ArrayHelper.Resize(OriginalNum);
		}
		return bSuccess;
	}

	// Map property
	if (const FMapProperty* MapProp = CastField<FMapProperty>(Property))
	{
		const TSharedPtr<FJsonObject>* MapObj = nullptr;
		if (!JsonValue->TryGetObject(MapObj) || MapObj == nullptr || !(*MapObj).IsValid())
		{
			OutWarnings.Add(FString::Printf(TEXT("Expected object for map property '%s'"), *Property->GetName()));
			return false;
		}

		FScriptMapHelper MapHelper(MapProp, ValuePtr);

		// Clean up existing instanced sub-objects before clearing map
		if (MapProp->ValueProp->HasAllPropertyFlags(CPF_InstancedReference))
		{
			if (const FObjectProperty* ValueObjProp = CastField<FObjectProperty>(MapProp->ValueProp))
			{
				for (int32 i = 0; i < MapHelper.GetMaxIndex(); ++i)
				{
					if (MapHelper.IsValidIndex(i))
					{
						UObject* Existing = ValueObjProp->GetObjectPropertyValue(MapHelper.GetValuePtr(i));
						if (Existing)
						{
							Existing->MarkAsGarbage();
						}
					}
				}
			}
		}

		MapHelper.EmptyValues();

		for (const auto& MapPair : (*MapObj)->Values)
		{
			int32 NewIndex = MapHelper.AddDefaultValue_Invalid_NeedsRehash();

			// Import key from string
			void* KeyPtr = MapHelper.GetKeyPtr(NewIndex);
			MapProp->KeyProp->ImportText_Direct(*MapPair.Key, KeyPtr, nullptr, PPF_None);

			// Import value
			void* MapValuePtr = MapHelper.GetValuePtr(NewIndex);
			JsonToProperty(MapPair.Value, MapProp->ValueProp, MapValuePtr, Outer, OutWarnings);
		}

		MapHelper.Rehash();
		return true;
	}

	// Soft object property
	if (CastField<FSoftObjectProperty>(Property) != nullptr)
	{
		const FString ObjectPath = JsonValue->AsString();

		if (!ObjectPath.IsEmpty())
		{
			// Fast path: object already loaded (engine CDOs, /Script/ objects).
			UObject* FoundObject = StaticFindObject(UObject::StaticClass(), nullptr, *ObjectPath, false);
			if (FoundObject == nullptr)
			{
				// Guard against SkipPackage warnings for non-existent or invalid packages.
				const FString PackageName = FPackageName::ObjectPathToPackageName(ObjectPath);
				if (!FPackageName::IsValidLongPackageName(PackageName)
					|| (!FindPackage(nullptr, *PackageName) && !FPackageName::DoesPackageExist(PackageName)))
				{
					OutWarnings.Add(FString::Printf(TEXT("Package not found for soft object '%s'"), *ObjectPath));
					return false;
				}
			}
		}

		FSoftObjectPtr* SoftPtr = reinterpret_cast<FSoftObjectPtr*>(ValuePtr);
		*SoftPtr = FSoftObjectPath(ObjectPath);
		return true;
	}

	// Object property — instanced sub-object path
	if (const FObjectProperty* ObjProp = CastField<FObjectProperty>(Property))
	{
		if (Property->HasAllPropertyFlags(CPF_InstancedReference) && JsonValue->Type == EJson::Object)
		{
			return JsonToInstancedSubObject(JsonValue, ObjProp, ValuePtr, Outer, OutWarnings);
		}

		// Existing asset-reference path (unchanged)
		const FString ObjectPath = JsonValue->AsString();
		if (ObjectPath.IsEmpty())
		{
			ObjProp->SetObjectPropertyValue(ValuePtr, nullptr);
			return true;
		}

		// Fast path: object already loaded (engine CDOs, /Script/ objects)
		UObject* ExistingObject = StaticFindObject(ObjProp->PropertyClass, nullptr, *ObjectPath, false);
		if (ExistingObject)
		{
			ObjProp->SetObjectPropertyValue(ValuePtr, ExistingObject);
			return true;
		}

		// Guard against SkipPackage warnings for non-existent or invalid packages.
		// Check IsValidLongPackageName first to skip DoesPackageExist for non-path values
		// (e.g., bare numbers like "123" from type-mismatched JSON).
		const FString PkgName = FPackageName::ObjectPathToPackageName(ObjectPath);
		if (!FPackageName::IsValidLongPackageName(PkgName)
			|| (!FindPackage(nullptr, *PkgName) && !FPackageName::DoesPackageExist(PkgName)))
		{
			OutWarnings.Add(FString::Printf(TEXT("Package not found for object '%s'"), *ObjectPath));
			return false;
		}

		UObject* LoadedObject = StaticLoadObject(ObjProp->PropertyClass, nullptr, *ObjectPath);
		if (LoadedObject == nullptr)
		{
			OutWarnings.Add(FString::Printf(TEXT("Failed to load object '%s' for property '%s'"), *ObjectPath, *Property->GetName()));
			return false;
		}
		ObjProp->SetObjectPropertyValue(ValuePtr, LoadedObject);
		return true;
	}

	UE_LOG(LogCortex, Verbose, TEXT("Unhandled property type for deserialization: %s (%s)"),
		*Property->GetName(), *Property->GetClass()->GetName());
	return false;
}

bool FCortexSerializer::JsonToInstancedSubObject(const TSharedPtr<FJsonValue>& JsonValue, const FObjectProperty* ObjProp, void* ValuePtr, UObject* Outer, TArray<FString>& OutWarnings)
{
	const TSharedPtr<FJsonObject>* JsonObj = nullptr;
	if (!JsonValue->TryGetObject(JsonObj) || !JsonObj || !(*JsonObj).IsValid())
	{
		OutWarnings.Add(TEXT("Expected JSON object for instanced sub-object"));
		return false;
	}

	// Extract _class discriminator
	FString ClassName;
	if (!(*JsonObj)->TryGetStringField(TEXT("_class"), ClassName) || ClassName.IsEmpty())
	{
		OutWarnings.Add(TEXT("Instanced sub-object missing '_class' field"));
		return false;
	}

	// Resolve class: qualified path first, then short name fallback
	UClass* ResolvedClass = nullptr;
	if (ClassName.Contains(TEXT(".")) || ClassName.Contains(TEXT("/")))
	{
		ResolvedClass = Cast<UClass>(StaticFindObject(UClass::StaticClass(), nullptr, *ClassName, false));
	}

	if (ResolvedClass == nullptr)
	{
		ResolvedClass = FindFirstObjectSafe<UClass>(*ClassName, EFindFirstObjectOptions::NativeFirst);
	}

	if (ResolvedClass == nullptr)
	{
		OutWarnings.Add(FString::Printf(TEXT("Could not find class '%s' for instanced sub-object"), *ClassName));
		return false;
	}

	// Validate class hierarchy
	if (!ResolvedClass->IsChildOf(ObjProp->PropertyClass))
	{
		OutWarnings.Add(FString::Printf(TEXT("Class '%s' is not a subclass of '%s'"),
			*ResolvedClass->GetName(), *ObjProp->PropertyClass->GetName()));
		return false;
	}

	// Cannot instantiate abstract classes
	if (ResolvedClass->HasAnyClassFlags(CLASS_Abstract))
	{
		OutWarnings.Add(FString::Printf(TEXT("Class '%s' is abstract and cannot be instantiated"), *ResolvedClass->GetName()));
		return false;
	}

	// Clean up existing sub-object
	UObject* Existing = ObjProp->GetObjectPropertyValue(ValuePtr);
	if (Existing)
	{
		Existing->MarkAsGarbage();
	}

	// Create new sub-object
	UObject* NewSubObj = NewObject<UObject>(Outer, ResolvedClass);

	// Deserialize properties if provided
	const TSharedPtr<FJsonObject>* PropertiesObj = nullptr;
	if ((*JsonObj)->TryGetObjectField(TEXT("properties"), PropertiesObj) && PropertiesObj && (*PropertiesObj).IsValid())
	{
		JsonToStruct(*PropertiesObj, ResolvedClass, NewSubObj, Outer, OutWarnings);
	}

	ObjProp->SetObjectPropertyValue(ValuePtr, NewSubObj);
	return true;
}

TSharedPtr<FJsonObject> FCortexSerializer::GetStructSchema(const UStruct* StructType, bool bIncludeInherited)
{
	TSharedPtr<FJsonObject> SchemaObj = MakeShared<FJsonObject>();

	if (StructType == nullptr)
	{
		return SchemaObj;
	}

	SchemaObj->SetStringField(TEXT("struct_name"), StructType->GetName());

	TArray<TSharedPtr<FJsonValue>> FieldsArray;

	for (TFieldIterator<FProperty> It(StructType); It; ++It)
	{
		const FProperty* Property = *It;

		// Skip inherited properties if not requested
		if (!bIncludeInherited && Property->GetOwnerStruct() != StructType)
		{
			continue;
		}

		TSharedPtr<FJsonObject> PropSchema = GetPropertySchema(Property);
		if (PropSchema.IsValid())
		{
			FieldsArray.Add(MakeShared<FJsonValueObject>(PropSchema));
		}
	}

	SchemaObj->SetArrayField(TEXT("fields"), FieldsArray);

	return SchemaObj;
}

TSharedPtr<FJsonObject> FCortexSerializer::GetPropertySchema(const FProperty* Property)
{
	if (Property == nullptr)
	{
		return nullptr;
	}

	TSharedPtr<FJsonObject> Schema = MakeShared<FJsonObject>();
	Schema->SetStringField(TEXT("name"), Property->GetName());
	Schema->SetStringField(TEXT("cpp_type"), Property->GetCPPType());

	// Bool
	if (CastField<FBoolProperty>(Property) != nullptr)
	{
		Schema->SetStringField(TEXT("type"), TEXT("bool"));
		return Schema;
	}

	// Int
	if (CastField<FIntProperty>(Property) != nullptr)
	{
		Schema->SetStringField(TEXT("type"), TEXT("int32"));
		return Schema;
	}

	// Int64
	if (CastField<FInt64Property>(Property) != nullptr)
	{
		Schema->SetStringField(TEXT("type"), TEXT("int64"));
		return Schema;
	}

	// Float
	if (CastField<FFloatProperty>(Property) != nullptr)
	{
		Schema->SetStringField(TEXT("type"), TEXT("float"));
		return Schema;
	}

	// Double
	if (CastField<FDoubleProperty>(Property) != nullptr)
	{
		Schema->SetStringField(TEXT("type"), TEXT("double"));
		return Schema;
	}

	// FString
	if (CastField<FStrProperty>(Property) != nullptr)
	{
		Schema->SetStringField(TEXT("type"), TEXT("FString"));
		return Schema;
	}

	// FName
	if (CastField<FNameProperty>(Property) != nullptr)
	{
		Schema->SetStringField(TEXT("type"), TEXT("FName"));
		return Schema;
	}

	// FText
	if (CastField<FTextProperty>(Property) != nullptr)
	{
		Schema->SetStringField(TEXT("type"), TEXT("FText"));
		return Schema;
	}

	// Enum property (enum class)
	if (const FEnumProperty* EnumProp = CastField<FEnumProperty>(Property))
	{
		const UEnum* Enum = EnumProp->GetEnum();
		Schema->SetStringField(TEXT("type"), TEXT("enum"));
		Schema->SetStringField(TEXT("enum_name"), Enum->GetName());

		TArray<TSharedPtr<FJsonValue>> EnumValues;
		for (int32 Index = 0; Index < Enum->NumEnums() - 1; ++Index)
		{
			EnumValues.Add(MakeShared<FJsonValueString>(Enum->GetNameStringByIndex(Index)));
		}
		Schema->SetArrayField(TEXT("enum_values"), EnumValues);
		return Schema;
	}

	// Byte property with enum (old-style TEnumAsByte)
	if (const FByteProperty* ByteProp = CastField<FByteProperty>(Property))
	{
		if (const UEnum* Enum = ByteProp->GetIntPropertyEnum())
		{
			Schema->SetStringField(TEXT("type"), TEXT("enum"));
			Schema->SetStringField(TEXT("enum_name"), Enum->GetName());

			TArray<TSharedPtr<FJsonValue>> EnumValues;
			for (int32 Index = 0; Index < Enum->NumEnums() - 1; ++Index)
			{
				EnumValues.Add(MakeShared<FJsonValueString>(Enum->GetNameStringByIndex(Index)));
			}
			Schema->SetArrayField(TEXT("enum_values"), EnumValues);
		}
		else
		{
			Schema->SetStringField(TEXT("type"), TEXT("uint8"));
		}
		return Schema;
	}

	// Struct property
	if (const FStructProperty* StructProp = CastField<FStructProperty>(Property))
	{
		// FInstancedStruct - include base type and known subtypes
		if (StructProp->Struct == FInstancedStruct::StaticStruct())
		{
			Schema->SetStringField(TEXT("type"), TEXT("FInstancedStruct"));

			// Check for metadata specifying the base struct type
			if (Property->HasMetaData(TEXT("BaseStruct")))
			{
				const FString& BaseStructMeta = Property->GetMetaData(TEXT("BaseStruct"));
				Schema->SetStringField(TEXT("instanced_struct_base"), BaseStructMeta);

				// Try to find the base struct and list known subtypes
				const UScriptStruct* BaseStruct = FindObject<UScriptStruct>(nullptr, *BaseStructMeta);
				if (BaseStruct == nullptr)
				{
					// Try with short name (O(1) hash lookup)
					BaseStruct = FindFirstObjectSafe<UScriptStruct>(*BaseStructMeta, EFindFirstObjectOptions::NativeFirst);
				}

				if (BaseStruct != nullptr)
				{
					TArray<UScriptStruct*> Subtypes = FindInstancedStructSubtypes(BaseStruct);
					TArray<TSharedPtr<FJsonValue>> SubtypeNames;
					for (const UScriptStruct* Subtype : Subtypes)
					{
						SubtypeNames.Add(MakeShared<FJsonValueString>(Subtype->GetName()));
					}
					Schema->SetArrayField(TEXT("known_subtypes"), SubtypeNames);
				}
			}
			return Schema;
		}

		// Named struct types
		Schema->SetStringField(TEXT("type"), StructProp->Struct->GetName());

		// Recursively include fields for nested structs
		TSharedPtr<FJsonObject> NestedSchema = GetStructSchema(StructProp->Struct);
		if (NestedSchema.IsValid())
		{
			const TArray<TSharedPtr<FJsonValue>>* NestedFields = nullptr;
			if (NestedSchema->TryGetArrayField(TEXT("fields"), NestedFields) && NestedFields != nullptr)
			{
				Schema->SetArrayField(TEXT("fields"), *NestedFields);
			}
		}
		return Schema;
	}

	// Array property
	if (const FArrayProperty* ArrayProp = CastField<FArrayProperty>(Property))
	{
		Schema->SetStringField(TEXT("type"), TEXT("TArray"));

		TSharedPtr<FJsonObject> ElementSchema = GetPropertySchema(ArrayProp->Inner);
		if (ElementSchema.IsValid())
		{
			Schema->SetObjectField(TEXT("element_type"), ElementSchema);
		}
		return Schema;
	}

	// Map property
	if (const FMapProperty* MapProp = CastField<FMapProperty>(Property))
	{
		Schema->SetStringField(TEXT("type"), TEXT("TMap"));

		TSharedPtr<FJsonObject> KeySchema = GetPropertySchema(MapProp->KeyProp);
		if (KeySchema.IsValid())
		{
			Schema->SetObjectField(TEXT("key_type"), KeySchema);
		}

		TSharedPtr<FJsonObject> ValueSchema = GetPropertySchema(MapProp->ValueProp);
		if (ValueSchema.IsValid())
		{
			Schema->SetObjectField(TEXT("value_type"), ValueSchema);
		}
		return Schema;
	}

	// Set property
	if (const FSetProperty* SetProp = CastField<FSetProperty>(Property))
	{
		Schema->SetStringField(TEXT("type"), TEXT("TSet"));

		TSharedPtr<FJsonObject> ElementSchema = GetPropertySchema(SetProp->ElementProp);
		if (ElementSchema.IsValid())
		{
			Schema->SetObjectField(TEXT("element_type"), ElementSchema);
		}
		return Schema;
	}

	// Object property (hard reference)
	if (const FObjectProperty* ObjProp = CastField<FObjectProperty>(Property))
	{
		Schema->SetStringField(TEXT("type"), TEXT("UObject*"));
		Schema->SetStringField(TEXT("object_class"), ObjProp->PropertyClass->GetName());
		if (Property->HasAllPropertyFlags(CPF_InstancedReference))
		{
			Schema->SetBoolField(TEXT("instanced"), true);
		}
		return Schema;
	}

	// Soft object property
	if (CastField<FSoftObjectProperty>(Property) != nullptr)
	{
		Schema->SetStringField(TEXT("type"), TEXT("TSoftObjectPtr"));
		return Schema;
	}

	// Fallback
	Schema->SetStringField(TEXT("type"), TEXT("unknown"));
	return Schema;
}

bool FCortexSerializer::IsPositionalNumericStruct(const UScriptStruct* Struct)
{
	if (Struct == nullptr)
	{
		return false;
	}

	// FColor: B,G,R,A memory layout makes positional promotion unsafe.
	// Callers must use {"R":r,"G":g,"B":b,"A":a} form instead.
	if (Struct == TBaseStructure<FColor>::Get())
	{
		return false;
	}

	// Note: FQuat (X,Y,Z,W) and FRotator (Pitch,Yaw,Roll) pass this check — their memory
	// layout matches declaration order, so positional promotion is technically correct.
	// However, callers may assume W-first quaternion convention or alphabetical rotator order.
	// FMatrix also passes (16 floats) — count mismatch is the only safety net for wrong usage.
	if (const bool* Cached = PositionalNumericStructCache.Find(Struct))
	{
		return *Cached;
	}

	bool bAllNumeric = true;
	// ExcludeSuper: positional contract is defined for the struct's own declared fields only.
	// A struct with inherited numeric fields would pass this check but the promotion loop
	// would only write the child's own fields, silently leaving parent fields at defaults.
	// In practice, all UE math structs (FLinearColor, FVector, etc.) have no numeric parents.
	for (TFieldIterator<FProperty> It(Struct, EFieldIteratorFlags::ExcludeSuper); It; ++It)
	{
		const FProperty* Prop = *It;
		bool bIsScalar =
			Prop->IsA<FFloatProperty>()  ||
			Prop->IsA<FDoubleProperty>() ||
			Prop->IsA<FIntProperty>()    ||
			Prop->IsA<FInt64Property>();

		if (!bIsScalar)
		{
			if (const FByteProperty* ByteProp = CastField<FByteProperty>(Prop))
			{
				bIsScalar = (ByteProp->GetIntPropertyEnum() == nullptr);
			}
		}

		if (!bIsScalar)
		{
			bAllNumeric = false;
			break;
		}
	}

	PositionalNumericStructCache.Add(Struct, bAllNumeric);
	return bAllNumeric;
}

TArray<UScriptStruct*> FCortexSerializer::FindInstancedStructSubtypes(const UScriptStruct* BaseStruct)
{
	if (BaseStruct == nullptr)
	{
		return TArray<UScriptStruct*>();
	}

	if (const TArray<UScriptStruct*>* Cached = SubtypeCache.Find(BaseStruct))
	{
		return *Cached;
	}

	TArray<UScriptStruct*> Subtypes;
	for (TObjectIterator<UScriptStruct> It; It; ++It)
	{
		if (It->IsChildOf(BaseStruct) && *It != BaseStruct)
		{
			Subtypes.Add(*It);
		}
	}

	SubtypeCache.Add(BaseStruct, Subtypes);
	return Subtypes;
}
