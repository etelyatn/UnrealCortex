// reflect.find_duplicate_graphs tests. Fixtures are saved assets under /Game/Temp/Cortex120/Duplicate/<guid>.
// Graphs are built separately per Blueprint (fresh GUIDs, different positions) so equality can only come from the
// normalised graph form.

#include "Misc/AutomationTest.h"
#include "CortexReflectCommandHandler.h"
#include "CortexTypes.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "Blueprint/UserWidget.h"
#include "Blueprint/WidgetTree.h"
#include "Components/CanvasPanel.h"
#include "Components/TextBlock.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "EdGraph/EdGraph.h"
#include "EdGraphNode_Comment.h"
#include "EdGraphSchema_K2.h"
#include "Engine/Blueprint.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "GameFramework/Actor.h"
#include "HAL/FileManager.h"
#include "K2Node_CallFunction.h"
#include "K2Node_CustomEvent.h"
#include "K2Node_FunctionEntry.h"
#include "K2Node_DynamicCast.h"
#include "K2Node_EnumLiteral.h"
#include "Kismet/GameplayStatics.h"
#include "Kismet/KismetSystemLibrary.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Misc/FileHelper.h"
#include "Misc/Guid.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "UObject/GarbageCollection.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"
#include "UObject/UObjectIterator.h"

namespace CortexReflectDuplicateGraphTestSupport
{
	TArray<TSharedPtr<FJsonObject>> ObjectArray(const TSharedPtr<FJsonObject>& Object, const TCHAR* Field)
	{
		TArray<TSharedPtr<FJsonObject>> Out;
		const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
		if (Object.IsValid() && Object->TryGetArrayField(Field, Values))
		{
			for (const TSharedPtr<FJsonValue>& Value : *Values)
			{
				Out.Add(Value->AsObject());
			}
		}
		return Out;
	}

	TArray<TSharedPtr<FJsonValue>> JsonStrings(const TArray<FString>& Strings)
	{
		TArray<TSharedPtr<FJsonValue>> Values;
		for (const FString& String : Strings)
		{
			Values.Add(MakeShared<FJsonValueString>(String));
		}
		return Values;
	}

	/** "BP_A:Compute" labels of a group's members: asset name and graph name. */
	TArray<FString> MemberLabels(const TSharedPtr<FJsonObject>& Group)
	{
		TArray<FString> Labels;
		for (const TSharedPtr<FJsonObject>& Member : ObjectArray(Group, TEXT("members")))
		{
			Labels.Add(FString::Printf(TEXT("%s:%s"), *FPaths::GetBaseFilename(Member->GetStringField(TEXT("asset_path"))),
				*Member->GetStringField(TEXT("graph_name"))));
		}
		Labels.Sort();
		return Labels;
	}

	TSharedPtr<FJsonObject> FindGroup(const TArray<TSharedPtr<FJsonObject>>& Groups, const FString& Kind)
	{
		for (const TSharedPtr<FJsonObject>& Group : Groups)
		{
			if (Group->GetStringField(TEXT("kind")) == Kind)
			{
				return Group;
			}
		}
		return nullptr;
	}

	/** The pair whose asset paths end in these two Blueprint names, in either order. */
	TSharedPtr<FJsonObject> FindPair(const TArray<TSharedPtr<FJsonObject>>& Pairs, const FString& First, const FString& Second)
	{
		for (const TSharedPtr<FJsonObject>& Pair : Pairs)
		{
			const FString A = FPaths::GetBaseFilename(Pair->GetStringField(TEXT("a")));
			const FString B = FPaths::GetBaseFilename(Pair->GetStringField(TEXT("b")));
			if ((A == First && B == Second) || (A == Second && B == First))
			{
				return Pair;
			}
		}
		return nullptr;
	}

	/** Saved Blueprint fixtures under /Game/Temp/Cortex120/Duplicate/<guid>. */
	struct FDuplicateFixture
	{
		FString Suffix = FGuid::NewGuid().ToString(EGuidFormats::Digits);
		FString Dir = FString::Printf(TEXT("/Game/Temp/Cortex120/Duplicate/%s"), *Suffix);
		TArray<FString> SavedFiles;

		bool SaveAsset(UObject* Asset)
		{
			UPackage* Package = Asset->GetOutermost();
			FString Filename;
			if (!FPackageName::TryConvertLongPackageNameToFilename(Package->GetName(), Filename, FPackageName::GetAssetPackageExtension()))
			{
				return false;
			}
			IFileManager::Get().MakeDirectory(*FPaths::GetPath(Filename), true);
			FSavePackageArgs SaveArgs;
			SaveArgs.TopLevelFlags = RF_Public | RF_Standalone;
			if (!UPackage::SavePackage(Package, Asset, *Filename, SaveArgs))
			{
				return false;
			}
			SavedFiles.Add(Filename);
			return true;
		}

		UBlueprint* MakeBlueprint(const FString& Name)
		{
			UPackage* Package = CreatePackage(*FString::Printf(TEXT("%s/%s"), *Dir, *Name));
			return FKismetEditorUtilities::CreateBlueprint(
				AActor::StaticClass(), Package, FName(*Name), BPTYPE_Normal, UBlueprint::StaticClass(),
				UBlueprintGeneratedClass::StaticClass(), NAME_None);
		}

		static UK2Node_CallFunction* AddPrint(UEdGraph* Graph, const FString& Text, const int32 X, const int32 Y)
		{
			FGraphNodeCreator<UK2Node_CallFunction> Creator(*Graph);
			UK2Node_CallFunction* Node = Creator.CreateNode();
			Node->FunctionReference.SetExternalMember(
				GET_FUNCTION_NAME_CHECKED(UKismetSystemLibrary, PrintString), UKismetSystemLibrary::StaticClass());
			Node->AllocateDefaultPins();
			if (UEdGraphPin* Pin = Node->FindPin(TEXT("InString"), EGPD_Input))
			{
				Pin->DefaultValue = Text;
			}
			Node->NodePosX = X;
			Node->NodePosY = Y;
			Creator.Finalize();
			return Node;
		}

		/** Links Start.then -> print(Texts[0]).then -> print(Texts[1])... and returns the number of nodes added. */
		static void AddPrintChain(UEdGraph* Graph, UEdGraphNode* Start, const TArray<FString>& Texts, const int32 PositionOffset)
		{
			const UEdGraphSchema_K2* Schema = GetDefault<UEdGraphSchema_K2>();
			UEdGraphNode* Previous = Start;
			int32 Index = 0;
			for (const FString& Text : Texts)
			{
				UK2Node_CallFunction* Print = AddPrint(Graph, Text, PositionOffset + 300 * Index, PositionOffset / 2 + 40 * Index);
				Schema->TryCreateConnection(Previous->FindPin(UEdGraphSchema_K2::PN_Then, EGPD_Output), Print->FindPin(UEdGraphSchema_K2::PN_Execute, EGPD_Input));
				Previous = Print;
				++Index;
			}
		}

		/** Function graph: entry plus one print node per text. Noise (comment box, disabled node) never changes the form. */
		static UEdGraphNode* FindEntry(UEdGraph* Function)
		{
			for (UEdGraphNode* Node : Function->Nodes)
			{
				if (Node->IsA<UK2Node_FunctionEntry>())
				{
					return Node;
				}
			}
			return nullptr;
		}

