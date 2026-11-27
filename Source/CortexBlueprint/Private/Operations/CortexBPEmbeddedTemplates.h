#pragma once

#include "CoreMinimal.h"
#include "UObject/WeakObjectPtrTemplates.h"

class FJsonObject;
class FJsonValue;
class UBlueprint;
class UClass;
class UUserWidget;

/**
 * Embedded UUserWidget templates in loaded Widget Blueprints.
 *
 * A Widget Blueprint placed inside another one is a plain subobject of the outer widget tree (no archetype link), and its
 * serialization is a delta against the class default object. A class-default change on the inner Blueprint (set_class_defaults,
 * reparent) therefore leaves every already-instanced template with the OLD default, and saving the outer Blueprint writes that old
 * value as an instance override. This helper records, before a mutation, which template properties still follow the class default
 * and which are overrides; afterwards it reports (and with resync_embedded repairs) the templates.
 *
 * Tracked properties are exactly the ones umg.get_embedded_widget_diff can show: CPF_Edit properties declared below UUserWidget
 * (the Blueprint's variables and native UPROPERTYs of the embedded class; UWidget/UUserWidget members are excluded), minus transient,
 * deprecated, static-array, delegate and instanced-subobject properties. A reported property is therefore always visible in the diff.
 *
 * Resync follows the engine's details-panel rule (FWidgetBlueprintEditor, PropagateDefaultPropertyChange): a template receives the new
 * default only when its value followed the old default before the change (FProperty::Identical against the old CDO, recorded before the
 * mutation); an intentional override is never overwritten. Values are compared and copied with the property itself, never through text.
 * Only the loaded outer Widget Blueprints are scanned.
 *
 * UWidgetBlueprint lives in UMGEditor, which CortexBlueprint does not link: Widget Blueprints are recognised by class and their tree is
 * read through CortexBPWidgetUtils.
 */
class FCortexBPEmbeddedTemplates
{
public:
	/** What the loaded embedded templates of one Widget Blueprint's class held before the class was mutated. */
	struct FPreState
	{
		/** False unless the Blueprint is a compiled Widget Blueprint; ReportInto then does nothing. */
		bool bApplicable = false;
		TWeakObjectPtr<UBlueprint> Blueprint;

		/** Per loaded embedded instance (key: outer asset path + '|' + widget name): the tracked properties that were identical to the old class default. */
		TMap<FString, TSet<FName>> FollowsDefault;

		/** A loaded embedded instance's property whose value differed from the old class default (display text only). */
		struct FOverride
		{
			FString OuterPath;
			FString WidgetName;
			FName Property;
			FString Value;
		};
		TArray<FOverride> Overrides;
	};

	/** Read-only. */
	static FPreState Capture(UBlueprint* Blueprint);

	/**
	 * Writes the post-mutation report into Data (no-op when Before is not applicable).
	 *
	 * bClassCurrent: the Blueprint's generated class already reflects the mutation (set_class_defaults; a compiled reparent). When false
	 * (a reparent with compile:false) the class is not recompiled yet, so embedded_instances is not reported, embedded_report_pending_compile
	 * is set, and dropped_overrides is resolved against NewParent plus the Blueprint's own variables instead of the old class.
	 *
	 *  - embedded_instances: [{asset_path, widget_name, properties, resynced}] templates of the class in other loaded Widget Blueprints that
	 *    followed the old default of a property and no longer equal the class default;
	 *  - dropped_overrides (bReportDropped): [{asset_path, widget_name, property, instance_value}] instance overrides recorded before the
	 *    mutation whose property no longer exists on the class;
	 *  - with bResync: the class default is copied into exactly the embedded_instances properties and the outer Blueprints are marked
	 *    modified (one undo step); bSaveResynced then saves them (embedded_saved_assets).
	 * Problems that do not fail the command (a save that failed) are appended to OutWarnings.
	 */
	static void ReportInto(
		const FPreState& Before,
		bool bClassCurrent,
		const UClass* NewParent,
		bool bResync,
		bool bSaveResynced,
		bool bReportDropped,
		const TSharedRef<FJsonObject>& Data,
		TArray<FString>& OutWarnings);
};
