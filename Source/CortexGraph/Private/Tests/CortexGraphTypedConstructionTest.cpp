#include "Misc/AutomationTest.h"
#include "Operations/CortexGraphPinDefaults.h"
#include "Operations/CortexGraphNodeContract.h"
#include "K2Node_GenericCreateObject.h"
#include "K2Node_DynamicCast.h"
#include "K2Node_CallFunction.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphPin.h"
#include "EdGraphSchema_K2.h"
#include "Engine/Blueprint.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "Engine/EngineTypes.h"
#include "GameFramework/Actor.h"
#include "Components/ActorComponent.h"
#include "Internationalization/StringTable.h"
#include "Internationalization/StringTableCore.h"
#include "CortexEngineCompat.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"
#include "HAL/FileManager.h"
#include "Editor.h"
#include "Editor/Transactor.h"

#if WITH_EDITOR

namespace
{
void CleanupConstructionTestPackage(UPackage* Pkg)
{
	if (!Pkg) return;
	Pkg->SetDirtyFlag(false);
	if (Pkg->IsRooted())
	{
		Pkg->RemoveFromRoot();
	}
	Pkg->ClearFlags(RF_Standalone);
	Pkg->MarkAsGarbage();
	if (GEditor && GEditor->Trans)
	{
		GEditor->Trans->Reset(FText::FromString(TEXT("TestCleanup")));
	}
}
}

