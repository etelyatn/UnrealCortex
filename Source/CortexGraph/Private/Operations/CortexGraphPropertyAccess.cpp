#include "Operations/CortexGraphPropertyAccess.h"

#include "Animation/AnimBlueprint.h"
#include "BlueprintActionDatabase.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "EdGraphSchema_K2.h"
#include "Engine/Blueprint.h"
#include "Features/IModularFeatures.h"
#include "IPropertyAccessEditor.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Preferences/PersonaOptions.h"
#include "UObject/UnrealType.h"
#include "UObject/TextProperty.h"

namespace
{
const FName PropertyAccessEditorFeature(TEXT("PropertyAccessEditor"));

IPropertyAccessEditor* GetPropertyAccessEditor()
{
	IModularFeatures& Features = IModularFeatures::Get();
	return Features.IsModularFeatureAvailable(PropertyAccessEditorFeature)
		? &Features.GetModularFeature<IPropertyAccessEditor>(PropertyAccessEditorFeature)
		: nullptr;
}

/** The class a path starts from: the skeleton class, which is what the node and its pin allocation resolve against. */
const UStruct* ResolveScope(const UBlueprint* Blueprint)
{
	return Blueprint != nullptr ? Blueprint->SkeletonGeneratedClass.Get() : nullptr;
}

FArrayProperty* FindPathProperty(const UClass* NodeClass)
{
	FArrayProperty* PathProperty = NodeClass != nullptr
		? CastField<FArrayProperty>(NodeClass->FindPropertyByName(TEXT("Path")))
		: nullptr;
	return PathProperty != nullptr && CastField<FStrProperty>(PathProperty->Inner) != nullptr ? PathProperty : nullptr;
}

/**
 * `Name` or `Name[N]`. The engine segment parser reads everything after the first '[' up to the last character as the
 * index and asserts on a trailing '[', so a segment is checked before it ever reaches the resolver.
 */
bool IsWellFormedSegment(const FString& Segment)
{
	int32 Bracket = INDEX_NONE;
	if (!Segment.FindChar(TEXT('['), Bracket))
	{
		return !Segment.IsEmpty() && !Segment.Contains(TEXT("]"));
	}
	if (Bracket == 0 || !Segment.EndsWith(TEXT("]")))
	{
		return false;
	}
	const FString Digits = Segment.Mid(Bracket + 1, Segment.Len() - Bracket - 2);
	if (Digits.IsEmpty() || Digits.Len() > 9)
	{
		return false;
	}
	for (const TCHAR Char : Digits)
	{
		if (!FChar::IsDigit(Char))
		{
			return false;
		}
	}
	return !Segment.Left(Bracket).Contains(TEXT("]"));
}

bool HasIndex(const FString& Segment)
{
	return Segment.Contains(TEXT("["));
}

bool AllSegmentsWellFormed(const TArray<FString>& Path)
{
	for (const FString& Segment : Path)
	{
		if (!IsWellFormedSegment(Segment))
		{
			return false;
		}
	}
	return Path.Num() > 0;
}

/** True when a path can continue past this property (struct, object, or an indexed array of either). */
bool IsTraversable(const FProperty* Property, bool bIndexed)
{
	if (const FArrayProperty* ArrayProperty = CastField<FArrayProperty>(Property))
	{
		return bIndexed && (CastField<FStructProperty>(ArrayProperty->Inner) != nullptr || CastField<FObjectPropertyBase>(ArrayProperty->Inner) != nullptr);
	}
	return CastField<FStructProperty>(Property) != nullptr || CastField<FObjectPropertyBase>(Property) != nullptr;
}
}

UClass* FCortexGraphPropertyAccess::GetNodeClass()
{
	return FindObject<UClass>(nullptr, TEXT("/Script/PropertyAccessNode.K2Node_PropertyAccess"));
}

bool FCortexGraphPropertyAccess::IsPropertyAccessNode(const UEdGraphNode* Node)
{
	const UClass* NodeClass = GetNodeClass();
	return Node != nullptr && NodeClass != nullptr && Node->IsA(NodeClass);
}

