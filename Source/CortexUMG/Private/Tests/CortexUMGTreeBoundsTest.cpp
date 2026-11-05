// Task 10 (token efficiency) RED staging: honest native UMG tree bounds, names projection and the
// independent asset-owned property-binding inspection.
//
// Consumer-visible regressions over `umg.get_tree` describe the accepted contract:
//   * full depth by default; optional `depth` (root = 0) and `max_nodes` (>= 1);
//   * a cut branch declares `child_count` + `incomplete` and is never presented as a leaf;
//   * actual leaves omit an empty `children` array;
//   * `names_only=true` keeps hierarchy/name/child counts and drops class identity;
//   * the top level reports `returned_widgets`, `total_widgets`, `tree_complete` and `reasons`;
//   * `include_property_bindings=true` stays a complete asset-wide inspection that a tree cut does
//     not shorten, and a case-only orphan record keeps its exact raw widget name;
//   * an unsafe oversized combined tree+bindings read (whose complete compact JSON text exceeds the
//     bounded-response ceiling) is refused explicitly instead of returning a complete-looking partial
//     payload, while a combined read whose shipped text still fits that ceiling stays accepted.
// Fixtures are dedicated task-owned Widget Blueprint packages in /Temp, retired without global GC.

#include "Misc/AutomationTest.h"
#include "CortexCommandRouter.h"
#include "CortexUMGCommandHandler.h"
#include "WidgetBlueprint.h"
#include "Blueprint/WidgetTree.h"
#include "Blueprint/WidgetBlueprintGeneratedClass.h"
#include "Blueprint/UserWidget.h"
#include "Components/Widget.h"
#include "Components/PanelWidget.h"
#include "Components/CanvasPanel.h"
#include "Components/VerticalBox.h"
#include "Components/TextBlock.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "Misc/Guid.h"
#include "Misc/PackageName.h"
#include "UObject/Package.h"
#include "UObject/StrongObjectPtr.h"
#include "UObject/UObjectGlobals.h"

struct FCortexUMGTreeBoundsFixture
{
	TStrongObjectPtr<UWidgetBlueprint> Blueprint;
	FCortexCommandRouter Router;
	FString AssetPath;

	explicit FCortexUMGTreeBoundsFixture(const FString& PackageSuffix)
	{
		UPackage* Package = CreatePackage(*(TEXT("/Temp/CortexUMGTreeBounds_") + PackageSuffix));
		Blueprint.Reset(NewObject<UWidgetBlueprint>(Package, TEXT("WBP_CortexTreeBounds"),
													RF_Public | RF_Standalone | RF_Transactional));
		Blueprint->ParentClass = UUserWidget::StaticClass();
		Blueprint->WidgetTree = NewObject<UWidgetTree>(Blueprint.Get(), TEXT("WidgetTree"));
		AssetPath = Blueprint->GetPathName();
		Router.RegisterDomain(TEXT("umg"), TEXT("Cortex UMG"), TEXT("1.0.1"),
							  MakeShared<FCortexUMGCommandHandler>());
	}

	~FCortexUMGTreeBoundsFixture()
	{
		if (Blueprint.IsValid())
		{
			ResetLoaders(Blueprint->GetPackage());
			Blueprint->GetPackage()->ClearDirtyFlag();
			Blueprint->ClearFlags(RF_Standalone);
			Blueprint->MarkAsGarbage();
			Blueprint->GetPackage()->MarkAsGarbage();
		}
	}

