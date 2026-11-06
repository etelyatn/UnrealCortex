// Regression coverage for umg.add_widget identity initialization on legacy Widget Blueprints.
//
// A legacy Designer asset (saved before the identity map existed) has an empty
// WidgetVariableNameToGuidMap. The native compiler only seeds that map while it is still empty
// (FWidgetBlueprintCompilerContext::ValidateAndFixUpVariableGuids); once a Cortex mutation makes it
// non-empty with just the new variable, every later compile repairs the still-missing pre-existing
// widgets/animations with random GUIDs and one ensure per repaired entry. The first Cortex mutation
// must therefore seed all pre-existing source widgets and animations with the compiler's own
// path-derived deterministic GUIDs before it registers anything new, and must leave a populated map
// untouched.

#include "Misc/AutomationTest.h"
#include "CortexCommandRouter.h"
#include "CortexUMGCommandHandler.h"
#include "WidgetBlueprint.h"
#include "Blueprint/WidgetTree.h"
#include "Blueprint/UserWidget.h"
#include "Components/CanvasPanel.h"
#include "Components/TextBlock.h"
#include "Animation/WidgetAnimation.h"
#include "MovieScene.h"
#include "Dom/JsonObject.h"
#include "Editor.h"
#include "Editor/Transactor.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "KismetCompiler.h"
#include "Misc/ScopeExit.h"

namespace
{
/**
 * Legacy Designer fixture: a pre-existing root, an attached child and a pre-existing animation in a
 * fresh package, with explicit control over the identity map population. Names are module-unique so
 * the unity build cannot collide with anonymous-namespace symbols in sibling test translation units.
 */
struct FCortexUMGAddWidgetLegacyIdentityFixture
{
	UPackage* Package = nullptr;
	UWidgetBlueprint* Blueprint = nullptr;
	UCanvasPanel* Root = nullptr;
	UWidgetAnimation* Animation = nullptr;

	void Create()
	{
		Package = CreatePackage(*(TEXT("/Temp/CortexUMGAddWidgetLegacy_")
			+ FGuid::NewGuid().ToString(EGuidFormats::Digits)));
		Blueprint = NewObject<UWidgetBlueprint>(
			Package, TEXT("WBP_AddWidgetLegacy"), RF_Public | RF_Standalone | RF_Transactional);
		Blueprint->ParentClass = UUserWidget::StaticClass();
		Blueprint->WidgetTree = NewObject<UWidgetTree>(
			Blueprint, UWidgetTree::StaticClass(), TEXT("WidgetTree"));

		Root = Blueprint->WidgetTree->ConstructWidget<UCanvasPanel>(
			UCanvasPanel::StaticClass(), TEXT("LegacyRoot"));
		Blueprint->WidgetTree->RootWidget = Root;
		UTextBlock* Child = Blueprint->WidgetTree->ConstructWidget<UTextBlock>(
			UTextBlock::StaticClass(), TEXT("LegacyChild"));
		Root->AddChild(Child);

		Animation = NewObject<UWidgetAnimation>(Blueprint, TEXT("LegacyFade"), RF_Transactional);
		UMovieScene* MovieScene = NewObject<UMovieScene>(Animation, TEXT("LegacyFade"));
		const FFrameRate TickResolution = MovieScene->GetTickResolution();
		MovieScene->SetPlaybackRange(TRange<FFrameNumber>(
			FFrameNumber(0), TickResolution.AsFrameNumber(1.0)));
		Animation->MovieScene = MovieScene;
		Blueprint->Animations.Add(Animation);
	}

	void Destroy()
	{
		if (Blueprint)
		{
			Blueprint->MarkAsGarbage();
		}
		if (Package)
		{
			Package->MarkAsGarbage();
		}
	}

	UWidget* ExistingChild() const
	{
		return Blueprint->WidgetTree->FindWidget(TEXT("LegacyChild"));
	}
};

/** Drives the public umg.add_widget handler so the regression covers the real dispatch path. */
FCortexCommandResult CortexUMGAddWidgetLegacyIdentityAdd(
	FCortexCommandRouter& Router, UWidgetBlueprint* Blueprint, const TCHAR* Name, const TCHAR* Parent)
{
	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("asset_path"), Blueprint->GetPathName());
	Params->SetStringField(TEXT("widget_class"), TEXT("TextBlock"));
	Params->SetStringField(TEXT("name"), Name);
	if (Parent != nullptr && *Parent != TEXT('\0'))
	{
		Params->SetStringField(TEXT("parent_name"), Parent);
	}
	return Router.Execute(TEXT("umg.add_widget"), Params);
}

