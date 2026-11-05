#include "Operations/CortexUMGNativeBindingOps.h"
#include "CortexUMGUtils.h"
#include "CortexTypes.h"
#include "WidgetBlueprint.h"
#include "WidgetBlueprintEditorUtils.h"
#include "Animation/WidgetAnimation.h"
#include "Blueprint/UserWidget.h"
#include "Blueprint/WidgetTree.h"
#include "Components/Widget.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "EdGraph/EdGraph.h"
#include "K2Node_Variable.h"
#include "Misc/PackageName.h"
#include "UObject/UnrealType.h"

namespace
{
namespace NativeBindingStatus
{
const TCHAR* const Matched = TEXT("matched");
const TCHAR* const TypeMismatch = TEXT("type_mismatch");
const TCHAR* const Missing = TEXT("missing");
const TCHAR* const OptionalMissing = TEXT("optional_missing");
const TCHAR* const NotTransient = TEXT("not_transient");
}

struct FNativeBindingRecord
{
    FString Kind;
    FString Name;
    bool bOptional = false;
    FString Status;
    FString ExpectedClass;
    FString ActualClass;
    FString NameSuggestion;
    int32 ReferencedByGraphs = 0;
    bool bBlueprintVisible = false;
};

int32 LevenshteinDistance(const FString& A, const FString& B)
{
    TArray<int32> Previous;
    TArray<int32> Current;
    Previous.SetNumUninitialized(B.Len() + 1);
    Current.SetNumUninitialized(B.Len() + 1);
    for (int32 J = 0; J <= B.Len(); ++J)
    {
        Previous[J] = J;
    }
    for (int32 I = 1; I <= A.Len(); ++I)
    {
        Current[0] = I;
        for (int32 J = 1; J <= B.Len(); ++J)
        {
            const int32 Substitution = Previous[J - 1] + (A[I - 1] == B[J - 1] ? 0 : 1);
            Current[J] = FMath::Min3(Previous[J] + 1, Current[J - 1] + 1, Substitution);
        }
        Swap(Previous, Current);
    }
    return Previous[B.Len()];
}

/**
 * Suggested name for a missing member: a case-insensitive exact match first, otherwise the nearest
 * candidate within max(2, len / 4) edits; ties break alphabetically. Empty when none qualifies.
 */
FString SuggestName(const FString& Missing, TArray<FString> Candidates)
{
    Candidates.Sort([](const FString& A, const FString& B) { return A.Compare(B, ESearchCase::CaseSensitive) < 0; });
    for (const FString& Candidate : Candidates)
    {
        if (Candidate.Equals(Missing, ESearchCase::IgnoreCase))
        {
            return Candidate;
        }
    }

    const int32 Limit = FMath::Max(2, Missing.Len() / 4);
    FString Best;
    int32 BestDistance = MAX_int32;
    for (const FString& Candidate : Candidates)
    {
        const int32 Distance = LevenshteinDistance(Missing, Candidate);
        if (Distance <= Limit && Distance < BestDistance)
        {
            Best = Candidate;
            BestDistance = Distance;
        }
    }
    return Best;
}

UClass* ResolveNativeClass(const FString& ClassPathOrName)
{
    if (!ClassPathOrName.Contains(TEXT("/")))
    {
        return FindFirstObjectSafe<UClass>(*ClassPathOrName, EFindFirstObjectOptions::NativeFirst);
    }

    if (UClass* Loaded = FindObject<UClass>(nullptr, *ClassPathOrName))
    {
        return Loaded;
    }
    // Native classes are never loadable from disk; only Blueprint class packages are.
    const FString PackageName = FPackageName::ObjectPathToPackageName(ClassPathOrName);
    if (PackageName.StartsWith(TEXT("/Script/")))
    {
        return nullptr;
    }
    if (FindPackage(nullptr, *PackageName) || FPackageName::DoesPackageExist(PackageName))
    {
        return LoadObject<UClass>(nullptr, *ClassPathOrName);
    }
    return nullptr;
}

/** The engine's widget-binding scan: the first Blueprint in the parent chain that owns widgets. */
TArray<UWidget*> CollectBoundWidgets(const UWidgetBlueprint* Blueprint)
{
    TArray<UWidget*> Widgets;
    for (const UWidgetBlueprint* Scan = Blueprint; Scan; )
    {
        if (Scan->WidgetTree)
        {
            Scan->WidgetTree->GetAllWidgets(Widgets);
        }
        if (Widgets.Num() > 0)
        {
            break;
        }
        Scan = Scan->ParentClass ? Cast<UWidgetBlueprint>(Scan->ParentClass->ClassGeneratedBy) : nullptr;
    }
    return Widgets;
}

/** Widgets of the parent Widget Blueprint of the one that owns the scanned widgets (the compiler's ParentWidgetToBindWidgetMap). */
TArray<UWidget*> CollectParentWidgets(const UWidgetBlueprint* Blueprint)
{
    for (const UWidgetBlueprint* Scan = Blueprint; Scan; )
    {
        TArray<UWidget*> OwnWidgets;
        if (Scan->WidgetTree)
        {
            Scan->WidgetTree->GetAllWidgets(OwnWidgets);
        }
        const bool bOwnsWidgets = OwnWidgets.Num() > 0;
        const UWidgetBlueprint* ParentWidgetBP = Scan->ParentClass ? Cast<UWidgetBlueprint>(Scan->ParentClass->ClassGeneratedBy) : nullptr;
        if (bOwnsWidgets)
        {
            return ParentWidgetBP ? const_cast<UWidgetBlueprint*>(ParentWidgetBP)->GetAllSourceWidgets() : TArray<UWidget*>();
        }
        Scan = ParentWidgetBP;
    }
    return TArray<UWidget*>();
}

/** Self-context variable nodes per member name across every graph of the Blueprint. */
TMap<FName, int32> CountGraphMemberReferences(const UWidgetBlueprint* Blueprint)
{
    TMap<FName, int32> References;
    TArray<UEdGraph*> Graphs;
    Blueprint->GetAllGraphs(Graphs);
    for (const UEdGraph* Graph : Graphs)
    {
        for (const UEdGraphNode* Node : Graph->Nodes)
        {
            const UK2Node_Variable* Variable = Cast<UK2Node_Variable>(Node);
            if (Variable && Variable->VariableReference.IsSelfContext() && !Variable->VariableReference.IsLocalScope())
            {
                ++References.FindOrAdd(Variable->GetVarName());
            }
        }
    }
    return References;
}

/** Animation names across the Blueprint and its Widget Blueprint ancestors, as the compiler scans them. */
TArray<FString> CollectBoundAnimationNames(const UWidgetBlueprint* Blueprint)
{
    TArray<FString> Names;
    for (const UWidgetBlueprint* Scan = Blueprint; Scan; )
    {
        for (const UWidgetAnimation* Animation : Scan->Animations)
        {
            if (Animation)
            {
                Names.Add(Animation->GetName());
            }
        }
        Scan = Scan->ParentClass ? Cast<UWidgetBlueprint>(Scan->ParentClass->ClassGeneratedBy) : nullptr;
    }
    return Names;
}

bool ContainsExactName(const TArray<FString>& Names, const FString& Name)
{
    return Names.ContainsByPredicate([&Name](const FString& Candidate)
    {
        return Candidate.Equals(Name, ESearchCase::CaseSensitive);
    });
}
}