	bool AddWidget(const FString& WidgetClass, const FString& Name, const FString& ParentName = FString())
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
	}

	TSharedPtr<FJsonObject> ReadParams(bool bBindings = false, int32 Depth = -1, int32 MaxNodes = -1,
									   bool bNamesOnly = false) const
	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("asset_path"), AssetPath);
		if (Depth >= 0)
		{
			Params->SetNumberField(TEXT("depth"), Depth);
		}
		if (MaxNodes >= 0)
		{
			Params->SetNumberField(TEXT("max_nodes"), MaxNodes);
		}
		if (bNamesOnly)
		{
			Params->SetBoolField(TEXT("names_only"), true);
		}
		if (bBindings)
		{
			Params->SetBoolField(TEXT("include_property_bindings"), true);
		}
		return Params;
	}

	// Root panel -> nested panel -> leaf, plus a sibling leaf of the nested panel.
	bool BuildMainTree()
	{
		return AddWidget(TEXT("CanvasPanel"), TEXT("RootPanel")) &&
			   AddWidget(TEXT("VerticalBox"), TEXT("NestedPanel"), TEXT("RootPanel")) &&
			   AddWidget(TEXT("TextBlock"), TEXT("DeepLeaf"), TEXT("NestedPanel")) &&
			   AddWidget(TEXT("TextBlock"), TEXT("RootSibling"), TEXT("RootPanel"));
	}

	// A flat, deliberately oversized tree whose serialized payload must exceed the module's
	// established bounded-response ceiling.
	bool BuildOversizedTree(int32 ChildCount, int32 NameFill)
	{
		if (!AddWidget(TEXT("CanvasPanel"), TEXT("OversizedRoot")))
		{
			return false;
		}
		const FString Fill = FString::ChrN(NameFill, TEXT('x'));
		for (int32 Index = 0; Index < ChildCount; ++Index)
		{
			if (!AddWidget(TEXT("TextBlock"),
						   FString::Printf(TEXT("BoundsLong_%d_%s"), Index, *Fill), TEXT("OversizedRoot")))
			{
				return false;
			}
		}
		return true;
	}

	// One resolved record plus one record that differs from the real widget name only by case.
	void SeedMainBindings()
	{
		FDelegateEditorBinding Resolved;
		Resolved.ObjectName = TEXT("RootSibling");
		Resolved.PropertyName = FName(TEXT("Text"));
		Resolved.SourceProperty = FName(TEXT("SiblingText"));
		Resolved.MemberGuid = FGuid::NewGuid();
		Resolved.Kind = EBindingKind::Property;
		Blueprint->Bindings.Add(Resolved);

		FDelegateEditorBinding CaseOnlyOrphan;
		CaseOnlyOrphan.ObjectName = TEXT("rootSibling");
		CaseOnlyOrphan.PropertyName = FName(TEXT("Text"));
		CaseOnlyOrphan.SourceProperty = FName(TEXT("OrphanText"));
		CaseOnlyOrphan.MemberGuid = FGuid::NewGuid();
		CaseOnlyOrphan.Kind = EBindingKind::Property;
		Blueprint->Bindings.Add(CaseOnlyOrphan);
	}

	void SeedOversizedBinding()
	{
		FDelegateEditorBinding Binding;
		Binding.ObjectName = TEXT("OversizedRoot");
		Binding.PropertyName = FName(TEXT("Visibility"));
		Binding.SourceProperty = FName(TEXT("OversizedValue"));
		Binding.MemberGuid = FGuid::NewGuid();
		Binding.Kind = EBindingKind::Property;
		Blueprint->Bindings.Add(Binding);
	}

	// Exact, case-sensitive designer lookup (FString operator== is case-insensitive).
	UWidget* WidgetByName(const FString& Name) const
	{
		UWidget* Found = nullptr;
		TFunction<void(UWidget*)> Visit = [&](UWidget* Widget)
		{
			if (Found || !Widget)
			{
				return;
			}
			if (Widget->GetName().Equals(Name, ESearchCase::CaseSensitive))
			{
				Found = Widget;
				return;
			}
			if (UPanelWidget* Panel = Cast<UPanelWidget>(Widget))
			{
				for (int32 Index = 0; Index < Panel->GetChildrenCount(); ++Index)
				{
					Visit(Panel->GetChildAt(Index));
				}
			}
		};
		if (Blueprint->WidgetTree)
		{
			Visit(Blueprint->WidgetTree->RootWidget);
		}
		return Found;
	}

	FString StructureSnapshot() const
	{
		TArray<FString> Parts;
		TFunction<void(UWidget*)> Visit = [&](UWidget* Widget)
		{
			if (!Widget)
			{
				return;
			}
			UPanelWidget* Panel = Cast<UPanelWidget>(Widget);
			Parts.Add(Widget->GetName() + TEXT("|") + FString::FromInt(Panel ? Panel->GetChildrenCount() : 0));
			if (Panel)
			{
				for (int32 Index = 0; Index < Panel->GetChildrenCount(); ++Index)
				{
					Visit(Panel->GetChildAt(Index));
				}
			}
		};
		if (Blueprint->WidgetTree)
		{
			Visit(Blueprint->WidgetTree->RootWidget);
		}
		return FString::Join(Parts, TEXT(";"));
	}

	TArray<FString> BindingSnapshot() const
	{
		TArray<FString> Parts;
		for (const FDelegateEditorBinding& Binding : Blueprint->Bindings)
		{
			Parts.Add(Binding.ObjectName + TEXT("|") + Binding.PropertyName.ToString() + TEXT("|") +
					  Binding.SourceProperty.ToString() + TEXT("|") +
					  Binding.MemberGuid.ToString(EGuidFormats::DigitsWithHyphens) + TEXT("|") +
					  FString::FromInt(static_cast<int32>(Binding.Kind)));
		}
		return Parts;
	}
};