		static UEdGraph* AddFunction(UBlueprint* Blueprint, const FName Name, const TArray<FString>& Texts, const int32 PositionOffset, const bool bNoise)
		{
			UEdGraph* Function = FBlueprintEditorUtils::CreateNewGraph(
				Blueprint, Name, UEdGraph::StaticClass(), UEdGraphSchema_K2::StaticClass());
			FBlueprintEditorUtils::AddFunctionGraph<UClass>(Blueprint, Function, true, nullptr);
			UEdGraphNode* Entry = FindEntry(Function);
			Entry->NodePosX = PositionOffset;
			AddPrintChain(Function, Entry, Texts, PositionOffset);
			if (bNoise)
			{
				FGraphNodeCreator<UEdGraphNode_Comment> Comment(*Function);
				UEdGraphNode_Comment* Box = Comment.CreateNode();
				Box->NodeComment = TEXT("Explains nothing about the logic");
				Comment.Finalize();
				UK2Node_CallFunction* Disabled = AddPrint(Function, TEXT("Never runs"), 7000, 7000);
				Disabled->SetEnabledState(ENodeEnabledState::Disabled);
			}
			return Function;
		}

		/** Event chain: a custom event followed by one print node per text, in the ubergraph. */
		static void AddEventChain(UBlueprint* Blueprint, const FName EventName, const TArray<FString>& Texts, const int32 PositionOffset)
		{
			UEdGraph* Event = Blueprint->UbergraphPages[0];
			FGraphNodeCreator<UK2Node_CustomEvent> Creator(*Event);
			UK2Node_CustomEvent* Node = Creator.CreateNode();
			Node->CustomFunctionName = EventName;
			Node->NodePosX = PositionOffset;
			Creator.Finalize();
			AddPrintChain(Event, Node, Texts, PositionOffset);
		}

		static UWidgetTree* GetWidgetTree(UBlueprint* WidgetBlueprint)
		{
			const FObjectProperty* Property = CastField<FObjectProperty>(WidgetBlueprint->GetClass()->FindPropertyByName(TEXT("WidgetTree")));
			return Property ? Cast<UWidgetTree>(Property->GetObjectPropertyValue_InContainer(WidgetBlueprint)) : nullptr;
		}

		/** Widget Blueprint with a canvas root and one text block per name, added in the given order. */
		UBlueprint* MakeWidgetBlueprint(const FString& Name, const TArray<FString>& TextNames, const FString& SubDir = FString())
		{
			UClass* WidgetBlueprintClass = FindObject<UClass>(nullptr, TEXT("/Script/UMGEditor.WidgetBlueprint"));
			UClass* WidgetGeneratedClass = FindObject<UClass>(nullptr, TEXT("/Script/UMG.WidgetBlueprintGeneratedClass"));
			if (!WidgetBlueprintClass || !WidgetGeneratedClass)
			{
				return nullptr;
			}
			UPackage* Package = CreatePackage(*FString::Printf(TEXT("%s%s/%s"), *Dir, *SubDir, *Name));
			UBlueprint* Blueprint = FKismetEditorUtilities::CreateBlueprint(
				UUserWidget::StaticClass(), Package, FName(*Name), BPTYPE_Normal, WidgetBlueprintClass, WidgetGeneratedClass, NAME_None);
			if (!Blueprint)
			{
				return nullptr;
			}
			UWidgetTree* Tree = GetWidgetTree(Blueprint);
			if (!Tree)
			{
				const FObjectProperty* Property = CastField<FObjectProperty>(Blueprint->GetClass()->FindPropertyByName(TEXT("WidgetTree")));
				if (!Property)
				{
					return nullptr;
				}
				Tree = NewObject<UWidgetTree>(Blueprint, UWidgetTree::StaticClass(), TEXT("WidgetTree"), RF_Transactional);
				Property->SetObjectPropertyValue_InContainer(Blueprint, Tree);
			}
			UCanvasPanel* Root = Tree->ConstructWidget<UCanvasPanel>(UCanvasPanel::StaticClass(), TEXT("Root"));
			Tree->RootWidget = Root;
			for (const FString& TextName : TextNames)
			{
				Root->AddChild(Tree->ConstructWidget<UTextBlock>(UTextBlock::StaticClass(), FName(*TextName)));
			}
			return Blueprint;
		}

		void RegisterSavedFiles() const
		{
			IAssetRegistry::GetChecked().ScanModifiedAssetFiles(SavedFiles);
		}

		FCortexCommandResult Run(const TOptional<int32> MinNodes, const TOptional<double> Similarity) const
		{
			TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
			Params->SetArrayField(TEXT("path_prefixes"), JsonStrings({Dir}));
			if (MinNodes.IsSet())
			{
				Params->SetNumberField(TEXT("min_nodes"), MinNodes.GetValue());
			}
			if (Similarity.IsSet())
			{
				Params->SetNumberField(TEXT("similarity"), Similarity.GetValue());
			}
			FCortexReflectCommandHandler Handler;
			return Handler.Execute(TEXT("find_duplicate_graphs"), Params);
		}

		void Cleanup()
		{
			IAssetRegistry::GetChecked().WaitForCompletion();
			TArray<UObject*> Assets;
			for (TObjectIterator<UObject> It; It; ++It)
			{
				UObject* Asset = *It;
				if (Asset && Asset->IsAsset() && Asset->GetOutermost()->GetName().StartsWith(Dir))
				{
					Assets.Add(Asset);
				}
			}
			for (UObject* Asset : Assets)
			{
				Asset->GetOutermost()->ClearDirtyFlag();
				Asset->ClearFlags(RF_Public | RF_Standalone);
				Asset->MarkAsGarbage();
			}
			CollectGarbage(RF_NoFlags);
			IFileManager::Get().DeleteDirectory(
				*FPaths::Combine(FPaths::ProjectContentDir(), TEXT("Temp"), TEXT("Cortex120"), TEXT("Duplicate"), Suffix), false, true);
		}
	};

	const TArray<FString> FourTexts = {TEXT("Alpha"), TEXT("Beta"), TEXT("Gamma"), TEXT("Delta")};
	const TArray<FString> FiveTexts = {TEXT("Alpha"), TEXT("Beta"), TEXT("Gamma"), TEXT("Delta"), TEXT("Epsilon")};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexReflectDuplicateGraphGroupsTest,
	"Cortex.Reflect.DuplicateGraphs.Groups",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexReflectDuplicateGraphGroupsTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	using namespace CortexReflectDuplicateGraphTestSupport;

	// A and B hold the same function (B under another name, with a comment box and a disabled node, at other
	// positions) and the same event chain; C holds a same-named function with one extra node.
	FDuplicateFixture Fixture;
	UBlueprint* A = Fixture.MakeBlueprint(TEXT("BP_A"));
	UBlueprint* B = Fixture.MakeBlueprint(TEXT("BP_B"));
	UBlueprint* C = Fixture.MakeBlueprint(TEXT("BP_C"));
	if (!TestNotNull(TEXT("A"), A) || !TestNotNull(TEXT("B"), B) || !TestNotNull(TEXT("C"), C))
	{
		Fixture.Cleanup();
		return false;
	}
	FDuplicateFixture::AddFunction(A, TEXT("Compute"), FourTexts, 0, false);
	FDuplicateFixture::AddFunction(B, TEXT("ComputeCopy"), FourTexts, 1200, true);
	FDuplicateFixture::AddFunction(C, TEXT("Compute"), FiveTexts, 400, false);
	FDuplicateFixture::AddEventChain(A, TEXT("Ping"), FourTexts, 0);
	FDuplicateFixture::AddEventChain(B, TEXT("Ping"), FourTexts, 900);
	if (!TestTrue(TEXT("save A"), Fixture.SaveAsset(A)) || !TestTrue(TEXT("save B"), Fixture.SaveAsset(B)) || !TestTrue(TEXT("save C"), Fixture.SaveAsset(C)))
	{
		Fixture.Cleanup();
		return false;
	}
	Fixture.RegisterSavedFiles();

