
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

TSharedPtr<FJsonObject> FCortexSerializer::TextToJson(const FText& Text)
{
	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("value"), Text.ToString());

	FName TableId;
	FString Key;
	if (FTextInspector::GetTableIdAndKey(Text, TableId, Key))
	{
		TSharedPtr<FJsonObject> StringTableObject = MakeShared<FJsonObject>();
		StringTableObject->SetStringField(TEXT("table_id"), TableId.ToString());
		StringTableObject->SetStringField(TEXT("key"), Key);
		Result->SetObjectField(TEXT("string_table"), StringTableObject);
	}

	return Result;
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
		return MakeShared<FJsonValueString>(TextValue.ToString());
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

	UE_LOG(LogCortex, Warning, TEXT("Unhandled property type: %s (%s)"),
		*Property->GetName(), *Property->GetClass()->GetName());
	return nullptr;
}

bool FCortexSerializer::JsonToStruct(const TSharedPtr<FJsonObject>& JsonObject, const UStruct* StructType, void* StructData, UObject* Outer, TArray<FString>& OutWarnings)
{
	if (!JsonObject.IsValid() || StructType == nullptr || StructData == nullptr)
	{
		return false;
	}

	for (const auto& Pair : JsonObject->Values)
	{
		const FString& FieldName = Pair.Key;
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
		}
	}

	return true;
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
		TextProp->SetPropertyValue(ValuePtr, FText::FromString(JsonValue->AsString()));
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

		ArrayHelper.Resize(JsonArray->Num());
		for (int32 Index = 0; Index < JsonArray->Num(); ++Index)
		{
			JsonToProperty((*JsonArray)[Index], ArrayProp->Inner, ArrayHelper.GetRawPtr(Index), Outer, OutWarnings);
		}
		return true;
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

	UE_LOG(LogCortex, Warning, TEXT("Unhandled property type for deserialization: %s (%s)"),
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