bool FCortexGraphPropertyAccess::ParsePathParam(const TSharedPtr<FJsonObject>& Params, TArray<FString>& OutPath, FString& OutError)
{
	OutPath.Reset();
	const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
	if (!Params.IsValid() || !Params->TryGetArrayField(TEXT("path"), Values) || Values == nullptr || Values->Num() == 0)
	{
		OutError = TEXT("PropertyAccess requires params.path: a non-empty array of path segments, e.g. [\"GetOwningActor\", \"bHidden\"]");
		return false;
	}
	for (int32 Index = 0; Index < Values->Num(); ++Index)
	{
		const TSharedPtr<FJsonValue>& Value = (*Values)[Index];
		FString Segment;
		if (!Value.IsValid() || Value->Type != EJson::String || !Value->TryGetString(Segment) || !IsWellFormedSegment(Segment))
		{
			OutError = FString::Printf(TEXT("PropertyAccess params.path[%d] must be a string 'Name' or 'Name[N]' with N a non-negative integer"), Index);
			OutPath.Reset();
			return false;
		}
		OutPath.Add(MoveTemp(Segment));
	}
	return true;
}

bool FCortexGraphPropertyAccess::ValidateBlueprint(const UBlueprint* Blueprint, FCortexCommandResult& OutError)
{
	const UAnimBlueprint* AnimBlueprint = Cast<UAnimBlueprint>(Blueprint);
	if (AnimBlueprint == nullptr)
	{
		OutError = FCortexCommandRouter::Error(CortexErrorCodes::InvalidOperation, FString::Printf(
			TEXT("Property Access nodes are supported only in Animation Blueprints; %s is a %s"),
			Blueprint != nullptr ? *Blueprint->GetPathName() : TEXT("<none>"),
			Blueprint != nullptr ? *Blueprint->GetClass()->GetName() : TEXT("<none>")));
		return false;
	}
	if (UAnimBlueprint::GetParentAnimBlueprint(AnimBlueprint) != nullptr)
	{
		OutError = FCortexCommandRouter::Error(CortexErrorCodes::InvalidOperation, FString::Printf(
			TEXT("Property Access nodes are unsupported in child Animation Blueprints (the engine refuses them at compile); %s derives from another Anim Blueprint"),
			*AnimBlueprint->GetPathName()));
		return false;
	}
	if (AnimBlueprint->SkeletonGeneratedClass == nullptr)
	{
		OutError = FCortexCommandRouter::Error(CortexErrorCodes::InvalidOperation, FString::Printf(
			TEXT("%s has no skeleton class to resolve a property access path from; compile it first"), *AnimBlueprint->GetPathName()));
		return false;
	}
	if (GetPropertyAccessEditor() == nullptr)
	{
		OutError = FCortexCommandRouter::Error(CortexErrorCodes::InvalidOperation,
			TEXT("The PropertyAccessEditor plugin is not loaded; Property Access paths cannot be resolved"));
		return false;
	}
	return true;
}

bool FCortexGraphPropertyAccess::ValidateGraph(const UBlueprint* Blueprint, const UEdGraph* Graph, FCortexCommandResult& OutError)
{
	const UEdGraph* TopLevel = Graph != nullptr ? FBlueprintEditorUtils::GetTopLevelGraph(Graph) : nullptr;
	if (Blueprint != nullptr && TopLevel != nullptr && Blueprint->MacroGraphs.Contains(TopLevel))
	{
		OutError = FCortexCommandRouter::Error(CortexErrorCodes::InvalidOperation, FString::Printf(
			TEXT("Property Access nodes cannot be placed in macro graph '%s': the Anim Blueprint compiler creates no cached copy for nodes inlined from macros"),
			*TopLevel->GetName()));
		return false;
	}
	return true;
}

