#include "Operations/CortexUMGPropertyBindingOps.h"
#include "CortexUMGUtils.h"
#include "CortexAssetFingerprint.h"
#include "CortexAssetMutationGuard.h"
#include "CortexCommandRouter.h"
#include "CortexEngineCompat.h"
#include "WidgetBlueprint.h"
#include "Blueprint/WidgetTree.h"
#include "Components/Widget.h"
#include "EdGraph/EdGraph.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "ScopedTransaction.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Serialization/MemoryWriter.h"
#include "Misc/PackageName.h"
#include "UObject/UnrealType.h"
#include "String/BytesToHex.h"
THIRD_PARTY_INCLUDES_START
#include <openssl/sha.h>
THIRD_PARTY_INCLUDES_END

namespace
{
const TCHAR* ReaderError = TEXT("PROPERTY_BINDING_READ_FAILED");

struct FBindingReader
{
	FObjectPropertyBase* Owner = nullptr;
	FNameProperty* Name = nullptr;
	FStructProperty* Guid = nullptr;
	FBoolProperty* IsProperty = nullptr;

	bool Initialize()
	{
		UScriptStruct* Type = FEditorPropertyPathSegment::StaticStruct();
		Owner = FindFProperty<FObjectPropertyBase>(Type, TEXT("Struct"));
		Name = FindFProperty<FNameProperty>(Type, TEXT("MemberName"));
		Guid = FindFProperty<FStructProperty>(Type, TEXT("MemberGuid"));
		IsProperty = FindFProperty<FBoolProperty>(Type, TEXT("IsProperty"));
		return FCortexUMGPropertyBindingOps::ValidateSegmentFields(Owner, Name, Guid, IsProperty);
	}

	TSharedPtr<FJsonObject> Record(const FDelegateEditorBinding& Binding, FArchive* Identity = nullptr) const
	{
		TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
		Data->SetStringField(TEXT("widget_name"), Binding.ObjectName);
		Data->SetStringField(TEXT("property_name"), Binding.PropertyName.ToString());
		Data->SetStringField(TEXT("kind"), Binding.Kind == EBindingKind::Property	? TEXT("property")
										   : Binding.Kind == EBindingKind::Function ? TEXT("function")
																					: TEXT("unknown"));
		Data->SetNumberField(TEXT("kind_value"), static_cast<uint8>(Binding.Kind));
		Data->SetStringField(TEXT("function_name"), Binding.FunctionName.ToString());
		Data->SetStringField(TEXT("source_property"), Binding.SourceProperty.ToString());
		Data->SetStringField(TEXT("member_guid"), Binding.MemberGuid.ToString(EGuidFormats::DigitsWithHyphens));
		if (Identity)
		{
			FString Widget = Binding.ObjectName, Property = Binding.PropertyName.ToString();
			FString Function = Binding.FunctionName.ToString(), Source = Binding.SourceProperty.ToString();
			FGuid MemberGuid = Binding.MemberGuid;
			uint8 Kind = static_cast<uint8>(Binding.Kind);
			int32 Count = Binding.SourcePath.Segments.Num();
			*Identity << Widget << Property << Function << Source << MemberGuid << Kind << Count;
		}
		TArray<TSharedPtr<FJsonValue>> Segments;
		Segments.Reserve(Binding.SourcePath.Segments.Num());
		for (const FEditorPropertyPathSegment& Segment : Binding.SourcePath.Segments)
		{
			const UObject* OwnerObject = Owner->GetObjectPropertyValue_InContainer(&Segment);
			FString OwnerPath = OwnerObject ? OwnerObject->GetPathName() : FString();
			FString MemberName = Name->GetPropertyValue_InContainer(&Segment).ToString();
			FGuid MemberGuid = *Guid->ContainerPtrToValuePtr<FGuid>(&Segment);
			bool bProperty = IsProperty->GetPropertyValue_InContainer(&Segment);
			TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
			Entry->SetStringField(TEXT("owner_path"), OwnerPath);
			Entry->SetStringField(TEXT("member_name"), MemberName);
			Entry->SetStringField(TEXT("member_guid"), MemberGuid.ToString(EGuidFormats::DigitsWithHyphens));
			Entry->SetBoolField(TEXT("is_property"), bProperty);
			Entry->SetBoolField(TEXT("resolved"), Segment.GetMember() != nullptr);
			Segments.Add(MakeShared<FJsonValueObject>(Entry));
			if (Identity)
			{
				*Identity << OwnerPath << MemberName << MemberGuid << bProperty;
			}
		}
		Data->SetArrayField(TEXT("source_path"), Segments);
		return Data;
	}

