#pragma once

#include "CoreMinimal.h"
#include "CortexCommandRouter.h"
#include "WidgetBlueprint.h"
#include "Blueprint/WidgetTree.h"
#include "Components/PanelWidget.h"

namespace CortexUMGUtils
{
    inline UWidgetBlueprint* LoadWidgetBlueprint(
        const FString& AssetPath, FCortexCommandResult& OutError)
    {
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

    inline UWidget* FindWidgetByName(UWidgetTree* WidgetTree, const FString& Name)
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
            if (Widget->GetName() == Name)
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
}