	{
		const FCortexCommandResult Run = Fixture.Run({}, {});
		if (TestTrue(TEXT("default run succeeds"), Run.bSuccess) && TestTrue(TEXT("data"), Run.Data.IsValid()))
		{
			const TArray<TSharedPtr<FJsonObject>> Exact = ObjectArray(Run.Data, TEXT("exact_groups"));
			TestEqual(TEXT("exact group count (function and event chain)"), Exact.Num(), 2);
			const TSharedPtr<FJsonObject> Function = FindGroup(Exact, TEXT("function"));
			const TSharedPtr<FJsonObject> Event = FindGroup(Exact, TEXT("event"));
			if (TestNotNull(TEXT("function group"), Function.Get()))
			{
				TestEqual(TEXT("function members (names, comments, positions and disabled nodes do not matter)"),
					MemberLabels(Function), TArray<FString>{TEXT("BP_A:Compute"), TEXT("BP_B:ComputeCopy")});
				TestEqual(TEXT("function node count"), static_cast<int32>(Function->GetNumberField(TEXT("node_count"))), 5);
				TestTrue(TEXT("function hash"), !Function->GetStringField(TEXT("hash")).IsEmpty());
			}
			if (TestNotNull(TEXT("event group"), Event.Get()))
			{
				TestEqual(TEXT("event members"), MemberLabels(Event), TArray<FString>{TEXT("BP_A:Ping"), TEXT("BP_B:Ping")});
				TestEqual(TEXT("event node count (event + four prints)"), static_cast<int32>(Event->GetNumberField(TEXT("node_count"))), 5);
			}
			TestEqual(TEXT("0.833 Jaccard is below the default 0.9"), ObjectArray(Run.Data, TEXT("similar_groups")).Num(), 0);

			const TArray<TSharedPtr<FJsonObject>> Pairs = ObjectArray(Run.Data, TEXT("blueprint_pairs"));
			TestEqual(TEXT("one pair under the default threshold"), Pairs.Num(), 1);
			const TSharedPtr<FJsonObject> AB = FindPair(Pairs, TEXT("BP_A"), TEXT("BP_B"));
			if (TestNotNull(TEXT("A-B pair"), AB.Get()))
			{
				TestEqual(TEXT("A-B shares the function and the event chain"), static_cast<int32>(AB->GetNumberField(TEXT("shared_graphs"))), 2);
				TestEqual(TEXT("no widget trees"), AB->GetNumberField(TEXT("widget_tree_similarity")), 0.0);
			}
		}
	}

	{
		const FCortexCommandResult Run = Fixture.Run({}, 0.8);
		if (TestTrue(TEXT("0.8 run succeeds"), Run.bSuccess) && TestTrue(TEXT("data"), Run.Data.IsValid()))
		{
			TestEqual(TEXT("exact groups unchanged"), ObjectArray(Run.Data, TEXT("exact_groups")).Num(), 2);
			const TArray<TSharedPtr<FJsonObject>> Similar = ObjectArray(Run.Data, TEXT("similar_groups"));
			if (TestEqual(TEXT("one similar group"), Similar.Num(), 1))
			{
				TestEqual(TEXT("similar kind"), Similar[0]->GetStringField(TEXT("kind")), FString(TEXT("function")));
				TestEqual(TEXT("similar name"), Similar[0]->GetStringField(TEXT("name")), FString(TEXT("Compute")));
				TestEqual(TEXT("similar members (the exact copy under another name is not compared)"),
					MemberLabels(Similar[0]), TArray<FString>{TEXT("BP_A:Compute"), TEXT("BP_C:Compute")});
				TestEqual(TEXT("min_similarity is 5 shared of 6 descriptors"), Similar[0]->GetNumberField(TEXT("min_similarity")), 5.0 / 6.0, 0.001);
				TestEqual(TEXT("max_similarity"), Similar[0]->GetNumberField(TEXT("max_similarity")), 5.0 / 6.0, 0.001);
			}
			const TArray<TSharedPtr<FJsonObject>> Pairs = ObjectArray(Run.Data, TEXT("blueprint_pairs"));
			if (TestEqual(TEXT("two pairs"), Pairs.Num(), 2))
			{
				TestEqual(TEXT("pairs sorted by shared graphs: A-B first"), FPaths::GetBaseFilename(Pairs[0]->GetStringField(TEXT("a"))) + FPaths::GetBaseFilename(Pairs[0]->GetStringField(TEXT("b"))), FString(TEXT("BP_ABP_B")));
				TestEqual(TEXT("A-B shared"), static_cast<int32>(Pairs[0]->GetNumberField(TEXT("shared_graphs"))), 2);
				const TSharedPtr<FJsonObject> AC = FindPair(Pairs, TEXT("BP_A"), TEXT("BP_C"));
				if (TestNotNull(TEXT("A-C pair"), AC.Get()))
				{
					TestEqual(TEXT("A-C shares the similar function"), static_cast<int32>(AC->GetNumberField(TEXT("shared_graphs"))), 1);
				}
			}
		}
	}

	Fixture.Cleanup();
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexReflectDuplicateGraphMinNodesTest,
	"Cortex.Reflect.DuplicateGraphs.MinNodes",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexReflectDuplicateGraphMinNodesTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	using namespace CortexReflectDuplicateGraphTestSupport;

	FDuplicateFixture Fixture;
	UBlueprint* A = Fixture.MakeBlueprint(TEXT("BP_A"));
	UBlueprint* B = Fixture.MakeBlueprint(TEXT("BP_B"));
	if (!TestNotNull(TEXT("A"), A) || !TestNotNull(TEXT("B"), B))
	{
		Fixture.Cleanup();
		return false;
	}
	FDuplicateFixture::AddFunction(A, TEXT("Compute"), FourTexts, 0, false);
	FDuplicateFixture::AddFunction(B, TEXT("Compute"), FourTexts, 800, false);
	if (!TestTrue(TEXT("save A"), Fixture.SaveAsset(A)) || !TestTrue(TEXT("save B"), Fixture.SaveAsset(B)))
	{
		Fixture.Cleanup();
		return false;
	}
	Fixture.RegisterSavedFiles();

	const FCortexCommandResult AtFive = Fixture.Run(5, {});
	if (TestTrue(TEXT("min_nodes 5 succeeds"), AtFive.bSuccess) && TestTrue(TEXT("data"), AtFive.Data.IsValid()))
	{
		TestEqual(TEXT("a five-node graph meets min_nodes 5"), ObjectArray(AtFive.Data, TEXT("exact_groups")).Num(), 1);
	}
	const FCortexCommandResult AtSix = Fixture.Run(6, {});
	if (TestTrue(TEXT("min_nodes 6 succeeds"), AtSix.bSuccess) && TestTrue(TEXT("data"), AtSix.Data.IsValid()))
	{
		TestEqual(TEXT("a five-node graph is below min_nodes 6: no exact group"), ObjectArray(AtSix.Data, TEXT("exact_groups")).Num(), 0);
		TestEqual(TEXT("no similar group"), ObjectArray(AtSix.Data, TEXT("similar_groups")).Num(), 0);
		TestEqual(TEXT("no pairs"), ObjectArray(AtSix.Data, TEXT("blueprint_pairs")).Num(), 0);
	}
	Fixture.Cleanup();
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexReflectDuplicateGraphWidgetTest,
	"Cortex.Reflect.DuplicateGraphs.WidgetTrees",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexReflectDuplicateGraphWidgetTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	using namespace CortexReflectDuplicateGraphTestSupport;

	// W1 and W2 hold the same class+name multiset (children in another order); W3 has one more widget.
	FDuplicateFixture Fixture;
	UBlueprint* W1 = Fixture.MakeWidgetBlueprint(TEXT("WBP_One"), {TEXT("T1"), TEXT("T2"), TEXT("T3"), TEXT("T4")});
	UBlueprint* W2 = Fixture.MakeWidgetBlueprint(TEXT("WBP_Two"), {TEXT("T4"), TEXT("T3"), TEXT("T2"), TEXT("T1")});
	UBlueprint* W3 = Fixture.MakeWidgetBlueprint(TEXT("WBP_Three"), {TEXT("T1"), TEXT("T2"), TEXT("T3"), TEXT("T4"), TEXT("T5")});
	if (!TestNotNull(TEXT("W1"), W1) || !TestNotNull(TEXT("W2"), W2) || !TestNotNull(TEXT("W3"), W3))
	{
		Fixture.Cleanup();
		return false;
	}
	if (!TestTrue(TEXT("save W1"), Fixture.SaveAsset(W1)) || !TestTrue(TEXT("save W2"), Fixture.SaveAsset(W2)) || !TestTrue(TEXT("save W3"), Fixture.SaveAsset(W3)))
	{
		Fixture.Cleanup();
		return false;
	}
	Fixture.RegisterSavedFiles();

