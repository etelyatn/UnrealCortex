
#include "CortexSerializer.h"
#include "UObject/UnrealType.h"
#include "UObject/TextProperty.h"
#include "UObject/EnumProperty.h"
#include "GameplayTagContainer.h"
#include "StructUtils/InstancedStruct.h"
#include "UObject/SoftObjectPath.h"
#include "Dom/JsonValue.h"

DEFINE_LOG_CATEGORY_STATIC(LogUDBSerializer, Log, All);

TMap<const UScriptStruct*, TArray<UScriptStruct*>> FUDBSerializer::SubtypeCache;

TSharedPtr<FJsonObject> FUDBSerializer::StructToJson(const UStruct* StructType, const void* StructData)
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

TSharedPtr<FJsonObject> FUDBSerializer::StructToJson(const UStruct* StructType, const void* StructData, const TSet<FString>& FieldFilter)
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

TSharedPtr<FJsonValue> FUDBSerializer::PropertyToJson(const FProperty* Property, const void* ValuePtr)
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

	UE_LOG(LogUDBSerializer, Warning, TEXT("Unhandled property type: %s (%s)"),
		*Property->GetName(), *Property->GetClass()->GetName());
	return nullptr;
}

bool FUDBSerializer::JsonToStruct(const TSharedPtr<FJsonObject>& JsonObject, const UStruct* StructType, void* StructData, TArray<FString>& OutWarnings)
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
		if (!JsonToProperty(JsonValue, Property, ValuePtr, OutWarnings))
		{
			OutWarnings.Add(FString::Printf(TEXT("Failed to deserialize field '%s'"), *FieldName));
		}
	}

	return true;
}

bool FUDBSerializer::JsonToProperty(const TSharedPtr<FJsonValue>& JsonValue, const FProperty* Property, void* ValuePtr, TArray<FString>& OutWarnings)
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
		const FString EnumString = JsonValue->AsString();
		int64 EnumValue = Enum->GetValueByNameString(EnumString);
		if (EnumValue == INDEX_NONE)
		{
			OutWarnings.Add(FString::Printf(TEXT("Unknown enum value '%s' for enum '%s'"), *EnumString, *Enum->GetName()));
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
			const FString EnumString = JsonValue->AsString();
			int64 EnumValue = Enum->GetValueByNameString(EnumString);
			if (EnumValue == INDEX_NONE)
			{
				OutWarnings.Add(FString::Printf(TEXT("Unknown enum value '%s' for enum '%s'"), *EnumString, *Enum->GetName()));
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

			// Find the UScriptStruct by name
			UScriptStruct* FoundStruct = nullptr;
			for (TObjectIterator<UScriptStruct> It; It; ++It)
			{
				if (It->GetName() == StructTypeName)
				{
					FoundStruct = *It;
					break;
				}
			}

			if (FoundStruct == nullptr)
			{
				OutWarnings.Add(FString::Printf(TEXT("Could not find struct type '%s' for FInstancedStruct"), *StructTypeName));
				return false;
			}

			FInstancedStruct* Instance = static_cast<FInstancedStruct*>(ValuePtr);
			Instance->InitializeAs(FoundStruct);
			return JsonToStruct(*InnerObj, FoundStruct, Instance->GetMutableMemory(), OutWarnings);
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
		return JsonToStruct(*NestedObj, StructProp->Struct, ValuePtr, OutWarnings);
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
		ArrayHelper.Resize(JsonArray->Num());
		for (int32 Index = 0; Index < JsonArray->Num(); ++Index)
		{
			JsonToProperty((*JsonArray)[Index], ArrayProp->Inner, ArrayHelper.GetRawPtr(Index), OutWarnings);
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
		MapHelper.EmptyValues();

		for (const auto& MapPair : (*MapObj)->Values)
		{
			int32 NewIndex = MapHelper.AddDefaultValue_Invalid_NeedsRehash();

			// Import key from string
			void* KeyPtr = MapHelper.GetKeyPtr(NewIndex);
			MapProp->KeyProp->ImportText_Direct(*MapPair.Key, KeyPtr, nullptr, PPF_None);

			// Import value
			void* MapValuePtr = MapHelper.GetValuePtr(NewIndex);
			JsonToProperty(MapPair.Value, MapProp->ValueProp, MapValuePtr, OutWarnings);
		}

		MapHelper.Rehash();
		return true;
	}

	// Soft object property
	if (const FSoftObjectProperty* SoftObjProp = CastField<FSoftObjectProperty>(Property))
	{
		FSoftObjectPtr* SoftPtr = reinterpret_cast<FSoftObjectPtr*>(ValuePtr);
		*SoftPtr = FSoftObjectPath(JsonValue->AsString());
		return true;
	}

	// Object property (hard reference)
	if (const FObjectProperty* ObjProp = CastField<FObjectProperty>(Property))
	{
		const FString ObjectPath = JsonValue->AsString();
		if (ObjectPath.IsEmpty())
		{
			ObjProp->SetObjectPropertyValue(ValuePtr, nullptr);
			return true;
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

	UE_LOG(LogUDBSerializer, Warning, TEXT("Unhandled property type for deserialization: %s (%s)"),
		*Property->GetName(), *Property->GetClass()->GetName());
	return false;
}

TSharedPtr<FJsonObject> FUDBSerializer::GetStructSchema(const UStruct* StructType, bool bIncludeInherited)
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

TSharedPtr<FJsonObject> FUDBSerializer::GetPropertySchema(const FProperty* Property)
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
					// Try with short name
					for (TObjectIterator<UScriptStruct> It; It; ++It)
					{
						if (It->GetName() == BaseStructMeta)
						{
							BaseStruct = *It;
							break;
						}
					}
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

TArray<UScriptStruct*> FUDBSerializer::FindInstancedStructSubtypes(const UScriptStruct* BaseStruct)
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
