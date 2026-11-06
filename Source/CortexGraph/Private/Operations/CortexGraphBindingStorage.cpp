#include "CortexGraphBindingStorage.h"

#include "Algo/Sort.h"
#include "EdGraph/EdGraphNode.h"
#include "K2Node.h"
#include "UObject/Class.h"
#include "UObject/UObjectGlobals.h"
#include "UObject/UnrealType.h"

namespace CortexGraphBindingStoragePrivate
{
	constexpr const TCHAR* NodeBaseClassPath = TEXT("/Script/AnimGraph.AnimGraphNode_Base");
	constexpr const TCHAR* DefaultBindingClassPath = TEXT("/Script/AnimGraph.AnimGraphNodeBinding_Base");
	constexpr const TCHAR* PropertyAccessClassPath = TEXT("/Script/PropertyAccessNode.K2Node_PropertyAccess");
	constexpr const TCHAR* BindingStructName = TEXT("AnimGraphNodePropertyBinding");

	constexpr const TCHAR* FieldNodeBinding = TEXT("AnimGraphNode_Base.Binding");
	constexpr const TCHAR* FieldBindingMap = TEXT("AnimGraphNodeBinding_Base.PropertyBindings");
	constexpr const TCHAR* FieldAccessPath = TEXT("K2Node_PropertyAccess.Path");
	constexpr const TCHAR* FieldAccessTextPath = TEXT("K2Node_PropertyAccess.TextPath");

	/** The three public function slots of an anim node (UPROPERTY names on AnimGraphNode_Base). */
	const TCHAR* const SlotNames[] = {
		TEXT("InitialUpdateFunction"),
		TEXT("BecomeRelevantFunction"),
		TEXT("UpdateFunction"),
	};

#if WITH_AUTOMATION_TESTS
	TMap<FString, FName>& NameOverrides()
	{
		static TMap<FString, FName> Overrides;
		return Overrides;
	}
#endif

	/** The property name a logical field is looked up under (the real name unless a test overrides it). */
	FName ActualName(const TCHAR* LogicalField, const TCHAR* RealName)
	{
#if WITH_AUTOMATION_TESTS
		if (const FName* Override = NameOverrides().Find(LogicalField))
		{
			return *Override;
		}
#endif
		return FName(RealName);
	}

	/** Each reflected field, resolved by name once (until a test hook changes the name table). */
	struct FFields
	{
		bool bResolved = false;

		const FObjectProperty* Binding = nullptr;
		const UClass* DefaultBindingClass = nullptr;
		const FMapProperty* BindingMap = nullptr;
		const FNameProperty* BindingMapKey = nullptr;
		const FStructProperty* BindingMapValue = nullptr;
		const FNameProperty* EntryName = nullptr;
		const FIntProperty* EntryArrayIndex = nullptr;
		const FEnumProperty* EntryType = nullptr;
		const FBoolProperty* EntryBound = nullptr;
		const FBoolProperty* EntryPromotion = nullptr;
		const FBoolProperty* EntryOnlyActive = nullptr;
		const FArrayProperty* EntryPath = nullptr;
		const FStrProperty* EntryPathElement = nullptr;
		const FTextProperty* EntryPathText = nullptr;
		FString BindingMissing;

		const UClass* PropertyAccessClass = nullptr;
		const FArrayProperty* AccessPath = nullptr;
		const FStrProperty* AccessPathElement = nullptr;
		const FTextProperty* AccessTextPath = nullptr;
		FString AccessMissing;
	};

	FFields& Fields()
	{
		static FFields Resolved;
		return Resolved;
	}

	template <typename T>
	const T* EntryField(const UScriptStruct* Struct, const TCHAR* Name)
	{
		return CastField<T>(Struct->FindPropertyByName(Name));
	}

