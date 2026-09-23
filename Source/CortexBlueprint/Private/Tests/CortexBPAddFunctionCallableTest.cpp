#include "Misc/AutomationTest.h"
#include "CortexBPCommandHandler.h"
#include "CortexCommandRouter.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "EdGraph/EdGraph.h"
#include "EdGraphSchema_K2.h"
#include "Engine/Blueprint.h"
#include "K2Node_CallFunction.h"
#include "K2Node_Event.h"
#include "K2Node_FunctionEntry.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "UObject/Package.h"

/**
 * A function added through the published `blueprint.add_function` command must be a real Blueprint
 * user function: callable and overridable from Kismet, exactly like the editor's own New Function.
 *
 * The engine only flags a new function graph with FUNC_BlueprintCallable | FUNC_BlueprintEvent |
 * FUNC_Public when the graph is created as a *user* graph
 * (FBlueprintEditorUtils::CreateFunctionGraph: `if (bIsUserCreated) { ... AddExtraFunctionFlags(...) }`).
 * A function created without that flag is not callable from any graph, so the graph authoring
 * preflight legitimately refuses a `CallFunction` node for it — including the cross-Blueprint call on
 * an explicit target that the typed authoring contract requires.
 *
 * These cases are the regression for that contract: the added function must carry the user-function
 * flags on both the skeleton and the generated class, the authoring placement predicate must accept a
 * call node for it from another Blueprint's ubergraph, and the predicate must still refuse a node that
 * is genuinely not placeable in the asked graph kind.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexBPAddFunctionCallableTest,
	"Cortex.Blueprint.AddFunctionCallable",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

namespace CortexBPAddFunctionCallableTest
{
static const TCHAR* TestDirectory = TEXT("/Game/Temp/CortexBPTest_AddFunctionCallable");
static const TCHAR* HostName = TEXT("BP_AddFunctionHost");
static const TCHAR* AdapterName = TEXT("BP_AddFunctionAdapter");
static const TCHAR* FunctionName = TEXT("RecordObservedValue");
static const TCHAR* FunctionGraphName = TEXT("AdapterHelper");

/** Creates an Actor Blueprint through the published command and resolves the loaded asset. */
static UBlueprint* CreateActorBlueprint(FCortexBPCommandHandler& Handler, const TCHAR* Name)
{
	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("name"), Name);
	Params->SetStringField(TEXT("path"), TestDirectory);
	Params->SetStringField(TEXT("type"), TEXT("Actor"));

	const FCortexCommandResult Result = Handler.Execute(TEXT("create"), Params);
	if (!Result.bSuccess || !Result.Data.IsValid())
	{
		return nullptr;
	}

	FString AssetPath;
	Result.Data->TryGetStringField(TEXT("asset_path"), AssetPath);
	if (AssetPath.IsEmpty())
	{
		return nullptr;
	}
	const FString ObjectPath = AssetPath.Contains(TEXT("."))
		? AssetPath
		: FString::Printf(TEXT("%s.%s"), *AssetPath, Name);
	UBlueprint* Blueprint = FindObject<UBlueprint>(nullptr, *ObjectPath);
	return Blueprint != nullptr ? Blueprint : LoadObject<UBlueprint>(nullptr, *ObjectPath);
}

/** Adds a function with one int input and one int output through the published command. */
static bool AddFunction(FCortexBPCommandHandler& Handler, const FString& AssetPath, const TCHAR* Name)
{
	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("asset_path"), AssetPath);
	Params->SetStringField(TEXT("name"), Name);

	TArray<TSharedPtr<FJsonValue>> Inputs;
	TSharedPtr<FJsonObject> Input = MakeShared<FJsonObject>();
	Input->SetStringField(TEXT("name"), TEXT("Value"));
	Input->SetStringField(TEXT("type"), TEXT("int"));
	Inputs.Add(MakeShared<FJsonValueObject>(Input));
	Params->SetArrayField(TEXT("inputs"), Inputs);

	TArray<TSharedPtr<FJsonValue>> Outputs;
	TSharedPtr<FJsonObject> Output = MakeShared<FJsonObject>();
	Output->SetStringField(TEXT("name"), TEXT("ReturnValue"));
	Output->SetStringField(TEXT("type"), TEXT("int"));
	Outputs.Add(MakeShared<FJsonValueObject>(Output));
	Params->SetArrayField(TEXT("outputs"), Outputs);

	return Handler.Execute(TEXT("add_function"), Params).bSuccess;
}

/** Compiles the asset through the published command. */
static bool CompileBlueprint(FCortexBPCommandHandler& Handler, const FString& AssetPath)
{
	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("asset_path"), AssetPath);
	return Handler.Execute(TEXT("compile"), Params).bSuccess;
}

static void Discard(UBlueprint* Blueprint)
{
	if (Blueprint == nullptr)
	{
		return;
	}
	if (UPackage* Package = Blueprint->GetOutermost())
	{
		Blueprint->ClearFlags(RF_Standalone);
		Blueprint->MarkAsGarbage();
		Package->ClearFlags(RF_Standalone);
		Package->MarkAsGarbage();
	}
}
}