// --- depth cut -------------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexUMGTreeBoundsDepthZeroCutTest,
	"Cortex.UMG.TreeBounds.DepthZeroCut",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexUMGTreeBoundsDepthZeroCutTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	FCortexUMGTreeBoundsFixture Fixture(FGuid::NewGuid().ToString(EGuidFormats::Digits));
	if (!Fixture.BuildMainTree())
	{
		AddError(TEXT("fixture widget tree could not be created"));
		return false;
	}

	FCortexCommandRouter& Router = Fixture.Router;
	TSharedPtr<FJsonObject> Params = Fixture.ReadParams();
	Params->SetNumberField(TEXT("depth"), 0);
	const auto Cut = Router.Execute(TEXT("umg.get_tree"), Params);
	bool bTreeComplete = true;
	double ChildCount = -1;
	TestTrue(TEXT("tree completeness declared"), Cut.Data->TryGetBoolField(TEXT("tree_complete"), bTreeComplete));
	TestFalse(TEXT("cut is not complete"), bTreeComplete);
	TestTrue(TEXT("root child count declared"),
		Cut.Data->GetObjectField(TEXT("root"))->TryGetNumberField(TEXT("child_count"), ChildCount));
	TestTrue(TEXT("root advertises omitted children"), ChildCount > 0);

	TestTrue(TEXT("depth0 read succeeds"), Cut.bSuccess && Cut.Data.IsValid());
	if (!Cut.bSuccess || !Cut.Data.IsValid())
	{
		return false;
	}

	int32 Returned = -1;
	int32 Total = -1;
	TestTrue(TEXT("cut declares returned_widgets"), Cut.Data->TryGetNumberField(TEXT("returned_widgets"), Returned));
	TestEqual(TEXT("depth0 returns only the root node"), Returned, 1);
	TestTrue(TEXT("cut declares total_widgets"), Cut.Data->TryGetNumberField(TEXT("total_widgets"), Total));
	TestEqual(TEXT("total_widgets counts the whole asset tree"), Total, 4);

	const TArray<TSharedPtr<FJsonValue>>* Reasons = nullptr;
	TestTrue(TEXT("cut declares reasons"), Cut.Data->TryGetArrayField(TEXT("reasons"), Reasons));
	TestTrue(TEXT("cut explains itself"), Reasons != nullptr && Reasons->Num() > 0);
	return true;
}

// --- node budget cut -------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexUMGTreeBoundsMaxNodesCutTest,
	"Cortex.UMG.TreeBounds.MaxNodesCut",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexUMGTreeBoundsMaxNodesCutTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	FCortexUMGTreeBoundsFixture Fixture(FGuid::NewGuid().ToString(EGuidFormats::Digits));
	if (!Fixture.BuildMainTree())
	{
		AddError(TEXT("fixture widget tree could not be created"));
		return false;
	}

	const FCortexCommandResult Cut =
		Fixture.Router.Execute(TEXT("umg.get_tree"), Fixture.ReadParams(false, -1, 2));
	TestTrue(TEXT("node-bounded read succeeds"), Cut.bSuccess && Cut.Data.IsValid());
	if (!Cut.bSuccess || !Cut.Data.IsValid())
	{
		return false;
	}
	// FString comparison is case-insensitive; exact designer names need explicit case sensitivity.
	auto TestExactName = [this](const TCHAR* What, const FString& Actual, const TCHAR* Expected)
	{
		TestTrue(What, Actual.Equals(Expected, ESearchCase::CaseSensitive));
	};

	bool bTreeComplete = true;
	TestTrue(TEXT("node-bounded read declares completeness"),
			Cut.Data->TryGetBoolField(TEXT("tree_complete"), bTreeComplete));
	TestFalse(TEXT("node-bounded read is incomplete"), bTreeComplete);

	int32 Returned = -1;
	int32 Total = -1;
	TestTrue(TEXT("node-bounded read declares returned_widgets"),
			Cut.Data->TryGetNumberField(TEXT("returned_widgets"), Returned));
	TestEqual(TEXT("exactly two widgets are visited"), Returned, 2);
	TestTrue(TEXT("node-bounded read declares total_widgets"),
			Cut.Data->TryGetNumberField(TEXT("total_widgets"), Total));
	TestEqual(TEXT("total_widgets still counts the whole tree"), Total, 4);

	const TSharedPtr<FJsonObject>* Root = nullptr;
	TestTrue(TEXT("node-bounded read returns the root"), Cut.Data->TryGetObjectField(TEXT("root"), Root));
	if (!Root || !Root->IsValid())
	{
		return false;
	}

	bool bRootIncomplete = false;
	TestTrue(TEXT("root declares incompleteness"), (*Root)->TryGetBoolField(TEXT("incomplete"), bRootIncomplete));
	TestTrue(TEXT("root is incomplete under the node budget"), bRootIncomplete);
	double RootChildCount = -1;
	TestTrue(TEXT("root declares its child count"),
			(*Root)->TryGetNumberField(TEXT("child_count"), RootChildCount));
	TestEqual(TEXT("root keeps the honest child count"), RootChildCount, 2.0);

	const TArray<TSharedPtr<FJsonValue>>* RootChildren = nullptr;
	TestTrue(TEXT("root retains its visited child"), (*Root)->TryGetArrayField(TEXT("children"), RootChildren));
	if (!RootChildren || RootChildren->Num() == 0)
	{
		AddError(TEXT("node-bounded read dropped the visited child"));
		return false;
	}
	TestEqual(TEXT("only the visited child is retained"), RootChildren->Num(), 1);

	const TSharedPtr<FJsonObject> CutBranch = (*RootChildren)[0]->AsObject();
	if (!CutBranch.IsValid())
	{
		AddError(TEXT("retained child is not an object"));
		return false;
	}
	TestExactName(TEXT("retained child keeps its exact designer name"),
				  CutBranch->GetStringField(TEXT("name")), TEXT("NestedPanel"));
	bool bBranchIncomplete = false;
	TestTrue(TEXT("cut branch declares incompleteness"),
			CutBranch->TryGetBoolField(TEXT("incomplete"), bBranchIncomplete));
	TestTrue(TEXT("cut branch is incomplete"), bBranchIncomplete);
	double BranchChildCount = -1;
	TestTrue(TEXT("cut branch declares its child count"),
			CutBranch->TryGetNumberField(TEXT("child_count"), BranchChildCount));
	TestEqual(TEXT("cut branch is not a leaf"), BranchChildCount, 1.0);
	TestFalse(TEXT("cut branch omits unvisited children"), CutBranch->HasField(TEXT("children")));
	return true;
}

