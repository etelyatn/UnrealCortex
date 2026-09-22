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

namespace
{
/** Native storage mode a reference pin can hold, derived from its category. */
enum class EPinReferenceMode : uint8
{
	None,
	HardObject,
	SoftObject,
	HardClass,
	SoftClass,
};

EPinReferenceMode ReferenceModeForCategory(const FName Category)
{
	if (Category == UEdGraphSchema_K2::PC_Class) return EPinReferenceMode::HardClass;
	if (Category == UEdGraphSchema_K2::PC_SoftClass) return EPinReferenceMode::SoftClass;
	if (Category == UEdGraphSchema_K2::PC_SoftObject) return EPinReferenceMode::SoftObject;
	if (Category == UEdGraphSchema_K2::PC_Object || Category == UEdGraphSchema_K2::PC_Interface) return EPinReferenceMode::HardObject;
	return EPinReferenceMode::None;
}

EPinReferenceMode RequestedReferenceMode(const FString& Kind)
{
	if (Kind == TEXT("class")) return EPinReferenceMode::HardClass;
	if (Kind == TEXT("soft_class")) return EPinReferenceMode::SoftClass;
	if (Kind == TEXT("soft_object")) return EPinReferenceMode::SoftObject;
	if (Kind == TEXT("object")) return EPinReferenceMode::HardObject;
	return EPinReferenceMode::None;
}

bool IsSoftReferenceMode(const EPinReferenceMode Mode)
{
	return Mode == EPinReferenceMode::SoftObject || Mode == EPinReferenceMode::SoftClass;
}

const TCHAR* ReferenceModeName(const EPinReferenceMode Mode)
{
	switch (Mode)
	{
	case EPinReferenceMode::HardClass:
		return TEXT("class");
	case EPinReferenceMode::SoftClass:
		return TEXT("soft_class");
	case EPinReferenceMode::SoftObject:
		return TEXT("soft_object");
	case EPinReferenceMode::HardObject:
		return TEXT("object");
	default:
		return TEXT("none");
	}
}

/**
 * Canonical native string for a reference default path. Soft references live in DefaultValue as a
 * path, so both the stored value and the compared value go through the same canonicalization.
 */
FString CanonicalReferencePath(const EPinReferenceMode Mode, const FString& Path)
{
	if (Path.IsEmpty() || Path.Equals(TEXT("None"), ESearchCase::IgnoreCase))
	{
		return TEXT("None");
	}
	if (Mode == EPinReferenceMode::HardClass || Mode == EPinReferenceMode::SoftClass)
	{
		UClass* ResolvedClass = nullptr;
		FCortexCommandResult ResolveError;
		if (FCortexGraphSymbolResolver::ResolveClass(Path, ResolvedClass, ResolveError) && ResolvedClass)
		{
			return ResolvedClass->GetPathName();
		}
		return Path;
	}
	if (UObject* Found = FindObject<UObject>(nullptr, *Path))
	{
		return Found->GetPathName();
	}
	return Path;
}

/**
 * Canonical identity of a text literal: string table identity (table id + key) or the
 * namespace/key/source triple of a literal. Display text alone is not an identity, because a
 * string table entry and a literal can share it, and so can two different tables.
 */
FString CanonicalTextIdentity(const FText& Text)
{
	// Fields are length-prefixed so no separator inside a field can forge another identity.
	if (Text.IsFromStringTable())
	{
		FName TableId;
		FTextKey TableKey;
		FTextInspector::GetTableIdAndKey(Text, TableId, TableKey);
		const FString TableIdText = TableId.ToString();
		const FString TableKeyText = TableKey.ToString();
		return FString::Printf(TEXT("table:%d:%s|%d:%s"), TableIdText.Len(), *TableIdText,
			TableKeyText.Len(), *TableKeyText);
	}
	const TOptional<FString> Namespace = FTextInspector::GetNamespace(Text);
	const TOptional<FString> Key = FTextInspector::GetKey(Text);
	const FString NamespaceText = Namespace.IsSet() ? Namespace.GetValue() : FString();
	const FString KeyText = Key.IsSet() ? Key.GetValue() : FString();
	const FString* SourceString = FTextInspector::GetSourceString(Text);
	const FString SourceText = SourceString ? **SourceString : FString();
	return FString::Printf(TEXT("literal:%d:%s|%d:%s|%d:%s"), NamespaceText.Len(), *NamespaceText,
		KeyText.Len(), *KeyText, SourceText.Len(), *SourceText);
}
}