	void ResolveBinding(FFields& F)
	{
		const UClass* NodeBase = FindObject<UClass>(nullptr, NodeBaseClassPath);
		F.Binding = NodeBase != nullptr ? FindFProperty<FObjectProperty>(NodeBase, ActualName(FieldNodeBinding, TEXT("Binding"))) : nullptr;
		if (F.Binding == nullptr)
		{
			F.BindingMissing = FieldNodeBinding;
			return;
		}
		F.DefaultBindingClass = FindObject<UClass>(nullptr, DefaultBindingClassPath);
		if (F.DefaultBindingClass == nullptr)
		{
			F.BindingMissing = DefaultBindingClassPath;
			return;
		}

		F.BindingMap = CastField<FMapProperty>(FindFProperty<FProperty>(F.DefaultBindingClass, ActualName(FieldBindingMap, TEXT("PropertyBindings"))));
		F.BindingMapKey = F.BindingMap != nullptr ? CastField<FNameProperty>(F.BindingMap->KeyProp) : nullptr;
		F.BindingMapValue = F.BindingMap != nullptr ? CastField<FStructProperty>(F.BindingMap->ValueProp) : nullptr;
		if (F.BindingMapKey == nullptr || F.BindingMapValue == nullptr || F.BindingMapValue->Struct->GetName() != BindingStructName)
		{
			F.BindingMap = nullptr;
			F.BindingMapKey = nullptr;
			F.BindingMapValue = nullptr;
			F.BindingMissing = FieldBindingMap;
			return;
		}

		const UScriptStruct* Struct = F.BindingMapValue->Struct;
		F.EntryName = EntryField<FNameProperty>(Struct, TEXT("PropertyName"));
		F.EntryArrayIndex = EntryField<FIntProperty>(Struct, TEXT("ArrayIndex"));
		F.EntryType = EntryField<FEnumProperty>(Struct, TEXT("Type"));
		F.EntryBound = EntryField<FBoolProperty>(Struct, TEXT("bIsBound"));
		F.EntryPromotion = EntryField<FBoolProperty>(Struct, TEXT("bIsPromotion"));
		F.EntryOnlyActive = EntryField<FBoolProperty>(Struct, TEXT("bOnlyUpdateWhenActive"));
		F.EntryPath = EntryField<FArrayProperty>(Struct, TEXT("PropertyPath"));
		F.EntryPathElement = F.EntryPath != nullptr ? CastField<FStrProperty>(F.EntryPath->Inner) : nullptr;
		F.EntryPathText = EntryField<FTextProperty>(Struct, TEXT("PathAsText"));
		const struct { const void* Field; const TCHAR* Name; } Required[] = {
			{ F.EntryName, TEXT("FAnimGraphNodePropertyBinding.PropertyName") },
			{ F.EntryArrayIndex, TEXT("FAnimGraphNodePropertyBinding.ArrayIndex") },
			{ F.EntryType, TEXT("FAnimGraphNodePropertyBinding.Type") },
			{ F.EntryBound, TEXT("FAnimGraphNodePropertyBinding.bIsBound") },
			{ F.EntryPromotion, TEXT("FAnimGraphNodePropertyBinding.bIsPromotion") },
			{ F.EntryOnlyActive, TEXT("FAnimGraphNodePropertyBinding.bOnlyUpdateWhenActive") },
			{ F.EntryPathElement, TEXT("FAnimGraphNodePropertyBinding.PropertyPath") },
			{ F.EntryPathText, TEXT("FAnimGraphNodePropertyBinding.PathAsText") },
		};
		for (const auto& Field : Required)
		{
			if (Field.Field == nullptr)
			{
				F.BindingMap = nullptr;
				F.BindingMissing = Field.Name;
				return;
			}
		}
	}

	void ResolveAccess(FFields& F)
	{
		F.PropertyAccessClass = FindObject<UClass>(nullptr, PropertyAccessClassPath);
		if (F.PropertyAccessClass == nullptr)
		{
			return;
		}
		F.AccessPath = CastField<FArrayProperty>(FindFProperty<FProperty>(F.PropertyAccessClass, ActualName(FieldAccessPath, TEXT("Path"))));
		F.AccessPathElement = F.AccessPath != nullptr ? CastField<FStrProperty>(F.AccessPath->Inner) : nullptr;
		F.AccessTextPath = CastField<FTextProperty>(FindFProperty<FProperty>(F.PropertyAccessClass, ActualName(FieldAccessTextPath, TEXT("TextPath"))));
		if (F.AccessPathElement == nullptr)
		{
			F.AccessMissing = FieldAccessPath;
		}
		else if (F.AccessTextPath == nullptr)
		{
			F.AccessMissing = FieldAccessTextPath;
		}
	}

