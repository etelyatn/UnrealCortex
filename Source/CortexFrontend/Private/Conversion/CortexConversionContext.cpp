#include "Conversion/CortexConversionContext.h"
#include "Conversion/CortexDependencyGatherer.h"

FCortexConversionContext::FCortexConversionContext(const FCortexConversionPayload& InPayload)
    : Payload(InPayload)
{
    TabGuid = FGuid::NewGuid();
    TabId = *FString::Printf(TEXT("CortexConversion_%s"), *TabGuid.ToString(EGuidFormats::DigitsWithHyphensLower));
    Document = MakeShared<FCortexCodeDocument>();

    // Derive class name from Blueprint name (strip UE asset prefixes, add A/U type prefix)
    FString DerivedName = InPayload.BlueprintName;
    if (DerivedName.StartsWith(TEXT("WBP_")))
    {
        DerivedName = DerivedName.Mid(4);
    }
    else if (DerivedName.StartsWith(TEXT("ABP_")))
    {
        DerivedName = DerivedName.Mid(4);
    }
    else if (DerivedName.StartsWith(TEXT("BP_")))
    {
        DerivedName = DerivedName.Mid(3);
    }
    else if (DerivedName.StartsWith(TEXT("B_")))
    {
        DerivedName = DerivedName.Mid(2);
    }
    if (!DerivedName.IsEmpty() && DerivedName[0] != TEXT('A') && DerivedName[0] != TEXT('U'))
    {
        if (InPayload.bIsWidgetBlueprint)
        {
            DerivedName = TEXT("U") + DerivedName;
        }
        else if (InPayload.bIsActorDescendant)
        {
            DerivedName = TEXT("A") + DerivedName;
        }
        else
        {
            DerivedName = TEXT("U") + DerivedName;
        }
    }
    else if (InPayload.bIsWidgetBlueprint && !DerivedName.IsEmpty()
        && DerivedName[0] == TEXT('A'))
    {
        // Widget BPs should have U prefix -- correct A to U (e.g., WBP_AWidget -> UWidget)
        DerivedName[0] = TEXT('U');
    }
    // Name already starts with U -- no prefix change needed for widget BPs
    Document->ClassName = DerivedName;

    // Auto-detect target module name from project
    TargetModuleName = FApp::GetProjectName();

    // Auto-select logic-referenced widgets for BindWidget
    SelectedWidgetBindings = InPayload.LogicReferencedWidgets;

    // Gather dependency info from Asset Registry + payload
    DependencyInfo = FCortexDependencyGatherer::GatherDependencies(InPayload);
}
