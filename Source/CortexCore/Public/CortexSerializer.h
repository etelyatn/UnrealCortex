
#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"

class CORTEXCORE_API FCortexSerializer
{
public:
	/** Serialize a UStruct instance to a JSON object using UProperty reflection */
	static TSharedPtr<FJsonObject> StructToJson(const UStruct* StructType, const void* StructData);

	/** Serialize a UStruct instance to a JSON object, only including fields in the filter set.
	 *  When FieldFilter is empty, delegates to the full-serialization overload. */
	static TSharedPtr<FJsonObject> StructToJson(const UStruct* StructType, const void* StructData, const TSet<FString>& FieldFilter);

	/** Serialize a single FProperty value to a JSON value */
	static TSharedPtr<FJsonValue> PropertyToJson(const FProperty* Property, const void* ValuePtr);

	/** Serialize only UObject properties that differ from the class default object. */
	static TSharedPtr<FJsonObject> NonDefaultPropertiesToJson(const UObject* Object, int32 MaxDepth = 1);

	/** Serialize FText to JSON with optional string table source metadata. */
	static TSharedPtr<FJsonObject> TextToJson(const FText& Text);

	/** Deserialize JSON into a UStruct instance. Returns true on success.
	 *  Outer is the owning UObject for instanced sub-object creation. */
	static bool JsonToStruct(const TSharedPtr<FJsonObject>& JsonObject, const UStruct* StructType, void* StructData, UObject* Outer, TArray<FString>& OutWarnings);

	/** Deserialize a JSON value into a single FProperty. Returns true on success.
	 *  Outer is the owning UObject for instanced sub-object creation. */
	static bool JsonToProperty(const TSharedPtr<FJsonValue>& JsonValue, const FProperty* Property, void* ValuePtr, UObject* Outer, TArray<FString>& OutWarnings);

	/** Returns true when all fields of Struct are numeric scalars and positional
	 *  array assignment is semantically safe. FColor is always false.
	 *  Result is cached per UScriptStruct*. */
	static bool IsPositionalNumericStruct(const UScriptStruct* Struct);

	/** Get schema for a UStruct (field names, types, enum values, nested schemas) */
	static TSharedPtr<FJsonObject> GetStructSchema(const UStruct* StructType, bool bIncludeInherited = true);

	/** Discover TInstancedStruct subtypes for a base struct */
	static TArray<UScriptStruct*> FindInstancedStructSubtypes(const UScriptStruct* BaseStruct);

private:
	/** Create an instanced sub-object from JSON {"_class": "...", "properties": {...}} */
	static bool JsonToInstancedSubObject(const TSharedPtr<FJsonValue>& JsonValue, const FObjectProperty* ObjProp, void* ValuePtr, UObject* Outer, TArray<FString>& OutWarnings);

	/** Build schema for a single property */
	static TSharedPtr<FJsonObject> GetPropertySchema(const FProperty* Property);

	/** Serialize non-default properties of a UObject, comparing against a provided default. */
	static TSharedPtr<FJsonObject> ObjectNonDefaultPropertiesToJson(const UObject* Object, const UObject* DefaultObject, int32 MaxDepth);

	/** Serialize non-default properties of a UStruct, comparing against provided defaults. */
	static TSharedPtr<FJsonObject> StructNonDefaultPropertiesToJson(const UStruct* StructType, const void* StructData, const void* DefaultData, int32 MaxDepth);

	/** Serialize a single property if it differs from its default value. */
	static TSharedPtr<FJsonValue> NonDefaultPropertyToJson(const FProperty* Property, const void* ValuePtr, const void* DefaultValuePtr, int32 MaxDepth);

	/** Sentinel string emitted when MaxDepth prevents full serialization. */
	static const FString ChangedMarker;

	/** Cache for TInstancedStruct subtype discovery */
	static TMap<const UScriptStruct*, TArray<UScriptStruct*>> SubtypeCache;

	/** Cache for IsPositionalNumericStruct results */
	static TMap<const UScriptStruct*, bool> PositionalNumericStructCache;
};