// --- names-only projection -------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexUMGTreeBoundsNamesOnlyTest,
	"Cortex.UMG.TreeBounds.NamesOnlyHierarchy",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexUMGTreeBoundsNamesOnlyTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	FCortexUMGTreeBoundsFixture Fixture(FGuid::NewGuid().ToString(EGuidFormats::Digits));
	if (!Fixture.BuildMainTree())
	{
		AddError(TEXT("fixture widget tree could not be created"));
		return false;
	}

	const FCortexCommandResult Names =
		Fixture.Router.Execute(TEXT("umg.get_tree"), Fixture.ReadParams(false, -1, -1, true));
	TestTrue(TEXT("names-only read succeeds"), Names.bSuccess && Names.Data.IsValid());
	if (!Names.bSuccess || !Names.Data.IsValid())
	{
		return false;
	}
	// FString comparison is case-insensitive; exact designer names need explicit case sensitivity.
	auto TestExactName = [this](const TCHAR* What, const FString& Actual, const TCHAR* Expected)
	{
		TestTrue(What, Actual.Equals(Expected, ESearchCase::CaseSensitive));
	};

	bool bTreeComplete = false;
	TestTrue(TEXT("names-only read declares completeness"),
			Names.Data->TryGetBoolField(TEXT("tree_complete"), bTreeComplete));
	TestTrue(TEXT("names-only read keeps the full hierarchy"), bTreeComplete);
	int32 Returned = -1;
	int32 Total = -1;
	TestTrue(TEXT("names-only read declares returned_widgets"),
			Names.Data->TryGetNumberField(TEXT("returned_widgets"), Returned));
	TestEqual(TEXT("names-only read returns every widget"), Returned, 4);
	TestTrue(TEXT("names-only read declares total_widgets"),
			Names.Data->TryGetNumberField(TEXT("total_widgets"), Total));
	TestEqual(TEXT("names-only read counts every widget"), Total, 4);

	const TSharedPtr<FJsonObject>* Root = nullptr;
	TestTrue(TEXT("names-only read returns the root"), Names.Data->TryGetObjectField(TEXT("root"), Root));
	if (!Root || !Root->IsValid())
	{
		return false;
	}
	TestExactName(TEXT("names-only root keeps its exact name"),
				  (*Root)->GetStringField(TEXT("name")), TEXT("RootPanel"));
	TestFalse(TEXT("names-only projection drops class identity"), (*Root)->HasField(TEXT("class")));
	double RootChildCount = -1;
	TestTrue(TEXT("names-only root keeps its child count"),
			(*Root)->TryGetNumberField(TEXT("child_count"), RootChildCount));
	TestEqual(TEXT("names-only root reports both children"), RootChildCount, 2.0);

	const TArray<TSharedPtr<FJsonValue>>* RootChildren = nullptr;
	TestTrue(TEXT("names-only root keeps its hierarchy"),
			(*Root)->TryGetArrayField(TEXT("children"), RootChildren));
	if (!RootChildren || RootChildren->Num() != 2)
	{
		AddError(TEXT("names-only hierarchy lost the root children"));
		return false;
	}
	TestExactName(TEXT("nested panel name preserved"),
				  (*RootChildren)[0]->AsObject()->GetStringField(TEXT("name")), TEXT("NestedPanel"));
	TestExactName(TEXT("sibling name preserved"),
				  (*RootChildren)[1]->AsObject()->GetStringField(TEXT("name")), TEXT("RootSibling"));

	const TSharedPtr<FJsonObject> Nested = (*RootChildren)[0]->AsObject();
	TestFalse(TEXT("names-only nested panel drops class identity"), Nested->HasField(TEXT("class")));
	const TArray<TSharedPtr<FJsonValue>>* NestedChildren = nullptr;
	TestTrue(TEXT("names-only nested panel keeps its child"),
			Nested->TryGetArrayField(TEXT("children"), NestedChildren));
	if (!NestedChildren || NestedChildren->Num() != 1)
	{
		AddError(TEXT("names-only hierarchy lost the nested leaf"));
		return false;
	}
	const TSharedPtr<FJsonObject> DeepLeaf = (*NestedChildren)[0]->AsObject();
	TestExactName(TEXT("deep leaf name preserved"),
				  DeepLeaf->GetStringField(TEXT("name")), TEXT("DeepLeaf"));
	TestFalse(TEXT("names-only actual leaf omits children"), DeepLeaf->HasField(TEXT("children")));
	return true;
}