bool FCortexBPAddFunctionCallableTest::RunTest(const FString& Parameters)
{
	(void)Parameters;

	FCortexBPCommandHandler Handler;

	UBlueprint* Host = CortexBPAddFunctionCallableTest::CreateActorBlueprint(
		Handler, CortexBPAddFunctionCallableTest::HostName);
	UBlueprint* Adapter = CortexBPAddFunctionCallableTest::CreateActorBlueprint(
		Handler, CortexBPAddFunctionCallableTest::AdapterName);
	if (!TestNotNull(TEXT("host Blueprint created"), Host) || !TestNotNull(TEXT("adapter Blueprint created"), Adapter))
	{
		CortexBPAddFunctionCallableTest::Discard(Host);
		CortexBPAddFunctionCallableTest::Discard(Adapter);
		return false;
	}

	const FString HostObjectPath = Host->GetPathName();
	const FString AdapterObjectPath = Adapter->GetPathName();

	TestTrue(TEXT("the host function is added through the published command"),
		CortexBPAddFunctionCallableTest::AddFunction(Handler, HostObjectPath, CortexBPAddFunctionCallableTest::FunctionName));
	TestTrue(TEXT("the host compiles through the published command"),
		CortexBPAddFunctionCallableTest::CompileBlueprint(Handler, HostObjectPath));
	TestTrue(TEXT("a second function is added to the adapter"),
		CortexBPAddFunctionCallableTest::AddFunction(Handler, AdapterObjectPath, CortexBPAddFunctionCallableTest::FunctionGraphName));
	TestTrue(TEXT("the adapter compiles through the published command"),
		CortexBPAddFunctionCallableTest::CompileBlueprint(Handler, AdapterObjectPath));

	const FName AddedFunctionName(CortexBPAddFunctionCallableTest::FunctionName);
	UFunction* GeneratedFunction = Host->GeneratedClass != nullptr
		? Host->GeneratedClass->FindFunctionByName(AddedFunctionName)
		: nullptr;
	UFunction* SkeletonFunction = Host->SkeletonGeneratedClass != nullptr
		? Host->SkeletonGeneratedClass->FindFunctionByName(AddedFunctionName)
		: nullptr;

	TestNotNull(TEXT("the added function exists on the generated class"), GeneratedFunction);
	TestNotNull(TEXT("the added function exists on the skeleton class"), SkeletonFunction);

	if (SkeletonFunction != nullptr)
	{
		TestTrue(TEXT("the added function is BlueprintCallable on the skeleton class"),
			SkeletonFunction->HasAllFunctionFlags(FUNC_BlueprintCallable));
		TestTrue(TEXT("the added function is BlueprintEvent (overridable) on the skeleton class"),
			SkeletonFunction->HasAllFunctionFlags(FUNC_BlueprintEvent));
		TestTrue(TEXT("the added function is Public on the skeleton class"),
			SkeletonFunction->HasAllFunctionFlags(FUNC_Public));
		TestTrue(TEXT("the schema accepts the added function as callable"),
			UEdGraphSchema_K2::CanUserKismetCallFunction(SkeletonFunction));
	}

	UEdGraph* AdapterUbergraph = Adapter->UbergraphPages.Num() > 0 ? Adapter->UbergraphPages[0].Get() : nullptr;
	if (TestNotNull(TEXT("the adapter has an ubergraph"), AdapterUbergraph) && SkeletonFunction != nullptr)
	{
		// Exactly the question the authoring preflight asks when it plans a CallFunction node for a
		// function declared by another Blueprint: is the node placeable in the prospective graph?
		UK2Node_CallFunction* CallNode = NewObject<UK2Node_CallFunction>(AdapterUbergraph);
		CallNode->SetFromFunction(SkeletonFunction);
		CallNode->AllocateDefaultPins();
		TestTrue(TEXT("a cross-Blueprint call node is placeable in another Blueprint's ubergraph"),
			CallNode->CanPasteHere(AdapterUbergraph));

		// The same predicate must keep refusing a node that is genuinely not placeable here, so the
		// regression can never be satisfied by removing the check.
		UK2Node_FunctionEntry* EntryNode = NewObject<UK2Node_FunctionEntry>(AdapterUbergraph);
		EntryNode->AllocateDefaultPins();
		TestFalse(TEXT("a function entry node is still refused in an ubergraph"),
			EntryNode->CanPasteHere(AdapterUbergraph));

		UEdGraph* AdapterFunctionGraph = nullptr;
		for (UEdGraph* Graph : Adapter->FunctionGraphs)
		{
			if (Graph != nullptr && Graph->GetFName() == FName(CortexBPAddFunctionCallableTest::FunctionGraphName))
			{
				AdapterFunctionGraph = Graph;
				break;
			}
		}
		if (TestNotNull(TEXT("the adapter function graph resolves"), AdapterFunctionGraph))
		{
			UK2Node_Event* EventNode = NewObject<UK2Node_Event>(AdapterFunctionGraph);
			EventNode->AllocateDefaultPins();
			TestFalse(TEXT("an event node is still refused in a function graph"),
				EventNode->CanPasteHere(AdapterFunctionGraph));
		}
	}

	CortexBPAddFunctionCallableTest::Discard(Host);
	CortexBPAddFunctionCallableTest::Discard(Adapter);
	return true;
}
