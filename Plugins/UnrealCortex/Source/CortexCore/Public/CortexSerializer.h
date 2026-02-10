
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

	/** Deserialize JSON into a UStruct instance. Returns true on success. */
	static bool JsonToStruct(const TSharedPtr<FJsonObject>& JsonObject, const UStruct* StructType, void* StructData, TArray<FString>& OutWarnings);

	/** Deserialize a JSON value into a single FProperty. Returns true on success. */
	static bool JsonToProperty(const TSharedPtr<FJsonValue>& JsonValue, const FProperty* Property, void* ValuePtr, TArray<FString>& OutWarnings);

	/** Get schema for a UStruct (field names, types, enum values, nested schemas) */
	static TSharedPtr<FJsonObject> GetStructSchema(const UStruct* StructType, bool bIncludeInherited = true);

	/** Discover TInstancedStruct subtypes for a base struct */
	static TArray<UScriptStruct*> FindInstancedStructSubtypes(const UScriptStruct* BaseStruct);

private:
	/** Build schema for a single property */
	static TSharedPtr<FJsonObject> GetPropertySchema(const FProperty* Property);

	/** Cache for TInstancedStruct subtype discovery */
	static TMap<const UScriptStruct*, TArray<UScriptStruct*>> SubtypeCache;
};