// --- property bindings survive a tree cut ----------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexUMGTreeBoundsBindingOrphanUnderCutTest,
	"Cortex.UMG.TreeBounds.BindingOrphanUnderCut",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexUMGTreeBoundsBindingOrphanUnderCutTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	FCortexUMGTreeBoundsFixture Fixture(FGuid::NewGuid().ToString(EGuidFormats::Digits));
	if (!Fixture.BuildMainTree())
	{
		AddError(TEXT("fixture widget tree could not be created"));
		return false;
	}
	Fixture.SeedMainBindings();

	const FCortexCommandResult Cut =
		Fixture.Router.Execute(TEXT("umg.get_tree"), Fixture.ReadParams(true, 0));
	TestTrue(TEXT("cut read with bindings succeeds"), Cut.bSuccess && Cut.Data.IsValid());
	if (!Cut.bSuccess || !Cut.Data.IsValid())
	{
		return false;
	}

	bool bTreeComplete = true;
	TestTrue(TEXT("cut read declares completeness"), Cut.Data->TryGetBoolField(TEXT("tree_complete"), bTreeComplete));
	TestFalse(TEXT("the tree really is cut for this read"), bTreeComplete);

	const TSharedPtr<FJsonObject>* State = nullptr;
	TestTrue(TEXT("cut read still returns property_binding_state"),
			Cut.Data->TryGetObjectField(TEXT("property_binding_state"), State));
	if (!State || !State->IsValid())
	{
		return false;
	}
	bool bReaderComplete = false;
	TestTrue(TEXT("binding reader declares completeness"),
			(*State)->TryGetBoolField(TEXT("reader_complete"), bReaderComplete));
	TestTrue(TEXT("binding inventory stays complete under a tree cut"), bReaderComplete);
	int32 Total = -1;
	TestTrue(TEXT("binding inventory declares total"), (*State)->TryGetNumberField(TEXT("total"), Total));
	TestEqual(TEXT("both serialized records are preserved"), Total, 2);
	TestEqual(TEXT("binding scope is the whole asset"),
			(*State)->GetStringField(TEXT("scope")), FString(TEXT("asset")));

	const TArray<TSharedPtr<FJsonValue>>* Records = nullptr;
	TestTrue(TEXT("binding inventory lists records"), (*State)->TryGetArrayField(TEXT("bindings"), Records));
	if (!Records)
	{
		return false;
	}
	TestEqual(TEXT("no record is dropped by the tree cut"), Records->Num(), 2);
	bool bOrphanPreserved = false;
	bool bResolvedPreserved = false;
	FString OrphanRawName;
	for (const TSharedPtr<FJsonValue>& Record : *Records)
	{
		const TSharedPtr<FJsonObject> Object = Record.IsValid() ? Record->AsObject() : nullptr;
		if (!Object.IsValid())
		{
			continue;
		}
		// FString comparison is case-insensitive, so the case-only distinction is explicit here.
		const FString WidgetName = Object->GetStringField(TEXT("widget_name"));
		if (WidgetName.Equals(TEXT("rootSibling"), ESearchCase::CaseSensitive))
		{
			bOrphanPreserved = true;
			OrphanRawName = WidgetName;
		}
		else if (WidgetName.Equals(TEXT("RootSibling"), ESearchCase::CaseSensitive))
		{
			bResolvedPreserved = true;
		}
	}
	TestTrue(TEXT("case-only orphan record is preserved"), bOrphanPreserved);
	TestTrue(TEXT("exact-case record is preserved"), bResolvedPreserved);
	TestTrue(TEXT("case-only orphan keeps its exact raw widget name"),
			 OrphanRawName.Equals(TEXT("rootSibling"), ESearchCase::CaseSensitive));

	const TArray<TSharedPtr<FJsonValue>>* Diagnostics = nullptr;
	TestTrue(TEXT("binding inventory lists diagnostics"),
			(*State)->TryGetArrayField(TEXT("diagnostics"), Diagnostics));
	TestEqual(TEXT("only the case-only orphan is diagnosed despite the tree cut"),
			  Diagnostics != nullptr ? Diagnostics->Num() : -1, 1);
	if (!Diagnostics || Diagnostics->Num() != 1)
	{
		AddError(TEXT("diagnostics must resolve against the full tree, not the cut projection"));
		return false;
	}
	const FString OrphanDiagnostic = (*Diagnostics)[0]->AsString();
	TestTrue(TEXT("case-sensitive lookup does not resolve the case-only orphan"),
			 OrphanDiagnostic.Equals(TEXT("Missing target widget: rootSibling"), ESearchCase::CaseSensitive));
	return true;
}