/** True while an identity-map key still names a live widget of the tree or one of its animations. */
bool CortexUMGAddWidgetLegacyIdentityResolves(const UWidgetBlueprint* Blueprint, const FName& Key)
{
	for (const UWidgetAnimation* Existing : Blueprint->Animations)
	{
		if (Existing && Existing->GetFName() == Key)
		{
			return true;
		}
	}
	return Blueprint->WidgetTree->FindWidget(Key) != nullptr;
}

/**
 * Asserts that the identity map never names a widget/animation that is not part of the asset, so an
 * undo/redo that restores identities is also checked against the hierarchy it belongs to.
 */
void CortexUMGAddWidgetLegacyIdentityAssertInventory(
	FAutomationTestBase& Test, const UWidgetBlueprint* Blueprint, const TCHAR* Stage)
{
	int32 Resolved = 0;
	for (const auto& Entry : Blueprint->WidgetVariableNameToGuidMap)
	{
		const bool bResolves = CortexUMGAddWidgetLegacyIdentityResolves(Blueprint, Entry.Key);
		Test.TestTrue(FString::Printf(
			TEXT("%s: identity '%s' resolves to a live widget or animation"),
			Stage, *Entry.Key.ToString()), bResolves);
		Resolved += bResolves ? 1 : 0;
	}
	Test.TestEqual(FString::Printf(TEXT("%s: every identity-map entry resolves"), Stage),
		Resolved, Blueprint->WidgetVariableNameToGuidMap.Num());
}

/**
 * Asserts the reverse direction of the identity/hierarchy contract: every widget still attached to the
 * tree and every animation has an identity. A widget left attached after an undo with no identity is
 * exactly the map/hierarchy inconsistency this guards. Detached source widgets (objects merely outered
 * to the tree, which is what ForEachSourceWidget and the compiler enumerate) are deliberately excluded:
 * undo restores the hierarchy, it does not delete the object created inside the transaction.
 */