	{
		const FCortexCommandResult Run = Fixture.Run({}, {});
		if (TestTrue(TEXT("default run succeeds"), Run.bSuccess) && TestTrue(TEXT("data"), Run.Data.IsValid()))
		{
			const TArray<TSharedPtr<FJsonObject>> Exact = ObjectArray(Run.Data, TEXT("exact_groups"));
			const TSharedPtr<FJsonObject> Widget = FindGroup(Exact, TEXT("widget_tree"));
			TestEqual(TEXT("one exact group"), Exact.Num(), 1);
			if (TestNotNull(TEXT("widget_tree group"), Widget.Get()))
			{
				TestEqual(TEXT("members are the two matching trees"), MemberLabels(Widget).Num(), 2);
				TestTrue(TEXT("W1 member"), MemberLabels(Widget)[0].StartsWith(TEXT("WBP_One")) || MemberLabels(Widget)[1].StartsWith(TEXT("WBP_One")));
				TestTrue(TEXT("W2 member"), MemberLabels(Widget)[0].StartsWith(TEXT("WBP_Two")) || MemberLabels(Widget)[1].StartsWith(TEXT("WBP_Two")));
				TestEqual(TEXT("widget count (root + four text blocks)"), static_cast<int32>(Widget->GetNumberField(TEXT("node_count"))), 5);
			}
			TestEqual(TEXT("5/6 is below 0.9: no similar group"), ObjectArray(Run.Data, TEXT("similar_groups")).Num(), 0);
			const TArray<TSharedPtr<FJsonObject>> Pairs = ObjectArray(Run.Data, TEXT("blueprint_pairs"));
			TestEqual(TEXT("only the identical pair"), Pairs.Num(), 1);
			const TSharedPtr<FJsonObject> OneTwo = FindPair(Pairs, TEXT("WBP_One"), TEXT("WBP_Two"));
			if (TestNotNull(TEXT("One-Two pair"), OneTwo.Get()))
			{
				TestEqual(TEXT("widget_tree_similarity"), OneTwo->GetNumberField(TEXT("widget_tree_similarity")), 1.0, 0.0001);
				TestEqual(TEXT("no graphs shared"), static_cast<int32>(OneTwo->GetNumberField(TEXT("shared_graphs"))), 0);
			}
		}
	}
	{
		const FCortexCommandResult Run = Fixture.Run({}, 0.8);
		if (TestTrue(TEXT("0.8 run succeeds"), Run.bSuccess) && TestTrue(TEXT("data"), Run.Data.IsValid()))
		{
			const TArray<TSharedPtr<FJsonObject>> Similar = ObjectArray(Run.Data, TEXT("similar_groups"));
			if (TestEqual(TEXT("one similar widget group"), Similar.Num(), 1))
			{
				TestEqual(TEXT("kind"), Similar[0]->GetStringField(TEXT("kind")), FString(TEXT("widget_tree")));
				TestEqual(TEXT("two members: the exact pair collapses to one representative"), ObjectArray(Similar[0], TEXT("members")).Num(), 2);
				TestEqual(TEXT("min_similarity 5/6"), Similar[0]->GetNumberField(TEXT("min_similarity")), 5.0 / 6.0, 0.001);
				TestEqual(TEXT("max_similarity 5/6"), Similar[0]->GetNumberField(TEXT("max_similarity")), 5.0 / 6.0, 0.001);
			}
			const TArray<TSharedPtr<FJsonObject>> Pairs = ObjectArray(Run.Data, TEXT("blueprint_pairs"));
			TestEqual(TEXT("three widget pairs"), Pairs.Num(), 3);
			const TSharedPtr<FJsonObject> OneThree = FindPair(Pairs, TEXT("WBP_One"), TEXT("WBP_Three"));
			if (TestNotNull(TEXT("One-Three pair"), OneThree.Get()))
			{
				TestEqual(TEXT("One-Three similarity"), OneThree->GetNumberField(TEXT("widget_tree_similarity")), 5.0 / 6.0, 0.001);
			}
		}
	}
	Fixture.Cleanup();
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexReflectDuplicateGraphSelfReferenceTest,
	"Cortex.Reflect.DuplicateGraphs.SelfReference",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexReflectDuplicateGraphSelfReferenceTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	using namespace CortexReflectDuplicateGraphTestSupport;

	// Each Blueprint's function casts to its own class and passes its own class as a class pin default.
	FDuplicateFixture Fixture;
	UBlueprint* A = Fixture.MakeBlueprint(TEXT("BP_SelfA"));
	UBlueprint* B = Fixture.MakeBlueprint(TEXT("BP_SelfB"));
	if (!TestNotNull(TEXT("A"), A) || !TestNotNull(TEXT("B"), B) || !TestNotNull(TEXT("A class"), A->GeneratedClass.Get())
		|| !TestNotNull(TEXT("B class"), B->GeneratedClass.Get()))
	{
		Fixture.Cleanup();
		return false;
	}
	for (UBlueprint* Blueprint : {A, B})
	{
		UEdGraph* Function = FBlueprintEditorUtils::CreateNewGraph(
			Blueprint, TEXT("Work"), UEdGraph::StaticClass(), UEdGraphSchema_K2::StaticClass());
		FBlueprintEditorUtils::AddFunctionGraph<UClass>(Blueprint, Function, true, nullptr);
		UEdGraphNode* Entry = FDuplicateFixture::FindEntry(Function);
		FGraphNodeCreator<UK2Node_DynamicCast> CastCreator(*Function);
		UK2Node_DynamicCast* Cast = CastCreator.CreateNode();
		Cast->TargetType = Blueprint->GeneratedClass;
		CastCreator.Finalize();
		FGraphNodeCreator<UK2Node_CallFunction> CallCreator(*Function);
		UK2Node_CallFunction* Call = CallCreator.CreateNode();
		Call->FunctionReference.SetExternalMember(
			GET_FUNCTION_NAME_CHECKED(UGameplayStatics, GetAllActorsOfClass), UGameplayStatics::StaticClass());
		CallCreator.Finalize();
		if (UEdGraphPin* ClassPin = Call->FindPin(TEXT("ActorClass"), EGPD_Input))
		{
			ClassPin->DefaultObject = Blueprint->GeneratedClass;
		}
		const UEdGraphSchema_K2* Schema = GetDefault<UEdGraphSchema_K2>();
		Schema->TryCreateConnection(Entry->FindPin(UEdGraphSchema_K2::PN_Then, EGPD_Output), Cast->FindPin(UEdGraphSchema_K2::PN_Execute, EGPD_Input));
		Schema->TryCreateConnection(Cast->FindPin(UEdGraphSchema_K2::PN_CastSucceeded, EGPD_Output), Call->FindPin(UEdGraphSchema_K2::PN_Execute, EGPD_Input));
		Schema->TryCreateConnection(Cast->GetCastResultPin(), Call->FindPin(TEXT("WorldContextObject"), EGPD_Input));
	}
	if (!TestTrue(TEXT("save A"), Fixture.SaveAsset(A)) || !TestTrue(TEXT("save B"), Fixture.SaveAsset(B)))
	{
		Fixture.Cleanup();
		return false;
	}
	Fixture.RegisterSavedFiles();

