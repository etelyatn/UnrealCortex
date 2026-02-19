#include "Operations/CortexBPTypeUtils.h"
#include "EdGraphSchema_K2.h"

FEdGraphPinType CortexBPTypeUtils::ResolveVariableType(const FString& TypeStr)
{
	FEdGraphPinType PinType;

	if (TypeStr == TEXT("bool"))
	{
		PinType.PinCategory = UEdGraphSchema_K2::PC_Boolean;
	}
	else if (TypeStr == TEXT("int") || TypeStr == TEXT("int32"))
	{
		PinType.PinCategory = UEdGraphSchema_K2::PC_Int;
	}
	else if (TypeStr == TEXT("float"))
	{
		PinType.PinCategory = UEdGraphSchema_K2::PC_Real;
		PinType.PinSubCategory = UEdGraphSchema_K2::PC_Float;
	}
	else if (TypeStr == TEXT("double"))
	{
		PinType.PinCategory = UEdGraphSchema_K2::PC_Real;
		PinType.PinSubCategory = UEdGraphSchema_K2::PC_Double;
	}
	else if (TypeStr == TEXT("FString") || TypeStr == TEXT("string"))
	{
		PinType.PinCategory = UEdGraphSchema_K2::PC_String;
	}
	else if (TypeStr == TEXT("FName") || TypeStr == TEXT("name"))
	{
		PinType.PinCategory = UEdGraphSchema_K2::PC_Name;
	}
	else if (TypeStr == TEXT("FText") || TypeStr == TEXT("text"))
	{
		PinType.PinCategory = UEdGraphSchema_K2::PC_Text;
	}
	else if (TypeStr == TEXT("FVector") || TypeStr == TEXT("vector"))
	{
		PinType.PinCategory = UEdGraphSchema_K2::PC_Struct;
		PinType.PinSubCategoryObject = TBaseStructure<FVector>::Get();
	}
	else if (TypeStr == TEXT("FRotator") || TypeStr == TEXT("rotator"))
	{
		PinType.PinCategory = UEdGraphSchema_K2::PC_Struct;
		PinType.PinSubCategoryObject = TBaseStructure<FRotator>::Get();
	}
	else if (TypeStr == TEXT("FLinearColor"))
	{
		PinType.PinCategory = UEdGraphSchema_K2::PC_Struct;
		PinType.PinSubCategoryObject = TBaseStructure<FLinearColor>::Get();
	}
	else
	{
		// Try to resolve as a class path (object reference)
		UClass* FoundClass = FindFirstObject<UClass>(*TypeStr, EFindFirstObjectOptions::NativeFirst);
		if (FoundClass != nullptr)
		{
			PinType.PinCategory = UEdGraphSchema_K2::PC_Object;
			PinType.PinSubCategoryObject = FoundClass;
		}
		else
		{
			// Fallback: try as a struct
			UScriptStruct* FoundStruct = FindFirstObject<UScriptStruct>(*TypeStr, EFindFirstObjectOptions::NativeFirst);
			if (FoundStruct != nullptr)
			{
				PinType.PinCategory = UEdGraphSchema_K2::PC_Struct;
				PinType.PinSubCategoryObject = FoundStruct;
			}
			else
			{
				// Default to wildcard if we can't resolve
				PinType.PinCategory = UEdGraphSchema_K2::PC_Wildcard;
			}
		}
	}

	return PinType;
}

FString CortexBPTypeUtils::FriendlyTypeName(const FEdGraphPinType& PinType)
{
	const FName Cat = PinType.PinCategory;

	if (Cat == UEdGraphSchema_K2::PC_Boolean)
	{
		return TEXT("bool");
	}
	if (Cat == UEdGraphSchema_K2::PC_Int)
	{
		return TEXT("int");
	}
	if (Cat == UEdGraphSchema_K2::PC_Real)
	{
		if (PinType.PinSubCategory == UEdGraphSchema_K2::PC_Float)
		{
			return TEXT("float");
		}
		if (PinType.PinSubCategory == UEdGraphSchema_K2::PC_Double)
		{
			return TEXT("double");
		}
		return TEXT("float");
	}
	if (Cat == UEdGraphSchema_K2::PC_String)
	{
		return TEXT("string");
	}
	if (Cat == UEdGraphSchema_K2::PC_Name)
	{
		return TEXT("name");
	}
	if (Cat == UEdGraphSchema_K2::PC_Text)
	{
		return TEXT("text");
	}
	if (Cat == UEdGraphSchema_K2::PC_Struct)
	{
		if (UScriptStruct* S = Cast<UScriptStruct>(PinType.PinSubCategoryObject.Get()))
		{
			return S->GetName();
		}
		return TEXT("struct");
	}
	if (Cat == UEdGraphSchema_K2::PC_Object)
	{
		if (UClass* C = Cast<UClass>(PinType.PinSubCategoryObject.Get()))
		{
			return C->GetName();
		}
		return TEXT("object");
	}

	return Cat.ToString();
}