	void Resolve()
	{
		FFields& F = Fields();
		if (F.bResolved)
		{
			return;
		}
		F = FFields();
		F.bResolved = true;
		ResolveBinding(F);
		ResolveAccess(F);
	}

	/** The node's Binding object; null when the storage is not the default one or the field is unavailable. */
	UObject* LocateBinding(const UK2Node* Node)
	{
		Resolve();
		const FFields& F = Fields();
		if (Node == nullptr || F.Binding == nullptr || F.BindingMap == nullptr || !Node->IsA(F.Binding->GetOwnerClass()))
		{
			return nullptr;
		}
		UObject* Binding = F.Binding->GetObjectPropertyValue_InContainer(Node);
		return Binding != nullptr && Binding->GetClass()->IsChildOf(F.DefaultBindingClass) ? Binding : nullptr;
	}

	void* MapPtr(UObject* Binding)
	{
		return Fields().BindingMap->ContainerPtrToValuePtr<void>(Binding);
	}

	FCortexBindingEntry ToEntry(const uint8* Value)
	{
		const FFields& F = Fields();
		FCortexBindingEntry Entry;
		Entry.PropertyName = F.EntryName->GetPropertyValue_InContainer(Value);
		Entry.ArrayIndex = F.EntryArrayIndex->GetPropertyValue_InContainer(Value);
		const void* TypePtr = F.EntryType->ContainerPtrToValuePtr<void>(Value);
		const int64 TypeValue = F.EntryType->GetUnderlyingProperty()->GetSignedIntPropertyValue(TypePtr);
		Entry.bFunction = F.EntryType->GetEnum() != nullptr && F.EntryType->GetEnum()->GetNameStringByValue(TypeValue).EndsWith(TEXT("Function"));
		Entry.bIsBound = F.EntryBound->GetPropertyValue_InContainer(Value);
		Entry.bIsPromotion = F.EntryPromotion->GetPropertyValue_InContainer(Value);
		Entry.bOnlyUpdateWhenActive = F.EntryOnlyActive->GetPropertyValue_InContainer(Value);
		FScriptArrayHelper Path(F.EntryPath, F.EntryPath->ContainerPtrToValuePtr<void>(Value));
		for (int32 Index = 0; Index < Path.Num(); ++Index)
		{
			Entry.Path.Add(F.EntryPathElement->GetPropertyValue(Path.GetRawPtr(Index)));
		}
		Entry.PathText = F.EntryPathText->GetPropertyValue_InContainer(Value).ToString();
		return Entry;
	}
}

FCortexBindingMapSnapshot::FCortexBindingMapSnapshot(FCortexBindingMapSnapshot&& Other) noexcept
	: Property(Other.Property)
	, Memory(Other.Memory)
{
	Other.Property = nullptr;
	Other.Memory = nullptr;
}

FCortexBindingMapSnapshot& FCortexBindingMapSnapshot::operator=(FCortexBindingMapSnapshot&& Other) noexcept
{
	if (this != &Other)
	{
		Reset();
		Property = Other.Property;
		Memory = Other.Memory;
		Other.Property = nullptr;
		Other.Memory = nullptr;
	}
	return *this;
}

FCortexBindingMapSnapshot::~FCortexBindingMapSnapshot()
{
	Reset();
}

void FCortexBindingMapSnapshot::Reset()
{
	if (Memory != nullptr)
	{
		Property->DestroyValue(Memory);
		FMemory::Free(Memory);
	}
	Memory = nullptr;
	Property = nullptr;
}

bool FCortexGraphBindingStorage::CheckBindingStorage(FString& OutMissingField)
{
	CortexGraphBindingStoragePrivate::Resolve();
	OutMissingField = CortexGraphBindingStoragePrivate::Fields().BindingMissing;
	return OutMissingField.IsEmpty();
}

bool FCortexGraphBindingStorage::CheckPropertyAccess(FString& OutMissingField)
{
	CortexGraphBindingStoragePrivate::Resolve();
	OutMissingField = CortexGraphBindingStoragePrivate::Fields().AccessMissing;
	return OutMissingField.IsEmpty();
}