// ---------------------------------------------------------------------------
// Test 1: UK2Node_GenericCreateObject lifecycle, exposed-on-spawn pins, reconstruction
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexGraphConstructGenericCreateObjectTest,
	"Cortex.Graph.Authoring.Construction.GenericCreateObject",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexGraphConstructGenericCreateObjectTest::RunTest(const FString& Parameters)
{
	// 1. Create BaseBP with exposed-on-spawn FText variable "Title"
	UPackage* BasePkg = CreatePackage(TEXT("/Temp/BP_ConstructibleBase_T04"));
	UBlueprint* BaseBP = FKismetEditorUtilities::CreateBlueprint(
		UObject::StaticClass(),
		BasePkg,
		FName("BP_ConstructibleBase_T04"),
		BPTYPE_Normal,
		UBlueprint::StaticClass(),
		UBlueprintGeneratedClass::StaticClass());
	TestNotNull(TEXT("BaseBP created"), BaseBP);
	if (!BaseBP) return false;

	FEdGraphPinType TextPinType;
	TextPinType.PinCategory = UEdGraphSchema_K2::PC_Text;
	FBlueprintEditorUtils::AddMemberVariable(BaseBP, FName("Title"), TextPinType);
	FBPVariableDescription* TitleDesc = BaseBP->NewVariables.FindByPredicate([](const FBPVariableDescription& Desc)
	{
		return Desc.VarName == FName("Title");
	});
	if (TitleDesc)
	{
		TitleDesc->PropertyFlags &= ~CPF_DisableEditOnInstance;
		TitleDesc->PropertyFlags |= CPF_ExposeOnSpawn;
	}
	FBlueprintEditorUtils::SetBlueprintVariableMetaData(BaseBP, FName("Title"), nullptr, FBlueprintMetadata::MD_ExposeOnSpawn, TEXT("true"));
	FKismetEditorUtilities::CompileBlueprint(BaseBP);

	// 2. Create ChildBP inheriting from BaseBP with exposed-on-spawn UObject variable "ConfigObj"
	UPackage* ChildPkg = CreatePackage(TEXT("/Temp/BP_ConstructibleChild_T04"));
	UBlueprint* ChildBP = FKismetEditorUtilities::CreateBlueprint(
		BaseBP->GeneratedClass,
		ChildPkg,
		FName("BP_ConstructibleChild_T04"),
		BPTYPE_Normal,
		UBlueprint::StaticClass(),
		UBlueprintGeneratedClass::StaticClass());
	TestNotNull(TEXT("ChildBP created"), ChildBP);
	if (!ChildBP)
	{
		CleanupConstructionTestPackage(BasePkg);
		return false;
	}

	FEdGraphPinType ObjPinType;
	ObjPinType.PinCategory = UEdGraphSchema_K2::PC_Object;
	ObjPinType.PinSubCategoryObject = UObject::StaticClass();
	FBlueprintEditorUtils::AddMemberVariable(ChildBP, FName("ConfigObj"), ObjPinType);
	FBPVariableDescription* ConfigDesc = ChildBP->NewVariables.FindByPredicate([](const FBPVariableDescription& Desc)
	{
		return Desc.VarName == FName("ConfigObj");
	});
	if (ConfigDesc)
	{
		ConfigDesc->PropertyFlags &= ~CPF_DisableEditOnInstance;
		ConfigDesc->PropertyFlags |= CPF_ExposeOnSpawn;
	}
	FBlueprintEditorUtils::SetBlueprintVariableMetaData(ChildBP, FName("ConfigObj"), nullptr, FBlueprintMetadata::MD_ExposeOnSpawn, TEXT("true"));
	FKismetEditorUtilities::CompileBlueprint(ChildBP);

	// 3. Create ActorBP with UK2Node_GenericCreateObject
	UPackage* ActorPkg = CreatePackage(TEXT("/Temp/BP_TestActor_T04"));
	UBlueprint* ActorBP = FKismetEditorUtilities::CreateBlueprint(
		AActor::StaticClass(),
		ActorPkg,
		FName("BP_TestActor_T04"),
		BPTYPE_Normal,
		UBlueprint::StaticClass(),
		UBlueprintGeneratedClass::StaticClass());
	TestNotNull(TEXT("ActorBP created"), ActorBP);
	if (!ActorBP)
	{
		CleanupConstructionTestPackage(ChildPkg);
		CleanupConstructionTestPackage(BasePkg);
		return false;
	}

	UEdGraph* EventGraph = FBlueprintEditorUtils::FindEventGraph(ActorBP);
	TestNotNull(TEXT("EventGraph exists"), EventGraph);
	if (!EventGraph)
	{
		CleanupConstructionTestPackage(ActorPkg);
		CleanupConstructionTestPackage(ChildPkg);
		CleanupConstructionTestPackage(BasePkg);
		return false;
	}

	UK2Node_GenericCreateObject* CreateNode = NewObject<UK2Node_GenericCreateObject>(EventGraph);
	CreateNode->CreateNewGuid();
	EventGraph->AddNode(CreateNode);
	CreateNode->AllocateDefaultPins();

	UEdGraphPin* ClassPin = CreateNode->GetClassPin();
	TestNotNull(TEXT("ClassPin allocated"), ClassPin);

	// Save package to establish baseline file bytes
	const FString PackageFilename = FPackageName::LongPackageNameToFilename(
		ActorPkg->GetName(), FPackageName::GetAssetPackageExtension());
	FSavePackageArgs SaveArgs;
	SaveArgs.TopLevelFlags = RF_Public | RF_Standalone;
	const bool bSaved = UPackage::SavePackage(ActorPkg, ActorBP, *PackageFilename, SaveArgs);
	TestTrue(TEXT("Initial package saved to establish baseline"), bSaved);
	const int64 SavedBytesBefore = IFileManager::Get().FileSize(*PackageFilename);

	// 4. Feed Literal through PinDefaults on the class input
	TSharedPtr<FJsonObject> ClassLiteral = MakeShared<FJsonObject>();
	ClassLiteral->SetStringField(TEXT("kind"), TEXT("class"));
	ClassLiteral->SetStringField(TEXT("path"), ChildBP->GeneratedClass->GetPathName());

	FCortexCommandResult Error;
	const bool bApplied = FCortexGraphPinDefaults::ApplyDefault(ClassPin, ClassLiteral, Error);
	TestTrue(TEXT("ApplyDefault on class pin succeeds"), bApplied);

	// Re-resolve the node and pins after reconstruction (never retain pin pointers across ReconstructNode!)
	TestNotNull(TEXT("exposed-on-spawn Title pin allocated"), CreateNode->FindPin(TEXT("Title")));
	TestNotNull(TEXT("exposed-on-spawn ConfigObj pin allocated"), CreateNode->FindPin(TEXT("ConfigObj")));

	UEdGraphPin* ResultPin = CreateNode->GetResultPin();
	TestNotNull(TEXT("Result pin allocated"), ResultPin);
	if (ResultPin)
	{
		TestEqual(TEXT("Result pin typed to ChildBP"), ResultPin->PinType.PinSubCategoryObject.Get(), (UObject*)ChildBP->GeneratedClass);
	}

	UEdGraphPin* OuterPin = CreateNode->GetOuterPin();
	TestNotNull(TEXT("Outer pin allocated"), OuterPin);
	if (OuterPin)
	{
		TestEqual(TEXT("Outer pin typed to UObject"), OuterPin->PinType.PinSubCategoryObject.Get(), (UObject*)UObject::StaticClass());
	}

	const int64 SavedBytesAfter = IFileManager::Get().FileSize(*PackageFilename);
	TestEqual(TEXT("no helper save"), SavedBytesAfter, SavedBytesBefore);

	// 5. Test class change that reconstructs pins: change to BaseBP
	UEdGraphPin* CurrentClassPin = CreateNode->GetClassPin();
	TestNotNull(TEXT("Class pin found after reconstruction"), CurrentClassPin);
	TSharedPtr<FJsonObject> BaseClassLiteral = MakeShared<FJsonObject>();
	BaseClassLiteral->SetStringField(TEXT("kind"), TEXT("class"));
	BaseClassLiteral->SetStringField(TEXT("path"), BaseBP->GeneratedClass->GetPathName());

	const bool bBaseApplied = FCortexGraphPinDefaults::ApplyDefault(CurrentClassPin, BaseClassLiteral, Error);
	TestTrue(TEXT("ApplyDefault to BaseBP succeeds"), bBaseApplied);

	TestNotNull(TEXT("Title pin still exists on BaseBP"), CreateNode->FindPin(TEXT("Title")));
	TestNull(TEXT("ConfigObj pin removed when switched to BaseBP"), CreateNode->FindPin(TEXT("ConfigObj")));

	ResultPin = CreateNode->GetResultPin();
	TestNotNull(TEXT("Result pin exists after switch"), ResultPin);
	if (ResultPin)
	{
		TestEqual(TEXT("Result pin typed to BaseBP"), ResultPin->PinType.PinSubCategoryObject.Get(), (UObject*)BaseBP->GeneratedClass);
	}

	// 6. Test setting default on exposed-on-spawn Title pin
	UEdGraphPin* TitlePin = CreateNode->FindPin(TEXT("Title"));
	TestNotNull(TEXT("Title pin accessible for defaults"), TitlePin);
	if (TitlePin)
	{
		TSharedPtr<FJsonObject> TitleLiteral = MakeShared<FJsonObject>();
		TitleLiteral->SetStringField(TEXT("kind"), TEXT("text"));
		TitleLiteral->SetStringField(TEXT("literal"), TEXT("SpawnTitleValue"));

		const bool bTitleApplied = FCortexGraphPinDefaults::ApplyDefault(TitlePin, TitleLiteral, Error);
		TestTrue(TEXT("ApplyDefault on Title pin succeeds"), bTitleApplied);

		TSharedPtr<FJsonObject> ReadTitleDesc;
		const bool bRead = FCortexGraphPinDefaults::ReadDefault(TitlePin, ReadTitleDesc, Error);
		TestTrue(TEXT("ReadDefault on Title pin succeeds"), bRead);
		if (ReadTitleDesc.IsValid())
		{
			TestEqual(TEXT("Read default kind is text"), ReadTitleDesc->GetStringField(TEXT("kind")), TEXT("text"));
			TestEqual(TEXT("Read default text value"), TitlePin->DefaultTextValue.ToString(), TEXT("SpawnTitleValue"));
		}
	}

	// Cleanup
	IFileManager::Get().Delete(*PackageFilename);
	CleanupConstructionTestPackage(ActorPkg);
	CleanupConstructionTestPackage(ChildPkg);
	CleanupConstructionTestPackage(BasePkg);
	return true;
}