	const FCortexCommandResult Run = Fixture.Run(2, {});
	if (TestTrue(TEXT("run succeeds"), Run.bSuccess) && TestTrue(TEXT("data"), Run.Data.IsValid()))
	{
		const TArray<TSharedPtr<FJsonObject>> Exact = ObjectArray(Run.Data, TEXT("exact_groups"));
		if (TestEqual(TEXT("self-referencing copies are one exact group"), Exact.Num(), 1))
		{
			TestEqual(TEXT("members"), MemberLabels(Exact[0]), TArray<FString>{TEXT("BP_SelfA:Work"), TEXT("BP_SelfB:Work")});
		}
	}
	Fixture.Cleanup();
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexReflectDuplicateGraphPrecisionTest,
	"Cortex.Reflect.DuplicateGraphs.Precision",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexReflectDuplicateGraphPrecisionTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	using namespace CortexReflectDuplicateGraphTestSupport;

	// An unused parameter of another type must separate otherwise identical functions.
	{
		FDuplicateFixture Fixture;
		UBlueprint* A = Fixture.MakeBlueprint(TEXT("BP_PA"));
		UBlueprint* B = Fixture.MakeBlueprint(TEXT("BP_PB"));
		UBlueprint* C = Fixture.MakeBlueprint(TEXT("BP_PC"));
		if (!TestNotNull(TEXT("A"), A) || !TestNotNull(TEXT("B"), B) || !TestNotNull(TEXT("C"), C))
		{
			Fixture.Cleanup();
			return false;
		}
		FEdGraphPinType IntType;
		IntType.PinCategory = UEdGraphSchema_K2::PC_Int;
		FEdGraphPinType RealType;
		RealType.PinCategory = UEdGraphSchema_K2::PC_Real;
		RealType.PinSubCategory = UEdGraphSchema_K2::PC_Double;
		const TPair<UBlueprint*, FEdGraphPinType> Cases[] = {{A, IntType}, {B, RealType}, {C, IntType}};
		for (const TPair<UBlueprint*, FEdGraphPinType>& Case : Cases)
		{
			UEdGraph* Function = FDuplicateFixture::AddFunction(Case.Key, TEXT("Work"), FourTexts, 0, false);
			CastChecked<UK2Node_FunctionEntry>(FDuplicateFixture::FindEntry(Function))->CreateUserDefinedPin(TEXT("Unused"), Case.Value, EGPD_Output);
		}
		if (!TestTrue(TEXT("save A"), Fixture.SaveAsset(A)) || !TestTrue(TEXT("save B"), Fixture.SaveAsset(B)) || !TestTrue(TEXT("save C"), Fixture.SaveAsset(C)))
		{
			Fixture.Cleanup();
			return false;
		}
		Fixture.RegisterSavedFiles();
		const FCortexCommandResult Run = Fixture.Run({}, {});
		if (TestTrue(TEXT("param run succeeds"), Run.bSuccess) && TestTrue(TEXT("data"), Run.Data.IsValid()))
		{
			const TArray<TSharedPtr<FJsonObject>> Exact = ObjectArray(Run.Data, TEXT("exact_groups"));
			if (TestEqual(TEXT("one exact group: the int-parameter copies"), Exact.Num(), 1))
			{
				TestEqual(TEXT("members"), MemberLabels(Exact[0]), TArray<FString>{TEXT("BP_PA:Work"), TEXT("BP_PC:Work")});
			}
		}
		Fixture.Cleanup();
	}

