#include "Operations/CortexGraphPinDefaults.h"
#include "Operations/CortexGraphSymbolResolver.h"
#include "CortexSerializer.h"
#include "EdGraph/EdGraphPin.h"
#include "EdGraph/EdGraphNode.h"
#include "EdGraph/EdGraph.h"
#include "EdGraphSchema_K2.h"
#include "K2Node_GenericCreateObject.h"
#include "UObject/Class.h"
#include "UObject/Package.h"
#include "Misc/PackageName.h"
#include "Internationalization/StringTable.h"
#include "Internationalization/StringTableCore.h"

bool FCortexGraphPinDefaults::Validate(
	const UEdGraphPin* Pin,
	const TSharedPtr<FJsonObject>& Literal,
	FCortexCommandResult& OutError)
{
	if (Pin == nullptr)
	{
		OutError = FCortexCommandRouter::Error(
			CortexErrorCodes::InvalidField,
			TEXT("Pin cannot be null"));
		return false;
	}

	// 1. Output pins do not accept defaults
	if (Pin->Direction != EGPD_Input)
	{
		OutError = FCortexCommandRouter::Error(
			CortexErrorCodes::InvalidOperation,
			FString::Printf(TEXT("Cannot set default value on output pin: %s"), *Pin->PinName.ToString()));
		return false;
	}

	// 2. Connected pins do not accept defaults
	if (Pin->LinkedTo.Num() > 0)
	{
		OutError = FCortexCommandRouter::Error(
			CortexErrorCodes::InvalidOperation,
			FString::Printf(TEXT("Cannot set default value on connected pin: %s"), *Pin->PinName.ToString()));
		return false;
	}

	// 3. Literal descriptor must be valid and contain a recognized "kind"
	if (!Literal.IsValid())
	{
		OutError = FCortexCommandRouter::Error(
			CortexErrorCodes::InvalidField,
			TEXT("Literal descriptor cannot be null"));
		return false;
	}

	FString Kind;
	if (!Literal->TryGetStringField(TEXT("kind"), Kind) || Kind.IsEmpty())
	{
		OutError = FCortexCommandRouter::Error(
			CortexErrorCodes::InvalidField,
			TEXT("Literal descriptor missing required 'kind' field"));
		return false;
	}

	// 4. Validate kind against pin category and value shape
	if (Kind == TEXT("class") || Kind == TEXT("soft_class"))
	{
		if (Pin->PinType.PinCategory != UEdGraphSchema_K2::PC_Class &&
			Pin->PinType.PinCategory != UEdGraphSchema_K2::PC_SoftClass)
		{
			OutError = FCortexCommandRouter::Error(
				CortexErrorCodes::TypeMismatch,
				FString::Printf(TEXT("Cannot assign class literal to pin of category '%s'"), *Pin->PinType.PinCategory.ToString()));
			return false;
		}

		if (Kind == TEXT("soft_class") && Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Class)
		{
			OutError = FCortexCommandRouter::Error(
				CortexErrorCodes::InvalidField,
				TEXT("Cannot assign soft class literal to hard class pin: unsupported soft-reference mode"));
			return false;
		}

		FString Path;
		if (!Literal->TryGetStringField(TEXT("path"), Path))
		{
			OutError = FCortexCommandRouter::Error(
				CortexErrorCodes::InvalidField,
				TEXT("Class literal requires 'path' field"));
			return false;
		}

		if (Path.IsEmpty() || Path.Equals(TEXT("None"), ESearchCase::IgnoreCase))
		{
			return true; // Typed null semantics
		}

		UClass* ResolvedClass = nullptr;
		FCortexCommandResult ResolveError;
		if (!FCortexGraphSymbolResolver::ResolveClass(Path, ResolvedClass, ResolveError))
		{
			OutError = ResolveError;
			return false;
		}

		if (ResolvedClass == nullptr)
		{
			OutError = FCortexCommandRouter::Error(
				CortexErrorCodes::ClassNotFound,
				FString::Printf(TEXT("Class not found: %s"), *Path));
			return false;
		}

		if (ResolvedClass->HasAnyClassFlags(CLASS_Abstract))
		{
			OutError = FCortexCommandRouter::Error(
				CortexErrorCodes::InvalidField,
				FString::Printf(TEXT("Class '%s' is abstract and cannot be used as a default"), *Path));
			return false;
		}

		if (ResolvedClass->HasAnyClassFlags(CLASS_Deprecated | CLASS_NewerVersionExists))
		{
			OutError = FCortexCommandRouter::Error(
				CortexErrorCodes::InvalidField,
				FString::Printf(TEXT("Class '%s' is deprecated and cannot be used as a default"), *Path));
			return false;
		}

		if (Pin->PinType.PinSubCategoryObject.IsValid())
		{
			UClass* ExpectedBaseClass = Cast<UClass>(Pin->PinType.PinSubCategoryObject.Get());
			if (ExpectedBaseClass && !ResolvedClass->IsChildOf(ExpectedBaseClass))
			{
				OutError = FCortexCommandRouter::Error(
					CortexErrorCodes::TypeMismatch,
					FString::Printf(TEXT("Class '%s' does not inherit from expected base '%s'"), *ResolvedClass->GetName(), *ExpectedBaseClass->GetName()));
				return false;
			}
		}

		return true;
	}

	if (Kind == TEXT("object") || Kind == TEXT("soft_object"))
	{
		if (Pin->PinType.PinCategory != UEdGraphSchema_K2::PC_Object &&
			Pin->PinType.PinCategory != UEdGraphSchema_K2::PC_SoftObject &&
			Pin->PinType.PinCategory != UEdGraphSchema_K2::PC_Interface)
		{
			OutError = FCortexCommandRouter::Error(
				CortexErrorCodes::TypeMismatch,
				FString::Printf(TEXT("Cannot assign object literal to pin of category '%s'"), *Pin->PinType.PinCategory.ToString()));
			return false;
		}

		if (Kind == TEXT("soft_object") && Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Object)
		{
			OutError = FCortexCommandRouter::Error(
				CortexErrorCodes::InvalidField,
				TEXT("Cannot assign soft object literal to hard object pin: unsupported soft-reference mode"));
			return false;
		}

		FString Path;
		if (!Literal->TryGetStringField(TEXT("path"), Path))
		{
			OutError = FCortexCommandRouter::Error(
				CortexErrorCodes::InvalidField,
				TEXT("Object literal requires 'path' field"));
			return false;
		}

		if (Path.IsEmpty() || Path.Equals(TEXT("None"), ESearchCase::IgnoreCase))
		{
			return true; // Typed null semantics
		}

		// Guarded loading: check memory first, then package existence before calling LoadObject
		UObject* LoadedObj = FindObject<UObject>(nullptr, *Path);
		if (LoadedObj == nullptr)
		{
			const FString LongPackageName = FPackageName::ObjectPathToPackageName(Path);
			if (LongPackageName.IsEmpty() ||
				(!FindPackage(nullptr, *LongPackageName) && !FPackageName::DoesPackageExist(LongPackageName)))
			{
				OutError = FCortexCommandRouter::Error(
					CortexErrorCodes::AssetNotFound,
					FString::Printf(TEXT("Asset package not found for object: %s"), *Path));
				return false;
			}
			LoadedObj = LoadObject<UObject>(nullptr, *Path);
		}

		if (LoadedObj == nullptr)
		{
			OutError = FCortexCommandRouter::Error(
				CortexErrorCodes::AssetNotFound,
				FString::Printf(TEXT("Object not found: %s"), *Path));
			return false;
		}

		if (Pin->PinType.PinSubCategoryObject.IsValid())
		{
			UClass* ExpectedClass = Cast<UClass>(Pin->PinType.PinSubCategoryObject.Get());
			if (ExpectedClass && !LoadedObj->IsA(ExpectedClass))
			{
				OutError = FCortexCommandRouter::Error(
					CortexErrorCodes::TypeMismatch,
					FString::Printf(TEXT("Object '%s' is of class '%s', expected '%s'"), *Path, *LoadedObj->GetClass()->GetName(), *ExpectedClass->GetName()));
				return false;
			}
		}

		return true;
	}

	if (Kind == TEXT("text"))
	{
		if (Pin->PinType.PinCategory != UEdGraphSchema_K2::PC_Text)
		{
			OutError = FCortexCommandRouter::Error(
				CortexErrorCodes::TypeMismatch,
				FString::Printf(TEXT("Cannot assign text literal to pin of category '%s'"), *Pin->PinType.PinCategory.ToString()));
			return false;
		}

		const bool bHasLiteral = Literal->HasField(TEXT("literal"));
		const bool bHasTable = Literal->HasField(TEXT("table")) || Literal->HasField(TEXT("table_id")) || Literal->HasField(TEXT("string_table"));
		const bool bHasKey = Literal->HasField(TEXT("key"));

		if (bHasLiteral && (bHasTable || bHasKey))
		{
			OutError = FCortexCommandRouter::Error(
				CortexErrorCodes::InvalidField,
				TEXT("Conflicting text encoding: contains both literal and string table reference"));
			return false;
		}

		if (bHasLiteral)
		{
			return true;
		}

		if (bHasTable || bHasKey)
		{
			FString TableId;
			if (!Literal->TryGetStringField(TEXT("table"), TableId))
			{
				Literal->TryGetStringField(TEXT("table_id"), TableId);
			}
			FString Key;
			Literal->TryGetStringField(TEXT("key"), Key);

			if (TableId.IsEmpty() || Key.IsEmpty())
			{
				OutError = FCortexCommandRouter::Error(
					CortexErrorCodes::InvalidField,
					TEXT("String table text descriptor requires both table and key"));
				return false;
			}
			return true;
		}

		if (Literal->HasField(TEXT("value")))
		{
			TArray<FString> TextErrors;
			TSharedPtr<FJsonObject> AppliedText;
			if (!FCortexSerializer::NormalizeTextDescriptor(Literal->GetField<EJson::None>(TEXT("value")), AppliedText, nullptr, TextErrors))
			{
				OutError = FCortexCommandRouter::Error(
					CortexErrorCodes::InvalidField,
					FString::Join(TextErrors, TEXT("; ")));
				return false;
			}
			return true;
		}

		OutError = FCortexCommandRouter::Error(
			CortexErrorCodes::InvalidField,
			TEXT("Text literal requires 'literal', 'table'/'key', or 'value'"));
		return false;
	}

	if (Kind == TEXT("bool"))
	{
		if (Pin->PinType.PinCategory != UEdGraphSchema_K2::PC_Boolean)
		{
			OutError = FCortexCommandRouter::Error(
				CortexErrorCodes::TypeMismatch,
				FString::Printf(TEXT("Cannot assign bool literal to pin of category '%s'"), *Pin->PinType.PinCategory.ToString()));
			return false;
		}

		bool bVal = false;
		if (!Literal->TryGetBoolField(TEXT("value"), bVal))
		{
			FString Str;
			if (Literal->TryGetStringField(TEXT("value"), Str))
			{
				if (!Str.Equals(TEXT("true"), ESearchCase::IgnoreCase) && !Str.Equals(TEXT("false"), ESearchCase::IgnoreCase))
				{
					OutError = FCortexCommandRouter::Error(
						CortexErrorCodes::InvalidField,
						TEXT("Invalid bool value in literal"));
					return false;
				}
			}
			else
			{
				OutError = FCortexCommandRouter::Error(
					CortexErrorCodes::InvalidField,
					TEXT("Bool literal requires boolean 'value'"));
				return false;
			}
		}
		return true;
	}

	if (Kind == TEXT("int"))
	{
		if (Pin->PinType.PinCategory != UEdGraphSchema_K2::PC_Int &&
			Pin->PinType.PinCategory != UEdGraphSchema_K2::PC_Int64 &&
			Pin->PinType.PinCategory != UEdGraphSchema_K2::PC_Byte)
		{
			OutError = FCortexCommandRouter::Error(
				CortexErrorCodes::TypeMismatch,
				FString::Printf(TEXT("Cannot assign int literal to pin of category '%s'"), *Pin->PinType.PinCategory.ToString()));
			return false;
		}

		int64 IntVal = 0;
		if (!Literal->TryGetNumberField(TEXT("value"), IntVal))
		{
			FString Str;
			if (!Literal->TryGetStringField(TEXT("value"), Str) || !Str.IsNumeric())
			{
				OutError = FCortexCommandRouter::Error(
					CortexErrorCodes::InvalidField,
					TEXT("Int literal requires numeric 'value'"));
				return false;
			}
		}
		return true;
	}

	if (Kind == TEXT("real") || Kind == TEXT("float"))
	{
		if (Pin->PinType.PinCategory != UEdGraphSchema_K2::PC_Real &&
			Pin->PinType.PinCategory != UEdGraphSchema_K2::PC_Float &&
			Pin->PinType.PinCategory != UEdGraphSchema_K2::PC_Double)
		{
			OutError = FCortexCommandRouter::Error(
				CortexErrorCodes::TypeMismatch,
				FString::Printf(TEXT("Cannot assign real literal to pin of category '%s'"), *Pin->PinType.PinCategory.ToString()));
			return false;
		}

		double RealVal = 0.0;
		if (!Literal->TryGetNumberField(TEXT("value"), RealVal))
		{
			FString Str;
			if (!Literal->TryGetStringField(TEXT("value"), Str) || !Str.IsNumeric())
			{
				OutError = FCortexCommandRouter::Error(
					CortexErrorCodes::InvalidField,
					TEXT("Real literal requires numeric 'value'"));
				return false;
			}
		}
		return true;
	}

	if (Kind == TEXT("string"))
	{
		if (Pin->PinType.PinCategory != UEdGraphSchema_K2::PC_String)
		{
			OutError = FCortexCommandRouter::Error(
				CortexErrorCodes::TypeMismatch,
				FString::Printf(TEXT("Cannot assign string literal to pin of category '%s'"), *Pin->PinType.PinCategory.ToString()));
			return false;
		}

		FString Str;
		if (!Literal->TryGetStringField(TEXT("value"), Str))
		{
			OutError = FCortexCommandRouter::Error(
				CortexErrorCodes::InvalidField,
				TEXT("String literal requires 'value' string"));
			return false;
		}
		return true;
	}

	if (Kind == TEXT("name"))
	{
		if (Pin->PinType.PinCategory != UEdGraphSchema_K2::PC_Name)
		{
			OutError = FCortexCommandRouter::Error(
				CortexErrorCodes::TypeMismatch,
				FString::Printf(TEXT("Cannot assign name literal to pin of category '%s'"), *Pin->PinType.PinCategory.ToString()));
			return false;
		}

		FString Name;
		if (!Literal->TryGetStringField(TEXT("value"), Name))
		{
			OutError = FCortexCommandRouter::Error(
				CortexErrorCodes::InvalidField,
				TEXT("Name literal requires 'value' string"));
			return false;
		}
		return true;
	}

	if (Kind == TEXT("enum"))
	{
		if (Pin->PinType.PinCategory != UEdGraphSchema_K2::PC_Enum &&
			!(Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Byte && Pin->PinType.PinSubCategoryObject.IsValid()))
		{
			OutError = FCortexCommandRouter::Error(
				CortexErrorCodes::TypeMismatch,
				FString::Printf(TEXT("Cannot assign enum literal to pin of category '%s'"), *Pin->PinType.PinCategory.ToString()));
			return false;
		}

		FString EnumValStr;
		if (!Literal->TryGetStringField(TEXT("value"), EnumValStr))
		{
			OutError = FCortexCommandRouter::Error(
				CortexErrorCodes::InvalidField,
				TEXT("Enum literal requires 'value' string"));
			return false;
		}

		UEnum* Enum = Cast<UEnum>(Pin->PinType.PinSubCategoryObject.Get());
		if (Enum != nullptr)
		{
			if (Enum->GetIndexByName(FName(*EnumValStr)) == INDEX_NONE &&
				Enum->GetIndexByNameString(EnumValStr) == INDEX_NONE &&
				Enum->GetValueByNameString(EnumValStr) == INDEX_NONE)
			{
				OutError = FCortexCommandRouter::Error(
					CortexErrorCodes::InvalidField,
					FString::Printf(TEXT("Invalid enum value '%s' for enum '%s'"), *EnumValStr, *Enum->GetName()));
				return false;
			}
		}
		return true;
	}

	if (Kind == TEXT("null"))
	{
		if (Pin->PinType.PinCategory != UEdGraphSchema_K2::PC_Object &&
			Pin->PinType.PinCategory != UEdGraphSchema_K2::PC_Class &&
			Pin->PinType.PinCategory != UEdGraphSchema_K2::PC_SoftObject &&
			Pin->PinType.PinCategory != UEdGraphSchema_K2::PC_SoftClass &&
			Pin->PinType.PinCategory != UEdGraphSchema_K2::PC_Interface)
		{
			OutError = FCortexCommandRouter::Error(
				CortexErrorCodes::TypeMismatch,
				FString::Printf(TEXT("Cannot assign null literal to pin of category '%s'"), *Pin->PinType.PinCategory.ToString()));
			return false;
		}
		return true;
	}

	if (Kind == TEXT("struct"))
	{
		if (Pin->PinType.PinCategory != UEdGraphSchema_K2::PC_Struct)
		{
			OutError = FCortexCommandRouter::Error(
				CortexErrorCodes::TypeMismatch,
				FString::Printf(TEXT("Cannot assign struct literal to pin of category '%s'"), *Pin->PinType.PinCategory.ToString()));
			return false;
		}
		return true;
	}

	OutError = FCortexCommandRouter::Error(
		CortexErrorCodes::InvalidField,
		FString::Printf(TEXT("Unsupported pin default kind: %s"), *Kind));
	return false;
}

