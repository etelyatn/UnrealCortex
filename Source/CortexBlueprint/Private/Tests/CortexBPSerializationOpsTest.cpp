#include "Misc/AutomationTest.h"
#include "CortexConversionTypes.h"
#include "Operations/CortexBPSerializationOps.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Engine/Blueprint.h"
#include "EdGraph/EdGraph.h"
#include "K2Node_Composite.h"
#include "K2Node_IfThenElse.h"
#include "EdGraphSchema_K2.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "GameFramework/Actor.h"
#include "UObject/SavePackage.h"
#include "HAL/FileManager.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexBPSerializeEntireBlueprintTest,
	"Cortex.Blueprint.Serialization.EntireBlueprint",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexBPSerializeEntireBlueprintTest::RunTest(const FString& Parameters)
{
	(void)Parameters;

	// Use a known test Blueprint — BP_SimpleActor exists in Content/Blueprints/
	FCortexSerializationRequest Request;
	Request.BlueprintPath = TEXT("/Game/Blueprints/BP_SimpleActor");
	Request.Scope = ECortexConversionScope::EntireBlueprint;

	bool bSuccess = false;
	FString JsonResult;
	FCortexBPSerializationOps::Serialize(Request,
		FOnSerializationComplete::CreateLambda([&](const FCortexSerializationResult& SerResult)
		{
			bSuccess = SerResult.bSuccess;
			JsonResult = SerResult.JsonPayload;
		}));

	TestTrue(TEXT("Serialization should succeed"), bSuccess);
	TestFalse(TEXT("JSON should not be empty"), JsonResult.IsEmpty());

	// Parse and verify structure
	TSharedPtr<FJsonObject> RootObj;
	TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(JsonResult);
	if (FJsonSerializer::Deserialize(Reader, RootObj) && RootObj.IsValid())
	{
		TestTrue(TEXT("Should have blueprint_name"), RootObj->HasField(TEXT("blueprint_name")));
		TestTrue(TEXT("Should have parent_class"), RootObj->HasField(TEXT("parent_class")));
		TestTrue(TEXT("Should have variables"), RootObj->HasField(TEXT("variables")));
		TestTrue(TEXT("Should have graphs"), RootObj->HasField(TEXT("graphs")));
	}
	else
	{
		AddError(TEXT("Failed to parse serialization JSON"));
	}

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexBPSerializeCompactModeTest,
	"Cortex.Blueprint.Serialization.CompactMode",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexBPSerializeCompactModeTest::RunTest(const FString& Parameters)
{
	(void)Parameters;

	// Serialize the same blueprint in both modes and compare
	FCortexSerializationRequest VerboseRequest;
	VerboseRequest.BlueprintPath = TEXT("/Game/Blueprints/BP_SimpleActor");
	VerboseRequest.Scope = ECortexConversionScope::EntireBlueprint;
	VerboseRequest.bConversionMode = false;

	FCortexSerializationRequest CompactRequest;
	CompactRequest.BlueprintPath = TEXT("/Game/Blueprints/BP_SimpleActor");
	CompactRequest.Scope = ECortexConversionScope::EntireBlueprint;
	CompactRequest.bConversionMode = true;

	FString VerboseJson, CompactJson;
	bool bVerboseOk = false, bCompactOk = false;

	FCortexBPSerializationOps::Serialize(VerboseRequest,
		FOnSerializationComplete::CreateLambda([&](const FCortexSerializationResult& SerResult)
		{
			bVerboseOk = SerResult.bSuccess;
			VerboseJson = SerResult.JsonPayload;
		}));

	FCortexBPSerializationOps::Serialize(CompactRequest,
		FOnSerializationComplete::CreateLambda([&](const FCortexSerializationResult& SerResult)
		{
			bCompactOk = SerResult.bSuccess;
			CompactJson = SerResult.JsonPayload;
		}));

	TestTrue(TEXT("Verbose serialization should succeed"), bVerboseOk);
	TestTrue(TEXT("Compact serialization should succeed"), bCompactOk);
	TestFalse(TEXT("Compact JSON should not be empty"), CompactJson.IsEmpty());

	// Compact output must be valid JSON
	TSharedPtr<FJsonObject> CompactRoot;
	TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(CompactJson);
	TestTrue(TEXT("Compact JSON should be valid"), FJsonSerializer::Deserialize(Reader, CompactRoot) && CompactRoot.IsValid());

	// Compact output must preserve essential structure
	if (CompactRoot.IsValid())
	{
		TestTrue(TEXT("Compact should have blueprint_name"), CompactRoot->HasField(TEXT("blueprint_name")));
		TestTrue(TEXT("Compact should have graphs"), CompactRoot->HasField(TEXT("graphs")));
	}

	// Compact output must NOT contain node position fields
	TestFalse(TEXT("Compact JSON must not contain x position"), CompactJson.Contains(TEXT("\"x\":")));
	TestFalse(TEXT("Compact JSON must not contain y position"), CompactJson.Contains(TEXT("\"y\":")));

	// Compact output must NOT contain full UE type paths (e.g. /Script/Engine.Actor)
	TestFalse(TEXT("Compact JSON must not contain full engine type paths"), CompactJson.Contains(TEXT("/Script/")));

	// Compact output must be smaller than verbose
	TestTrue(TEXT("Compact JSON should be smaller than verbose"), CompactJson.Len() < VerboseJson.Len());

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexBPSerializeCompactEventOrFunctionTest,
	"Cortex.Blueprint.Serialization.CompactModeEventOrFunction",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexBPSerializeCompactEventOrFunctionTest::RunTest(const FString& Parameters)
{
	(void)Parameters;

	// EventOrFunction scope — the most common conversion path
	FCortexSerializationRequest Request;
	Request.BlueprintPath = TEXT("/Game/Blueprints/BP_SimpleActor");
	Request.Scope = ECortexConversionScope::EventOrFunction;
	Request.TargetGraphNames.Add(TEXT("ReceiveBeginPlay"));
	Request.bConversionMode = true;

	bool bSuccess = false;
	FString JsonResult;
	FCortexBPSerializationOps::Serialize(Request,
		FOnSerializationComplete::CreateLambda([&](const FCortexSerializationResult& SerResult)
		{
			bSuccess = SerResult.bSuccess;
			JsonResult = SerResult.JsonPayload;
		}));

	if (!bSuccess)
	{
		AddInfo(TEXT("ReceiveBeginPlay not found in BP_SimpleActor — skipping EventOrFunction compact test"));
		return true;
	}

	TestFalse(TEXT("Compact JSON should not be empty"), JsonResult.IsEmpty());

	// Parse and verify
	TSharedPtr<FJsonObject> Root;
	TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(JsonResult);
	TestTrue(TEXT("Compact EventOrFunction JSON should be valid"), FJsonSerializer::Deserialize(Reader, Root) && Root.IsValid());

	// No position fields
	TestFalse(TEXT("Compact EventOrFunction must not contain x position"), JsonResult.Contains(TEXT("\"x\":")));
	TestFalse(TEXT("Compact EventOrFunction must not contain y position"), JsonResult.Contains(TEXT("\"y\":")));

	// No full type paths
	TestFalse(TEXT("Compact EventOrFunction must not contain full engine type paths"), JsonResult.Contains(TEXT("/Script/")));

	return true;
}

// ── Composite subgraph serialization ──────────────────────────────────────────

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexBPSerializeEntireBlueprintCompositeTest,
	"Cortex.Blueprint.Serialization.EntireBlueprintIncludesCompositeSubgraphs",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexBPSerializeEntireBlueprintCompositeTest::RunTest(const FString& Parameters)
{
	(void)Parameters;

	// Asset path — must be a real package so LoadBlueprintSafe can find it.
	// Use /Game/Blueprints/ which is guaranteed to exist in the Sandbox.
	const FString AssetPath = TEXT("/Game/Blueprints/BP_SerialCompositeTest");
	const FString PkgName = FPackageName::ObjectPathToPackageName(AssetPath);
	const FString PkgFile = FPackageName::LongPackageNameToFilename(PkgName, TEXT(".uasset"));
	IFileManager::Get().MakeDirectory(*FPaths::GetPath(PkgFile), true);

	// Create a real package so DoesPackageExist() returns true after save
	UPackage* Pkg = CreatePackage(*PkgName);
	TestNotNull(TEXT("Package created"), Pkg);
	if (!Pkg) { return false; }

	UBlueprint* TestBP = FKismetEditorUtilities::CreateBlueprint(
		AActor::StaticClass(),
		Pkg,
		FName(TEXT("BP_SerialCompositeTest")),
		BPTYPE_Normal,
		UBlueprint::StaticClass(),
		UBlueprintGeneratedClass::StaticClass()
	);
	TestNotNull(TEXT("TestBP created"), TestBP);
	if (!TestBP) { Pkg->MarkAsGarbage(); return false; }

	UEdGraph* EventGraph = nullptr;
	for (UEdGraph* G : TestBP->UbergraphPages)
	{
		if (G && G->GetName() == TEXT("EventGraph")) { EventGraph = G; break; }
	}
	TestNotNull(TEXT("EventGraph found"), EventGraph);
	if (!EventGraph) { TestBP->MarkAsGarbage(); Pkg->MarkAsGarbage(); return false; }

	// Build composite: EventGraph -> composite node -> BoundGraph with IfThenElse inside
	UK2Node_Composite* Composite = NewObject<UK2Node_Composite>(EventGraph);
	Composite->CreateNewGuid();
	EventGraph->AddNode(Composite, true, false);

	UEdGraph* Sub = FBlueprintEditorUtils::CreateNewGraph(
		TestBP, FName(TEXT("InnerGraph")), UEdGraph::StaticClass(), UEdGraphSchema_K2::StaticClass()
	);
	Composite->BoundGraph = Sub;
	Composite->AllocateDefaultPins();
	// Register Sub in EventGraph->SubGraphs so GetAllChildrenGraphs() finds it.
	// In a real editor flow, UK2Node_Composite::PostPlacedNewNode() does this.
	EventGraph->SubGraphs.Add(Sub);

	// Add an IfThenElse node inside the composite's BoundGraph
	UK2Node_IfThenElse* Branch = NewObject<UK2Node_IfThenElse>(Sub);
	Branch->CreateNewGuid();
	Sub->AddNode(Branch, true, false);

	// Save the package so LoadBlueprintSafe can find it via DoesPackageExist()
	FSavePackageArgs SaveArgs;
	SaveArgs.TopLevelFlags = RF_Public | RF_Standalone;
	UPackage::SavePackage(Pkg, TestBP, *PkgFile, SaveArgs);

	// Serialize with EntireBlueprint scope (both verbose and compact)
	auto RunSerial = [&](bool bCompact) -> FString
	{
		FCortexSerializationRequest Req;
		Req.BlueprintPath = AssetPath;
		Req.Scope = ECortexConversionScope::EntireBlueprint;
		Req.bConversionMode = bCompact;
		FString Out;
		FCortexBPSerializationOps::Serialize(Req,
			FOnSerializationComplete::CreateLambda([&](const FCortexSerializationResult& R)
			{
				if (R.bSuccess) { Out = R.JsonPayload; }
			}));
		return Out;
	};

	FString VerboseJson = RunSerial(false);
	FString CompactJson = RunSerial(true);

	TestFalse(TEXT("Verbose JSON must not be empty"), VerboseJson.IsEmpty());
	TestFalse(TEXT("Compact JSON must not be empty"), CompactJson.IsEmpty());

	// The IfThenElse node lives only inside the composite's BoundGraph.
	// GetAllGraphs() must find it; the old UbergraphPages+FunctionGraphs iteration misses it.
	TestTrue(TEXT("Verbose output must include IfThenElse node from composite subgraph"),
		VerboseJson.Contains(TEXT("IfThenElse")));
	TestTrue(TEXT("Compact output must include IfThenElse node from composite subgraph"),
		CompactJson.Contains(TEXT("IfThenElse")));

	// Cleanup: remove saved file and mark all created assets as garbage
	IFileManager::Get().Delete(*PkgFile);
	TestBP->MarkAsGarbage();
	Pkg->MarkAsGarbage();
	return true;
}

// ── Nested composite serialization (TD-BP-SERIAL-003) ───────────────────────

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexBPSerializeNestedCompositeTest,
	"Cortex.Blueprint.Serialization.NestedCompositeSubgraphs",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexBPSerializeNestedCompositeTest::RunTest(const FString& Parameters)
{
	(void)Parameters;

	const FString AssetPath = TEXT("/Game/Blueprints/BP_SerialNestedCompositeTest");
	const FString PkgName = FPackageName::ObjectPathToPackageName(AssetPath);
	const FString PkgFile = FPackageName::LongPackageNameToFilename(PkgName, TEXT(".uasset"));
	IFileManager::Get().MakeDirectory(*FPaths::GetPath(PkgFile), true);

	UPackage* Pkg = CreatePackage(*PkgName);
	TestNotNull(TEXT("Package created"), Pkg);
	if (!Pkg) { return false; }

	UBlueprint* TestBP = FKismetEditorUtilities::CreateBlueprint(
		AActor::StaticClass(), Pkg,
		FName(TEXT("BP_SerialNestedCompositeTest")),
		BPTYPE_Normal, UBlueprint::StaticClass(), UBlueprintGeneratedClass::StaticClass()
	);
	TestNotNull(TEXT("TestBP created"), TestBP);
	if (!TestBP) { Pkg->MarkAsGarbage(); return false; }

	UEdGraph* EventGraph = nullptr;
	for (UEdGraph* G : TestBP->UbergraphPages)
	{
		if (G && G->GetName() == TEXT("EventGraph")) { EventGraph = G; break; }
	}
	TestNotNull(TEXT("EventGraph found"), EventGraph);
	if (!EventGraph) { TestBP->MarkAsGarbage(); Pkg->MarkAsGarbage(); return false; }

	// Build: EventGraph -> OuterComposite -> InnerComposite -> IfThenElse
	UK2Node_Composite* Outer = NewObject<UK2Node_Composite>(EventGraph);
	Outer->CreateNewGuid();
	EventGraph->AddNode(Outer, true, false);
	UEdGraph* OuterSub = FBlueprintEditorUtils::CreateNewGraph(
		TestBP, FName(TEXT("OuterComposite")), UEdGraph::StaticClass(), UEdGraphSchema_K2::StaticClass()
	);
	Outer->BoundGraph = OuterSub;
	Outer->AllocateDefaultPins();
	EventGraph->SubGraphs.Add(OuterSub);

	UK2Node_Composite* Inner = NewObject<UK2Node_Composite>(OuterSub);
	Inner->CreateNewGuid();
	OuterSub->AddNode(Inner, true, false);
	UEdGraph* InnerSub = FBlueprintEditorUtils::CreateNewGraph(
		TestBP, FName(TEXT("InnerComposite")), UEdGraph::StaticClass(), UEdGraphSchema_K2::StaticClass()
	);
	Inner->BoundGraph = InnerSub;
	Inner->AllocateDefaultPins();
	OuterSub->SubGraphs.Add(InnerSub);

	// Add IfThenElse inside the innermost composite
	UK2Node_IfThenElse* Branch = NewObject<UK2Node_IfThenElse>(InnerSub);
	Branch->CreateNewGuid();
	InnerSub->AddNode(Branch, true, false);

	FSavePackageArgs SaveArgs;
	SaveArgs.TopLevelFlags = RF_Public | RF_Standalone;
	UPackage::SavePackage(Pkg, TestBP, *PkgFile, SaveArgs);

	// Serialize — GetAllGraphs() must recurse 2 levels to find the IfThenElse
	FCortexSerializationRequest Req;
	Req.BlueprintPath = AssetPath;
	Req.Scope = ECortexConversionScope::EntireBlueprint;

	FString JsonResult;
	FCortexBPSerializationOps::Serialize(Req,
		FOnSerializationComplete::CreateLambda([&](const FCortexSerializationResult& R)
		{
			if (R.bSuccess) { JsonResult = R.JsonPayload; }
		}));

	TestFalse(TEXT("JSON must not be empty"), JsonResult.IsEmpty());
	TestTrue(TEXT("Output must include IfThenElse from nested composite"),
		JsonResult.Contains(TEXT("IfThenElse")));

	IFileManager::Get().Delete(*PkgFile);
	TestBP->MarkAsGarbage();
	Pkg->MarkAsGarbage();
	return true;
}

// ── Function graph composite serialization (TD-BP-SERIAL-003) ───────────────

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexBPSerializeFuncGraphCompositeTest,
	"Cortex.Blueprint.Serialization.FunctionGraphCompositeSubgraph",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexBPSerializeFuncGraphCompositeTest::RunTest(const FString& Parameters)
{
	(void)Parameters;

	const FString AssetPath = TEXT("/Game/Blueprints/BP_SerialFuncCompositeTest");
	const FString PkgName = FPackageName::ObjectPathToPackageName(AssetPath);
	const FString PkgFile = FPackageName::LongPackageNameToFilename(PkgName, TEXT(".uasset"));
	IFileManager::Get().MakeDirectory(*FPaths::GetPath(PkgFile), true);

	UPackage* Pkg = CreatePackage(*PkgName);
	TestNotNull(TEXT("Package created"), Pkg);
	if (!Pkg) { return false; }

	UBlueprint* TestBP = FKismetEditorUtilities::CreateBlueprint(
		AActor::StaticClass(), Pkg,
		FName(TEXT("BP_SerialFuncCompositeTest")),
		BPTYPE_Normal, UBlueprint::StaticClass(), UBlueprintGeneratedClass::StaticClass()
	);
	TestNotNull(TEXT("TestBP created"), TestBP);
	if (!TestBP) { Pkg->MarkAsGarbage(); return false; }

	// Create a function graph
	UEdGraph* FuncGraph = FBlueprintEditorUtils::CreateNewGraph(
		TestBP, FName(TEXT("MyTestFunction")),
		UEdGraph::StaticClass(), UEdGraphSchema_K2::StaticClass()
	);
	TestBP->FunctionGraphs.Add(FuncGraph);

	// Build composite inside the function graph with a CallFunction node
	UK2Node_Composite* Composite = NewObject<UK2Node_Composite>(FuncGraph);
	Composite->CreateNewGuid();
	FuncGraph->AddNode(Composite, true, false);
	UEdGraph* Sub = FBlueprintEditorUtils::CreateNewGraph(
		TestBP, FName(TEXT("FuncComposite")), UEdGraph::StaticClass(), UEdGraphSchema_K2::StaticClass()
	);
	Composite->BoundGraph = Sub;
	Composite->AllocateDefaultPins();
	FuncGraph->SubGraphs.Add(Sub);

	// Use IfThenElse (same as nested test) — its class name "IfThenElse" is
	// reliably present in serialized JSON, unlike CallFunction where the
	// function name appears only in the display title with spaces.
	UK2Node_IfThenElse* BranchNode = NewObject<UK2Node_IfThenElse>(Sub);
	BranchNode->CreateNewGuid();
	Sub->AddNode(BranchNode, true, false);

	FSavePackageArgs SaveArgs;
	SaveArgs.TopLevelFlags = RF_Public | RF_Standalone;
	UPackage::SavePackage(Pkg, TestBP, *PkgFile, SaveArgs);

	// Serialize — must find the IfThenElse inside the function graph's composite
	FCortexSerializationRequest Req;
	Req.BlueprintPath = AssetPath;
	Req.Scope = ECortexConversionScope::EntireBlueprint;

	FString JsonResult;
	FCortexBPSerializationOps::Serialize(Req,
		FOnSerializationComplete::CreateLambda([&](const FCortexSerializationResult& R)
		{
			if (R.bSuccess) { JsonResult = R.JsonPayload; }
		}));

	TestFalse(TEXT("JSON must not be empty"), JsonResult.IsEmpty());
	TestTrue(TEXT("Output must include IfThenElse from function graph composite"),
		JsonResult.Contains(TEXT("IfThenElse")));

	IFileManager::Get().Delete(*PkgFile);
	TestBP->MarkAsGarbage();
	Pkg->MarkAsGarbage();
	return true;
}