	bool Snapshot(UWidgetBlueprint* Blueprint, TArray<TSharedPtr<FJsonValue>>& Records,
				  TSharedPtr<FJsonObject>& Fingerprint) const
	{
		TArray<uint8> Identity;
		FMemoryWriter Writer(Identity);
		FString AssetPath = FPackageName::ObjectPathToPackageName(Blueprint->GetPathName());
		int32 Version = 1, Count = Blueprint->Bindings.Num();
		Writer << Version << AssetPath << Count;
		Records.Reserve(Count);
		for (const FDelegateEditorBinding& Binding : Blueprint->Bindings)
		{
			Records.Add(MakeShared<FJsonValueObject>(Record(Binding, &Writer)));
		}
		uint8 Digest[SHA256_DIGEST_LENGTH];
		if (!SHA256(Identity.GetData(), static_cast<size_t>(Identity.Num()), Digest))
		{
			return false;
		}
		Fingerprint = MakeObjectAssetFingerprint(Blueprint).ToJson();
		TSharedPtr<FJsonObject> Signature = MakeShared<FJsonObject>();
		Signature->SetNumberField(TEXT("version"), 1);
		Signature->SetStringField(TEXT("scope"), TEXT("umg.property_binding"));
		Signature->SetStringField(TEXT("asset_path"), AssetPath);
		Signature->SetStringField(TEXT("digest"), BytesToHex(Digest, SHA256_DIGEST_LENGTH).ToLower());
		Fingerprint->SetObjectField(TEXT("domain_signature"), Signature);
		return true;
	}
};

bool RejectPagination(const TSharedPtr<FJsonObject>& Params, FCortexCommandResult& Error)
{
	for (const TCHAR* Field : {TEXT("cursor"), TEXT("offset"), TEXT("limit")})
	{
		if (Params->HasField(Field))
		{
			Error =
				FCortexCommandRouter::Error(CortexErrorCodes::InvalidField,
											TEXT("Pagination fields are unsupported for property binding operations"));
			return true;
		}
	}
	return false;
}

bool VerifyGuard(const TSharedPtr<FJsonObject>& Expected, const TSharedPtr<FJsonObject>& Current,
				 FCortexCommandResult& Error)
{
	bool bMalformed = !Expected.IsValid(), bMatches = true;
	if (Expected.IsValid())
	{
		for (const TCHAR* Field : {TEXT("package_saved_hash"), TEXT("dirty_epoch")})
		{
			FString Value;
			if (!Expected->HasTypedField<EJson::String>(Field) || !Expected->TryGetStringField(Field, Value))
			{
				bMalformed = true;
			}
			else if (Value != Current->GetStringField(Field))
			{
				bMatches = false;
			}
		}
		for (const TCHAR* Field : {TEXT("is_dirty"), TEXT("not_ready")})
		{
			bool Value = false;
			if (!Expected->HasTypedField<EJson::Boolean>(Field) || !Expected->TryGetBoolField(Field, Value))
			{
				bMalformed = true;
			}
			else if (Value != Current->GetBoolField(Field))
			{
				bMatches = false;
			}
		}
		if (Current->HasField(TEXT("compiled_signature_crc")))
		{
			double Value = 0;
			if (!Expected->HasTypedField<EJson::Number>(TEXT("compiled_signature_crc")) ||
				!Expected->TryGetNumberField(TEXT("compiled_signature_crc"), Value))
			{
				bMalformed = true;
			}
			else if (Value != Current->GetNumberField(TEXT("compiled_signature_crc")))
			{
				bMatches = false;
			}
		}
		const TSharedPtr<FJsonObject>* Signature = nullptr;
		if (!Expected->TryGetObjectField(TEXT("domain_signature"), Signature) || !Signature->IsValid())
		{
			bMalformed = true;
		}
		else
		{
			const TSharedPtr<FJsonObject> Live = Current->GetObjectField(TEXT("domain_signature"));
			double Version = 0;
			if (!(*Signature)->HasTypedField<EJson::Number>(TEXT("version")) ||
				!(*Signature)->TryGetNumberField(TEXT("version"), Version) || Version != 1)
			{
				bMalformed = true;
			}
			for (const TCHAR* Field : {TEXT("scope"), TEXT("asset_path"), TEXT("digest")})
			{
				FString Value;
				if (!(*Signature)->HasTypedField<EJson::String>(Field) ||
					!(*Signature)->TryGetStringField(Field, Value))
				{
					bMalformed = true;
				}
				else if (Value != Live->GetStringField(Field))
				{
					bMatches = false;
				}
			}
		}
	}
	if (bMalformed || !bMatches)
	{
		Error = FCortexCommandRouter::Error(bMalformed ? CortexErrorCodes::InvalidField
													   : CortexErrorCodes::StalePrecondition,
											bMalformed ? TEXT("A complete property binding fingerprint is required")
													   : TEXT("Property binding state changed since inspection"));
		Error.AddContext(TEXT("current_fingerprint"), Current);
		return false;
	}
	return true;
}

bool BuildBinding(UWidgetBlueprint* Blueprint, UWidget* Widget, FName PropertyName,
				  const TSharedPtr<FJsonObject>& Request, FDelegateEditorBinding& Binding, FString& Error)
{
	FString Kind;
	if (!Request->HasTypedField<EJson::String>(TEXT("kind")) || !Request->TryGetStringField(TEXT("kind"), Kind))
	{
		Error = TEXT("binding.kind must be property or function");
		return false;
	}
	for (const auto& Field : Request->Values)
	{
		const FString FieldKey = CortexEngineCompat::JsonKeyToString(Field.Key);
		if (FieldKey != TEXT("kind") &&
			FieldKey != (Kind == TEXT("property") ? TEXT("source_path") : TEXT("function_name")))
		{
			Error = TEXT("Unknown or kind-incompatible binding field: ") + FieldKey;
			return false;
		}
	}
	const FString DelegateName = PropertyName.ToString() + TEXT("Delegate");
	if (!FindFProperty<FProperty>(Widget->GetClass(), PropertyName) || DelegateName.Len() >= NAME_SIZE)
	{
		Error = TEXT("Target is not a Designer-bindable attribute property");
		return false;
	}
	FDelegateProperty* Delegate = FindFProperty<FDelegateProperty>(Widget->GetClass(), FName(*DelegateName));
	if (!Delegate || !Delegate->SignatureFunction)
	{
		Error = TEXT("Target is not a Designer-bindable attribute property");
		return false;
	}
	if (!Blueprint->ArePropertyBindingsAllowed())
	{
		Error = TEXT("Designer property bindings are disabled for this Widget Blueprint; clearing remains permitted");
		return false;
	}
	UClass* SelfClass = Blueprint->SkeletonGeneratedClass;
	if (!SelfClass)
	{
		Error = TEXT("Compile source declarations explicitly before authoring a property binding");
		return false;
	}
	Binding.ObjectName = Widget->GetName();
	Binding.PropertyName = PropertyName;
	if (Kind == TEXT("property"))
	{
		const TArray<TSharedPtr<FJsonValue>>* Names = nullptr;
		if (!Request->TryGetArrayField(TEXT("source_path"), Names) || Names->IsEmpty())
		{
			Error = TEXT("Property binding requires a non-empty source_path of member names");
			return false;
		}
		UStruct* Owner = SelfClass;
		TArray<FFieldVariant> Chain;
		Chain.Reserve(Names->Num());
		for (int32 Index = 0; Index < Names->Num(); ++Index)
		{
			FString Name;
			if (!(*Names)[Index].IsValid() || (*Names)[Index]->Type != EJson::String ||
				!(*Names)[Index]->TryGetString(Name) || Name.IsEmpty() || Name.Len() >= NAME_SIZE)
			{
				Error = TEXT("Every source_path member must be a non-empty string");
				return false;
			}
			FProperty* Property = FindFProperty<FProperty>(Owner, FName(*Name));
			if (!Property)
			{
				Error = TEXT("Source member not found: ") + Name;
				return false;
			}
			Chain.Add(Property);
			if (Index + 1 < Names->Num())
			{
				if (FStructProperty* Struct = CastField<FStructProperty>(Property))
				{
					Owner = Struct->Struct;
				}
				else if (FObjectPropertyBase* Object = CastField<FObjectPropertyBase>(Property))
				{
					Owner = Object->PropertyClass;
				}
				else
				{
					Error = TEXT("Intermediate source members must be object or struct properties");
					return false;
				}
			}
		}
		Binding.Kind = EBindingKind::Property;
		Binding.SourcePath = FEditorPropertyPath(Chain);
		FText Validation;
		if (!Binding.SourcePath.Validate(Delegate, Validation))
		{
			Error = Validation.ToString();
			return false;
		}
		return true;
	}
	if (Kind == TEXT("function"))
	{
		FString Name;
		if (!Request->HasTypedField<EJson::String>(TEXT("function_name")) ||
			!Request->TryGetStringField(TEXT("function_name"), Name) || Name.IsEmpty() || Name.Len() >= NAME_SIZE)
		{
			Error = TEXT("Function binding requires function_name");
			return false;
		}
		UFunction* Function = SelfClass->FindFunctionByName(FName(*Name));
		if (!Function || !Function->HasAnyFunctionFlags(FUNC_Const | FUNC_BlueprintPure) ||
			!Function->IsSignatureCompatibleWith(Delegate->SignatureFunction,
												 UFunction::GetDefaultIgnoredSignatureCompatibilityFlags() |
													 CPF_ReturnParm))
		{
			Error = TEXT("Binding function must exist, be pure/const, and match the attribute delegate signature");
			return false;
		}
		Binding.Kind = EBindingKind::Function;
		Binding.FunctionName = Function->GetFName();
		for (const UEdGraph* Graph : Blueprint->FunctionGraphs)
		{
			if (Graph && Graph->GetFName() == Binding.FunctionName)
			{
				Binding.MemberGuid = Graph->GraphGuid;
			}
		}
		return true;
	}
	Error = TEXT("binding.kind must be property or function");
	return false;
}
} // namespace
bool FCortexUMGPropertyBindingOps::ValidateSegmentFields(const FProperty* Owner, const FProperty* Name,
														 const FProperty* Guid, const FProperty* IsProperty)
{
	const FObjectPropertyBase* OwnerField = CastField<FObjectPropertyBase>(Owner);
	const FStructProperty* GuidField = CastField<FStructProperty>(Guid);
	return OwnerField && OwnerField->PropertyClass == UStruct::StaticClass() && CastField<FNameProperty>(Name) &&
		   GuidField && GuidField->Struct == TBaseStructure<FGuid>::Get() && CastField<FBoolProperty>(IsProperty);
}