bool FCortexGraphPinDefaults::ApplyDefault(
	UEdGraphPin* Pin,
	const TSharedPtr<FJsonObject>& Literal,
	FCortexCommandResult& OutError)
{
	if (!Validate(Pin, Literal, OutError))
	{
		return false;
	}

	UEdGraphNode* Node = Pin->GetOwningNode();
	UEdGraph* Graph = Node ? Node->GetGraph() : nullptr;

	if (Node != nullptr)
	{
		Node->Modify();
	}
	if (Graph != nullptr)
	{
		Graph->Modify();
	}

	const FString Kind = Literal->GetStringField(TEXT("kind"));

	if (Kind == TEXT("class") || Kind == TEXT("soft_class"))
	{
		const FString Path = Literal->GetStringField(TEXT("path"));
		if (Path.IsEmpty() || Path.Equals(TEXT("None"), ESearchCase::IgnoreCase))
		{
			Pin->DefaultObject = nullptr;
			Pin->DefaultValue = TEXT("None");
		}
		else if (Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_SoftClass || Kind == TEXT("soft_class"))
		{
			// Preserve soft references as soft references
			Pin->DefaultObject = nullptr;
			Pin->DefaultValue = Path;
		}
		else
		{
			UClass* ResolvedClass = nullptr;
			FCortexCommandResult ResolveError;
			FCortexGraphSymbolResolver::ResolveClass(Path, ResolvedClass, ResolveError);
			Pin->DefaultObject = ResolvedClass;
			Pin->DefaultValue.Empty();
		}

		// G1 lifecycle for UK2Node_GenericCreateObject. Orphan pin saving is
		// suppressed so exposed-on-spawn pins from the previous class are dropped
		// instead of lingering as non-connectable orphan pins.
		if (UK2Node_GenericCreateObject* CreateNode = Cast<UK2Node_GenericCreateObject>(Node))
		{
			if (Pin == CreateNode->GetClassPin())
			{
				const bool bPreviousDisableOrphanSaving = CreateNode->bDisableOrphanPinSaving;
				CreateNode->bDisableOrphanPinSaving = true;
				CreateNode->PinDefaultValueChanged(Pin);
				CreateNode->ReconstructNode();
				CreateNode->bDisableOrphanPinSaving = bPreviousDisableOrphanSaving;
			}
		}
	}
	else if (Kind == TEXT("object") || Kind == TEXT("soft_object"))
	{
		const FString Path = Literal->GetStringField(TEXT("path"));
		if (Path.IsEmpty() || Path.Equals(TEXT("None"), ESearchCase::IgnoreCase))
		{
			Pin->DefaultObject = nullptr;
			Pin->DefaultValue = TEXT("None");
		}
		else if (Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_SoftObject || Kind == TEXT("soft_object"))
		{
			// Preserve soft references as soft references
			Pin->DefaultObject = nullptr;
			Pin->DefaultValue = Path;
		}
		else
		{
			UObject* LoadedObj = FindObject<UObject>(nullptr, *Path);
			if (LoadedObj == nullptr)
			{
				LoadedObj = LoadObject<UObject>(nullptr, *Path);
			}
			Pin->DefaultObject = LoadedObj;
			Pin->DefaultValue.Empty();
		}
	}
	else if (Kind == TEXT("text"))
	{
		return ApplyTextDefault(Pin, Literal, OutError);
	}
	else if (Kind == TEXT("bool"))
	{
		bool bVal = false;
		if (!Literal->TryGetBoolField(TEXT("value"), bVal))
		{
			FString Str;
			if (Literal->TryGetStringField(TEXT("value"), Str))
			{
				bVal = Str.ToBool();
			}
		}
		Pin->DefaultValue = bVal ? TEXT("true") : TEXT("false");
	}
	else if (Kind == TEXT("int"))
	{
		int64 IntVal = 0;
		if (!Literal->TryGetNumberField(TEXT("value"), IntVal))
		{
			FString Str;
			if (Literal->TryGetStringField(TEXT("value"), Str))
			{
				IntVal = FCString::Atoi64(*Str);
			}
		}
		Pin->DefaultValue = FString::Printf(TEXT("%lld"), IntVal);
	}
	else if (Kind == TEXT("real") || Kind == TEXT("float"))
	{
		double RealVal = 0.0;
		if (!Literal->TryGetNumberField(TEXT("value"), RealVal))
		{
			FString Str;
			if (Literal->TryGetStringField(TEXT("value"), Str))
			{
				RealVal = FCString::Atod(*Str);
			}
		}
		Pin->DefaultValue = FString::Printf(TEXT("%f"), RealVal);
	}
	else if (Kind == TEXT("string") || Kind == TEXT("name") || Kind == TEXT("enum"))
	{
		FString StrVal;
		Literal->TryGetStringField(TEXT("value"), StrVal);
		Pin->DefaultValue = StrVal;
	}
	else if (Kind == TEXT("null"))
	{
		Pin->DefaultObject = nullptr;
		Pin->DefaultValue = TEXT("None");
	}

	if (Graph != nullptr)
	{
		Graph->NotifyGraphChanged();
	}

	return true;
}

