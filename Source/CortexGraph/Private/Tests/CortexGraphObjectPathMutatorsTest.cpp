#include "Misc/AutomationTest.h"
#include "CortexCommandRouter.h"
#include "CortexGraphCommandHandler.h"
#include "Dom/JsonObject.h"
#include "Engine/Blueprint.h"
#include "GameFramework/Actor.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Misc/Guid.h"
#include "UObject/GarbageCollection.h"
#include "UObject/Package.h"

namespace CortexGraphObjectPathMutatorsTest
{
	TSharedPtr<FJsonObject> MakePrintStringAddParams(const FString& AssetPath)
	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("asset_path"), AssetPath);
		Params->SetStringField(TEXT("node_class"), TEXT("UK2Node_CallFunction"));

		TSharedPtr<FJsonObject> NodeParams = MakeShared<FJsonObject>();
		NodeParams->SetStringField(TEXT("function_name"), TEXT("KismetSystemLibrary.PrintString"));
		Params->SetObjectField(TEXT("params"), NodeParams);
		return Params;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexGraphObjectPathMutatorsTest,
	"Cortex.Graph.ObjectPathMutators",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexGraphObjectPathMutatorsTest::RunTest(const FString& Parameters)
{
	using namespace CortexGraphObjectPathMutatorsTest;

	const FString Suffix = FGuid::NewGuid().ToString(EGuidFormats::Digits).Left(8);
	const FString PackagePath = FString::Printf(
		TEXT("/Game/Temp/CortexGraphObjectPathMutators_%s/BP_ObjectPathMutators_%s"),
		*Suffix,
		*Suffix);
	UPackage* Package = CreatePackage(*PackagePath);
	Package->SetPackageFlags(PKG_PlayInEditor);

	UBlueprint* TestBP = FKismetEditorUtilities::CreateBlueprint(
		AActor::StaticClass(),
		Package,
		*FPackageName::GetShortName(PackagePath),
		BPTYPE_Normal,
		UBlueprint::StaticClass(),
		UBlueprintGeneratedClass::StaticClass());
	TestNotNull(TEXT("test Blueprint should be created"), TestBP);
	if (TestBP == nullptr)
	{
		return false;
	}

	const FString ObjectPath = TestBP->GetPathName();
	TestEqual(TEXT("test input uses object-path shape"), ObjectPath, PackagePath + TEXT(".") + FPackageName::GetShortName(PackagePath));

	FCortexCommandRouter Router;
	Router.RegisterDomain(TEXT("graph"), TEXT("Cortex Graph"), TEXT("1.0.0"), MakeShared<FCortexGraphCommandHandler>());

	FCortexCommandResult AddAResult = Router.Execute(TEXT("graph.add_node"), MakePrintStringAddParams(ObjectPath));
	TestTrue(TEXT("graph.add_node accepts Blueprint object path"), AddAResult.bSuccess);
	FString NodeA;
	if (AddAResult.Data.IsValid())
	{
		AddAResult.Data->TryGetStringField(TEXT("node_id"), NodeA);
	}

	FCortexCommandResult AddBResult = Router.Execute(TEXT("graph.add_node"), MakePrintStringAddParams(ObjectPath));
	TestTrue(TEXT("second graph.add_node accepts Blueprint object path"), AddBResult.bSuccess);
	FString NodeB;
	if (AddBResult.Data.IsValid())
	{
		AddBResult.Data->TryGetStringField(TEXT("node_id"), NodeB);
	}

	TestFalse(TEXT("first node id exists"), NodeA.IsEmpty());
	TestFalse(TEXT("second node id exists"), NodeB.IsEmpty());
	if (!AddAResult.bSuccess || !AddBResult.bSuccess || NodeA.IsEmpty() || NodeB.IsEmpty())
	{
		TestBP->MarkAsGarbage();
		Package->MarkAsGarbage();
		CollectGarbage(RF_NoFlags);
		return false;
	}

	TSharedPtr<FJsonObject> SetPinParams = MakeShared<FJsonObject>();
	SetPinParams->SetStringField(TEXT("asset_path"), ObjectPath);
	SetPinParams->SetStringField(TEXT("node_id"), NodeA);
	SetPinParams->SetStringField(TEXT("pin_name"), TEXT("InString"));
	SetPinParams->SetStringField(TEXT("value"), TEXT("Object path input"));
	FCortexCommandResult SetPinResult = Router.Execute(TEXT("graph.set_pin_value"), SetPinParams);
	TestTrue(TEXT("graph.set_pin_value accepts Blueprint object path"), SetPinResult.bSuccess);

	TSharedPtr<FJsonObject> ConnectParams = MakeShared<FJsonObject>();
	ConnectParams->SetStringField(TEXT("asset_path"), ObjectPath);
	ConnectParams->SetStringField(TEXT("source_node"), NodeA);
	ConnectParams->SetStringField(TEXT("source_pin"), TEXT("then"));
	ConnectParams->SetStringField(TEXT("target_node"), NodeB);
	ConnectParams->SetStringField(TEXT("target_pin"), TEXT("execute"));
	FCortexCommandResult ConnectResult = Router.Execute(TEXT("graph.connect"), ConnectParams);
	TestTrue(TEXT("graph.connect accepts Blueprint object path"), ConnectResult.bSuccess);

	TSharedPtr<FJsonObject> LayoutParams = MakeShared<FJsonObject>();
	LayoutParams->SetStringField(TEXT("asset_path"), ObjectPath);
	FCortexCommandResult LayoutResult = Router.Execute(TEXT("graph.auto_layout"), LayoutParams);
	TestTrue(TEXT("graph.auto_layout accepts Blueprint object path"), LayoutResult.bSuccess);

	TSharedPtr<FJsonObject> DisconnectParams = MakeShared<FJsonObject>();
	DisconnectParams->SetStringField(TEXT("asset_path"), ObjectPath);
	DisconnectParams->SetStringField(TEXT("node_id"), NodeA);
	DisconnectParams->SetStringField(TEXT("pin_name"), TEXT("then"));
	FCortexCommandResult DisconnectResult = Router.Execute(TEXT("graph.disconnect"), DisconnectParams);
	TestTrue(TEXT("graph.disconnect accepts Blueprint object path"), DisconnectResult.bSuccess);

	TestBP->MarkAsGarbage();
	Package->MarkAsGarbage();
	CollectGarbage(RF_NoFlags);
	return true;
}