	// Two graphs differing only in the enum of an enum literal are not duplicates.
	{
		FDuplicateFixture Fixture;
		UBlueprint* A = Fixture.MakeBlueprint(TEXT("BP_EA"));
		UBlueprint* B = Fixture.MakeBlueprint(TEXT("BP_EB"));
		UBlueprint* C = Fixture.MakeBlueprint(TEXT("BP_EC"));
		if (!TestNotNull(TEXT("A"), A) || !TestNotNull(TEXT("B"), B) || !TestNotNull(TEXT("C"), C))
		{
			Fixture.Cleanup();
			return false;
		}
		const TPair<UBlueprint*, UEnum*> Cases[] = {{A, StaticEnum<ECollisionChannel>()}, {B, StaticEnum<EMovementMode>()}, {C, StaticEnum<ECollisionChannel>()}};
		for (const TPair<UBlueprint*, UEnum*>& Case : Cases)
		{
			UEdGraph* Function = FDuplicateFixture::AddFunction(Case.Key, TEXT("Work"), FourTexts, 0, false);
			FGraphNodeCreator<UK2Node_EnumLiteral> Creator(*Function);
			UK2Node_EnumLiteral* Literal = Creator.CreateNode();
			Literal->Enum = Case.Value;
			Creator.Finalize();
		}
		if (!TestTrue(TEXT("save A"), Fixture.SaveAsset(A)) || !TestTrue(TEXT("save B"), Fixture.SaveAsset(B)) || !TestTrue(TEXT("save C"), Fixture.SaveAsset(C)))
		{
			Fixture.Cleanup();
			return false;
		}
		Fixture.RegisterSavedFiles();
		const FCortexCommandResult Run = Fixture.Run({}, {});
		if (TestTrue(TEXT("enum run succeeds"), Run.bSuccess) && TestTrue(TEXT("data"), Run.Data.IsValid()))
		{
			const TArray<TSharedPtr<FJsonObject>> Exact = ObjectArray(Run.Data, TEXT("exact_groups"));
			if (TestEqual(TEXT("one exact group: the same-enum copies"), Exact.Num(), 1))
			{
				TestEqual(TEXT("members"), MemberLabels(Exact[0]), TArray<FString>{TEXT("BP_EA:Work"), TEXT("BP_EC:Work")});
			}
		}
		Fixture.Cleanup();
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexReflectDuplicateGraphClusterTest,
	"Cortex.Reflect.DuplicateGraphs.SimilarClusters",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexReflectDuplicateGraphClusterTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	using namespace CortexReflectDuplicateGraphTestSupport;

	// A~B and B~C reach 0.8 but A~C does not: star cluster around B reporting the true min/max, and an exact copy
	// of A (A2) collapses into A's representative instead of appearing again.
	FDuplicateFixture Fixture;
	const TArray<FString> TextsA = {TEXT("T1"), TEXT("T2"), TEXT("T3"), TEXT("T4"), TEXT("T5"), TEXT("T6"), TEXT("T7"), TEXT("T8"), TEXT("T9"), TEXT("T10")};
	const TArray<FString> TextsB = {TEXT("T2"), TEXT("T3"), TEXT("T4"), TEXT("T5"), TEXT("T6"), TEXT("T7"), TEXT("T8"), TEXT("T9"), TEXT("T10"), TEXT("T11")};
	const TArray<FString> TextsC = {TEXT("T3"), TEXT("T4"), TEXT("T5"), TEXT("T6"), TEXT("T7"), TEXT("T8"), TEXT("T9"), TEXT("T10"), TEXT("T11"), TEXT("T12")};
	UBlueprint* A = Fixture.MakeBlueprint(TEXT("BP_A"));
	UBlueprint* A2 = Fixture.MakeBlueprint(TEXT("BP_A2"));
	UBlueprint* B = Fixture.MakeBlueprint(TEXT("BP_B"));
	UBlueprint* C = Fixture.MakeBlueprint(TEXT("BP_C"));
	if (!TestNotNull(TEXT("A"), A) || !TestNotNull(TEXT("A2"), A2) || !TestNotNull(TEXT("B"), B) || !TestNotNull(TEXT("C"), C))
	{
		Fixture.Cleanup();
		return false;
	}
	FDuplicateFixture::AddFunction(A, TEXT("Chain"), TextsA, 0, false);
	FDuplicateFixture::AddFunction(A2, TEXT("Chain"), TextsA, 500, false);
	FDuplicateFixture::AddFunction(B, TEXT("Chain"), TextsB, 0, false);
	FDuplicateFixture::AddFunction(C, TEXT("Chain"), TextsC, 0, false);
	if (!TestTrue(TEXT("save A"), Fixture.SaveAsset(A)) || !TestTrue(TEXT("save A2"), Fixture.SaveAsset(A2))
		|| !TestTrue(TEXT("save B"), Fixture.SaveAsset(B)) || !TestTrue(TEXT("save C"), Fixture.SaveAsset(C)))
	{
		Fixture.Cleanup();
		return false;
	}
	Fixture.RegisterSavedFiles();

	const FCortexCommandResult Run = Fixture.Run({}, 0.8);
	if (TestTrue(TEXT("run succeeds"), Run.bSuccess) && TestTrue(TEXT("data"), Run.Data.IsValid()))
	{
		const TArray<TSharedPtr<FJsonObject>> Exact = ObjectArray(Run.Data, TEXT("exact_groups"));
		if (TestEqual(TEXT("A and A2 are one exact group"), Exact.Num(), 1))
		{
			TestEqual(TEXT("exact members"), MemberLabels(Exact[0]), TArray<FString>{TEXT("BP_A2:Chain"), TEXT("BP_A:Chain")});
		}
		const TArray<TSharedPtr<FJsonObject>> Similar = ObjectArray(Run.Data, TEXT("similar_groups"));
		if (TestEqual(TEXT("one similar cluster"), Similar.Num(), 1))
		{
			TestEqual(TEXT("three members: representatives only, no exact duplicate"), ObjectArray(Similar[0], TEXT("members")).Num(), 3);
			TestTrue(TEXT("representative is B"), Similar[0]->GetStringField(TEXT("representative")).EndsWith(TEXT("/BP_B.BP_B")));
			TestEqual(TEXT("min_similarity is the true A-C ratio (9/13)"), Similar[0]->GetNumberField(TEXT("min_similarity")), 9.0 / 13.0, 0.001);
			TestEqual(TEXT("max_similarity is the A-B ratio (10/12)"), Similar[0]->GetNumberField(TEXT("max_similarity")), 10.0 / 12.0, 0.001);
		}
		const TArray<TSharedPtr<FJsonObject>> Pairs = ObjectArray(Run.Data, TEXT("blueprint_pairs"));
		const TSharedPtr<FJsonObject> AA2 = FindPair(Pairs, TEXT("BP_A"), TEXT("BP_A2"));
		if (TestNotNull(TEXT("A-A2 pair"), AA2.Get()))
		{
			TestEqual(TEXT("the unit pair counts once"), static_cast<int32>(AA2->GetNumberField(TEXT("shared_graphs"))), 1);
		}
		const TSharedPtr<FJsonObject> A2B = FindPair(Pairs, TEXT("BP_A2"), TEXT("BP_B"));
		if (TestNotNull(TEXT("A2-B pair (exact copy shares the similarity edge)"), A2B.Get()))
		{
			TestEqual(TEXT("A2-B shared"), static_cast<int32>(A2B->GetNumberField(TEXT("shared_graphs"))), 1);
		}
		TestNull(TEXT("A-C is not a linked pair"), FindPair(Pairs, TEXT("BP_A"), TEXT("BP_C")).Get());
	}
	Fixture.Cleanup();
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexReflectDuplicateGraphLooseTest,
	"Cortex.Reflect.DuplicateGraphs.SiblingClasses",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexReflectDuplicateGraphLooseTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	using namespace CortexReflectDuplicateGraphTestSupport;

	// Parallel sibling child widgets A1/A2 (same native parent). Host H1 uses A1, H2 uses A2, with identical widget
	// names and a function that casts to its child class and calls a member of it: strict forms differ, the loose
	// forms (native ancestor class instead of the sibling class) are identical.
	FDuplicateFixture Fixture;
	UBlueprint* A1 = Fixture.MakeWidgetBlueprint(TEXT("WBP_ChildA1"), {});
	UBlueprint* A2 = Fixture.MakeWidgetBlueprint(TEXT("WBP_ChildA2"), {});
	if (!TestNotNull(TEXT("A1"), A1) || !TestNotNull(TEXT("A2"), A2))
	{
		Fixture.Cleanup();
		return false;
	}
	for (UBlueprint* Child : {A1, A2})
	{
		UEdGraph* Ping = FBlueprintEditorUtils::CreateNewGraph(
			Child, TEXT("Ping"), UEdGraph::StaticClass(), UEdGraphSchema_K2::StaticClass());
		FBlueprintEditorUtils::AddFunctionGraph<UClass>(Child, Ping, true, nullptr);
		FKismetEditorUtilities::CompileBlueprint(Child);
	}
	if (!TestNotNull(TEXT("A1 class"), A1->GeneratedClass.Get()) || !TestNotNull(TEXT("A2 class"), A2->GeneratedClass.Get()))
	{
		Fixture.Cleanup();
		return false;
	}
	UBlueprint* Hosts[2] = {Fixture.MakeWidgetBlueprint(TEXT("WBP_HostH1"), {}), Fixture.MakeWidgetBlueprint(TEXT("WBP_HostH2"), {})};
	UBlueprint* Children[2] = {A1, A2};
	for (int32 Slot = 0; Slot < 2; ++Slot)
	{
		UBlueprint* Host = Hosts[Slot];
		if (!TestNotNull(TEXT("host"), Host))
		{
			Fixture.Cleanup();
			return false;
		}
		UClass* ChildClass = Children[Slot]->GeneratedClass;
		UWidgetTree* Tree = FDuplicateFixture::GetWidgetTree(Host);
		UCanvasPanel* Root = Tree->ConstructWidget<UCanvasPanel>(UCanvasPanel::StaticClass(), TEXT("Root"));
		Tree->RootWidget = Root;
		for (const TCHAR* Name : {TEXT("Btn1"), TEXT("Btn2"), TEXT("Btn3")})
		{
			Root->AddChild(Tree->ConstructWidget<UUserWidget>(ChildClass, Name));
		}
		Root->AddChild(Tree->ConstructWidget<UTextBlock>(UTextBlock::StaticClass(), TEXT("Label")));

		UEdGraph* Work = FDuplicateFixture::AddFunction(Host, TEXT("Work"), FourTexts, 0, false);
		FGraphNodeCreator<UK2Node_DynamicCast> CastCreator(*Work);
		UK2Node_DynamicCast* Cast = CastCreator.CreateNode();
		Cast->TargetType = ChildClass;
		CastCreator.Finalize();
		FGraphNodeCreator<UK2Node_CallFunction> CallCreator(*Work);
		UK2Node_CallFunction* Call = CallCreator.CreateNode();
		Call->FunctionReference.SetExternalMember(TEXT("Ping"), ChildClass);
		CallCreator.Finalize();
	}
	if (!TestTrue(TEXT("save A1"), Fixture.SaveAsset(A1)) || !TestTrue(TEXT("save A2"), Fixture.SaveAsset(A2))
		|| !TestTrue(TEXT("save H1"), Fixture.SaveAsset(Hosts[0])) || !TestTrue(TEXT("save H2"), Fixture.SaveAsset(Hosts[1])))
	{
		Fixture.Cleanup();
		return false;
	}
	Fixture.RegisterSavedFiles();

	const FCortexCommandResult Run = Fixture.Run({}, {});
	if (TestTrue(TEXT("run succeeds"), Run.bSuccess) && TestTrue(TEXT("data"), Run.Data.IsValid()))
	{
		TestEqual(TEXT("sibling-class copies are not exact"), ObjectArray(Run.Data, TEXT("exact_groups")).Num(), 0);
		const TArray<TSharedPtr<FJsonObject>> Similar = ObjectArray(Run.Data, TEXT("similar_groups"));
		const TSharedPtr<FJsonObject> Function = FindGroup(Similar, TEXT("function"));
		if (TestNotNull(TEXT("function similar group"), Function.Get()))
		{
			TestEqual(TEXT("function members"), MemberLabels(Function), TArray<FString>{TEXT("WBP_HostH1:Work"), TEXT("WBP_HostH2:Work")});
			TestEqual(TEXT("loose marker"), Function->GetBoolField(TEXT("loose")), true);
			TestEqual(TEXT("loose forms are identical"), Function->GetNumberField(TEXT("min_similarity")), 1.0, 0.0001);
		}
		const TArray<TSharedPtr<FJsonObject>> Pairs = ObjectArray(Run.Data, TEXT("blueprint_pairs"));
		const TSharedPtr<FJsonObject> HostPair = FindPair(Pairs, TEXT("WBP_HostH1"), TEXT("WBP_HostH2"));
		if (TestNotNull(TEXT("host pair"), HostPair.Get()))
		{
			TestEqual(TEXT("widget_tree_similarity ignores the sibling class"), HostPair->GetNumberField(TEXT("widget_tree_similarity")), 1.0, 0.0001);
		}
	}
	Fixture.Cleanup();
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexReflectDuplicateGraphStrictWidgetClassTest,
	"Cortex.Reflect.DuplicateGraphs.StrictWidgetClassIdentity",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexReflectDuplicateGraphStrictWidgetClassTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	using namespace CortexReflectDuplicateGraphTestSupport;

	// Two different child widget classes share the short name WBP_Row (/FeatureA and /FeatureB). Hosts H1 and H3 place
	// the FeatureA one, H2 the FeatureB one, with identical widget names: only H1 and H3 are strictly identical.
	FDuplicateFixture Fixture;
	UBlueprint* RowA = Fixture.MakeWidgetBlueprint(TEXT("WBP_Row"), {}, TEXT("/FeatureA"));
	UBlueprint* RowB = Fixture.MakeWidgetBlueprint(TEXT("WBP_Row"), {}, TEXT("/FeatureB"));
	if (!TestNotNull(TEXT("row A"), RowA) || !TestNotNull(TEXT("row B"), RowB))
	{
		Fixture.Cleanup();
		return false;
	}
	FKismetEditorUtilities::CompileBlueprint(RowA);
	FKismetEditorUtilities::CompileBlueprint(RowB);
	if (!TestNotNull(TEXT("row A class"), RowA->GeneratedClass.Get()) || !TestNotNull(TEXT("row B class"), RowB->GeneratedClass.Get()))
	{
		Fixture.Cleanup();
		return false;
	}
	TestNotEqual(TEXT("the two row classes share a short name but not a path"), RowA->GeneratedClass->GetPathName(), RowB->GeneratedClass->GetPathName());
	TestEqual(TEXT("the two row classes share a short name"), RowA->GeneratedClass->GetName(), RowB->GeneratedClass->GetName());

	UBlueprint* Hosts[3] = {
		Fixture.MakeWidgetBlueprint(TEXT("WBP_HostH1"), {}), Fixture.MakeWidgetBlueprint(TEXT("WBP_HostH2"), {}),
		Fixture.MakeWidgetBlueprint(TEXT("WBP_HostH3"), {})};
	UBlueprint* RowFor[3] = {RowA, RowB, RowA};
	for (int32 Slot = 0; Slot < 3; ++Slot)
	{
		if (!TestNotNull(TEXT("host"), Hosts[Slot]))
		{
			Fixture.Cleanup();
			return false;
		}
		UWidgetTree* Tree = FDuplicateFixture::GetWidgetTree(Hosts[Slot]);
		UPanelWidget* Root = Cast<UPanelWidget>(Tree->RootWidget);
		UClass* RowClass = RowFor[Slot]->GeneratedClass;
		for (const TCHAR* Name : {TEXT("Row1"), TEXT("Row2"), TEXT("Row3")})
		{
			Root->AddChild(Tree->ConstructWidget<UUserWidget>(RowClass, Name));
		}
		Root->AddChild(Tree->ConstructWidget<UTextBlock>(UTextBlock::StaticClass(), TEXT("Label")));
	}
	if (!TestTrue(TEXT("save row A"), Fixture.SaveAsset(RowA)) || !TestTrue(TEXT("save row B"), Fixture.SaveAsset(RowB))
		|| !TestTrue(TEXT("save H1"), Fixture.SaveAsset(Hosts[0])) || !TestTrue(TEXT("save H2"), Fixture.SaveAsset(Hosts[1]))
		|| !TestTrue(TEXT("save H3"), Fixture.SaveAsset(Hosts[2])))
	{
		Fixture.Cleanup();
		return false;
	}
	Fixture.RegisterSavedFiles();

	const FCortexCommandResult Run = Fixture.Run({}, {});
	if (TestTrue(TEXT("run succeeds"), Run.bSuccess) && TestTrue(TEXT("data"), Run.Data.IsValid()))
	{
		const TSharedPtr<FJsonObject> Group = FindGroup(ObjectArray(Run.Data, TEXT("exact_groups")), TEXT("widget_tree"));
		if (TestNotNull(TEXT("H1 and H3 are an exact widget_tree group"), Group.Get()))
		{
			const TArray<FString> Labels = MemberLabels(Group);
			TestEqual(TEXT("exactly the two hosts that place the same row class"), Labels.Num(), 2);
			const auto Has = [&Labels](const TCHAR* Host)
			{
				return Labels.ContainsByPredicate([Host](const FString& Label) { return Label.StartsWith(Host); });
			};
			TestTrue(TEXT("H1 grouped"), Has(TEXT("WBP_HostH1")));
			TestTrue(TEXT("H3 grouped"), Has(TEXT("WBP_HostH3")));
			TestFalse(TEXT("H2 places a different class of the same short name"), Has(TEXT("WBP_HostH2")));
		}
	}
	Fixture.Cleanup();
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexReflectDuplicateGraphParamsTest,
	"Cortex.Reflect.DuplicateGraphs.Params",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexReflectDuplicateGraphParamsTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	using namespace CortexReflectDuplicateGraphTestSupport;

	FCortexReflectCommandHandler Handler;
	const FString EmptyFolder = FString::Printf(TEXT("/Game/Temp/Cortex120/Duplicate/None_%s"), *FGuid::NewGuid().ToString(EGuidFormats::Digits));

	const auto Refused = [&](const TCHAR* Label, const TFunction<void(const TSharedPtr<FJsonObject>&)>& Mutate)
	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetArrayField(TEXT("path_prefixes"), JsonStrings({EmptyFolder}));
		Mutate(Params);
		const FCortexCommandResult Result = Handler.Execute(TEXT("find_duplicate_graphs"), Params);
		TestFalse(*FString::Printf(TEXT("%s is refused"), Label), Result.bSuccess);
		TestEqual(*FString::Printf(TEXT("%s code"), Label), Result.ErrorCode, FString(CortexErrorCodes::InvalidField));
	};