ECortexBindingStorage FCortexGraphBindingStorage::ReadBindings(const UK2Node* Node, TArray<FCortexBindingEntry>& OutEntries)
{
	using namespace CortexGraphBindingStoragePrivate;
	UObject* Binding = LocateBinding(Node);
	if (Binding == nullptr)
	{
		return ECortexBindingStorage::Unsupported;
	}

	FScriptMapHelper Helper(Fields().BindingMap, MapPtr(Binding));
	const int32 First = OutEntries.Num();
	for (int32 Index = 0; Index < Helper.GetMaxIndex(); ++Index)
	{
		if (Helper.IsValidIndex(Index))
		{
			OutEntries.Add(ToEntry(Helper.GetValuePtr(Index)));
		}
	}
	TArrayView<FCortexBindingEntry> Added(OutEntries.GetData() + First, OutEntries.Num() - First);
	Algo::Sort(Added, [](const FCortexBindingEntry& Left, const FCortexBindingEntry& Right)
	{
		const int32 Compare = Left.PropertyName.Compare(Right.PropertyName);
		return Compare != 0 ? Compare < 0 : Left.ArrayIndex < Right.ArrayIndex;
	});
	return ECortexBindingStorage::Supported;
}

bool FCortexGraphBindingStorage::CaptureBindingMap(const UK2Node* Node, FCortexBindingMapSnapshot& OutSnapshot)
{
	using namespace CortexGraphBindingStoragePrivate;
	UObject* Binding = LocateBinding(Node);
	if (Binding == nullptr)
	{
		return false;
	}
	const FMapProperty* Map = Fields().BindingMap;
	OutSnapshot.Reset();
	OutSnapshot.Memory = static_cast<uint8*>(FMemory::Malloc(Map->GetSize(), Map->GetMinAlignment()));
	OutSnapshot.Property = Map;
	Map->InitializeValue(OutSnapshot.Memory);
	Map->CopyCompleteValue(OutSnapshot.Memory, MapPtr(Binding));
	return true;
}

bool FCortexGraphBindingStorage::RestoreBindingMap(UK2Node* Node, const FCortexBindingMapSnapshot& Snapshot)
{
	using namespace CortexGraphBindingStoragePrivate;
	UObject* Binding = LocateBinding(Node);
	if (Binding == nullptr || !Snapshot.IsValid())
	{
		return false;
	}
	MarkBindingModified(Node);
	Fields().BindingMap->CopyCompleteValue(MapPtr(Binding), Snapshot.Memory);
	return true;
}

bool FCortexGraphBindingStorage::BindingMapMatches(const UK2Node* Node, const FCortexBindingMapSnapshot& Snapshot)
{
	using namespace CortexGraphBindingStoragePrivate;
	UObject* Binding = LocateBinding(Node);
	return Binding != nullptr && Snapshot.IsValid() && Fields().BindingMap->Identical(MapPtr(Binding), Snapshot.Memory, PPF_None);
}

bool FCortexGraphBindingStorage::ExportBindingText(const UK2Node* Node, FString& OutText)
{
	using namespace CortexGraphBindingStoragePrivate;
	UObject* Binding = LocateBinding(Node);
	if (Binding == nullptr)
	{
		return false;
	}
	OutText.Reset();
	Fields().BindingMap->ExportText_InContainer(0, OutText, Binding, nullptr, Binding, PPF_None);
	return true;
}

bool FCortexGraphBindingStorage::ImportBindingText(UK2Node* Node, const FString& Text)
{
	using namespace CortexGraphBindingStoragePrivate;
	UObject* Binding = LocateBinding(Node);
	if (Binding == nullptr)
	{
		return false;
	}
	MarkBindingModified(Node);
	return Fields().BindingMap->ImportText_InContainer(*Text, Binding, Binding, PPF_None) != nullptr;
}

bool FCortexGraphBindingStorage::IsAnimNode(const UEdGraphNode* Node)
{
	using namespace CortexGraphBindingStoragePrivate;
	const UClass* NodeBase = FindObject<UClass>(nullptr, NodeBaseClassPath);
	return Node != nullptr && NodeBase != nullptr && Node->IsA(NodeBase);
}

