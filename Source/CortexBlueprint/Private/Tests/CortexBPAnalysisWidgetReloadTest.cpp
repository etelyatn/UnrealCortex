#include "Misc/AutomationTest.h"
#include "CortexBPCommandHandler.h"
#include "CortexTypes.h"
#include "Blueprint/WidgetTree.h"
#include "Components/Button.h"
#include "Components/CanvasPanel.h"
#include "Components/ListView.h"
#include "Components/NamedSlot.h"
#include "Components/TextBlock.h"
#include "Blueprint/UserWidget.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Engine/Blueprint.h"
#include "HAL/FileManager.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Misc/Guid.h"
#include "Editor.h"
#include "Misc/PackageName.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"

// Regression for issue #119 (D1): UWidgetTree::PostLoad empties AllWidgets, which is only refilled in PreSave.
// Analysis must enumerate the tree through UWidgetTree::GetAllWidgets so a cold-loaded Widget Blueprint
// still reports its widgets (including those nested in a NamedSlot) and the widget dependencies they imply.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexBPAnalysisWidgetEntitiesAfterReloadTest,
	"Cortex.Blueprint.Analysis.WidgetEntitiesAfterReload",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexBPAnalysisWidgetEntitiesAfterReloadTest::RunTest(const FString& Parameters)
{
	FCortexBPCommandHandler Handler;
	const FString Suffix = FGuid::NewGuid().ToString(EGuidFormats::Digits).Right(8);
	const FString BlueprintName = FString::Printf(TEXT("WBP_Reload_%s"), *Suffix);
	const FString TestPath = TEXT("/Game/Temp/Cortex119");
	const FString PackageName = FString::Printf(TEXT("%s/%s"), *TestPath, *BlueprintName);
	const FString ObjectPath = FString::Printf(TEXT("%s.%s"), *PackageName, *BlueprintName);

	auto Cleanup = [&PackageName, &ObjectPath]()
	{
		if (UPackage* Pkg = FindPackage(nullptr, *PackageName))
		{
			ResetLoaders(Pkg);
			if (UObject* Asset = FindObject<UObject>(nullptr, *ObjectPath))
			{
				Asset->MarkAsGarbage();
			}
			Pkg->MarkAsGarbage();
		}
		GEditor->ResetTransaction(FText::FromString(TEXT("Cortex119 reload test cleanup")));
		CollectGarbage(GARBAGE_COLLECTION_KEEPFLAGS);

		FString PackageFilename;
		if (FPackageName::TryConvertLongPackageNameToFilename(
			PackageName, PackageFilename, FPackageName::GetAssetPackageExtension()))
		{
			IFileManager::Get().Delete(*PackageFilename);
		}
	};

	{
		TSharedPtr<FJsonObject> CreateParams = MakeShared<FJsonObject>();
		CreateParams->SetStringField(TEXT("name"), BlueprintName);
		CreateParams->SetStringField(TEXT("path"), TestPath);
		CreateParams->SetStringField(TEXT("type"), TEXT("Widget"));
		const FCortexCommandResult CreateResult = Handler.Execute(TEXT("create"), CreateParams);
		if (!CreateResult.bSuccess)
		{
			AddInfo(TEXT("Skipping: Widget Blueprint creation unavailable in current test environment"));
			Cleanup();
			return true;
		}
	}

	UBlueprint* WidgetBP = LoadObject<UBlueprint>(nullptr, *ObjectPath);
	if (!WidgetBP)
	{
		AddError(TEXT("Created Widget Blueprint did not load"));
		Cleanup();
		return false;
	}

	const FObjectProperty* WidgetTreeProp = CastField<FObjectProperty>(
		WidgetBP->GetClass()->FindPropertyByName(TEXT("WidgetTree")));
	UWidgetTree* Tree = WidgetTreeProp
		? Cast<UWidgetTree>(WidgetTreeProp->GetObjectPropertyValue_InContainer(WidgetBP))
		: nullptr;
	if (!Tree)
	{
		AddError(TEXT("Widget Blueprint has no WidgetTree"));
		Cleanup();
		return false;
	}

	// CanvasPanel root -> TextBlock, Button, NamedSlot (-> NamedSlotChild), ListView (entry class set).
	UCanvasPanel* Root = Tree->ConstructWidget<UCanvasPanel>(UCanvasPanel::StaticClass(), TEXT("RootCanvas"));
	Tree->RootWidget = Root;
	UTextBlock* Text = Tree->ConstructWidget<UTextBlock>(UTextBlock::StaticClass(), TEXT("TitleText"));
	UButton* Button = Tree->ConstructWidget<UButton>(UButton::StaticClass(), TEXT("ActionButton"));
	UNamedSlot* Slot = Tree->ConstructWidget<UNamedSlot>(UNamedSlot::StaticClass(), TEXT("ContentSlot"));
	UTextBlock* SlotChild = Tree->ConstructWidget<UTextBlock>(UTextBlock::StaticClass(), TEXT("SlotChildText"));
	UListView* List = Tree->ConstructWidget<UListView>(UListView::StaticClass(), TEXT("ItemList"));
	// EntryWidgetClass is protected on UListViewBase; set it through its reflected property.
	const FClassProperty* EntryClassProp = CastField<FClassProperty>(
		UListView::StaticClass()->FindPropertyByName(TEXT("EntryWidgetClass")));
	if (!EntryClassProp)
	{
		AddError(TEXT("ListView has no EntryWidgetClass property"));
		Cleanup();
		return false;
	}
	EntryClassProp->SetObjectPropertyValue_InContainer(List, UUserWidget::StaticClass());
	Root->AddChild(Text);
	Root->AddChild(Button);
	Root->AddChild(Slot);
	Root->AddChild(List);
	Slot->SetContent(SlotChild);

	const TArray<UWidget*> Built = {Root, Text, Button, Slot, SlotChild, List};
	TSet<FString> ExpectedNames;
	for (UWidget* Widget : Built)
	{
		Widget->bIsVariable = true;
		ExpectedNames.Add(Widget->GetName());
	}

	FKismetEditorUtilities::CompileBlueprint(WidgetBP);

	{
		UPackage* Pkg = WidgetBP->GetOutermost();
		FString PackageFilename;
		if (!FPackageName::TryConvertLongPackageNameToFilename(
			PackageName, PackageFilename, FPackageName::GetAssetPackageExtension()))
		{
			AddError(TEXT("Failed to resolve package filename for save"));
			Cleanup();
			return false;
		}

		FSavePackageArgs SaveArgs;
		SaveArgs.TopLevelFlags = RF_Public | RF_Standalone;
		if (!UPackage::SavePackage(Pkg, WidgetBP, *PackageFilename, SaveArgs))
		{
			AddError(TEXT("SavePackage failed"));
			Cleanup();
			return false;
		}
	}

	// Force a cold load: purge the in-memory asset and package so analyze_for_migration runs PostLoad on the tree.
	{
		UPackage* Pkg = FindPackage(nullptr, *PackageName);
		if (Pkg)
		{
			ResetLoaders(Pkg);
			if (UObject* Asset = FindObject<UObject>(nullptr, *ObjectPath))
			{
				Asset->ClearFlags(RF_Public | RF_Standalone);
				Asset->MarkAsGarbage();
			}
			Pkg->MarkAsGarbage();
		}
		// The editor transaction buffer holds the created asset (via the create command's transaction) and would keep it alive.
		GEditor->ResetTransaction(FText::FromString(TEXT("Cortex119 reload test")));
		CollectGarbage(RF_NoFlags);
		if (!TestNull(TEXT("Widget Blueprint must be unloaded before analysis"),
			FindObject<UObject>(nullptr, *ObjectPath)))
		{
			Cleanup();
			return false;
		}
	}

	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("asset_path"), PackageName);
		const FCortexCommandResult Result = Handler.Execute(TEXT("analyze_for_migration"), Params);
		TestTrue(TEXT("analyze_for_migration should succeed after reload"), Result.bSuccess);
		if (!Result.Data.IsValid())
		{
			AddError(TEXT("Result data is null"));
			Cleanup();
			return false;
		}

		TSet<FString> ActualNames;
		const TArray<TSharedPtr<FJsonValue>>* WidgetsArray = nullptr;
		if (TestTrue(TEXT("widgets field should exist"), Result.Data->TryGetArrayField(TEXT("widgets"), WidgetsArray)))
		{
			for (const TSharedPtr<FJsonValue>& Value : *WidgetsArray)
			{
				const TSharedPtr<FJsonObject> Entry = Value->AsObject();
				if (Entry.IsValid())
				{
					ActualNames.Add(Entry->GetStringField(TEXT("name")));
				}
			}
		}
		TestEqual(TEXT("widget count after cold load"), ActualNames.Num(), ExpectedNames.Num());
		for (const FString& Expected : ExpectedNames)
		{
			TestTrue(*FString::Printf(TEXT("widgets contains %s"), *Expected), ActualNames.Contains(Expected));
		}

		const TSharedPtr<FJsonObject>* Dependencies = nullptr;
		if (TestTrue(TEXT("widget_dependencies should exist"),
			Result.Data->TryGetObjectField(TEXT("widget_dependencies"), Dependencies)))
		{
			const TArray<TSharedPtr<FJsonValue>>* References = nullptr;
			if (TestTrue(TEXT("references_widgets should exist"),
				(*Dependencies)->TryGetArrayField(TEXT("references_widgets"), References)))
			{
				bool bHasEntryClass = false;
				for (const TSharedPtr<FJsonValue>& Reference : *References)
				{
					bHasEntryClass |= Reference->AsString() == UUserWidget::StaticClass()->GetPathName();
				}
				TestTrue(TEXT("references_widgets should list the ListView entry widget class"), bHasEntryClass);
			}
		}
	}

	Cleanup();
	return true;
}