	Refused(TEXT("missing path_prefixes"), [](const TSharedPtr<FJsonObject>& Params) { Params->RemoveField(TEXT("path_prefixes")); });
	Refused(TEXT("empty path_prefixes"), [](const TSharedPtr<FJsonObject>& Params) { Params->SetArrayField(TEXT("path_prefixes"), {}); });
	Refused(TEXT("non-folder prefix"), [](const TSharedPtr<FJsonObject>& Params) { Params->SetArrayField(TEXT("path_prefixes"), JsonStrings({TEXT("/Game/../Escape")})); });
	Refused(TEXT("min_nodes 0"), [](const TSharedPtr<FJsonObject>& Params) { Params->SetNumberField(TEXT("min_nodes"), 0); });
	Refused(TEXT("negative min_nodes"), [](const TSharedPtr<FJsonObject>& Params) { Params->SetNumberField(TEXT("min_nodes"), -3); });
	Refused(TEXT("fractional min_nodes"), [](const TSharedPtr<FJsonObject>& Params) { Params->SetNumberField(TEXT("min_nodes"), 2.5); });
	Refused(TEXT("string min_nodes"), [](const TSharedPtr<FJsonObject>& Params) { Params->SetStringField(TEXT("min_nodes"), TEXT("five")); });
	Refused(TEXT("similarity 0"), [](const TSharedPtr<FJsonObject>& Params) { Params->SetNumberField(TEXT("similarity"), 0.0); });
	Refused(TEXT("similarity above 1"), [](const TSharedPtr<FJsonObject>& Params) { Params->SetNumberField(TEXT("similarity"), 1.1); });
	Refused(TEXT("string similarity"), [](const TSharedPtr<FJsonObject>& Params) { Params->SetStringField(TEXT("similarity"), TEXT("high")); });

	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetArrayField(TEXT("path_prefixes"), JsonStrings({EmptyFolder}));
		Params->SetNumberField(TEXT("min_nodes"), 1);
		Params->SetNumberField(TEXT("similarity"), 1.0);
		const FCortexCommandResult Ok = Handler.Execute(TEXT("find_duplicate_graphs"), Params);
		if (TestTrue(TEXT("boundary values are accepted"), Ok.bSuccess) && TestTrue(TEXT("data"), Ok.Data.IsValid()))
		{
			TestTrue(TEXT("exact_groups present"), Ok.Data->HasTypedField<EJson::Array>(TEXT("exact_groups")));
			TestTrue(TEXT("similar_groups present"), Ok.Data->HasTypedField<EJson::Array>(TEXT("similar_groups")));
			TestTrue(TEXT("blueprint_pairs present"), Ok.Data->HasTypedField<EJson::Array>(TEXT("blueprint_pairs")));
		}
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexReflectDuplicateGraphOutPathTest,
	"Cortex.Reflect.DuplicateGraphs.OutPath",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexReflectDuplicateGraphOutPathTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	using namespace CortexReflectDuplicateGraphTestSupport;