bool FCortexGraphBindingStorage::CloneBindingMap(const FCortexBindingMapSnapshot& Source, FCortexBindingMapSnapshot& OutClone)
{
	if (!Source.IsValid())
	{
		return false;
	}
	OutClone.Reset();
	OutClone.Memory = static_cast<uint8*>(FMemory::Malloc(Source.Property->GetSize(), Source.Property->GetMinAlignment()));
	OutClone.Property = Source.Property;
	Source.Property->InitializeValue(OutClone.Memory);
	Source.Property->CopyCompleteValue(OutClone.Memory, Source.Memory);
	return true;
}

bool FCortexGraphBindingStorage::RewriteSnapshotEntry(FCortexBindingMapSnapshot& Snapshot, FName PropertyName, int32 ArrayIndex, const TArray<FString>& NewPath, const FText& NewPathText)
{
	using namespace CortexGraphBindingStoragePrivate;
	if (!Snapshot.IsValid())
	{
		return false;
	}
	const FFields& F = Fields();
	FScriptMapHelper Helper(Snapshot.Property, Snapshot.Memory);
	for (int32 Index = 0; Index < Helper.GetMaxIndex(); ++Index)
	{
		if (!Helper.IsValidIndex(Index))
		{
			continue;
		}
		uint8* Value = Helper.GetValuePtr(Index);
		const FCortexBindingEntry Entry = ToEntry(Value);
		if (Entry.PropertyName != PropertyName || Entry.ArrayIndex != ArrayIndex)
		{
			continue;
		}
		FScriptArrayHelper Path(F.EntryPath, F.EntryPath->ContainerPtrToValuePtr<void>(Value));
		Path.EmptyValues();
		for (const FString& Segment : NewPath)
		{
			F.EntryPathElement->SetPropertyValue(Path.GetRawPtr(Path.AddValue()), Segment);
		}
		F.EntryPathText->SetPropertyValue_InContainer(Value, NewPathText);
		return true;
	}
	return false;
}

bool FCortexGraphBindingStorage::BindingMapMatchesContent(const UK2Node* Node, const FCortexBindingMapSnapshot& Snapshot)
{
	using namespace CortexGraphBindingStoragePrivate;
	UObject* Binding = LocateBinding(Node);
	if (Binding == nullptr || !Snapshot.IsValid())
	{
		return false;
	}
	const FFields& F = Fields();
	auto ExportEntries = [&F](const FMapProperty* Map, void* MapMemory)
	{
		TMap<FString, FString> Out;
		FScriptMapHelper Helper(Map, MapMemory);
		for (int32 Index = 0; Index < Helper.GetMaxIndex(); ++Index)
		{
			if (!Helper.IsValidIndex(Index))
			{
				continue;
			}
			FString Key;
			F.BindingMapKey->ExportText_Direct(Key, Helper.GetKeyPtr(Index), Helper.GetKeyPtr(Index), nullptr, PPF_None);
			FString Text;
			F.BindingMapValue->ExportText_Direct(Text, Helper.GetValuePtr(Index), Helper.GetValuePtr(Index), nullptr, PPF_None);
			Out.Add(Key, Text);
		}
		return Out;
	};
	const TMap<FString, FString> Live = ExportEntries(F.BindingMap, MapPtr(Binding));
	const TMap<FString, FString> Expected = ExportEntries(Snapshot.Property, Snapshot.Memory);
	if (Live.Num() != Expected.Num())
	{
		return false;
	}
	for (const TPair<FString, FString>& Pair : Expected)
	{
		const FString* Current = Live.Find(Pair.Key);
		if (Current == nullptr || *Current != Pair.Value)
		{
			return false;
		}
	}
	return true;
}

void FCortexGraphBindingStorage::MarkBindingModified(UK2Node* Node)
{
	using namespace CortexGraphBindingStoragePrivate;
	if (UObject* Binding = LocateBinding(Node))
	{
		if (!Binding->HasAnyFlags(RF_Transactional))
		{
			Binding->SetFlags(RF_Transactional);
		}
		Binding->Modify();
	}
}

