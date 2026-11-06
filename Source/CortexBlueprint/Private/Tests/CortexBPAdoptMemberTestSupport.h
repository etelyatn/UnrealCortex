#pragma once

#include "CoreMinimal.h"

#include "CortexBPReparentTestSupport.h"
#include "CortexTypes.h"
#include "K2Node_CustomEvent.h"
#include "K2Node_VariableSet.h"
#include "Operations/CortexBPAdoptMemberOps.h"
#include "Operations/CortexBPMigrationGuard.h"
#include "UObject/CoreRedirects.h"

/** Fixture helpers shared by the Cortex.Blueprint.AdoptMember test files. */
namespace CortexAdoptMemberTest
{
	using namespace CortexReparentTest;

	struct FStateAFixture
	{
		UBlueprint* Blueprint = nullptr;
		UK2Node_VariableGet* PowerGet = nullptr;
		UK2Node_VariableSet* PowerSet = nullptr;
		UK2Node_CustomEvent* Event = nullptr;
		UK2Node_VariableGet* SpeedGet = nullptr;
	};

	inline UK2Node_VariableSet* AddVariableSet(UBlueprint* Blueprint, const TCHAR* Name)
	{
		UEdGraph* Graph = FBlueprintEditorUtils::FindEventGraph(Blueprint);
		if (Graph == nullptr)
		{
			return nullptr;
		}
		FGraphNodeCreator<UK2Node_VariableSet> Creator(*Graph);
		UK2Node_VariableSet* Node = Creator.CreateNode();
		Node->VariableReference.SetSelfMember(FName(Name));
		Creator.Finalize();
		return Node;
	}

	/** Event -> set bPowerOn, fed by a get of bPowerOn; the chain a destroyed node would cut. */
	inline void AddPowerChain(FStateAFixture& Fixture)
	{
		UEdGraph* Graph = FBlueprintEditorUtils::FindEventGraph(Fixture.Blueprint);
		FGraphNodeCreator<UK2Node_CustomEvent> EventCreator(*Graph);
		Fixture.Event = EventCreator.CreateNode();
		Fixture.Event->CustomFunctionName = TEXT("AdoptFire");
		EventCreator.Finalize();

		Fixture.PowerSet = AddVariableSet(Fixture.Blueprint, TEXT("bPowerOn"));
		Fixture.PowerGet = AddVariableGet(Fixture.Blueprint, TEXT("bPowerOn"));
		if (Fixture.Event != nullptr && Fixture.PowerSet != nullptr && Fixture.PowerGet != nullptr)
		{
			const UEdGraphSchema_K2* Schema = GetDefault<UEdGraphSchema_K2>();
			Schema->TryCreateConnection(Fixture.Event->FindPin(UEdGraphSchema_K2::PN_Then), Fixture.PowerSet->FindPin(UEdGraphSchema_K2::PN_Execute));
			Schema->TryCreateConnection(Fixture.PowerGet->GetValuePin(), Fixture.PowerSet->FindPin(TEXT("bPowerOn")));
		}
	}