// --- unsafe oversized combined read ----------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexUMGTreeBoundsOversizedCombinedReadTest,
	"Cortex.UMG.TreeBounds.OversizedCombinedReadRefused",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexUMGTreeBoundsOversizedCombinedReadTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	FCortexUMGTreeBoundsFixture Fixture(FGuid::NewGuid().ToString(EGuidFormats::Digits));
	if (!Fixture.BuildOversizedTree(120, 512))
	{
		AddError(TEXT("oversized fixture tree could not be created"));
		return false;
	}
	Fixture.SeedOversizedBinding();

	const FCortexCommandResult Oversized = Fixture.Router.Execute(TEXT("umg.get_tree"), Fixture.ReadParams(true));
	FString OversizedError;
	const bool bBoundedEnvelope =
		Oversized.Data.IsValid() && Oversized.Data->TryGetStringField(TEXT("_error"), OversizedError);
	TestTrue(TEXT("oversized combined read is refused explicitly instead of returning a complete tree"),
			 !Oversized.bSuccess || bBoundedEnvelope);
	if (!Oversized.bSuccess)
	{
		TestTrue(TEXT("oversized combined read reports a refusal code"), !Oversized.ErrorCode.IsEmpty());
		return true;
	}
	if (!Oversized.Data.IsValid())
	{
		return false;
	}
	TestTrue(TEXT("oversized combined read reports the bounded response error"),
			 OversizedError.Equals(TEXT("RESPONSE_TOO_LARGE"), ESearchCase::CaseSensitive));
	bool bReaderComplete = true;
	TestTrue(TEXT("oversized refusal declares reader_complete"),
			Oversized.Data->TryGetBoolField(TEXT("reader_complete"), bReaderComplete));
	TestFalse(TEXT("oversized refusal is incomplete"), bReaderComplete);
	TestTrue(TEXT("oversized refusal retains asset_path"), Oversized.Data->HasField(TEXT("asset_path")));
	TestFalse(TEXT("oversized refusal never claims a complete tree"),
			  Oversized.Data->HasField(TEXT("tree_complete")) && Oversized.Data->GetBoolField(TEXT("tree_complete")));
	return true;
}

