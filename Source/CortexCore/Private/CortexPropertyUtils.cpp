#include "CortexPropertyUtils.h"

bool FCortexPropertyUtils::ResolvePropertyPath(
	UObject* Object,
	const FString& PropertyPath,
	FProperty*& OutProperty,
	void*& OutValuePtr)
{
	if (!Object || PropertyPath.IsEmpty())
	{
		return false;
	}

	TArray<FString> Segments;
	PropertyPath.ParseIntoArray(Segments, TEXT("."), true);
	if (Segments.Num() == 0)
	{
		return false;
	}

	UStruct* CurrentStruct = Object->GetClass();
	void* CurrentContainer = Object;
	FProperty* CurrentProperty = nullptr;

	for (int32 i = 0; i < Segments.Num(); ++i)
	{
		CurrentProperty = CurrentStruct->FindPropertyByName(FName(*Segments[i]));
		if (!CurrentProperty)
		{
			return false;
		}

		void* ValuePtr = CurrentProperty->ContainerPtrToValuePtr<void>(CurrentContainer);
		if (i == Segments.Num() - 1)
		{
			OutProperty = CurrentProperty;
			OutValuePtr = ValuePtr;
			return true;
		}

		if (FStructProperty* StructProperty = CastField<FStructProperty>(CurrentProperty))
		{
			CurrentStruct = StructProperty->Struct;
			CurrentContainer = ValuePtr;
		}
		else if (FObjectProperty* ObjectProperty = CastField<FObjectProperty>(CurrentProperty))
		{
			UObject* ObjectValue = ObjectProperty->GetObjectPropertyValue(ValuePtr);
			if (!ObjectValue)
			{
				return false;
			}

			CurrentStruct = ObjectValue->GetClass();
			CurrentContainer = ObjectValue;
		}
		else
		{
			return false;
		}
	}

	return false;
}