FCortexCommandResult FCortexUMGNativeBindingOps::CheckNativeBindings(const TSharedPtr<FJsonObject>& Params)
{
    const FString AssetPath = Params->GetStringField(TEXT("asset_path"));
    FCortexCommandResult LoadError;
    UWidgetBlueprint* WBP = CortexUMGUtils::LoadWidgetBlueprint(AssetPath, LoadError);
    if (!WBP)
    {
        return LoadError;
    }

    UClass* BindingClass = WBP->ParentClass;
    FString NativeClassParam;
    if (Params->TryGetStringField(TEXT("native_class"), NativeClassParam))
    {
        BindingClass = ResolveNativeClass(NativeClassParam);
        if (!BindingClass)
        {
            return FCortexCommandRouter::Error(CortexErrorCodes::InvalidField,
                FString::Printf(TEXT("native_class not found: %s"), *NativeClassParam));
        }
    }
    if (!BindingClass || !BindingClass->IsChildOf(UUserWidget::StaticClass()))
    {
        return FCortexCommandRouter::Error(CortexErrorCodes::InvalidField,
            FString::Printf(TEXT("native_class must resolve to a UserWidget subclass: %s"),
                BindingClass ? *BindingClass->GetPathName() : TEXT("<none>")));
    }

    const TArray<UWidget*> Widgets = CollectBoundWidgets(WBP);
    const TArray<UWidget*> ParentWidgets = CollectParentWidgets(WBP);
    const TArray<FString> AnimationNames = CollectBoundAnimationNames(WBP);
    const TMap<FName, int32> GraphReferences = CountGraphMemberReferences(WBP);

    TArray<FNativeBindingRecord> Records;
    for (TFieldIterator<FObjectPropertyBase> It(BindingClass); It; ++It)
    {
        const FObjectPropertyBase* Property = *It;
        bool bOptional = false;

        if (Property->PropertyClass && Property->PropertyClass->IsChildOf(UWidget::StaticClass())
            && FWidgetBlueprintEditorUtils::IsBindWidgetProperty(Property, bOptional))
        {
            FNativeBindingRecord Record;
            Record.Kind = TEXT("widget");
            Record.Name = Property->GetName();
            Record.bOptional = bOptional;
            Record.ExpectedClass = Property->PropertyClass->GetName();

            const UWidget* Same = nullptr;
            for (const UWidget* Widget : Widgets)
            {
                if (Widget && Widget->GetName().Equals(Record.Name, ESearchCase::CaseSensitive))
                {
                    Same = Widget;
                    break;
                }
            }
            if (!Same)
            {
                // The compiler also binds a member to the parent Widget Blueprint's own widget of the same name and type.
                for (const UWidget* Widget : ParentWidgets)
                {
                    if (Widget && Widget->GetName().Equals(Record.Name, ESearchCase::CaseSensitive)
                        && Widget->IsA(Property->PropertyClass))
                    {
                        Same = Widget;
                        break;
                    }
                }
            }
            if (Same)
            {
                const bool bCompatible = Same->IsA(Property->PropertyClass);
                Record.Status = bCompatible ? NativeBindingStatus::Matched : NativeBindingStatus::TypeMismatch;
                if (!bCompatible)
                {
                    Record.ActualClass = Same->GetClass()->GetName();
                }
            }
            else
            {
                Record.Status = bOptional ? NativeBindingStatus::OptionalMissing : NativeBindingStatus::Missing;
                TArray<FString> Candidates;
                for (const UWidget* Widget : Widgets)
                {
                    if (Widget && Widget->IsA(Property->PropertyClass))
                    {
                        Candidates.Add(Widget->GetName());
                    }
                }
                Record.NameSuggestion = SuggestName(Record.Name, MoveTemp(Candidates));
            }
            Record.ReferencedByGraphs = GraphReferences.FindRef(FName(*Record.Name));
            Record.bBlueprintVisible = Property->HasAnyPropertyFlags(CPF_BlueprintVisible);
            Records.Add(MoveTemp(Record));
        }
        else if (Property->PropertyClass && Property->PropertyClass->IsChildOf(UWidgetAnimation::StaticClass())
            && FWidgetBlueprintEditorUtils::IsBindWidgetAnimProperty(Property, bOptional))
        {
            FNativeBindingRecord Record;
            Record.Kind = TEXT("animation");
            Record.Name = Property->GetName();
            Record.bOptional = bOptional;
            Record.ExpectedClass = Property->PropertyClass->GetName();

            if (!Property->HasAnyPropertyFlags(CPF_Transient))
            {
                // The widget compiler rejects a BindWidgetAnim member that is not Transient.
                Record.Status = NativeBindingStatus::NotTransient;
            }
            else if (ContainsExactName(AnimationNames, Record.Name))
            {
                Record.Status = NativeBindingStatus::Matched;
            }
            else
            {
                Record.Status = bOptional ? NativeBindingStatus::OptionalMissing : NativeBindingStatus::Missing;
                Record.NameSuggestion = SuggestName(Record.Name, AnimationNames);
            }
            Record.ReferencedByGraphs = GraphReferences.FindRef(FName(*Record.Name));
            Record.bBlueprintVisible = Property->HasAnyPropertyFlags(CPF_BlueprintVisible);
            Records.Add(MoveTemp(Record));
        }
    }

    Records.Sort([](const FNativeBindingRecord& A, const FNativeBindingRecord& B)
    {
        const int32 KindOrder = A.Kind.Compare(B.Kind, ESearchCase::CaseSensitive);
        return KindOrder != 0 ? KindOrder < 0 : A.Name.Compare(B.Name, ESearchCase::CaseSensitive) < 0;
    });

    const TCHAR* const AllStatuses[] = {
        NativeBindingStatus::Matched, NativeBindingStatus::TypeMismatch, NativeBindingStatus::Missing,
        NativeBindingStatus::OptionalMissing, NativeBindingStatus::NotTransient };
    TMap<FString, int32> StatusCounts;
    for (const TCHAR* Status : AllStatuses)
    {
        StatusCounts.Add(Status, 0);
    }

    bool bHasErrors = false;
    int32 NotBlueprintVisibleCount = 0;
    TArray<TSharedPtr<FJsonValue>> BindingsJson;
    for (const FNativeBindingRecord& Record : Records)
    {
        ++StatusCounts.FindChecked(Record.Status);
        // A graph that reads the member fails the compile ("X is not blueprint visible") when the native property lacks exposure.
        const bool bNotBlueprintVisible = Record.ReferencedByGraphs > 0 && !Record.bBlueprintVisible;
        NotBlueprintVisibleCount += bNotBlueprintVisible ? 1 : 0;
        bHasErrors |= Record.Status == NativeBindingStatus::Missing
            || Record.Status == NativeBindingStatus::TypeMismatch
            || Record.Status == NativeBindingStatus::NotTransient
            || bNotBlueprintVisible;

        TSharedPtr<FJsonObject> Json = MakeShared<FJsonObject>();
        Json->SetStringField(TEXT("kind"), Record.Kind);
        Json->SetStringField(TEXT("name"), Record.Name);
        Json->SetBoolField(TEXT("optional"), Record.bOptional);
        Json->SetStringField(TEXT("status"), Record.Status);
        Json->SetStringField(TEXT("expected_class"), Record.ExpectedClass);
        Json->SetNumberField(TEXT("referenced_by_graphs"), Record.ReferencedByGraphs);
        Json->SetBoolField(TEXT("blueprint_visible"), Record.bBlueprintVisible);
        if (!Record.ActualClass.IsEmpty())
        {
            Json->SetStringField(TEXT("actual_class"), Record.ActualClass);
        }
        if (!Record.NameSuggestion.IsEmpty())
        {
            Json->SetStringField(TEXT("name_suggestion"), Record.NameSuggestion);
        }
        BindingsJson.Add(MakeShared<FJsonValueObject>(Json));
    }

    TSharedPtr<FJsonObject> Counts = MakeShared<FJsonObject>();
    for (const TCHAR* Status : AllStatuses)
    {
        Counts->SetNumberField(Status, StatusCounts.FindChecked(Status));
    }
    Counts->SetNumberField(TEXT("not_blueprint_visible"), NotBlueprintVisibleCount);

    TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
    Data->SetStringField(TEXT("asset_path"), AssetPath);
    Data->SetStringField(TEXT("native_class"), BindingClass->GetPathName());
    Data->SetArrayField(TEXT("bindings"), BindingsJson);
    Data->SetObjectField(TEXT("counts"), Counts);
    Data->SetBoolField(TEXT("has_errors"), bHasErrors);
    return FCortexCommandRouter::Success(Data);
}
