#include "Operations/CortexAnimBindingReflection.h"

#include "AnimGraphNode_Base.h"
#include "Algo/Sort.h"
#include "EdGraph/EdGraphNode.h"
#include "Engine/MemberReference.h"
#include "UObject/UnrealType.h"
#include "UObject/UObjectGlobals.h"

namespace CortexAnimBindingReflectionPrivate
{
	constexpr const TCHAR* DefaultBindingClassPath = TEXT("/Script/AnimGraph.AnimGraphNodeBinding_Base");
	constexpr const TCHAR* PropertyAccessClassPath = TEXT("/Script/PropertyAccessNode.K2Node_PropertyAccess");

	constexpr const TCHAR* FieldNodeBinding = TEXT("AnimGraphNode_Base.Binding");
	constexpr const TCHAR* FieldBindingMap = TEXT("AnimGraphNodeBinding_Base.PropertyBindings");
	constexpr const TCHAR* FieldAccessPath = TEXT("K2Node_PropertyAccess.Path");
	constexpr const TCHAR* FieldAccessTextPath = TEXT("K2Node_PropertyAccess.TextPath");

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

	void Resolve()
	{
		FFields& F = Fields();
		if (F.bResolved)
		{
			return;
		}
		F = FFields();
		F.bResolved = true;

		F.Binding = FindFProperty<FObjectProperty>(UAnimGraphNode_Base::StaticClass(), ActualName(FieldNodeBinding, TEXT("Binding")));
		if (F.Binding == nullptr)
		{
			F.BindingMissing = FieldNodeBinding;
		}
		else
		{
			F.DefaultBindingClass = FindObject<UClass>(nullptr, DefaultBindingClassPath);
			if (F.DefaultBindingClass == nullptr)
			{
				F.BindingMissing = DefaultBindingClassPath;
			}
			else
			{
				F.BindingMap = CastField<FMapProperty>(FindFProperty<FProperty>(F.DefaultBindingClass, ActualName(FieldBindingMap, TEXT("PropertyBindings"))));
				F.BindingMapKey = F.BindingMap != nullptr ? CastField<FNameProperty>(F.BindingMap->KeyProp) : nullptr;
				F.BindingMapValue = F.BindingMap != nullptr ? CastField<FStructProperty>(F.BindingMap->ValueProp) : nullptr;
				if (F.BindingMapKey == nullptr || F.BindingMapValue == nullptr || F.BindingMapValue->Struct != FAnimGraphNodePropertyBinding::StaticStruct())
				{
					F.BindingMap = nullptr;
					F.BindingMapKey = nullptr;
					F.BindingMapValue = nullptr;
					F.BindingMissing = FieldBindingMap;
				}
			}
		}

		F.PropertyAccessClass = FindObject<UClass>(nullptr, PropertyAccessClassPath);
		if (F.PropertyAccessClass != nullptr)
		{
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
	}

	/** The node's Binding object and its binding map; null when the storage is not the default one. */
	UObject* LocateBinding(const UAnimGraphNode_Base* Node, const FMapProperty*& OutMap)
	{
		Resolve();
		const FFields& F = Fields();
		OutMap = nullptr;
		if (Node == nullptr || F.Binding == nullptr || F.BindingMap == nullptr)
		{
			return nullptr;
		}
		UObject* Binding = F.Binding->GetObjectPropertyValue_InContainer(Node);
		if (Binding == nullptr || !Binding->GetClass()->IsChildOf(F.DefaultBindingClass))
		{
			return nullptr;
		}
		OutMap = F.BindingMap;
		return Binding;
	}

	FCortexAnimBindingEntry ToEntry(const FAnimGraphNodePropertyBinding& Source)
	{
		FCortexAnimBindingEntry Entry;
		Entry.PropertyName = Source.PropertyName;
		Entry.ArrayIndex = Source.ArrayIndex;
		Entry.bFunction = Source.Type == EAnimGraphNodePropertyBindingType::Function;
		Entry.bIsBound = Source.bIsBound;
		Entry.bIsPromotion = Source.bIsPromotion;
		Entry.bOnlyUpdateWhenActive = Source.bOnlyUpdateWhenActive;
		Entry.Path = Source.PropertyPath;
		Entry.PathText = Source.PathAsText.ToString();
		return Entry;
	}
}

bool FCortexAnimBindingReflection::CheckBindingStorage(FString& OutMissingField)
{
	using namespace CortexAnimBindingReflectionPrivate;
	Resolve();
	OutMissingField = Fields().BindingMissing;
	return OutMissingField.IsEmpty();
}

bool FCortexAnimBindingReflection::CheckPropertyAccess(FString& OutMissingField)
{
	using namespace CortexAnimBindingReflectionPrivate;
	Resolve();
	OutMissingField = Fields().AccessMissing;
	return OutMissingField.IsEmpty();
}

ECortexAnimBindingStorage FCortexAnimBindingReflection::ReadNodeBindings(const UAnimGraphNode_Base* Node, TArray<FCortexAnimBindingEntry>& OutEntries)
{
	using namespace CortexAnimBindingReflectionPrivate;
	const FMapProperty* Map = nullptr;
	const UObject* Binding = LocateBinding(Node, Map);
	if (Binding == nullptr)
	{
		return ECortexAnimBindingStorage::Unsupported;
	}

	FScriptMapHelper Helper(Map, Map->ContainerPtrToValuePtr<void>(Binding));
	const int32 FirstEntry = OutEntries.Num();
	for (int32 Index = 0; Index < Helper.GetMaxIndex(); ++Index)
	{
		if (!Helper.IsValidIndex(Index))
		{
			continue;
		}
		OutEntries.Add(ToEntry(*reinterpret_cast<const FAnimGraphNodePropertyBinding*>(Helper.GetValuePtr(Index))));
	}
	TArrayView<FCortexAnimBindingEntry> Added(OutEntries.GetData() + FirstEntry, OutEntries.Num() - FirstEntry);
	Algo::Sort(Added, [](const FCortexAnimBindingEntry& Left, const FCortexAnimBindingEntry& Right)
	{
		const int32 Compare = Left.PropertyName.Compare(Right.PropertyName);
		return Compare != 0 ? Compare < 0 : Left.ArrayIndex < Right.ArrayIndex;
	});
	return ECortexAnimBindingStorage::Supported;
}

bool FCortexAnimBindingReflection::CopyBindingMap(const UAnimGraphNode_Base* Node, TMap<FName, FAnimGraphNodePropertyBinding>& OutMap)
{
	using namespace CortexAnimBindingReflectionPrivate;
	const FMapProperty* Map = nullptr;
	const UObject* Binding = LocateBinding(Node, Map);
	if (Binding == nullptr)
	{
		return false;
	}

	OutMap.Reset();
	FScriptMapHelper Helper(Map, Map->ContainerPtrToValuePtr<void>(Binding));
	for (int32 Index = 0; Index < Helper.GetMaxIndex(); ++Index)
	{
		if (!Helper.IsValidIndex(Index))
		{
			continue;
		}
		const FName Key = Fields().BindingMapKey->GetPropertyValue(Helper.GetKeyPtr(Index));
		OutMap.Add(Key, *reinterpret_cast<const FAnimGraphNodePropertyBinding*>(Helper.GetValuePtr(Index)));
	}
	return true;
}

bool FCortexAnimBindingReflection::RestoreBindingMap(UAnimGraphNode_Base* Node, const TMap<FName, FAnimGraphNodePropertyBinding>& Source)
{
	using namespace CortexAnimBindingReflectionPrivate;
	const FMapProperty* Map = nullptr;
	UObject* Binding = LocateBinding(Node, Map);
	if (Binding == nullptr)
	{
		return false;
	}

	Binding->Modify();
	FScriptMapHelper Helper(Map, Map->ContainerPtrToValuePtr<void>(Binding));
	Helper.EmptyValues();
	for (const TPair<FName, FAnimGraphNodePropertyBinding>& Pair : Source)
	{
		const int32 Index = Helper.AddDefaultValue_Invalid_NeedsRehash();
		Fields().BindingMapKey->SetPropertyValue(Helper.GetKeyPtr(Index), Pair.Key);
		Fields().BindingMapValue->Struct->CopyScriptStruct(Helper.GetValuePtr(Index), &Pair.Value);
	}
	Helper.Rehash();
	return true;
}

bool FCortexAnimBindingReflection::IsPropertyAccessNode(const UEdGraphNode* Node)
{
	using namespace CortexAnimBindingReflectionPrivate;
	Resolve();
	return Node != nullptr && Fields().PropertyAccessClass != nullptr && Node->IsA(Fields().PropertyAccessClass);
}

bool FCortexAnimBindingReflection::ReadPropertyAccessNode(const UEdGraphNode* Node, TArray<FString>& OutPath, FString& OutTextPath)
{
	using namespace CortexAnimBindingReflectionPrivate;
	Resolve();
	const FFields& F = Fields();
	if (!IsPropertyAccessNode(Node) || F.AccessPath == nullptr || F.AccessTextPath == nullptr || F.AccessPathElement == nullptr)
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

bool FCortexAnimBindingReflection::WritePropertyAccessNode(UEdGraphNode* Node, const TArray<FString>& Path, const FString& TextPath)
{
	using namespace CortexAnimBindingReflectionPrivate;
	Resolve();
	const FFields& F = Fields();
	if (!IsPropertyAccessNode(Node) || F.AccessPath == nullptr || F.AccessTextPath == nullptr || F.AccessPathElement == nullptr)
	{
		return false;
	}

	Node->Modify();
	FScriptArrayHelper Array(F.AccessPath, F.AccessPath->ContainerPtrToValuePtr<void>(Node));
	Array.EmptyValues();
	for (const FString& Segment : Path)
	{
		const int32 Index = Array.AddValue();
		F.AccessPathElement->SetPropertyValue(Array.GetRawPtr(Index), Segment);
	}
	F.AccessTextPath->SetPropertyValue_InContainer(Node, FText::FromString(TextPath));
	return true;
}

void FCortexAnimBindingReflection::ReadFunctionSlots(const UAnimGraphNode_Base* Node, TArray<FCortexAnimFunctionSlot>& OutSlots)
{
	OutSlots.Reset();
	if (Node == nullptr)
	{
		return;
	}
	OutSlots.Add({ TEXT("initial_update"), TEXT("InitialUpdateFunction"), &Node->InitialUpdateFunction });
	OutSlots.Add({ TEXT("become_relevant"), TEXT("BecomeRelevantFunction"), &Node->BecomeRelevantFunction });
	OutSlots.Add({ TEXT("update"), TEXT("UpdateFunction"), &Node->UpdateFunction });
}

FMemberReference* FCortexAnimBindingReflection::FindMutableSlot(UAnimGraphNode_Base* Node, FName PropertyName)
{
	if (Node == nullptr)
	{
		return nullptr;
	}
	if (PropertyName == TEXT("InitialUpdateFunction"))
	{
		return &Node->InitialUpdateFunction;
	}
	if (PropertyName == TEXT("BecomeRelevantFunction"))
	{
		return &Node->BecomeRelevantFunction;
	}
	if (PropertyName == TEXT("UpdateFunction"))
	{
		return &Node->UpdateFunction;
	}
	return nullptr;
}

#if WITH_AUTOMATION_TESTS
void FCortexAnimBindingReflection::SetFieldNameForTest(const FString& LogicalField, FName ActualName)
{
	CortexAnimBindingReflectionPrivate::NameOverrides().Add(LogicalField, ActualName);
	CortexAnimBindingReflectionPrivate::Fields().bResolved = false;
}

void FCortexAnimBindingReflection::ClearFieldNameOverridesForTest()
{
	CortexAnimBindingReflectionPrivate::NameOverrides().Reset();
	CortexAnimBindingReflectionPrivate::Fields().bResolved = false;
}
#endif