// --- full tree recovery and read-only guarantees ---------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexUMGTreeBoundsFullTreeRecoveryTest,
	"Cortex.UMG.TreeBounds.FullTreeRecovery",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexUMGTreeBoundsFullTreeRecoveryTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	FCortexUMGTreeBoundsFixture Fixture(FGuid::NewGuid().ToString(EGuidFormats::Digits));
	if (!Fixture.BuildMainTree())
	{
		AddError(TEXT("fixture widget tree could not be created"));
		return false;
	}
	Fixture.SeedMainBindings();

	UTextBlock* Leaf = Cast<UTextBlock>(Fixture.WidgetByName(TEXT("DeepLeaf")));
	if (!Leaf)
	{
		AddError(TEXT("fixture leaf widget is missing"));
		return false;
	}
	Leaf->SetText(FText::FromString(TEXT("TreeBoundsLeafText")));

	const FString StructureBefore = Fixture.StructureSnapshot();
	const FString BindingsBefore = FString::Join(Fixture.BindingSnapshot(), TEXT(","));

	// Every bounded/projected read runs before the whole tree is recovered.
	Fixture.Router.Execute(TEXT("umg.get_tree"), Fixture.ReadParams(false, 0));
	Fixture.Router.Execute(TEXT("umg.get_tree"), Fixture.ReadParams(false, -1, 2));
	Fixture.Router.Execute(TEXT("umg.get_tree"), Fixture.ReadParams(false, -1, -1, true));
	Fixture.Router.Execute(TEXT("umg.get_tree"), Fixture.ReadParams(true, 0));

	TestTrue(TEXT("bounded reads leave the Designer tree unchanged"),
			 Fixture.StructureSnapshot().Equals(StructureBefore, ESearchCase::CaseSensitive));
	TestTrue(TEXT("bounded reads leave serialized bindings unchanged"),
			 FString::Join(Fixture.BindingSnapshot(), TEXT(",")).Equals(BindingsBefore, ESearchCase::CaseSensitive));

	const FCortexCommandResult Full = Fixture.Router.Execute(TEXT("umg.get_tree"), Fixture.ReadParams());
	TestTrue(TEXT("full read succeeds"), Full.bSuccess && Full.Data.IsValid());
	if (!Full.bSuccess || !Full.Data.IsValid())
	{
		return false;
	}
	// FString comparison is case-insensitive; exact designer names need explicit case sensitivity.
	auto TestExactName = [this](const TCHAR* What, const FString& Actual, const TCHAR* Expected)
	{
		TestTrue(What, Actual.Equals(Expected, ESearchCase::CaseSensitive));
	};
	// A complete node may omit the marker or report false; it must never claim a cut.
	auto IsMarkedIncomplete = [](const TSharedPtr<FJsonObject>& Node)
	{
		bool bIncomplete = false;
		return Node->TryGetBoolField(TEXT("incomplete"), bIncomplete) && bIncomplete;
	};

	bool bTreeComplete = false;
	TestTrue(TEXT("full read declares completeness"), Full.Data->TryGetBoolField(TEXT("tree_complete"), bTreeComplete));
	TestTrue(TEXT("full read is complete"), bTreeComplete);
	int32 Returned = -1;
	int32 Total = -1;
	TestTrue(TEXT("full read declares returned_widgets"), Full.Data->TryGetNumberField(TEXT("returned_widgets"), Returned));
	TestEqual(TEXT("full read returns every widget"), Returned, 4);
	TestTrue(TEXT("full read declares total_widgets"), Full.Data->TryGetNumberField(TEXT("total_widgets"), Total));
	TestEqual(TEXT("full read counts every widget"), Total, 4);
	const TArray<TSharedPtr<FJsonValue>>* Reasons = nullptr;
	if (Full.Data->TryGetArrayField(TEXT("reasons"), Reasons) && Reasons != nullptr)
	{
		TestEqual(TEXT("complete read has no reasons"), Reasons->Num(), 0);
	}

	const TSharedPtr<FJsonObject>* Root = nullptr;
	TestTrue(TEXT("full read returns the root"), Full.Data->TryGetObjectField(TEXT("root"), Root));
	if (!Root || !Root->IsValid())
	{
		return false;
	}
	TestExactName(TEXT("root identity recovered"),
				  (*Root)->GetStringField(TEXT("name")), TEXT("RootPanel"));
	TestExactName(TEXT("root class recovered"),
				  (*Root)->GetStringField(TEXT("class")), TEXT("CanvasPanel"));
	TestFalse(TEXT("complete root is not marked incomplete"), IsMarkedIncomplete(*Root));

	const TArray<TSharedPtr<FJsonValue>>* RootChildren = nullptr;
	TestTrue(TEXT("full read recovers the root hierarchy"),
			(*Root)->TryGetArrayField(TEXT("children"), RootChildren));
	if (!RootChildren || RootChildren->Num() != 2)
	{
		AddError(TEXT("full read lost the root children"));
		return false;
	}
	const TSharedPtr<FJsonObject> Nested = (*RootChildren)[0]->AsObject();
	const TSharedPtr<FJsonObject> Sibling = (*RootChildren)[1]->AsObject();
	TestExactName(TEXT("nested panel recovered first"), Nested->GetStringField(TEXT("name")), TEXT("NestedPanel"));
	TestExactName(TEXT("root sibling recovered second"), Sibling->GetStringField(TEXT("name")), TEXT("RootSibling"));
	TestFalse(TEXT("root sibling leaf omits children"), Sibling->HasField(TEXT("children")));

	const TArray<TSharedPtr<FJsonValue>>* NestedChildren = nullptr;
	TestTrue(TEXT("full read recovers the nested hierarchy"),
			Nested->TryGetArrayField(TEXT("children"), NestedChildren));
	if (!NestedChildren || NestedChildren->Num() != 1)
	{
		AddError(TEXT("full read lost the nested leaf"));
		return false;
	}
	const TSharedPtr<FJsonObject> DeepLeaf = (*NestedChildren)[0]->AsObject();
	TestExactName(TEXT("deep leaf recovered"), DeepLeaf->GetStringField(TEXT("name")), TEXT("DeepLeaf"));
	TestFalse(TEXT("actual leaf omits children"), DeepLeaf->HasField(TEXT("children")));
	TestFalse(TEXT("complete nested panel is not marked incomplete"), IsMarkedIncomplete(Nested));

	TestTrue(TEXT("full read leaves the Designer tree unchanged"),
			 Fixture.StructureSnapshot().Equals(StructureBefore, ESearchCase::CaseSensitive));
	TestTrue(TEXT("full read leaves serialized bindings unchanged"),
			 FString::Join(Fixture.BindingSnapshot(), TEXT(",")).Equals(BindingsBefore, ESearchCase::CaseSensitive));

	UTextBlock* LeafAfter = Cast<UTextBlock>(Fixture.WidgetByName(TEXT("DeepLeaf")));
	TestNotNull(TEXT("leaf widget still exists after reads"), LeafAfter);
	if (LeafAfter)
	{
		TestTrue(TEXT("widget properties are unchanged by reads"),
				 LeafAfter->GetText().ToString().Equals(TEXT("TreeBoundsLeafText"), ESearchCase::CaseSensitive));
	}
	return true;
}