void CortexUMGAddWidgetLegacyIdentityAssertHierarchyCoverage(
	FAutomationTestBase& Test, const UWidgetBlueprint* Blueprint, const TCHAR* Stage)
{
	TArray<UWidget*> AttachedWidgets;
	if (Blueprint->WidgetTree)
	{
		Blueprint->WidgetTree->GetAllWidgets(AttachedWidgets);
	}
	for (UWidget* Widget : AttachedWidgets)
	{
		if (!Widget)
		{
			continue;
		}
		Test.TestTrue(FString::Printf(TEXT("%s: attached widget '%s' has an identity"),
			Stage, *Widget->GetName()), Blueprint->WidgetVariableNameToGuidMap.Contains(Widget->GetFName()));
	}
	for (const UWidgetAnimation* Existing : Blueprint->Animations)
	{
		if (Existing)
		{
			Test.TestTrue(FString::Printf(TEXT("%s: animation '%s' has an identity"),
				Stage, *Existing->GetName()),
				Blueprint->WidgetVariableNameToGuidMap.Contains(Existing->GetFName()));
		}
	}
}
} // namespace

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexUMGAddWidgetLegacyIdentityTest,
	"Cortex.UMG.AddWidgetLegacyIdentity.EmptyMap",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexUMGAddWidgetLegacyIdentityTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	FCortexUMGAddWidgetLegacyIdentityFixture Fixture;
	Fixture.Create();
	ON_SCOPE_EXIT { Fixture.Destroy(); };

	UWidgetBlueprint* const WBP = Fixture.Blueprint;
	UWidget* const ExistingChild = Fixture.ExistingChild();

	// A valid historical legacy Designer owns a generated class and an empty serialized identity map:
	// it was compiled and saved by an engine that predates the map. Compile once here so the asset has
	// the GeneratedClass/CDO native undo/redo requires (the blueprint undo handler dereferences it), then
	// clear the map the compiler just seeded (root, child, animation) so the asset models that state.
	FCompilerResultsLog BaselineCompile;
	FKismetEditorUtilities::CompileBlueprint(WBP, EBlueprintCompileOptions::None, &BaselineCompile);
	TestEqual(TEXT("Legacy baseline compiles without errors"), BaselineCompile.NumErrors, 0);
	TestEqual(TEXT("Legacy baseline compiles without warnings"), BaselineCompile.NumWarnings, 0);
	TestTrue(TEXT("Legacy baseline compile seeds the compiler's deterministic identities"),
		WBP->WidgetVariableNameToGuidMap.Num() > 0);
	WBP->WidgetVariableNameToGuidMap.Empty();

	TestTrue(TEXT("Legacy Designer starts with the compiler's empty identity map"),
		WBP->WidgetVariableNameToGuidMap.IsEmpty());

	FCortexCommandRouter Router;
	Router.RegisterDomain(TEXT("umg"), TEXT("Cortex UMG"), TEXT("1.0.1"),
		MakeShared<FCortexUMGCommandHandler>());

	const FCortexCommandResult AddResult =
		CortexUMGAddWidgetLegacyIdentityAdd(Router, WBP, TEXT("AddedLabel"), TEXT("LegacyRoot"));
	if (!TestTrue(TEXT("Adding the first authored widget to a legacy Designer succeeds"),
		AddResult.bSuccess))
	{
		return false;
	}

	UWidget* const AddedLabel = WBP->WidgetTree->FindWidget(TEXT("AddedLabel"));
	if (!TestNotNull(TEXT("Added widget is attached under the legacy root"), AddedLabel))
	{
		return false;
	}
	TestEqual(TEXT("Legacy root holds the pre-existing child and the addition"),
		Fixture.Root->GetChildrenCount(), 2);
	TestEqual(TEXT("Pre-existing child keeps its slot index"),
		Fixture.Root->GetChildAt(0), ExistingChild);
	TestEqual(TEXT("Added widget takes the next slot index"),
		Fixture.Root->GetChildAt(1), AddedLabel);

	// Pre-existing identities are asserted before any compile, and a missing identity ends the test
	// here: otherwise the compiler would repair the map with random GUIDs and one ensure per repaired
	// entry, which would bury this clean assertion under unrelated ensure noise.
	TestEqual(TEXT("Legacy root carries the compiler's deterministic identity"),
		WBP->WidgetVariableNameToGuidMap.FindRef(TEXT("LegacyRoot")),
		FGuid::NewDeterministicGuid(Fixture.Root->GetPathName()));
	TestEqual(TEXT("Legacy child carries the compiler's deterministic identity"),
		WBP->WidgetVariableNameToGuidMap.FindRef(TEXT("LegacyChild")),
		FGuid::NewDeterministicGuid(ExistingChild->GetPathName()));
	TestEqual(TEXT("Pre-existing animation carries the compiler's deterministic identity"),
		WBP->WidgetVariableNameToGuidMap.FindRef(TEXT("LegacyFade")),
		FGuid::NewDeterministicGuid(Fixture.Animation->GetPathName()));
	TestTrue(TEXT("Added widget carries an identity"),
		WBP->WidgetVariableNameToGuidMap.FindRef(TEXT("AddedLabel")).IsValid());
	TestEqual(TEXT("Identity map holds exactly the legacy widgets, the animation and the addition"),
		WBP->WidgetVariableNameToGuidMap.Num(), 4);
	CortexUMGAddWidgetLegacyIdentityAssertInventory(*this, WBP, TEXT("before compile"));
	CortexUMGAddWidgetLegacyIdentityAssertHierarchyCoverage(*this, WBP, TEXT("before compile"));

	if (!WBP->WidgetVariableNameToGuidMap.Contains(TEXT("LegacyRoot"))
		|| !WBP->WidgetVariableNameToGuidMap.Contains(TEXT("LegacyChild"))
		|| !WBP->WidgetVariableNameToGuidMap.Contains(TEXT("LegacyFade")))
	{
		// RED stops here: without the deterministic identities the compiler would repair the map with
		// random GUIDs and one ensure per repaired entry, burying this clean assertion under noise.
		return false;
	}

	// Undo/redo must restore the identity map and the hierarchy together. The round trip runs before
	// compilation so the transaction under test is unambiguously the addition, and membership is
	// asserted from the panel side: FindWidget walks the live tree and the root's slot list is the
	// authority for containment. The added widget object itself survives an undo (it was created inside
	// the transaction and stays outered to the tree), so only its membership and identity must go away.
	if (GEditor != nullptr && GEditor->Trans != nullptr && GEditor->CanTransact())
	{
		const bool bUndo = GEditor->UndoTransaction();
		TestTrue(TEXT("Undoing the legacy addition succeeds"), bUndo);
		TestFalse(TEXT("Undo drops the added widget identity"),
			WBP->WidgetVariableNameToGuidMap.Contains(TEXT("AddedLabel")));
		TestNull(TEXT("Undo removes the added widget from the source hierarchy"),
			WBP->WidgetTree->FindWidget(TEXT("AddedLabel")));
		TestEqual(TEXT("Undo removes the added widget's child slot"),
			Fixture.Root->GetChildIndex(AddedLabel), INDEX_NONE);
		TestEqual(TEXT("Undo restores the legacy root child count"),
			Fixture.Root->GetChildrenCount(), 1);
		TestEqual(TEXT("Undo restores the pre-existing child slot"),
			Fixture.Root->GetChildAt(0), ExistingChild);
		TestNotNull(TEXT("Undo keeps the pre-existing root widget"),
			WBP->WidgetTree->FindWidget(TEXT("LegacyRoot")));
		TestEqual(TEXT("Undo keeps the pre-existing animation count"), WBP->Animations.Num(), 1);
		UWidgetAnimation* UndoneAnimation = nullptr;
		if (WBP->Animations.Num() > 0)
		{
			UndoneAnimation = WBP->Animations[0];
		}
		TestEqual(TEXT("Undo keeps the pre-existing animation object"),
			UndoneAnimation, Fixture.Animation);
		CortexUMGAddWidgetLegacyIdentityAssertInventory(*this, WBP, TEXT("after undo"));

		// Compile while the addition is undone. The retired widget must not be visible as a source
		// widget, so the compiler neither repairs nor invents an identity for it, and the identities it
		// re-derives for the pre-existing root/child/animation are the same deterministic values.
		FCompilerResultsLog UndoCompile;
		FKismetEditorUtilities::CompileBlueprint(WBP, EBlueprintCompileOptions::None, &UndoCompile);
		TestEqual(TEXT("Compile after legacy undo reports no errors"), UndoCompile.NumErrors, 0);
		TestEqual(TEXT("Compile after legacy undo reports no warnings"), UndoCompile.NumWarnings, 0);
		TestEqual(TEXT("Compile after legacy undo keeps the legacy root identity"),
			WBP->WidgetVariableNameToGuidMap.FindRef(TEXT("LegacyRoot")),
			FGuid::NewDeterministicGuid(Fixture.Root->GetPathName()));
		TestEqual(TEXT("Compile after legacy undo keeps the legacy child identity"),
			WBP->WidgetVariableNameToGuidMap.FindRef(TEXT("LegacyChild")),
			FGuid::NewDeterministicGuid(ExistingChild->GetPathName()));
		TestEqual(TEXT("Compile after legacy undo keeps the pre-existing animation identity"),
			WBP->WidgetVariableNameToGuidMap.FindRef(TEXT("LegacyFade")),
			FGuid::NewDeterministicGuid(Fixture.Animation->GetPathName()));
		TestEqual(TEXT("Compile after legacy undo invents no identity for the retired widget"),
			WBP->WidgetVariableNameToGuidMap.Num(), 3);
		TestFalse(TEXT("Compile after legacy undo keeps the retired widget unnamed"),
			WBP->WidgetVariableNameToGuidMap.Contains(TEXT("AddedLabel")));
		TestNull(TEXT("Compile after legacy undo leaves the widget out of the hierarchy"),
			WBP->WidgetTree->FindWidget(TEXT("AddedLabel")));
		TestEqual(TEXT("Compile after legacy undo keeps the root child count"),
			Fixture.Root->GetChildrenCount(), 1);
		CortexUMGAddWidgetLegacyIdentityAssertInventory(*this, WBP, TEXT("after legacy undo compile"));

		const bool bRedo = GEditor->RedoTransaction();
		TestTrue(TEXT("Redoing the legacy addition succeeds"), bRedo);
		TestEqual(TEXT("Redo restores the legacy root identity"),
			WBP->WidgetVariableNameToGuidMap.FindRef(TEXT("LegacyRoot")),
			FGuid::NewDeterministicGuid(Fixture.Root->GetPathName()));
		TestEqual(TEXT("Redo restores the legacy child identity"),
			WBP->WidgetVariableNameToGuidMap.FindRef(TEXT("LegacyChild")),
			FGuid::NewDeterministicGuid(ExistingChild->GetPathName()));
		TestEqual(TEXT("Redo restores the pre-existing animation identity"),
			WBP->WidgetVariableNameToGuidMap.FindRef(TEXT("LegacyFade")),
			FGuid::NewDeterministicGuid(Fixture.Animation->GetPathName()));
		TestNotNull(TEXT("Redo reattaches the added widget"),
			WBP->WidgetTree->FindWidget(TEXT("AddedLabel")));
		TestEqual(TEXT("Redo restores the legacy root child count"),
			Fixture.Root->GetChildrenCount(), 2);
		TestEqual(TEXT("Redo keeps the pre-existing child slot"),
			Fixture.Root->GetChildAt(0), ExistingChild);
		CortexUMGAddWidgetLegacyIdentityAssertInventory(*this, WBP, TEXT("after redo"));
		CortexUMGAddWidgetLegacyIdentityAssertHierarchyCoverage(*this, WBP, TEXT("after redo"));
	}
	else
	{
		AddInfo(TEXT("Editor undo system not available - skipping legacy identity undo/redo assertions"));
	}

	FCompilerResultsLog Compile;
	FKismetEditorUtilities::CompileBlueprint(WBP, EBlueprintCompileOptions::None, &Compile);
	TestEqual(TEXT("Legacy Designer compiles without errors"), Compile.NumErrors, 0);
	TestEqual(TEXT("Legacy Designer compiles without warnings"), Compile.NumWarnings, 0);
	// A compiler-repaired identity is a fresh random GUID, so unchanged deterministic values prove the
	// compiler never had to fix the map - i.e. it did not fire its per-entry repair ensure.
	TestEqual(TEXT("Compile preserves the legacy root identity"),
		WBP->WidgetVariableNameToGuidMap.FindRef(TEXT("LegacyRoot")),
		FGuid::NewDeterministicGuid(Fixture.Root->GetPathName()));
	TestEqual(TEXT("Compile preserves the legacy child identity"),
		WBP->WidgetVariableNameToGuidMap.FindRef(TEXT("LegacyChild")),
		FGuid::NewDeterministicGuid(ExistingChild->GetPathName()));
	TestEqual(TEXT("Compile preserves the pre-existing animation identity"),
		WBP->WidgetVariableNameToGuidMap.FindRef(TEXT("LegacyFade")),
		FGuid::NewDeterministicGuid(Fixture.Animation->GetPathName()));
	TestEqual(TEXT("Compile leaves the identity map size unchanged"),
		WBP->WidgetVariableNameToGuidMap.Num(), 4);
	CortexUMGAddWidgetLegacyIdentityAssertInventory(*this, WBP, TEXT("after compile"));
	CortexUMGAddWidgetLegacyIdentityAssertHierarchyCoverage(*this, WBP, TEXT("after compile"));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexUMGAddWidgetRejectedParentKeepsEmptyMapTest,
	"Cortex.UMG.AddWidgetLegacyIdentity.RejectedParentKeepsEmptyMap",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexUMGAddWidgetRejectedParentKeepsEmptyMapTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	FCortexUMGAddWidgetLegacyIdentityFixture Fixture;
	Fixture.Create();
	ON_SCOPE_EXIT { Fixture.Destroy(); };

	UWidgetBlueprint* const WBP = Fixture.Blueprint;
	UWidget* const ExistingChild = Fixture.ExistingChild();

	FCortexCommandRouter Router;
	Router.RegisterDomain(TEXT("umg"), TEXT("Cortex UMG"), TEXT("1.0.1"),
		MakeShared<FCortexUMGCommandHandler>());

	// A refused add must not seed the identity map. The refusal path constructs a widget before it
	// resolves the parent (pre-existing behaviour), so this case asserts only what the correction owns:
	// no newly populated identity map and an unchanged attached hierarchy. Its own fixture keeps the
	// successful-addition cases free of that detached object.
	const FCortexCommandResult RejectedResult =
		CortexUMGAddWidgetLegacyIdentityAdd(Router, WBP, TEXT("RejectedLabel"), TEXT("MissingParent"));
	TestFalse(TEXT("Add with a missing parent is refused"), RejectedResult.bSuccess);
	TestEqual(TEXT("Missing-parent refusal reports WIDGET_NOT_FOUND"),
		RejectedResult.ErrorCode, CortexErrorCodes::WidgetNotFound);
	TestTrue(TEXT("Refused add leaves the legacy identity map empty"),
		WBP->WidgetVariableNameToGuidMap.IsEmpty());
	TestNotNull(TEXT("Refused add keeps the pre-existing root"),
		WBP->WidgetTree->FindWidget(TEXT("LegacyRoot")));
	TestNotNull(TEXT("Refused add keeps the pre-existing child"),
		WBP->WidgetTree->FindWidget(TEXT("LegacyChild")));
	TestEqual(TEXT("Refused add keeps the legacy root child count"),
		Fixture.Root->GetChildrenCount(), 1);
	TestEqual(TEXT("Refused add keeps the pre-existing child slot"),
		Fixture.Root->GetChildAt(0), ExistingChild);
	TestEqual(TEXT("Refused add keeps the pre-existing animation count"), WBP->Animations.Num(), 1);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexUMGAddWidgetPopulatedIdentityRetentionTest,
	"Cortex.UMG.AddWidgetLegacyIdentity.PopulatedRetention",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexUMGAddWidgetPopulatedIdentityRetentionTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	FCortexUMGAddWidgetLegacyIdentityFixture Fixture;
	Fixture.Create();
	ON_SCOPE_EXIT { Fixture.Destroy(); };

	UWidgetBlueprint* const WBP = Fixture.Blueprint;
	const FGuid RootGuid = FGuid::NewGuid();
	const FGuid ChildGuid = FGuid::NewGuid();
	const FGuid AnimationGuid = FGuid::NewGuid();
	WBP->WidgetVariableNameToGuidMap.Add(TEXT("LegacyRoot"), RootGuid);
	WBP->WidgetVariableNameToGuidMap.Add(TEXT("LegacyChild"), ChildGuid);
	WBP->WidgetVariableNameToGuidMap.Add(TEXT("LegacyFade"), AnimationGuid);

	FCortexCommandRouter Router;
	Router.RegisterDomain(TEXT("umg"), TEXT("Cortex UMG"), TEXT("1.0.1"),
		MakeShared<FCortexUMGCommandHandler>());

	const FCortexCommandResult AddResult =
		CortexUMGAddWidgetLegacyIdentityAdd(Router, WBP, TEXT("AddedLabel"), TEXT("LegacyRoot"));
	if (!TestTrue(TEXT("Adding a widget to a populated Designer succeeds"), AddResult.bSuccess))
	{
		return false;
	}

	TestEqual(TEXT("Populated root identity is retained"),
		WBP->WidgetVariableNameToGuidMap.FindRef(TEXT("LegacyRoot")), RootGuid);
	TestEqual(TEXT("Populated child identity is retained"),
		WBP->WidgetVariableNameToGuidMap.FindRef(TEXT("LegacyChild")), ChildGuid);
	TestEqual(TEXT("Populated animation identity is retained"),
		WBP->WidgetVariableNameToGuidMap.FindRef(TEXT("LegacyFade")), AnimationGuid);
	TestTrue(TEXT("Added widget carries an identity"),
		WBP->WidgetVariableNameToGuidMap.FindRef(TEXT("AddedLabel")).IsValid());
	TestEqual(TEXT("Populated identity map gains exactly the new identity"),
		WBP->WidgetVariableNameToGuidMap.Num(), 4);
	CortexUMGAddWidgetLegacyIdentityAssertInventory(*this, WBP, TEXT("before compile"));
	CortexUMGAddWidgetLegacyIdentityAssertHierarchyCoverage(*this, WBP, TEXT("before compile"));

	FCompilerResultsLog Compile;
	FKismetEditorUtilities::CompileBlueprint(WBP, EBlueprintCompileOptions::None, &Compile);
	TestEqual(TEXT("Populated Designer compiles without errors"), Compile.NumErrors, 0);
	TestEqual(TEXT("Populated Designer compiles without warnings"), Compile.NumWarnings, 0);
	TestEqual(TEXT("Compile retains the populated root identity"),
		WBP->WidgetVariableNameToGuidMap.FindRef(TEXT("LegacyRoot")), RootGuid);
	TestEqual(TEXT("Compile retains the populated child identity"),
		WBP->WidgetVariableNameToGuidMap.FindRef(TEXT("LegacyChild")), ChildGuid);
	TestEqual(TEXT("Compile retains the populated animation identity"),
		WBP->WidgetVariableNameToGuidMap.FindRef(TEXT("LegacyFade")), AnimationGuid);
	TestEqual(TEXT("Compile leaves the populated identity map size unchanged"),
		WBP->WidgetVariableNameToGuidMap.Num(), 4);
	CortexUMGAddWidgetLegacyIdentityAssertInventory(*this, WBP, TEXT("after compile"));
	CortexUMGAddWidgetLegacyIdentityAssertHierarchyCoverage(*this, WBP, TEXT("after compile"));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexUMGAddWidgetPopulatedUndoRedoTest,
	"Cortex.UMG.AddWidgetLegacyIdentity.PopulatedUndoRedo",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexUMGAddWidgetPopulatedUndoRedoTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	if (GEditor == nullptr || GEditor->Trans == nullptr || !GEditor->CanTransact())
	{
		AddInfo(TEXT("Editor undo system not available - skipping"));
		return true;
	}

	FCortexUMGAddWidgetLegacyIdentityFixture Fixture;
	Fixture.Create();
	ON_SCOPE_EXIT { Fixture.Destroy(); };

	UWidgetBlueprint* const WBP = Fixture.Blueprint;
	UWidget* const ExistingChild = Fixture.ExistingChild();
	const FGuid RootGuid = FGuid::NewGuid();
	const FGuid ChildGuid = FGuid::NewGuid();
	const FGuid AnimationGuid = FGuid::NewGuid();
	WBP->WidgetVariableNameToGuidMap.Add(TEXT("LegacyRoot"), RootGuid);
	WBP->WidgetVariableNameToGuidMap.Add(TEXT("LegacyChild"), ChildGuid);
	WBP->WidgetVariableNameToGuidMap.Add(TEXT("LegacyFade"), AnimationGuid);

	FCortexCommandRouter Router;
	Router.RegisterDomain(TEXT("umg"), TEXT("Cortex UMG"), TEXT("1.0.1"),
		MakeShared<FCortexUMGCommandHandler>());

	// Compile before the mutation so the asset owns a generated class, and so the addition is
	// unambiguously the transaction that undo/redo will act on.
	FCompilerResultsLog FixtureCompile;
	FKismetEditorUtilities::CompileBlueprint(WBP, EBlueprintCompileOptions::None, &FixtureCompile);
	if (!TestEqual(TEXT("Populated Designer compiles before the addition"),
		FixtureCompile.NumErrors, 0))
	{
		return false;
	}

	const FCortexCommandResult AddResult =
		CortexUMGAddWidgetLegacyIdentityAdd(Router, WBP, TEXT("AddedLabel"), TEXT("LegacyRoot"));
	if (!TestTrue(TEXT("Adding a widget to a populated Designer succeeds"), AddResult.bSuccess))
	{
		return false;
	}

	const FGuid AddedGuid = WBP->WidgetVariableNameToGuidMap.FindRef(TEXT("AddedLabel"));
	TestTrue(TEXT("Added widget carries an identity"), AddedGuid.IsValid());
	TestEqual(TEXT("Addition retains the populated root identity"),
		WBP->WidgetVariableNameToGuidMap.FindRef(TEXT("LegacyRoot")), RootGuid);
	TestEqual(TEXT("Addition retains the populated child identity"),
		WBP->WidgetVariableNameToGuidMap.FindRef(TEXT("LegacyChild")), ChildGuid);
	TestEqual(TEXT("Addition retains the populated animation identity"),
		WBP->WidgetVariableNameToGuidMap.FindRef(TEXT("LegacyFade")), AnimationGuid);
	TestEqual(TEXT("Addition extends the populated identity map by exactly one entry"),
		WBP->WidgetVariableNameToGuidMap.Num(), 4);
	UWidget* const AddedLabel = WBP->WidgetTree->FindWidget(TEXT("AddedLabel"));
	if (!TestNotNull(TEXT("Added widget is attached before undo"), AddedLabel))
	{
		return false;
	}
	TestEqual(TEXT("Legacy root holds both children before undo"),
		Fixture.Root->GetChildrenCount(), 2);
	CortexUMGAddWidgetLegacyIdentityAssertHierarchyCoverage(*this, WBP, TEXT("before undo"));

	// Undo must restore the identity population that existed before the addition without disturbing
	// the pre-existing hierarchy; redo must restore the addition with the same identity.
	const bool bUndo = GEditor->UndoTransaction();
	TestTrue(TEXT("Undoing the legacy addition succeeds"), bUndo);
	TestFalse(TEXT("Undo drops the added widget identity"),
		WBP->WidgetVariableNameToGuidMap.Contains(TEXT("AddedLabel")));
	TestEqual(TEXT("Undo restores the populated root identity"),
		WBP->WidgetVariableNameToGuidMap.FindRef(TEXT("LegacyRoot")), RootGuid);
	TestEqual(TEXT("Undo restores the populated child identity"),
		WBP->WidgetVariableNameToGuidMap.FindRef(TEXT("LegacyChild")), ChildGuid);
	TestEqual(TEXT("Undo restores the populated animation identity"),
		WBP->WidgetVariableNameToGuidMap.FindRef(TEXT("LegacyFade")), AnimationGuid);
	TestEqual(TEXT("Undo restores the populated identity population"),
		WBP->WidgetVariableNameToGuidMap.Num(), 3);
	// The hierarchy must come back with the identities. Membership is asserted from the panel side,
	// because FindWidget walks the live tree and the root's slot list is the authority for containment:
	// a widget left attached with no identity is precisely the inconsistency this case must catch.
	TestNull(TEXT("Undo removes the added widget from the source hierarchy"),
		WBP->WidgetTree->FindWidget(TEXT("AddedLabel")));
	TestEqual(TEXT("Undo removes the added widget's child slot"),
		Fixture.Root->GetChildIndex(AddedLabel), INDEX_NONE);
	TestEqual(TEXT("Undo restores the legacy root child count"),
		Fixture.Root->GetChildrenCount(), 1);
	TestEqual(TEXT("Undo restores the pre-existing child slot"),
		Fixture.Root->GetChildAt(0), ExistingChild);
	TestNotNull(TEXT("Undo keeps the pre-existing root in the hierarchy"),
		WBP->WidgetTree->FindWidget(TEXT("LegacyRoot")));
	TestNotNull(TEXT("Undo keeps the pre-existing child in the hierarchy"),
		WBP->WidgetTree->FindWidget(TEXT("LegacyChild")));
	TestEqual(TEXT("Undo keeps the pre-existing animation count"), WBP->Animations.Num(), 1);
	UWidgetAnimation* PreExistingAnimation = nullptr;
	if (WBP->Animations.Num() > 0)
	{
		PreExistingAnimation = WBP->Animations[0];
	}
	TestEqual(TEXT("Undo keeps the pre-existing animation object"),
		PreExistingAnimation, Fixture.Animation);
	CortexUMGAddWidgetLegacyIdentityAssertInventory(*this, WBP, TEXT("after undo"));
	CortexUMGAddWidgetLegacyIdentityAssertHierarchyCoverage(*this, WBP, TEXT("after undo"));

	// Compile while the addition is undone, before the redo restores it. The identity population must
	// be unchanged (same values, no invented entry and no orphan identity for the retired widget) and
	// the compile must stay clean: a missing identity here is what makes the compiler repair the map
	// (random GUID plus one ensure) instead of leaving the legacy/undone state alone.
	FCompilerResultsLog UndoCompile;
	FKismetEditorUtilities::CompileBlueprint(WBP, EBlueprintCompileOptions::None, &UndoCompile);
	TestEqual(TEXT("Compile after populated undo reports no errors"), UndoCompile.NumErrors, 0);
	TestEqual(TEXT("Compile after populated undo reports no warnings"), UndoCompile.NumWarnings, 0);
	TestEqual(TEXT("Compile after populated undo keeps the root identity"),
		WBP->WidgetVariableNameToGuidMap.FindRef(TEXT("LegacyRoot")), RootGuid);
	TestEqual(TEXT("Compile after populated undo keeps the child identity"),
		WBP->WidgetVariableNameToGuidMap.FindRef(TEXT("LegacyChild")), ChildGuid);
	TestEqual(TEXT("Compile after populated undo keeps the animation identity"),
		WBP->WidgetVariableNameToGuidMap.FindRef(TEXT("LegacyFade")), AnimationGuid);
	TestEqual(TEXT("Compile after populated undo invents no identity for the retired widget"),
		WBP->WidgetVariableNameToGuidMap.Num(), 3);
	TestFalse(TEXT("Compile after populated undo keeps the retired widget unnamed"),
		WBP->WidgetVariableNameToGuidMap.Contains(TEXT("AddedLabel")));
	TestNull(TEXT("Compile after populated undo leaves the widget out of the hierarchy"),
		WBP->WidgetTree->FindWidget(TEXT("AddedLabel")));
	TestEqual(TEXT("Compile after populated undo keeps the root child count"),
		Fixture.Root->GetChildrenCount(), 1);
	CortexUMGAddWidgetLegacyIdentityAssertInventory(*this, WBP, TEXT("after populated undo compile"));

	const bool bRedo = GEditor->RedoTransaction();
	TestTrue(TEXT("Redoing the legacy addition succeeds"), bRedo);
	TestEqual(TEXT("Redo restores the added widget identity"),
		WBP->WidgetVariableNameToGuidMap.FindRef(TEXT("AddedLabel")), AddedGuid);
	TestEqual(TEXT("Redo retains the populated root identity"),
		WBP->WidgetVariableNameToGuidMap.FindRef(TEXT("LegacyRoot")), RootGuid);
	TestEqual(TEXT("Redo retains the populated child identity"),
		WBP->WidgetVariableNameToGuidMap.FindRef(TEXT("LegacyChild")), ChildGuid);
	TestEqual(TEXT("Redo retains the populated animation identity"),
		WBP->WidgetVariableNameToGuidMap.FindRef(TEXT("LegacyFade")), AnimationGuid);
	TestEqual(TEXT("Redo restores the populated identity population"),
		WBP->WidgetVariableNameToGuidMap.Num(), 4);
	UWidget* const RedoneLabel = WBP->WidgetTree->FindWidget(TEXT("AddedLabel"));
	TestNotNull(TEXT("Redo reattaches the added widget"), RedoneLabel);
	TestEqual(TEXT("Redo keeps the pre-existing child in its slot"),
		Fixture.Root->GetChildAt(0), ExistingChild);
	TestEqual(TEXT("Redo places the added widget in the next slot"),
		Fixture.Root->GetChildAt(1), RedoneLabel);
	TestEqual(TEXT("Redo keeps both children under the legacy root"),
		Fixture.Root->GetChildrenCount(), 2);
	CortexUMGAddWidgetLegacyIdentityAssertInventory(*this, WBP, TEXT("after redo"));
	CortexUMGAddWidgetLegacyIdentityAssertHierarchyCoverage(*this, WBP, TEXT("after redo"));
	return true;
}