bool FCortexGraphPropertyAccess::ValidatePath(const UBlueprint* Blueprint, const TArray<FString>& Path, FCortexCommandResult& OutError)
{
	IPropertyAccessEditor* Editor = GetPropertyAccessEditor();
	const UStruct* Scope = ResolveScope(Blueprint);
	if (Editor == nullptr || Scope == nullptr || !AllSegmentsWellFormed(Path))
	{
		OutError = FCortexCommandRouter::Error(CortexErrorCodes::InvalidOperation,
			TEXT("Property Access paths cannot be resolved: the PropertyAccessEditor plugin is not loaded, the Blueprint has no skeleton class, or the path is malformed"));
		return false;
	}

	const FString JoinedPath = FString::Join(Path, TEXT("."));
	FString SegmentError;
	FProperty* LeafProperty = nullptr;
	int32 LeafArrayIndex = INDEX_NONE;
	IPropertyAccessEditor::FResolvePropertyAccessArgs Args;
	Args.PropertyFunction = [&](int32 SegmentIndex, FProperty* Property, int32 StaticArrayIndex)
	{
		if (SegmentError.IsEmpty() && HasIndex(Path[SegmentIndex]) && Property->ArrayDim <= 1)
		{
			SegmentError = FString::Printf(TEXT("segment '%s' indexes '%s', which is not an array"), *Path[SegmentIndex], *Property->GetName());
		}
		if (SegmentError.IsEmpty() && !Property->HasAnyPropertyFlags(CPF_ReturnParm) && !GetDefault<UPersonaOptions>()->IsAllowedProperty(Property))
		{
			SegmentError = FString::Printf(TEXT("property '%s' is not allowed for property access by the editor permission lists"), *Property->GetName());
		}
		LeafProperty = Property;
		LeafArrayIndex = StaticArrayIndex;
	};
	Args.ArrayFunction = [&](int32 SegmentIndex, FArrayProperty* Property, int32 ArrayIndex)
	{
		if (SegmentError.IsEmpty() && !GetDefault<UPersonaOptions>()->IsAllowedProperty(Property))
		{
			SegmentError = FString::Printf(TEXT("property '%s' is not allowed for property access by the editor permission lists"), *Property->GetName());
		}
		LeafProperty = Property;
		LeafArrayIndex = ArrayIndex;
	};
	Args.FunctionFunction = [&](int32 SegmentIndex, UFunction* Function, FProperty* ReturnProperty)
	{
		// The compiler expands a getter's whole return value and never applies an index to it.
		if (SegmentError.IsEmpty() && HasIndex(Path[SegmentIndex]))
		{
			SegmentError = FString::Printf(TEXT("getter segment '%s' cannot be indexed"), *Path[SegmentIndex]);
		}
		if (SegmentError.IsEmpty() && !FBlueprintActionDatabase::IsFunctionAllowed(Function, FBlueprintActionDatabase::EPermissionsContext::Property))
		{
			SegmentError = FString::Printf(TEXT("function '%s' is not allowed for property access by the editor permission lists"), *Function->GetName());
		}
		LeafProperty = ReturnProperty;
		LeafArrayIndex = INDEX_NONE;
	};

	if (Editor->ResolvePropertyAccess(Scope, MakeArrayView(Path), Args).Result == EPropertyAccessResolveResult::Failed)
	{
		// The resolver reports no failing segment: the first failing prefix names it. When that prefix's last valid
		// segment is a value without members (a bool, a number), it is the segment that blocks the path.
		FString Reason = FString::Printf(TEXT("segment '%s' (index %d) is not a property or a getter function (one return value, no parameters)"),
			*Path.Last(), Path.Num() - 1);
		for (int32 Count = 1; Count <= Path.Num(); ++Count)
		{
			FProperty* PrefixLeaf = nullptr;
			int32 PrefixIndex = INDEX_NONE;
			if (Editor->ResolvePropertyAccess(Scope, MakeArrayView(Path.GetData(), Count), PrefixLeaf, PrefixIndex).Result != EPropertyAccessResolveResult::Failed)
			{
				if (Count < Path.Num() && PrefixLeaf != nullptr && !IsTraversable(PrefixLeaf, PrefixIndex != INDEX_NONE))
				{
					Reason = FString::Printf(TEXT("segment '%s' (index %d) is a %s and has no members, so the path cannot continue to '%s'"),
						*Path[Count - 1], Count - 1, *PrefixLeaf->GetCPPType(), *Path[Count]);
					break;
				}
				continue;
			}
			Reason = FString::Printf(TEXT("segment '%s' (index %d) is not a property or a getter function (one return value, no parameters) of the preceding segment"),
				*Path[Count - 1], Count - 1);
			break;
		}
		OutError = FCortexCommandRouter::Error(CortexErrorCodes::InvalidField, FString::Printf(
			TEXT("Property access path '%s' does not resolve from %s: %s"), *JoinedPath, *Scope->GetName(), *Reason));
		return false;
	}
	if (!SegmentError.IsEmpty())
	{
		OutError = FCortexCommandRouter::Error(CortexErrorCodes::InvalidField, FString::Printf(
			TEXT("Property access path '%s' cannot be compiled: %s"), *JoinedPath, *SegmentError));
		return false;
	}

	FProperty* PinProperty = LeafProperty;
	if (const FArrayProperty* ArrayLeaf = CastField<FArrayProperty>(LeafProperty))
	{
		if (LeafArrayIndex != INDEX_NONE)
		{
			PinProperty = ArrayLeaf->Inner;
		}
	}
	FEdGraphPinType PinType;
	if (PinProperty == nullptr
		|| !GetDefault<UEdGraphSchema_K2>()->ConvertPropertyToPinType(PinProperty, PinType)
		|| PinType.PinCategory == UEdGraphSchema_K2::PC_Wildcard)
	{
		OutError = FCortexCommandRouter::Error(CortexErrorCodes::InvalidField, FString::Printf(
			TEXT("Property access path '%s' ends in '%s', which has no concrete Blueprint pin type"),
			*JoinedPath, PinProperty != nullptr ? *PinProperty->GetName() : TEXT("<none>")));
		return false;
	}
	return true;
}