const TCHAR* FCortexGraphPinDefaults::ReferenceLiteralKind(const UEdGraphPin& Pin)
{
	switch (ReferenceModeForCategory(Pin.PinType.PinCategory))
	{
	case EPinReferenceMode::HardClass:
		return TEXT("class");
	case EPinReferenceMode::SoftClass:
		return TEXT("soft_class");
	case EPinReferenceMode::SoftObject:
		return TEXT("soft_object");
	default:
		return TEXT("object");
	}
}

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
		const EPinReferenceMode RequestedMode = RequestedReferenceMode(Kind);
		const EPinReferenceMode NativeMode = ReferenceModeForCategory(Pin->PinType.PinCategory);
		if (NativeMode != RequestedMode)
		{
			if (NativeMode == EPinReferenceMode::HardClass)
			{
				OutError = FCortexCommandRouter::Error(
					CortexErrorCodes::InvalidField,
					TEXT("Cannot assign soft class literal to hard class pin: unsupported soft-reference mode"));
			}
			else if (NativeMode == EPinReferenceMode::SoftClass)
			{
				OutError = FCortexCommandRouter::Error(
					CortexErrorCodes::InvalidField,
					TEXT("Cannot assign hard class literal to soft class pin: use a soft_class literal"));
			}
			else
			{
				OutError = FCortexCommandRouter::Error(
					CortexErrorCodes::TypeMismatch,
					FString::Printf(TEXT("Cannot assign class literal to pin of category '%s'"), *Pin->PinType.PinCategory.ToString()));
			}
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
		const EPinReferenceMode RequestedMode = RequestedReferenceMode(Kind);
		const EPinReferenceMode NativeMode = ReferenceModeForCategory(Pin->PinType.PinCategory);
		if (NativeMode != RequestedMode)
		{
			if (NativeMode == EPinReferenceMode::HardObject)
			{
				OutError = FCortexCommandRouter::Error(
					CortexErrorCodes::InvalidField,
					TEXT("Cannot assign soft object literal to hard object pin: unsupported soft-reference mode"));
			}
			else if (NativeMode == EPinReferenceMode::SoftObject)
			{
				OutError = FCortexCommandRouter::Error(
					CortexErrorCodes::InvalidField,
					TEXT("Cannot assign hard object literal to soft object pin: use a soft_object literal"));
			}
			else
			{
				OutError = FCortexCommandRouter::Error(
					CortexErrorCodes::TypeMismatch,
					FString::Printf(TEXT("Cannot assign object literal to pin of category '%s'"), *Pin->PinType.PinCategory.ToString()));
			}
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
		else if (IsSoftReferenceMode(RequestedReferenceMode(Kind)))
		{
			// Soft references stay soft and live in DefaultValue as a canonical path.
			Pin->DefaultObject = nullptr;
			Pin->DefaultValue = CanonicalReferencePath(EPinReferenceMode::SoftClass, Path);
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
		else if (IsSoftReferenceMode(RequestedReferenceMode(Kind)))
		{
			// Preserve soft references as soft references.
			Pin->DefaultObject = nullptr;
			Pin->DefaultValue = CanonicalReferencePath(EPinReferenceMode::SoftObject, Path);
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
		Pin->DefaultValue = FString::Printf(TEXT("%.17g"), RealVal);
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

	const TCHAR* ReferenceKind = ReferenceLiteralKind(*Pin);

	if (Pin->DefaultObject != nullptr)
	{
		OutDescriptor->SetStringField(TEXT("kind"), ReferenceKind);
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

	const bool bIsEnumPin = Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Enum ||
		(Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Byte && Pin->PinType.PinSubCategoryObject.IsValid());
	if (bIsEnumPin)
	{
		OutDescriptor->SetStringField(TEXT("kind"), TEXT("enum"));
		OutDescriptor->SetStringField(TEXT("value"), Pin->DefaultValue);
		return true;
	}

	if (Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Int ||
		Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Int64 ||
		(Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Byte && !Pin->PinType.PinSubCategoryObject.IsValid()))
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

	if (Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Object ||
		Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Class ||
		Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_SoftObject ||
		Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_SoftClass)
	{
		if (Pin->DefaultValue.Equals(TEXT("None"), ESearchCase::IgnoreCase) || Pin->DefaultValue.IsEmpty())
		{
			OutDescriptor->SetStringField(TEXT("kind"), TEXT("null"));
		}
		else
		{
			OutDescriptor->SetStringField(TEXT("kind"), ReferenceKind);
			OutDescriptor->SetStringField(TEXT("path"), Pin->DefaultValue);
		}
		return true;
	}

	OutError = FCortexCommandRouter::Error(
		CortexErrorCodes::UnsupportedOperation,
		FString::Printf(TEXT("Pin category '%s' cannot be read as default descriptor"), *Pin->PinType.PinCategory.ToString()));
	return false;
}

/**
 * Compares one tagged literal descriptor against the pin's native default state.
 */
bool FCortexGraphPinDefaults::CompareAppliedLiteral(
	const UEdGraphPin* Pin,
	const TSharedPtr<FJsonObject>& Literal,
	FString& OutExpected,
	FString& OutActual,
	FString& OutFailure)
{
	OutExpected.Reset();
	OutActual.Reset();
	if (!Pin || !Literal.IsValid())
	{
		OutFailure = TEXT("planned default no longer resolves to a pin and literal");
		return false;
	}
	FString Kind;
	if (!Literal->TryGetStringField(TEXT("kind"), Kind) || Kind.IsEmpty())
	{
		OutFailure = TEXT("planned default has no literal kind");
		return false;
	}

	// The planned kind must still match the pin's category: a native pin of another category that
	// happens to carry the same default text is never a match.
	FCortexCommandResult KindError;
	if (!Validate(Pin, Literal, KindError))
	{
		OutFailure = FString::Printf(TEXT("planned default no longer validates against the pin: %s"),
			*KindError.ErrorMessage);
		return false;
	}

	if (Kind == TEXT("class") || Kind == TEXT("soft_class") || Kind == TEXT("object") || Kind == TEXT("soft_object"))
	{
		const EPinReferenceMode RequestedMode = RequestedReferenceMode(Kind);
		const EPinReferenceMode NativeMode = ReferenceModeForCategory(Pin->PinType.PinCategory);
		if (RequestedMode == EPinReferenceMode::None || NativeMode != RequestedMode)
		{
			OutFailure = FString::Printf(TEXT("pin category '%s' cannot hold a %s default"),
				*Pin->PinType.PinCategory.ToString(), *Kind);
			return false;
		}

		FString Path;
		Literal->TryGetStringField(TEXT("path"), Path);
		if (!Path.IsEmpty() && !Path.Equals(TEXT("None"), ESearchCase::IgnoreCase)
			&& (RequestedMode == EPinReferenceMode::HardClass || RequestedMode == EPinReferenceMode::SoftClass))
		{
			UClass* ResolvedClass = nullptr;
			FCortexCommandResult ResolveError;
			if (!FCortexGraphSymbolResolver::ResolveClass(Path, ResolvedClass, ResolveError) || !ResolvedClass)
			{
				OutFailure = FString::Printf(TEXT("planned class default no longer resolves: %s"), *ResolveError.ErrorMessage);
				return false;
			}
		}

		// Native storage must agree with the mode before the identities are compared: a soft
		// reference never also carries a hard object, and a hard reference stores its value as the
		// native object rather than as competing default text.
		if (IsSoftReferenceMode(NativeMode))
		{
			if (Pin->DefaultObject)
			{
				OutFailure = FString::Printf(
					TEXT("soft reference pin '%s' also stores hard object '%s'"),
					*Pin->PinName.ToString(), *Pin->DefaultObject->GetPathName());
				return false;
			}
		}
		else if (Pin->DefaultObject)
		{
			if (!Pin->DefaultValue.IsEmpty() && !Pin->DefaultValue.Equals(TEXT("None"), ESearchCase::IgnoreCase))
			{
				OutFailure = FString::Printf(TEXT("hard reference pin '%s' stores competing default text '%s'"),
					*Pin->PinName.ToString(), *Pin->DefaultValue);
				return false;
			}
		}
		else if (!Path.IsEmpty() && !Path.Equals(TEXT("None"), ESearchCase::IgnoreCase))
		{
			OutFailure = FString::Printf(TEXT("hard reference pin '%s' does not store its default as a native object"),
				*Pin->PinName.ToString());
			return false;
		}

		// Soft references live in DefaultValue; hard references are the DefaultObject.
		const FString NativePath = IsSoftReferenceMode(NativeMode)
			? Pin->DefaultValue
			: (Pin->DefaultObject ? Pin->DefaultObject->GetPathName() : Pin->DefaultValue);
		OutExpected = FString::Printf(TEXT("%s:%s"), ReferenceModeName(RequestedMode),
			*CanonicalReferencePath(RequestedMode, Path));
		OutActual = FString::Printf(TEXT("%s:%s"), ReferenceModeName(NativeMode),
			*CanonicalReferencePath(NativeMode, NativePath));
		return true;
	}
	if (Kind == TEXT("null"))
	{
		OutExpected = TEXT("None");
		OutActual = (Pin->DefaultObject == nullptr && (Pin->DefaultValue.IsEmpty() || Pin->DefaultValue.Equals(TEXT("None"), ESearchCase::IgnoreCase)))
			? FString(TEXT("None"))
			: Pin->DefaultValue;
		return true;
	}
	if (Kind == TEXT("text"))
	{
		FText ExpectedText;
		bool bHasExpectedText = false;
		FString LiteralValue;
		if (Literal->TryGetStringField(TEXT("literal"), LiteralValue))
		{
			ExpectedText = FText::FromString(LiteralValue);
			bHasExpectedText = true;
		}
		else if (Literal->HasField(TEXT("table")) || Literal->HasField(TEXT("table_id")))
		{
			FString TableId;
			FString Key;
			if (!Literal->TryGetStringField(TEXT("table"), TableId))
			{
				Literal->TryGetStringField(TEXT("table_id"), TableId);
			}
			Literal->TryGetStringField(TEXT("key"), Key);
			ExpectedText = FText::FromStringTable(FName(*TableId), Key);
			bHasExpectedText = true;
		}
		else
		{
			TArray<FString> Errors;
			TSharedPtr<FJsonObject> Normalized;
			FText NormalizedText;
			const TSharedPtr<FJsonValue> Value = Literal->HasField(TEXT("value"))
				? Literal->TryGetField(TEXT("value"))
				: MakeShared<FJsonValueObject>(Literal);
			if (Value.IsValid() && FCortexSerializer::NormalizeTextDescriptor(Value, Normalized, &NormalizedText, Errors))
			{
				ExpectedText = NormalizedText;
				bHasExpectedText = true;
			}
		}
		if (!bHasExpectedText)
		{
			OutFailure = TEXT("planned text default cannot be compared with native readback");
			return false;
		}
		OutExpected = CanonicalTextIdentity(ExpectedText);
		OutActual = CanonicalTextIdentity(Pin->DefaultTextValue);
		return true;
	}
	if (Kind == TEXT("bool"))
	{
		bool bValue = false;
		if (!Literal->TryGetBoolField(TEXT("value"), bValue))
		{
			FString Text;
			Literal->TryGetStringField(TEXT("value"), Text);
			bValue = Text.ToBool();
		}
		OutExpected = bValue ? TEXT("true") : TEXT("false");
		OutActual = Pin->DefaultValue.ToBool() ? TEXT("true") : TEXT("false");
		return true;
	}
	if (Kind == TEXT("int"))
	{
		int64 Value = 0;
		if (!Literal->TryGetNumberField(TEXT("value"), Value))
		{
			FString Text;
			Literal->TryGetStringField(TEXT("value"), Text);
			Value = FCString::Atoi64(*Text);
		}
		OutExpected = FString::Printf(TEXT("%lld"), Value);
		OutActual = FString::Printf(TEXT("%lld"), FCString::Atoi64(*Pin->DefaultValue));
		return true;
	}
	if (Kind == TEXT("real") || Kind == TEXT("float"))
	{
		double Value = 0.0;
		if (!Literal->TryGetNumberField(TEXT("value"), Value))
		{
			FString Text;
			Literal->TryGetStringField(TEXT("value"), Text);
			Value = FCString::Atod(*Text);
		}
		OutExpected = FString::Printf(TEXT("%.17g"), Value);
		OutActual = FString::Printf(TEXT("%.17g"), FCString::Atod(*Pin->DefaultValue));
		return true;
	}
	if (Kind == TEXT("string") || Kind == TEXT("name") || Kind == TEXT("enum"))
	{
		FString Value;
		Literal->TryGetStringField(TEXT("value"), Value);
		OutExpected = Value;
		OutActual = Pin->DefaultValue;
		return true;
	}

	OutFailure = FString::Printf(TEXT("planned default kind '%s' has no native readback comparison"), *Kind);
	return false;
}