	FCortexReflectCommandHandler Handler;

	// Unknown parameters are refused (StrictParams) rather than silently ignored.
	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetArrayField(TEXT("path_prefixes"), JsonStrings({TEXT("/Game/Temp/Cortex120/Duplicate/NoSuchFolder")}));
		Params->SetNumberField(TEXT("min_node"), 3);
		const FCortexCommandResult Typo = Handler.Execute(TEXT("find_duplicate_graphs"), Params);
		TestFalse(TEXT("misspelled min_node is refused"), Typo.bSuccess);
		TestEqual(TEXT("misspelled param code"), Typo.ErrorCode, FString(CortexErrorCodes::InvalidField));
	}

	// An unusable out_path is refused before any Blueprint is touched; nothing is written.
	{
		const FString Forbidden = FPaths::ConvertRelativePathToFull(FPaths::Combine(FPaths::ProjectDir(), TEXT("Source"), TEXT("Cortex120DuplicateRefused.json")));
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetArrayField(TEXT("path_prefixes"), JsonStrings({TEXT("/Game/Temp/Cortex120/Duplicate/NoSuchFolder")}));
		Params->SetStringField(TEXT("out_path"), Forbidden);
		const FCortexCommandResult Refused = Handler.Execute(TEXT("find_duplicate_graphs"), Params);
		TestFalse(TEXT("out_path outside the write policy is refused"), Refused.bSuccess);
		TestFalse(TEXT("refused out_path was not written"), IFileManager::Get().FileExists(*Forbidden));
	}

	// A prefix that matches nothing is reported, not silently accepted.
	{
		const FString Missing = TEXT("/Game/Temp/Cortex120/Duplicate/NoSuchFolder");
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetArrayField(TEXT("path_prefixes"), JsonStrings({Missing}));
		const FCortexCommandResult Empty = Handler.Execute(TEXT("find_duplicate_graphs"), Params);
		if (TestTrue(TEXT("empty prefix succeeds"), Empty.bSuccess) && TestTrue(TEXT("data"), Empty.Data.IsValid()))
		{
			const TSharedPtr<FJsonObject>* Matched = nullptr;
			if (TestTrue(TEXT("matched_assets present"), Empty.Data->TryGetObjectField(TEXT("matched_assets"), Matched)))
			{
				TestEqual(TEXT("prefix matched zero assets"), static_cast<int32>((*Matched)->GetNumberField(Missing)), 0);
			}
			const TArray<TSharedPtr<FJsonValue>>* WarningValues = nullptr;
			if (TestTrue(TEXT("warnings present"), Empty.Data->TryGetArrayField(TEXT("warnings"), WarningValues)))
			{
				TestEqual(TEXT("one warning for the empty prefix"), WarningValues->Num(), 1);
			}
		}
	}

	// With out_path the full result goes to the file and the response is a compact summary.
	FDuplicateFixture Fixture;
	UBlueprint* A = Fixture.MakeBlueprint(TEXT("BP_A"));
	UBlueprint* B = Fixture.MakeBlueprint(TEXT("BP_B"));
	if (!TestNotNull(TEXT("A"), A) || !TestNotNull(TEXT("B"), B))
	{
		Fixture.Cleanup();
		return false;
	}
	FDuplicateFixture::AddFunction(A, TEXT("Compute"), FourTexts, 0, false);
	FDuplicateFixture::AddFunction(B, TEXT("Compute"), FourTexts, 600, false);
	if (!TestTrue(TEXT("save A"), Fixture.SaveAsset(A)) || !TestTrue(TEXT("save B"), Fixture.SaveAsset(B)))
	{
		Fixture.Cleanup();
		return false;
	}
	Fixture.RegisterSavedFiles();

	const FString OutPath = FPaths::ConvertRelativePathToFull(
		FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("Cortex120"), FString::Printf(TEXT("Duplicate_%s.json"), *Fixture.Suffix)));
	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetArrayField(TEXT("path_prefixes"), JsonStrings({Fixture.Dir}));
	Params->SetStringField(TEXT("out_path"), OutPath);
	const FCortexCommandResult Result = Handler.Execute(TEXT("find_duplicate_graphs"), Params);
	if (TestTrue(*FString::Printf(TEXT("out_path run succeeds (%s)"), *Result.ErrorMessage), Result.bSuccess) && TestTrue(TEXT("data"), Result.Data.IsValid()))
	{
		TestEqual(TEXT("response names the file"), Result.Data->GetStringField(TEXT("out_path")), OutPath);
		TestEqual(TEXT("summary counts the exact groups"), static_cast<int32>(Result.Data->GetNumberField(TEXT("exact_groups_total"))), 1);
		TestTrue(TEXT("summary keeps at most the first 10 groups"), ObjectArray(Result.Data, TEXT("exact_groups")).Num() <= 10);
		TestTrue(TEXT("summary counts the pairs"), Result.Data->HasField(TEXT("blueprint_pairs_total")));
		const TSharedPtr<FJsonObject>* Matched = nullptr;
		if (TestTrue(TEXT("matched_assets in the summary"), Result.Data->TryGetObjectField(TEXT("matched_assets"), Matched)))
		{
			TestEqual(TEXT("two Blueprints matched the prefix"), static_cast<int32>((*Matched)->GetNumberField(Fixture.Dir)), 2);
		}

		FString Text;
		TSharedPtr<FJsonObject> File;
		if (TestTrue(TEXT("report file is written"), FFileHelper::LoadFileToString(Text, *OutPath))
			&& TestTrue(TEXT("report parses"), FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Text), File) && File.IsValid()))
		{
			TestEqual(TEXT("file holds every exact group"), ObjectArray(File, TEXT("exact_groups")).Num(), 1);
			TestFalse(TEXT("file is the full result, not a summary"), File->HasField(TEXT("out_path")));
		}
	}
	IFileManager::Get().Delete(*OutPath);
	Fixture.Cleanup();
	return true;
}
