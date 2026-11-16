// Regression coverage for issue #219: widget-tree mutations must keep the hierarchy acyclic, never
// lose or orphan a widget behind a success response, release removed names, and journal every object
// they write so editor undo restores the change.

#include "Misc/AutomationTest.h"
#include "CortexCommandRouter.h"
#include "CortexUMGCommandHandler.h"
#include "WidgetBlueprint.h"
#include "Blueprint/WidgetTree.h"
#include "Blueprint/UserWidget.h"
#include "Components/Button.h"
#include "Components/CanvasPanel.h"
#include "Components/RichTextBlock.h"
#include "Components/TextBlock.h"
#include "Components/VerticalBox.h"
#include "Components/VerticalBoxSlot.h"
#include "Dom/JsonObject.h"
#include "Editor.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "KismetCompiler.h"
#include "Misc/ScopeExit.h"
#include "UObject/Package.h"

namespace
{
/**
 * Root (CanvasPanel)
 *   Box (VerticalBox)
 *     Label (TextBlock)
 *     InnerBox (VerticalBox)
 *   Btn (Button)
 *     BtnText (TextBlock)
 */
struct FCortexUMGTreeIntegrityFixture
{
	UPackage* Package = nullptr;
	UWidgetBlueprint* Blueprint = nullptr;
	UCanvasPanel* Root = nullptr;
	UVerticalBox* Box = nullptr;
	UTextBlock* Label = nullptr;
	UVerticalBox* InnerBox = nullptr;
	UButton* Btn = nullptr;
	UTextBlock* BtnText = nullptr;
	FCortexCommandRouter Router;

	explicit FCortexUMGTreeIntegrityFixture(FAutomationTestBase& Test)
	{
		Package = CreatePackage(*(TEXT("/Temp/CortexUMGTreeIntegrity_")
			+ FGuid::NewGuid().ToString(EGuidFormats::Digits)));
		Blueprint = NewObject<UWidgetBlueprint>(
			Package, TEXT("WBP_TreeIntegrity"), RF_Public | RF_Standalone | RF_Transactional);
		Blueprint->ParentClass = UUserWidget::StaticClass();
		Blueprint->WidgetTree = NewObject<UWidgetTree>(
			Blueprint, UWidgetTree::StaticClass(), TEXT("WidgetTree"), RF_Transactional);

		UWidgetTree* Tree = Blueprint->WidgetTree;
		Root = Tree->ConstructWidget<UCanvasPanel>(UCanvasPanel::StaticClass(), TEXT("Root"));
		Tree->RootWidget = Root;
		Box = Tree->ConstructWidget<UVerticalBox>(UVerticalBox::StaticClass(), TEXT("Box"));
		Root->AddChild(Box);
		Label = Tree->ConstructWidget<UTextBlock>(UTextBlock::StaticClass(), TEXT("Label"));
		Box->AddChild(Label);
		InnerBox = Tree->ConstructWidget<UVerticalBox>(UVerticalBox::StaticClass(), TEXT("InnerBox"));
		Box->AddChild(InnerBox);
		Btn = Tree->ConstructWidget<UButton>(UButton::StaticClass(), TEXT("Btn"));
		Root->AddChild(Btn);
		BtnText = Tree->ConstructWidget<UTextBlock>(UTextBlock::StaticClass(), TEXT("BtnText"));
		Btn->AddChild(BtnText);

		// Native undo dereferences the generated class, so compile the fixture once.
		FCompilerResultsLog Compile;
		FKismetEditorUtilities::CompileBlueprint(Blueprint, EBlueprintCompileOptions::None, &Compile);
		Test.TestEqual(TEXT("fixture compiles without errors"), Compile.NumErrors, 0);

		Router.RegisterDomain(TEXT("umg"), TEXT("Cortex UMG"), TEXT("1.0.1"),
			MakeShared<FCortexUMGCommandHandler>());
	}