// --- compact-admissible combined read is never refused ----------------------------------------

// The bounded-response ceiling applies to the compact (model-facing) JSON text the command actually
// ships. 110 children with a 25-character fill put the combined tree + inventory payload at roughly
// 34 kB of condensed text (each node carries is_variable and its slot summary) while its
// pretty-indented estimate is far larger, so a read judged on the shipped text must stay accepted
// even though a pretty-estimate ceiling would have refused it.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexUMGTreeBoundsCompactAdmissibleCombinedTest,
	"Cortex.UMG.TreeBounds.CompactAdmissibleCombinedReadAccepted",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexUMGTreeBoundsCompactAdmissibleCombinedTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	FCortexUMGTreeBoundsFixture Fixture(FGuid::NewGuid().ToString(EGuidFormats::Digits));
	if (!Fixture.BuildOversizedTree(110, 25))
	{
		AddError(TEXT("compact-admissible fixture tree could not be created"));
		return false;
	}
	Fixture.SeedOversizedBinding();

	const FCortexCommandResult Read =
		Fixture.Router.Execute(TEXT("umg.get_tree"), Fixture.ReadParams(true));
	TestTrue(TEXT("compact-admissible combined read succeeds"), Read.bSuccess && Read.Data.IsValid());
	if (!Read.bSuccess || !Read.Data.IsValid())
	{
		return false;
	}
	TestFalse(TEXT("compact-admissible combined read is not refused"),
			  Read.Data->HasField(TEXT("_error")));

	// Same condensed writer the command measures itself with: this is the exact text length the
	// ceiling is compared against, so the fixture is admissible by construction.
	FString CompactText;
	const TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> Writer =
		TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&CompactText);
	TestTrue(TEXT("combined payload serializes to condensed text"),
			 FJsonSerializer::Serialize(Read.Data.ToSharedRef(), Writer));
	TestTrue(TEXT("combined payload fits the bounded-response ceiling"), CompactText.Len() < 40000);
	TestTrue(TEXT("fixture payload is genuinely large"), CompactText.Len() > 30000);

	bool bTreeComplete = false;
	TestTrue(TEXT("compact-admissible read declares completeness"),
			 Read.Data->TryGetBoolField(TEXT("tree_complete"), bTreeComplete));
	TestTrue(TEXT("compact-admissible read recovers the whole tree"), bTreeComplete);
	int32 Returned = -1;
	TestTrue(TEXT("compact-admissible read declares returned_widgets"),
			 Read.Data->TryGetNumberField(TEXT("returned_widgets"), Returned));
	TestEqual(TEXT("compact-admissible read returns every widget"), Returned, 111);
	int32 Total = -1;
	TestTrue(TEXT("compact-admissible read declares total_widgets"),
			 Read.Data->TryGetNumberField(TEXT("total_widgets"), Total));
	TestEqual(TEXT("compact-admissible read counts every widget"), Total, 111);

	const TSharedPtr<FJsonObject>* State = nullptr;
	TestTrue(TEXT("compact-admissible read still returns the binding inventory"),
			 Read.Data->TryGetObjectField(TEXT("property_binding_state"), State));
	if (!State || !State->IsValid())
	{
		return false;
	}
	bool bReaderComplete = false;
	TestTrue(TEXT("compact-admissible read declares reader_complete"),
			 (*State)->TryGetBoolField(TEXT("reader_complete"), bReaderComplete));
	TestTrue(TEXT("compact-admissible read keeps the complete inventory"), bReaderComplete);
	int32 BindingTotal = -1;
	TestTrue(TEXT("compact-admissible read declares the inventory total"),
			 (*State)->TryGetNumberField(TEXT("total"), BindingTotal));
	TestEqual(TEXT("compact-admissible read keeps the seeded record"), BindingTotal, 1);
	return true;
}