bool FCortexGraphPinDefaults::ApplyTextDefault(
	UEdGraphPin* Pin,
	const TSharedPtr<FJsonObject>& TextObject,
	FCortexCommandResult& OutError)
{
	if (Pin == nullptr || !TextObject.IsValid())
	{
		OutError = FCortexCommandRouter::Error(CortexErrorCodes::InvalidField, TEXT("Invalid pin or text object"));
		return false;
	}

	FText NewText;

	if (TextObject->HasField(TEXT("literal")))
	{
		FString LiteralVal;
		TextObject->TryGetStringField(TEXT("literal"), LiteralVal);
		NewText = FText::FromString(LiteralVal);
	}
	else if (TextObject->HasField(TEXT("table")) || TextObject->HasField(TEXT("table_id")))
	{
		FString TableId;
		if (!TextObject->TryGetStringField(TEXT("table"), TableId))
		{
			TextObject->TryGetStringField(TEXT("table_id"), TableId);
		}
		FString Key;
		TextObject->TryGetStringField(TEXT("key"), Key);

		NewText = FText::FromStringTable(FName(*TableId), Key);
	}
	else if (TextObject->HasField(TEXT("value")))
	{
		TArray<FString> Errors;
		TSharedPtr<FJsonObject> AppliedText;
		if (!FCortexSerializer::NormalizeTextDescriptor(TextObject->GetField<EJson::None>(TEXT("value")), AppliedText, &NewText, Errors))
		{
			OutError = FCortexCommandRouter::Error(CortexErrorCodes::InvalidField, FString::Join(Errors, TEXT("; ")));
			return false;
		}
	}
	else
	{
		TArray<FString> Errors;
		TSharedPtr<FJsonObject> AppliedText;
		if (!FCortexSerializer::NormalizeTextDescriptor(MakeShared<FJsonValueObject>(TextObject), AppliedText, &NewText, Errors))
		{
			OutError = FCortexCommandRouter::Error(CortexErrorCodes::InvalidField, FString::Join(Errors, TEXT("; ")));
			return false;
		}
	}

	if (UEdGraphNode* Node = Pin->GetOwningNode())
	{
		Node->Modify();
		if (UEdGraph* Graph = Node->GetGraph())
		{
			Graph->Modify();
		}
	}

	Pin->DefaultTextValue = NewText;
	Pin->DefaultValue.Empty();
	Pin->DefaultObject = nullptr;

	if (UEdGraphNode* Node = Pin->GetOwningNode())
	{
		if (UEdGraph* Graph = Node->GetGraph())
		{
			Graph->NotifyGraphChanged();
		}
	}

	return true;
}

