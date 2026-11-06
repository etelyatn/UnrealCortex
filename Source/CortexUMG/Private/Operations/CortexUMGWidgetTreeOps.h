#pragma once

#include "CoreMinimal.h"
#include "CortexCommandRouter.h"

class UWidget;

class FCortexUMGWidgetTreeOps
{
public:
    static FCortexCommandResult AddWidget(const TSharedPtr<FJsonObject>& Params);
    static FCortexCommandResult RemoveWidget(const TSharedPtr<FJsonObject>& Params);
    static FCortexCommandResult Reparent(const TSharedPtr<FJsonObject>& Params);
    static FCortexCommandResult GetTree(const TSharedPtr<FJsonObject>& Params);
    static FCortexCommandResult GetWidget(const TSharedPtr<FJsonObject>& Params);
    static FCortexCommandResult SetWidgetVariable(const TSharedPtr<FJsonObject>& Params);
    static FCortexCommandResult RenameWidget(const TSharedPtr<FJsonObject>& Params);
    static FCortexCommandResult ListWidgetClasses(const TSharedPtr<FJsonObject>& Params);
    static FCortexCommandResult DuplicateWidget(const TSharedPtr<FJsonObject>& Params);

private:
    /**
     * One shared traversal budget for a bounded tree read: the depth bound, the remaining node
     * budget and the projection flags, plus the honest accounting the caller reports.
     * A node is only emitted after this budget is inspected, so a cut branch is never built as JSON.
     */
    struct FTreeReadBudget
    {
        /** Deepest retained level; the root is 0. Negative means unlimited depth. */
        int32 MaxDepth = -1;
        /** Nodes the walk may still emit, the root included. Negative means unlimited. */
        int32 RemainingNodes = -1;
        /** Names-only projection: keeps hierarchy/name/child counts and drops class identity. */
        bool bNamesOnly = false;

        /** Nodes actually emitted by the walk. */
        int32 ReturnedWidgets = 0;
        /** True when the depth bound omitted deeper widgets. */
        bool bDepthCut = false;
        /** True when the node budget omitted remaining widgets. */
        bool bNodeCut = false;
    };

    static UClass* ResolveWidgetClass(const FString& ClassName);
    static TSharedPtr<FJsonObject> BuildWidgetTreeJson(UWidget* Widget, int32 Depth, FTreeReadBudget& Budget);
};