bool FCortexUMGPropertyBindingOps::RestoreBindingArray(UWidgetBlueprint* Blueprint,
													   const TArray<FDelegateEditorBinding>& Original, bool bWasDirty)
{
	Blueprint->Bindings = Original;
	Blueprint->GetPackage()->SetDirtyFlag(bWasDirty);
	if (Blueprint->Bindings.Num() != Original.Num() || Blueprint->GetPackage()->IsDirty() != bWasDirty)
	{
		return false;
	}
	for (int32 Index = 0; Index < Original.Num(); ++Index)
	{
		if (!FDelegateEditorBinding::StaticStruct()->CompareScriptStruct(&Blueprint->Bindings[Index], &Original[Index],
																		 PPF_None))
		{
			return false;
		}
	}
	return true;
}

bool FCortexUMGPropertyBindingOps::AppendInspection(UWidgetBlueprint* Blueprint, const TSharedPtr<FJsonObject>& Params,
													const FString* WidgetName, const TSharedPtr<FJsonObject>& Data,
													FCortexCommandResult& OutError)
{
	if (!Params->HasField(TEXT("include_property_bindings")))
	{
		return true;
	}
	bool bInclude = false;
	if (!Params->HasTypedField<EJson::Boolean>(TEXT("include_property_bindings")) ||
		!Params->TryGetBoolField(TEXT("include_property_bindings"), bInclude))
	{
		OutError = FCortexCommandRouter::Error(CortexErrorCodes::InvalidField,
											   TEXT("include_property_bindings must be boolean"));
		return false;
	}
	if (!bInclude)
	{
		return true;
	}
	if (RejectPagination(Params, OutError))
	{
		return false;
	}
	FBindingReader Reader;
	TArray<TSharedPtr<FJsonValue>> AllRecords;
	TSharedPtr<FJsonObject> Fingerprint;
	if (!Reader.Initialize() || !Reader.Snapshot(Blueprint, AllRecords, Fingerprint))
	{
		OutError = FCortexCommandRouter::Error(ReaderError,
											   TEXT("Complete serialized property binding identity is unavailable"));
		return false;
	}
	TArray<TSharedPtr<FJsonValue>> Records, Diagnostics;
	Records.Reserve(AllRecords.Num());
	for (int32 Index = 0; Index < AllRecords.Num(); ++Index)
	{
		const FDelegateEditorBinding& Binding = Blueprint->Bindings[Index];
		if (WidgetName && !Binding.ObjectName.Equals(*WidgetName, ESearchCase::CaseSensitive))
		{
			continue;
		}
		Records.Add(AllRecords[Index]);
		if (!CortexUMGUtils::FindWidgetByName(Blueprint->WidgetTree, Binding.ObjectName, ESearchCase::CaseSensitive))
		{
			Diagnostics.Add(MakeShared<FJsonValueString>(TEXT("Missing target widget: ") + Binding.ObjectName));
		}
	}
	TSharedPtr<FJsonObject> State = MakeShared<FJsonObject>();
	State->SetBoolField(TEXT("reader_complete"), true);
	State->SetStringField(TEXT("scope"), WidgetName ? TEXT("widget") : TEXT("asset"));
	State->SetNumberField(TEXT("total"), Records.Num());
	State->SetArrayField(TEXT("bindings"), Records);
	State->SetArrayField(TEXT("diagnostics"), Diagnostics);
	State->SetObjectField(TEXT("fingerprint"), Fingerprint);
	Data->SetObjectField(TEXT("property_binding_state"), State);
	return true;
}