// ---------------------------------------------------------------------------
// Test 2: Rejections for invalid literals, wrong kinds, nonexistent paths, connected/output pins
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexGraphConstructionRejectionsTest,
	"Cortex.Graph.Authoring.Construction.Rejections",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexGraphConstructionRejectionsTest::RunTest(const FString& Parameters)
{
	UPackage* ActorPkg = CreatePackage(TEXT("/Temp/BP_RejectionsActor_T04"));
	UBlueprint* ActorBP = FKismetEditorUtilities::CreateBlueprint(
		AActor::StaticClass(),
		ActorPkg,
		FName("BP_RejectionsActor_T04"),
		BPTYPE_Normal,
		UBlueprint::StaticClass(),
		UBlueprintGeneratedClass::StaticClass());
	TestNotNull(TEXT("ActorBP created"), ActorBP);
	if (!ActorBP) return false;

	UEdGraph* EventGraph = FBlueprintEditorUtils::FindEventGraph(ActorBP);
	TestNotNull(TEXT("EventGraph exists"), EventGraph);
	if (!EventGraph)
	{
		CleanupConstructionTestPackage(ActorPkg);
		return false;
	}

	UK2Node_GenericCreateObject* CreateNode = NewObject<UK2Node_GenericCreateObject>(EventGraph);
	CreateNode->CreateNewGuid();
	EventGraph->AddNode(CreateNode);
	CreateNode->AllocateDefaultPins();

	UEdGraphPin* ClassPin = CreateNode->GetClassPin();
	UEdGraphPin* OuterPin = CreateNode->GetOuterPin();
	UEdGraphPin* ResultPin = CreateNode->GetResultPin();

	FCortexCommandResult Error;

	// 1. Reject abstract class
	TSharedPtr<FJsonObject> AbstractLiteral = MakeShared<FJsonObject>();
	AbstractLiteral->SetStringField(TEXT("kind"), TEXT("class"));
	AbstractLiteral->SetStringField(TEXT("path"), UActorComponent::StaticClass()->GetPathName());
	TestFalse(TEXT("Abstract class rejected"), FCortexGraphPinDefaults::Validate(ClassPin, AbstractLiteral, Error));
	TestTrue(TEXT("Error indicates abstract"), Error.ErrorMessage.Contains(TEXT("abstract")));

	// 2. Reject nonexistent class
	TSharedPtr<FJsonObject> NonexistentClass = MakeShared<FJsonObject>();
	NonexistentClass->SetStringField(TEXT("kind"), TEXT("class"));
	NonexistentClass->SetStringField(TEXT("path"), TEXT("/Engine/NonExistentClass_XYZ"));
	TestFalse(TEXT("Nonexistent class rejected"), FCortexGraphPinDefaults::Validate(ClassPin, NonexistentClass, Error));

	// 3. Reject nonexistent object path on object pin (without SkipPackage warnings)
	TSharedPtr<FJsonObject> NonexistentObject = MakeShared<FJsonObject>();
	NonexistentObject->SetStringField(TEXT("kind"), TEXT("object"));
	NonexistentObject->SetStringField(TEXT("path"), TEXT("/Game/NonExistent/Asset_XYZ.Asset_XYZ"));
	TestFalse(TEXT("Nonexistent object path rejected"), FCortexGraphPinDefaults::Validate(OuterPin, NonexistentObject, Error));

	// 4. Reject wrong kind for the pin category
	TSharedPtr<FJsonObject> WrongKindForClass = MakeShared<FJsonObject>();
	WrongKindForClass->SetStringField(TEXT("kind"), TEXT("int"));
	WrongKindForClass->SetNumberField(TEXT("value"), 42);
	TestFalse(TEXT("Int literal on class pin rejected"), FCortexGraphPinDefaults::Validate(ClassPin, WrongKindForClass, Error));

	// 5. Reject an object whose class does not match the pin's declared class
	UK2Node_CallFunction* FixtureNode = NewObject<UK2Node_CallFunction>(EventGraph);
	FixtureNode->CreateNewGuid();
	EventGraph->AddNode(FixtureNode);
	FixtureNode->AllocateDefaultPins();

	UEdGraphPin* TypedObjectPin = FixtureNode->CreatePin(
		EGPD_Input, UEdGraphSchema_K2::PC_Object, AActor::StaticClass(), TEXT("TypedObjectPin"));
	TestNotNull(TEXT("Typed object pin created"), TypedObjectPin);
	if (TypedObjectPin)
	{
		TSharedPtr<FJsonObject> WrongClassObject = MakeShared<FJsonObject>();
		WrongClassObject->SetStringField(TEXT("kind"), TEXT("object"));
		WrongClassObject->SetStringField(TEXT("path"), ActorBP->GetPathName());
		TestFalse(TEXT("Object of wrong class rejected"), FCortexGraphPinDefaults::Validate(TypedObjectPin, WrongClassObject, Error));
	}

	// 6. Reject an invalid enum value for an enum-typed pin
	UEdGraphPin* EnumPin = FixtureNode->CreatePin(
		EGPD_Input, UEdGraphSchema_K2::PC_Byte, StaticEnum<ECollisionChannel>(), TEXT("TypedEnumPin"));
	TestNotNull(TEXT("Typed enum pin created"), EnumPin);
	if (EnumPin)
	{
		TSharedPtr<FJsonObject> InvalidEnum = MakeShared<FJsonObject>();
		InvalidEnum->SetStringField(TEXT("kind"), TEXT("enum"));
		InvalidEnum->SetStringField(TEXT("value"), TEXT("ECC_DoesNotExistT04"));
		TestFalse(TEXT("Invalid enum value rejected"), FCortexGraphPinDefaults::Validate(EnumPin, InvalidEnum, Error));
	}

	// 7. Reject unsupported soft-reference modes
	TSharedPtr<FJsonObject> SoftObjectOnHardPin = MakeShared<FJsonObject>();
	SoftObjectOnHardPin->SetStringField(TEXT("kind"), TEXT("soft_object"));
	SoftObjectOnHardPin->SetStringField(TEXT("path"), AActor::StaticClass()->GetPathName());
	TestFalse(TEXT("Soft object literal on hard object pin rejected"), FCortexGraphPinDefaults::Validate(OuterPin, SoftObjectOnHardPin, Error));
	TestTrue(TEXT("Error indicates unsupported soft-reference mode"), Error.ErrorMessage.Contains(TEXT("soft-reference")));

	TSharedPtr<FJsonObject> SoftClassOnHardPin = MakeShared<FJsonObject>();
	SoftClassOnHardPin->SetStringField(TEXT("kind"), TEXT("soft_class"));
	SoftClassOnHardPin->SetStringField(TEXT("path"), AActor::StaticClass()->GetPathName());
	TestFalse(TEXT("Soft class literal on hard class pin rejected"), FCortexGraphPinDefaults::Validate(ClassPin, SoftClassOnHardPin, Error));

	// 8. Reject output pin defaults
	TSharedPtr<FJsonObject> ValidClassLiteral = MakeShared<FJsonObject>();
	ValidClassLiteral->SetStringField(TEXT("kind"), TEXT("class"));
	ValidClassLiteral->SetStringField(TEXT("path"), AActor::StaticClass()->GetPathName());
	TestFalse(TEXT("Output pin defaults rejected"), FCortexGraphPinDefaults::Validate(ResultPin, ValidClassLiteral, Error));

	// 9. Reject connected pin defaults
	// Connect OuterPin to another node's output pin
	UK2Node_CallFunction* CallNode = NewObject<UK2Node_CallFunction>(EventGraph);
	CallNode->CreateNewGuid();
	CallNode->FunctionReference.SetExternalMember(FName("GetActorForwardVector"), AActor::StaticClass());
	EventGraph->AddNode(CallNode);
	CallNode->AllocateDefaultPins();

	UEdGraphPin* SelfPin = CallNode->FindPin(UEdGraphSchema_K2::PN_Self);
	if (SelfPin && OuterPin)
	{
		SelfPin->MakeLinkTo(OuterPin);
		TestFalse(TEXT("Connected pin defaults rejected"), FCortexGraphPinDefaults::Validate(OuterPin, ValidClassLiteral, Error));
		OuterPin->BreakAllPinLinks();
	}

	// 10. Reject conflicting encodings
	TSharedPtr<FJsonObject> ConflictingText = MakeShared<FJsonObject>();
	ConflictingText->SetStringField(TEXT("kind"), TEXT("text"));
	ConflictingText->SetStringField(TEXT("literal"), TEXT("literal_value"));
	ConflictingText->SetStringField(TEXT("table"), TEXT("table_name"));
	ConflictingText->SetStringField(TEXT("key"), TEXT("key_name"));
	// Create a text pin by adding a PrintText node
	UK2Node_CallFunction* PrintTextNode = NewObject<UK2Node_CallFunction>(EventGraph);
	PrintTextNode->CreateNewGuid();
	PrintTextNode->FunctionReference.SetExternalMember(FName("PrintText"), UKismetSystemLibrary::StaticClass());
	EventGraph->AddNode(PrintTextNode);
	PrintTextNode->AllocateDefaultPins();

	UEdGraphPin* InTextPin = PrintTextNode->FindPin(TEXT("InText"));
	if (InTextPin)
	{
		TestFalse(TEXT("Conflicting text encoding rejected"), FCortexGraphPinDefaults::Validate(InTextPin, ConflictingText, Error));
	}

	CleanupConstructionTestPackage(ActorPkg);
	return true;
}