bool FCortexGraphPropertyAccess::WritePath(UEdGraphNode* Node, const TArray<FString>& Path)
{
	const FArrayProperty* PathProperty = IsPropertyAccessNode(Node) ? FindPathProperty(Node->GetClass()) : nullptr;
	if (PathProperty == nullptr)
	{
		return false;
	}
	*PathProperty->ContainerPtrToValuePtr<TArray<FString>>(Node) = Path;
	return true;
}

bool FCortexGraphPropertyAccess::ReadPath(const UEdGraphNode* Node, TArray<FString>& OutPath)
{
	const FArrayProperty* PathProperty = IsPropertyAccessNode(Node) ? FindPathProperty(Node->GetClass()) : nullptr;
	if (PathProperty == nullptr)
	{
		OutPath.Reset();
		return false;
	}
	OutPath = *PathProperty->ContainerPtrToValuePtr<TArray<FString>>(Node);
	return true;
}

FName FCortexGraphPropertyAccess::ReadContextId(const UEdGraphNode* Node)
{
	const FNameProperty* ContextProperty = IsPropertyAccessNode(Node)
		? CastField<FNameProperty>(Node->GetClass()->FindPropertyByName(TEXT("ContextId")))
		: nullptr;
	return ContextProperty != nullptr ? *ContextProperty->ContainerPtrToValuePtr<FName>(Node) : NAME_None;
}

TSharedPtr<FJsonObject> FCortexGraphPropertyAccess::Describe(const UEdGraphNode* Node)
{
	TArray<FString> Path;
	if (!ReadPath(Node, Path))
	{
		return nullptr;
	}

	TSharedPtr<FJsonObject> Access = MakeShared<FJsonObject>();
	TArray<TSharedPtr<FJsonValue>> Segments;
	for (const FString& Segment : Path)
	{
		Segments.Add(MakeShared<FJsonValueString>(Segment));
	}
	Access->SetArrayField(TEXT("path"), Segments);

	FString TextPath;
	if (const FTextProperty* TextProperty = CastField<FTextProperty>(Node->GetClass()->FindPropertyByName(TEXT("TextPath"))))
	{
		TextPath = TextProperty->ContainerPtrToValuePtr<FText>(Node)->ToString();
	}
	Access->SetStringField(TEXT("text_path"), TextPath);

	// thread_safe describes the resolved path (what the node records), not the compiled call site; it is false
	// whenever the path does not resolve. A malformed stored path is reported unresolved without reaching the parser.
	bool bResolved = false;
	bool bThreadSafe = false;
	IPropertyAccessEditor* Editor = GetPropertyAccessEditor();
	const UStruct* Scope = ResolveScope(FBlueprintEditorUtils::FindBlueprintForNode(Node));
	if (Editor != nullptr && Scope != nullptr && AllSegmentsWellFormed(Path))
	{
		FProperty* Leaf = nullptr;
		int32 LeafArrayIndex = INDEX_NONE;
		const FPropertyAccessResolveResult Result = Editor->ResolvePropertyAccess(Scope, MakeArrayView(Path), Leaf, LeafArrayIndex);
		bResolved = Result.Result != EPropertyAccessResolveResult::Failed;
		bThreadSafe = bResolved && Result.bIsThreadSafe;
	}
	Access->SetBoolField(TEXT("resolved"), bResolved);
	Access->SetBoolField(TEXT("thread_safe"), bThreadSafe);
	return Access;
}
