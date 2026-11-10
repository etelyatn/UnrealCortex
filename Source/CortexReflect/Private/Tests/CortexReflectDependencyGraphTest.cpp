// reflect.export_dependency_graph tests. Fixtures are saved assets under /Game/Temp/Cortex120/Graph/<guid> so the
// Asset Registry sees real tags and dependencies; report files live under Saved/Cortex120.

#include "Misc/AutomationTest.h"
#include "CortexReflectCommandHandler.h"
#include "CortexSafeFileContract.h"
#include "CortexTypes.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "Blueprint/UserWidget.h"
#include "Blueprint/WidgetTree.h"
#include "Components/CanvasPanel.h"
#include "Components/Widget.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "EdGraph/EdGraph.h"
#include "EdGraphSchema_K2.h"
#include "Engine/Blueprint.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "Engine/UserDefinedEnum.h"
#include "StructUtils/UserDefinedStruct.h"
#include "Engine/Level.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "HAL/FileManager.h"
#include "Kismet/KismetSystemLibrary.h"
#include "Operations/CortexReflectDependencyGraphOps.h"
#include "K2Node_DynamicCast.h"
#include "K2Node_Message.h"
#include "K2Node_SpawnActorFromClass.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/EnumEditorUtils.h"
#include "K2Node_CallFunction.h"
#include "K2Node_ConstructObjectFromClass.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Kismet2/StructureEditorUtils.h"
#include "Misc/FileHelper.h"
#include "Misc/Guid.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "UObject/GarbageCollection.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"
#include "UObject/UObjectIterator.h"