FCortexCommandResult FCortexUMGPropertyBindingOps::SetPropertyBinding(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath, WidgetName, PropertyName;
	if (!Params.IsValid() || !Params->HasTypedField<EJson::String>(TEXT("asset_path")) ||
		!Params->HasTypedField<EJson::String>(TEXT("widget_name")) ||
		!Params->HasTypedField<EJson::String>(TEXT("property_name")) ||
		!Params->TryGetStringField(TEXT("asset_path"), AssetPath) || AssetPath.IsEmpty() ||
		!Params->TryGetStringField(TEXT("widget_name"), WidgetName) || WidgetName.IsEmpty() ||
		!Params->TryGetStringField(TEXT("property_name"), PropertyName) || PropertyName.IsEmpty())
	{
		return FCortexCommandRouter::Error(
			CortexErrorCodes::InvalidField,
			TEXT("asset_path, widget_name and property_name are required non-empty strings"));
	}
	int32 ObjectSeparator = INDEX_NONE;
	AssetPath.FindChar(TEXT('.'), ObjectSeparator);
	const int32 PackageLength = ObjectSeparator == INDEX_NONE ? AssetPath.Len() : ObjectSeparator;
	const int32 ObjectLength = ObjectSeparator == INDEX_NONE ? 0 : AssetPath.Len() - ObjectSeparator - 1;
	if (PackageLength >= NAME_SIZE || ObjectLength >= NAME_SIZE || WidgetName.Len() >= NAME_SIZE ||
		PropertyName.Len() >= NAME_SIZE)
	{
		return FCortexCommandRouter::Error(CortexErrorCodes::InvalidField,
										   TEXT("Target names exceed Unreal's native name bounds"));
	}
	FCortexCommandResult Error;
	if (RejectPagination(Params, Error))
	{
		return Error;
	}
	const TSharedPtr<FJsonValue> Request = Params->TryGetField(TEXT("binding"));
	if (!Request.IsValid() || (Request->Type != EJson::Null && Request->Type != EJson::Object))
	{
		return FCortexCommandRouter::Error(
			CortexErrorCodes::InvalidField,
			TEXT("binding is required: explicit null clears; object creates or replaces"));
	}
	FString BlockReason;
	if (FCortexAssetMutationGuard::IsPathBlocked(AssetPath, BlockReason))
	{
		return FCortexCommandRouter::Error(CortexErrorCodes::InvalidOperation, BlockReason);
	}
	UWidgetBlueprint* Blueprint = CortexUMGUtils::LoadWidgetBlueprint(AssetPath, Error);
	if (!Blueprint)
	{
		return Error;
	}
	UWidget* Widget =
		CortexUMGUtils::FindWidgetByName(Blueprint->WidgetTree, WidgetName, ESearchCase::CaseSensitive);
	if (!Widget)
	{
		return FCortexCommandRouter::Error(CortexErrorCodes::WidgetNotFound,
										   TEXT("Target Designer widget does not exist"));
	}
	FBindingReader Reader;
	TArray<TSharedPtr<FJsonValue>> BeforeRecords;
	TSharedPtr<FJsonObject> Fingerprint;
	if (!Reader.Initialize() || !Reader.Snapshot(Blueprint, BeforeRecords, Fingerprint))
	{
		return FCortexCommandRouter::Error(ReaderError,
										   TEXT("Complete serialized property binding reader is unavailable"));
	}
	const TSharedPtr<FJsonObject>* Expected = nullptr;
	if (!Params->TryGetObjectField(TEXT("expected_fingerprint"), Expected))
	{
		return FCortexCommandRouter::Error(CortexErrorCodes::InvalidField, TEXT("expected_fingerprint is required"));
	}
	if (!VerifyGuard(*Expected, Fingerprint, Error))
	{
		return Error;
	}
	int32 Match = INDEX_NONE;
	const FName TargetPropertyName(*PropertyName);
	for (int32 Index = 0; Index < Blueprint->Bindings.Num(); ++Index)
	{
		const FDelegateEditorBinding& Binding = Blueprint->Bindings[Index];
		if (Binding.ObjectName.Equals(WidgetName, ESearchCase::CaseSensitive) &&
			Binding.PropertyName == TargetPropertyName)
		{
			if (Match != INDEX_NONE)
			{
				return FCortexCommandRouter::Error(TEXT("PROPERTY_BINDING_AMBIGUOUS"),
												   TEXT("Multiple serialized records match the exact target"));
			}
			Match = Index;
		}
	}
	const bool bClear = Request->Type == EJson::Null;
	FDelegateEditorBinding Authored;
	if (!bClear)
	{
		FString Validation;
		if (!BuildBinding(Blueprint, Widget, TargetPropertyName, Request->AsObject(), Authored, Validation))
		{
			return FCortexCommandRouter::Error(CortexErrorCodes::InvalidField, Validation);
		}
	}
	const bool bChanged = bClear ? Match != INDEX_NONE
								 : Match == INDEX_NONE || !FDelegateEditorBinding::StaticStruct()->CompareScriptStruct(
															  &Blueprint->Bindings[Match], &Authored, PPF_None);
	TSharedPtr<FJsonValue> Before =
		Match == INDEX_NONE ? TSharedPtr<FJsonValue>(MakeShared<FJsonValueNull>()) : BeforeRecords[Match];
	TSharedPtr<FJsonValue> After = Before;
	if (bChanged)
	{
		const TArray<FDelegateEditorBinding> Original = Blueprint->Bindings;
		const bool bWasDirty = Blueprint->GetPackage()->IsDirty();
		FScopedTransaction Transaction(NSLOCTEXT("CortexUMG", "SetPropertyBinding", "Set UMG Property Binding"));
		Blueprint->SetFlags(RF_Transactional);
		Blueprint->Modify();
		if (bClear)
		{
			Blueprint->Bindings.RemoveAt(Match);
		}
		else if (Match == INDEX_NONE)
		{
			Blueprint->Bindings.Add(Authored);
		}
		else
		{
			Blueprint->Bindings[Match] = Authored;
		}
		bool bVerified = Blueprint->Bindings.Num() == Original.Num() + (bClear ? -1 : Match == INDEX_NONE ? 1 : 0);
		for (int32 Index = 0; bVerified && Index < Blueprint->Bindings.Num(); ++Index)
		{
			const int32 OriginalIndex = bClear && Index >= Match ? Index + 1 : Index;
			const bool bAuthoredIndex = !bClear && Index == (Match == INDEX_NONE ? Original.Num() : Match);
			const FDelegateEditorBinding& Wanted = bAuthoredIndex ? Authored : Original[OriginalIndex];
			bVerified = FDelegateEditorBinding::StaticStruct()->CompareScriptStruct(&Blueprint->Bindings[Index],
																					&Wanted, PPF_None);
		}
		TArray<TSharedPtr<FJsonValue>> PostRecords;
		TSharedPtr<FJsonObject> PostFingerprint;
		bVerified = bVerified && Reader.Snapshot(Blueprint, PostRecords, PostFingerprint);
		if (!bVerified)
		{
			const bool bRestored = RestoreBindingArray(Blueprint, Original, bWasDirty);
			Transaction.Cancel();
			if (!bRestored)
			{
				FCortexAssetMutationGuard::Block(Blueprint,
												 TEXT("Serialized property binding recovery could not be verified"));
			}
			return FCortexCommandRouter::Error(
				ReaderError, bRestored ? TEXT("Serialized post-state mismatched; original bindings restored")
									   : TEXT("Serialized recovery failed; further asset mutation is blocked"));
		}
		// Capture serialized readback before notifying Blueprint observers.
		After = bClear ? TSharedPtr<FJsonValue>(MakeShared<FJsonValueNull>())
					   : PostRecords[Match == INDEX_NONE ? Original.Num() : Match];
		FBlueprintEditorUtils::MarkBlueprintAsModified(Blueprint);
		TSharedPtr<FJsonObject> Refreshed = MakeObjectAssetFingerprint(Blueprint).ToJson();
		Refreshed->SetObjectField(TEXT("domain_signature"), PostFingerprint->GetObjectField(TEXT("domain_signature")));
		Fingerprint = Refreshed;
	}
	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("asset_path"), Blueprint->GetPathName());
	Data->SetStringField(TEXT("widget_name"), WidgetName);
	Data->SetStringField(TEXT("property_name"), PropertyName);
	Data->SetBoolField(TEXT("changed"), bChanged);
	Data->SetField(TEXT("before_binding"), Before);
	Data->SetField(TEXT("binding"), After);
	Data->SetBoolField(TEXT("reader_complete"), true);
	Data->SetObjectField(TEXT("fingerprint"), Fingerprint);
	Data->SetBoolField(TEXT("compiled"), false);
	Data->SetBoolField(TEXT("saved"), false);
	return FCortexCommandRouter::Success(Data);
}