FString FCortexGraphBindingStorage::FindBindingKeyNaming(const UK2Node* Node, FName Member)
{
	using namespace CortexGraphBindingStoragePrivate;
	UObject* Binding = LocateBinding(Node);
	if (Binding == nullptr)
	{
		return FString();
	}

	const FString MemberText = Member.ToString();
	const FFields& F = Fields();
	FScriptMapHelper Helper(F.BindingMap, MapPtr(Binding));
	for (int32 Index = 0; Index < Helper.GetMaxIndex(); ++Index)
	{
		if (!Helper.IsValidIndex(Index))
		{
			continue;
		}
		const FCortexBindingEntry Entry = ToEntry(Helper.GetValuePtr(Index));
		if (Entry.Path.Contains(MemberText))
		{
			return F.BindingMapKey->GetPropertyValue(Helper.GetKeyPtr(Index)).ToString();
		}
	}
	return FString();
}

FString FCortexGraphBindingStorage::FindFunctionSlotNaming(const UK2Node* Node, FName Member)
{
	using namespace CortexGraphBindingStoragePrivate;
	for (const TCHAR* SlotName : SlotNames)
	{
		const FStructProperty* Slot = FindFProperty<FStructProperty>(Node->GetClass(), SlotName);
		const FNameProperty* MemberName = Slot != nullptr ? CastField<FNameProperty>(Slot->Struct->FindPropertyByName(TEXT("MemberName"))) : nullptr;
		if (MemberName != nullptr && MemberName->GetPropertyValue_InContainer(Slot->ContainerPtrToValuePtr<void>(Node)) == Member)
		{
			return SlotName;
		}
	}
	return FString();
}

bool FCortexGraphBindingStorage::IsPropertyAccessNode(const UEdGraphNode* Node)
{
	using namespace CortexGraphBindingStoragePrivate;
	Resolve();
	return Node != nullptr && Fields().PropertyAccessClass != nullptr && Node->IsA(Fields().PropertyAccessClass);
}

bool FCortexGraphBindingStorage::ReadPropertyAccess(const UEdGraphNode* Node, TArray<FString>& OutPath, FString& OutTextPath)
{
	using namespace CortexGraphBindingStoragePrivate;
	const FFields& F = Fields();
	if (!IsPropertyAccessNode(Node) || F.AccessPathElement == nullptr || F.AccessTextPath == nullptr)
	{
		return false;
	}

	OutPath.Reset();
	FScriptArrayHelper Array(F.AccessPath, F.AccessPath->ContainerPtrToValuePtr<void>(Node));
	for (int32 Index = 0; Index < Array.Num(); ++Index)
	{
		OutPath.Add(F.AccessPathElement->GetPropertyValue(Array.GetRawPtr(Index)));
	}
	OutTextPath = F.AccessTextPath->GetPropertyValue_InContainer(Node).ToString();
	return true;
}

bool FCortexGraphBindingStorage::WritePropertyAccess(UEdGraphNode* Node, const TArray<FString>& Path, const FString& TextPath)
{
	using namespace CortexGraphBindingStoragePrivate;
	const FFields& F = Fields();
	if (!IsPropertyAccessNode(Node) || F.AccessPathElement == nullptr || F.AccessTextPath == nullptr)
	{
		return false;
	}

	Node->Modify();
	FScriptArrayHelper Array(F.AccessPath, F.AccessPath->ContainerPtrToValuePtr<void>(Node));
	Array.EmptyValues();
	for (const FString& Segment : Path)
	{
		F.AccessPathElement->SetPropertyValue(Array.GetRawPtr(Array.AddValue()), Segment);
	}
	F.AccessTextPath->SetPropertyValue_InContainer(Node, FText::FromString(TextPath));
	return true;
}

#if WITH_AUTOMATION_TESTS
void FCortexGraphBindingStorage::SetFieldNameForTest(const FString& LogicalField, FName ActualName)
{
	CortexGraphBindingStoragePrivate::NameOverrides().Add(LogicalField, ActualName);
	CortexGraphBindingStoragePrivate::Fields().bResolved = false;
}

void FCortexGraphBindingStorage::ClearFieldNameOverridesForTest()
{
	CortexGraphBindingStoragePrivate::NameOverrides().Reset();
	CortexGraphBindingStoragePrivate::Fields().bResolved = false;
}
#endif