// ---------------------------------------------------------------------------
// Test 3: UK2Node_DynamicCast target class, pure/impure mode
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexGraphDynamicCastTest,
	"Cortex.Graph.Authoring.Construction.DynamicCast",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexGraphDynamicCastTest::RunTest(const FString& Parameters)
{
	UPackage* ActorPkg = CreatePackage(TEXT("/Temp/BP_DynamicCastActor_T04"));
	UBlueprint* ActorBP = FKismetEditorUtilities::CreateBlueprint(
		AActor::StaticClass(),
		ActorPkg,
		FName("BP_DynamicCastActor_T04"),
		BPTYPE_Normal,
		UBlueprint::StaticClass(),
		UBlueprintGeneratedClass::StaticClass());
	TestNotNull(TEXT("ActorBP created"), ActorBP);
	if (!ActorBP) return false;

	UEdGraph* EventGraph = FBlueprintEditorUtils::FindEventGraph(ActorBP);
	TestNotNull(TEXT("EventGraph exists"), EventGraph);
	if (!EventGraph)
	{
		CleanupConstructionTestPackage(ActorPkg);
		return false;
	}

	// 1. Pure cast with explicit target class
	UK2Node_DynamicCast* PureCastNode = NewObject<UK2Node_DynamicCast>(EventGraph);
	PureCastNode->CreateNewGuid();
	EventGraph->AddNode(PureCastNode);
	PureCastNode->AllocateDefaultPins();

	TSharedPtr<FJsonObject> PureParams = MakeShared<FJsonObject>();
	PureParams->SetStringField(TEXT("class"), TEXT("Actor"));
	PureParams->SetBoolField(TEXT("is_pure"), true);

	FString ErrorStr;
	TestTrue(TEXT("Apply pure cast params"), FCortexGraphNodeContract::ApplyNodeConstructionParams(
		EventGraph, PureCastNode, ActorBP, PureParams, ErrorStr));
	TestTrue(TEXT("DynamicCast is pure"), PureCastNode->IsNodePure());
	TestNull(TEXT("Pure cast has no valid cast exec pin"), PureCastNode->GetValidCastPin());
	TestNull(TEXT("Pure cast has no invalid cast exec pin"), PureCastNode->GetInvalidCastPin());
	TestEqual(TEXT("TargetType is AActor"), PureCastNode->TargetType.Get(), (UClass*)AActor::StaticClass());

	// 2. Impure cast
	UK2Node_DynamicCast* ImpureCastNode = NewObject<UK2Node_DynamicCast>(EventGraph);
	ImpureCastNode->CreateNewGuid();
	EventGraph->AddNode(ImpureCastNode);
	ImpureCastNode->AllocateDefaultPins();

	TSharedPtr<FJsonObject> ImpureParams = MakeShared<FJsonObject>();
	ImpureParams->SetStringField(TEXT("class"), TEXT("Actor"));
	ImpureParams->SetBoolField(TEXT("is_pure"), false);

	TestTrue(TEXT("Apply impure cast params"), FCortexGraphNodeContract::ApplyNodeConstructionParams(
		EventGraph, ImpureCastNode, ActorBP, ImpureParams, ErrorStr));
	TestFalse(TEXT("DynamicCast is impure"), ImpureCastNode->IsNodePure());
	TestNotNull(TEXT("Impure cast has valid cast exec pin"), ImpureCastNode->GetValidCastPin());
	TestNotNull(TEXT("Impure cast has invalid cast exec pin"), ImpureCastNode->GetInvalidCastPin());

	// 3. Generated-class target resolution
	UPackage* ModelPkg = CreatePackage(TEXT("/Temp/BP_CastTargetModel_T04"));
	UBlueprint* ModelBP = FKismetEditorUtilities::CreateBlueprint(
		UObject::StaticClass(),
		ModelPkg,
		FName("BP_CastTargetModel_T04"),
		BPTYPE_Normal,
		UBlueprint::StaticClass(),
		UBlueprintGeneratedClass::StaticClass());
	FKismetEditorUtilities::CompileBlueprint(ModelBP);

	TSharedPtr<FJsonObject> GenClassParams = MakeShared<FJsonObject>();
	GenClassParams->SetStringField(TEXT("class"), ModelBP->GeneratedClass->GetPathName());
	GenClassParams->SetBoolField(TEXT("is_pure"), true);

	UK2Node_DynamicCast* GenCastNode = NewObject<UK2Node_DynamicCast>(EventGraph);
	GenCastNode->CreateNewGuid();
	EventGraph->AddNode(GenCastNode);
	GenCastNode->AllocateDefaultPins();

	TestTrue(TEXT("Apply generated class cast params"), FCortexGraphNodeContract::ApplyNodeConstructionParams(
		EventGraph, GenCastNode, ActorBP, GenClassParams, ErrorStr));
	TestEqual(TEXT("TargetType matches generated class"), GenCastNode->TargetType.Get(), (UClass*)ModelBP->GeneratedClass);

	UEdGraphPin* ResultPin = GenCastNode->GetCastResultPin();
	TestNotNull(TEXT("Cast result pin exists"), ResultPin);
	if (ResultPin)
	{
		TestEqual(TEXT("Cast result pin typed to generated class"), ResultPin->PinType.PinSubCategoryObject.Get(), (UObject*)ModelBP->GeneratedClass);
	}

	CleanupConstructionTestPackage(ModelPkg);
	CleanupConstructionTestPackage(ActorPkg);
	return true;
}