	/**
	 * State A: a compiled Blueprint on AActor (bPowerOn true as SaveGame, optional Speed double 2.5, SavedName, TypeMismatch string)
	 * whose ParentClass was then assigned straight to ACortexBPAdoptParent without a refresh or compile, so the variables stay in
	 * NewVariables and the generated class stays stale.
	 */
	inline FStateAFixture MakeStateA(
		const TCHAR* Name, const bool bSpeed, const bool bSavedName, const bool bTypeMismatch,
		UClass* Parent = nullptr, UClass* InitialParent = nullptr, const bool bNodes = true)
	{
		FStateAFixture Fixture;
		Fixture.Blueprint = CortexAdoptTest::MakeBlueprint(Name, InitialParent != nullptr ? InitialParent : AActor::StaticClass());
		if (Fixture.Blueprint == nullptr)
		{
			return Fixture;
		}
		AddVariable(Fixture.Blueprint, TEXT("bPowerOn"), BoolPin(), TEXT("true"), /*bSaveGame=*/true);
		if (bSpeed)
		{
			AddVariable(Fixture.Blueprint, TEXT("Speed"), DoublePin(), TEXT("2.5"));
		}
		if (bSavedName)
		{
			AddVariable(Fixture.Blueprint, TEXT("SavedName"), StringPin(), TEXT("abc"), /*bSaveGame=*/true);
		}
		if (bTypeMismatch)
		{
			AddVariable(Fixture.Blueprint, TEXT("TypeMismatch"), StringPin(), TEXT("x"));
		}
		if (bNodes)
		{
			AddPowerChain(Fixture);
		}
		if (bSpeed && bNodes)
		{
			Fixture.SpeedGet = AddVariableGet(Fixture.Blueprint, TEXT("Speed"));
		}
		FKismetEditorUtilities::CompileBlueprint(Fixture.Blueprint);
		Fixture.Blueprint->ParentClass = Parent != nullptr ? Parent : ACortexBPAdoptParent::StaticClass();
		return Fixture;
	}

	inline TSharedRef<FJsonObject> Mapping(const TCHAR* Variable, const TCHAR* Native)
	{
		TSharedRef<FJsonObject> Item = MakeShared<FJsonObject>();
		Item->SetStringField(TEXT("bp_variable"), Variable);
		Item->SetStringField(TEXT("native_property"), Native);
		return Item;
	}

