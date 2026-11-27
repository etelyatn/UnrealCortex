#include "CortexGraphNodeVariation.h"

#include "EdGraph/EdGraphNode.h"
#include "EdGraph/EdGraphPin.h"
#include "K2Node_VariableGet.h"
#include "UObject/Class.h"
#include "UObject/UnrealType.h"

namespace
{
	FString ShortEnumName(const UEnum* Enum, const int64 Value)
	{
		FString Name = Enum ? Enum->GetNameStringByValue(Value) : FString();
		int32 ScopeIndex = INDEX_NONE;
		if (Name.FindLastChar(TEXT(':'), ScopeIndex))
		{
			Name = Name.Mid(ScopeIndex + 1);
		}
		return Name;
	}

	/** Maps the engine's EGetNodeVariation enumerator name; empty when the name is not one of the three variations. */
	FString VariationFromName(const FString& Name)
	{
		if (Name == TEXT("Pure"))
		{
			return FCortexGraphNodeVariation::Pure();
		}
		if (Name == TEXT("Branch"))
		{
			return FCortexGraphNodeVariation::Branch();
		}
		if (Name == TEXT("ValidatedObject"))
		{
			return FCortexGraphNodeVariation::ValidatedObject();
		}
		return FString();
	}
}

FString FCortexGraphNodeVariation::GetVariableGetVariation(const UEdGraphNode* Node)
{
	const UK2Node_VariableGet* Get = Cast<UK2Node_VariableGet>(Node);
	if (Get == nullptr)
	{
		return FString();
	}

	static const FName VariationPropertyName(TEXT("CurrentVariation"));
	const FProperty* Property = Get->GetClass()->FindPropertyByName(VariationPropertyName);
	if (const FEnumProperty* EnumProperty = CastField<FEnumProperty>(Property))
	{
		const void* Value = EnumProperty->ContainerPtrToValuePtr<void>(Get);
		const int64 Raw = EnumProperty->GetUnderlyingProperty()->GetSignedIntPropertyValue(Value);
		const FString Mapped = VariationFromName(ShortEnumName(EnumProperty->GetEnum(), Raw));
		if (!Mapped.IsEmpty())
		{
			return Mapped;
		}
	}
	else if (const FByteProperty* ByteProperty = CastField<FByteProperty>(Property))
	{
		const uint8 Raw = *ByteProperty->ContainerPtrToValuePtr<uint8>(Get);
		const FString Mapped = VariationFromName(ShortEnumName(ByteProperty->Enum, Raw));
		if (!Mapped.IsEmpty())
		{
			return Mapped;
		}
	}

	// The reflected field is not readable: infer the variation from the pins the engine allocates for each.
	if (Get->IsNodePure())
	{
		return Pure();
	}
	return (Get->FindPin(FName(TEXT("then"))) != nullptr && Get->FindPin(FName(TEXT("else"))) != nullptr)
		? FString(Branch())
		: FString(ValidatedObject());
}

bool FCortexGraphNodeVariation::IsConditionalVariableGet(const UEdGraphNode* Node)
{
	const FString Variation = GetVariableGetVariation(Node);
	return Variation == Branch() || Variation == ValidatedObject();
}