// ---------------------------------------------------------------------------
// Test 4: FText / StringTable defaults via no-save helper
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexGraphTextNoSaveHelperTest,
	"Cortex.Graph.Authoring.Construction.TextNoSaveHelper",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexGraphTextNoSaveHelperTest::RunTest(const FString& Parameters)
{
	// 1. Create StringTable
	const FString TablePackageName = TEXT("/Game/Temp/TestTable_NoSaveHelper_T04");
	UPackage* TablePackage = CreatePackage(*TablePackageName);
	UStringTable* TestTable = NewObject<UStringTable>(
		TablePackage,
		FName(TEXT("TestTable_NoSaveHelper")),
		RF_Public | RF_Standalone);
	TestTable->GetMutableStringTable()->SetNamespace(TEXT("TestNS"));
	CortexEngineCompat::SetStringTableSourceString(
		*TestTable->GetMutableStringTable(), TEXT("UI.Spawn.Title"), TEXT("SpawnTitleValue"));

	// 2. Create ActorBP with PrintText node
	UPackage* ActorPkg = CreatePackage(TEXT("/Temp/BP_TextNoSaveActor_T04"));
	UBlueprint* ActorBP = FKismetEditorUtilities::CreateBlueprint(
		AActor::StaticClass(),
		ActorPkg,
		FName("BP_TextNoSaveActor_T04"),
		BPTYPE_Normal,
		UBlueprint::StaticClass(),
		UBlueprintGeneratedClass::StaticClass());
	TestNotNull(TEXT("ActorBP created"), ActorBP);
	if (!ActorBP)
	{
		CleanupConstructionTestPackage(TablePackage);
		return false;
	}

	UEdGraph* EventGraph = FBlueprintEditorUtils::FindEventGraph(ActorBP);
	UK2Node_CallFunction* PrintTextNode = NewObject<UK2Node_CallFunction>(EventGraph);
	PrintTextNode->CreateNewGuid();
	PrintTextNode->FunctionReference.SetExternalMember(FName("PrintText"), UKismetSystemLibrary::StaticClass());
	EventGraph->AddNode(PrintTextNode);
	PrintTextNode->AllocateDefaultPins();

	UEdGraphPin* InTextPin = PrintTextNode->FindPin(TEXT("InText"));
	TestNotNull(TEXT("InText pin exists"), InTextPin);
	if (!InTextPin)
	{
		CleanupConstructionTestPackage(ActorPkg);
		CleanupConstructionTestPackage(TablePackage);
		return false;
	}

	// 3. Save package to disk to establish baseline bytes
	const FString PackageFilename = FPackageName::LongPackageNameToFilename(
		ActorPkg->GetName(), FPackageName::GetAssetPackageExtension());
	FSavePackageArgs SaveArgs;
	SaveArgs.TopLevelFlags = RF_Public | RF_Standalone;
	const bool bSaved = UPackage::SavePackage(ActorPkg, ActorBP, *PackageFilename, SaveArgs);
	TestTrue(TEXT("Package saved for baseline"), bSaved);
	const int64 SavedBytesBefore = IFileManager::Get().FileSize(*PackageFilename);

	// 4. Create an unrelated dirty sentinel package
	UPackage* SentinelPkg = CreatePackage(TEXT("/Temp/Sentinel_TextNoSave_T04"));
	SentinelPkg->SetDirtyFlag(true);
	TestTrue(TEXT("Sentinel package is dirty"), SentinelPkg->IsDirty());

	// 5. Apply StringTable FText default via no-save helper
	TSharedPtr<FJsonObject> TextLiteral = MakeShared<FJsonObject>();
	TextLiteral->SetStringField(TEXT("kind"), TEXT("text"));
	TextLiteral->SetStringField(TEXT("source"), TEXT("SpawnTitleValue"));
	TextLiteral->SetStringField(TEXT("table"), TestTable->GetStringTableId().ToString());
	TextLiteral->SetStringField(TEXT("key"), TEXT("UI.Spawn.Title"));

	FCortexCommandResult Error;
	const bool bApplied = FCortexGraphPinDefaults::ApplyDefault(InTextPin, TextLiteral, Error);
	TestTrue(TEXT("ApplyDefault for FText StringTable succeeds"), bApplied);

	// 6. Assert source/table/key identity
	TSharedPtr<FJsonObject> ReadDesc;
	TestTrue(TEXT("ReadDefault succeeds"), FCortexGraphPinDefaults::ReadDefault(InTextPin, ReadDesc, Error));
	if (ReadDesc.IsValid())
	{
		TestEqual(TEXT("Read descriptor kind is text"), ReadDesc->GetStringField(TEXT("kind")), TEXT("text"));
		const TSharedPtr<FJsonObject>* ValueObj = nullptr;
		if (ReadDesc->TryGetObjectField(TEXT("value"), ValueObj) && ValueObj)
		{
			TestEqual(TEXT("source_kind is string_table"), (*ValueObj)->GetStringField(TEXT("source_kind")), TEXT("string_table"));
			const TSharedPtr<FJsonObject>* TableObj = nullptr;
			if ((*ValueObj)->TryGetObjectField(TEXT("string_table"), TableObj) && TableObj)
			{
				TestEqual(TEXT("table_id matches"), (*TableObj)->GetStringField(TEXT("table_id")), TestTable->GetStringTableId().ToString());
				TestEqual(TEXT("key matches"), (*TableObj)->GetStringField(TEXT("key")), TEXT("UI.Spawn.Title"));
			}
		}
	}

	// 7. Assert unchanged file bytes (strictly no persistence in helper!)
	const int64 SavedBytesAfter = IFileManager::Get().FileSize(*PackageFilename);
	TestEqual(TEXT("Saved bytes unchanged (no helper save)"), SavedBytesAfter, SavedBytesBefore);

	// 8. Assert unrelated dirty sentinel is still dirty
	TestTrue(TEXT("Sentinel package remains dirty"), SentinelPkg->IsDirty());

	// Cleanup
	IFileManager::Get().Delete(*PackageFilename);
	CleanupConstructionTestPackage(SentinelPkg);
	CleanupConstructionTestPackage(ActorPkg);
	CleanupConstructionTestPackage(TablePackage);
	return true;
}

#endif // WITH_EDITOR
