// umg.get_tree reports `is_variable` and a typed `slot` summary per node, built by the same helper
// as umg.get_widget so the two reads cannot drift. `names_only` nodes carry neither field.

#include "Misc/AutomationTest.h"
#include "CortexCommandRouter.h"
#include "CortexUMGCommandHandler.h"
#include "WidgetBlueprint.h"
#include "Blueprint/WidgetTree.h"
#include "Blueprint/UserWidget.h"
#include "Components/CanvasPanelSlot.h"
#include "Components/Widget.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "Misc/Guid.h"
#include "UObject/Package.h"
#include "UObject/StrongObjectPtr.h"
#include "UObject/UObjectGlobals.h"

namespace
{
	FString CompactJson(const TSharedPtr<FJsonObject>& Object)
	{
		FString Text;
		if (!Object.IsValid())
		{
			return TEXT("<null>");
		}
		TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> Writer =
			TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Text);
		FJsonSerializer::Serialize(Object.ToSharedRef(), Writer);
		return Text;
	}

	TSharedPtr<FJsonObject> FindNode(const TSharedPtr<FJsonObject>& Node, const FString& Name)
	{
		if (!Node.IsValid())
		{
			return nullptr;
		}
		FString NodeName;
		if (Node->TryGetStringField(TEXT("name"), NodeName) && NodeName == Name)
		{
			return Node;
		}
		const TArray<TSharedPtr<FJsonValue>>* Children = nullptr;
		if (Node->TryGetArrayField(TEXT("children"), Children))
		{
			for (const TSharedPtr<FJsonValue>& Child : *Children)
			{
				if (TSharedPtr<FJsonObject> Found = FindNode(Child->AsObject(), Name))
				{
					return Found;
				}
			}
		}
		return nullptr;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexUMGGetTreeVariableAndSlotTest,
	"Cortex.UMG.GetTreeVariableAndSlot",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexUMGGetTreeVariableAndSlotTest::RunTest(const FString& Parameters)
{
	(void)Parameters;

	UPackage* Package = CreatePackage(*(TEXT("/Temp/CortexUMGGetTreeSlot_") +
		FGuid::NewGuid().ToString(EGuidFormats::Digits)));
	TStrongObjectPtr<UWidgetBlueprint> Blueprint(NewObject<UWidgetBlueprint>(
		Package, TEXT("WBP_CortexGetTreeSlot"), RF_Public | RF_Standalone | RF_Transactional));
	Blueprint->ParentClass = UUserWidget::StaticClass();
	Blueprint->WidgetTree = NewObject<UWidgetTree>(Blueprint.Get(), TEXT("WidgetTree"));
	const FString AssetPath = Blueprint->GetPathName();

	FCortexCommandRouter Router;
	Router.RegisterDomain(TEXT("umg"), TEXT("Cortex UMG"), TEXT("1.0.1"),
		MakeShared<FCortexUMGCommandHandler>());

	auto AddWidget = [&](const FString& WidgetClass, const FString& Name, const FString& ParentName)
	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("asset_path"), AssetPath);
		Params->SetStringField(TEXT("widget_class"), WidgetClass);
		Params->SetStringField(TEXT("name"), Name);
		if (!ParentName.IsEmpty())
		{
			Params->SetStringField(TEXT("parent_name"), ParentName);
		}
		return Router.Execute(TEXT("umg.add_widget"), Params).bSuccess;
	};

	const bool bBuilt = AddWidget(TEXT("CanvasPanel"), TEXT("RootPanel"), FString()) &&
		AddWidget(TEXT("TextBlock"), TEXT("Title"), TEXT("RootPanel")) &&
		AddWidget(TEXT("VerticalBox"), TEXT("List"), TEXT("RootPanel")) &&
		AddWidget(TEXT("Image"), TEXT("Icon"), TEXT("List"));
	if (!TestTrue(TEXT("fixture tree built"), bBuilt))
	{
		return false;
	}

	UWidget* Title = Blueprint->WidgetTree->FindWidget(TEXT("Title"));
	UWidget* Icon = Blueprint->WidgetTree->FindWidget(TEXT("Icon"));
	if (!TestNotNull(TEXT("Title widget"), Title) || !TestNotNull(TEXT("Icon widget"), Icon))
	{
		return false;
	}
	Title->bIsVariable = true;
	Icon->bIsVariable = false;
	UCanvasPanelSlot* TitleSlot = Cast<UCanvasPanelSlot>(Title->Slot);
	if (!TestNotNull(TEXT("Title canvas slot"), TitleSlot))
	{
		return false;
	}
	TitleSlot->SetAnchors(FAnchors(0.25f, 0.5f, 0.75f, 1.0f));
	TitleSlot->SetOffsets(FMargin(4.0f, 8.0f, 16.0f, 32.0f));

	TSharedPtr<FJsonObject> TreeParams = MakeShared<FJsonObject>();
	TreeParams->SetStringField(TEXT("asset_path"), AssetPath);
	const FCortexCommandResult Tree = Router.Execute(TEXT("umg.get_tree"), TreeParams);
	if (!TestTrue(TEXT("get_tree succeeds"), Tree.bSuccess && Tree.Data.IsValid()))
	{
		return false;
	}
	const TSharedPtr<FJsonObject> Root = Tree.Data->GetObjectField(TEXT("root"));
	const TSharedPtr<FJsonObject> TitleNode = FindNode(Root, TEXT("Title"));
	const TSharedPtr<FJsonObject> ListNode = FindNode(Root, TEXT("List"));
	const TSharedPtr<FJsonObject> IconNode = FindNode(Root, TEXT("Icon"));
	if (!TestTrue(TEXT("tree nodes found"), TitleNode.IsValid() && ListNode.IsValid() && IconNode.IsValid()))
	{
		return false;
	}

	bool bFlag = false;
	TestTrue(TEXT("Title carries is_variable"), TitleNode->TryGetBoolField(TEXT("is_variable"), bFlag));
	TestTrue(TEXT("Title is a variable"), bFlag);
	bFlag = true;
	TestTrue(TEXT("Icon carries is_variable"), IconNode->TryGetBoolField(TEXT("is_variable"), bFlag));
	TestFalse(TEXT("Icon is not a variable"), bFlag);

	TestFalse(TEXT("root has no slot"), Root->HasField(TEXT("slot")));

	const TSharedPtr<FJsonObject>* TitleSlotJson = nullptr;
	const TSharedPtr<FJsonObject>* IconSlotJson = nullptr;
	TestTrue(TEXT("Title carries slot"), TitleNode->TryGetObjectField(TEXT("slot"), TitleSlotJson));
	TestTrue(TEXT("Icon carries slot"), IconNode->TryGetObjectField(TEXT("slot"), IconSlotJson));
	if (TitleSlotJson && IconSlotJson)
	{
		FString SlotType;
		TestTrue(TEXT("Title slot declares type"), (*TitleSlotJson)->TryGetStringField(TEXT("type"), SlotType));
		TestEqual(TEXT("Title slot type"), SlotType, FString(TEXT("CanvasPanelSlot")));
		TestTrue(TEXT("Icon slot declares type"), (*IconSlotJson)->TryGetStringField(TEXT("type"), SlotType));
		TestEqual(TEXT("Icon slot type"), SlotType, FString(TEXT("VerticalBoxSlot")));

		// The canvas summary is the very object get_widget returns.
		TSharedPtr<FJsonObject> WidgetParams = MakeShared<FJsonObject>();
		WidgetParams->SetStringField(TEXT("asset_path"), AssetPath);
		WidgetParams->SetStringField(TEXT("widget_name"), TEXT("Title"));
		const FCortexCommandResult WidgetResult = Router.Execute(TEXT("umg.get_widget"), WidgetParams);
		const TSharedPtr<FJsonObject>* WidgetSlotJson = nullptr;
		if (TestTrue(TEXT("get_widget returns slot"),
			WidgetResult.bSuccess && WidgetResult.Data->TryGetObjectField(TEXT("slot"), WidgetSlotJson)))
		{
			TestEqual(TEXT("anchors match get_widget"),
				CompactJson((*TitleSlotJson)->GetObjectField(TEXT("anchors"))),
				CompactJson((*WidgetSlotJson)->GetObjectField(TEXT("anchors"))));
			TestEqual(TEXT("offsets match get_widget"),
				CompactJson((*TitleSlotJson)->GetObjectField(TEXT("offsets"))),
				CompactJson((*WidgetSlotJson)->GetObjectField(TEXT("offsets"))));
			TestEqual(TEXT("whole slot summary matches get_widget"),
				CompactJson(*TitleSlotJson), CompactJson(*WidgetSlotJson));
		}
		const TSharedPtr<FJsonObject>* Anchors = nullptr;
		TestTrue(TEXT("anchors reflect the fixture value"),
			(*TitleSlotJson)->TryGetObjectField(TEXT("anchors"), Anchors));
		if (Anchors)
		{
			double MinX = -1.0;
			(*Anchors)->GetObjectField(TEXT("min"))->TryGetNumberField(TEXT("x"), MinX);
			TestEqual(TEXT("anchor min.x"), MinX, 0.25);
		}
	}

	// names_only keeps hierarchy and names only.
	TreeParams->SetBoolField(TEXT("names_only"), true);
	const FCortexCommandResult Names = Router.Execute(TEXT("umg.get_tree"), TreeParams);
	if (TestTrue(TEXT("names_only get_tree succeeds"), Names.bSuccess && Names.Data.IsValid()))
	{
		const TSharedPtr<FJsonObject> NamesTitle =
			FindNode(Names.Data->GetObjectField(TEXT("root")), TEXT("Title"));
		if (TestTrue(TEXT("names_only Title found"), NamesTitle.IsValid()))
		{
			TestFalse(TEXT("names_only drops is_variable"), NamesTitle->HasField(TEXT("is_variable")));
			TestFalse(TEXT("names_only drops slot"), NamesTitle->HasField(TEXT("slot")));
		}
	}

	ResetLoaders(Package);
	Package->ClearDirtyFlag();
	Blueprint->ClearFlags(RF_Standalone);
	Blueprint->MarkAsGarbage();
	Package->MarkAsGarbage();
	return true;
}
