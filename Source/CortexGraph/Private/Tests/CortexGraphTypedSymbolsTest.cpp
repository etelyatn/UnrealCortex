#include "Misc/AutomationTest.h"
#include "Operations/CortexGraphNodeContract.h"
#include "Operations/CortexGraphNodeOps.h"
#include "Operations/CortexGraphSymbolResolver.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "EdGraphSchema_K2.h"
#include "K2Node_CallFunction.h"
#include "K2Node_VariableGet.h"
#include "K2Node_VariableSet.h"
#include "GameFramework/Actor.h"
#include "GameFramework/Pawn.h"
#include "Kismet/KismetSystemLibrary.h"
#include "UObject/Interface.h"
#include "Engine/BlendableInterface.h"
#include "Editor.h"
#include "Editor/Transactor.h"

#if WITH_EDITOR

namespace
{
void CleanupTestPackage(UPackage* Pkg)
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

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexGraphSymbolsFamilyResolutionTest,
	"Cortex.Graph.Authoring.Symbols.Families",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexGraphSymbolsFamilyResolutionTest::RunTest(const FString& Parameters)
{
	// All previously accepted family identifiers/aliases used by composer and canonical class paths
	const TArray<FString> AcceptedIdentifiers = {
		TEXT("CallFunction"),
		TEXT("UK2Node_CallFunction"),
		TEXT("/Script/BlueprintGraph.K2Node_CallFunction"),
		TEXT("IfThenElse"),
		TEXT("UK2Node_IfThenElse"),
		TEXT("Branch"),
		TEXT("/Script/BlueprintGraph.K2Node_IfThenElse"),
		TEXT("VariableSet"),
		TEXT("UK2Node_VariableSet"),
		TEXT("/Script/BlueprintGraph.K2Node_VariableSet"),
		TEXT("VariableGet"),
		TEXT("UK2Node_VariableGet"),
		TEXT("/Script/BlueprintGraph.K2Node_VariableGet"),
		TEXT("Event"),
		TEXT("UK2Node_Event"),
		TEXT("/Script/BlueprintGraph.K2Node_Event"),
		TEXT("ExecutionSequence"),
		TEXT("UK2Node_ExecutionSequence"),
		TEXT("Sequence"),
		TEXT("/Script/BlueprintGraph.K2Node_ExecutionSequence"),
		TEXT("CustomEvent"),
		TEXT("UK2Node_CustomEvent"),
		TEXT("/Script/BlueprintGraph.K2Node_CustomEvent"),
		TEXT("Self"),
		TEXT("UK2Node_Self"),
		TEXT("/Script/BlueprintGraph.K2Node_Self"),
		TEXT("Knot"),
		TEXT("UK2Node_Knot"),
		TEXT("Reroute"),
		TEXT("/Script/BlueprintGraph.K2Node_Knot"),
		TEXT("MakeArray"),
		TEXT("UK2Node_MakeArray"),
		TEXT("/Script/BlueprintGraph.K2Node_MakeArray"),
		TEXT("Timeline"),
		TEXT("UK2Node_Timeline"),
		TEXT("/Script/BlueprintGraph.K2Node_Timeline"),
		TEXT("SpawnActorFromClass"),
		TEXT("UK2Node_SpawnActorFromClass"),
		TEXT("SpawnActor"),
		TEXT("/Script/BlueprintGraph.K2Node_SpawnActorFromClass"),
		TEXT("DynamicCast"),
		TEXT("UK2Node_DynamicCast"),
		TEXT("CastTo"),
		TEXT("Cast"),
		TEXT("/Script/BlueprintGraph.K2Node_DynamicCast"),
		TEXT("MacroInstance"),
		TEXT("UK2Node_MacroInstance"),
		TEXT("/Script/BlueprintGraph.K2Node_MacroInstance"),
		TEXT("SwitchEnum"),
		TEXT("UK2Node_SwitchEnum"),
		TEXT("/Script/BlueprintGraph.K2Node_SwitchEnum"),
		TEXT("SwitchString"),
		TEXT("UK2Node_SwitchString"),
		TEXT("/Script/BlueprintGraph.K2Node_SwitchString"),
		TEXT("SwitchInteger"),
		TEXT("UK2Node_SwitchInteger"),
		TEXT("/Script/BlueprintGraph.K2Node_SwitchInteger"),
		TEXT("AddDelegate"),
		TEXT("UK2Node_AddDelegate"),
		TEXT("BindEvent"),
		TEXT("/Script/BlueprintGraph.K2Node_AddDelegate"),
		TEXT("RemoveDelegate"),
		TEXT("UK2Node_RemoveDelegate"),
		TEXT("UnbindEvent"),
		TEXT("/Script/BlueprintGraph.K2Node_RemoveDelegate"),
		TEXT("ClearDelegate"),
		TEXT("UK2Node_ClearDelegate"),
		TEXT("UnbindAllEvents"),
		TEXT("/Script/BlueprintGraph.K2Node_ClearDelegate"),
		TEXT("CreateDelegate"),
		TEXT("UK2Node_CreateDelegate"),
		TEXT("CreateEvent"),
		TEXT("/Script/BlueprintGraph.K2Node_CreateDelegate"),
		TEXT("Composite"),
		TEXT("UK2Node_Composite"),
		TEXT("/Script/BlueprintGraph.K2Node_Composite"),
		TEXT("ConstructObject"),
		TEXT("GenericCreateObject"),
		TEXT("UK2Node_GenericCreateObject"),
		TEXT("/Script/BlueprintGraph.K2Node_GenericCreateObject")
	};

	for (const FString& Identifier : AcceptedIdentifiers)
	{
		FCortexNodeConstructionContract Contract = FCortexGraphNodeContract::Describe(Identifier);
		TestTrue(FString::Printf(TEXT("Describe recognizes family/alias: %s"), *Identifier), Contract.bSupported);
		TestFalse(FString::Printf(TEXT("ResolvedClass is not empty for: %s"), *Identifier), Contract.ResolvedClass.IsEmpty());

		UClass* ResolvedClass = FCortexGraphNodeOps::ResolveNodeClass(Identifier);
		TestNotNull(FString::Printf(TEXT("ResolveNodeClass succeeds for: %s"), *Identifier), ResolvedClass);
	}

	// Unknown family should fail
	{
		FCortexNodeConstructionContract BadContract = FCortexGraphNodeContract::Describe(TEXT("NonExistentFamily_12345"));
		TestFalse(TEXT("Describe rejects unknown family"), BadContract.bSupported);

		UClass* BadClass = FCortexGraphNodeOps::ResolveNodeClass(TEXT("NonExistentFamily_12345"));
		TestNull(TEXT("ResolveNodeClass returns null for unknown family"), BadClass);
	}

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexGraphSymbolsCanonicalSelectorsTest,
	"Cortex.Graph.Authoring.Symbols.CanonicalSelectors",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexGraphSymbolsCanonicalSelectorsTest::RunTest(const FString& Parameters)
{
	UPackage* Pkg = CreatePackage(TEXT("/Temp/BP_SymbolsTest_BP"));
	UBlueprint* Blueprint = FKismetEditorUtilities::CreateBlueprint(
		AActor::StaticClass(), Pkg, FName("BP_SymbolsTest_BP"),
		BPTYPE_Normal, UBlueprint::StaticClass(), UBlueprintGeneratedClass::StaticClass());

	// 1. Canonical qualified selector validates
	{
		TSharedPtr<FJsonObject> NodeParams = MakeShared<FJsonObject>();
		NodeParams->SetStringField(TEXT("owner_class"), TEXT("/Script/Engine.KismetSystemLibrary"));
		NodeParams->SetStringField(TEXT("function_name"), TEXT("PrintString"));
		FCortexCommandResult Error;
		TestTrue(TEXT("canonical qualified selector validates"),
			FCortexGraphNodeContract::Validate(TEXT("CallFunction"), Blueprint, NodeParams, Error));
	}

	// 2. Unknown function rejected
	{
		TSharedPtr<FJsonObject> NodeParams = MakeShared<FJsonObject>();
		NodeParams->SetStringField(TEXT("owner_class"), TEXT("/Script/Engine.KismetSystemLibrary"));
		NodeParams->SetStringField(TEXT("function_name"), TEXT("FunctionThatDoesNotExist"));
		FCortexCommandResult Error;
		TestFalse(TEXT("unknown function rejected"),
			FCortexGraphNodeContract::Validate(TEXT("CallFunction"), Blueprint, NodeParams, Error));
	}

	// 3. Non-existent package guarded
	{
		TSharedPtr<FJsonObject> NodeParams = MakeShared<FJsonObject>();
		NodeParams->SetStringField(TEXT("owner_class"), TEXT("/Script/NonExistentPackage.FakeClass"));
		NodeParams->SetStringField(TEXT("function_name"), TEXT("SomeFunction"));
		FCortexCommandResult Error;
		TestFalse(TEXT("non-existent package rejected cleanly"),
			FCortexGraphNodeContract::Validate(TEXT("CallFunction"), Blueprint, NodeParams, Error));
	}

	// 4. Object path not split at first dot
	{
		UClass* FoundClass = nullptr;
		FCortexCommandResult Error;
		TestTrue(TEXT("ResolveClass handles path with dot correctly"),
			FCortexGraphSymbolResolver::ResolveClass(TEXT("/Script/Engine.KismetSystemLibrary"), FoundClass, Error));
		TestEqual(TEXT("Resolved class matches UKismetSystemLibrary"), FoundClass, UKismetSystemLibrary::StaticClass());
	}

	// 5. Legacy combined selector still supported
	{
		TSharedPtr<FJsonObject> NodeParams = MakeShared<FJsonObject>();
		NodeParams->SetStringField(TEXT("function_name"), TEXT("KismetSystemLibrary.PrintString"));
		FCortexCommandResult Error;
		TestTrue(TEXT("legacy combined selector validates"),
			FCortexGraphNodeContract::Validate(TEXT("CallFunction"), Blueprint, NodeParams, Error));
	}

	CleanupTestPackage(Pkg);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexGraphSymbolsOwnersAndInheritanceTest,
	"Cortex.Graph.Authoring.Symbols.OwnersAndInheritance",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexGraphSymbolsOwnersAndInheritanceTest::RunTest(const FString& Parameters)
{
	UPackage* Pkg = CreatePackage(TEXT("/Temp/BP_Inheritance_Test"));
	UBlueprint* Blueprint = FKismetEditorUtilities::CreateBlueprint(
		APawn::StaticClass(), Pkg, FName("BP_Inheritance_Test"),
		BPTYPE_Normal, UBlueprint::StaticClass(), UBlueprintGeneratedClass::StaticClass());

	// 1. Inherited function: K2_DestroyActor on Pawn (declared on Actor)
	{
		TSharedPtr<FJsonObject> NodeParams = MakeShared<FJsonObject>();
		NodeParams->SetStringField(TEXT("owner_class"), TEXT("/Script/Engine.Pawn"));
		NodeParams->SetStringField(TEXT("function_name"), TEXT("K2_DestroyActor"));
		FCortexCommandResult Error;
		TestTrue(TEXT("inherited function on Pawn validates"),
			FCortexGraphNodeContract::Validate(TEXT("CallFunction"), Blueprint, NodeParams, Error));

		FCortexResolvedSymbol Symbol;
		TestTrue(TEXT("ResolveFunction resolves inherited function"),
			FCortexGraphSymbolResolver::ResolveFunction(Blueprint, NodeParams, Symbol, Error));
		TestEqual(TEXT("Declaring class is Actor"), Symbol.DeclaringClassPath, FString(TEXT("/Script/Engine.Actor")));
		TestEqual(TEXT("Context class is Pawn"), Symbol.ContextClassPath, FString(TEXT("/Script/Engine.Pawn")));
	}

	// 2. Inherited property: bHidden on Pawn (declared on Actor)
	{
		TSharedPtr<FJsonObject> NodeParams = MakeShared<FJsonObject>();
		NodeParams->SetStringField(TEXT("owner_class"), TEXT("/Script/Engine.Pawn"));
		NodeParams->SetStringField(TEXT("variable_name"), TEXT("bHidden"));
		FCortexCommandResult Error;
		TestTrue(TEXT("inherited property on Pawn validates"),
			FCortexGraphNodeContract::Validate(TEXT("VariableGet"), Blueprint, NodeParams, Error));

		FCortexResolvedSymbol Symbol;
		TestTrue(TEXT("ResolveProperty resolves inherited property"),
			FCortexGraphSymbolResolver::ResolveProperty(Blueprint, NodeParams, false, Symbol, Error));
		TestEqual(TEXT("Declaring class is Actor"), Symbol.DeclaringClassPath, FString(TEXT("/Script/Engine.Actor")));
	}

	// 3. Blueprint-generated owner
	{
		UPackage* TargetPkg = CreatePackage(TEXT("/Temp/BP_TargetClass_T02"));
		UBlueprint* TargetBP = FKismetEditorUtilities::CreateBlueprint(
			AActor::StaticClass(), TargetPkg, FName("BP_TargetClass_T02"),
			BPTYPE_Normal, UBlueprint::StaticClass(), UBlueprintGeneratedClass::StaticClass());

		// Add a variable
		FEdGraphPinType PinType;
		PinType.PinCategory = UEdGraphSchema_K2::PC_Int;
		FBlueprintEditorUtils::AddMemberVariable(TargetBP, FName("TargetVar"), PinType);

		// Add a function
		UEdGraph* FuncGraph = FBlueprintEditorUtils::CreateNewGraph(TargetBP, FName("TargetFunc"), UEdGraph::StaticClass(), UEdGraphSchema_K2::StaticClass());
		FBlueprintEditorUtils::AddFunctionGraph<UClass>(TargetBP, FuncGraph, false, nullptr);
		FKismetEditorUtilities::CompileBlueprint(TargetBP);

		// Resolve function on Blueprint generated class
		{
			TSharedPtr<FJsonObject> NodeParams = MakeShared<FJsonObject>();
			NodeParams->SetStringField(TEXT("owner_class"), TargetBP->GeneratedClass->GetPathName());
			NodeParams->SetStringField(TEXT("function_name"), TEXT("TargetFunc"));
			FCortexCommandResult Error;
			TestTrue(TEXT("function on generated class validates"),
				FCortexGraphNodeContract::Validate(TEXT("CallFunction"), Blueprint, NodeParams, Error));
		}

		// Resolve variable on Blueprint generated class
		{
			TSharedPtr<FJsonObject> NodeParams = MakeShared<FJsonObject>();
			NodeParams->SetStringField(TEXT("owner_class"), TargetBP->GeneratedClass->GetPathName());
			NodeParams->SetStringField(TEXT("variable_name"), TEXT("TargetVar"));
			FCortexCommandResult Error;
			TestTrue(TEXT("variable on generated class validates"),
				FCortexGraphNodeContract::Validate(TEXT("VariableGet"), Blueprint, NodeParams, Error));
		}

		CleanupTestPackage(TargetPkg);
	}

	CleanupTestPackage(Pkg);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexGraphSymbolsAccessibilityTest,
	"Cortex.Graph.Authoring.Symbols.Accessibility",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexGraphSymbolsAccessibilityTest::RunTest(const FString& Parameters)
{
	UPackage* Pkg = CreatePackage(TEXT("/Temp/BP_Access_Test"));
	UBlueprint* Blueprint = FKismetEditorUtilities::CreateBlueprint(
		AActor::StaticClass(), Pkg, FName("BP_Access_Test"),
		BPTYPE_Normal, UBlueprint::StaticClass(), UBlueprintGeneratedClass::StaticClass());

	// 1. Inaccessible function: non-BlueprintCallable native function
	// AActor::Tick is not BlueprintCallable
	{
		TSharedPtr<FJsonObject> NodeParams = MakeShared<FJsonObject>();
		NodeParams->SetStringField(TEXT("owner_class"), TEXT("/Script/Engine.Actor"));
		NodeParams->SetStringField(TEXT("function_name"), TEXT("Tick"));
		FCortexCommandResult Error;
		TestFalse(TEXT("non-BlueprintCallable function rejected"),
			FCortexGraphNodeContract::Validate(TEXT("CallFunction"), Blueprint, NodeParams, Error));
	}

	// 2. BlueprintReadOnly property write rejected on VariableSet
	// AActor::InitialLifeSpan has CPF_BlueprintReadOnly
	{
		TSharedPtr<FJsonObject> NodeParams = MakeShared<FJsonObject>();
		NodeParams->SetStringField(TEXT("owner_class"), TEXT("/Script/Engine.Actor"));
		NodeParams->SetStringField(TEXT("variable_name"), TEXT("InitialLifeSpan"));
		FCortexCommandResult Error;

		// Reading is allowed
		TestTrue(TEXT("VariableGet on BlueprintReadOnly property succeeds"),
			FCortexGraphNodeContract::Validate(TEXT("VariableGet"), Blueprint, NodeParams, Error));

		// Writing is rejected
		TestFalse(TEXT("VariableSet on BlueprintReadOnly property rejected"),
			FCortexGraphNodeContract::Validate(TEXT("VariableSet"), Blueprint, NodeParams, Error));
	}

	// 3. Blueprint member variable with BlueprintReadOnly flag
	{
		FEdGraphPinType PinType;
		PinType.PinCategory = UEdGraphSchema_K2::PC_Int;
		FBlueprintEditorUtils::AddMemberVariable(Blueprint, FName("ReadOnlyVar"), PinType);
		FBPVariableDescription* VarDesc = Blueprint->NewVariables.FindByPredicate(
			[](const FBPVariableDescription& Desc) { return Desc.VarName == FName("ReadOnlyVar"); });
		if (VarDesc)
		{
			VarDesc->PropertyFlags |= CPF_BlueprintReadOnly;
		}

		TSharedPtr<FJsonObject> NodeParams = MakeShared<FJsonObject>();
		NodeParams->SetStringField(TEXT("variable_name"), TEXT("ReadOnlyVar"));
		FCortexCommandResult Error;

		TestTrue(TEXT("VariableGet on self BlueprintReadOnly succeeds"),
			FCortexGraphNodeContract::Validate(TEXT("VariableGet"), Blueprint, NodeParams, Error));

		TestFalse(TEXT("VariableSet on self BlueprintReadOnly rejected"),
			FCortexGraphNodeContract::Validate(TEXT("VariableSet"), Blueprint, NodeParams, Error));
	}

	CleanupTestPackage(Pkg);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexGraphSymbolsConflictingSelectorsTest,
	"Cortex.Graph.Authoring.Symbols.ConflictingSelectors",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexGraphSymbolsConflictingSelectorsTest::RunTest(const FString& Parameters)
{
	UPackage* Pkg = CreatePackage(TEXT("/Temp/BP_Conflict_Test"));
	UBlueprint* Blueprint = FKismetEditorUtilities::CreateBlueprint(
		AActor::StaticClass(), Pkg, FName("BP_Conflict_Test"),
		BPTYPE_Normal, UBlueprint::StaticClass(), UBlueprintGeneratedClass::StaticClass());

	// 1. Empty selectors rejected
	{
		TSharedPtr<FJsonObject> NodeParams = MakeShared<FJsonObject>();
		FCortexCommandResult Error;
		TestFalse(TEXT("empty function selector rejected"),
			FCortexGraphNodeContract::Validate(TEXT("CallFunction"), Blueprint, NodeParams, Error));
		TestFalse(TEXT("empty variable selector rejected"),
			FCortexGraphNodeContract::Validate(TEXT("VariableGet"), Blueprint, NodeParams, Error));
	}

	// 2. Conflicting owner_class and combined function_name
	{
		TSharedPtr<FJsonObject> NodeParams = MakeShared<FJsonObject>();
		NodeParams->SetStringField(TEXT("owner_class"), TEXT("/Script/Engine.Actor"));
		NodeParams->SetStringField(TEXT("function_name"), TEXT("KismetSystemLibrary.PrintString"));
		FCortexCommandResult Error;
		TestFalse(TEXT("conflicting owner_class and combined selector rejected"),
			FCortexGraphNodeContract::Validate(TEXT("CallFunction"), Blueprint, NodeParams, Error));
	}

	// 3. Conflicting owner_class and variable_class
	{
		TSharedPtr<FJsonObject> NodeParams = MakeShared<FJsonObject>();
		NodeParams->SetStringField(TEXT("owner_class"), TEXT("/Script/Engine.Actor"));
		NodeParams->SetStringField(TEXT("variable_class"), TEXT("/Script/Engine.Pawn"));
		NodeParams->SetStringField(TEXT("variable_name"), TEXT("bHidden"));
		FCortexCommandResult Error;
		TestFalse(TEXT("conflicting owner_class and variable_class rejected"),
			FCortexGraphNodeContract::Validate(TEXT("VariableGet"), Blueprint, NodeParams, Error));
	}

	// 4. Malformed combined selector (multiple dots without leading slash)
	{
		TSharedPtr<FJsonObject> NodeParams = MakeShared<FJsonObject>();
		NodeParams->SetStringField(TEXT("function_name"), TEXT("Foo.Bar.Baz"));
		FCortexCommandResult Error;
		TestFalse(TEXT("malformed combined selector rejected"),
			FCortexGraphNodeContract::Validate(TEXT("CallFunction"), Blueprint, NodeParams, Error));
	}

	CleanupTestPackage(Pkg);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexGraphSymbolsCallKindsTest,
	"Cortex.Graph.Authoring.Symbols.CallKinds",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexGraphSymbolsCallKindsTest::RunTest(const FString& Parameters)
{
	UPackage* Pkg = CreatePackage(TEXT("/Temp/BP_CallKinds_Test"));
	UBlueprint* Blueprint = FKismetEditorUtilities::CreateBlueprint(
		AActor::StaticClass(), Pkg, FName("BP_CallKinds_Test"),
		BPTYPE_Normal, UBlueprint::StaticClass(), UBlueprintGeneratedClass::StaticClass());

	// 1. Ordinary call kind
	{
		TSharedPtr<FJsonObject> NodeParams = MakeShared<FJsonObject>();
		NodeParams->SetStringField(TEXT("owner_class"), TEXT("/Script/Engine.KismetSystemLibrary"));
		NodeParams->SetStringField(TEXT("function_name"), TEXT("PrintString"));
		NodeParams->SetStringField(TEXT("call_kind"), TEXT("ordinary"));
		FCortexCommandResult Error;
		TestTrue(TEXT("ordinary call kind validates"),
			FCortexGraphNodeContract::Validate(TEXT("CallFunction"), Blueprint, NodeParams, Error));

		FCortexResolvedSymbol Symbol;
		TestTrue(TEXT("SymbolResolver identifies ordinary call"),
			FCortexGraphSymbolResolver::ResolveFunction(Blueprint, NodeParams, Symbol, Error));
		TestEqual(TEXT("CallKind is Ordinary"), Symbol.CallKind, ECortexCallKind::Ordinary);
	}

	// 2. Parent call rejected on ordinary CallFunction
	{
		TSharedPtr<FJsonObject> NodeParams = MakeShared<FJsonObject>();
		NodeParams->SetStringField(TEXT("owner_class"), TEXT("/Script/Engine.Actor"));
		NodeParams->SetStringField(TEXT("function_name"), TEXT("K2_DestroyActor"));
		NodeParams->SetStringField(TEXT("call_kind"), TEXT("parent"));
		FCortexCommandResult Error;
		TestFalse(TEXT("parent call kind rejected on CallFunction"),
			FCortexGraphNodeContract::Validate(TEXT("CallFunction"), Blueprint, NodeParams, Error));
	}

	// 3. Unsupported / invalid call kind rejected
	{
		TSharedPtr<FJsonObject> NodeParams = MakeShared<FJsonObject>();
		NodeParams->SetStringField(TEXT("owner_class"), TEXT("/Script/Engine.KismetSystemLibrary"));
		NodeParams->SetStringField(TEXT("function_name"), TEXT("PrintString"));
		NodeParams->SetStringField(TEXT("call_kind"), TEXT("unsupported_magic"));
		FCortexCommandResult Error;
		TestFalse(TEXT("unsupported call kind rejected"),
			FCortexGraphNodeContract::Validate(TEXT("CallFunction"), Blueprint, NodeParams, Error));
	}

	// 4. Interface function identified as InterfaceMessage
	{
		TSharedPtr<FJsonObject> NodeParams = MakeShared<FJsonObject>();
		NodeParams->SetStringField(TEXT("owner_class"), TEXT("/Script/Engine.BlendableInterface"));
		NodeParams->SetStringField(TEXT("function_name"), TEXT("SomeInterfaceFunction"));
		// BlendableInterface has no BP functions, but test interface class detection
		UClass* BlendClass = nullptr;
		FCortexCommandResult Error;
		TestTrue(TEXT("ResolveClass resolves UBlendableInterface"),
			FCortexGraphSymbolResolver::ResolveClass(TEXT("/Script/Engine.BlendableInterface"), BlendClass, Error));
		TestTrue(TEXT("BlendableInterface is interface"), BlendClass && BlendClass->IsChildOf(UInterface::StaticClass()));
	}

	CleanupTestPackage(Pkg);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexGraphSymbolsRebuildNoStalePointerTest,
	"Cortex.Graph.Authoring.Symbols.RebuildNoStalePointer",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexGraphSymbolsRebuildNoStalePointerTest::RunTest(const FString& Parameters)
{
	UPackage* Pkg = CreatePackage(TEXT("/Temp/BP_Rebuild_Test"));
	UBlueprint* Blueprint = FKismetEditorUtilities::CreateBlueprint(
		AActor::StaticClass(), Pkg, FName("BP_Rebuild_Test"),
		BPTYPE_Normal, UBlueprint::StaticClass(), UBlueprintGeneratedClass::StaticClass());

	FEdGraphPinType PinType;
	PinType.PinCategory = UEdGraphSchema_K2::PC_Int;
	FBlueprintEditorUtils::AddMemberVariable(Blueprint, FName("LiveVar"), PinType);
	FKismetEditorUtilities::CompileBlueprint(Blueprint);

	// First resolution
	TSharedPtr<FJsonObject> NodeParams = MakeShared<FJsonObject>();
	NodeParams->SetStringField(TEXT("owner_class"), Blueprint->GeneratedClass->GetPathName());
	NodeParams->SetStringField(TEXT("variable_name"), TEXT("LiveVar"));

	FCortexResolvedSymbol Symbol1;
	FCortexCommandResult Error1;
	TestTrue(TEXT("First resolution succeeds"),
		FCortexGraphSymbolResolver::ResolveProperty(Blueprint, NodeParams, false, Symbol1, Error1));

	// Recompile blueprint (reinstancing/regenerating class)
	FKismetEditorUtilities::CompileBlueprint(Blueprint);

	// Second resolution using canonical descriptor string
	TSharedPtr<FJsonObject> DescriptorParams = MakeShared<FJsonObject>();
	DescriptorParams->SetStringField(TEXT("owner_class"), Symbol1.DeclaringClassPath);
	DescriptorParams->SetStringField(TEXT("variable_name"), Symbol1.MemberName.ToString());

	FCortexResolvedSymbol Symbol2;
	FCortexCommandResult Error2;
	TestTrue(TEXT("Second resolution after recompile succeeds with fresh pointer"),
		FCortexGraphSymbolResolver::ResolveProperty(Blueprint, DescriptorParams, false, Symbol2, Error2));
	TestNotNull(TEXT("Fresh property pointer is valid"), Symbol2.Property);

	CleanupTestPackage(Pkg);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexGraphSymbolsAmbiguityTest,
	"Cortex.Graph.Authoring.Symbols.Ambiguity",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexGraphSymbolsAmbiguityTest::RunTest(const FString& Parameters)
{
	UPackage* PkgA = CreatePackage(TEXT("/Temp/PkgA"));
	UBlueprint* BPA = FKismetEditorUtilities::CreateBlueprint(
		AActor::StaticClass(), PkgA, FName("BP_CollisionTest"),
		BPTYPE_Normal, UBlueprint::StaticClass(), UBlueprintGeneratedClass::StaticClass());
	FKismetEditorUtilities::CompileBlueprint(BPA);

	UPackage* PkgB = CreatePackage(TEXT("/Temp/PkgB"));
	UBlueprint* BPB = FKismetEditorUtilities::CreateBlueprint(
		AActor::StaticClass(), PkgB, FName("BP_CollisionTest"),
		BPTYPE_Normal, UBlueprint::StaticClass(), UBlueprintGeneratedClass::StaticClass());
	FKismetEditorUtilities::CompileBlueprint(BPB);

	UPackage* ConsumerPkg = CreatePackage(TEXT("/Temp/BP_Ambiguity_Consumer"));
	UBlueprint* ConsumerBP = FKismetEditorUtilities::CreateBlueprint(
		AActor::StaticClass(), ConsumerPkg, FName("BP_Ambiguity_Consumer"),
		BPTYPE_Normal, UBlueprint::StaticClass(), UBlueprintGeneratedClass::StaticClass());

	// 1. Short name resolution fails with ambiguity error and returns candidate paths
	{
		UClass* ResolvedClass = nullptr;
		FCortexCommandResult Error;
		TArray<FString> Candidates;
		const bool bResolved = FCortexGraphSymbolResolver::ResolveClass(TEXT("BP_CollisionTest"), ResolvedClass, Error, &Candidates);
		TestFalse(TEXT("Short name BP_CollisionTest is ambiguous and fails resolution"), bResolved);
		TestEqual(TEXT("Error code is INVALID_FIELD"), Error.ErrorCode, CortexErrorCodes::InvalidField);
		TestTrue(TEXT("Error details has candidates array"), Error.ErrorDetails.IsValid() && Error.ErrorDetails->HasField(TEXT("candidates")));
		if (Error.ErrorDetails.IsValid())
		{
			const TArray<TSharedPtr<FJsonValue>>* CandArray = nullptr;
			if (Error.ErrorDetails->TryGetArrayField(TEXT("candidates"), CandArray))
			{
				TestTrue(TEXT("Candidates array has at least 2 entries"), CandArray->Num() >= 2);
				TestTrue(TEXT("Candidates array capped at 16"), CandArray->Num() <= 16);
			}
		}
		TestTrue(TEXT("OutAmbiguityCandidates has at least 2 entries"), Candidates.Num() >= 2);

		// Also verify via Validate
		TSharedPtr<FJsonObject> NodeParams = MakeShared<FJsonObject>();
		NodeParams->SetStringField(TEXT("class"), TEXT("BP_CollisionTest"));
		FCortexCommandResult ValidateError;
		TestFalse(TEXT("Validate ConstructObject with ambiguous class fails"),
			FCortexGraphNodeContract::Validate(TEXT("ConstructObject"), ConsumerBP, NodeParams, ValidateError));
		TestTrue(TEXT("Validate error details embed describe_node contract"),
			ValidateError.ErrorDetails.IsValid() && ValidateError.ErrorDetails->HasField(TEXT("describe_node")));
	}

	// 2. Canonical path resolution succeeds unambiguously
	{
		UClass* ResolvedClass = nullptr;
		FCortexCommandResult Error;
		const bool bResolved = FCortexGraphSymbolResolver::ResolveClass(
			BPA->GeneratedClass->GetPathName(), ResolvedClass, Error);
		TestTrue(TEXT("Canonical path resolution succeeds"), bResolved);
		TestEqual(TEXT("Resolved class matches BPA GeneratedClass"), ResolvedClass, BPA->GeneratedClass.Get());

		// Also verify via Validate
		TSharedPtr<FJsonObject> NodeParams = MakeShared<FJsonObject>();
		NodeParams->SetStringField(TEXT("class"), BPA->GeneratedClass->GetPathName());
		FCortexCommandResult ValidateError;
		TestTrue(TEXT("Validate ConstructObject with canonical path succeeds"),
			FCortexGraphNodeContract::Validate(TEXT("ConstructObject"), ConsumerBP, NodeParams, ValidateError));
	}

	CleanupTestPackage(PkgA);
	CleanupTestPackage(PkgB);
	CleanupTestPackage(ConsumerPkg);
	return true;
}

#endif // WITH_EDITOR