bool FCortexGraphPinDefaults::ReadDefault(
	const UEdGraphPin* Pin,
	TSharedPtr<FJsonObject>& OutDescriptor,
	FCortexCommandResult& OutError)
{
	if (Pin == nullptr)
	{
		OutError = FCortexCommandRouter::Error(CortexErrorCodes::InvalidField, TEXT("Pin cannot be null"));
		return false;
	}

	OutDescriptor = MakeShared<FJsonObject>();

	if (Pin->DefaultObject != nullptr)
	{
		if (Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Class ||
			Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_SoftClass)
		{
			OutDescriptor->SetStringField(TEXT("kind"), TEXT("class"));
		}
		else
		{
			OutDescriptor->SetStringField(TEXT("kind"), TEXT("object"));
		}
		OutDescriptor->SetStringField(TEXT("path"), Pin->DefaultObject->GetPathName());
		return true;
	}

	if (Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Text)
	{
		OutDescriptor->SetStringField(TEXT("kind"), TEXT("text"));
		if (!Pin->DefaultTextValue.IsEmpty())
		{
			OutDescriptor->SetObjectField(TEXT("value"), FCortexSerializer::TextToJson(Pin->DefaultTextValue));
		}
		else if (!Pin->DefaultValue.IsEmpty())
		{
			OutDescriptor->SetObjectField(TEXT("value"), FCortexSerializer::TextToJson(FText::FromString(Pin->DefaultValue)));
		}
		else
		{
			OutDescriptor->SetObjectField(TEXT("value"), FCortexSerializer::TextToJson(FText::GetEmpty()));
		}
		return true;
	}

	if (Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Boolean)
	{
		OutDescriptor->SetStringField(TEXT("kind"), TEXT("bool"));
		OutDescriptor->SetBoolField(TEXT("value"), Pin->DefaultValue.ToBool());
		return true;
	}

	if (Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Int ||
		Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Int64 ||
		Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Byte)
	{
		OutDescriptor->SetStringField(TEXT("kind"), TEXT("int"));
		OutDescriptor->SetNumberField(TEXT("value"), FCString::Atoi64(*Pin->DefaultValue));
		return true;
	}

	if (Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Real ||
		Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Float ||
		Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Double)
	{
		OutDescriptor->SetStringField(TEXT("kind"), TEXT("real"));
		OutDescriptor->SetNumberField(TEXT("value"), FCString::Atod(*Pin->DefaultValue));
		return true;
	}

	if (Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_String)
	{
		OutDescriptor->SetStringField(TEXT("kind"), TEXT("string"));
		OutDescriptor->SetStringField(TEXT("value"), Pin->DefaultValue);
		return true;
	}

	if (Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Name)
	{
		OutDescriptor->SetStringField(TEXT("kind"), TEXT("name"));
		OutDescriptor->SetStringField(TEXT("value"), Pin->DefaultValue);
		return true;
	}

	if (Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Enum)
	{
		OutDescriptor->SetStringField(TEXT("kind"), TEXT("enum"));
		OutDescriptor->SetStringField(TEXT("value"), Pin->DefaultValue);
		return true;
	}

	if (Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Object ||
		Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Class ||
		Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_SoftObject ||
		Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_SoftClass)
	{
		if (Pin->DefaultValue.Equals(TEXT("None"), ESearchCase::IgnoreCase) || Pin->DefaultValue.IsEmpty())
		{
			OutDescriptor->SetStringField(TEXT("kind"), TEXT("null"));
		}
		else if (Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Class ||
				 Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_SoftClass)
		{
			OutDescriptor->SetStringField(TEXT("kind"), TEXT("class"));
			OutDescriptor->SetStringField(TEXT("path"), Pin->DefaultValue);
		}
		else
		{
			OutDescriptor->SetStringField(TEXT("kind"), TEXT("object"));
			OutDescriptor->SetStringField(TEXT("path"), Pin->DefaultValue);
		}
		return true;
	}

	OutError = FCortexCommandRouter::Error(
		CortexErrorCodes::UnsupportedOperation,
		FString::Printf(TEXT("Pin category '%s' cannot be read as default descriptor"), *Pin->PinType.PinCategory.ToString()));
	return false;
}