namespace CortexReflectDependencyGraphTestSupport
{
	TSharedPtr<FJsonObject> ReadJsonDocument(const FString& Path)
	{
		FString Text;
		TSharedPtr<FJsonObject> Root;
		if (!FFileHelper::LoadFileToString(Text, *Path))
		{
			return nullptr;
		}
		const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Text);
		return FJsonSerializer::Deserialize(Reader, Root) ? Root : nullptr;
	}

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

	/** Ids of every group in a waves/cycles style array of string arrays. */
	TArray<TArray<FString>> StringGroups(const TSharedPtr<FJsonObject>& Object, const TCHAR* Field)
	{
		TArray<TArray<FString>> Groups;
		const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
		if (Object.IsValid() && Object->TryGetArrayField(Field, Values))
		{
			for (const TSharedPtr<FJsonValue>& Value : *Values)
			{
				TArray<FString>& Group = Groups.AddDefaulted_GetRef();
				for (const TSharedPtr<FJsonValue>& Item : Value->AsArray())
				{
					Group.Add(Item->AsString());
				}
			}
		}
		return Groups;
	}

	bool HasEdge(const TSharedPtr<FJsonObject>& Report, const FString& From, const FString& To, const FString& Kind)
	{
		for (const TSharedPtr<FJsonObject>& Edge : ObjectArray(Report, TEXT("edges")))
		{
			if (Edge->GetStringField(TEXT("from")) == From && Edge->GetStringField(TEXT("to")) == To
				&& Edge->GetStringField(TEXT("kind")) == Kind)
			{
				return true;
			}
		}
		return false;
	}

	TSharedPtr<FJsonObject> FindNode(const TSharedPtr<FJsonObject>& Report, const FString& Id)
	{
		for (const TSharedPtr<FJsonObject>& Node : ObjectArray(Report, TEXT("nodes")))
		{
			if (Node->GetStringField(TEXT("id")) == Id)
			{
				return Node;
			}
		}
		return nullptr;
	}

	/** Index of the wave holding Id, or INDEX_NONE. */
	int32 WaveOf(const TArray<TArray<FString>>& Waves, const FString& Id)
	{
		for (int32 Index = 0; Index < Waves.Num(); ++Index)
		{
			if (Waves[Index].Contains(Id))
			{
				return Index;
			}
		}
		return INDEX_NONE;
	}

	int32 EdgesOfKind(const TSharedPtr<FJsonObject>& Data, const TCHAR* Kind)
	{
		const TSharedPtr<FJsonObject>* ByKind = nullptr;
		double Count = 0.0;
		if (Data.IsValid() && Data->TryGetObjectField(TEXT("edges_by_kind"), ByKind))
		{
			(*ByKind)->TryGetNumberField(Kind, Count);
		}
		return static_cast<int32>(Count);
	}

	FCortexCommandResult Run(const TSharedPtr<FJsonObject>& Params)
	{
		FCortexReflectCommandHandler Handler;
		return Handler.Execute(TEXT("export_dependency_graph"), Params);
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

	/** Ini in the project Config dir, removed on scope exit. */
	struct FTempIni
	{
		FString IniName;
		FString IniPath;
		bool bWritten = false;

		explicit FTempIni(const FString& Body)
		{
			IniName = FString::Printf(TEXT("CortexTest120_Graph_%s.ini"), *FGuid::NewGuid().ToString(EGuidFormats::Digits));
			IniPath = FPaths::Combine(FPaths::ProjectConfigDir(), IniName);
			bWritten = FFileHelper::SaveStringToFile(Body, *IniPath);
		}

		~FTempIni()
		{
			IFileManager::Get().Delete(*IniPath, false, true, true);
		}
	};

	/** Saved assets under /Game/Temp/Cortex120/Graph/<guid>, plus the report files a test asks for. */
	struct FGraphFixture
	{
		FString Suffix = FGuid::NewGuid().ToString(EGuidFormats::Digits);
		FString Dir = FString::Printf(TEXT("/Game/Temp/Cortex120/Graph/%s"), *Suffix);
		TArray<FString> SavedFiles;
		TArray<FString> ReportFiles;

		bool SaveAsset(UObject* Asset, const TCHAR* Extension)
		{
			UPackage* Package = Asset->GetOutermost();
			FString Filename;
			if (!FPackageName::TryConvertLongPackageNameToFilename(Package->GetName(), Filename, Extension))
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

		/** Compiles, optionally. A Blueprint holding nodes is saved without compiling: the nodes are unconnected. */
		UBlueprint* MakeBlueprint(
			const FString& Name, UClass* ParentClass, const bool bCompile = true,
			const EBlueprintType Type = BPTYPE_Normal, UClass* BlueprintClass = nullptr, UClass* GeneratedClass = nullptr)
		{
			UPackage* Package = CreatePackage(*FString::Printf(TEXT("%s/%s"), *Dir, *Name));
			UBlueprint* Blueprint = FKismetEditorUtilities::CreateBlueprint(
				ParentClass, Package, FName(*Name), Type, BlueprintClass ? BlueprintClass : UBlueprint::StaticClass(),
				GeneratedClass ? GeneratedClass : UBlueprintGeneratedClass::StaticClass(), NAME_None);
			if (Blueprint && bCompile)
			{
				FKismetEditorUtilities::CompileBlueprint(Blueprint);
			}
			return Blueprint;
		}

		UBlueprint* MakeWidgetBlueprint(const FString& Name, const bool bCompile)
		{
			UClass* WidgetBlueprintClass = FindObject<UClass>(nullptr, TEXT("/Script/UMGEditor.WidgetBlueprint"));
			UClass* WidgetGeneratedClass = FindObject<UClass>(nullptr, TEXT("/Script/UMG.WidgetBlueprintGeneratedClass"));
			if (!WidgetBlueprintClass || !WidgetGeneratedClass)
			{
				return nullptr;
			}
			UBlueprint* Blueprint = MakeBlueprint(Name, UUserWidget::StaticClass(), bCompile, BPTYPE_Normal, WidgetBlueprintClass, WidgetGeneratedClass);
			if (Blueprint && !GetWidgetTree(Blueprint))
			{
				const FObjectProperty* Property = CastField<FObjectProperty>(Blueprint->GetClass()->FindPropertyByName(TEXT("WidgetTree")));
				if (Property)
				{
					Property->SetObjectPropertyValue_InContainer(
						Blueprint, NewObject<UWidgetTree>(Blueprint, UWidgetTree::StaticClass(), TEXT("WidgetTree"), RF_Transactional));
				}
			}
			return Blueprint;
		}

		static UWidgetTree* GetWidgetTree(UBlueprint* WidgetBlueprint)
		{
			const FObjectProperty* Property = CastField<FObjectProperty>(WidgetBlueprint->GetClass()->FindPropertyByName(TEXT("WidgetTree")));
			return Property ? Cast<UWidgetTree>(Property->GetObjectPropertyValue_InContainer(WidgetBlueprint)) : nullptr;
		}

		static UEdGraph* EventGraph(UBlueprint* Blueprint)
		{
			return Blueprint->UbergraphPages.Num() > 0 ? Blueprint->UbergraphPages[0] : nullptr;
		}

		static void AddCast(UBlueprint* Blueprint, UClass* Target)
		{
			FGraphNodeCreator<UK2Node_DynamicCast> Creator(*EventGraph(Blueprint));
			UK2Node_DynamicCast* Node = Creator.CreateNode();
			Node->TargetType = Target;
			Creator.Finalize();
		}

		static void AddSpawn(UBlueprint* Blueprint, UClass* SpawnClass)
		{
			FGraphNodeCreator<UK2Node_SpawnActorFromClass> Creator(*EventGraph(Blueprint));
			UK2Node_SpawnActorFromClass* Node = Creator.CreateNode();
			Node->AllocateDefaultPins(); // PostPlacedNewNode reads the scale-method pin
			Creator.Finalize();
			Node->GetClassPin()->DefaultObject = SpawnClass;
		}

		static void AddMessage(UEdGraph* Graph, const FName FunctionName, UClass* InterfaceClass)
		{
			FGraphNodeCreator<UK2Node_Message> Creator(*Graph);
			UK2Node_Message* Node = Creator.CreateNode();
			Node->FunctionReference.SetExternalMember(FunctionName, InterfaceClass);
			Creator.Finalize();
		}

		static bool AddCreateWidget(UBlueprint* Blueprint, UClass* WidgetClass)
		{
			UClass* NodeClass = FindObject<UClass>(nullptr, TEXT("/Script/UMGEditor.K2Node_CreateWidget"));
			if (!NodeClass)
			{
				return false;
			}
			UEdGraph* Graph = EventGraph(Blueprint);
			UK2Node_ConstructObjectFromClass* Node = NewObject<UK2Node_ConstructObjectFromClass>(Graph, NodeClass);
			Graph->AddNode(Node, false, false);
			Node->CreateNewGuid();
			Node->AllocateDefaultPins();
			Node->PostPlacedNewNode();
			UEdGraphPin* ClassPin = Node->GetClassPin();
			if (!ClassPin)
			{
				return false;
			}
			ClassPin->DefaultObject = WidgetClass;
			return true;
		}

		/** Report file under Saved/Cortex120. */
		FString ReportPath(const FString& Label)
		{
			const FString Path = FPaths::ConvertRelativePathToFull(FPaths::Combine(
				FPaths::ProjectSavedDir(), TEXT("Cortex120"), FString::Printf(TEXT("Graph_%s_%s.json"), *Label, *Suffix)));
			ReportFiles.Add(Path);
			return Path;
		}

		TSharedPtr<FJsonObject> ExportParams(const FString& OutPath) const
		{
			TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
			Params->SetArrayField(TEXT("path_prefixes"), JsonStrings({Dir}));
			Params->SetStringField(TEXT("out_path"), OutPath);
			return Params;
		}

		void RegisterSavedFiles() const
		{
			IAssetRegistry::GetChecked().ScanModifiedAssetFiles(SavedFiles);
		}

		void Cleanup()
		{
			// Pending scans of the saved files must land before the assets are deleted from the registry.
			IAssetRegistry::GetChecked().WaitForCompletion();
			for (const FString& File : ReportFiles)
			{
				IFileManager::Get().Delete(*File);
			}
			TArray<UWorld*> Worlds;
			for (TObjectIterator<UWorld> It; It; ++It)
			{
				if (It->GetOutermost()->GetName().StartsWith(Dir))
				{
					Worlds.Add(*It);
				}
			}
			for (UWorld* World : Worlds)
			{
				World->DestroyWorld(false);
			}
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
				*FPaths::Combine(FPaths::ProjectContentDir(), TEXT("Temp"), TEXT("Cortex120"), TEXT("Graph"), Suffix), false, true);
		}
	};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexReflectDependencyGraphEdgesTest,
	"Cortex.Reflect.DependencyGraph.Edges",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexReflectDependencyGraphEdgesTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	using namespace CortexReflectDependencyGraphTestSupport;
	FGraphFixture Fixture;

	// Targets first (compiled), then the Blueprints that carry nodes (saved without compiling).
	UBlueprint* Interface = Fixture.MakeBlueprint(TEXT("BPI_Ping"), UInterface::StaticClass(), false, BPTYPE_Interface);
	UBlueprint* Base = Fixture.MakeBlueprint(TEXT("BP_Base"), AActor::StaticClass());
	UBlueprint* LeafWidget = Fixture.MakeWidgetBlueprint(TEXT("WBP_Leaf"), true);
	UBlueprint* EntryWidget = Fixture.MakeWidgetBlueprint(TEXT("WBP_Entry"), true);
	if (!TestNotNull(TEXT("interface"), Interface) || !TestNotNull(TEXT("base"), Base)
		|| !TestNotNull(TEXT("leaf widget"), LeafWidget) || !TestNotNull(TEXT("entry widget"), EntryWidget))
	{
		Fixture.Cleanup();
		return false;
	}

	UEdGraph* PingGraph = FBlueprintEditorUtils::CreateNewGraph(
		Interface, TEXT("Ping"), UEdGraph::StaticClass(), UEdGraphSchema_K2::StaticClass());
	FBlueprintEditorUtils::AddFunctionGraph<UClass>(Interface, PingGraph, true, nullptr);
	FKismetEditorUtilities::CompileBlueprint(Interface);
	UClass* InterfaceClass = Interface->GeneratedClass;
	UClass* BaseClass = Base->GeneratedClass;
	UClass* LeafClass = LeafWidget->GeneratedClass;
	if (!TestNotNull(TEXT("interface class"), InterfaceClass) || !TestNotNull(TEXT("base class"), BaseClass)
		|| !TestNotNull(TEXT("leaf class"), LeafClass) || !TestNotNull(TEXT("entry class"), EntryWidget->GeneratedClass.Get()))
	{
		Fixture.Cleanup();
		return false;
	}

	UBlueprint* Child = Fixture.MakeBlueprint(TEXT("BP_Child"), BaseClass);
	UBlueprint* Implementer = Fixture.MakeBlueprint(TEXT("BP_Impl"), AActor::StaticClass(), false);
	UBlueprint* CycleA = Fixture.MakeBlueprint(TEXT("BP_CycleA"), AActor::StaticClass());
	UBlueprint* CycleB = Fixture.MakeBlueprint(TEXT("BP_CycleB"), AActor::StaticClass());
	UBlueprint* Caller = Fixture.MakeBlueprint(TEXT("BP_Caller"), AActor::StaticClass());
	UBlueprint* Host = Fixture.MakeWidgetBlueprint(TEXT("WBP_Host"), false);
	UBlueprint* Downstream = Fixture.MakeBlueprint(TEXT("BP_Downstream"), AActor::StaticClass(), false);
	if (!TestNotNull(TEXT("child"), Child) || !TestNotNull(TEXT("implementer"), Implementer)
		|| !TestNotNull(TEXT("cycle a"), CycleA) || !TestNotNull(TEXT("cycle b"), CycleB)
		|| !TestNotNull(TEXT("caller"), Caller) || !TestNotNull(TEXT("host widget"), Host) || !TestNotNull(TEXT("downstream"), Downstream))
	{
		Fixture.Cleanup();
		return false;
	}

	TestTrue(TEXT("implement interface"),
		FBlueprintEditorUtils::ImplementNewInterface(Implementer, InterfaceClass->GetClassPathName()));
	FKismetEditorUtilities::CompileBlueprint(Implementer);

	UEdGraph* CallPing = FBlueprintEditorUtils::CreateNewGraph(
		Caller, TEXT("CallPing"), UEdGraph::StaticClass(), UEdGraphSchema_K2::StaticClass());
	FBlueprintEditorUtils::AddFunctionGraph<UClass>(Caller, CallPing, true, nullptr);
	FGraphFixture::AddMessage(CallPing, TEXT("Ping"), InterfaceClass);
	FGraphFixture::AddCast(Caller, BaseClass);
	FGraphFixture::AddSpawn(Caller, Child->GeneratedClass);
	const bool bCreateWidgetNode = FGraphFixture::AddCreateWidget(Caller, LeafClass);
	FGraphFixture::AddCast(CycleA, CycleB->GeneratedClass);
	FGraphFixture::AddCast(CycleB, CycleA->GeneratedClass);
	FGraphFixture::AddCast(Downstream, CycleA->GeneratedClass);

	UWidgetTree* HostTree = FGraphFixture::GetWidgetTree(Host);
	if (!TestTrue(TEXT("create-widget node"), bCreateWidgetNode) || !TestNotNull(TEXT("host widget tree"), HostTree))
	{
		Fixture.Cleanup();
		return false;
	}
	UCanvasPanel* Root = HostTree->ConstructWidget<UCanvasPanel>(UCanvasPanel::StaticClass(), TEXT("RootCanvas"));
	HostTree->RootWidget = Root;
	Root->AddChild(HostTree->ConstructWidget<UUserWidget>(LeafClass, TEXT("PlacedLeaf")));
	UClass* ListViewClass = FindObject<UClass>(nullptr, TEXT("/Script/UMG.ListView"));
	if (!TestNotNull(TEXT("ListView class"), ListViewClass))
	{
		Fixture.Cleanup();
		return false;
	}
	UWidget* List = HostTree->ConstructWidget<UWidget>(ListViewClass, TEXT("EntryList"));
	const FClassProperty* EntryProperty = FindFProperty<FClassProperty>(ListViewClass, TEXT("EntryWidgetClass"));
	if (!TestNotNull(TEXT("EntryWidgetClass property"), EntryProperty))
	{
		Fixture.Cleanup();
		return false;
	}
	EntryProperty->SetObjectPropertyValue_InContainer(List, EntryWidget->GeneratedClass);
	Root->AddChild(List);

	for (UBlueprint* Blueprint : {Interface, Base, LeafWidget, EntryWidget, Child, Implementer, CycleA, CycleB, Caller, Host, Downstream})
	{
		if (!TestTrue(*FString::Printf(TEXT("save %s"), *Blueprint->GetName()), Fixture.SaveAsset(Blueprint, *FPackageName::GetAssetPackageExtension())))
		{
			Fixture.Cleanup();
			return false;
		}
	}
	Fixture.RegisterSavedFiles();

	const FString BasePath = Base->GetPathName();
	FTempIni Ini(FString::Printf(TEXT("[/Script/Test.Settings]\n+Items=%s_C\n+Other=/Game/Blueprints/BP_SimpleActor.BP_SimpleActor_C\n"), *BasePath));
	TestTrue(TEXT("temp ini written"), Ini.bWritten);

	const FString OutPath = Fixture.ReportPath(TEXT("edges"));
	const FCortexCommandResult Result = Run(Fixture.ExportParams(OutPath));
	if (!TestTrue(*FString::Printf(TEXT("export succeeds (%s: %s)"), *Result.ErrorCode, *Result.ErrorMessage), Result.bSuccess && Result.Data.IsValid()))
	{
		Fixture.Cleanup();
		return false;
	}
	TestEqual(TEXT("response out_path"), Result.Data->GetStringField(TEXT("out_path")), OutPath);
	const TSharedPtr<FJsonObject> Report = ReadJsonDocument(OutPath);
	if (!TestTrue(TEXT("report parses"), Report.IsValid()))
	{
		Fixture.Cleanup();
		return false;
	}

	const FString InterfacePath = Interface->GetPathName();
	const FString ChildPath = Child->GetPathName();
	const FString ImplPath = Implementer->GetPathName();
	const FString CallerPath = Caller->GetPathName();
	const FString CycleAPath = CycleA->GetPathName();
	const FString CycleBPath = CycleB->GetPathName();
	const FString LeafPath = LeafWidget->GetPathName();
	const FString EntryPath = EntryWidget->GetPathName();
	const FString HostPath = Host->GetPathName();
	const FString DownstreamPath = Downstream->GetPathName();
	const FString OutsideObjectPath = TEXT("/Game/Blueprints/BP_SimpleActor.BP_SimpleActor");
	TestTrue(TEXT("outside config target exists in the sandbox"), FPackageName::DoesPackageExist(TEXT("/Game/Blueprints/BP_SimpleActor")));
	TestTrue(TEXT("casts: downstream -> cycle a"), HasEdge(Report, DownstreamPath, CycleAPath, TEXT("casts")));

	TestTrue(TEXT("inherits: child -> base"), HasEdge(Report, ChildPath, BasePath, TEXT("inherits")));
	TestTrue(TEXT("inherits: base -> native Actor"), HasEdge(Report, BasePath, TEXT("/Script/Engine.Actor"), TEXT("inherits")));
	TestTrue(TEXT("implements: impl -> interface"), HasEdge(Report, ImplPath, InterfacePath, TEXT("implements")));
	TestTrue(TEXT("calls_message: caller -> interface"), HasEdge(Report, CallerPath, InterfacePath, TEXT("calls_message")));
	TestTrue(TEXT("casts: caller -> base"), HasEdge(Report, CallerPath, BasePath, TEXT("casts")));
	TestTrue(TEXT("spawns: caller -> child"), HasEdge(Report, CallerPath, ChildPath, TEXT("spawns")));
	TestTrue(TEXT("creates_widget: caller -> leaf"), HasEdge(Report, CallerPath, LeafPath, TEXT("creates_widget")));
	TestTrue(TEXT("widget_tree_child: host -> leaf"), HasEdge(Report, HostPath, LeafPath, TEXT("widget_tree_child")));
	TestTrue(TEXT("listview_entry: host -> entry"), HasEdge(Report, HostPath, EntryPath, TEXT("listview_entry")));
	TestTrue(TEXT("casts: cycle a -> cycle b"), HasEdge(Report, CycleAPath, CycleBPath, TEXT("casts")));
	TestTrue(TEXT("casts: cycle b -> cycle a"), HasEdge(Report, CycleBPath, CycleAPath, TEXT("casts")));

	// The edge carries the graph that holds the node.
	for (const TSharedPtr<FJsonObject>& Edge : ObjectArray(Report, TEXT("edges")))
	{
		if (Edge->GetStringField(TEXT("kind")) == TEXT("casts") && Edge->GetStringField(TEXT("from")) == CallerPath)
		{
			TestEqual(TEXT("cast edge graph"), Edge->GetStringField(TEXT("graph")), FString(TEXT("EventGraph")));
		}
	}

	// Config edge: the ini file is a node of its own and points at the Blueprint asset.
	FString ConfigFrom;
	for (const TSharedPtr<FJsonObject>& Edge : ObjectArray(Report, TEXT("edges")))
	{
		if (Edge->GetStringField(TEXT("kind")) == TEXT("config") && Edge->GetStringField(TEXT("from")).EndsWith(Ini.IniName))
		{
			ConfigFrom = Edge->GetStringField(TEXT("from"));
			// Both ini lines (inside and outside the prefix) produce edges from the same ini node.
		}
	}
	TestTrue(TEXT("config edge targets the Blueprint asset"), HasEdge(Report, ConfigFrom, BasePath, TEXT("config")));
	if (TestFalse(TEXT("config edge from the temp ini"), ConfigFrom.IsEmpty()))
	{
		const TSharedPtr<FJsonObject> IniNode = FindNode(Report, ConfigFrom);
		TestTrue(TEXT("ini node present"), IniNode.IsValid());
	}

	// Nodes: in-prefix assets are internal, the native parent is external.
	const TSharedPtr<FJsonObject> BaseNode = FindNode(Report, BasePath);
	if (TestTrue(TEXT("base node"), BaseNode.IsValid()))
	{
		TestFalse(TEXT("base node is not external"), BaseNode->HasField(TEXT("external")) && BaseNode->GetBoolField(TEXT("external")));
	}
	const TSharedPtr<FJsonObject> ActorNode = FindNode(Report, TEXT("/Script/Engine.Actor"));
	if (TestTrue(TEXT("native parent node"), ActorNode.IsValid()))
	{
		TestTrue(TEXT("native parent flagged external"), ActorNode->GetBoolField(TEXT("external")));
	}

	// Config target outside the prefixes: kept as an external node with its edge.
	const TSharedPtr<FJsonObject> OutsideNode = FindNode(Report, OutsideObjectPath);
	if (TestTrue(TEXT("outside config target node"), OutsideNode.IsValid()))
	{
		TestTrue(TEXT("outside config target flagged external"), OutsideNode->GetBoolField(TEXT("external")));
	}
	TestTrue(TEXT("config edge to the outside target"), HasEdge(Report, ConfigFrom, OutsideObjectPath, TEXT("config")));

	// The Message node inside a function graph carries that graph's name.
	bool bFunctionGraphMessage = false;
	for (const TSharedPtr<FJsonObject>& Edge : ObjectArray(Report, TEXT("edges")))
	{
		if (Edge->GetStringField(TEXT("kind")) == TEXT("calls_message") && Edge->GetStringField(TEXT("from")) == CallerPath
			&& Edge->GetStringField(TEXT("graph")) == TEXT("CallPing"))
		{
			bFunctionGraphMessage = true;
		}
	}
	TestTrue(TEXT("calls_message edge from the function graph"), bFunctionGraphMessage);

	// Default ordering uses hard kinds only: soft kinds (casts, spawns, ...) are edges but never order waves.
	const TArray<TArray<FString>> Waves = StringGroups(Report, TEXT("waves"));
	const TArray<TArray<FString>> Cycles = StringGroups(Report, TEXT("cycles"));
	const auto Before = [this](const TArray<TArray<FString>>& InWaves, const FString& Dependency, const FString& Dependant, const TCHAR* What)
	{
		const int32 DependencyWave = WaveOf(InWaves, Dependency);
		const int32 DependantWave = WaveOf(InWaves, Dependant);
		TestTrue(*FString::Printf(TEXT("%s: both scheduled"), What), DependencyWave != INDEX_NONE && DependantWave != INDEX_NONE);
		TestTrue(*FString::Printf(TEXT("%s: dependency wave %d before dependant wave %d"), What, DependencyWave, DependantWave),
			DependencyWave < DependantWave);
	};
	Before(Waves, InterfacePath, ImplPath, TEXT("interface before implementer"));
	Before(Waves, BasePath, ChildPath, TEXT("base before child"));
	Before(Waves, LeafPath, HostPath, TEXT("child widget before host"));
	Before(Waves, EntryPath, HostPath, TEXT("entry widget before host"));
	TestEqual(TEXT("default ordering: a mutual cast pair is not a cycle"), Cycles.Num(), 0);
	TestEqual(TEXT("default ordering: cast peers are not ordered"), WaveOf(Waves, CycleAPath), 0);
	TestEqual(TEXT("default ordering: cast peers share wave 0"), WaveOf(Waves, CycleBPath), 0);
	TestEqual(TEXT("default ordering: a soft dependant is not pushed later"), WaveOf(Waves, DownstreamPath), 0);
	TestEqual(TEXT("external native parent is never scheduled"), WaveOf(Waves, TEXT("/Script/Engine.Actor")), static_cast<int32>(INDEX_NONE));
	TestEqual(TEXT("external config target is never scheduled"), WaveOf(Waves, OutsideObjectPath), static_cast<int32>(INDEX_NONE));
	TestEqual(TEXT("ini node is never scheduled"), WaveOf(Waves, ConfigFrom), static_cast<int32>(INDEX_NONE));
	{
		const TArray<TSharedPtr<FJsonValue>>* Used = nullptr;
		if (TestTrue(TEXT("response ordering_kinds"), Result.Data->TryGetArrayField(TEXT("ordering_kinds"), Used)))
		{
			TArray<FString> UsedNames;
			for (const TSharedPtr<FJsonValue>& Value : *Used)
			{
				UsedNames.Add(Value->AsString());
			}
			TestEqual(TEXT("default ordering kinds are the hard kinds"), UsedNames,
				TArray<FString>({TEXT("enum_use"), TEXT("implements"), TEXT("inherits"), TEXT("listview_entry"), TEXT("struct_use"), TEXT("widget_tree_child")}));
		}
		const TSharedPtr<FJsonObject>* Unresolved = nullptr;
		if (TestTrue(TEXT("response unresolved summary"), Result.Data->TryGetObjectField(TEXT("unresolved"), Unresolved)))
		{
			TestEqual(TEXT("no unresolved parents"), static_cast<int32>((*Unresolved)->GetNumberField(TEXT("parents"))), 0);
			TestEqual(TEXT("no unresolved interfaces"), static_cast<int32>((*Unresolved)->GetNumberField(TEXT("interfaces"))), 0);
		}
	}

	// Soft ordering on request: casts/spawns/messages/create-widget order the waves and the cast pair is a cycle.
	TSharedPtr<FJsonObject> Soft = Fixture.ExportParams(Fixture.ReportPath(TEXT("soft")));
	Soft->SetArrayField(TEXT("ordering_kinds"), JsonStrings({TEXT("calls_message"), TEXT("casts"), TEXT("spawns"), TEXT("creates_widget")}));
	const FCortexCommandResult SoftResult = Run(Soft);
	if (TestTrue(TEXT("soft ordering export succeeds"), SoftResult.bSuccess && SoftResult.Data.IsValid()))
	{
		const TSharedPtr<FJsonObject> SoftReport = ReadJsonDocument(Soft->GetStringField(TEXT("out_path")));
		if (TestTrue(TEXT("soft report parses"), SoftReport.IsValid()))
		{
			const TArray<TArray<FString>> SoftWaves = StringGroups(SoftReport, TEXT("waves"));
			const TArray<TArray<FString>> SoftCycles = StringGroups(SoftReport, TEXT("cycles"));
			Before(SoftWaves, InterfacePath, CallerPath, TEXT("message target before caller"));
			Before(SoftWaves, BasePath, CallerPath, TEXT("cast target before caller"));
			Before(SoftWaves, ChildPath, CallerPath, TEXT("spawn target before caller"));
			Before(SoftWaves, LeafPath, CallerPath, TEXT("created widget before caller"));
			if (TestEqual(TEXT("one cycle"), SoftCycles.Num(), 1))
			{
				TestEqual(TEXT("cycle has exactly two members"), SoftCycles[0].Num(), 2);
				TestTrue(TEXT("cycle holds both cast peers"), SoftCycles[0].Contains(CycleAPath) && SoftCycles[0].Contains(CycleBPath));
			}
			TestEqual(TEXT("cycle peers share one wave"), WaveOf(SoftWaves, CycleAPath), WaveOf(SoftWaves, CycleBPath));
			const int32 CycleWave = WaveOf(SoftWaves, CycleAPath);
			TestTrue(TEXT("a node depending on a cycle member lands in a strictly later wave"),
				CycleWave != INDEX_NONE && WaveOf(SoftWaves, DownstreamPath) > CycleWave);
			const TArray<TSharedPtr<FJsonValue>>* SoftUsed = nullptr;
			if (TestTrue(TEXT("soft response ordering_kinds"), SoftResult.Data->TryGetArrayField(TEXT("ordering_kinds"), SoftUsed)))
			{
				TestEqual(TEXT("ordering_kinds echoes the request"), SoftUsed->Num(), 4);
			}
		}
	}

	// Compact response mirrors the file.
	for (const TCHAR* Kind : {TEXT("inherits"), TEXT("implements"), TEXT("calls_message"), TEXT("casts"), TEXT("spawns"),
		TEXT("creates_widget"), TEXT("widget_tree_child"), TEXT("listview_entry"), TEXT("config")})
	{
		TestTrue(*FString::Printf(TEXT("edges_by_kind counts %s"), Kind), EdgesOfKind(Result.Data, Kind) >= 1);
	}
	int32 FileEdges = ObjectArray(Report, TEXT("edges")).Num();
	int32 CountedEdges = 0;
	const TSharedPtr<FJsonObject>* ByKind = nullptr;
	if (Result.Data->TryGetObjectField(TEXT("edges_by_kind"), ByKind))
	{
		for (const auto& Pair : (*ByKind)->Values)
		{
			CountedEdges += static_cast<int32>(Pair.Value->AsNumber());
		}
	}
	TestEqual(TEXT("edges_by_kind totals the file edges"), CountedEdges, FileEdges);
	TestEqual(TEXT("response nodes equals file nodes"), static_cast<int32>(Result.Data->GetNumberField(TEXT("nodes"))),
		ObjectArray(Report, TEXT("nodes")).Num());
	const TArray<TSharedPtr<FJsonValue>>* WaveSizes = nullptr;
	if (TestTrue(TEXT("response waves"), Result.Data->TryGetArrayField(TEXT("waves"), WaveSizes)) && TestEqual(TEXT("one size per wave"), WaveSizes->Num(), Waves.Num()))
	{
		for (int32 Index = 0; Index < Waves.Num(); ++Index)
		{
			TestEqual(*FString::Printf(TEXT("wave %d size"), Index), static_cast<int32>((*WaveSizes)[Index]->AsNumber()), Waves[Index].Num());
		}
	}
	TestEqual(TEXT("response cycles count"), static_cast<int32>(Result.Data->GetNumberField(TEXT("cycles"))), Cycles.Num());
	TestFalse(TEXT("response is compact: no edges"), Result.Data->HasField(TEXT("edges")));

	// edge_kinds restricts both the derivation and the waves input.
	TSharedPtr<FJsonObject> Filtered = Fixture.ExportParams(Fixture.ReportPath(TEXT("filtered")));
	Filtered->SetArrayField(TEXT("edge_kinds"), JsonStrings({TEXT("inherits"), TEXT("casts")}));
	const FCortexCommandResult FilteredResult = Run(Filtered);
	if (TestTrue(TEXT("filtered export succeeds"), FilteredResult.bSuccess && FilteredResult.Data.IsValid()))
	{
		const TSharedPtr<FJsonObject> FilteredReport = ReadJsonDocument(Filtered->GetStringField(TEXT("out_path")));
		if (TestTrue(TEXT("filtered report parses"), FilteredReport.IsValid()))
		{
			for (const TSharedPtr<FJsonObject>& Edge : ObjectArray(FilteredReport, TEXT("edges")))
			{
				const FString Kind = Edge->GetStringField(TEXT("kind"));
				TestTrue(*FString::Printf(TEXT("only requested kinds, saw %s"), *Kind), Kind == TEXT("inherits") || Kind == TEXT("casts"));
			}
			TestTrue(TEXT("filtered keeps inherits"), HasEdge(FilteredReport, ChildPath, BasePath, TEXT("inherits")));
			TestFalse(TEXT("filtered drops implements"), HasEdge(FilteredReport, ImplPath, InterfacePath, TEXT("implements")));
		}
		TestEqual(TEXT("filtered response has no implements"), EdgesOfKind(FilteredResult.Data, TEXT("implements")), 0);
	}

	Fixture.Cleanup();
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexReflectDependencyGraphStructEnumTest,
	"Cortex.Reflect.DependencyGraph.StructEnumUse",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexReflectDependencyGraphStructEnumTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	using namespace CortexReflectDependencyGraphTestSupport;
	FGraphFixture Fixture;

	UPackage* StructPackage = CreatePackage(*FString::Printf(TEXT("%s/F_Payload"), *Fixture.Dir));
	UPackage* EnumPackage = CreatePackage(*FString::Printf(TEXT("%s/E_Mode"), *Fixture.Dir));
	UUserDefinedStruct* Struct = FStructureEditorUtils::CreateUserDefinedStruct(
		StructPackage, TEXT("F_Payload"), RF_Public | RF_Standalone | RF_Transactional);
	UUserDefinedEnum* Enum = Cast<UUserDefinedEnum>(FEnumEditorUtils::CreateUserDefinedEnum(
		EnumPackage, TEXT("E_Mode"), RF_Public | RF_Standalone | RF_Transactional));
	if (!Struct || !Enum)
	{
		AddInfo(TEXT("User-defined struct/enum creation is unavailable in this editor session; struct_use/enum_use skipped"));
		Fixture.Cleanup();
		return true;
	}

	UBlueprint* User = Fixture.MakeBlueprint(TEXT("BP_PayloadUser"), AActor::StaticClass(), false);
	if (!TestNotNull(TEXT("user blueprint"), User))
	{
		Fixture.Cleanup();
		return false;
	}
	FEdGraphPinType StructType(UEdGraphSchema_K2::PC_Struct, NAME_None, Struct, EPinContainerType::None, false, FEdGraphTerminalType());
	FEdGraphPinType EnumType(UEdGraphSchema_K2::PC_Byte, NAME_None, Enum, EPinContainerType::None, false, FEdGraphTerminalType());
	TestTrue(TEXT("struct variable"), FBlueprintEditorUtils::AddMemberVariable(User, TEXT("Payload"), StructType));
	TestTrue(TEXT("enum variable"), FBlueprintEditorUtils::AddMemberVariable(User, TEXT("Mode"), EnumType));
	FKismetEditorUtilities::CompileBlueprint(User);

	if (!TestTrue(TEXT("save struct"), Fixture.SaveAsset(Struct, *FPackageName::GetAssetPackageExtension()))
		|| !TestTrue(TEXT("save enum"), Fixture.SaveAsset(Enum, *FPackageName::GetAssetPackageExtension()))
		|| !TestTrue(TEXT("save user"), Fixture.SaveAsset(User, *FPackageName::GetAssetPackageExtension())))
	{
		Fixture.Cleanup();
		return false;
	}
	Fixture.RegisterSavedFiles();

	const FString OutPath = Fixture.ReportPath(TEXT("structenum"));
	const FCortexCommandResult Result = Run(Fixture.ExportParams(OutPath));
	if (!TestTrue(*FString::Printf(TEXT("export succeeds (%s: %s)"), *Result.ErrorCode, *Result.ErrorMessage), Result.bSuccess && Result.Data.IsValid()))
	{
		Fixture.Cleanup();
		return false;
	}
	const TSharedPtr<FJsonObject> Report = ReadJsonDocument(OutPath);
	if (TestTrue(TEXT("report parses"), Report.IsValid()))
	{
		TestTrue(TEXT("struct_use: blueprint -> struct"), HasEdge(Report, User->GetPathName(), Struct->GetPathName(), TEXT("struct_use")));
		TestTrue(TEXT("enum_use: blueprint -> enum"), HasEdge(Report, User->GetPathName(), Enum->GetPathName(), TEXT("enum_use")));
		const TArray<TArray<FString>> Waves = StringGroups(Report, TEXT("waves"));
		TestTrue(TEXT("struct scheduled before its user"), WaveOf(Waves, Struct->GetPathName()) < WaveOf(Waves, User->GetPathName())
			&& WaveOf(Waves, Struct->GetPathName()) != INDEX_NONE);
		TestTrue(TEXT("enum scheduled before its user"), WaveOf(Waves, Enum->GetPathName()) < WaveOf(Waves, User->GetPathName())
			&& WaveOf(Waves, Enum->GetPathName()) != INDEX_NONE);
	}
	Fixture.Cleanup();
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexReflectDependencyGraphLevelPlacementTest,
	"Cortex.Reflect.DependencyGraph.LevelPlacement",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexReflectDependencyGraphLevelPlacementTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	using namespace CortexReflectDependencyGraphTestSupport;
	FGraphFixture Fixture;

	UBlueprint* Placed = Fixture.MakeBlueprint(TEXT("BP_Placed"), AActor::StaticClass());
	if (!TestNotNull(TEXT("placed blueprint"), Placed) || !TestNotNull(TEXT("placed class"), Placed->GeneratedClass.Get())
		|| !TestTrue(TEXT("save placed"), Fixture.SaveAsset(Placed, *FPackageName::GetAssetPackageExtension())))
	{
		Fixture.Cleanup();
		return false;
	}

	UPackage* MapPackage = CreatePackage(*FString::Printf(TEXT("%s/L_Placement"), *Fixture.Dir));
	UWorld* World = UWorld::CreateWorld(EWorldType::Inactive, false, TEXT("L_Placement"), MapPackage);
	if (!TestNotNull(TEXT("world"), World))
	{
		Fixture.Cleanup();
		return false;
	}
	World->SetFlags(RF_Public | RF_Standalone);
	AActor* Actor = World->SpawnActor<AActor>(Placed->GeneratedClass);
	if (!TestNotNull(TEXT("placed actor"), Actor) || !TestTrue(TEXT("save map"), Fixture.SaveAsset(World, *FPackageName::GetMapPackageExtension())))
	{
		Fixture.Cleanup();
		return false;
	}
	Fixture.RegisterSavedFiles();

	const FString OutPath = Fixture.ReportPath(TEXT("level"));
	TSharedPtr<FJsonObject> LevelParams = Fixture.ExportParams(OutPath);
	LevelParams->SetArrayField(TEXT("ordering_kinds"), JsonStrings({TEXT("level_placement")})); // soft kind, ordering on request
	const FCortexCommandResult Result = Run(LevelParams);
	if (!TestTrue(*FString::Printf(TEXT("export succeeds (%s: %s)"), *Result.ErrorCode, *Result.ErrorMessage), Result.bSuccess && Result.Data.IsValid()))
	{
		Fixture.Cleanup();
		return false;
	}
	const TSharedPtr<FJsonObject> Report = ReadJsonDocument(OutPath);
	if (TestTrue(TEXT("report parses"), Report.IsValid()))
	{
		const FString WorldPath = World->GetPathName();
		TestTrue(TEXT("level_placement: world -> blueprint"), HasEdge(Report, WorldPath, Placed->GetPathName(), TEXT("level_placement")));
		const TArray<TArray<FString>> Waves = StringGroups(Report, TEXT("waves"));
		TestTrue(TEXT("blueprint scheduled before the map that places it"),
			WaveOf(Waves, Placed->GetPathName()) != INDEX_NONE && WaveOf(Waves, Placed->GetPathName()) < WaveOf(Waves, WorldPath));
	}
	Fixture.Cleanup();
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexReflectDependencyGraphOfpaOutsidePrefixTest,
	"Cortex.Reflect.DependencyGraph.ExternalActorOutsidePrefix",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexReflectDependencyGraphOfpaOutsidePrefixTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	using namespace CortexReflectDependencyGraphTestSupport;
	FGraphFixture Fixture;  // the exported prefix: holds the map only
	FGraphFixture Outside;  // another folder: holds the Blueprint the map places

	UBlueprint* Placed = Outside.MakeBlueprint(TEXT("BP_OutsideActor"), AActor::StaticClass());
	if (!TestNotNull(TEXT("placed blueprint"), Placed) || !TestNotNull(TEXT("placed class"), Placed->GeneratedClass.Get())
		|| !TestTrue(TEXT("save placed"), Outside.SaveAsset(Placed, *FPackageName::GetAssetPackageExtension())))
	{
		Fixture.Cleanup();
		Outside.Cleanup();
		return false;
	}

	// One-File-Per-Actor: the placed actor lives in its own package; the map package does not depend on the Blueprint.
	UPackage* MapPackage = CreatePackage(*FString::Printf(TEXT("%s/L_Ofpa"), *Fixture.Dir));
	UWorld* World = UWorld::CreateWorld(EWorldType::Inactive, false, TEXT("L_Ofpa"), MapPackage);
	if (!TestNotNull(TEXT("world"), World))
	{
		Fixture.Cleanup();
		Outside.Cleanup();
		return false;
	}
	World->SetFlags(RF_Public | RF_Standalone);
	World->PersistentLevel->SetUseExternalActors(true);
	FActorSpawnParameters SpawnParameters;
	SpawnParameters.OverrideLevel = World->PersistentLevel;
	AActor* Actor = World->SpawnActor<AActor>(Placed->GeneratedClass, FTransform::Identity, SpawnParameters);
	const FString ExternalActorsPath = ULevel::GetExternalActorsPath(MapPackage->GetName());
	bool bSaved = TestNotNull(TEXT("placed actor"), Actor) && Fixture.SaveAsset(World, *FPackageName::GetMapPackageExtension());
	if (bSaved)
	{
		for (AActor* LevelActor : World->PersistentLevel->Actors)
		{
			UPackage* ExternalPackage = LevelActor ? LevelActor->GetExternalPackage() : nullptr;
			if (!ExternalPackage)
			{
				continue;
			}
			FString ExternalFile;
			bSaved &= FPackageName::TryConvertLongPackageNameToFilename(ExternalPackage->GetName(), ExternalFile, FPackageName::GetAssetPackageExtension());
			IFileManager::Get().MakeDirectory(*FPaths::GetPath(ExternalFile), true);
			FSavePackageArgs ExternalArgs;
			ExternalArgs.TopLevelFlags = RF_Standalone;
			bSaved &= UPackage::SavePackage(ExternalPackage, nullptr, *ExternalFile, ExternalArgs);
			Fixture.SavedFiles.Add(ExternalFile);
		}
	}
	const auto Cleanup = [&]()
	{
		Fixture.Cleanup();
		Outside.Cleanup();
		IFileManager::Get().DeleteDirectory(*FPackageName::LongPackageNameToFilename(ExternalActorsPath), false, true);
	};
	if (!TestTrue(TEXT("map and its external actor saved"), bSaved))
	{
		Cleanup();
		return false;
	}
	Fixture.RegisterSavedFiles();
	Outside.RegisterSavedFiles();

	const FString OutPath = Fixture.ReportPath(TEXT("ofpa"));
	const FCortexCommandResult Result = Run(Fixture.ExportParams(OutPath));
	if (!TestTrue(*FString::Printf(TEXT("export succeeds (%s: %s)"), *Result.ErrorCode, *Result.ErrorMessage), Result.bSuccess && Result.Data.IsValid()))
	{
		Cleanup();
		return false;
	}
	const TSharedPtr<FJsonObject> Report = ReadJsonDocument(OutPath);
	if (TestTrue(TEXT("report parses"), Report.IsValid()))
	{
		TestTrue(TEXT("level_placement: world -> a Blueprint outside the prefixes"),
			HasEdge(Report, World->GetPathName(), Placed->GetPathName(), TEXT("level_placement")));
		const TSharedPtr<FJsonObject> Node = FindNode(Report, Placed->GetPathName());
		if (TestNotNull(TEXT("the outside Blueprint is an external node"), Node.Get()))
		{
			TestTrue(TEXT("flagged external"), Node->HasField(TEXT("external")) && Node->GetBoolField(TEXT("external")));
		}
	}
	Cleanup();
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexReflectDependencyGraphRefusalsTest,
	"Cortex.Reflect.DependencyGraph.Refusals",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexReflectDependencyGraphRefusalsTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	using namespace CortexReflectDependencyGraphTestSupport;
	FGraphFixture Fixture;
	const FString OutPath = Fixture.ReportPath(TEXT("refusal"));

	const auto Expect = [this, &OutPath](const TCHAR* What, const TSharedPtr<FJsonObject>& Params, const FString& ExpectedCode)
	{
		const FCortexCommandResult Result = Run(Params);
		TestFalse(*FString::Printf(TEXT("%s: refused"), What), Result.bSuccess);
		if (!ExpectedCode.IsEmpty())
		{
			TestEqual(*FString::Printf(TEXT("%s: error code"), What), Result.ErrorCode, ExpectedCode);
		}
		TestFalse(*FString::Printf(TEXT("%s: nothing written"), What), IFileManager::Get().FileExists(*OutPath));
	};

	TSharedPtr<FJsonObject> UnknownKind = Fixture.ExportParams(OutPath);
	UnknownKind->SetArrayField(TEXT("edge_kinds"), JsonStrings({TEXT("inherits"), TEXT("imports")}));
	Expect(TEXT("unknown edge kind"), UnknownKind, CortexErrorCodes::InvalidField);

	TSharedPtr<FJsonObject> EmptyKinds = Fixture.ExportParams(OutPath);
	EmptyKinds->SetArrayField(TEXT("edge_kinds"), {});
	Expect(TEXT("empty edge_kinds"), EmptyKinds, CortexErrorCodes::InvalidField);

	TSharedPtr<FJsonObject> UnknownOrdering = Fixture.ExportParams(OutPath);
	UnknownOrdering->SetArrayField(TEXT("ordering_kinds"), JsonStrings({TEXT("inherits"), TEXT("imports")}));
	Expect(TEXT("unknown ordering kind"), UnknownOrdering, CortexErrorCodes::InvalidField);

	TSharedPtr<FJsonObject> EmptyOrdering = Fixture.ExportParams(OutPath);
	EmptyOrdering->SetArrayField(TEXT("ordering_kinds"), {});
	Expect(TEXT("empty ordering_kinds"), EmptyOrdering, CortexErrorCodes::InvalidField);

	TSharedPtr<FJsonObject> NoPrefixes = MakeShared<FJsonObject>();
	NoPrefixes->SetStringField(TEXT("out_path"), OutPath);
	Expect(TEXT("missing path_prefixes"), NoPrefixes, CortexErrorCodes::InvalidField);

	TSharedPtr<FJsonObject> BadPrefix = Fixture.ExportParams(OutPath);
	BadPrefix->SetArrayField(TEXT("path_prefixes"), JsonStrings({TEXT("/Game/Fine"), TEXT("/Game/Bad.Prefix")}));
	Expect(TEXT("bad prefix"), BadPrefix, CortexErrorCodes::InvalidField);

	TSharedPtr<FJsonObject> NoOutPath = MakeShared<FJsonObject>();
	NoOutPath->SetArrayField(TEXT("path_prefixes"), JsonStrings({Fixture.Dir}));
	Expect(TEXT("missing out_path"), NoOutPath, CortexErrorCodes::InvalidField);

	// out_path outside Saved/Content/Config is refused by the shared write policy, before any asset is touched.
	const FString Forbidden = FPaths::ConvertRelativePathToFull(FPaths::Combine(FPaths::ProjectDir(), TEXT("Source"), TEXT("Cortex120GraphRefused.json")));
	TSharedPtr<FJsonObject> ForbiddenPath = Fixture.ExportParams(Forbidden);
	const FCortexCommandResult ForbiddenResult = Run(ForbiddenPath);
	TestFalse(TEXT("out_path outside the write policy is refused"), ForbiddenResult.bSuccess);
	TestFalse(TEXT("refused out_path was not written"), IFileManager::Get().FileExists(*Forbidden));

	// A well-formed request over an empty folder writes an empty graph rather than failing.
	TSharedPtr<FJsonObject> Empty = Fixture.ExportParams(OutPath);
	Empty->SetArrayField(TEXT("edge_kinds"), JsonStrings({TEXT("inherits")})); // config edges would add nodes from the real project ini files
	const FCortexCommandResult EmptyResult = Run(Empty);
	if (TestTrue(*FString::Printf(TEXT("empty folder exports (%s)"), *EmptyResult.ErrorMessage), EmptyResult.bSuccess && EmptyResult.Data.IsValid()))
	{
		TestEqual(TEXT("no nodes"), static_cast<int32>(EmptyResult.Data->GetNumberField(TEXT("nodes"))), 0);
		const TSharedPtr<FJsonObject>* Matched = nullptr;
		if (TestTrue(TEXT("matched_assets present"), EmptyResult.Data->TryGetObjectField(TEXT("matched_assets"), Matched)))
		{
			TestEqual(TEXT("the prefix matched no assets"), static_cast<int32>((*Matched)->GetNumberField(Fixture.Dir)), 0);
		}
		const TArray<TSharedPtr<FJsonValue>>* Warnings = nullptr;
		if (TestTrue(TEXT("warnings present"), EmptyResult.Data->TryGetArrayField(TEXT("warnings"), Warnings)))
		{
			TestEqual(TEXT("one warning for the empty prefix"), Warnings->Num(), 1);
		}
		TestEqual(TEXT("no cycles"), static_cast<int32>(EmptyResult.Data->GetNumberField(TEXT("cycles"))), 0);
		const TSharedPtr<FJsonObject> Report = ReadJsonDocument(OutPath);
		TestTrue(TEXT("empty report written"), Report.IsValid() && Report->HasField(TEXT("waves")));
	}

	Fixture.Cleanup();
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexReflectDependencyGraphSelfAndInterfaceCallTest,
	"Cortex.Reflect.DependencyGraph.SelfLoopsAndInterfaceCalls",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexReflectDependencyGraphSelfAndInterfaceCallTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	using namespace CortexReflectDependencyGraphTestSupport;
	FGraphFixture Fixture;

	UBlueprint* Interface = Fixture.MakeBlueprint(TEXT("BPI_Ping"), UInterface::StaticClass(), false, BPTYPE_Interface);
	UBlueprint* SelfCaster = Fixture.MakeBlueprint(TEXT("BP_SelfCaster"), AActor::StaticClass());
	if (!TestNotNull(TEXT("interface"), Interface) || !TestNotNull(TEXT("self caster"), SelfCaster))
	{
		Fixture.Cleanup();
		return false;
	}
	UEdGraph* PingGraph = FBlueprintEditorUtils::CreateNewGraph(
		Interface, TEXT("Ping"), UEdGraph::StaticClass(), UEdGraphSchema_K2::StaticClass());
	FBlueprintEditorUtils::AddFunctionGraph<UClass>(Interface, PingGraph, true, nullptr);
	FKismetEditorUtilities::CompileBlueprint(Interface);
	UClass* InterfaceClass = Interface->GeneratedClass;
	if (!TestNotNull(TEXT("interface class"), InterfaceClass) || !TestNotNull(TEXT("self class"), SelfCaster->GeneratedClass.Get()))
	{
		Fixture.Cleanup();
		return false;
	}

	UBlueprint* Caller = Fixture.MakeBlueprint(TEXT("BP_TypedCaller"), AActor::StaticClass(), false);
	if (!TestNotNull(TEXT("caller"), Caller))
	{
		Fixture.Cleanup();
		return false;
	}

	// A cast to its own class is a self-loop and must not become an edge.
	FGraphFixture::AddCast(SelfCaster, SelfCaster->GeneratedClass);

	// A typed call (not a message node) of a function that lives on an interface class reaches its implementers.
	{
		FGraphNodeCreator<UK2Node_CallFunction> Creator(*FGraphFixture::EventGraph(Caller));
		UK2Node_CallFunction* Call = Creator.CreateNode();
		Call->FunctionReference.SetExternalMember(TEXT("Ping"), InterfaceClass);
		Creator.Finalize();
	}
	// A call of a native, non-interface function is not a calls_message edge.
	{
		FGraphNodeCreator<UK2Node_CallFunction> Creator(*FGraphFixture::EventGraph(Caller));
		UK2Node_CallFunction* Call = Creator.CreateNode();
		Call->FunctionReference.SetExternalMember(GET_FUNCTION_NAME_CHECKED(UKismetSystemLibrary, PrintString), UKismetSystemLibrary::StaticClass());
		Creator.Finalize();
	}

	for (UBlueprint* Blueprint : {Interface, SelfCaster, Caller})
	{
		if (!TestTrue(*FString::Printf(TEXT("save %s"), *Blueprint->GetName()), Fixture.SaveAsset(Blueprint, *FPackageName::GetAssetPackageExtension())))
		{
			Fixture.Cleanup();
			return false;
		}
	}
	Fixture.RegisterSavedFiles();

	const FString OutPath = Fixture.ReportPath(TEXT("selfcall"));
	TSharedPtr<FJsonObject> Params = Fixture.ExportParams(OutPath);
	Params->SetArrayField(TEXT("edge_kinds"), JsonStrings({TEXT("casts"), TEXT("calls_message")}));
	const FCortexCommandResult Result = Run(Params);
	if (TestTrue(*FString::Printf(TEXT("export succeeds (%s: %s)"), *Result.ErrorCode, *Result.ErrorMessage), Result.bSuccess && Result.Data.IsValid()))
	{
		const TSharedPtr<FJsonObject> Report = ReadJsonDocument(OutPath);
		if (TestTrue(TEXT("report parses"), Report.IsValid()))
		{
			const FString SelfPath = SelfCaster->GetPathName();
			TestFalse(TEXT("self cast edge is dropped"), HasEdge(Report, SelfPath, SelfPath, TEXT("casts")));
			TestEqual(TEXT("no cast edges at all"), EdgesOfKind(Result.Data, TEXT("casts")), 0);
			TestTrue(TEXT("typed interface call: caller -> interface"),
				HasEdge(Report, Caller->GetPathName(), Interface->GetPathName(), TEXT("calls_message")));
			TestEqual(TEXT("only the interface call is a calls_message edge"), EdgesOfKind(Result.Data, TEXT("calls_message")), 1);
		}
	}
	Fixture.Cleanup();
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexReflectDependencyGraphNestedOfpaOwnerTest,
	"Cortex.Reflect.DependencyGraph.ExternalActorLongestOwner",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexReflectDependencyGraphNestedOfpaOwnerTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	using namespace CortexReflectDependencyGraphTestSupport;
	FGraphFixture Fixture;
	TArray<FString> ExternalRoots;
	const auto Cleanup = [&]()
	{
		Fixture.Cleanup();
		for (const FString& Root : ExternalRoots)
		{
			IFileManager::Get().DeleteDirectory(*FPackageName::LongPackageNameToFilename(Root), false, true);
		}
	};

	// /Arena and /Arena/Sub are both exported; each places its own Blueprint through One-File-Per-Actor.
	UBlueprint* OuterBlueprint = Fixture.MakeBlueprint(TEXT("BP_OuterActor"), AActor::StaticClass());
	UBlueprint* SubBlueprint = Fixture.MakeBlueprint(TEXT("BP_SubActor"), AActor::StaticClass());
	if (!TestNotNull(TEXT("blueprints"), OuterBlueprint) || !TestNotNull(TEXT("blueprints"), SubBlueprint)
		|| !TestTrue(TEXT("save blueprints"), Fixture.SaveAsset(OuterBlueprint, *FPackageName::GetAssetPackageExtension())
			&& Fixture.SaveAsset(SubBlueprint, *FPackageName::GetAssetPackageExtension())))
	{
		Cleanup();
		return false;
	}

	const auto MakeMap = [&](const FString& Name, UBlueprint* Placed) -> UWorld*
	{
		UPackage* Package = CreatePackage(*FString::Printf(TEXT("%s/%s"), *Fixture.Dir, *Name));
		UWorld* World = UWorld::CreateWorld(EWorldType::Inactive, false, FName(*FPackageName::GetShortName(Name)), Package);
		if (!World)
		{
			return nullptr;
		}
		World->SetFlags(RF_Public | RF_Standalone);
		World->PersistentLevel->SetUseExternalActors(true);
		ExternalRoots.Add(ULevel::GetExternalActorsPath(Package->GetName()));
		FActorSpawnParameters Parameters;
		Parameters.OverrideLevel = World->PersistentLevel;
		bool bSaved = World->SpawnActor<AActor>(Placed->GeneratedClass, FTransform::Identity, Parameters) != nullptr
			&& Fixture.SaveAsset(World, *FPackageName::GetMapPackageExtension());
		for (AActor* LevelActor : World->PersistentLevel->Actors)
		{
			UPackage* ExternalPackage = LevelActor ? LevelActor->GetExternalPackage() : nullptr;
			if (!ExternalPackage)
			{
				continue;
			}
			FString ExternalFile;
			bSaved &= FPackageName::TryConvertLongPackageNameToFilename(ExternalPackage->GetName(), ExternalFile, FPackageName::GetAssetPackageExtension());
			IFileManager::Get().MakeDirectory(*FPaths::GetPath(ExternalFile), true);
			FSavePackageArgs ExternalArgs;
			ExternalArgs.TopLevelFlags = RF_Standalone;
			bSaved &= UPackage::SavePackage(ExternalPackage, nullptr, *ExternalFile, ExternalArgs);
			Fixture.SavedFiles.Add(ExternalFile);
		}
		return bSaved ? World : nullptr;
	};
	UWorld* OuterWorld = MakeMap(TEXT("Arena"), OuterBlueprint);
	UWorld* SubWorld = MakeMap(TEXT("Arena/Sub"), SubBlueprint);
	if (!TestTrue(TEXT("both maps and their external actors saved"), OuterWorld && SubWorld))
	{
		Cleanup();
		return false;
	}
	Fixture.RegisterSavedFiles();

	const FString OutPath = Fixture.ReportPath(TEXT("nested"));
	const FCortexCommandResult Result = Run(Fixture.ExportParams(OutPath));
	if (!TestTrue(*FString::Printf(TEXT("export succeeds (%s)"), *Result.ErrorMessage), Result.bSuccess))
	{
		Cleanup();
		return false;
	}
	const TSharedPtr<FJsonObject> Report = ReadJsonDocument(OutPath);
	if (TestTrue(TEXT("report parses"), Report.IsValid()))
	{
		TestTrue(TEXT("Arena places its own Blueprint"),
			HasEdge(Report, OuterWorld->GetPathName(), OuterBlueprint->GetPathName(), TEXT("level_placement")));
		TestTrue(TEXT("Arena/Sub places its own Blueprint"),
			HasEdge(Report, SubWorld->GetPathName(), SubBlueprint->GetPathName(), TEXT("level_placement")));
		TestFalse(TEXT("the nested map's actor is not attributed to the enclosing map"),
			HasEdge(Report, OuterWorld->GetPathName(), SubBlueprint->GetPathName(), TEXT("level_placement")));
	}
	Cleanup();
	return true;
}