	~FCortexUMGTreeIntegrityFixture()
	{
		if (GEditor != nullptr)
		{
			GEditor->ResetTransaction(FText::FromString(TEXT("Cortex UMG Tree Integrity Cleanup")));
		}
		Blueprint->MarkAsGarbage();
		Package->MarkAsGarbage();
	}

	FCortexCommandResult Run(const TCHAR* Command, const TMap<FString, FString>& Strings,
		const TCHAR* NumberKey = nullptr, int32 Number = 0)
	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("asset_path"), Blueprint->GetPathName());
		for (const TPair<FString, FString>& Pair : Strings)
		{
			Params->SetStringField(Pair.Key, Pair.Value);
		}
		if (NumberKey != nullptr)
		{
			Params->SetNumberField(NumberKey, Number);
		}
		return Router.Execute(FString(TEXT("umg.")) + Command, Params);
	}

	/** Any object of that name outered to the tree, attached or not. */
	UObject* FindTreeObject(const TCHAR* Name) const
	{
		return StaticFindObjectFast(UObject::StaticClass(), Blueprint->WidgetTree, FName(Name));
	}

	/** The fixture hierarchy is exactly as constructed. */
	void ExpectUnchanged(FAutomationTestBase& Test, const FString& Stage) const
	{
		Test.TestEqual(Stage + TEXT(": Root stays the root"), Blueprint->WidgetTree->RootWidget.Get(), (UWidget*)Root);
		Test.TestNull(Stage + TEXT(": Root has no parent"), Root->GetParent());
		Test.TestEqual(Stage + TEXT(": Root child count"), Root->GetChildrenCount(), 2);
		Test.TestEqual(Stage + TEXT(": Box at Root[0]"), Root->GetChildAt(0), (UWidget*)Box);
		Test.TestEqual(Stage + TEXT(": Btn at Root[1]"), Root->GetChildAt(1), (UWidget*)Btn);
		Test.TestEqual(Stage + TEXT(": Box child count"), Box->GetChildrenCount(), 2);
		Test.TestEqual(Stage + TEXT(": Label at Box[0]"), Box->GetChildAt(0), (UWidget*)Label);
		Test.TestEqual(Stage + TEXT(": InnerBox at Box[1]"), Box->GetChildAt(1), (UWidget*)InnerBox);
		Test.TestEqual(Stage + TEXT(": InnerBox is empty"), InnerBox->GetChildrenCount(), 0);
		Test.TestEqual(Stage + TEXT(": Btn holds BtnText"), Btn->GetChildAt(0), (UWidget*)BtnText);
	}
};

