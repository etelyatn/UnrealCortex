#pragma once

#include "CoreMinimal.h"
#include "CortexCommandRouter.h"

class UMaterial;
class UMaterialExpression;

/** Material-specific layout constants derived from UE SGraphNode rendering.
 *  See docs/plans/2026-02-23-material-layout-sizing-design.md for derivation. */
namespace CortexMaterialLayout
{
	/** SGraphNode horizontal chrome: pin labels + shadow + border on both sides */
	constexpr int32 NodeChromePaddingX = 80;

	/** SGraphNode title bar height */
	constexpr int32 TitleBarHeight = 30;

	/** Preview thumbnail region: 106px viewport + 10px border padding */
	constexpr int32 PreviewHeight = 116;

	/** SGraphNode pin row height (not ME_STD_TAB_HEIGHT which is internal) */
	constexpr int32 PinRowHeight = 24;

	/** Default horizontal spacing for material graphs (wider than blueprint default of 80) */
	constexpr int32 DefaultHorizontalSpacing = 120;

	/** Default vertical spacing for material graphs (wider than blueprint default of 40) */
	constexpr int32 DefaultVerticalSpacing = 60;

	/** Floor for Expression->GetWidth() to handle edge cases */
	constexpr int32 MinNodeWidth = 112;

	/** Floor for Expression->GetHeight() to handle edge cases */
	constexpr int32 MinNodeHeight = 80;

	/** Approximate width of the Material Output graph node */
	constexpr int32 MaterialResultWidth = 250;
}

class FCortexMaterialGraphOps
{
public:
	static FCortexCommandResult ListNodes(const TSharedPtr<FJsonObject>& Params);
	static FCortexCommandResult GetNode(const TSharedPtr<FJsonObject>& Params);
	static FCortexCommandResult AddNode(const TSharedPtr<FJsonObject>& Params);
	static FCortexCommandResult RemoveNode(const TSharedPtr<FJsonObject>& Params);
	static FCortexCommandResult ListConnections(const TSharedPtr<FJsonObject>& Params);
	static FCortexCommandResult Connect(const TSharedPtr<FJsonObject>& Params);
	static FCortexCommandResult Disconnect(const TSharedPtr<FJsonObject>& Params);
	static FCortexCommandResult AutoLayout(const TSharedPtr<FJsonObject>& Params);
	static FCortexCommandResult SetNodeProperty(const TSharedPtr<FJsonObject>& Params);
	static FCortexCommandResult GetNodePins(const TSharedPtr<FJsonObject>& Params);

private:
	static UMaterialExpression* FindExpression(UMaterial* Material, const FString& NodeId);
};