	inline TSharedPtr<FJsonObject> AdoptParams(const UBlueprint* Blueprint, const TArray<TSharedRef<FJsonObject>>& Mappings)
	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("asset_path"), Blueprint->GetPathName());
		TArray<TSharedPtr<FJsonValue>> Values;
		for (const TSharedRef<FJsonObject>& Item : Mappings)
		{
			Values.Add(MakeShared<FJsonValueObject>(Item));
		}
		Params->SetArrayField(TEXT("mappings"), Values);
		return Params;
	}

	/** The mappings entry of the result (success data or refusal details) whose bp_variable is Variable. */
	inline TSharedPtr<FJsonObject> FindMapping(const FCortexCommandResult& Result, const TCHAR* Variable)
	{
		const TSharedPtr<FJsonObject>& Source = Result.bSuccess ? Result.Data : Result.ErrorDetails;
		const TArray<TSharedPtr<FJsonValue>>* Array = nullptr;
		if (Source.IsValid() && Source->TryGetArrayField(TEXT("mappings"), Array) && Array != nullptr)
		{
			for (const TSharedPtr<FJsonValue>& Value : *Array)
			{
				const TSharedPtr<FJsonObject>* Item = nullptr;
				FString Name;
				if (Value->TryGetObject(Item) && Item != nullptr && (*Item)->TryGetStringField(TEXT("bp_variable"), Name) && Name == Variable)
				{
					return *Item;
				}
			}
		}
		return nullptr;
	}

	inline FString StringOf(const TSharedPtr<FJsonObject>& Object, const TCHAR* Field)
	{
		FString Value;
		if (Object.IsValid())
		{
			Object->TryGetStringField(Field, Value);
		}
		return Value;
	}

	/**
	 * A compile of a Blueprint that declares a variable the native parent also declares is the engine's own conformance (it
	 * removes or renames the variable and says so); tests that build or recover such a state expect these messages.
	 */
	inline void ExpectConformanceWarnings(FAutomationTestBase& Test)
	{
		Test.AddExpectedMessagePlain(TEXT("ValidateVariableNames name"), ELogVerbosity::Warning, EAutomationExpectedMessageFlags::Contains, -1);
		Test.AddExpectedMessagePlain(TEXT("Found a member variable with a conflicting name"), ELogVerbosity::Warning, EAutomationExpectedMessageFlags::Contains, -1);
	}

	/** Failure text of a result: code, message and the guarded run's failure phase and rollback state. */
	inline FString Explain(const FCortexCommandResult& Result)
	{
		FString Text = FString::Printf(TEXT("%s: %s"), *Result.ErrorCode, *Result.ErrorMessage);
		if (Result.ErrorDetails.IsValid())
		{
			FString Phase;
			FString Message;
			FString Rollback;
			Result.ErrorDetails->TryGetStringField(TEXT("failure_phase"), Phase);
			Result.ErrorDetails->TryGetStringField(TEXT("failure_message"), Message);
			Result.ErrorDetails->TryGetStringField(TEXT("rollback_status"), Rollback);
			Text += FString::Printf(TEXT(" [phase=%s message=%s rollback=%s]"), *Phase, *Message, *Rollback);
		}
		return Text;
	}

	/** What a dry run must leave untouched. */
	struct FStateDigest
	{
		bool bDirty = false;
		FString Parent;
		TArray<FString> Variables;
		FString MigrationHash;
		FString GraphHash;
		int32 NodeCount = 0;

		static FStateDigest Of(UBlueprint* Blueprint)
		{
			FStateDigest Digest;
			Digest.bDirty = Blueprint->GetOutermost()->IsDirty();
			Digest.Parent = Blueprint->ParentClass != nullptr ? Blueprint->ParentClass->GetPathName() : FString();
			for (const FBPVariableDescription& Variable : Blueprint->NewVariables)
			{
				Digest.Variables.Add(FString::Printf(TEXT("%s|%s|%s|%llu"),
					*Variable.VarName.ToString(), *Variable.VarGuid.ToString(), *Variable.DefaultValue, static_cast<uint64>(Variable.PropertyFlags)));
			}
			const TSharedPtr<FJsonObject> Fingerprint = FCortexBPMigrationGuard::ComputeFingerprint(Blueprint);
			Digest.MigrationHash = Fingerprint->GetStringField(TEXT("migration_state_hash"));
			Digest.GraphHash = Fingerprint->GetStringField(TEXT("graph_authoring_hash"));
			TArray<UEdGraph*> Graphs;
			Blueprint->GetAllGraphs(Graphs);
			for (const UEdGraph* Graph : Graphs)
			{
				Digest.NodeCount += Graph->Nodes.Num();
			}
			return Digest;
		}

		bool operator==(const FStateDigest& Other) const
		{
			return bDirty == Other.bDirty && Parent == Other.Parent && Variables == Other.Variables
				&& MigrationHash == Other.MigrationHash && GraphHash == Other.GraphHash && NodeCount == Other.NodeCount;
		}
	};

	/** Registers a property redirect for the scope. */
	struct FRedirectScope
	{
		FRedirectScope(const UClass* Owner, const TCHAR* OldName, const TCHAR* NewName)
		{
			const FName Package = Owner->GetOutermost()->GetFName();
			const FName OuterName = Owner->GetFName();
			Redirects.Add(FCoreRedirect(ECoreRedirectFlags::Type_Property,
				FCoreRedirectObjectName(FName(OldName), OuterName, Package),
				FCoreRedirectObjectName(FName(NewName), OuterName, Package)));
			FCoreRedirects::AddRedirectList(Redirects, TEXT("CortexAdoptMemberTest"));
		}
		~FRedirectScope()
		{
			FCoreRedirects::RemoveRedirectList(Redirects, TEXT("CortexAdoptMemberTest"));
		}

		TArray<FCoreRedirect> Redirects;
	};

	inline bool HasNodeGuid(const UBlueprint* Blueprint, const FGuid& Guid)
	{
		TArray<UEdGraph*> Graphs;
		Blueprint->GetAllGraphs(Graphs);
		for (const UEdGraph* Graph : Graphs)
		{
			for (const UEdGraphNode* Node : Graph->Nodes)
			{
				if (Node != nullptr && Node->NodeGuid == Guid)
				{
					return true;
				}
			}
		}
		return false;
	}
}