bool CortexUMGTreeIntegrityCanUndo()
{
	return GEditor != nullptr && GEditor->Trans != nullptr && GEditor->CanTransact();
}
} // namespace

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexUMGTreeIntegrityReparentTest,
	"Cortex.UMG.TreeIntegrity.ReparentRefusals",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexUMGTreeIntegrityReparentTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	FCortexUMGTreeIntegrityFixture F(*this);

	// A cycle makes every later name lookup recurse without end, so stop at the first accepted one.
	const FCortexCommandResult RootIntoDescendant =
		F.Run(TEXT("reparent"), {{TEXT("widget_name"), TEXT("Root")}, {TEXT("new_parent"), TEXT("Box")}});
	TestFalse(TEXT("reparenting the root under its descendant is refused"), RootIntoDescendant.bSuccess);
	if (RootIntoDescendant.bSuccess)
	{
		return false;
	}
	TestEqual(TEXT("root-into-descendant error code"), RootIntoDescendant.ErrorCode, CortexErrorCodes::InvalidParent);
	F.ExpectUnchanged(*this, TEXT("root into descendant"));

	const FCortexCommandResult IntoSelf =
		F.Run(TEXT("reparent"), {{TEXT("widget_name"), TEXT("Box")}, {TEXT("new_parent"), TEXT("Box")}});
	TestFalse(TEXT("reparenting a widget under itself is refused"), IntoSelf.bSuccess);
	if (IntoSelf.bSuccess)
	{
		return false;
	}
	TestEqual(TEXT("self error code"), IntoSelf.ErrorCode, CortexErrorCodes::InvalidParent);
	F.ExpectUnchanged(*this, TEXT("into self"));

	const FCortexCommandResult IntoDescendant =
		F.Run(TEXT("reparent"), {{TEXT("widget_name"), TEXT("Box")}, {TEXT("new_parent"), TEXT("InnerBox")}});
	TestFalse(TEXT("reparenting a widget under its descendant is refused"), IntoDescendant.bSuccess);
	if (IntoDescendant.bSuccess)
	{
		return false;
	}
	TestEqual(TEXT("descendant error code"), IntoDescendant.ErrorCode, CortexErrorCodes::InvalidParent);
	F.ExpectUnchanged(*this, TEXT("into descendant"));

	const FCortexCommandResult IntoFullButton =
		F.Run(TEXT("reparent"), {{TEXT("widget_name"), TEXT("Label")}, {TEXT("new_parent"), TEXT("Btn")}});
	TestFalse(TEXT("reparenting into a full single-child widget is refused"), IntoFullButton.bSuccess);
	TestEqual(TEXT("full parent error code"), IntoFullButton.ErrorCode, CortexErrorCodes::InvalidParent);
	F.ExpectUnchanged(*this, TEXT("into full button"));

	const FCortexCommandResult BadSlot = F.Run(TEXT("reparent"),
		{{TEXT("widget_name"), TEXT("Label")}, {TEXT("new_parent"), TEXT("Root")}}, TEXT("slot_index"), 99);
	TestFalse(TEXT("an out-of-range slot_index is refused"), BadSlot.bSuccess);
	TestEqual(TEXT("slot error code"), BadSlot.ErrorCode, CortexErrorCodes::InvalidSlotIndex);
	F.ExpectUnchanged(*this, TEXT("bad slot"));

	const FCortexCommandResult Moved = F.Run(TEXT("reparent"),
		{{TEXT("widget_name"), TEXT("Label")}, {TEXT("new_parent"), TEXT("Root")}}, TEXT("slot_index"), 0);
	TestTrue(TEXT("a valid slot_index reparent succeeds"), Moved.bSuccess);
	TestEqual(TEXT("Label lands at Root[0]"), F.Root->GetChildAt(0), (UWidget*)F.Label);
	TestEqual(TEXT("Box lost Label"), F.Box->GetChildIndex(F.Label), INDEX_NONE);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexUMGTreeIntegrityAddTest,
	"Cortex.UMG.TreeIntegrity.AddRefusalsLeaveNoOrphan",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexUMGTreeIntegrityAddTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	FCortexUMGTreeIntegrityFixture F(*this);

	const FCortexCommandResult IntoFull = F.Run(TEXT("add_widget"),
		{{TEXT("parent_name"), TEXT("Btn")}, {TEXT("widget_class"), TEXT("Image")}, {TEXT("name"), TEXT("Icon")}});
	TestFalse(TEXT("adding to a full Button is refused"), IntoFull.bSuccess);
	TestEqual(TEXT("full parent error code"), IntoFull.ErrorCode, CortexErrorCodes::InvalidParent);
	TestNull(TEXT("no Icon object is left in the tree outer"), F.FindTreeObject(TEXT("Icon")));
	TestFalse(TEXT("Icon is not registered as a variable"),
		F.Blueprint->WidgetVariableNameToGuidMap.Contains(TEXT("Icon")));

	const FCortexCommandResult MissingParent = F.Run(TEXT("add_widget"),
		{{TEXT("parent_name"), TEXT("Nope")}, {TEXT("widget_class"), TEXT("Image")}, {TEXT("name"), TEXT("Lost")}});
	TestFalse(TEXT("a missing parent is refused"), MissingParent.bSuccess);
	TestNull(TEXT("a missing-parent refusal leaves no object"), F.FindTreeObject(TEXT("Lost")));

	const FCortexCommandResult LeafParent = F.Run(TEXT("add_widget"),
		{{TEXT("parent_name"), TEXT("Label")}, {TEXT("widget_class"), TEXT("Image")}, {TEXT("name"), TEXT("UnderLeaf")}});
	TestFalse(TEXT("a non-panel parent is refused"), LeafParent.bSuccess);
	TestNull(TEXT("a non-panel refusal leaves no object"), F.FindTreeObject(TEXT("UnderLeaf")));

	const FCortexCommandResult BadSlot = F.Run(TEXT("add_widget"),
		{{TEXT("parent_name"), TEXT("Box")}, {TEXT("widget_class"), TEXT("Image")}, {TEXT("name"), TEXT("Slotless")}},
		TEXT("slot_index"), 99);
	TestFalse(TEXT("an out-of-range slot is refused"), BadSlot.bSuccess);
	TestNull(TEXT("a slot refusal leaves no object"), F.FindTreeObject(TEXT("Slotless")));

	const FCortexCommandResult SecondRoot = F.Run(TEXT("add_widget"),
		{{TEXT("widget_class"), TEXT("CanvasPanel")}, {TEXT("name"), TEXT("SecondRoot")}});
	TestFalse(TEXT("a second root is refused"), SecondRoot.bSuccess);
	TestNull(TEXT("a second-root refusal leaves no object"), F.FindTreeObject(TEXT("SecondRoot")));

	F.ExpectUnchanged(*this, TEXT("after add refusals"));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexUMGTreeIntegrityDuplicateTest,
	"Cortex.UMG.TreeIntegrity.DuplicateRefusals",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexUMGTreeIntegrityDuplicateTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	FCortexUMGTreeIntegrityFixture F(*this);

	const FCortexCommandResult IntoFull = F.Run(TEXT("duplicate_widget"),
		{{TEXT("widget_name"), TEXT("BtnText")}, {TEXT("new_name"), TEXT("BtnTextCopy")}});
	TestFalse(TEXT("duplicating into a full Button is refused"), IntoFull.bSuccess);
	TestEqual(TEXT("full parent error code"), IntoFull.ErrorCode, CortexErrorCodes::InvalidParent);
	TestNull(TEXT("no BtnTextCopy object is left"), F.FindTreeObject(TEXT("BtnTextCopy")));

	const FCortexCommandResult RootCopy = F.Run(TEXT("duplicate_widget"),
		{{TEXT("widget_name"), TEXT("Root")}, {TEXT("new_name"), TEXT("RootCopy")}});
	TestFalse(TEXT("duplicating the parentless root is refused"), RootCopy.bSuccess);
	TestEqual(TEXT("root duplicate error code"), RootCopy.ErrorCode, CortexErrorCodes::InvalidParent);
	TestNull(TEXT("no RootCopy object is left"), F.FindTreeObject(TEXT("RootCopy")));

	// An empty prefix maps the child copies onto the existing child names.
	const FCortexCommandResult Colliding = F.Run(TEXT("duplicate_widget"),
		{{TEXT("widget_name"), TEXT("Box")}, {TEXT("new_name"), TEXT("BoxCopy")}, {TEXT("name_prefix"), TEXT("")}});
	TestFalse(TEXT("a duplicate whose child names collide is refused"), Colliding.bSuccess);
	TestEqual(TEXT("collision error code"), Colliding.ErrorCode, CortexErrorCodes::WidgetNameExists);
	TestNull(TEXT("no BoxCopy object is left"), F.FindTreeObject(TEXT("BoxCopy")));
	TestEqual(TEXT("Label object is untouched"), F.FindTreeObject(TEXT("Label")), (UObject*)F.Label);
	TestTrue(TEXT("Label is still a TextBlock"), IsValid(F.Label) && F.Label->IsA<UTextBlock>());

	F.ExpectUnchanged(*this, TEXT("after duplicate refusals"));

	const FCortexCommandResult Copied = F.Run(TEXT("duplicate_widget"),
		{{TEXT("widget_name"), TEXT("Box")}, {TEXT("new_name"), TEXT("BoxCopy")}});
	TestTrue(TEXT("a valid duplicate succeeds"), Copied.bSuccess);
	TestEqual(TEXT("the copy is appended to Root"), F.Root->GetChildrenCount(), 3);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexUMGTreeIntegrityRemoveTest,
	"Cortex.UMG.TreeIntegrity.RemoveReleasesNames",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexUMGTreeIntegrityRemoveTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	FCortexUMGTreeIntegrityFixture F(*this);

	const FCortexCommandResult Removed = F.Run(TEXT("remove_widget"), {{TEXT("widget_name"), TEXT("Box")}});
	TestTrue(TEXT("remove succeeds"), Removed.bSuccess);
	TestNull(TEXT("Box name is released from the tree outer"), F.FindTreeObject(TEXT("Box")));
	TestNull(TEXT("Label name is released from the tree outer"), F.FindTreeObject(TEXT("Label")));
	TestNull(TEXT("InnerBox name is released from the tree outer"), F.FindTreeObject(TEXT("InnerBox")));
	TestEqual(TEXT("removed Box moves to the transient package"), F.Box->GetOutermost(), GetTransientPackage());
	TestEqual(TEXT("removed Label moves to the transient package"), F.Label->GetOutermost(), GetTransientPackage());
	TestFalse(TEXT("Box variable identity is released"), F.Blueprint->WidgetVariableNameToGuidMap.Contains(TEXT("Box")));
	TestFalse(TEXT("Label variable identity is released"), F.Blueprint->WidgetVariableNameToGuidMap.Contains(TEXT("Label")));

	// Constructing a different class on a still-occupied name is fatal, so stop rather than crash.
	if (F.FindTreeObject(TEXT("Label")) != nullptr)
	{
		return false;
	}
	const FCortexCommandResult Reused = F.Run(TEXT("add_widget"),
		{{TEXT("parent_name"), TEXT("Root")}, {TEXT("widget_class"), TEXT("RichTextBlock")}, {TEXT("name"), TEXT("Label")}});
	TestTrue(TEXT("a removed name can be reused with another class"), Reused.bSuccess);
	UObject* NewLabel = F.FindTreeObject(TEXT("Label"));
	TestTrue(TEXT("the reused name holds the new RichTextBlock"), NewLabel != nullptr && NewLabel->IsA<URichTextBlock>());

	// A detached widget persisted by an older remove still owns its name in the tree outer;
	// constructing another class on that name is fatal, so it must be refused.
	UTextBlock* Ghost = NewObject<UTextBlock>(F.Blueprint->WidgetTree, TEXT("Ghost"), RF_Transactional);
	const FCortexCommandResult OntoGhost = F.Run(TEXT("add_widget"),
		{{TEXT("parent_name"), TEXT("Root")}, {TEXT("widget_class"), TEXT("RichTextBlock")}, {TEXT("name"), TEXT("Ghost")}});
	TestFalse(TEXT("adding onto a detached object's name is refused"), OntoGhost.bSuccess);
	TestEqual(TEXT("detached name error code"), OntoGhost.ErrorCode, CortexErrorCodes::WidgetNameExists);
	TestNull(TEXT("the detached object stays detached"), Ghost->GetParent());
	TestEqual(TEXT("Root did not gain the ghost"), F.Root->GetChildIndex(Ghost), INDEX_NONE);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexUMGTreeIntegrityUndoTest,
	"Cortex.UMG.TreeIntegrity.UndoRestores",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexUMGTreeIntegrityUndoTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	if (!CortexUMGTreeIntegrityCanUndo())
	{
		AddInfo(TEXT("Editor undo system not available - skipping"));
		return true;
	}
	FCortexUMGTreeIntegrityFixture F(*this);
	GEditor->ResetTransaction(FText::FromString(TEXT("Cortex UMG Tree Integrity Undo Setup")));

	TestTrue(TEXT("remove succeeds"), F.Run(TEXT("remove_widget"), {{TEXT("widget_name"), TEXT("Box")}}).bSuccess);
	TestTrue(TEXT("undo remove"), GEditor->UndoTransaction());
	F.ExpectUnchanged(*this, TEXT("undo remove"));
	TestEqual(TEXT("undo remove restores Box's outer"), F.Box->GetOuter(), (UObject*)F.Blueprint->WidgetTree);
	TestEqual(TEXT("undo remove restores Box's name"), F.Box->GetFName(), FName(TEXT("Box")));
	TestEqual(TEXT("undo remove restores Label's outer"), F.Label->GetOuter(), (UObject*)F.Blueprint->WidgetTree);

	TestTrue(TEXT("reparent succeeds"), F.Run(TEXT("reparent"),
		{{TEXT("widget_name"), TEXT("Label")}, {TEXT("new_parent"), TEXT("InnerBox")}}).bSuccess);
	TestTrue(TEXT("undo reparent"), GEditor->UndoTransaction());
	F.ExpectUnchanged(*this, TEXT("undo reparent"));

	TestTrue(TEXT("duplicate succeeds"), F.Run(TEXT("duplicate_widget"),
		{{TEXT("widget_name"), TEXT("Label")}, {TEXT("new_name"), TEXT("LabelCopy")}}).bSuccess);
	TestTrue(TEXT("undo duplicate"), GEditor->UndoTransaction());
	F.ExpectUnchanged(*this, TEXT("undo duplicate"));

	const FText OriginalText = F.Label->GetText();
	TestTrue(TEXT("set_text succeeds"), F.Run(TEXT("set_text"),
		{{TEXT("widget_name"), TEXT("Label")}, {TEXT("text"), TEXT("Changed by Cortex")}}).bSuccess);
	TestTrue(TEXT("undo set_text"), GEditor->UndoTransaction());
	TestTrue(TEXT("undo set_text restores the widget text"), F.Label->GetText().EqualTo(OriginalText));

	UVerticalBoxSlot* LabelSlot = Cast<UVerticalBoxSlot>(F.Label->Slot);
	if (!TestNotNull(TEXT("Label has a VerticalBox slot"), LabelSlot))
	{
		return false;
	}
	const FMargin OriginalPadding = LabelSlot->GetPadding();
	TestTrue(TEXT("set_padding succeeds"), F.Run(TEXT("set_padding"),
		{{TEXT("widget_name"), TEXT("Label")}}, TEXT("padding"), 17).bSuccess);
	TestEqual(TEXT("set_padding writes the slot"), LabelSlot->GetPadding(), FMargin(17.f));
	TestTrue(TEXT("undo set_padding"), GEditor->UndoTransaction());
	TestEqual(TEXT("undo set_padding restores the slot padding"), LabelSlot->GetPadding(), OriginalPadding);

	// An undone creation keeps its object (and name) for redo; recreating the same widget must work.
	TestTrue(TEXT("add Fresh succeeds"), F.Run(TEXT("add_widget"),
		{{TEXT("parent_name"), TEXT("Box")}, {TEXT("widget_class"), TEXT("TextBlock")}, {TEXT("name"), TEXT("Fresh")}}).bSuccess);
	TestTrue(TEXT("undo add"), GEditor->UndoTransaction());
	TestEqual(TEXT("undo add detaches Fresh"), F.Box->GetChildrenCount(), 2);
	const FCortexCommandResult Recreated = F.Run(TEXT("add_widget"),
		{{TEXT("parent_name"), TEXT("Box")}, {TEXT("widget_class"), TEXT("TextBlock")}, {TEXT("name"), TEXT("Fresh")}});
	TestTrue(TEXT("recreating an undone widget name succeeds"), Recreated.bSuccess);
	UWidget* Fresh = F.Blueprint->WidgetTree->FindWidget(TEXT("Fresh"));
	TestTrue(TEXT("recreated Fresh is attached to Box"), Fresh != nullptr && Fresh->GetParent() == F.Box);
	return true;
}
