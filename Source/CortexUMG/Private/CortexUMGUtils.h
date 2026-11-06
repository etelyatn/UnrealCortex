#pragma once

#include "CoreMinimal.h"
#include "CortexCommandRouter.h"
#include "CortexPropertyUtils.h"
#include "WidgetBlueprint.h"
#include "Animation/WidgetAnimation.h"
#include "Blueprint/WidgetTree.h"
#include "Components/PanelWidget.h"
#include "Components/Widget.h"
#include "Misc/PackageName.h"

namespace CortexUMGUtils
{
    inline UWidgetBlueprint* LoadWidgetBlueprint(
        const FString& AssetPath, FCortexCommandResult& OutError)
    {
        FString PkgName = FPackageName::ObjectPathToPackageName(AssetPath);
        if (!FindPackage(nullptr, *PkgName) && !FPackageName::DoesPackageExist(PkgName))
        {
            OutError = FCortexCommandRouter::Error(
                CortexErrorCodes::BlueprintNotFound,
                FString::Printf(TEXT("Widget blueprint package not found: %s"), *AssetPath));
            return nullptr;
        }

        UObject* Obj = StaticLoadObject(UWidgetBlueprint::StaticClass(), nullptr, *AssetPath);
        UWidgetBlueprint* WBP = Cast<UWidgetBlueprint>(Obj);
        if (!WBP)
        {
            OutError = FCortexCommandRouter::Error(
                CortexErrorCodes::BlueprintNotFound,
                FString::Printf(TEXT("Widget blueprint not found: %s"), *AssetPath));
            return nullptr;
        }
        if (!WBP->WidgetTree)
        {
            OutError = FCortexCommandRouter::Error(
                CortexErrorCodes::BlueprintNotFound,
                FString::Printf(TEXT("Widget blueprint has no WidgetTree: %s"), *AssetPath));
            return nullptr;
        }
        return WBP;
    }

    inline UWidget* FindWidgetByName(UWidgetTree* WidgetTree, const FString& Name,
        ESearchCase::Type SearchCase = ESearchCase::IgnoreCase)
    {
        if (!WidgetTree || !WidgetTree->RootWidget)
        {
            return nullptr;
        }

        TFunction<UWidget*(UWidget*)> Search = [&](UWidget* Widget) -> UWidget*
        {
            if (!Widget)
            {
                return nullptr;
            }
            if (Widget->GetName().Equals(Name, SearchCase))
            {
                return Widget;
            }

            if (UPanelWidget* Panel = Cast<UPanelWidget>(Widget))
            {
                for (int32 i = 0; i < Panel->GetChildrenCount(); ++i)
                {
                    if (UWidget* Found = Search(Panel->GetChildAt(i)))
                    {
                        return Found;
                    }
                }
            }
            return nullptr;
        };

        return Search(WidgetTree->RootWidget);
    }

    inline bool WidgetNameExists(UWidgetTree* WidgetTree, const FString& Name)
    {
        return FindWidgetByName(WidgetTree, Name) != nullptr;
    }

    inline int32 CountWidgets(UWidget* Widget)
    {
        if (!Widget)
        {
            return 0;
        }

        int32 Count = 1;
        if (UPanelWidget* Panel = Cast<UPanelWidget>(Widget))
        {
            for (int32 i = 0; i < Panel->GetChildrenCount(); ++i)
            {
                Count += CountWidgets(Panel->GetChildAt(i));
            }
        }
        return Count;
    }

    /**
     * Seeds an empty identity map with the native compiler's deterministic identities for every
     * pre-existing source widget and animation. Assets saved before the map existed start empty, and
     * the compiler only seeds such a map while it is still empty, so the first mutation has to seed it
     * before registering anything new. Populated maps are the authority and are never rewritten.
     */
    inline void InitializeLegacyIdentityMap(UWidgetBlueprint* WidgetBlueprint)
    {
        if (!WidgetBlueprint || !WidgetBlueprint->WidgetVariableNameToGuidMap.IsEmpty())
        {
            return;
        }

        WidgetBlueprint->ForEachSourceWidget([WidgetBlueprint](UWidget* Widget)
        {
            if (Widget)
            {
                WidgetBlueprint->WidgetVariableNameToGuidMap.Add(
                    Widget->GetFName(), FGuid::NewDeterministicGuid(Widget->GetPathName()));
            }
        });

        for (UWidgetAnimation* Existing : WidgetBlueprint->Animations)
        {
            if (Existing)
            {
                WidgetBlueprint->WidgetVariableNameToGuidMap.Add(
                    Existing->GetFName(), FGuid::NewDeterministicGuid(Existing->GetPathName()));
            }
        }
    }

    inline bool ResolvePropertyPath(UObject* Object, const FString& PropertyPath, FProperty*& OutProperty, void*& OutValuePtr)
    {
        if (PropertyPath.StartsWith(TEXT("slot.")))
        {
            UWidget* Widget = Cast<UWidget>(Object);
            if (!Widget || !Widget->Slot)
            {
                return false;
            }

            const FString SlotPath = PropertyPath.Mid(5);
            if (SlotPath.IsEmpty())
            {
                return false;
            }

            return FCortexPropertyUtils::ResolvePropertyPath(Widget->Slot, SlotPath, OutProperty, OutValuePtr);
        }

        return FCortexPropertyUtils::ResolvePropertyPath(Object, PropertyPath, OutProperty, OutValuePtr);
    }
}
