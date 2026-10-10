#include "Operations/CortexGraphMigrationOps.h"
#include "Operations/CortexGraphPatchOps.h"
#include "Operations/CortexGraphPatchState.h"
#include "Misc/AutomationTest.h"

#include "CortexCommandRouter.h"
#include "CortexGraphCommandHandler.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "WidgetBlueprint.h"
#include "Blueprint/WidgetTree.h"
#include "Components/Button.h"
#include "Components/CanvasPanel.h"
#include "CortexGraphMigrationTestTypes.h"
#include "CortexGraphTestContentRoot.h"
#include "EdGraph/EdGraph.h"
#include "Editor.h"
#include "Editor/Transactor.h"
#include "EdGraph/EdGraphPin.h"
#include "EdGraphSchema_K2.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "Engine/ComponentDelegateBinding.h"
#include "K2Node_CallFunction.h"
#include "K2Node_ComponentBoundEvent.h"
#include "K2Node_CustomEvent.h"
#include "K2Node_Event.h"
#include "Kismet/KismetStringLibrary.h"
#include "Kismet/KismetSystemLibrary.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Kismet/GameplayStatics.h"
#include "Engine/World.h"
#include "K2Node_AddDelegate.h"
#include "K2Node_AssignDelegate.h"
#include "K2Node_BaseAsyncTask.h"
#include "K2Node_Composite.h"
#include "K2Node_CreateDelegate.h"
#include "K2Node_FunctionEntry.h"
#include "K2Node_Knot.h"
#include "K2Node_MacroInstance.h"
#include "K2Node_Tunnel.h"
#include "K2Node_VariableGet.h"
#include "K2Node_VariableSet.h"
#include "K2Node_FunctionResult.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"
#include "Misc/FileHelper.h"
#include "Misc/PackageName.h"
#include "HAL/FileManager.h"
#include "PackageTools.h"
#include "UObject/GarbageCollection.h"
#include "Dom/JsonObject.h"
#include "UObject/UnrealType.h"
#include "UObject/UObjectGlobals.h"
#include "UObject/UObjectHash.h"

#if WITH_EDITOR && WITH_AUTOMATION_TESTS
namespace CortexGraphMigrationRetireTest
{
struct FFixture
{
	UPackage* Package = nullptr;
	FString OwnedPackageName;
	UWidgetBlueprint* Blueprint = nullptr;
	UEdGraph* Graph = nullptr;
	UK2Node_Event* Alpha = nullptr;
	UK2Node_Event* Beta = nullptr;
	UK2Node_Event* Retained = nullptr;
	UK2Node_CallFunction* Producer = nullptr;
	UK2Node_CallFunction* AlphaBody = nullptr;
	UK2Node_CallFunction* BetaBody = nullptr;
	UK2Node_CallFunction* RetainedBody = nullptr;
	FGuid AlphaGuid;
	FGuid BetaGuid;
	FGuid RetainedGuid;
	FGuid ProducerGuid;
	FGuid AlphaBodyGuid;
	FGuid BetaBodyGuid;
	FGuid RetainedBodyGuid;
	/** Late-retained producer chain: the execution body whose output feeds the retained cosmetic body. */
	UK2Node_CallFunction* RetainedProducer = nullptr;
	/** The execution body downstream of `RetainedProducer`, which must never be approved for removal. */
	UK2Node_CallFunction* RetainedProducerConsumer = nullptr;
	/** The pair of pure producers whose outputs feed each other inside the selected island. */
	UK2Node_CallFunction* CycleFirst = nullptr;
	UK2Node_CallFunction* CycleSecond = nullptr;
	FGuid RetainedProducerGuid;
	FGuid RetainedProducerConsumerGuid;
	FGuid CycleFirstGuid;
	FGuid CycleSecondGuid;


	UK2Node_Event* AddEvent(const TCHAR* Name)
	{
		for (UEdGraphNode* Node : Graph->Nodes)
		{
			UK2Node_Event* Existing = Cast<UK2Node_Event>(Node);
			if (Existing && Existing->EventReference.GetMemberName() == FName(Name))
			{
				Existing->EventReference.SetExternalMember(FName(Name), UCortexGraphRetireLegacyWidget::StaticClass());
				Existing->bOverrideFunction = true;
				return Existing;
			}
		}
		UK2Node_Event* Event = NewObject<UK2Node_Event>(Graph);
		Event->EventReference.SetExternalMember(FName(Name), UCortexGraphRetireLegacyWidget::StaticClass());
		Event->bOverrideFunction = true;
		Event->CreateNewGuid();
		Event->AllocateDefaultPins();
		Graph->AddNode(Event, true, false);
		return Event;
	}

	UK2Node_CallFunction* AddCall(UFunction* Function)
	{
		UK2Node_CallFunction* Call = NewObject<UK2Node_CallFunction>(Graph);
		Call->FunctionReference.SetExternalMember(Function->GetFName(), Function->GetOuterUClass());
		Call->CreateNewGuid();
		Call->AllocateDefaultPins();
		Graph->AddNode(Call, true, false);
		return Call;
	}
	UK2Node_CustomEvent* AddNativeNameCollision()
	{
		UK2Node_CustomEvent* Event = NewObject<UK2Node_CustomEvent>(Graph);
		Event->CustomFunctionName = TEXT("OnInitialized");
		Event->CreateNewGuid();
		Event->AllocateDefaultPins();
		Graph->AddNode(Event, true, false);
		return Event;
	}


	bool Build(const TCHAR* Name, const bool bRetainProducer = false, const bool bBlockAlpha = false,
		UClass* ParentClass = nullptr, const bool bCompileParentFirst = false, const bool bDelayGraphNodes = false)
	{
		Package = CreateCortexGraphTestPackage(Name, OwnedPackageName);
		Blueprint = Cast<UWidgetBlueprint>(FKismetEditorUtilities::CreateBlueprint(
			ParentClass ? ParentClass : UCortexGraphRetireLegacyWidget::StaticClass(), Package, FName(Name), BPTYPE_Normal,
			UWidgetBlueprint::StaticClass(), UWidgetBlueprintGeneratedClass::StaticClass()));
		if (!Blueprint) return false;
		if (Blueprint->UbergraphPages.Num() == 0)
		{
			Graph = FBlueprintEditorUtils::CreateNewGraph(Blueprint, UEdGraphSchema_K2::GN_EventGraph,
				UEdGraph::StaticClass(), UEdGraphSchema_K2::StaticClass());
			FBlueprintEditorUtils::AddUbergraphPage(Blueprint, Graph);
		}
		else Graph = Blueprint->UbergraphPages[0];
		if (bCompileParentFirst) FKismetEditorUtilities::CompileBlueprint(Blueprint);
		return bDelayGraphNodes || PopulateGraph(bRetainProducer, bBlockAlpha);
	}

	bool PopulateGraph(const bool bRetainProducer = false, const bool bBlockAlpha = false)
	{
		Alpha = AddEvent(TEXT("OnLegacyAlpha"));
		Beta = AddEvent(TEXT("OnLegacyBeta"));
		Retained = AddEvent(TEXT("OnRetainedEvent"));
		Producer = AddCall(UKismetStringLibrary::StaticClass()->FindFunctionByName(TEXT("Conv_IntToString")));
		AlphaBody = AddCall(UKismetSystemLibrary::StaticClass()->FindFunctionByName(TEXT("PrintString")));
		BetaBody = AddCall(UKismetSystemLibrary::StaticClass()->FindFunctionByName(TEXT("PrintString")));
		RetainedBody = AddCall(UKismetSystemLibrary::StaticClass()->FindFunctionByName(TEXT("PrintString")));
		const UEdGraphSchema* Schema = Graph->GetSchema();
		auto Link = [Schema](UEdGraphNode* From, const TCHAR* FromName, UEdGraphNode* To, const TCHAR* ToName)
		{
			UEdGraphPin* FromPin = From ? From->FindPin(FName(FromName)) : nullptr;
			UEdGraphPin* ToPin = To ? To->FindPin(FName(ToName)) : nullptr;
			return Schema && FromPin && ToPin && Schema->TryCreateConnection(FromPin, ToPin);
		};
		if (!Link(Alpha, TEXT("then"), AlphaBody, TEXT("execute"))
			|| !Link(Beta, TEXT("then"), BetaBody, TEXT("execute"))
			|| !Link(Producer, TEXT("ReturnValue"), AlphaBody, TEXT("InString"))
			|| !Link(Producer, TEXT("ReturnValue"), BetaBody, TEXT("InString"))) return false;
		if (bRetainProducer)
		{
			if (!Link(Retained, TEXT("then"), RetainedBody, TEXT("execute"))
				|| !Link(Producer, TEXT("ReturnValue"), RetainedBody, TEXT("InString"))) return false;
		}
		AlphaGuid = Alpha->NodeGuid;
		BetaGuid = Beta->NodeGuid;
		RetainedGuid = Retained->NodeGuid;
		ProducerGuid = Producer->NodeGuid;
		AlphaBodyGuid = AlphaBody->NodeGuid;
		BetaBodyGuid = BetaBody->NodeGuid;
		RetainedBodyGuid = RetainedBody->NodeGuid;
		if (bBlockAlpha)
		{
			UK2Node_CallFunction* RetainedData = AddCall(UKismetStringLibrary::StaticClass()->FindFunctionByName(TEXT("Conv_IntToString")));
			if (!Link(Retained, TEXT("then"), RetainedBody, TEXT("execute"))
				|| !Link(Alpha, TEXT("Value"), RetainedData, TEXT("InInt"))
				|| !Link(RetainedData, TEXT("ReturnValue"), RetainedBody, TEXT("InString"))) return false;
		}
		return true;
	}

	/** Links two pins of this fixture's graph, reporting whether the schema accepted the connection. */
	bool LinkPins(UEdGraphNode* From, const TCHAR* FromName, UEdGraphNode* To, const TCHAR* ToName)
	{
		const UEdGraphSchema* Schema = Graph ? Graph->GetSchema() : nullptr;
		UEdGraphPin* FromPin = From ? From->FindPin(FName(FromName)) : nullptr;
		UEdGraphPin* ToPin = To ? To->FindPin(FName(ToName)) : nullptr;
		return Schema && FromPin && ToPin && Schema->TryCreateConnection(FromPin, ToPin);
	}

	/**
	 * Adds the late-retained producer chain the ownership cut must reach a fixed point over: the
	 * selected entry Alpha drives the execution body `RetainedProducer`, whose string output is
	 * consumed by the retained cosmetic body, and `RetainedProducer` drives the execution body
	 * `RetainedProducerConsumer`.
	 *
	 * The producer is retained only because a retained consumer uses its output. That decision is made
	 * after a candidate sweep over the island, so the consumer's incoming execution edge has to be
	 * re-checked afterwards; otherwise approving the consumer's removal severs the retained
	 * producer -> consumer execution link.
	 */
	bool AddLateRetainedProducerChain()
	{
		RetainedProducer = AddCall(UKismetSystemLibrary::StaticClass()->FindFunctionByName(TEXT("GetConsoleVariableStringValue")));
		RetainedProducerConsumer = AddCall(UKismetSystemLibrary::StaticClass()->FindFunctionByName(TEXT("PrintString")));
		if (!RetainedProducer || !RetainedProducerConsumer) return false;
		if (!LinkPins(Retained, TEXT("then"), RetainedBody, TEXT("execute"))
			|| !LinkPins(Alpha, TEXT("then"), RetainedProducer, TEXT("execute"))
			|| !LinkPins(RetainedProducer, TEXT("then"), RetainedProducerConsumer, TEXT("execute"))
			|| !LinkPins(RetainedProducer, TEXT("ReturnValue"), RetainedBody, TEXT("InString"))) return false;
		RetainedProducerGuid = RetainedProducer->NodeGuid;
		RetainedProducerConsumerGuid = RetainedProducerConsumer->NodeGuid;
		return true;
	}

	/**
	 * Adds a data cycle between two pure producers that only removable candidates consume, and feeds
	 * the island's shared int producer from the cycle. The cycle therefore sits inside the scanned
	 * island: the partition must terminate on it and keep both members removable instead of refusing,
	 * spinning or reporting the cycle as unproven ownership.
	 */
	bool AddRemovableDataCycle()
	{
		CycleFirst = AddCall(UKismetStringLibrary::StaticClass()->FindFunctionByName(TEXT("Conv_StringToInt")));
		CycleSecond = AddCall(UKismetStringLibrary::StaticClass()->FindFunctionByName(TEXT("Conv_IntToString")));
		if (!CycleFirst || !CycleSecond) return false;
		if (!LinkPins(CycleSecond, TEXT("ReturnValue"), CycleFirst, TEXT("InString"))
			|| !LinkPins(CycleFirst, TEXT("ReturnValue"), CycleSecond, TEXT("InInt"))
			|| !LinkPins(CycleFirst, TEXT("ReturnValue"), Producer, TEXT("InInt"))) return false;
		CycleFirstGuid = CycleFirst->NodeGuid;
		CycleSecondGuid = CycleSecond->NodeGuid;
		return true;
	}


	bool RefreshPointers()
	{
		auto Find = [this](const FGuid& Guid) -> UEdGraphNode*
		{
			for (UEdGraphNode* Node : Graph->Nodes) if (Node && Node->NodeGuid == Guid) return Node;
			return nullptr;
		};
		Alpha = Cast<UK2Node_Event>(Find(AlphaGuid));
		Beta = Cast<UK2Node_Event>(Find(BetaGuid));
		Retained = Cast<UK2Node_Event>(Find(RetainedGuid));
		Producer = Cast<UK2Node_CallFunction>(Find(ProducerGuid));
		AlphaBody = Cast<UK2Node_CallFunction>(Find(AlphaBodyGuid));
		BetaBody = Cast<UK2Node_CallFunction>(Find(BetaBodyGuid));
		RetainedBody = Cast<UK2Node_CallFunction>(Find(RetainedBodyGuid));
		return Alpha && Beta && Retained && Producer && AlphaBody && BetaBody && RetainedBody;
	}
	TSharedPtr<FJsonObject> Migration(const TArray<FString>& Entries, const bool bApproved = false,
		const TArray<FString>& Approved = {}) const
	{
		TSharedPtr<FJsonObject> MigrationJson = MakeShared<FJsonObject>();
		MigrationJson->SetStringField(TEXT("op"), TEXT("retire_entries"));
		TSharedPtr<FJsonObject> Source = MakeShared<FJsonObject>();
		TSharedPtr<FJsonObject> GraphRef = MakeShared<FJsonObject>();
		GraphRef->SetStringField(TEXT("graph_guid"), Graph->GraphGuid.ToString());
		Source->SetObjectField(TEXT("graph_ref"), GraphRef);
		TArray<TSharedPtr<FJsonValue>> EntryValues;
		for (const FString& Entry : Entries) EntryValues.Add(MakeShared<FJsonValueString>(Entry));
		Source->SetArrayField(TEXT("entry_node_guids"), EntryValues);
		MigrationJson->SetObjectField(TEXT("source"), Source);
		if (bApproved)
		{
			TArray<TSharedPtr<FJsonValue>> ApprovedValues;
			for (const FString& Entry : Approved) ApprovedValues.Add(MakeShared<FJsonValueString>(Entry));
			MigrationJson->SetArrayField(TEXT("approved_node_guids"), ApprovedValues);
		}
		return MigrationJson;
	}

	FString Filename() const
	{
		return Package ? FPackageName::LongPackageNameToFilename(Package->GetName(),
			FPackageName::GetAssetPackageExtension()) : FString();
	}

	bool SaveToDisk()
	{
		FSavePackageArgs SaveArgs;
		SaveArgs.TopLevelFlags = RF_Public | RF_Standalone;
		return UPackage::SavePackage(Package, Blueprint, *Filename(), SaveArgs);
	}

	void Cleanup()
	{
		if (Blueprint) { Blueprint->ClearFlags(RF_Standalone); Blueprint->MarkAsGarbage(); Blueprint = nullptr; }
		if (Package) { Package->ClearFlags(RF_Standalone); Package->MarkAsGarbage(); Package = nullptr; }
	}

	~FFixture()
	{
		DeleteCortexGraphTestPackageFile(Package, OwnedPackageName);
		Cleanup();
	}
};

struct FOperations
{
	int32 TargetCompiles = 0;
	int32 RecoveryCompiles = 0;
	int32 Saves = 0;
	/**
	 * Every completed editor Blueprint compilation while the watch is installed. `GEditor`
	 * broadcasts it independently of the patch seam, so a compile-on-load of a foreign Blueprint
	 * cannot hide behind the operation's own counters.
	 */
	int32 EditorCompiles = 0;

	void Begin()
	{
		Active = this;
		TargetCompiles = 0;
		RecoveryCompiles = 0;
		Saves = 0;
		EditorCompiles = 0;
		FCortexGraphPatchOps::SetOperationObserverForTesting([](const FName Operation, UBlueprint*)
		{
			if (!Active) return;
			if (Operation == TEXT("target_compile")) ++Active->TargetCompiles;
			else if (Operation == TEXT("recovery_compile")) ++Active->RecoveryCompiles;
		});
		SaveHandle = UPackage::PackageSavedWithContextEvent.AddLambda(
			[](const FString&, UPackage*, FObjectPostSaveContext)
			{
				if (Active) ++Active->Saves;
			});
		if (GEditor)
		{
			CompileHandle = GEditor->OnBlueprintCompiled().AddLambda([this]() { ++EditorCompiles; });
		}
	}

	void End()
	{
		if (GEditor && CompileHandle.IsValid()) GEditor->OnBlueprintCompiled().Remove(CompileHandle);
		CompileHandle.Reset();
		UPackage::PackageSavedWithContextEvent.Remove(SaveHandle);
		FCortexGraphPatchOps::ClearOperationObserverForTesting();
		Active = nullptr;
	}

private:
	static FOperations* Active;
	FDelegateHandle SaveHandle;
	FDelegateHandle CompileHandle;
};

FOperations* FOperations::Active = nullptr;

TArray<uint8> ReadBytes(const FString& Filename)
{
	TArray<uint8> Bytes;
	FFileHelper::LoadFileToArray(Bytes, *Filename);
	return Bytes;
}

bool SameBytes(const TArray<uint8>& Left, const TArray<uint8>& Right)
{
	return Left.Num() > 0 && Left.Num() == Right.Num()
		&& FMemory::Memcmp(Left.GetData(), Right.GetData(), Left.Num()) == 0;
}

bool Plan(FFixture& Fixture, const TArray<FString>& Entries, FCortexGraphMigrationRetirePlan& OutPlan,
	bool& bReused, FCortexCommandResult& Error, const bool bApproved = false, const TArray<FString>& Approved = {})
{
	return FCortexGraphMigrationOps::PlanRetirement(Fixture.Blueprint,
		Fixture.Migration(Entries, bApproved, Approved), OutPlan, bReused, Error);
}
FString CaptureNativeGraph(UEdGraph* Graph, const TSet<FGuid>* Filter = nullptr)
{
	TArray<FString> Records;
	if (!Graph) return FString();
	for (const UEdGraphNode* Node : Graph->Nodes)
	{
		if (!Node) continue;
		const FString Guid = Node->NodeGuid.ToString();
		if (Filter && !Filter->Contains(Node->NodeGuid)) continue;
		Records.Add(FString::Printf(TEXT("N|%s|%s|%d|%d|%s"), *Guid, *Node->GetClass()->GetPathName(),
			Node->NodePosX, Node->NodePosY, *Node->NodeComment));
		for (const UEdGraphPin* Pin : Node->Pins)
		{
			if (!Pin) continue;
			Records.Add(FString::Printf(TEXT("P|%s|%s|%d|%s|%s|%s"), *Guid, *Pin->PinName.ToString(),
				static_cast<int32>(Pin->Direction), *Pin->PinType.PinCategory.ToString(), *Pin->DefaultValue,
				Pin->DefaultObject ? *Pin->DefaultObject->GetPathName() : TEXT("")));
			for (const UEdGraphPin* LinkedPin : Pin->LinkedTo)
			{
				const UEdGraphNode* FarNode = LinkedPin ? LinkedPin->GetOwningNode() : nullptr;
				if (!FarNode || !LinkedPin || (Filter && !Filter->Contains(FarNode->NodeGuid))) continue;
				const FString From = Guid + TEXT(".") + Pin->PinName.ToString();
				const FString To = FarNode->NodeGuid.ToString() + TEXT(".") + LinkedPin->PinName.ToString();
				if (From < To) Records.Add(TEXT("L|") + From + TEXT("|") + To);
			}
		}
	}
	Records.Sort();
	return FString::Join(Records, TEXT("\n"));
}

/**
 * Two-stage reviewed retirement request for an arbitrary entry set: preview, echo the published
 * removable set, then approve it. `OutApproved` is the approved set the reviewed preview published.
 */
bool PrepareRetirementRequest(
	FFixture& Fixture,
	const TArray<FString>& Entries,
	const TCHAR* PatchId,
	TSharedPtr<FJsonObject>& OutRequest,
	TArray<FString>& OutApproved,
	FCortexCommandResult& OutError,
	const bool bCompile = false,
	const bool bSave = false)
{
	OutRequest = MakeShared<FJsonObject>();
	OutRequest->SetStringField(TEXT("asset_path"), Fixture.Blueprint->GetPathName());
	OutRequest->SetStringField(TEXT("patch_id"), PatchId);
	OutRequest->SetObjectField(TEXT("expected_fingerprint"), FCortexGraphPatchState::ComputeFingerprint(Fixture.Blueprint));
	OutRequest->SetArrayField(TEXT("nodes"), {});
	OutRequest->SetArrayField(TEXT("connections"), {});
	OutRequest->SetArrayField(TEXT("pin_updates"), {});
	OutRequest->SetBoolField(TEXT("dry_run"), true);
	OutRequest->SetBoolField(TEXT("compile"), bCompile);
	OutRequest->SetBoolField(TEXT("save"), false);
	OutRequest->SetBoolField(TEXT("allow_noop"), false);
	TSharedPtr<FJsonObject> Migration = Fixture.Migration(Entries);
	OutRequest->SetObjectField(TEXT("migration"), Migration);

	FCortexGraphPreparedPatch Preview;
	if (!FCortexGraphPatchOps::Preflight(Fixture.Blueprint, OutRequest, Preview, OutError)) return false;
	const TSharedPtr<FJsonObject> Inventory =
		FCortexGraphMigrationOps::MakeRetirementInventory(Preview.RetirementPlan);
	const TArray<TSharedPtr<FJsonValue>>* Removable = nullptr;
	if (!Inventory.IsValid() || !Inventory->TryGetArrayField(TEXT("removable"), Removable) || !Removable)
	{
		OutError = FCortexCommandRouter::Error(CortexErrorCodes::InvalidOperation,
			TEXT("retirement preview did not publish its removable set"));
		return false;
	}
	for (const TSharedPtr<FJsonValue>& Value : *Removable) OutApproved.Add(Value->AsString());
	TArray<TSharedPtr<FJsonValue>> ApprovalValues;
	for (const FString& Guid : OutApproved) ApprovalValues.Add(MakeShared<FJsonValueString>(Guid));
	Migration->SetArrayField(TEXT("approved_node_guids"), ApprovalValues);

	FCortexGraphPreparedPatch Reviewed;
	if (!FCortexGraphPatchOps::Preflight(Fixture.Blueprint, OutRequest, Reviewed, OutError)) return false;
	OutRequest->SetBoolField(TEXT("dry_run"), false);
	OutRequest->SetBoolField(TEXT("save"), bSave);
	OutRequest->SetStringField(TEXT("expected_validation_hash"), Reviewed.ValidationHash);
	return true;
}

/** The reviewed retirement request of the fixture's two legacy entries. */
bool PrepareApprovedRequest(
	FFixture& Fixture,
	const TCHAR* PatchId,
	TSharedPtr<FJsonObject>& OutRequest,
	TArray<FString>& OutApproved,
	FCortexCommandResult& OutError,
	const bool bCompile = false,
	const bool bSave = false)
{
	return PrepareRetirementRequest(Fixture,
		{ Fixture.Alpha->NodeGuid.ToString(), Fixture.Beta->NodeGuid.ToString() },
		PatchId, OutRequest, OutApproved, OutError, bCompile, bSave);
}

/** Canonical widget-tree signature, so preview purity covers the preserved designer tree. */
FString CaptureWidgetTree(const UWidgetBlueprint* Blueprint)
{
	TArray<FString> Records;
	if (!Blueprint) return FString();
	for (const UWidget* Widget : Blueprint->GetAllSourceWidgets())
	{
		if (!Widget) continue;
		Records.Add(FString::Printf(TEXT("%s|%s|%d"), *Widget->GetName(),
			*Widget->GetClass()->GetPathName(), Widget->bIsVariable ? 1 : 0));
	}
	if (Blueprint->WidgetTree && Blueprint->WidgetTree->RootWidget)
	{
		Records.Add(TEXT("root|") + Blueprint->WidgetTree->RootWidget->GetName());
	}
	Records.Sort();
	return FString::Join(Records, TEXT("\n"));
}

/** Canonical generated component-delegate binding signature (component|delegate|function). */
FString CaptureComponentBindings(const UBlueprint* Blueprint)
{
	TArray<FString> Records;
	const UBlueprintGeneratedClass* GeneratedClass = Blueprint ? Cast<UBlueprintGeneratedClass>(Blueprint->GeneratedClass) : nullptr;
	if (!GeneratedClass) return FString();
	for (const UDynamicBlueprintBinding* Binding : GeneratedClass->DynamicBindingObjects)
	{
		const UComponentDelegateBinding* ComponentBinding = Cast<UComponentDelegateBinding>(Binding);
		if (!ComponentBinding) continue;
		for (const FBlueprintComponentDelegateBinding& Entry : ComponentBinding->ComponentDelegateBindings)
		{
			Records.Add(FString::Printf(TEXT("%s|%s|%s"), *Entry.ComponentPropertyName.ToString(),
				*Entry.DelegatePropertyName.ToString(), *Entry.FunctionNameToBind.ToString()));
		}
	}
	Records.Sort();
	return FString::Join(Records, TEXT("\n"));
}

/**
 * Guarded-retirement fixture for the entry classes CortexSandbox issue #112 must accept.
 *
 * Everything is built on a real Widget Blueprint through engine APIs: the widget variable is created
 * by the widget compiler, the component-bound node binds to that compiled component property plus the
 * native `UButton::OnClicked` delegate exactly as `FKismetEditorUtilities::CreateNewBoundEventForComponent`
 * and `UBlueprintBoundEventNodeSpawner::BindToNode` do, and the custom event is a compiled callable
 * generated function. One unrelated cosmetic override stays in the graph so preservation is asserted
 * next to eligibility. The Blueprint parent is a native widget class because a plain `UUserWidget`
 * parent is not proof that native behaviour replaced a retired widget lifecycle body.
 */
struct FGuardedEntryFixture
{
	UPackage* Package = nullptr;
	FString OwnedPackageName;
	UWidgetBlueprint* Blueprint = nullptr;
	UEdGraph* Graph = nullptr;
	UK2Node_ComponentBoundEvent* Bound = nullptr;
	UK2Node_Event* Construct = nullptr;
	UK2Node_Event* Initialized = nullptr;
	UK2Node_Event* Retained = nullptr;
	UK2Node_CustomEvent* Custom = nullptr;
	UK2Node_CallFunction* BoundBody = nullptr;
	UK2Node_CallFunction* ConstructBody = nullptr;
	UK2Node_CallFunction* InitializedBody = nullptr;
	UK2Node_CallFunction* RetainedBody = nullptr;
	UK2Node_CallFunction* CustomBody = nullptr;
	/** The compiled widget variable and native delegate the component-bound node binds. */
	FObjectProperty* ComponentProperty = nullptr;
	FMulticastDelegateProperty* ClickedDelegate = nullptr;
	/** A compiled widget variable whose own class cannot host the native delegate above. */
	FObjectProperty* IncompatibleComponentProperty = nullptr;
	FGuid BoundGuid;
	FGuid ConstructGuid;
	FGuid InitializedGuid;
	FGuid RetainedGuid;
	FGuid CustomGuid;
	FGuid BoundBodyGuid;
	FGuid ConstructBodyGuid;
	FGuid InitializedBodyGuid;
	FGuid RetainedBodyGuid;
	FGuid CustomBodyGuid;

	TSharedPtr<FJsonObject> Migration(const TArray<FString>& Entries, const bool bApproved = false,
		const TArray<FString>& Approved = {}) const
	{
		TSharedPtr<FJsonObject> MigrationJson = MakeShared<FJsonObject>();
		MigrationJson->SetStringField(TEXT("op"), TEXT("retire_entries"));
		TSharedPtr<FJsonObject> Source = MakeShared<FJsonObject>();
		TSharedPtr<FJsonObject> GraphRef = MakeShared<FJsonObject>();
		GraphRef->SetStringField(TEXT("graph_guid"), Graph->GraphGuid.ToString());
		Source->SetObjectField(TEXT("graph_ref"), GraphRef);
		TArray<TSharedPtr<FJsonValue>> EntryValues;
		for (const FString& Entry : Entries) EntryValues.Add(MakeShared<FJsonValueString>(Entry));
		Source->SetArrayField(TEXT("entry_node_guids"), EntryValues);
		MigrationJson->SetObjectField(TEXT("source"), Source);
		if (bApproved)
		{
			TArray<TSharedPtr<FJsonValue>> ApprovedValues;
			for (const FString& Entry : Approved) ApprovedValues.Add(MakeShared<FJsonValueString>(Entry));
			MigrationJson->SetArrayField(TEXT("approved_node_guids"), ApprovedValues);
		}
		return MigrationJson;
	}

	UK2Node_CallFunction* AddCall(UFunction* Function)
	{
		if (!Graph || !Function) return nullptr;
		UK2Node_CallFunction* Call = NewObject<UK2Node_CallFunction>(Graph, NAME_None, RF_Transactional);
		Call->FunctionReference.SetExternalMember(Function->GetFName(), Function->GetOuterUClass());
		Call->CreateNewGuid();
		Call->AllocateDefaultPins();
		Graph->AddNode(Call, true, false);
		return Call;
	}

	UK2Node_Event* AddOverride(const TCHAR* Member, UClass* MemberClass)
	{
		if (!Graph || !MemberClass) return nullptr;
		UK2Node_Event* Event = NewObject<UK2Node_Event>(Graph, NAME_None, RF_Transactional);
		Event->EventReference.SetExternalMember(FName(Member), MemberClass);
		Event->bOverrideFunction = true;
		Event->CreateNewGuid();
		Event->AllocateDefaultPins();
		Graph->AddNode(Event, true, false);
		return Event;
	}

	bool LinkNodes(UEdGraphNode* From, const TCHAR* FromPin, UEdGraphNode* To, const TCHAR* ToPin)
	{
		const UEdGraphSchema* Schema = Graph ? Graph->GetSchema() : nullptr;
		UEdGraphPin* Source = From ? From->FindPin(FName(FromPin)) : nullptr;
		UEdGraphPin* Target = To ? To->FindPin(FName(ToPin)) : nullptr;
		return Schema && Source && Target && Schema->TryCreateConnection(Source, Target);
	}

	/** Every selected entry owns a real downstream body, so the partition runs on a genuine island. */
	UK2Node_CallFunction* AddBody(UEdGraphNode* Entry)
	{
		UK2Node_CallFunction* Body = AddCall(UKismetSystemLibrary::StaticClass()->FindFunctionByName(TEXT("PrintString")));
		if (!Body || !LinkNodes(Entry, TEXT("then"), Body, TEXT("execute"))) return nullptr;
		return Body;
	}

	UEdGraphNode* FindByGuid(const FGuid& Guid) const
	{
		if (!Graph) return nullptr;
		for (UEdGraphNode* Node : Graph->Nodes) if (Node && Node->NodeGuid == Guid) return Node;
		return nullptr;
	}

	/** True only when every fixture node and graph the guarded tests dereference exists. */
	bool IsComplete() const
	{
		return Blueprint && Graph && Bound && Construct && Initialized && Retained && Custom
			&& BoundBody && ConstructBody && InitializedBody && RetainedBody && CustomBody;
	}

	/** On-disk path of the fixture asset, so preview purity also covers the persisted bytes. */
	FString Filename() const
	{
		return Package ? FPackageName::LongPackageNameToFilename(Package->GetName(),
			FPackageName::GetAssetPackageExtension()) : FString();
	}

	bool SaveToDisk()
	{
		FSavePackageArgs SaveArgs;
		SaveArgs.TopLevelFlags = RF_Public | RF_Standalone;
		return UPackage::SavePackage(Package, Blueprint, *Filename(), SaveArgs);
	}

	bool RefreshPointers()
	{
		Bound = Cast<UK2Node_ComponentBoundEvent>(FindByGuid(BoundGuid));
		Construct = Cast<UK2Node_Event>(FindByGuid(ConstructGuid));
		Initialized = Cast<UK2Node_Event>(FindByGuid(InitializedGuid));
		Retained = Cast<UK2Node_Event>(FindByGuid(RetainedGuid));
		Custom = Cast<UK2Node_CustomEvent>(FindByGuid(CustomGuid));
		BoundBody = Cast<UK2Node_CallFunction>(FindByGuid(BoundBodyGuid));
		ConstructBody = Cast<UK2Node_CallFunction>(FindByGuid(ConstructBodyGuid));
		InitializedBody = Cast<UK2Node_CallFunction>(FindByGuid(InitializedBodyGuid));
		RetainedBody = Cast<UK2Node_CallFunction>(FindByGuid(RetainedBodyGuid));
		CustomBody = Cast<UK2Node_CallFunction>(FindByGuid(CustomBodyGuid));
		return Bound && Construct && Initialized && Retained && Custom
			&& BoundBody && ConstructBody && InitializedBody && RetainedBody && CustomBody;
	}

	/** `bAddIncompatibleComponent` compiles a second widget variable of an unrelated widget class. */
	bool Build(const TCHAR* Name, const bool bAddIncompatibleComponent = false)
	{
		Package = CreateCortexGraphTestPackage(Name, OwnedPackageName);
		Blueprint = Cast<UWidgetBlueprint>(FKismetEditorUtilities::CreateBlueprint(
			UCortexGraphRetireNativeLifecycleWidget::StaticClass(), Package, FName(Name), BPTYPE_Normal,
			UWidgetBlueprint::StaticClass(), UWidgetBlueprintGeneratedClass::StaticClass()));
		if (!Blueprint) return false;

		UWidgetTree* Tree = Blueprint->WidgetTree;
		if (!Tree)
		{
			Tree = NewObject<UWidgetTree>(Blueprint, UWidgetTree::StaticClass(), TEXT("WidgetTree"));
			Blueprint->WidgetTree = Tree;
		}
		UCanvasPanel* Root = Tree->ConstructWidget<UCanvasPanel>(UCanvasPanel::StaticClass(), TEXT("Root"));
		UButton* ActionButton = Tree->ConstructWidget<UButton>(UButton::StaticClass(), TEXT("ActionButton"));
		if (!Root || !ActionButton) return false;
		Tree->RootWidget = Root;
		ActionButton->bIsVariable = true;
		Root->AddChild(ActionButton);
		if (bAddIncompatibleComponent)
		{
			UCanvasPanel* IncompatiblePanel =
				Tree->ConstructWidget<UCanvasPanel>(UCanvasPanel::StaticClass(), TEXT("IncompatiblePanel"));
			if (!IncompatiblePanel) return false;
			IncompatiblePanel->bIsVariable = true;
			Root->AddChild(IncompatiblePanel);
		}

		if (Blueprint->UbergraphPages.Num() == 0)
		{
			Graph = FBlueprintEditorUtils::CreateNewGraph(Blueprint, UEdGraphSchema_K2::GN_EventGraph,
				UEdGraph::StaticClass(), UEdGraphSchema_K2::StaticClass());
			FBlueprintEditorUtils::AddUbergraphPage(Blueprint, Graph);
		}
		else Graph = Blueprint->UbergraphPages[0];
		if (!Graph) return false;

		// The widget variable becomes a generated object property only after a widget compile, and the
		// component-bound node must bind to that real property.
		FKismetEditorUtilities::CompileBlueprint(Blueprint);
		FObjectProperty* FoundComponentProperty = Blueprint->SkeletonGeneratedClass
			? FindFProperty<FObjectProperty>(Blueprint->SkeletonGeneratedClass, TEXT("ActionButton")) : nullptr;
		if (!FoundComponentProperty && Blueprint->GeneratedClass)
		{
			FoundComponentProperty = FindFProperty<FObjectProperty>(Blueprint->GeneratedClass, TEXT("ActionButton"));
		}
		if (bAddIncompatibleComponent)
		{
			IncompatibleComponentProperty = Blueprint->SkeletonGeneratedClass
				? FindFProperty<FObjectProperty>(Blueprint->SkeletonGeneratedClass, TEXT("IncompatiblePanel")) : nullptr;
			if (!IncompatibleComponentProperty && Blueprint->GeneratedClass)
			{
				IncompatibleComponentProperty =
					FindFProperty<FObjectProperty>(Blueprint->GeneratedClass, TEXT("IncompatiblePanel"));
			}
			if (!IncompatibleComponentProperty) return false;
		}
		FMulticastDelegateProperty* FoundClickedDelegate =
			FindFProperty<FMulticastDelegateProperty>(UButton::StaticClass(), TEXT("OnClicked"));
		if (!FoundComponentProperty || !FoundClickedDelegate) return false;
		ComponentProperty = FoundComponentProperty;
		ClickedDelegate = FoundClickedDelegate;

		Retained = AddOverride(TEXT("OnRetainedCosmeticEvent"), UCortexGraphRetireNativeLifecycleWidget::StaticClass());
		RetainedBody = AddBody(Retained);
		Construct = AddOverride(TEXT("Construct"), UCortexGraphRetireNativeLifecycleWidget::StaticClass());
		ConstructBody = AddBody(Construct);
		Initialized = AddOverride(TEXT("OnInitialized"), UCortexGraphRetireNativeLifecycleWidget::StaticClass());
		InitializedBody = AddBody(Initialized);

		Custom = NewObject<UK2Node_CustomEvent>(Graph, NAME_None, RF_Transactional);
		Custom->CustomFunctionName = TEXT("OnLegacyTutorialEvent");
		Custom->CreateNewGuid();
		Custom->AllocateDefaultPins();
		Graph->AddNode(Custom, true, false);
		CustomBody = AddBody(Custom);

		// Same sequence the editor uses for a component binding: initialize from the compiled component
		// property and the native delegate, add the node, then allocate the event pins.
		Bound = NewObject<UK2Node_ComponentBoundEvent>(Graph, NAME_None, RF_Transactional);
		Bound->CreateNewGuid();
		Bound->InitializeComponentBoundEventParams(ComponentProperty, ClickedDelegate);
		Graph->AddNode(Bound, true, false);
		Bound->AllocateDefaultPins();
		BoundBody = AddBody(Bound);

		if (!Retained || !RetainedBody || !Construct || !ConstructBody || !Initialized || !InitializedBody
			|| !Custom || !CustomBody || !Bound || !BoundBody) return false;
		RetainedGuid = Retained->NodeGuid;
		RetainedBodyGuid = RetainedBody->NodeGuid;
		ConstructGuid = Construct->NodeGuid;
		ConstructBodyGuid = ConstructBody->NodeGuid;
		InitializedGuid = Initialized->NodeGuid;
		InitializedBodyGuid = InitializedBody->NodeGuid;
		CustomGuid = Custom->NodeGuid;
		CustomBodyGuid = CustomBody->NodeGuid;
		BoundGuid = Bound->NodeGuid;
		BoundBodyGuid = BoundBody->NodeGuid;

		// The second compile materializes the generated binding function and the callable custom event
		// function, and registers the real dynamic component delegate binding on the generated class.
		FKismetEditorUtilities::CompileBlueprint(Blueprint);
		if (Bound) Bound->ReconstructNode();
		return RefreshPointers();
	}

	void Cleanup()
	{
		if (Blueprint) { Blueprint->ClearFlags(RF_Standalone); Blueprint->MarkAsGarbage(); Blueprint = nullptr; }
		if (Package) { Package->ClearFlags(RF_Standalone); Package->MarkAsGarbage(); Package = nullptr; }
	}

	~FGuardedEntryFixture()
	{
		DeleteCortexGraphTestPackageFile(Package, OwnedPackageName);
		Cleanup();
	}
};

bool Plan(FGuardedEntryFixture& Fixture, const TArray<FString>& Entries, FCortexGraphMigrationRetirePlan& OutPlan,
	bool& bReused, FCortexCommandResult& Error, const bool bApproved = false, const TArray<FString>& Approved = {})
{
	return FCortexGraphMigrationOps::PlanRetirement(Fixture.Blueprint,
		Fixture.Migration(Entries, bApproved, Approved), OutPlan, bReused, Error);
}

/** Two-stage guarded apply request: preview, echo the published removable set, then approve it. */
bool PrepareGuardedRequest(
	FGuardedEntryFixture& Fixture,
	const TArray<FString>& Selected,
	const TCHAR* PatchId,
	TSharedPtr<FJsonObject>& OutRequest,
	TArray<FString>& OutApproved,
	FCortexCommandResult& OutError,
	const bool bCompile = true,
	const bool bSave = false)
{
	OutRequest = MakeShared<FJsonObject>();
	OutRequest->SetStringField(TEXT("asset_path"), Fixture.Blueprint->GetPathName());
	OutRequest->SetStringField(TEXT("patch_id"), PatchId);
	OutRequest->SetObjectField(TEXT("expected_fingerprint"), FCortexGraphPatchState::ComputeFingerprint(Fixture.Blueprint));
	OutRequest->SetArrayField(TEXT("nodes"), {});
	OutRequest->SetArrayField(TEXT("connections"), {});
	OutRequest->SetArrayField(TEXT("pin_updates"), {});
	OutRequest->SetBoolField(TEXT("dry_run"), true);
	OutRequest->SetBoolField(TEXT("compile"), bCompile);
	OutRequest->SetBoolField(TEXT("save"), false);
	OutRequest->SetBoolField(TEXT("allow_noop"), false);
	TSharedPtr<FJsonObject> Migration = Fixture.Migration(Selected);
	OutRequest->SetObjectField(TEXT("migration"), Migration);

	FCortexGraphPreparedPatch Preview;
	if (!FCortexGraphPatchOps::Preflight(Fixture.Blueprint, OutRequest, Preview, OutError)) return false;
	const TSharedPtr<FJsonObject> Inventory =
		FCortexGraphMigrationOps::MakeRetirementInventory(Preview.RetirementPlan);
	const TArray<TSharedPtr<FJsonValue>>* Removable = nullptr;
	if (!Inventory.IsValid() || !Inventory->TryGetArrayField(TEXT("removable"), Removable) || !Removable)
	{
		OutError = FCortexCommandRouter::Error(CortexErrorCodes::InvalidOperation,
			TEXT("guarded retirement preview did not publish its removable set"));
		return false;
	}
	for (const TSharedPtr<FJsonValue>& Value : *Removable) OutApproved.Add(Value->AsString());
	TArray<TSharedPtr<FJsonValue>> ApprovalValues;
	for (const FString& Guid : OutApproved) ApprovalValues.Add(MakeShared<FJsonValueString>(Guid));
	Migration->SetArrayField(TEXT("approved_node_guids"), ApprovalValues);

	FCortexGraphPreparedPatch Reviewed;
	if (!FCortexGraphPatchOps::Preflight(Fixture.Blueprint, OutRequest, Reviewed, OutError)) return false;
	OutRequest->SetBoolField(TEXT("dry_run"), false);
	OutRequest->SetBoolField(TEXT("save"), bSave);
	OutRequest->SetStringField(TEXT("expected_validation_hash"), Reviewed.ValidationHash);
	return true;
}

/**
 * Preview-only guarded request: the refusal must arrive before any approval echo can exist, so the
 * published removable set is deliberately not echoed back here.
 */
bool PreviewRetirement(
	FGuardedEntryFixture& Fixture,
	const TArray<FString>& Selected,
	const TCHAR* PatchId,
	FCortexGraphPreparedPatch& OutPreview,
	FCortexCommandResult& OutError)
{
	TSharedPtr<FJsonObject> Request = MakeShared<FJsonObject>();
	Request->SetStringField(TEXT("asset_path"), Fixture.Blueprint->GetPathName());
	Request->SetStringField(TEXT("patch_id"), PatchId);
	Request->SetObjectField(TEXT("expected_fingerprint"), FCortexGraphPatchState::ComputeFingerprint(Fixture.Blueprint));
	Request->SetArrayField(TEXT("nodes"), {});
	Request->SetArrayField(TEXT("connections"), {});
	Request->SetArrayField(TEXT("pin_updates"), {});
	Request->SetBoolField(TEXT("dry_run"), true);
	Request->SetBoolField(TEXT("compile"), true);
	Request->SetBoolField(TEXT("save"), false);
	Request->SetBoolField(TEXT("allow_noop"), false);
	Request->SetObjectField(TEXT("migration"), Fixture.Migration(Selected));
	return FCortexGraphPatchOps::Preflight(Fixture.Blueprint, Request, OutPreview, OutError);
}

/**
 * A real second Widget Blueprint asset whose event graph calls one generated function of another
 * asset. `UK2Node_CustomEvent` compiles its node into a `BlueprintCallable` `Public` function, so
 * this is exactly the cross-asset call site a retirement must not silently sever.
 */
struct FExternalCallerFixture
{
	UPackage* Package = nullptr;
	FString OwnedPackageName;
	UWidgetBlueprint* Blueprint = nullptr;
	UEdGraph* Graph = nullptr;
	UK2Node_CallFunction* Caller = nullptr;
	/** The asset name the package holds, kept after the asset object itself is unloaded. */
	FName AssetName;

	/** The class of the target asset that owns its compiled custom-event generated function. */
	static UClass* ResolveCallableOwner(UBlueprint* Target, const FName FunctionName)
	{
		UClass* const Generated = Target ? Target->GeneratedClass : nullptr;
		return Generated && Generated->FindFunctionByName(FunctionName) ? Generated : nullptr;
	}

	FString Filename() const
	{
		return Package ? FPackageName::LongPackageNameToFilename(Package->GetName(),
			FPackageName::GetAssetPackageExtension()) : FString();
	}

	FString PackageName() const
	{
		return Package ? Package->GetName() : FString();
	}

	FString ObjectPath() const
	{
		return !Package || AssetName.IsNone()
			? FString() : Package->GetName() + TEXT(".") + AssetName.ToString();
	}

	bool SaveToDisk()
	{
		FSavePackageArgs SaveArgs;
		SaveArgs.TopLevelFlags = RF_Public | RF_Standalone;
		return UPackage::SavePackage(Package, Blueprint, *Filename(), SaveArgs);
	}

	/**
	 * Drops the asset object while keeping its package resident: the exact state a referencer is in
	 * when the editor still holds the `UPackage` (a dirty tracker, a pending reload, a partially
	 * loaded package) but the Blueprint inside it was garbage collected. `FindPackage` alone cannot
	 * tell that state apart from an inspectable referencer, which is the guard under test.
	 *
	 * Uses the engine's own unload idiom (`UPackageTools::UnloadPackages`): root the package so it
	 * survives, clear `RF_Standalone` on the objects inside it, detach the linker, then collect.
	 */
	bool UnloadAssetKeepingPackageResident()
	{
		if (!Package || !Blueprint) return false;
		const FString ResidentPackageName = Package->GetName();
		if (GEditor && GEditor->Trans)
		{
			GEditor->Trans->Reset(FText::FromString(TEXT("CortexGraphMigrationRetireReferencerUnload")));
		}
		Package->SetFlags(RF_Standalone);
		Blueprint->ClearEditorReferences();
		Blueprint->ClearFlags(RF_Standalone);
		Blueprint->MarkAsGarbage();
		ResetLoaders(Package);
		Package->SetDirtyFlag(false);
		Blueprint = nullptr;
		Graph = nullptr;
		Caller = nullptr;
		FlushAsyncLoading();
		CollectGarbage(GARBAGE_COLLECTION_KEEPFLAGS);
		UPackage* const ResidentPackage = FindPackage(nullptr, *ResidentPackageName);
		Package = ResidentPackage;
		if (ResidentPackage) ResidentPackage->SetDirtyFlag(false);
		return ResidentPackage != nullptr && FindObject<UBlueprint>(ResidentPackage, *AssetName.ToString()) == nullptr;
	}

	/**
	 * Unloads the referencer entirely, leaving only its bytes on disk: a referencer the registry
	 * reports but nothing in memory can inspect without loading it.
	 */
	bool UnloadCompletely()
	{
		if (!Package || !Blueprint) return false;
		const FString UnloadedPackageName = Package->GetName();
		if (GEditor && GEditor->Trans)
		{
			GEditor->Trans->Reset(FText::FromString(TEXT("CortexGraphMigrationRetireReferencerUnload")));
		}
		Blueprint->ClearEditorReferences();
		Blueprint->ClearFlags(RF_Standalone);
		Blueprint->MarkAsGarbage();
		Package->ClearFlags(RF_Standalone);
		Package->MarkAsGarbage();
		ResetLoaders(Package);
		Blueprint = nullptr;
		Graph = nullptr;
		Caller = nullptr;
		Package = nullptr;
		FlushAsyncLoading();
		CollectGarbage(GARBAGE_COLLECTION_KEEPFLAGS);
		return FindPackage(nullptr, *UnloadedPackageName) == nullptr;
	}

	bool Build(const TCHAR* Name, UBlueprint* Target, const FName FunctionName)
	{
		UClass* const OwnerClass = ResolveCallableOwner(Target, FunctionName);
		if (!OwnerClass) return false;
		Package = CreateCortexGraphTestPackage(Name, OwnedPackageName);
		Blueprint = Cast<UWidgetBlueprint>(FKismetEditorUtilities::CreateBlueprint(
			UUserWidget::StaticClass(), Package, FName(Name), BPTYPE_Normal,
			UWidgetBlueprint::StaticClass(), UWidgetBlueprintGeneratedClass::StaticClass()));
		if (!Blueprint) return false;
		AssetName = FName(Name);

		UWidgetTree* Tree = Blueprint->WidgetTree;
		if (!Tree)
		{
			Tree = NewObject<UWidgetTree>(Blueprint, UWidgetTree::StaticClass(), TEXT("WidgetTree"));
			Blueprint->WidgetTree = Tree;
		}
		UCanvasPanel* Root = Tree->ConstructWidget<UCanvasPanel>(UCanvasPanel::StaticClass(), TEXT("Root"));
		if (!Root) return false;
		Tree->RootWidget = Root;

		if (Blueprint->UbergraphPages.Num() == 0)
		{
			Graph = FBlueprintEditorUtils::CreateNewGraph(Blueprint, UEdGraphSchema_K2::GN_EventGraph,
				UEdGraph::StaticClass(), UEdGraphSchema_K2::StaticClass());
			FBlueprintEditorUtils::AddUbergraphPage(Blueprint, Graph);
		}
		else Graph = Blueprint->UbergraphPages[0];
		if (!Graph) return false;

		Caller = NewObject<UK2Node_CallFunction>(Graph, NAME_None, RF_Transactional);
		// `SetExternalMember` runs the parent through `GetAuthoritativeClass`, so a Blueprint parent is
		// stored as its generated class: the real owner class a cross-asset call node references.
		Caller->FunctionReference.SetExternalMember(FunctionName, OwnerClass);
		Caller->CreateNewGuid();
		Caller->AllocateDefaultPins();
		Graph->AddNode(Caller, true, false);
		return Caller->FindPin(TEXT("execute")) != nullptr;
	}

	void Cleanup()
	{
		if (Blueprint) { Blueprint->ClearFlags(RF_Standalone); Blueprint->MarkAsGarbage(); Blueprint = nullptr; }
		if (Package) { Package->ClearFlags(RF_Standalone); Package->MarkAsGarbage(); Package = nullptr; }
	}

	~FExternalCallerFixture()
	{
		DeleteCortexGraphTestPackageFile(Package, OwnedPackageName);
		Cleanup();
	}
};

/**
 * Removes every in-memory trace of one referencer package, whatever state the preview left it in:
 * the resident package without its asset, or the package a preview auto-loaded. Ends with a
 * collection so the file can be deleted afterwards.
 */
void PurgeReferencerPackage(const FString& PackageName)
{
	if (UPackage* const Resident = FindPackage(nullptr, *PackageName))
	{
		if (GEditor && GEditor->Trans)
		{
			GEditor->Trans->Reset(FText::FromString(TEXT("CortexGraphMigrationRetireReferencerCleanup")));
		}
		TArray<UObject*> ObjectsInPackage;
		GetObjectsWithPackage(Resident, ObjectsInPackage);
		for (UObject* Object : ObjectsInPackage)
		{
			if (UBlueprint* const LoadedBlueprint = Cast<UBlueprint>(Object)) LoadedBlueprint->ClearEditorReferences();
		}
		for (UObject* Object : ObjectsInPackage)
		{
			if (Object && Object->HasAnyFlags(RF_Standalone)) Object->ClearFlags(RF_Standalone);
		}
		Resident->ClearFlags(RF_Standalone);
		Resident->SetDirtyFlag(false);
		Resident->MarkAsGarbage();
		ResetLoaders(Resident);
	}
	FlushAsyncLoading();
	CollectGarbage(GARBAGE_COLLECTION_KEEPFLAGS);
}

/**
 * Makes the editor Asset Registry aware of the two just-saved packages by reading their files, then
 * verifies the referencer relationship the preview is about to be asked about. The Asset Registry
 * is the only source of on-disk referencer identities, so the fixture proves its own precondition
 * and reports the limitation instead of silently testing an unrelated code path.
 */
bool ScanSavedReferencers(
	const TArray<FString>& Files,
	const FName TargetPackageName,
	const FName ReferencerPackageName,
	FString& OutFailure)
{
	OutFailure.Reset();
	IAssetRegistry* const AssetRegistry = IAssetRegistry::Get();
	if (!AssetRegistry)
	{
		OutFailure = TEXT("the Asset Registry is unavailable");
		return false;
	}
	AssetRegistry->WaitForCompletion();
	if (AssetRegistry->IsLoadingAssets())
	{
		OutFailure = TEXT("the Asset Registry is still loading assets, so the referencer investigation would be incomplete for an unrelated reason");
		return false;
	}
	AssetRegistry->ScanFilesSynchronous(Files, /*bForceRescan=*/true);
	if (AssetRegistry->IsLoadingAssets())
	{
		OutFailure = TEXT("the Asset Registry is still loading assets after the fixture scan, so the referencer investigation would be incomplete for an unrelated reason");
		return false;
	}
	TArray<FName> Referencers;
	if (!AssetRegistry->GetReferencers(TargetPackageName, Referencers,
		UE::AssetRegistry::EDependencyCategory::Package))
	{
		OutFailure = FString::Printf(TEXT("the Asset Registry has no dependency node for target package '%s'"),
			*TargetPackageName.ToString());
		return false;
	}
	if (!Referencers.Contains(ReferencerPackageName))
	{
		TArray<FString> ReportedNames;
		for (const FName& Referencer : Referencers) ReportedNames.Add(Referencer.ToString());
		OutFailure = FString::Printf(
			TEXT("the saved referencer package '%s' is not reported for target package '%s' (reported: %s)"),
			*ReferencerPackageName.ToString(), *TargetPackageName.ToString(),
			ReportedNames.IsEmpty() ? TEXT("none") : *FString::Join(ReportedNames, TEXT(", ")));
		return false;
	}
	return true;
}

bool ReferencerRefusalIsDiagnostic(const FCortexCommandResult& Error, const FString& ReferencerName)
{
	const bool bNamesReferencer = Error.ErrorMessage.Contains(ReferencerName)
		|| Error.ErrorMessage.Contains(TEXT("BP_RetireResidentReferencerHost"))
		|| Error.ErrorMessage.Contains(TEXT("BP_RetireNonresidentReferencerHost"));
	const bool bExplainsWhy = Error.ErrorMessage.Contains(TEXT("inspect"))
		|| Error.ErrorMessage.Contains(TEXT("not loaded"))
		|| Error.ErrorMessage.Contains(TEXT("could not be loaded"))
		|| Error.ErrorMessage.Contains(TEXT("cannot be ruled out"))
		|| Error.ErrorMessage.Contains(TEXT("unloaded"))
		|| Error.ErrorMessage.Contains(TEXT("resident"));
	return bNamesReferencer && bExplainsWhy;
}

/**
 * Attaches the reviewed `migration.source.additional_node_guids` list to a migration object the
 * existing `FFixture::Migration` helper built, so a test never hand-rolls the source object. The
 * field is always written, so an empty list is a request for nothing instead of a missing field.
 */
TSharedPtr<FJsonObject> WithAdditionalNodeGuids(TSharedPtr<FJsonObject> Migration,
	const TArray<FString>& AdditionalGuids, const TCHAR* Field = TEXT("additional_node_guids"))
{
	const TSharedPtr<FJsonObject>* Source = nullptr;
	if (!Migration.IsValid() || !Migration->TryGetObjectField(TEXT("source"), Source)
		|| !Source || !Source->IsValid()) return Migration;
	TArray<TSharedPtr<FJsonValue>> Values;
	for (const FString& Guid : AdditionalGuids) Values.Add(MakeShared<FJsonValueString>(Guid));
	(*Source)->SetArrayField(Field, Values);
	return Migration;
}

/** Reviewed retirement plan for a request that names explicit additional nodes. */
bool PlanWithAdditionalNodes(FFixture& Fixture, const TArray<FString>& Entries,
	const TArray<FString>& AdditionalGuids, FCortexGraphMigrationRetirePlan& OutPlan, bool& bReused,
	FCortexCommandResult& OutError, const bool bApproved = false, const TArray<FString>& Approved = {})
{
	return FCortexGraphMigrationOps::PlanRetirement(Fixture.Blueprint,
		WithAdditionalNodeGuids(Fixture.Migration(Entries, bApproved, Approved), AdditionalGuids),
		OutPlan, bReused, OutError);
}

/** Preview-only request that names explicit additional nodes; no approval echo is involved. */
bool PreviewAdditionalRetirement(FFixture& Fixture, const TArray<FString>& Entries,
	const TArray<FString>& AdditionalGuids, const TCHAR* PatchId, FCortexGraphPreparedPatch& OutPreview,
	FCortexCommandResult& OutError)
{
	TSharedPtr<FJsonObject> Request = MakeShared<FJsonObject>();
	Request->SetStringField(TEXT("asset_path"), Fixture.Blueprint->GetPathName());
	Request->SetStringField(TEXT("patch_id"), PatchId);
	Request->SetObjectField(TEXT("expected_fingerprint"), FCortexGraphPatchState::ComputeFingerprint(Fixture.Blueprint));
	Request->SetArrayField(TEXT("nodes"), {});
	Request->SetArrayField(TEXT("connections"), {});
	Request->SetArrayField(TEXT("pin_updates"), {});
	Request->SetBoolField(TEXT("dry_run"), true);
	Request->SetBoolField(TEXT("compile"), false);
	Request->SetBoolField(TEXT("save"), false);
	Request->SetBoolField(TEXT("allow_noop"), false);
	Request->SetObjectField(TEXT("migration"), WithAdditionalNodeGuids(Fixture.Migration(Entries), AdditionalGuids));
	return FCortexGraphPatchOps::Preflight(Fixture.Blueprint, Request, OutPreview, OutError);
}

/**
 * Two-stage reviewed retirement request that names explicit additional nodes: preview, echo the
 * published removable set, then approve it. Mirrors `PrepareRetirementRequest`, which is the same
 * sequence without the additional-node list.
 */
bool PrepareAdditionalRetirementRequest(
	FFixture& Fixture,
	const TArray<FString>& Entries,
	const TArray<FString>& AdditionalGuids,
	const TCHAR* PatchId,
	TSharedPtr<FJsonObject>& OutRequest,
	TArray<FString>& OutApproved,
	FCortexCommandResult& OutError,
	const bool bCompile = false,
	const bool bSave = false,
	const TArray<FString>& OrphanGuids = {})
{
	OutRequest = MakeShared<FJsonObject>();
	OutRequest->SetStringField(TEXT("asset_path"), Fixture.Blueprint->GetPathName());
	OutRequest->SetStringField(TEXT("patch_id"), PatchId);
	OutRequest->SetObjectField(TEXT("expected_fingerprint"), FCortexGraphPatchState::ComputeFingerprint(Fixture.Blueprint));
	OutRequest->SetArrayField(TEXT("nodes"), {});
	OutRequest->SetArrayField(TEXT("connections"), {});
	OutRequest->SetArrayField(TEXT("pin_updates"), {});
	OutRequest->SetBoolField(TEXT("dry_run"), true);
	OutRequest->SetBoolField(TEXT("compile"), bCompile);
	OutRequest->SetBoolField(TEXT("save"), false);
	OutRequest->SetBoolField(TEXT("allow_noop"), false);
	TSharedPtr<FJsonObject> Migration = WithAdditionalNodeGuids(Fixture.Migration(Entries), AdditionalGuids);
	WithAdditionalNodeGuids(Migration, OrphanGuids, TEXT("orphan_component_node_guids"));
	OutRequest->SetObjectField(TEXT("migration"), Migration);

	FCortexGraphPreparedPatch Preview;
	if (!FCortexGraphPatchOps::Preflight(Fixture.Blueprint, OutRequest, Preview, OutError)) return false;
	const TSharedPtr<FJsonObject> Inventory =
		FCortexGraphMigrationOps::MakeRetirementInventory(Preview.RetirementPlan);
	const TArray<TSharedPtr<FJsonValue>>* Removable = nullptr;
	if (!Inventory.IsValid() || !Inventory->TryGetArrayField(TEXT("removable"), Removable) || !Removable)
	{
		OutError = FCortexCommandRouter::Error(CortexErrorCodes::InvalidOperation,
			TEXT("additional-node retirement preview did not publish its removable set"));
		return false;
	}
	for (const TSharedPtr<FJsonValue>& Value : *Removable) OutApproved.Add(Value->AsString());
	TArray<TSharedPtr<FJsonValue>> ApprovalValues;
	for (const FString& Guid : OutApproved) ApprovalValues.Add(MakeShared<FJsonValueString>(Guid));
	Migration->SetArrayField(TEXT("approved_node_guids"), ApprovalValues);

	FCortexGraphPreparedPatch Reviewed;
	if (!FCortexGraphPatchOps::Preflight(Fixture.Blueprint, OutRequest, Reviewed, OutError)) return false;
	OutRequest->SetBoolField(TEXT("dry_run"), false);
	OutRequest->SetBoolField(TEXT("save"), bSave);
	OutRequest->SetStringField(TEXT("expected_validation_hash"), Reviewed.ValidationHash);
	return true;
}

/** Canonical ascending GUID list: the order a durable plan must publish. */
TArray<FString> CanonicalGuids(TArray<FString> Guids)
{
	Guids.Sort();
	return Guids;
}

/** String values of one JSON array field; empty when the object is null or the field is absent. */
TArray<FString> JsonStringArray(const TSharedPtr<FJsonObject>& Json, const TCHAR* Field)
{
	TArray<FString> Values;
	const TArray<TSharedPtr<FJsonValue>>* Array = nullptr;
	if (Json.IsValid() && Json->TryGetArrayField(Field, Array) && Array)
	{
		for (const TSharedPtr<FJsonValue>& Value : *Array)
		{
			if (Value.IsValid()) Values.Add(Value->AsString());
		}
	}
	return Values;
}

/** First pin of one node matching a direction and category, so a fixture never guesses pin names. */
UEdGraphPin* FindTypedPin(UEdGraphNode* Node, const EEdGraphPinDirection Direction, const FName PinCategory)
{
	if (!Node) return nullptr;
	for (UEdGraphPin* Pin : Node->Pins)
	{
		if (Pin && Pin->Direction == Direction && Pin->PinType.PinCategory == PinCategory) return Pin;
	}
	return nullptr;
}

/** First non-execution pin of one node in the requested direction, whatever its category. */
UEdGraphPin* FindDataPin(UEdGraphNode* Node, const EEdGraphPinDirection Direction)
{
	if (!Node) return nullptr;
	for (UEdGraphPin* Pin : Node->Pins)
	{
		if (Pin && Pin->Direction == Direction && Pin->PinType.PinCategory != UEdGraphSchema_K2::PC_Exec) return Pin;
	}
	return nullptr;
}

/** `name:in|out:category` for every pin of one node, so a failure message names the real interface. */
FString PinSummary(const UEdGraphNode* Node)
{
	if (!Node) return TEXT("no node");
	TArray<FString> Items;
	for (const UEdGraphPin* Pin : Node->Pins)
	{
		if (!Pin) continue;
		Items.Add(FString::Printf(TEXT("%s:%s:%s"), *Pin->PinName.ToString(),
			Pin->Direction == EGPD_Input ? TEXT("in") : TEXT("out"), *Pin->PinType.PinCategory.ToString()));
	}
	return FString::Join(Items, TEXT(", "));
}

/**
 * Connects two pins through the schema, recording the exact refusal or missing pin in `OutFailure`.
 * The schema response is the evidence a fixture failure reports, so a wiring problem is never silent.
 */
bool ConnectTypedPins(const UEdGraphSchema* Schema, UEdGraphPin* From, UEdGraphPin* To,
	const TCHAR* Stage, FString& OutFailure)
{
	if (!From || !To)
	{
		OutFailure = FString::Printf(TEXT("%s: pin missing (from=%s, to=%s)"), Stage,
			From ? *PinSummary(From->GetOwningNode()) : TEXT("none"),
			To ? *PinSummary(To->GetOwningNode()) : TEXT("none"));
		return false;
	}
	const FPinConnectionResponse Response = Schema->CanCreateConnection(From, To);
	if (Response.Response == CONNECT_RESPONSE_DISALLOW)
	{
		OutFailure = FString::Printf(TEXT("%s: schema refused '%s'(%s) -> '%s'(%s): %s"), Stage,
			*From->PinName.ToString(), *From->PinType.PinCategory.ToString(),
			*To->PinName.ToString(), *To->PinType.PinCategory.ToString(), *Response.Message.ToString());
		return false;
	}
	if (!Schema->TryCreateConnection(From, To))
	{
		OutFailure = FString::Printf(TEXT("%s: schema connection returned false after response %d"),
			Stage, static_cast<int32>(Response.Response));
		return false;
	}
	return true;
}

/**
 * Ends one fixture case of a multi-case test.
 *
 * The engine transaction buffer keeps references to the transactional fixture nodes, so it is reset
 * before the objects are marked garbage, exactly as the referencer fixture in this file does before it
 * unloads. Without that reset a later blueprint compile in the same test can collect a fixture the
 * transaction buffer still points at.
 */
void EndFixtureCase(FFixture& Fixture)
{
	if (GEditor && GEditor->Trans)
	{
		GEditor->Trans->Reset(FText::FromString(TEXT("CortexGraphMigrationRetireFixtureCase")));
	}
	Fixture.Cleanup();
}

/**
 * The observed page's admitted non-entry nodes, built through engine APIs and linked into the
 * selected entry's execution island:
 *
 *   AlphaBody.then -> DelayUntilNextTick.execute -> AddDelegate.execute -> macro instance
 *   CreateDelegate.delegate -> AddDelegate.delegate
 *   an object producer -> the macro instance's data input (a real source, so the chain is
 *   compile-valid exactly like the page's `IsValid` node, whose input is fed by a real object)
 *
 * The macro graph comes from the engine's own standard macro library, so the fixture exercises the
 * same `UK2Node_MacroInstance` the page holds. `Build` records the exact stage and the observed pin
 * interface in `Failure` instead of returning silently.
 */
struct FAdmittedNodeChain
{
	UK2Node_CallFunction* Latent = nullptr;
	UK2Node_CreateDelegate* CreateDelegate = nullptr;
	UK2Node_AddDelegate* AddDelegate = nullptr;
	UK2Node_MacroInstance* Macro = nullptr;
	UK2Node_CallFunction* ObjectSource = nullptr;
	UEdGraph* MacroGraph = nullptr;
	/** Why `Build` returned false: the first stage that could not be wired, with the observed pins. */
	FString Failure;
	/**
	 * True when the macro instance's data input needs no source or is fed by the object producer. A
	 * wildcard input left unresolved would make the chain uncompilable, so this is the compile-validity
	 * state of the fixture as well as its evidence.
	 */
	bool bMacroDataInputFed = false;
	/** Name of the macro graph the fixture instantiated, so a fallback is never mistaken for the page's. */
	FString MacroGraphName;
	/**
	 * True only when the instantiated macro graph really is the engine's `IsValid` macro the page holds.
	 * The execution-capable fallback keeps the chain wireable but is deliberately not proof of the
	 * observed node, so the page-fidelity tests assert this flag.
	 */
	bool bMacroGraphIsIsValid = false;

	bool IsComplete() const
	{
		return Latent && CreateDelegate && AddDelegate && Macro && MacroGraph;
	}

	/** The explicitly requested additional identities of the chain. */
	TArray<FString> Guids() const
	{
		TArray<FString> Values;
		for (const UEdGraphNode* Node : { static_cast<const UEdGraphNode*>(Latent),
			static_cast<const UEdGraphNode*>(CreateDelegate), static_cast<const UEdGraphNode*>(AddDelegate),
			static_cast<const UEdGraphNode*>(Macro) })
		{
			if (Node) Values.Add(Node->NodeGuid.ToString());
		}
		return Values;
	}

	bool Build(FFixture& Fixture)
	{
		Failure.Reset();
		bMacroDataInputFed = false;
		MacroGraphName.Reset();
		bMacroGraphIsIsValid = false;
		if (!Fixture.Graph) { Failure = TEXT("fixture graph is missing"); return false; }
		const UEdGraphSchema* const Schema = Fixture.Graph->GetSchema();
		if (!Schema) { Failure = TEXT("fixture graph has no schema"); return false; }

		// Stage 1: the latent call on the selected entry's execution path.
		Latent = Fixture.AddCall(UKismetSystemLibrary::StaticClass()->FindFunctionByName(TEXT("DelayUntilNextTick")));
		if (!Latent) { Failure = TEXT("latent: DelayUntilNextTick node could not be created"); return false; }
		if (!Latent->FindPin(TEXT("execute")))
		{
			Failure = FString::Printf(TEXT("latent: no execute pin [%s]"), *PinSummary(Latent));
			return false;
		}
		if (!ConnectTypedPins(Schema, Fixture.AlphaBody->FindPin(TEXT("then")), Latent->FindPin(TEXT("execute")),
			TEXT("latent link"), Failure)) return false;

		// Stage 2: the delegate create/add pair.
		//
		// A delegate binding only compiles when the two nodes are real: the add node needs a Target object
		// of the delegate's owner class (`self is not a Button, Target must have connection`), and the
		// create node needs a selected function/event name that its scope can see. The fixture follows the
		// project's own pattern (`CortexGraphMigrationTest.cpp`): a typed member variable provides the
		// target object, the Blueprint is compiled so the variable resolves, and `SetFunction` names a real
		// parameterless callable function visible on the Button the delegate is declared on.
		FMulticastDelegateProperty* const ClickedDelegate =
			FindFProperty<FMulticastDelegateProperty>(UButton::StaticClass(), TEXT("OnClicked"));
		if (!ClickedDelegate) { Failure = TEXT("delegate: UButton::OnClicked is not available"); return false; }
		// The delegate target is a native property of the fixture's parent class, so the getter resolves it
		// through that class without adding a Blueprint variable and without any skeleton regeneration or
		// compile during fixture setup.
		UClass* const FixtureParent = Fixture.Blueprint->ParentClass;
		FObjectProperty* const TargetProperty = FixtureParent
			? FindFProperty<FObjectProperty>(FixtureParent, FName(TEXT("RetireButtonTarget"))) : nullptr;
		if (!TargetProperty || !TargetProperty->PropertyClass
			|| !TargetProperty->PropertyClass->IsChildOf(UButton::StaticClass()))
		{
			Failure = FString::Printf(
				TEXT("delegate: parent class '%s' does not expose a UButton RetireButtonTarget property"),
				FixtureParent ? *FixtureParent->GetName() : TEXT("none"));
			return false;
		}
		UK2Node_VariableGet* const TargetGet =
			NewObject<UK2Node_VariableGet>(Fixture.Graph, NAME_None, RF_Transactional);
		TargetGet->VariableReference.SetSelfMember(FName(TEXT("RetireButtonTarget")));
		TargetGet->CreateNewGuid();
		TargetGet->AllocateDefaultPins();
		Fixture.Graph->AddNode(TargetGet, true, false);
		UEdGraphPin* const TargetOut = FindTypedPin(TargetGet, EGPD_Output, UEdGraphSchema_K2::PC_Object);
		if (!TargetOut)
		{
			Failure = FString::Printf(TEXT("delegate: target variable get has no object output [%s]"), *PinSummary(TargetGet));
			return false;
		}
		CreateDelegate = NewObject<UK2Node_CreateDelegate>(Fixture.Graph, NAME_None, RF_Transactional);
		CreateDelegate->CreateNewGuid();
		CreateDelegate->AllocateDefaultPins();
		Fixture.Graph->AddNode(CreateDelegate, true, false);
		AddDelegate = NewObject<UK2Node_AddDelegate>(Fixture.Graph, NAME_None, RF_Transactional);
		AddDelegate->SetFromProperty(ClickedDelegate, /*bSelfContext=*/false, UButton::StaticClass());
		AddDelegate->CreateNewGuid();
		AddDelegate->AllocateDefaultPins();
		Fixture.Graph->AddNode(AddDelegate, true, false);
		UEdGraphPin* const CreatedDelegateOut = FindTypedPin(CreateDelegate, EGPD_Output, UEdGraphSchema_K2::PC_Delegate);
		if (!CreatedDelegateOut)
		{
			Failure = FString::Printf(TEXT("delegate: create node has no delegate output [%s]"), *PinSummary(CreateDelegate));
			return false;
		}
		UEdGraphPin* const AddedDelegateIn = FindTypedPin(AddDelegate, EGPD_Input, UEdGraphSchema_K2::PC_Delegate);
		if (!AddedDelegateIn)
		{
			Failure = FString::Printf(TEXT("delegate: add node has no delegate input [%s]"), *PinSummary(AddDelegate));
			return false;
		}
		if (!ConnectTypedPins(Schema, Latent->FindPin(TEXT("then")), AddDelegate->FindPin(TEXT("execute")),
			TEXT("delegate execution link"), Failure)) return false;
		if (!ConnectTypedPins(Schema, CreatedDelegateOut, AddedDelegateIn,
			TEXT("delegate binding link"), Failure)) return false;
		if (!ConnectTypedPins(Schema, TargetOut, AddDelegate->FindPin(UEdGraphSchema_K2::PN_Self),
			TEXT("delegate target link"), Failure)) return false;
		if (!ConnectTypedPins(Schema, TargetOut, CreateDelegate->FindPin(UEdGraphSchema_K2::PN_Self),
			TEXT("create delegate scope link"), Failure)) return false;
		CreateDelegate->SetFunction(FName(TEXT("SetFocus")));

		// Stage 3: the engine macro instance.
		UBlueprint* const StandardMacros = LoadObject<UBlueprint>(nullptr,
			TEXT("/Engine/EditorBlueprintResources/StandardMacros.StandardMacros"));
		if (!StandardMacros) { Failure = TEXT("macro: the engine standard macro library did not load"); return false; }
		TArray<UEdGraph*> StandardGraphs;
		StandardMacros->GetAllGraphs(StandardGraphs);
		StandardGraphs.Sort([](const UEdGraph& A, const UEdGraph& B)
		{
			return A.GetName().Compare(B.GetName()) < 0;
		});
		for (UEdGraph* Candidate : StandardGraphs)
		{
			if (Candidate && Candidate->GetName() == TEXT("IsValid")) { MacroGraph = Candidate; break; }
		}
		if (!MacroGraph)
		{
			for (UEdGraph* Candidate : StandardGraphs)
			{
				if (Candidate && Candidate->GetName().Contains(TEXT("IsValid"))) { MacroGraph = Candidate; break; }
			}
		}
		if (!MacroGraph)
		{
			// No `IsValid` graph: fall back to the first macro graph whose tunnel declares an execution
			// pin, which is the interface the chain needs, and keep the chosen graph name for evidence.
			for (UEdGraph* Candidate : StandardGraphs)
			{
				if (!Candidate) continue;
				for (UEdGraphNode* Node : Candidate->Nodes)
				{
					UK2Node_Tunnel* const Tunnel = Cast<UK2Node_Tunnel>(Node);
					if (Tunnel && FindTypedPin(Tunnel, EGPD_Output, UEdGraphSchema_K2::PC_Exec))
					{
						MacroGraph = Candidate;
						break;
					}
				}
				if (MacroGraph) break;
			}
		}
		if (!MacroGraph)
		{
			TArray<FString> Names;
			for (const UEdGraph* Candidate : StandardGraphs) if (Candidate) Names.Add(Candidate->GetName());
			Failure = FString::Printf(TEXT("macro: no IsValid graph and no execution-capable macro graph among [%s]"),
				*FString::Join(Names, TEXT(", ")));
			return false;
		}
		MacroGraphName = MacroGraph->GetName();
		bMacroGraphIsIsValid = MacroGraphName == TEXT("IsValid") || MacroGraphName.Contains(TEXT("IsValid"));

		Macro = NewObject<UK2Node_MacroInstance>(Fixture.Graph, NAME_None, RF_Transactional);
		Macro->SetMacroGraph(MacroGraph);
		Macro->CreateNewGuid();
		Macro->AllocateDefaultPins();
		Fixture.Graph->AddNode(Macro, true, false);
		UEdGraphPin* const MacroExecIn = FindTypedPin(Macro, EGPD_Input, UEdGraphSchema_K2::PC_Exec);
		if (!MacroExecIn)
		{
			Failure = FString::Printf(TEXT("macro: instance '%s' has no execution input [%s]"),
				*MacroGraph->GetName(), *PinSummary(Macro));
			return false;
		}
		if (!ConnectTypedPins(Schema, AddDelegate->FindPin(TEXT("then")), MacroExecIn,
			TEXT("macro execution link"), Failure)) return false;

		// Stage 4: the macro's data input, fed by a real object so a wildcard resolves at compile time.
		UEdGraphPin* const MacroDataIn = FindDataPin(Macro, EGPD_Input);
		if (MacroDataIn
			&& (MacroDataIn->PinType.PinCategory == UEdGraphSchema_K2::PC_Wildcard
				|| MacroDataIn->PinType.PinCategory == UEdGraphSchema_K2::PC_Object))
		{
			ObjectSource = Fixture.AddCall(UGameplayStatics::StaticClass()->FindFunctionByName(TEXT("GetGameInstance")));
			if (!ObjectSource)
			{
				Failure = TEXT("macro data: UGameplayStatics::GetGameInstance node could not be created");
				return false;
			}
			UEdGraphPin* const ObjectOut = FindTypedPin(ObjectSource, EGPD_Output, UEdGraphSchema_K2::PC_Object);
			if (!ObjectOut)
			{
				Failure = FString::Printf(TEXT("macro data: object source has no object output [%s]"), *PinSummary(ObjectSource));
				return false;
			}
			if (!ConnectTypedPins(Schema, ObjectOut, MacroDataIn, TEXT("macro data link"), Failure)) return false;
			bMacroDataInputFed = true;
		}
		else
		{
			// The macro declares no data input, or one that needs no source: nothing to resolve.
			bMacroDataInputFed = true;
		}
		return true;
	}
};

/** Adds one standalone reroute knot to the fixture graph. */
UK2Node_Knot* AddRerouteKnot(FFixture& Fixture)
{
	if (!Fixture.Graph) return nullptr;
	UK2Node_Knot* Knot = NewObject<UK2Node_Knot>(Fixture.Graph, NAME_None, RF_Transactional);
	Knot->CreateNewGuid();
	Knot->AllocateDefaultPins();
	Fixture.Graph->AddNode(Knot, true, false);
	return Knot;
}

/**
 * Reroutes one existing link through a new reroute knot: breaks `From.FromPin -> To.ToPin`, adds the
 * knot, then wires `From.FromPin -> knot -> To.ToPin`. The schema connection notifies the knot, which
 * propagates its wildcard pins from the surviving link, so an execution link stays an execution link.
 */
UK2Node_Knot* RerouteThroughKnot(FFixture& Fixture, UEdGraphNode* From, const TCHAR* FromPin,
	UEdGraphNode* To, const TCHAR* ToPin)
{
	UEdGraphPin* SourcePin = From ? From->FindPin(FName(FromPin)) : nullptr;
	UEdGraphPin* TargetPin = To ? To->FindPin(FName(ToPin)) : nullptr;
	const UEdGraphSchema* Schema = Fixture.Graph ? Fixture.Graph->GetSchema() : nullptr;
	if (!SourcePin || !TargetPin || !Schema) return nullptr;
	UK2Node_Knot* Knot = AddRerouteKnot(Fixture);
	if (!Knot) return nullptr;
	SourcePin->BreakLinkTo(TargetPin);
	if (!Schema->TryCreateConnection(SourcePin, Knot->GetInputPin())) return nullptr;
	if (!Schema->TryCreateConnection(Knot->GetOutputPin(), TargetPin)) return nullptr;
	return Knot;
}

}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexGraphMigrationRetireSelectedEntriesTest,
	"Cortex.Graph.Authoring.Migration.Retire.SelectedEntriesAndPrivateProducer",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexGraphMigrationRetireSelectedEntriesTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	using namespace CortexGraphMigrationRetireTest;
	FFixture Fixture;
	TestTrue(TEXT("Widget fixture is created"), Fixture.Build(TEXT("BP_RetireSelected")));
	if (!Fixture.Blueprint) { Fixture.Cleanup(); return false; }
	FCortexGraphMigrationRetirePlan PlanValue;
	bool bReused = false;
	FCortexCommandResult Error;
	TestTrue(FString::Printf(TEXT("retirement plan is computed: %s"), *Error.ErrorMessage),
		Plan(Fixture, { Fixture.Alpha->NodeGuid.ToString(), Fixture.Beta->NodeGuid.ToString() }, PlanValue, bReused, Error));
	TestTrue(TEXT("alpha is removable"), PlanValue.RemovableGuids.Contains(Fixture.Alpha->NodeGuid.ToString()));
	TestTrue(TEXT("beta is removable"), PlanValue.RemovableGuids.Contains(Fixture.Beta->NodeGuid.ToString()));
	TestTrue(TEXT("producer shared only by selected entries is removable"), PlanValue.RemovableGuids.Contains(Fixture.Producer->NodeGuid.ToString()));
	FCortexGraphMigrationRetirePlan RoundTrip;
	TestTrue(TEXT("durable plan round-trips"), FCortexGraphMigrationRetirePlan::FromJson(PlanValue.ToJson(), RoundTrip, Error));
	TestEqual(TEXT("round-trip keeps selected GUIDs"), RoundTrip.SelectedEntryGuids.Num(), 2);
	const TSharedPtr<FJsonObject> Inventory = FCortexGraphMigrationOps::MakeRetirementInventory(PlanValue.ToJson());
	TestNotNull(TEXT("retirement inventory is created"), Inventory.Get());
	if (Inventory.IsValid())
	{
		FString Status;
		TestTrue(TEXT("inventory reports explicit Blueprint status"), Inventory->TryGetStringField(TEXT("blueprint_status_before"), Status)
			&& Status.StartsWith(TEXT("BS_")));
		TestEqual(TEXT("inventory names cached diagnostic source"),
			Inventory->GetStringField(TEXT("preexisting_diagnostics_source")), FString(TEXT("cached_node_messages")));
	}
	Fixture.Cleanup();
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexGraphMigrationRetireStaleLegacyOverrideTest,
	"Cortex.Graph.Authoring.Migration.Retire.StaleLegacyOverrideAfterReparent",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexGraphMigrationRetireStaleLegacyOverrideTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	using namespace CortexGraphMigrationRetireTest;
	FFixture Fixture;
	TestTrue(TEXT("reparented Widget fixture is created"), Fixture.Build(TEXT("BP_RetireStaleLegacy"),
		false, false, UCortexGraphRetireTargetWidget::StaticClass()));
	if (!Fixture.Blueprint) { Fixture.Cleanup(); return false; }
	TestTrue(TEXT("the Widget BP is reparented away from the stale event parent"),
		!Fixture.Blueprint->ParentClass->IsChildOf(UCortexGraphRetireLegacyWidget::StaticClass()));
	TestEqual(TEXT("the event retains its legacy parent identity"),
		Fixture.Alpha->EventReference.GetMemberParentClass(), UCortexGraphRetireLegacyWidget::StaticClass());
	FCortexGraphMigrationRetirePlan PlanValue;
	bool bReused = false;
	FCortexCommandResult Error;
	const FString AlphaGuid = Fixture.Alpha->NodeGuid.ToString();
	TestTrue(FString::Printf(TEXT("stale legacy override remains eligible for retirement: %s"), *Error.ErrorMessage),
		Plan(Fixture, { AlphaGuid }, PlanValue, bReused, Error));
	TestTrue(TEXT("stale legacy override is removable"),
		PlanValue.RemovableGuids.Contains(AlphaGuid));
	Fixture.Cleanup();
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexGraphMigrationRetireSharedProducerTest,
	"Cortex.Graph.Authoring.Migration.Retire.RetainsSharedProducer",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexGraphMigrationRetireSharedProducerTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	using namespace CortexGraphMigrationRetireTest;
	FFixture Fixture;
	TestTrue(TEXT("Widget fixture is created"), Fixture.Build(TEXT("BP_RetireShared"), true));
	if (!Fixture.Blueprint) { Fixture.Cleanup(); return false; }
	FCortexGraphMigrationRetirePlan PlanValue;
	bool bReused = false;
	FCortexCommandResult Error;
	TestTrue(FString::Printf(TEXT("retirement plan is computed: %s"), *Error.ErrorMessage),
		Plan(Fixture, { Fixture.Alpha->NodeGuid.ToString(), Fixture.Beta->NodeGuid.ToString() }, PlanValue, bReused, Error));
	TestTrue(TEXT("producer is reported shared"), PlanValue.Shared.ContainsByPredicate([&](const FCortexGraphPruneNode& Node)
		{ return Node.NodeGuid == Fixture.Producer->NodeGuid.ToString(); }));
	TestFalse(TEXT("shared producer is not removable"), PlanValue.RemovableGuids.Contains(Fixture.Producer->NodeGuid.ToString()));
	Fixture.Cleanup();
	return true;
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexGraphMigrationRetireSharedExecutionBodyTest,
	"Cortex.Graph.Authoring.Migration.Retire.RefusesSharedExecutionBody",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexGraphMigrationRetireSharedExecutionBodyTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	using namespace CortexGraphMigrationRetireTest;
	FFixture Fixture;
	TestTrue(TEXT("Widget fixture is created"), Fixture.Build(TEXT("BP_RetireSharedExecutionBody")));
	if (!Fixture.Blueprint) { Fixture.Cleanup(); return false; }

	const UEdGraphSchema* Schema = Fixture.Graph->GetSchema();
	UEdGraphPin* RetainedThen = Fixture.Retained->FindPin(TEXT("then"));
	UEdGraphPin* SharedBodyExecute = Fixture.AlphaBody->FindPin(TEXT("execute"));
	TestTrue(TEXT("retained entry can share the selected entry's execution body"),
		Schema && RetainedThen && SharedBodyExecute && Schema->TryCreateConnection(RetainedThen, SharedBodyExecute));
	const FString GraphBefore = CaptureNativeGraph(Fixture.Graph);

	FCortexGraphMigrationRetirePlan PlanValue;
	bool bReused = false;
	FCortexCommandResult Error;
	const FString SelectedGuid = Fixture.Alpha->NodeGuid.ToString();
	TestTrue(FString::Printf(TEXT("retirement preview succeeds: %s"), *Error.ErrorMessage),
		Plan(Fixture, { SelectedGuid }, PlanValue, bReused, Error));
	TestTrue(TEXT("execution body with an incoming retained execution edge is blocked"),
		PlanValue.Blocked.ContainsByPredicate([&](const FCortexGraphPruneNode& Node)
			{ return Node.NodeGuid == Fixture.AlphaBody->NodeGuid.ToString(); }));
	TestFalse(TEXT("blocked execution body is not approved for removal"),
		PlanValue.RemovableGuids.Contains(Fixture.AlphaBody->NodeGuid.ToString()));
	TestTrue(TEXT("selected entry feeding the retained body is blocked"),
		PlanValue.Blocked.ContainsByPredicate([&](const FCortexGraphPruneNode& Node)
			{ return Node.NodeGuid == SelectedGuid; }));
	TestFalse(TEXT("selected entry is not approved for removal"), PlanValue.RemovableGuids.Contains(SelectedGuid));

	Error = FCortexCommandResult();
	TestFalse(TEXT("reviewed retirement refuses the blocked shared execution body"),
		Plan(Fixture, { SelectedGuid }, PlanValue, bReused, Error, true, PlanValue.RemovableGuids));
	TestEqual(TEXT("shared execution body refusal is INVALID_OPERATION"),
		Error.ErrorCode, FString(CortexErrorCodes::InvalidOperation));
	TestEqual(TEXT("refused retirement leaves the complete graph unchanged"),
		CaptureNativeGraph(Fixture.Graph), GraphBefore);

	Fixture.Cleanup();
	return true;
}

// RIP-44 / CortexSandbox #112 partition regression. The selected obsolete entry `Alpha` drives the
// execution body A, A's string output is consumed by the retained cosmetic body, and A drives the
// execution body B. A is retained only because that retained consumer uses its output, and that
// decision lands after a one-pass candidate sweep had already accepted B, so approving B deleted the
// retained A -> B execution link. The cut must reach a fixed point across both link kinds: B stays
// retained, or the reviewed request refuses instead of severing the retained link. A different,
// independent selected entry must still be retired, so the new closure may not over-retain.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexGraphMigrationRetireLateRetainedProducerTest,
	"Cortex.Graph.Authoring.Migration.Retire.RetainsLateRetainedProducerExecConsumer",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexGraphMigrationRetireLateRetainedProducerTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	using namespace CortexGraphMigrationRetireTest;
	FFixture Fixture;
	TestTrue(TEXT("Widget fixture is created"), Fixture.Build(TEXT("BP_RetireLateRetainedProducer")));
	if (!Fixture.Blueprint) { Fixture.Cleanup(); return false; }
	TestTrue(TEXT("the late-retained producer chain is created"), Fixture.AddLateRetainedProducerChain());
	if (!Fixture.RetainedProducer || !Fixture.RetainedProducerConsumer) { Fixture.Cleanup(); return false; }

	const FString ProducerGuidText = Fixture.RetainedProducerGuid.ToString();
	const FString ConsumerGuidText = Fixture.RetainedProducerConsumerGuid.ToString();
	const FString AlphaGuidText = Fixture.Alpha->NodeGuid.ToString();
	const FString BetaGuidText = Fixture.Beta->NodeGuid.ToString();

	FCortexGraphMigrationRetirePlan PlanValue;
	bool bReused = false;
	FCortexCommandResult Error;
	TestTrue(FString::Printf(TEXT("retirement preview succeeds: %s"), *Error.ErrorMessage),
		Plan(Fixture, { AlphaGuidText, BetaGuidText }, PlanValue, bReused, Error));

	auto IsShared = [&PlanValue](const FString& GuidText)
	{
		return PlanValue.Shared.ContainsByPredicate([&GuidText](const FCortexGraphPruneNode& Node)
			{ return Node.NodeGuid == GuidText; });
	};
	auto IsBlocked = [&PlanValue](const FString& GuidText)
	{
		return PlanValue.Blocked.ContainsByPredicate([&GuidText](const FCortexGraphPruneNode& Node)
			{ return Node.NodeGuid == GuidText; });
	};
	auto IsRetained = [&IsShared, &IsBlocked](const FString& GuidText)
	{
		return IsShared(GuidText) || IsBlocked(GuidText);
	};
	TestTrue(TEXT("the data-shared producer is retained rather than removable"),
		!PlanValue.RemovableGuids.Contains(ProducerGuidText) && IsShared(ProducerGuidText));
	TestTrue(TEXT("the selected entry feeding the retained producer is blocked"), IsBlocked(AlphaGuidText));

	TestFalse(TEXT("the execution consumer of the retained producer is not removable"),
		PlanValue.RemovableGuids.Contains(ConsumerGuidText));
	TestTrue(TEXT("the retained execution consumer is published as retained, never removable"),
		IsRetained(ConsumerGuidText));
	TestFalse(TEXT("the retained producer -> consumer execution link is not a boundary edge a removal severs"),
		PlanValue.ExternalEdges.ContainsByPredicate([&ProducerGuidText, &ConsumerGuidText](const FCortexGraphPruneEdge& Edge)
		{
			return Edge.FromGuid == ProducerGuidText && Edge.FromPin == TEXT("then")
				&& Edge.ToGuid == ConsumerGuidText && Edge.ToPin == TEXT("execute");
		}));

	TestTrue(TEXT("the independent selected entry stays removable"), PlanValue.RemovableGuids.Contains(BetaGuidText));
	TestTrue(TEXT("the independent selected entry body stays removable"),
		PlanValue.RemovableGuids.Contains(Fixture.BetaBody->NodeGuid.ToString()));

	// The caller-visible inventory must publish the same fixed point: the consumer is never offered.
	const TSharedPtr<FJsonObject> Inventory = FCortexGraphMigrationOps::MakeRetirementInventory(PlanValue.ToJson());
	TestNotNull(TEXT("the retirement inventory is created"), Inventory.Get());
	if (Inventory.IsValid())
	{
		const TArray<TSharedPtr<FJsonValue>>* Removable = nullptr;
		const TArray<TSharedPtr<FJsonValue>>* Shared = nullptr;
		const TArray<TSharedPtr<FJsonValue>>* BlockedNodes = nullptr;
		const TArray<TSharedPtr<FJsonValue>>* ExternalEdges = nullptr;
		TestTrue(TEXT("the inventory publishes its removable set"),
			Inventory->TryGetArrayField(TEXT("removable"), Removable) && Removable);
		TestTrue(TEXT("the inventory publishes its shared set"),
			Inventory->TryGetArrayField(TEXT("shared"), Shared) && Shared);
		TestTrue(TEXT("the inventory publishes its blocked set"),
			Inventory->TryGetArrayField(TEXT("blocked_nodes"), BlockedNodes) && BlockedNodes);
		TestTrue(TEXT("the inventory publishes its boundary edges"),
			Inventory->TryGetArrayField(TEXT("external_edges"), ExternalEdges) && ExternalEdges);
		if (Removable && Shared && BlockedNodes && ExternalEdges)
		{
			auto NamesGuid = [&ConsumerGuidText](const TArray<TSharedPtr<FJsonValue>>& Values)
			{
				return Values.ContainsByPredicate([&ConsumerGuidText](const TSharedPtr<FJsonValue>& Value)
					{ return Value->AsString().StartsWith(ConsumerGuidText); });
			};
			TestFalse(TEXT("the inventory never offers the retained execution consumer for approval"),
				Removable->ContainsByPredicate([&ConsumerGuidText](const TSharedPtr<FJsonValue>& Value)
					{ return Value->AsString() == ConsumerGuidText; }));
			TestTrue(TEXT("the inventory names the retained execution consumer as retained"),
				NamesGuid(*Shared) || NamesGuid(*BlockedNodes));
			TestFalse(TEXT("the inventory publishes no boundary edge for the retained execution link"),
				ExternalEdges->ContainsByPredicate([&ProducerGuidText, &ConsumerGuidText](const TSharedPtr<FJsonValue>& Value)
					{ return Value->AsString() == FString::Printf(TEXT("%s.then -> %s.execute"), *ProducerGuidText, *ConsumerGuidText); }));
		}
	}

	// Fail closed: the reviewed request may not approve a cut that severs the retained link.
	TSharedPtr<FJsonObject> Request;
	TArray<FString> Approved;
	FCortexCommandResult ReviewError;
	TestFalse(TEXT("the reviewed retirement refuses instead of severing the retained execution link"),
		PrepareApprovedRequest(Fixture, TEXT("00000000-0000-0000-0000-000000107601"), Request, Approved, ReviewError));
	TestTrue(TEXT("the reviewed refusal is diagnostic"), !ReviewError.ErrorMessage.IsEmpty());
	// The approved set is the preview's removable set, which excludes the retained selected entry Alpha, so
	// the guard that refuses first is the approval-inclusion guard (`approved_node_guids must include
	// selected entry '<Alpha>'`, CortexGraphMigrationOps.cpp:7111) rather than the later blocked/retained
	// refusal (:7278). Naming Alpha as the selected entry its approved set could not include is therefore the
	// honest refusal reason, and it cannot be produced by a malformed request: an invalid patch_id names
	// neither a selected entry nor a node GUID.
	TestTrue(TEXT("the reviewed refusal names the selected entry its approved set could not include"),
		ReviewError.ErrorMessage.Contains(TEXT("selected entry"))
			&& ReviewError.ErrorMessage.Contains(AlphaGuidText));

	// The independent entry is then retired for real: a separately reviewed Beta-only request agrees on
	// exactly its own removable pair, applies it, and the retained producer, its execution consumer and
	// the execution link between them have to survive the applied removal.
	const FString BetaBodyGuidText = Fixture.BetaBody->NodeGuid.ToString();
	TSharedPtr<FJsonObject> IndependentRequest;
	TArray<FString> IndependentApproved;
	FCortexCommandResult IndependentError;
	TestTrue(FString::Printf(TEXT("the independent reviewed request is prepared: %s"), *IndependentError.ErrorMessage),
		PrepareRetirementRequest(Fixture, { BetaGuidText }, TEXT("00000000-0000-0000-0000-000000107602"),
			IndependentRequest, IndependentApproved, IndependentError));
	TestTrue(TEXT("the independent request approves exactly its own removable pair"),
		IndependentApproved.Contains(BetaGuidText) && IndependentApproved.Contains(BetaBodyGuidText)
			&& !IndependentApproved.Contains(ProducerGuidText) && !IndependentApproved.Contains(ConsumerGuidText));

	FCortexGraphPatchOutcome Outcome;
	TestTrue(FString::Printf(TEXT("the reviewed independent retirement applies: %s [%s]"),
		*IndependentError.ErrorMessage, *FString::Join(Outcome.Diagnostics, TEXT("; "))),
		FCortexGraphPatchOps::Execute(Fixture.Blueprint, IndependentRequest, Outcome, IndependentError));
	TestEqual(TEXT("the independent retirement readback matches"), Outcome.ReadbackStatus, FString(TEXT("matched")));
	TestNull(TEXT("the independent entry is absent after the applied retirement"),
		FCortexGraphMigrationOps::FindNodeByGuid(Fixture.Blueprint, Fixture.BetaGuid));
	TestNull(TEXT("the independent entry body is absent after the applied retirement"),
		FCortexGraphMigrationOps::FindNodeByGuid(Fixture.Blueprint, Fixture.BetaBodyGuid));
	UEdGraphNode* SurvivorProducer = FCortexGraphMigrationOps::FindNodeByGuid(Fixture.Blueprint, Fixture.RetainedProducerGuid);
	UEdGraphNode* SurvivorConsumer = FCortexGraphMigrationOps::FindNodeByGuid(Fixture.Blueprint, Fixture.RetainedProducerConsumerGuid);
	TestNotNull(TEXT("the retained producer survives the independent retirement"), SurvivorProducer);
	TestNotNull(TEXT("the retained execution consumer survives the independent retirement"), SurvivorConsumer);
	if (SurvivorProducer && SurvivorConsumer)
	{
		UEdGraphPin* ProducerThen = SurvivorProducer->FindPin(TEXT("then"));
		UEdGraphPin* ConsumerExecute = SurvivorConsumer->FindPin(TEXT("execute"));
		TestTrue(TEXT("the retained producer -> consumer execution link survives the applied retirement"),
			ProducerThen && ConsumerExecute && ProducerThen->LinkedTo.Contains(ConsumerExecute));
		UEdGraphPin* ProducerReturn = SurvivorProducer->FindPin(TEXT("ReturnValue"));
		UEdGraphPin* RetainedBodyInput = Fixture.RetainedBody->FindPin(TEXT("InString"));
		TestTrue(TEXT("the retained producer still feeds the retained cosmetic body"),
			ProducerReturn && RetainedBodyInput && ProducerReturn->LinkedTo.Contains(RetainedBodyInput));
	}

	Fixture.Cleanup();
	return true;
}

// Same partition: a data cycle between removable producers, all of it inside the scanned island. The
// fixed-point walk must terminate on the cycle and keep every member removable instead of refusing or
// classifying the cycle as unproven ownership.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexGraphMigrationRetireRemovableDataCycleTest,
	"Cortex.Graph.Authoring.Migration.Retire.BoundedRemovableDataCycle",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexGraphMigrationRetireRemovableDataCycleTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	using namespace CortexGraphMigrationRetireTest;
	FFixture Fixture;
	TestTrue(TEXT("Widget fixture is created"), Fixture.Build(TEXT("BP_RetireRemovableDataCycle")));
	if (!Fixture.Blueprint) { Fixture.Cleanup(); return false; }
	TestTrue(TEXT("the removable data cycle is created"), Fixture.AddRemovableDataCycle());
	if (!Fixture.CycleFirst || !Fixture.CycleSecond) { Fixture.Cleanup(); return false; }

	FCortexGraphMigrationRetirePlan PlanValue;
	bool bReused = false;
	FCortexCommandResult Error;
	TestTrue(FString::Printf(TEXT("retirement preview terminates on the cycle: %s"), *Error.ErrorMessage),
		Plan(Fixture, { Fixture.Alpha->NodeGuid.ToString(), Fixture.Beta->NodeGuid.ToString() },
			PlanValue, bReused, Error));
	TestTrue(TEXT("the bounded cycle traversal reports a complete partition"), PlanValue.bComplete);
	TestTrue(TEXT("the bounded cycle traversal counts the work it examined"),
		PlanValue.ScannedNodes > 0 && PlanValue.ScannedLinks > 0);
	TestTrue(TEXT("the first cycle member is removable"),
		PlanValue.RemovableGuids.Contains(Fixture.CycleFirstGuid.ToString()));
	TestTrue(TEXT("the second cycle member is removable"),
		PlanValue.RemovableGuids.Contains(Fixture.CycleSecondGuid.ToString()));
	TestTrue(TEXT("the whole cyclic island is removable"),
		PlanValue.RemovableGuids.Contains(Fixture.Alpha->NodeGuid.ToString())
			&& PlanValue.RemovableGuids.Contains(Fixture.AlphaBody->NodeGuid.ToString())
			&& PlanValue.RemovableGuids.Contains(Fixture.Producer->NodeGuid.ToString())
			&& PlanValue.RemovableGuids.Contains(Fixture.Beta->NodeGuid.ToString())
			&& PlanValue.RemovableGuids.Contains(Fixture.BetaBody->NodeGuid.ToString()));
	TestTrue(TEXT("the cycle retains and blocks nothing inside the island"),
		PlanValue.Shared.Num() == 0 && PlanValue.Blocked.Num() == 0);

	Fixture.Cleanup();
	return true;
}


IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexGraphMigrationRetireBlockedEntryTest,
	"Cortex.Graph.Authoring.Migration.Retire.BlocksSelectedEntryFeedingRetainedLogic",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexGraphMigrationRetireBlockedEntryTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	using namespace CortexGraphMigrationRetireTest;
	FFixture Fixture;
	TestTrue(TEXT("Widget fixture is created"), Fixture.Build(TEXT("BP_RetireBlocked"), false, true));
	if (!Fixture.Blueprint) { Fixture.Cleanup(); return false; }
	const FString AlphaGuid = Fixture.Alpha->NodeGuid.ToString();
	FCortexGraphMigrationRetirePlan PlanValue;

	bool bReused = false;
	FCortexCommandResult Error;
	TestTrue(FString::Printf(TEXT("partition-only preview includes blockers: %s"), *Error.ErrorMessage),
		Plan(Fixture, { AlphaGuid }, PlanValue, bReused, Error));
	TestTrue(TEXT("selected entry feeding retained logic is blocked"), PlanValue.Blocked.ContainsByPredicate(
		[&](const FCortexGraphPruneNode& Node) { return Node.NodeGuid == AlphaGuid; }));
	TestFalse(TEXT("blocked selected entry is not removable"), PlanValue.RemovableGuids.Contains(AlphaGuid));
	Error = FCortexCommandResult();
	TestFalse(TEXT("reviewed request refuses blockers"), Plan(Fixture, { AlphaGuid }, PlanValue, bReused, Error, true, { AlphaGuid }));
	TestTrue(TEXT("review refusal is diagnostic"), !Error.ErrorMessage.IsEmpty());
	Fixture.Cleanup();
	return true;
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexGraphMigrationRetireStrictShapeTest,
	"Cortex.Graph.Authoring.Migration.Retire.StrictSourceAndEntryShape",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexGraphMigrationRetireStrictShapeTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	using namespace CortexGraphMigrationRetireTest;
	FFixture Fixture;
	TestTrue(TEXT("Widget fixture is created"), Fixture.Build(TEXT("BP_RetireStrict")));
	if (!Fixture.Blueprint) { Fixture.Cleanup(); return false; }
	FCortexGraphMigrationRetirePlan PlanValue;
	bool bReused = false;
	FCortexCommandResult Error;
	TestFalse(TEXT("empty selected entry set is refused"),
		FCortexGraphMigrationOps::PlanRetirement(Fixture.Blueprint, Fixture.Migration({}), PlanValue, bReused, Error));
	Error = FCortexCommandResult();
	const FString AlphaGuid = Fixture.Alpha->NodeGuid.ToString();
	TestFalse(TEXT("duplicate selected GUID is refused"),
		Plan(Fixture, { AlphaGuid, AlphaGuid }, PlanValue, bReused, Error));

	Error = FCortexCommandResult();
	TSharedPtr<FJsonObject> WrongGraph = Fixture.Migration({ AlphaGuid });
	const TSharedPtr<FJsonObject>* Source = nullptr;
	WrongGraph->TryGetObjectField(TEXT("source"), Source);
	const TSharedPtr<FJsonObject>* GraphRef = nullptr;
	(*Source)->TryGetObjectField(TEXT("graph_ref"), GraphRef);
	(*GraphRef)->SetStringField(TEXT("graph_guid"), FGuid::NewGuid().ToString());
	TestFalse(TEXT("wrong graph identity is refused"),
		FCortexGraphMigrationOps::PlanRetirement(Fixture.Blueprint, WrongGraph, PlanValue, bReused, Error));

	auto AddUnsupportedNode = [&Fixture](UEdGraphNode* Node)
	{
		Node->CreateNewGuid();
		Node->AllocateDefaultPins();
		Fixture.Graph->AddNode(Node, true, false);
		return Node->NodeGuid.ToString();
	};
	Error = FCortexCommandResult();
	UK2Node_CustomEvent* Custom = NewObject<UK2Node_CustomEvent>(Fixture.Graph);
	Custom->CustomFunctionName = TEXT("OnLegacyAlpha");
	Custom->EventReference.SetExternalMember(TEXT("OnLegacyAlpha"), UCortexGraphRetireLegacyWidget::StaticClass());
	Custom->bOverrideFunction = true;
	const FString CustomGuid = AddUnsupportedNode(Custom);
	TestFalse(TEXT("override-shaped custom event is refused"),
		Plan(Fixture, { CustomGuid }, PlanValue, bReused, Error));

	Error = FCortexCommandResult();
	UK2Node_FunctionEntry* FunctionEntry = NewObject<UK2Node_FunctionEntry>(Fixture.Graph);
	const FString FunctionEntryGuid = AddUnsupportedNode(FunctionEntry);
	TestFalse(TEXT("function entry is refused"),
		Plan(Fixture, { FunctionEntryGuid }, PlanValue, bReused, Error));

	Error = FCortexCommandResult();
	UK2Node_FunctionResult* FunctionResult = NewObject<UK2Node_FunctionResult>(Fixture.Graph);
	const FString FunctionResultGuid = AddUnsupportedNode(FunctionResult);
	TestFalse(TEXT("function result is refused"),
		Plan(Fixture, { FunctionResultGuid }, PlanValue, bReused, Error));

	Error = FCortexCommandResult();
	UK2Node_CustomEvent* DuplicateOwner = NewObject<UK2Node_CustomEvent>(Fixture.Graph);
	DuplicateOwner->NodeGuid = Fixture.Alpha->NodeGuid;
	Fixture.Graph->AddNode(DuplicateOwner, true, false);
	TestFalse(TEXT("duplicate GUID ownership is refused"),
		Plan(Fixture, { AlphaGuid }, PlanValue, bReused, Error));

	Fixture.Cleanup();
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexGraphMigrationRetireNestedGraphTest,
	"Cortex.Graph.Authoring.Migration.Retire.RefusesNestedCompositeGraph",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexGraphMigrationRetireNestedGraphTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	using namespace CortexGraphMigrationRetireTest;
	FFixture Fixture;
	TestTrue(TEXT("Widget fixture is created"), Fixture.Build(TEXT("BP_RetireNested")));
	if (!Fixture.Blueprint) { Fixture.Cleanup(); return false; }
	FCortexGraphMigrationRetirePlan PlanValue;
	bool bReused = false;
	FCortexCommandResult Error;

	UK2Node_Composite* Composite = NewObject<UK2Node_Composite>(Fixture.Graph);
	Composite->CreateNewGuid();
	Fixture.Graph->AddNode(Composite, true, false);
	Composite->PostPlacedNewNode();
	UEdGraph* ChildGraph = Composite->BoundGraph;
	TestNotNull(TEXT("a real nested composite graph is created"), ChildGraph);
	if (!ChildGraph) { Fixture.Cleanup(); return false; }

	UK2Node_Event* ChildEntry = NewObject<UK2Node_Event>(ChildGraph);
	ChildEntry->EventReference.SetExternalMember(TEXT("OnLegacyAlpha"), UCortexGraphRetireLegacyWidget::StaticClass());
	ChildEntry->bOverrideFunction = true;
	ChildEntry->CreateNewGuid();
	ChildEntry->AllocateDefaultPins();
	ChildGraph->AddNode(ChildEntry, true, false);

	TSharedPtr<FJsonObject> Migration = Fixture.Migration({ ChildEntry->NodeGuid.ToString() });
	const TSharedPtr<FJsonObject>* Source = nullptr;
	Migration->TryGetObjectField(TEXT("source"), Source);
	const TSharedPtr<FJsonObject>* GraphRef = nullptr;
	(*Source)->TryGetObjectField(TEXT("graph_ref"), GraphRef);
	(*GraphRef)->SetStringField(TEXT("graph_guid"), ChildGraph->GraphGuid.ToString());
	TestFalse(TEXT("nested graph GUID with empty subgraph path is refused"),
		FCortexGraphMigrationOps::PlanRetirement(Fixture.Blueprint, Migration, PlanValue, bReused, Error));
	Fixture.Cleanup();
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexGraphMigrationRetireUnsupportedBodyTest,
	"Cortex.Graph.Authoring.Migration.Retire.RefusesBoundAndLatentBody",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexGraphMigrationRetireUnsupportedBodyTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	using namespace CortexGraphMigrationRetireTest;
	FFixture Fixture;
	TestTrue(TEXT("Widget fixture is created"), Fixture.Build(TEXT("BP_RetireUnsupportedBody")));
	if (!Fixture.Blueprint) { Fixture.Cleanup(); return false; }
	FCortexGraphMigrationRetirePlan PlanValue;
	bool bReused = false;
	FCortexCommandResult Error;
	Fixture.Alpha->bInternalEvent = true;
	TestFalse(TEXT("bound/internal event is refused"),
		Plan(Fixture, { Fixture.Alpha->NodeGuid.ToString() }, PlanValue, bReused, Error));
	Fixture.Alpha->bInternalEvent = false;

	UK2Node_CallFunction* Delay = Fixture.AddCall(UKismetSystemLibrary::StaticClass()->FindFunctionByName(TEXT("Delay")));
	const UEdGraphSchema* Schema = Fixture.Graph->GetSchema();
	TestTrue(TEXT("latent delay is attached to the selected body"),
		Schema->TryCreateConnection(Fixture.AlphaBody->FindPin(TEXT("then")), Delay->FindPin(TEXT("execute"))));
	TestTrue(TEXT("partition preview identifies latent body node"),
		Plan(Fixture, { Fixture.Alpha->NodeGuid.ToString() }, PlanValue, bReused, Error));
	TestTrue(TEXT("latent delay is blocked"), PlanValue.Blocked.ContainsByPredicate(
		[&](const FCortexGraphPruneNode& Node) { return Node.NodeGuid == Delay->NodeGuid.ToString(); }));
	Error = FCortexCommandResult();
	const TArray<FString> Approved = PlanValue.RemovableGuids;
	TestFalse(TEXT("reviewed retirement refuses the latent body blocker"),
		Plan(Fixture, { Fixture.Alpha->NodeGuid.ToString() }, PlanValue, bReused, Error, true, Approved));
	Fixture.Cleanup();
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexGraphMigrationRetirePatchEligibilityTest,
	"Cortex.Graph.Authoring.Migration.Retire.PatchEligibilityAndStrictPreflight",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexGraphMigrationRetirePatchEligibilityTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	using namespace CortexGraphMigrationRetireTest;
	FFixture Fixture;
	TestTrue(TEXT("Widget fixture is created"), Fixture.Build(TEXT("BP_RetirePatchEligibility")));
	if (!Fixture.Blueprint) { Fixture.Cleanup(); return false; }

	Fixture.Blueprint->Status = BS_Error;
	FCortexCommandResult Error;
	const TSharedPtr<FJsonObject> RetirementMigration = Fixture.Migration({ Fixture.Alpha->NodeGuid.ToString() });
	const TSharedPtr<FJsonObject> RetireRequest = MakeShared<FJsonObject>();
	RetireRequest->SetObjectField(TEXT("migration"), RetirementMigration);
	TestTrue(TEXT("retire_entries is eligible for a compiler-error Blueprint"),
		FCortexGraphPatchOps::ValidateEligibility(Fixture.Blueprint, RetireRequest, Error));
	Error = FCortexCommandResult();
	const TSharedPtr<FJsonObject> PruneRequest = MakeShared<FJsonObject>();
	TSharedPtr<FJsonObject> PruneMigration = MakeShared<FJsonObject>();
	PruneMigration->SetStringField(TEXT("op"), TEXT("prune_island"));
	PruneRequest->SetObjectField(TEXT("migration"), PruneMigration);
	TestFalse(TEXT("prune_island remains ineligible for a compiler-error Blueprint"),
		FCortexGraphPatchOps::ValidateEligibility(Fixture.Blueprint, PruneRequest, Error));
	Error = FCortexCommandResult();
	const TSharedPtr<FJsonObject> AuthoringRequest = MakeShared<FJsonObject>();
	TestFalse(TEXT("ordinary authoring remains ineligible for a compiler-error Blueprint"),
		FCortexGraphPatchOps::ValidateEligibility(Fixture.Blueprint, AuthoringRequest, Error));

	TSharedPtr<FJsonObject> Malformed = MakeShared<FJsonObject>();
	Malformed->SetStringField(TEXT("asset_path"), Fixture.Blueprint->GetPathName());
	Malformed->SetStringField(TEXT("patch_id"), FGuid::NewGuid().ToString());
	Malformed->SetObjectField(TEXT("expected_fingerprint"),
		FCortexGraphPatchState::ComputeFingerprint(Fixture.Blueprint));
	Malformed->SetObjectField(TEXT("migration"), RetirementMigration);
	TArray<TSharedPtr<FJsonValue>> Nodes;
	Nodes.Add(MakeShared<FJsonValueObject>(MakeShared<FJsonObject>()));
	Malformed->SetArrayField(TEXT("nodes"), Nodes);
	FCortexGraphPreparedPatch Prepared;
	Error = FCortexCommandResult();
	const FString BeforeHash = FCortexGraphPatchState::ComputeFingerprint(Fixture.Blueprint)
		->GetStringField(TEXT("graph_authoring_hash"));
	const int32 BeforeNodeCount = Fixture.Graph->Nodes.Num();
	TestFalse(TEXT("retirement with mixed authoring nodes is refused"),
		FCortexGraphPatchOps::Preflight(Fixture.Blueprint, Malformed, Prepared, Error));
	TestEqual(TEXT("mixed authoring fields report INVALID_FIELD"), Error.ErrorCode, FString(TEXT("INVALID_FIELD")));
	TestEqual(TEXT("malformed retirement does not change the graph hash"),
		FCortexGraphPatchState::ComputeFingerprint(Fixture.Blueprint)->GetStringField(TEXT("graph_authoring_hash")),
		BeforeHash);
	TestEqual(TEXT("malformed retirement does not change the graph node count"),
		Fixture.Graph->Nodes.Num(), BeforeNodeCount);
	Fixture.Cleanup();
	return true;
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexGraphMigrationRetireFailureAtomicityTest,
	"Cortex.Graph.Authoring.Migration.Retire.FailureAtomicity",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexGraphMigrationRetireFailureAtomicityTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	using namespace CortexGraphMigrationRetireTest;
	struct FFaultCase
	{
		const TCHAR* Name;
		bool bReadback;
	};
	const FFaultCase Faults[] = {
		{ TEXT("migration_retire_after_first_removal"), false },
		{ TEXT("migration_retire_after_removals"), false },
		{ TEXT("retire_after_removal"), true },
		{ TEXT("retire_after_preservation"), true }
	};
	bool bAllPassed = true;
	for (int32 Index = 0; Index < UE_ARRAY_COUNT(Faults); ++Index)
	{
		FFixture Fixture;
		const FString AssetName = FString::Printf(TEXT("BP_RetireAtomic_%d"), Index);
		bAllPassed &= TestTrue(TEXT("retirement fault fixture is created"), Fixture.Build(*AssetName, true));
		if (!Fixture.Blueprint) { Fixture.Cleanup(); continue; }
		const FString GraphBefore = CaptureNativeGraph(Fixture.Graph);
		const bool bDirtyBefore = Fixture.Package->IsDirty();
		TSharedPtr<FJsonObject> Request;
		TArray<FString> Approved;
		FCortexCommandResult Error;
		const FString PatchId = FString::Printf(TEXT("00000000-0000-0000-0000-00000010731%d"), Index);
		if (!PrepareApprovedRequest(Fixture, *PatchId, Request, Approved, Error))
		{
			bAllPassed &= TestFalse(FString::Printf(TEXT("%s approved preflight failed: %s"), Faults[Index].Name,
				*Error.ErrorMessage), true);
			Fixture.Cleanup();
			continue;
		}
		if (Faults[Index].bReadback)
		{
			FCortexGraphMigrationOps::SetRetirementReadbackFaultForTesting(FName(Faults[Index].Name));
		}
		else
		{
			FCortexGraphPatchOps::SetApplyFaultPointForTesting(FName(Faults[Index].Name));
		}
		FCortexGraphPatchOutcome Outcome;
		Error = FCortexCommandResult();
		bAllPassed &= TestFalse(FString::Printf(TEXT("%s causes Execute to fail"), Faults[Index].Name),
			FCortexGraphPatchOps::Execute(Fixture.Blueprint, Request, Outcome, Error));
		FCortexGraphPatchOps::SetApplyFaultPointForTesting(NAME_None);
		FCortexGraphMigrationOps::ClearRetirementReadbackFaultForTesting();
		bAllPassed &= TestEqual(FString::Printf(TEXT("%s restores the transaction"), Faults[Index].Name),
			Outcome.RollbackStatus, FString(TEXT("restored")));
		bAllPassed &= TestFalse(FString::Printf(TEXT("%s does not block the asset"), Faults[Index].Name),
			Outcome.bBlocked);
		if (Faults[Index].bReadback)
		{
			bAllPassed &= TestEqual(FString::Printf(TEXT("%s reports failed readback"), Faults[Index].Name),
				Outcome.ReadbackStatus, FString(TEXT("mismatched")));
		}
		bAllPassed &= TestEqual(FString::Printf(TEXT("%s restores the independent native graph snapshot"), Faults[Index].Name),
			CaptureNativeGraph(Fixture.Graph), GraphBefore);
		bAllPassed &= TestEqual(FString::Printf(TEXT("%s restores the dirty baseline"), Faults[Index].Name),
			Fixture.Package->IsDirty(), bDirtyBefore);
		for (const FString& GuidText : Approved)
		{
			FGuid Guid;
			FGuid::Parse(GuidText, Guid);
			bAllPassed &= TestNotNull(FString::Printf(TEXT("%s restores approved GUID %s"), Faults[Index].Name, *GuidText),
				FCortexGraphMigrationOps::FindNodeByGuid(Fixture.Blueprint, Guid));
		}
		Fixture.Cleanup();
	}
	FCortexGraphPatchOps::SetApplyFaultPointForTesting(NAME_None);
	FCortexGraphMigrationOps::ClearRetirementReadbackFaultForTesting();
	return bAllPassed;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexGraphMigrationRetireApplyReadbackTest,
	"Cortex.Graph.Authoring.Migration.Retire.ApplyReadbackWithoutCompile",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexGraphMigrationRetireApplyReadbackTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	using namespace CortexGraphMigrationRetireTest;
	FFixture Fixture;
	TestTrue(TEXT("retirement fixture is created"), Fixture.Build(TEXT("BP_RetireApplyReadback"), true));
	if (!Fixture.Blueprint) { Fixture.Cleanup(); return false; }

	TSharedPtr<FJsonObject> Request = MakeShared<FJsonObject>();
	Request->SetStringField(TEXT("asset_path"), Fixture.Blueprint->GetPathName());
	Request->SetStringField(TEXT("patch_id"), TEXT("00000000-0000-0000-0000-000000107301"));
	Request->SetObjectField(TEXT("expected_fingerprint"), FCortexGraphPatchState::ComputeFingerprint(Fixture.Blueprint));
	Request->SetArrayField(TEXT("nodes"), {});
	Request->SetArrayField(TEXT("connections"), {});
	Request->SetArrayField(TEXT("pin_updates"), {});
	Request->SetBoolField(TEXT("dry_run"), true);
	Request->SetBoolField(TEXT("compile"), false);
	Request->SetBoolField(TEXT("save"), false);
	Request->SetBoolField(TEXT("allow_noop"), false);
	const FString AlphaGuid = Fixture.Alpha->NodeGuid.ToString();
	const FString BetaGuid = Fixture.Beta->NodeGuid.ToString();
	TSharedPtr<FJsonObject> Migration = Fixture.Migration({ AlphaGuid, BetaGuid });
	Request->SetObjectField(TEXT("migration"), Migration);

	FCortexGraphPreparedPatch Preview;
	FCortexCommandResult Error;
	TestTrue(FString::Printf(TEXT("retirement preview succeeds: %s"), *Error.ErrorMessage),
		FCortexGraphPatchOps::Preflight(Fixture.Blueprint, Request, Preview, Error));
	TArray<FString> Approved;
	const TSharedPtr<FJsonObject> PreviewInventory =
		FCortexGraphMigrationOps::MakeRetirementInventory(Preview.RetirementPlan);
	if (PreviewInventory.IsValid())
	{
		const TArray<TSharedPtr<FJsonValue>>* ApprovedValues = nullptr;
		PreviewInventory->TryGetArrayField(TEXT("removable"), ApprovedValues);
		if (ApprovedValues)
		{
			for (const TSharedPtr<FJsonValue>& Value : *ApprovedValues) Approved.Add(Value->AsString());
		}
	}
	TestTrue(TEXT("the reviewed approval includes both selected entries"), Approved.Contains(AlphaGuid) && Approved.Contains(BetaGuid));
	TArray<TSharedPtr<FJsonValue>> ApprovedValues;
	for (const FString& Guid : Approved) ApprovedValues.Add(MakeShared<FJsonValueString>(Guid));
	Migration->SetArrayField(TEXT("approved_node_guids"), ApprovedValues);
	FCortexGraphPreparedPatch Reviewed;
	Error = FCortexCommandResult();
	TestTrue(FString::Printf(TEXT("approved preview succeeds: %s"), *Error.ErrorMessage),
		FCortexGraphPatchOps::Preflight(Fixture.Blueprint, Request, Reviewed, Error));
	TSet<FGuid> RetainedGuids = { Fixture.Retained->NodeGuid, Fixture.Producer->NodeGuid, Fixture.RetainedBody->NodeGuid };
	const FString RetainedBefore = CaptureNativeGraph(Fixture.Graph, &RetainedGuids);
	Request->SetBoolField(TEXT("dry_run"), false);
	Request->SetStringField(TEXT("expected_validation_hash"), Reviewed.ValidationHash);

	FCortexGraphPatchOutcome Outcome;
	Error = FCortexCommandResult();
	TestTrue(FString::Printf(TEXT("approved retirement applies: %s"), *Error.ErrorMessage),
		FCortexGraphPatchOps::Execute(Fixture.Blueprint, Request, Outcome, Error));
	TestEqual(TEXT("approved retirement reports applied"), Outcome.ApplyStatus, FString(TEXT("applied")));
	for (const FString& GuidText : Approved)
	{
		FGuid Guid;
		FGuid::Parse(GuidText, Guid);
		TestNull(FString::Printf(TEXT("approved node %s is absent from the graph and asset"), *GuidText),
			FCortexGraphMigrationOps::FindNodeByGuid(Fixture.Blueprint, Guid));
	}
	TestNotNull(TEXT("retained event remains"), FCortexGraphMigrationOps::FindNodeByGuid(Fixture.Blueprint, Fixture.Retained->NodeGuid));
	TestNotNull(TEXT("shared producer remains"), FCortexGraphMigrationOps::FindNodeByGuid(Fixture.Blueprint, Fixture.Producer->NodeGuid));
	TestNotNull(TEXT("retained body remains"), FCortexGraphMigrationOps::FindNodeByGuid(Fixture.Blueprint, Fixture.RetainedBody->NodeGuid));
	TestEqual(TEXT("retained event, body and shared producer are unchanged"),
		CaptureNativeGraph(Fixture.Graph, &RetainedGuids), RetainedBefore);
	TestTrue(TEXT("retained body execute link survives"),
		Fixture.Retained->FindPin(TEXT("then"))->LinkedTo.Contains(Fixture.RetainedBody->FindPin(TEXT("execute"))));
	TestTrue(TEXT("shared producer remains linked to retained body"),
		Fixture.Producer->FindPin(TEXT("ReturnValue"))->LinkedTo.Contains(Fixture.RetainedBody->FindPin(TEXT("InString"))));
	TestEqual(TEXT("compile is not requested"), Outcome.CompileStatus, FString(TEXT("not_requested")));
	TestEqual(TEXT("native retirement readback matches"), Outcome.ReadbackStatus, FString(TEXT("matched")));
	TestEqual(TEXT("retirement requests no target compile"), Outcome.TargetCompileCount, 0);
	TestEqual(TEXT("rollback is not requested"), Outcome.RollbackStatus, FString(TEXT("not_requested")));
	TestTrue(TEXT("the package is dirty after retirement"), Fixture.Package->IsDirty());
	TestNotNull(TEXT("retirement inventory is returned"), Outcome.RetirementInventory.Get());
	const TArray<TSharedPtr<FJsonValue>>* InventoryApproved = nullptr;
	TestTrue(TEXT("inventory reports the exact applied approval"),
		Outcome.RetirementInventory.IsValid()
			&& Outcome.RetirementInventory->TryGetArrayField(TEXT("approved_guids"), InventoryApproved)
			&& InventoryApproved && InventoryApproved->Num() == Approved.Num());
	Fixture.Cleanup();
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexGraphMigrationRetireCompileRecoveryTest,
	"Cortex.Graph.Authoring.Migration.Retire.CompileRecoveryRejectsGeneratedDrift",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexGraphMigrationRetireCompileRecoveryTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	using namespace CortexGraphMigrationRetireTest;
	FFixture Fixture;
	TestTrue(TEXT("collision Widget fixture is created under its legacy parent"),
		Fixture.Build(TEXT("BP_RetireCompileRecovery"), false, false, nullptr, false, true));
	if (!Fixture.Blueprint) { Fixture.Cleanup(); return false; }
	const FString Filename = Fixture.Filename();
	TestTrue(TEXT("legacy-parent baseline is saved"), Fixture.SaveToDisk());
	Fixture.Blueprint->ParentClass = UCortexGraphRetireCollisionTargetWidget::StaticClass();
	FBlueprintEditorUtils::RefreshAllNodes(Fixture.Blueprint);
	Fixture.AddNativeNameCollision();
	AddExpectedError(TEXT("name conflicts with a native"), EAutomationExpectedErrorFlags::Contains, 1);
	FKismetEditorUtilities::CompileBlueprint(Fixture.Blueprint);
	TestEqual(TEXT("unrelated native collision establishes the original error status"),
		static_cast<int32>(Fixture.Blueprint->Status), static_cast<int32>(BS_Error));
	TestTrue(TEXT("stale graph entries are created after failed compilation"), Fixture.PopulateGraph(true));
	Fixture.Retained->EventReference.SetExternalMember(TEXT("OnRetainedEvent"), UCortexGraphRetireTargetWidget::StaticClass());
	const FString GraphBefore = CaptureNativeGraph(Fixture.Graph);
	const FString GeneratedBefore = FCortexGraphPatchState::ComputeGeneratedStateDigest(Fixture.Blueprint);
	TArray<FString> Approved;
	TSharedPtr<FJsonObject> Request;
	FCortexCommandResult Error;
	TestTrue(FString::Printf(TEXT("approved compile request is prepared: %s"), *Error.ErrorMessage),
		PrepareApprovedRequest(Fixture, TEXT("00000000-0000-0000-0000-000000107401"),
			Request, Approved, Error, true));
	FOperations Operations;
	Operations.Begin();
	AddExpectedError(TEXT("name conflicts with a native"), EAutomationExpectedErrorFlags::Contains, 2);
	AddExpectedError(TEXT("Pasted node"), EAutomationExpectedErrorFlags::Contains, 2);
	FCortexGraphPatchOutcome Outcome;
	Error = FCortexCommandResult();
	TestFalse(TEXT("retirement fails while unrelated compiler collision persists"),
		FCortexGraphPatchOps::Execute(Fixture.Blueprint, Request, Outcome, Error));
	TestEqual(TEXT("generated state drift keeps recovery blocked"), Error.ErrorCode, FString(TEXT("INVALID_OPERATION")));
	TestEqual(TEXT("rollback refuses a changed generated class"), Outcome.RollbackStatus, FString(TEXT("unverified")));
	TestTrue(TEXT("unverified generated recovery blocks the asset"), Outcome.bBlocked);
	TestEqual(TEXT("the recovery compile retains BS_Error"),
		static_cast<int32>(Fixture.Blueprint->Status), static_cast<int32>(BS_Error));
	TestNotEqual(TEXT("generated drift is not reported as exact restoration"),
		FCortexGraphPatchState::ComputeGeneratedStateDigest(Fixture.Blueprint), GeneratedBefore);
	TestNotEqual(TEXT("engine mutation during failed recovery is not represented as restored authoring"),
		CaptureNativeGraph(Fixture.Graph), GraphBefore);
	for (const FString& GuidText : Approved)
	{
		FGuid Guid;
		FGuid::Parse(GuidText, Guid);
		TestNotNull(TEXT("every approved GUID is restored"), FCortexGraphMigrationOps::FindNodeByGuid(Fixture.Blueprint, Guid));
	}
	TestTrue(TEXT("blocked failed recovery remains dirty"), Fixture.Package->IsDirty());
	TestEqual(TEXT("failed apply does not save"), Operations.Saves, 0);
	Operations.End();
	Fixture.Cleanup();
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexGraphMigrationRetireCompileRecoveryExactMatchTest,
	"Cortex.Graph.Authoring.Migration.Retire.CompileRecoveryAcceptsExactErrorState",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexGraphMigrationRetireCompileRecoveryExactMatchTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	using namespace CortexGraphMigrationRetireTest;
	FFixture Fixture;
	TestTrue(TEXT("legacy Widget fixture includes the selected override entries"),
		Fixture.Build(TEXT("BP_RetireCompileRecoveryExact"), true, false, nullptr, false, false));
	if (!Fixture.Blueprint) { Fixture.Cleanup(); return false; }
	const FString Filename = Fixture.Filename();
	Fixture.AddNativeNameCollision();
	AddExpectedError(TEXT("name conflicts with a native"), EAutomationExpectedErrorFlags::Contains, 1);
	TestTrue(TEXT("legacy-parent entries and the unrelated compile error are saved"), Fixture.SaveToDisk());
	AddExpectedError(TEXT("name conflicts with a native"), EAutomationExpectedErrorFlags::Contains, 1);
	FKismetEditorUtilities::CompileBlueprint(Fixture.Blueprint);
	TestEqual(TEXT("saved legacy-parent fixture has the real unrelated compile error"),
		static_cast<int32>(Fixture.Blueprint->Status), static_cast<int32>(BS_Error));
	TestTrue(TEXT("legacy override nodes survive their valid-parent compile"), Fixture.RefreshPointers());

	Fixture.Blueprint->ParentClass = UCortexGraphRetireCollisionTargetWidget::StaticClass();
	FBlueprintEditorUtils::RefreshAllNodes(Fixture.Blueprint);
	TestTrue(TEXT("event nodes remain override nodes after reparent refresh"),
		Fixture.RefreshPointers() && Fixture.Alpha->bOverrideFunction && !Fixture.Alpha->IsA<UK2Node_CustomEvent>()
			&& Fixture.Beta->bOverrideFunction && !Fixture.Beta->IsA<UK2Node_CustomEvent>());
	FBPVariableDescription& CollisionVariable = Fixture.Blueprint->NewVariables.AddDefaulted_GetRef();
	CollisionVariable.VarName = FName(TEXT("NativeCollision"));
	CollisionVariable.VarType.PinCategory = UEdGraphSchema_K2::PC_Int;
	Fixture.Package->MarkPackageDirty();
	Fixture.Blueprint->Status = BS_Error;
	Fixture.Retained->EventReference.SetExternalMember(TEXT("OnRetainedEvent"), UCortexGraphRetireTargetWidget::StaticClass());

	const FString GraphBefore = CaptureNativeGraph(Fixture.Graph);
	const FString GeneratedBefore = FCortexGraphPatchState::ComputeGeneratedStateDigest(Fixture.Blueprint);
	const bool bDirtyBefore = Fixture.Package->IsDirty();
	TestEqual(TEXT("the pre-request generated digest is captured exactly"),
		FCortexGraphPatchState::ComputeGeneratedStateDigest(Fixture.Blueprint), GeneratedBefore);
	TArray<FString> Approved;
	TSharedPtr<FJsonObject> Request;
	FCortexCommandResult Error;
	TestTrue(FString::Printf(TEXT("approved compile request is prepared: %s"), *Error.ErrorMessage),
		PrepareApprovedRequest(Fixture, TEXT("00000000-0000-0000-0000-000000107405"),
			Request, Approved, Error, true));

	FOperations Operations;
	Operations.Begin();
	FCortexGraphPatchOps::SetApplyFaultPointForTesting(TEXT("retirement_compile_result_failure"));
	FCortexGraphPatchOutcome Outcome;
	TestFalse(TEXT("injected compile-result failure enters recovery"),
		FCortexGraphPatchOps::Execute(Fixture.Blueprint, Request, Outcome, Error));
	FCortexGraphPatchOps::ClearApplyFaultPointForTesting();

	TestEqual(TEXT("target compile fails once"), Outcome.TargetCompileCount, 1);
	TestEqual(TEXT("coordinator observes one target compile"), Operations.TargetCompiles, 1);
	TestEqual(TEXT("failed recovery compile runs once"), Outcome.RecoveryCompileCount, 1);
	TestEqual(TEXT("coordinator observes one recovery compile"), Operations.RecoveryCompiles, 1);
	TestEqual(TEXT("compile failure remains the operation result"), Error.ErrorCode, FString(TEXT("COMPILE_FAILED")));
	TestEqual(TEXT("exact error-state restoration verifies"), Outcome.RollbackStatus, FString(TEXT("restored")));
	TestFalse(TEXT("exact error-state restoration does not block the asset"), Outcome.bBlocked);
	TestEqual(TEXT("BS_Error is restored exactly"),
		static_cast<int32>(Fixture.Blueprint->Status), static_cast<int32>(BS_Error));
	TestEqual(TEXT("generated state digest is identical"),
		FCortexGraphPatchState::ComputeGeneratedStateDigest(Fixture.Blueprint), GeneratedBefore);
	TestEqual(TEXT("authoring graph is restored exactly"), CaptureNativeGraph(Fixture.Graph), GraphBefore);
	TestEqual(TEXT("dirty baseline is restored"), Fixture.Package->IsDirty(), bDirtyBefore);
	TestEqual(TEXT("failed recovery does not save"), Operations.Saves, 0);
	Operations.End();
	Fixture.Cleanup();
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexGraphMigrationRetireCompileOnceTest,
	"Cortex.Graph.Authoring.Migration.Retire.CompileOnceAfterReparent",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexGraphMigrationRetireCompileOnceTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	using namespace CortexGraphMigrationRetireTest;
	FFixture Fixture;
	TestTrue(TEXT("Widget fixture is created under its legacy parent"),
		Fixture.Build(TEXT("BP_RetireCompileOnce"), false, false, nullptr, false, true));
	if (!Fixture.Blueprint) { Fixture.Cleanup(); return false; }
	const FString Filename = Fixture.Filename();
	TestTrue(TEXT("legacy-parent Widget is saved before reparenting"), Fixture.SaveToDisk());
	Fixture.Blueprint->ParentClass = UCortexGraphRetireTargetWidget::StaticClass();
	FBlueprintEditorUtils::RefreshAllNodes(Fixture.Blueprint);
	UK2Node_CustomEvent* NativeNameCollision = Fixture.AddNativeNameCollision();
	AddExpectedError(TEXT("name conflicts with a native"), EAutomationExpectedErrorFlags::Contains, 1);
	FKismetEditorUtilities::CompileBlueprint(Fixture.Blueprint);
	TestEqual(TEXT("baseline compiler collision establishes BS_Error status"),
		static_cast<int32>(Fixture.Blueprint->Status), static_cast<int32>(BS_Error));
	Fixture.Graph->RemoveNode(NativeNameCollision);
	TestTrue(TEXT("stale graph entries are created after baseline compilation"), Fixture.PopulateGraph(true));
	Fixture.Retained->EventReference.SetExternalMember(TEXT("OnRetainedEvent"), UCortexGraphRetireTargetWidget::StaticClass());
	TArray<FString> Approved;
	TSharedPtr<FJsonObject> Request;
	FCortexCommandResult Error;
	TestTrue(FString::Printf(TEXT("approved compile-once request is prepared: %s"), *Error.ErrorMessage),
		PrepareApprovedRequest(Fixture, TEXT("00000000-0000-0000-0000-000000107402"),
			Request, Approved, Error, true));
	FOperations Operations;
	Operations.Begin();
	FCortexGraphPatchOutcome Outcome;
	TestTrue(FString::Printf(TEXT("retirement compiles and verifies: %s"), *Error.ErrorMessage),
		FCortexGraphPatchOps::Execute(Fixture.Blueprint, Request, Outcome, Error));
	TestEqual(TEXT("successful retirement target compile count is one"), Outcome.TargetCompileCount, 1);
	TestEqual(TEXT("one real target compile is observed"), Operations.TargetCompiles, 1);
	TestEqual(TEXT("successful retirement does not recovery compile"), Operations.RecoveryCompiles, 0);
	TestEqual(TEXT("compile status is compiled"), Outcome.CompileStatus, FString(TEXT("compiled")));
	TestEqual(TEXT("readback status is matched"), Outcome.ReadbackStatus, FString(TEXT("matched")));
	TestEqual(TEXT("successful retirement does not save"), Operations.Saves, 0);
	for (const FString& GuidText : Approved)
	{
		FGuid Guid;
		FGuid::Parse(GuidText, Guid);
		TestNull(TEXT("retired entry/body/producer is absent"), FCortexGraphMigrationOps::FindNodeByGuid(Fixture.Blueprint, Guid));
	}
	TestNotNull(TEXT("retained override remains"), FCortexGraphMigrationOps::FindNodeByGuid(Fixture.Blueprint, Fixture.Retained->NodeGuid));
	TestNotNull(TEXT("retained body remains"), FCortexGraphMigrationOps::FindNodeByGuid(Fixture.Blueprint, Fixture.RetainedBody->NodeGuid));
	Operations.End();
	Fixture.Cleanup();
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexGraphMigrationRetireStagedBytesTest,
	"Cortex.Graph.Authoring.Migration.Retire.StagedApplyPreservesDiskBytes",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexGraphMigrationRetireStagedBytesTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	using namespace CortexGraphMigrationRetireTest;
	FFixture Fixture;
	TestTrue(TEXT("Widget fixture is created"), Fixture.Build(TEXT("BP_RetireStagedBytes"), false, false, nullptr, false, true));
	if (!Fixture.Blueprint) { Fixture.Cleanup(); return false; }
	const FString Filename = Fixture.Filename();
	TestTrue(TEXT("clean baseline package is saved"), Fixture.SaveToDisk());
	const TArray<uint8> DiskBefore = ReadBytes(Filename);
	Fixture.AddNativeNameCollision();
	AddExpectedError(TEXT("name conflicts with a native"), EAutomationExpectedErrorFlags::Contains, 1);
	FKismetEditorUtilities::CompileBlueprint(Fixture.Blueprint);
	TestEqual(TEXT("in-memory package starts invalid"), static_cast<int32>(Fixture.Blueprint->Status), static_cast<int32>(BS_Error));
	Fixture.Blueprint->ParentClass = UCortexGraphRetireCollisionTargetWidget::StaticClass();
	FBlueprintEditorUtils::RefreshAllNodes(Fixture.Blueprint);
	FBPVariableDescription& CollisionVariable = Fixture.Blueprint->NewVariables.AddDefaulted_GetRef();
	CollisionVariable.VarName = FName(TEXT("NativeCollision"));
	CollisionVariable.VarType.PinCategory = UEdGraphSchema_K2::PC_Int;
	Fixture.Package->MarkPackageDirty();
	TestTrue(TEXT("stale graph entries are created after failed compilation"), Fixture.PopulateGraph(false));
	Fixture.Blueprint->Status = BS_Error;
	Fixture.Retained->EventReference.SetExternalMember(TEXT("OnRetainedEvent"), UCortexGraphRetireTargetWidget::StaticClass());
	TArray<FString> Approved;
	TSharedPtr<FJsonObject> Request;
	FCortexCommandResult Error;
	TestTrue(FString::Printf(TEXT("staged request is prepared: %s"), *Error.ErrorMessage),
		PrepareApprovedRequest(Fixture, TEXT("00000000-0000-0000-0000-000000107403"),
			Request, Approved, Error, false, false));
	FOperations Operations;
	Operations.Begin();
	FCortexGraphPatchOutcome Outcome;
	TestTrue(FString::Printf(TEXT("staged retirement applies and reads back: %s"), *Error.ErrorMessage),
		FCortexGraphPatchOps::Execute(Fixture.Blueprint, Request, Outcome, Error));
	TestEqual(TEXT("staged compile status is not_requested"), Outcome.CompileStatus, FString(TEXT("not_requested")));
	TestEqual(TEXT("staged readback matches"), Outcome.ReadbackStatus, FString(TEXT("matched")));
	TestFalse(TEXT("staged operation does not claim save"), Outcome.bSaved);
	TestEqual(TEXT("staged operation does not compile"), Operations.TargetCompiles, 0);
	TestEqual(TEXT("staged operation does not save"), Operations.Saves, 0);
	TestTrue(TEXT("staged operation leaves package dirty"), Fixture.Package->IsDirty());
	TestEqual(TEXT("staged mutation leaves the compiler status dirty"),
		static_cast<int32>(Fixture.Blueprint->Status), static_cast<int32>(BS_Dirty));
	TestTrue(TEXT("cached compiler information does not claim a successful compile"),
		Outcome.CompileStatus != TEXT("compiled"));
	TestTrue(TEXT("on-disk bytes are unchanged"), SameBytes(DiskBefore, ReadBytes(Filename)));
	Operations.End();
	Fixture.Cleanup();
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexGraphMigrationRetireSaveCleanStartTest,
	"Cortex.Graph.Authoring.Migration.Retire.SaveAfterVerifiedCleanStart",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexGraphMigrationRetireSaveCleanStartTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	using namespace CortexGraphMigrationRetireTest;
	FFixture Fixture;
	TestTrue(TEXT("Widget fixture is created"), Fixture.Build(TEXT("BP_RetireSaveClean"), false, false, nullptr, false, true));
	if (!Fixture.Blueprint) { Fixture.Cleanup(); return false; }
	const FString Filename = Fixture.Filename();
	TestTrue(TEXT("legacy-parent baseline saves"), Fixture.SaveToDisk());
	Fixture.Blueprint->ParentClass = UCortexGraphRetireTargetWidget::StaticClass();
	FBlueprintEditorUtils::RefreshAllNodes(Fixture.Blueprint);
	UK2Node_CustomEvent* NativeNameCollision = Fixture.AddNativeNameCollision();
	AddExpectedError(TEXT("name conflicts with a native"), EAutomationExpectedErrorFlags::Contains, 1);
	FKismetEditorUtilities::CompileBlueprint(Fixture.Blueprint);
	TestEqual(TEXT("invalid reparented package is BS_Error before recovery"),
		static_cast<int32>(Fixture.Blueprint->Status), static_cast<int32>(BS_Error));
	Fixture.Graph->RemoveNode(NativeNameCollision);
	TestTrue(TEXT("stale graph entries are created after baseline compilation"), Fixture.PopulateGraph(true));
	Fixture.Retained->EventReference.SetExternalMember(TEXT("OnRetainedEvent"), UCortexGraphRetireTargetWidget::StaticClass());
	TestTrue(TEXT("invalid reparented state is persisted as a clean starting package"), Fixture.SaveToDisk());
	TestFalse(TEXT("package begins clean"), Fixture.Package->IsDirty());
	TArray<FString> Approved;
	TSharedPtr<FJsonObject> Request;
	FCortexCommandResult Error;
	TestTrue(FString::Printf(TEXT("save request is prepared: %s"), *Error.ErrorMessage),
		PrepareApprovedRequest(Fixture, TEXT("00000000-0000-0000-0000-000000107404"),
			Request, Approved, Error, true, true));
	FOperations Operations;
	Operations.Begin();
	FCortexGraphPatchOutcome Outcome;
	TestTrue(FString::Printf(TEXT("verified retirement saves: %s"), *Error.ErrorMessage),
		FCortexGraphPatchOps::Execute(Fixture.Blueprint, Request, Outcome, Error));
	TestEqual(TEXT("post-save status is saved"), Outcome.SaveStatus, FString(TEXT("saved")));
	TestEqual(TEXT("post-save verification is verified"), Outcome.PostSaveStatus, FString(TEXT("verified")));
	TestTrue(TEXT("outcome reports saved"), Outcome.bSaved);
	TestEqual(TEXT("one target compile precedes save"), Operations.TargetCompiles, 1);
	TestEqual(TEXT("one explicit package save occurs"), Operations.Saves, 1);
	TestFalse(TEXT("verified saved package is clean"), Fixture.Package->IsDirty());
	const FString PackageName = Fixture.Package->GetName();
	const FString ObjectName = Fixture.Blueprint->GetName();
	const FGuid GraphGuid = Fixture.Graph->GraphGuid;
	const FGuid RetainedGuid = Fixture.Retained->NodeGuid;
	const FGuid RetainedBodyGuid = Fixture.RetainedBody->NodeGuid;
	const FGuid ProducerGuid = Fixture.Producer->NodeGuid;
	const TArray<FString> RetiredGuidTexts = Approved;
	UPackage* const PackageBeforeReload = Fixture.Package;
	UBlueprint* const BlueprintBeforeReload = Fixture.Blueprint;
	Operations.End();

	TArray<UPackage*> PackagesToReload;
	PackagesToReload.Add(PackageBeforeReload);
	FText ReloadError;
	const bool bReloaded = UPackageTools::ReloadPackages(
		PackagesToReload, ReloadError, EReloadPackagesInteractionMode::AssumeNegative);
	TestTrue(FString::Printf(TEXT("saved retirement package reloads: %s"), *ReloadError.ToString()), bReloaded);
	UPackage* ReloadedPackage = FindPackage(nullptr, *PackageName);
	UWidgetBlueprint* Reloaded = ReloadedPackage
		? FindObject<UWidgetBlueprint>(ReloadedPackage, *ObjectName) : nullptr;
	TestNotNull(TEXT("saved Widget Blueprint resolves after reload"), Reloaded);
	if (Reloaded)
	{
		TestTrue(TEXT("reload replaced the in-memory Blueprint instance"), Reloaded != BlueprintBeforeReload);
		TArray<UEdGraph*> ReloadedGraphs;
		Reloaded->GetAllGraphs(ReloadedGraphs);
		UEdGraph* ReloadedGraph = nullptr;
		for (UEdGraph* Candidate : ReloadedGraphs)
		{
			if (Candidate && Candidate->GraphGuid == GraphGuid)
			{
				ReloadedGraph = Candidate;
				break;
			}
		}
		TestNotNull(TEXT("retained event graph resolves from disk"), ReloadedGraph);
		auto FindNodeInGraph = [](UEdGraph* Graph, const FGuid& Guid) -> UEdGraphNode*
		{
			if (!Graph) return nullptr;
			for (UEdGraphNode* Node : Graph->Nodes) if (Node && Node->NodeGuid == Guid) return Node;
			return nullptr;
		};
		UEdGraphNode* ReloadedRetained = FindNodeInGraph(ReloadedGraph, RetainedGuid);
		UEdGraphNode* ReloadedRetainedBody = FindNodeInGraph(ReloadedGraph, RetainedBodyGuid);
		UEdGraphNode* ReloadedProducer = FindNodeInGraph(ReloadedGraph, ProducerGuid);
		TestNotNull(TEXT("retained event survives in the reloaded graph"), ReloadedRetained);
		TestNotNull(TEXT("retained body survives in the reloaded graph"), ReloadedRetainedBody);
		TestNotNull(TEXT("shared producer survives in the reloaded graph"), ReloadedProducer);
		for (const FString& GuidText : RetiredGuidTexts)
		{
			FGuid Guid;
			FGuid::Parse(GuidText, Guid);
			TestNull(TEXT("retired GUID is absent from the reloaded asset"),
				FCortexGraphMigrationOps::FindNodeByGuid(Reloaded, Guid));
		}
		if (ReloadedRetained && ReloadedRetainedBody && ReloadedProducer)
		{
			UEdGraphPin* RetainedThen = ReloadedRetained->FindPin(TEXT("then"));
			UEdGraphPin* RetainedBodyExecute = ReloadedRetainedBody->FindPin(TEXT("execute"));
			UEdGraphPin* ProducerOutput = ReloadedProducer->FindPin(TEXT("ReturnValue"));
			UEdGraphPin* RetainedBodyInput = ReloadedRetainedBody->FindPin(TEXT("InString"));
			TestNotNull(TEXT("reloaded retained entry has its exec pin"), RetainedThen);
			TestNotNull(TEXT("reloaded retained body has its exec input"), RetainedBodyExecute);
			TestNotNull(TEXT("reloaded producer has its output pin"), ProducerOutput);
			TestNotNull(TEXT("reloaded retained body has its data input"), RetainedBodyInput);
			if (RetainedThen && RetainedBodyExecute)
			{
				TestTrue(TEXT("retained entry-to-body link survives the disk reload"),
					RetainedThen->LinkedTo.Contains(RetainedBodyExecute));
			}
			if (ProducerOutput && RetainedBodyInput)
			{
				TestTrue(TEXT("retained producer-to-body link survives the disk reload"),
					ProducerOutput->LinkedTo.Contains(RetainedBodyInput));
			}
		}
	}

	if (GEditor && GEditor->Trans)
	{
		GEditor->Trans->Reset(FText::FromString(TEXT("CortexGraphMigrationRetireSaveReloadCleanup")));
	}
	UPackage* PackageToCleanup = ReloadedPackage ? ReloadedPackage : PackageBeforeReload;
	if (PackageToCleanup)
	{
		PackageToCleanup->ClearFlags(RF_Standalone);
		PackageToCleanup->MarkAsGarbage();
		ResetLoaders(PackageToCleanup);
	}
	Fixture.Package = nullptr;
	Fixture.Blueprint = nullptr;
	FlushAsyncLoading();
	CollectGarbage(GARBAGE_COLLECTION_KEEPFLAGS);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexGraphMigrationRetireSaveDirtyStartTest,
	"Cortex.Graph.Authoring.Migration.Retire.RefusesDirtyStartSave",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexGraphMigrationRetireSaveDirtyStartTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	using namespace CortexGraphMigrationRetireTest;
	FFixture Fixture;
	TestTrue(TEXT("Widget fixture is created"), Fixture.Build(TEXT("BP_RetireSaveDirty"), true));
	if (!Fixture.Blueprint) { Fixture.Cleanup(); return false; }
	const FString Filename = Fixture.Filename();
	TestTrue(TEXT("baseline package is saved"), Fixture.SaveToDisk());
	const TArray<uint8> DiskBefore = ReadBytes(Filename);
	Fixture.Retained->NodeComment = TEXT("unrelated retained edit");
	FBlueprintEditorUtils::MarkBlueprintAsModified(Fixture.Blueprint);
	const FString GraphBefore = CaptureNativeGraph(Fixture.Graph);
	TArray<FString> Approved;
	TSharedPtr<FJsonObject> Request;
	FCortexCommandResult Error;
	TestTrue(FString::Printf(TEXT("dirty-start save request is prepared: %s"), *Error.ErrorMessage),
		PrepareApprovedRequest(Fixture, TEXT("00000000-0000-0000-0000-000000107405"),
			Request, Approved, Error, true, true));
	FCortexGraphPatchOutcome Outcome;
	TestFalse(TEXT("dirty-start save is refused"), FCortexGraphPatchOps::Execute(Fixture.Blueprint, Request, Outcome, Error));
	TestEqual(TEXT("dirty-start refusal reports DIRTY_EDITOR_STATE"), Error.ErrorCode, FString(TEXT("DIRTY_EDITOR_STATE")));
	TestEqual(TEXT("preflight refusal has no target compile"), Outcome.TargetCompileCount, 0);
	TestEqual(TEXT("dirty start has no recovery compile"), Outcome.RecoveryCompileCount, 0);
	TestEqual(TEXT("dirty edit remains in memory"), CaptureNativeGraph(Fixture.Graph), GraphBefore);
	TestTrue(TEXT("dirty baseline remains dirty"), Fixture.Package->IsDirty());
	TestTrue(TEXT("disk bytes remain unchanged"), SameBytes(DiskBefore, ReadBytes(Filename)));
	Fixture.Cleanup();
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexGraphMigrationRetireAbsentSourceReplayTest,
	"Cortex.Graph.Authoring.Migration.Retire.AbsentSourceReplayIsUnchanged",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexGraphMigrationRetireAbsentSourceReplayTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	using namespace CortexGraphMigrationRetireTest;
	FFixture Fixture;
	TestTrue(TEXT("retirement fixture is created"), Fixture.Build(TEXT("BP_RetireReplay"), true));
	if (!Fixture.Blueprint) { Fixture.Cleanup(); return false; }

	TSharedPtr<FJsonObject> InitialRequest;
	TArray<FString> Approved;
	FCortexCommandResult Error;
	TestTrue(TEXT("initial reviewed request is prepared"),
		PrepareApprovedRequest(Fixture, TEXT("00000000-0000-0000-0000-000000107501"),
			InitialRequest, Approved, Error));
	FCortexGraphPatchOutcome InitialOutcome;
	TestTrue(FString::Printf(TEXT("initial retirement applies: %s"), *Error.ErrorMessage),
		FCortexGraphPatchOps::Execute(Fixture.Blueprint, InitialRequest, InitialOutcome, Error));
	TestEqual(TEXT("initial retirement applies"), InitialOutcome.ApplyStatus, FString(TEXT("applied")));

	const FString GraphAfterRetirement = CaptureNativeGraph(Fixture.Graph);
	const TSharedPtr<FJsonObject> FingerprintAfterRetirement =
		FCortexGraphPatchState::ComputeFingerprint(Fixture.Blueprint);
	const FString FingerprintHashAfterRetirement =
		FingerprintAfterRetirement->GetStringField(TEXT("graph_authoring_hash"));
	const bool bDirtyAfterRetirement = Fixture.Package->IsDirty();
	const int32 QueueBeforeReplay = GEditor->Trans->GetQueueLength();
	const int32 UndoBeforeReplay = GEditor->Trans->GetUndoCount();
	const int32 NodeCountBeforeReplay = Fixture.Graph->Nodes.Num();
	TSharedPtr<FJsonObject> ReplayRequest = MakeShared<FJsonObject>();
	ReplayRequest->SetStringField(TEXT("asset_path"), Fixture.Blueprint->GetPathName());
	ReplayRequest->SetStringField(TEXT("patch_id"), TEXT("00000000-0000-0000-0000-000000107501"));
	ReplayRequest->SetObjectField(TEXT("expected_fingerprint"), FingerprintAfterRetirement);
	ReplayRequest->SetArrayField(TEXT("nodes"), {});
	ReplayRequest->SetArrayField(TEXT("connections"), {});
	ReplayRequest->SetArrayField(TEXT("pin_updates"), {});
	ReplayRequest->SetBoolField(TEXT("dry_run"), true);
	ReplayRequest->SetBoolField(TEXT("compile"), false);
	ReplayRequest->SetBoolField(TEXT("save"), false);
	ReplayRequest->SetBoolField(TEXT("allow_noop"), false);
	ReplayRequest->SetObjectField(TEXT("migration"), Fixture.Migration(
		{ Fixture.AlphaGuid.ToString(), Fixture.BetaGuid.ToString() }, true, Approved));

	FCortexGraphPreparedPatch ReplayPreview;
	Error = FCortexCommandResult();
	TestTrue(FString::Printf(TEXT("fresh approved replay preview succeeds: %s"), *Error.ErrorMessage),
		FCortexGraphPatchOps::Preflight(Fixture.Blueprint, ReplayRequest, ReplayPreview, Error));
	TestTrue(TEXT("replay preview reuses the retirement plan"), ReplayPreview.RetirementPlan.IsValid());
	TestFalse(TEXT("replay preview reports no graph change"), ReplayPreview.bChanged);
	TestTrue(TEXT("replay preview records absent-source provenance"), ReplayPreview.bReplayedWithAbsentSource);
	TestTrue(TEXT("replay preview carries a fresh validation hash"),
		!ReplayPreview.ValidationHash.IsEmpty()
			&& ReplayPreview.ValidationHash != InitialRequest->GetStringField(TEXT("expected_validation_hash")));
	if (ReplayPreview.RetirementPlan.IsValid())
	{
		const TSharedPtr<FJsonObject> Inventory =
			FCortexGraphMigrationOps::MakeRetirementInventory(ReplayPreview.RetirementPlan);
		TestTrue(TEXT("replay is marked reused"), Inventory.IsValid() && Inventory->GetBoolField(TEXT("reused")));
		TestTrue(TEXT("replay inventory is complete"), Inventory.IsValid() && Inventory->GetBoolField(TEXT("complete")));
		TestFalse(TEXT("replay inventory is not awaiting approval"),
			Inventory.IsValid() && Inventory->GetBoolField(TEXT("awaiting_approval")));
	}
	ReplayRequest->SetBoolField(TEXT("dry_run"), false);
	ReplayRequest->SetStringField(TEXT("expected_validation_hash"), ReplayPreview.ValidationHash);

	FOperations Operations;
	Operations.Begin();
	FCortexGraphPatchOutcome ReplayOutcome;
	Error = FCortexCommandResult();
	TestTrue(FString::Printf(TEXT("freshly reviewed replay applies: %s"), *Error.ErrorMessage),
		FCortexGraphPatchOps::Execute(Fixture.Blueprint, ReplayRequest, ReplayOutcome, Error));
	Operations.End();
	TestEqual(TEXT("replay reports unchanged"), ReplayOutcome.ApplyStatus, FString(TEXT("unchanged")));
	TestFalse(TEXT("replay reports no graph change"), ReplayOutcome.bChanged);
	TestTrue(TEXT("replay outcome reports reused retirement inventory"),
		ReplayOutcome.RetirementInventory.IsValid()
			&& ReplayOutcome.RetirementInventory->GetBoolField(TEXT("reused")));
	TestTrue(TEXT("replay records absent-source provenance"), ReplayOutcome.bReplayedWithAbsentSource);
	TestEqual(TEXT("replay creates no transaction"), GEditor->Trans->GetQueueLength(), QueueBeforeReplay);
	TestEqual(TEXT("replay creates no undo record"), GEditor->Trans->GetUndoCount(), UndoBeforeReplay);
	TestEqual(TEXT("replay requests no target compile"), Operations.TargetCompiles, 0);
	TestEqual(TEXT("replay requests no recovery compile"), Operations.RecoveryCompiles, 0);
	TestEqual(TEXT("replay does not save"), Operations.Saves, 0);
	TestFalse(TEXT("replay does not claim save"), ReplayOutcome.bSaved);
	TestEqual(TEXT("replay preserves graph state"), CaptureNativeGraph(Fixture.Graph), GraphAfterRetirement);
	TestEqual(TEXT("replay preserves graph node count"), Fixture.Graph->Nodes.Num(), NodeCountBeforeReplay);
	TestEqual(TEXT("replay preserves fingerprint"), FCortexGraphPatchState::ComputeFingerprint(Fixture.Blueprint)
		->GetStringField(TEXT("graph_authoring_hash")), FingerprintHashAfterRetirement);
	TestEqual(TEXT("replay preserves package dirty state"), Fixture.Package->IsDirty(), bDirtyAfterRetirement);
	Fixture.Cleanup();
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexGraphMigrationRetirePartialReplayRefusalTest,
	"Cortex.Graph.Authoring.Migration.Retire.PartialReplayNamesAbsentAndPresentIdentities",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexGraphMigrationRetirePartialReplayRefusalTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	using namespace CortexGraphMigrationRetireTest;
	FFixture Fixture;
	TestTrue(TEXT("retirement fixture is created"), Fixture.Build(TEXT("BP_RetirePartialReplay"), true));
	if (!Fixture.Blueprint) { Fixture.Cleanup(); return false; }
	TSharedPtr<FJsonObject> InitialRequest;
	TArray<FString> Approved;
	FCortexCommandResult Error;
	TestTrue(TEXT("initial reviewed request is prepared"),
		PrepareApprovedRequest(Fixture, TEXT("00000000-0000-0000-0000-000000107502"),
			InitialRequest, Approved, Error));
	FCortexGraphPatchOutcome Outcome;
	TestTrue(TEXT("initial retirement applies"),
		FCortexGraphPatchOps::Execute(Fixture.Blueprint, InitialRequest, Outcome, Error));

	UK2Node_CustomEvent* Reintroduced = NewObject<UK2Node_CustomEvent>(Fixture.Graph);
	Reintroduced->NodeGuid = FGuid(Approved[0]);
	Fixture.Graph->AddNode(Reintroduced, true, false);
	FCortexGraphMigrationRetirePlan PlanValue;
	bool bReused = false;
	Error = FCortexCommandResult();
	TestFalse(TEXT("partial postcondition is refused"),
		Plan(Fixture, { Fixture.AlphaGuid.ToString(), Fixture.BetaGuid.ToString() },
			PlanValue, bReused, Error, true, Approved));
	TestEqual(TEXT("partial replay reports INVALID_OPERATION"), Error.ErrorCode, FString(TEXT("INVALID_OPERATION")));
	TArray<FString> ExpectedAbsent = Approved;
	ExpectedAbsent.RemoveAt(0);
	ExpectedAbsent.Sort();
	const FString ExpectedPartialDiagnostic = FString::Printf(
		TEXT("retirement replay is partial: %d approved identities are absent [%s] and 1 are present [%s]"),
		Approved.Num() - 1, *FString::Join(ExpectedAbsent, TEXT(", ")), *Approved[0]);
	TestEqual(TEXT("partial replay reports exact absent and present counts and identities"),
		Error.ErrorMessage, ExpectedPartialDiagnostic);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexGraphMigrationRetireOwnershipConflictTest,
	"Cortex.Graph.Authoring.Migration.Retire.AbsentSourceOwnershipConflictIsRefused",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexGraphMigrationRetireOwnershipConflictTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	using namespace CortexGraphMigrationRetireTest;
	FFixture Fixture;
	TestTrue(TEXT("retirement fixture is created"), Fixture.Build(TEXT("BP_RetireOwnershipConflict"), true));
	if (!Fixture.Blueprint) { Fixture.Cleanup(); return false; }
	TSharedPtr<FJsonObject> InitialRequest;
	TArray<FString> Approved;
	FCortexCommandResult Error;
	TestTrue(TEXT("initial reviewed request is prepared"),
		PrepareApprovedRequest(Fixture, TEXT("00000000-0000-0000-0000-000000107503"),
			InitialRequest, Approved, Error));
	FCortexGraphPatchOutcome Outcome;
	TestTrue(TEXT("initial retirement applies"),
		FCortexGraphPatchOps::Execute(Fixture.Blueprint, InitialRequest, Outcome, Error));

	UEdGraph* OtherGraph = NewObject<UEdGraph>(Fixture.Blueprint);
	OtherGraph->GraphGuid = FGuid::NewGuid();
	Fixture.Blueprint->FunctionGraphs.Add(OtherGraph);
	UK2Node_CustomEvent* Reowned = NewObject<UK2Node_CustomEvent>(OtherGraph);
	Reowned->NodeGuid = FGuid(Approved[0]);
	OtherGraph->AddNode(Reowned, true, false);
	FCortexGraphMigrationRetirePlan PlanValue;
	bool bReused = false;
	Error = FCortexCommandResult();
	TestFalse(TEXT("approved absent GUID owned by another graph is refused"),
		Plan(Fixture, { Fixture.AlphaGuid.ToString(), Fixture.BetaGuid.ToString() },
			PlanValue, bReused, Error, true, Approved));
	TestEqual(TEXT("ownership conflict reports INVALID_OPERATION"), Error.ErrorCode, FString(TEXT("INVALID_OPERATION")));
	TestTrue(TEXT("ownership conflict names the conflicting GUID and graph"),
		Error.ErrorMessage.Contains(Approved[0]) && Error.ErrorMessage.Contains(OtherGraph->GraphGuid.ToString()));
	Fixture.Cleanup();
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexGraphMigrationRetireDiagnosticsTruncationTest,
	"Cortex.Graph.Authoring.Migration.Retire.DiagnosticsTruncationIsTruthful",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexGraphMigrationRetireDiagnosticsTruncationTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	using namespace CortexGraphMigrationRetireTest;

	// Case A: Long cached diagnostic (> 512 chars)
	{
		FFixture Fixture;
		TestTrue(TEXT("Case A fixture created"), Fixture.Build(TEXT("BP_RetireDiagLong")));
		if (Fixture.Blueprint)
		{
			Fixture.Alpha->bHasCompilerMessage = true;
			Fixture.Alpha->ErrorMsg = FString::ChrN(700, TEXT('x'));

			FCortexGraphMigrationRetirePlan PlanValue;
			bool bReused = false;
			FCortexCommandResult Error;
			TestTrue(TEXT("preview succeeds with long diagnostic"),
				Plan(Fixture, { Fixture.AlphaGuid.ToString(), Fixture.BetaGuid.ToString() }, PlanValue, bReused, Error));
			TestEqual(TEXT("exactly one cached diagnostic collected"), PlanValue.PreexistingDiagnostics.Num(), 1);
			if (PlanValue.PreexistingDiagnostics.Num() == 1)
			{
				TestTrue(TEXT("returned string respects 512 character bound"), PlanValue.PreexistingDiagnostics[0].Len() <= 512);
			}
			TestTrue(TEXT("preexisting_diagnostics_truncated is true for character truncation"),
				PlanValue.bPreexistingDiagnosticsTruncated);

			const TSharedPtr<FJsonObject> Inventory = FCortexGraphMigrationOps::MakeRetirementInventory(PlanValue.ToJson());
			TestNotNull(TEXT("inventory is valid"), Inventory.Get());
			if (Inventory.IsValid())
			{
				TestTrue(TEXT("inventory reports preexisting_diagnostics_truncated true"),
					Inventory->GetBoolField(TEXT("preexisting_diagnostics_truncated")));
				TestEqual(TEXT("inventory names cached_node_messages source"),
					Inventory->GetStringField(TEXT("preexisting_diagnostics_source")), FString(TEXT("cached_node_messages")));
			}
			Fixture.Cleanup();
		}
	}

	// Case B: Count truncation (> 16 messages)
	{
		FFixture Fixture;
		TestTrue(TEXT("Case B fixture created"), Fixture.Build(TEXT("BP_RetireDiagCount")));
		if (Fixture.Blueprint)
		{
			for (int32 Index = 0; Index < 20; ++Index)
			{
				UK2Node_CallFunction* Dummy = Fixture.AddCall(UKismetSystemLibrary::StaticClass()->FindFunctionByName(TEXT("PrintString")));
				Dummy->bHasCompilerMessage = true;
				Dummy->ErrorMsg = FString::Printf(TEXT("error message %d"), Index);
			}

			FCortexGraphMigrationRetirePlan PlanValue;
			bool bReused = false;
			FCortexCommandResult Error;
			TestTrue(TEXT("preview succeeds with many diagnostics"),
				Plan(Fixture, { Fixture.AlphaGuid.ToString(), Fixture.BetaGuid.ToString() }, PlanValue, bReused, Error));
			TestTrue(TEXT("diagnostics count bounded by 16"), PlanValue.PreexistingDiagnostics.Num() <= 16);
			TestTrue(TEXT("omission marker is present"),
				PlanValue.PreexistingDiagnostics.Contains(TEXT("additional compiler diagnostics omitted")));
			TestTrue(TEXT("preexisting_diagnostics_truncated is true for count truncation"),
				PlanValue.bPreexistingDiagnosticsTruncated);

			const TSharedPtr<FJsonObject> Inventory = FCortexGraphMigrationOps::MakeRetirementInventory(PlanValue.ToJson());
			TestNotNull(TEXT("inventory is valid"), Inventory.Get());
			if (Inventory.IsValid())
			{
				TestTrue(TEXT("inventory reports preexisting_diagnostics_truncated true for count truncation"),
					Inventory->GetBoolField(TEXT("preexisting_diagnostics_truncated")));
			}
			Fixture.Cleanup();
		}
	}

	// Case C: No truncation (single short message)
	{
		FFixture Fixture;
		TestTrue(TEXT("Case C fixture created"), Fixture.Build(TEXT("BP_RetireDiagShort")));
		if (Fixture.Blueprint)
		{
			Fixture.Alpha->bHasCompilerMessage = true;
			Fixture.Alpha->ErrorMsg = TEXT("short compiler warning message");

			FCortexGraphMigrationRetirePlan PlanValue;
			bool bReused = false;
			FCortexCommandResult Error;
			TestTrue(TEXT("preview succeeds with short diagnostic"),
				Plan(Fixture, { Fixture.AlphaGuid.ToString(), Fixture.BetaGuid.ToString() }, PlanValue, bReused, Error));
			TestEqual(TEXT("exactly one cached diagnostic collected"), PlanValue.PreexistingDiagnostics.Num(), 1);
			TestFalse(TEXT("preexisting_diagnostics_truncated is false when no truncation occurs"),
				PlanValue.bPreexistingDiagnosticsTruncated);

			const TSharedPtr<FJsonObject> Inventory = FCortexGraphMigrationOps::MakeRetirementInventory(PlanValue.ToJson());
			TestNotNull(TEXT("inventory is valid"), Inventory.Get());
			if (Inventory.IsValid())
			{
				TestFalse(TEXT("inventory reports preexisting_diagnostics_truncated false when no truncation"),
					Inventory->GetBoolField(TEXT("preexisting_diagnostics_truncated")));
			}
			Fixture.Cleanup();
		}
	}

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexGraphMigrationRetireConformEngineTransitionTest,
	"Cortex.Graph.Authoring.Migration.Retire.PostReparentCompileConformsStaleOverride",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexGraphMigrationRetireConformEngineTransitionTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	using namespace CortexGraphMigrationRetireTest;

	FFixture Fixture;
	TestTrue(TEXT("Build legacy parent widget fixture"),
		Fixture.Build(TEXT("BP_RetireEngineTransition"), false, false, UCortexGraphRetireLegacyWidget::StaticClass()));
	if (!Fixture.Blueprint) return false;

	// 1. Before compile / reparent: genuine eligible override event
	TestNotNull(TEXT("Alpha exists before reparent"), Fixture.Alpha);
	TestTrue(TEXT("Alpha is UK2Node_Event before reparent"), Fixture.Alpha != nullptr && !Fixture.Alpha->IsA<UK2Node_CustomEvent>());
	if (Fixture.Alpha)
	{
		TestTrue(TEXT("Alpha is an override function before reparent"), Fixture.Alpha->bOverrideFunction);
		TestEqual(TEXT("Alpha member name is OnLegacyAlpha"),
			Fixture.Alpha->EventReference.GetMemberName(), FName(TEXT("OnLegacyAlpha")));
	}
	const FGuid OriginalAlphaGuid = Fixture.AlphaGuid;

	// 2. Reparent to target widget (where OnLegacyAlpha does not exist)
	Fixture.Blueprint->ParentClass = UCortexGraphRetireCollisionTargetWidget::StaticClass();
	FBlueprintEditorUtils::RefreshAllNodes(Fixture.Blueprint);

	// 3. Add an independent deterministic defect causing compile failure
	Fixture.AddNativeNameCollision();

	// 4. Compile: Blueprint enters real BS_Error
	// In UE 5.8, ConformImplementedEvents emits a warning for each stale override conformed to custom event
	AddExpectedError(TEXT("replaced as a Custom Event"), EAutomationExpectedErrorFlags::Contains, 2);
	AddExpectedError(TEXT("name conflicts with a native"), EAutomationExpectedErrorFlags::Contains, 1);
	FKismetEditorUtilities::CompileBlueprint(Fixture.Blueprint);

	TestEqual(TEXT("Blueprint entered real BS_Error status"),
		static_cast<int32>(Fixture.Blueprint->Status), static_cast<int32>(BS_Error));

	// 5. In UE 5.8, FBlueprintEditorUtils::ConformImplementedEvents destroys stale override UK2Node_Event
	// and substitutes a UK2Node_CustomEvent that preserves the original NodeGuid
	UK2Node_CustomEvent* SubstituteCustomEvent = nullptr;
	for (UEdGraphNode* Node : Fixture.Graph->Nodes)
	{
		if (UK2Node_CustomEvent* CustomEvent = Cast<UK2Node_CustomEvent>(Node))
		{
			if (CustomEvent->CustomFunctionName.ToString().Contains(TEXT("OnLegacyAlpha")))
			{
				SubstituteCustomEvent = CustomEvent;
				break;
			}
		}
	}
	TestNotNull(TEXT("stale override node was conformed into a UK2Node_CustomEvent by UE 5.8 compiler"),
		SubstituteCustomEvent);
	if (SubstituteCustomEvent)
	{
		TestEqual(TEXT("substitute custom event inherits the original node GUID"),
			SubstituteCustomEvent->NodeGuid, OriginalAlphaGuid);
		TestTrue(TEXT("substitute node is a UK2Node_CustomEvent"),
			SubstituteCustomEvent->IsA<UK2Node_CustomEvent>());
		TestFalse(TEXT("substitute node no longer has bOverrideFunction"),
			SubstituteCustomEvent->bOverrideFunction);
	}

	// 6. CortexSandbox #112 classifies the conformed node by its current shape: it is now an
	// unreferenced custom event, so the guarded contract accepts it instead of pinning the legacy
	// refusal that only described the old override-only predicate.
	FCortexGraphMigrationRetirePlan PlanValue;
	bool bReused = false;
	FCortexCommandResult Error;
	const TArray<FString> Selected = { OriginalAlphaGuid.ToString() };
	const bool bAccepted = Plan(Fixture, Selected, PlanValue, bReused, Error);
	TestTrue(FString::Printf(TEXT("retire_entries accepts the conformed unreferenced custom event: %s"), *Error.ErrorMessage), bAccepted);
	if (bAccepted)
	{
		TestTrue(TEXT("the conformed custom event entry is removable"),
			PlanValue.RemovableGuids.Contains(OriginalAlphaGuid.ToString()));
		TestTrue(TEXT("a custom-event entry publishes that a compile is required for its cleanup"),
			PlanValue.bRequiresCompile);
		TestEqual(TEXT("the plan publishes exactly one entry"), PlanValue.Entries.Num(), 1);
		if (PlanValue.Entries.Num() == 1)
		{
			TestEqual(TEXT("the conformed entry is classified as a custom event by its current node shape"),
				PlanValue.Entries[0].Kind, FString(TEXT("custom_event")));
			TestTrue(TEXT("the conformed entry carries its real generated function name"),
				PlanValue.Entries[0].CustomFunctionName.Contains(TEXT("OnLegacyAlpha")));
		}
	}

	// 7. A live caller makes the very same node non-retirable, because removing it would sever the
	// dispatch the asset really relies on.
	if (SubstituteCustomEvent && bAccepted)
	{
		Error = FCortexCommandResult();
		UK2Node_CallFunction* Caller = NewObject<UK2Node_CallFunction>(Fixture.Graph);
		Caller->FunctionReference.SetSelfMember(FName(*SubstituteCustomEvent->CustomFunctionName.ToString()));
		Caller->CreateNewGuid();
		Caller->AllocateDefaultPins();
		Fixture.Graph->AddNode(Caller, true, false);
		TestFalse(TEXT("retire_entries refuses a custom event with an in-asset caller"),
			Plan(Fixture, Selected, PlanValue, bReused, Error));
		TestEqual(TEXT("caller refusal error code is INVALID_OPERATION"), Error.ErrorCode, FString(TEXT("INVALID_OPERATION")));
		TestTrue(TEXT("the refusal names the in-asset caller"),
			Error.ErrorMessage.Contains(TEXT("still referenced in this asset")));
	}

	Fixture.Cleanup();
	return true;
}

// CortexSandbox #112: a component-bound delegate entry is a genuine event island, not an
// unsupported node. Before the guarded extension `retire_entries` refused it as "not a supported
// unbound override event", so this test fails on the old entry eligibility.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexGraphMigrationRetireComponentBoundEntryTest,
	"Cortex.Graph.Authoring.Migration.Retire.SupportsComponentBoundDelegateEntry",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexGraphMigrationRetireComponentBoundEntryTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	using namespace CortexGraphMigrationRetireTest;
	FGuardedEntryFixture Fixture;
	TestTrue(TEXT("Widget fixture with a compiled component-bound delegate is created"),
		Fixture.Build(TEXT("BP_RetireComponentBoundEntry")));
	if (!Fixture.IsComplete()) { Fixture.Cleanup(); return false; }
	TestTrue(TEXT("the guarded fixture compiles without a Blueprint error"),
		static_cast<int32>(Fixture.Blueprint->Status) != static_cast<int32>(BS_Error));

	// Genuine identity: the compiled widget variable, the native delegate and the binding function.
	TestEqual(TEXT("component-bound entry binds the compiled widget variable"),
		Fixture.Bound->GetComponentPropertyName(), FName(TEXT("ActionButton")));
	TestEqual(TEXT("component-bound entry binds the native OnClicked delegate"),
		Fixture.Bound->DelegatePropertyName, FName(TEXT("OnClicked")));
	TestTrue(TEXT("component delegate owner is the native button class"),
		Fixture.Bound->DelegateOwnerClass.Get() == UButton::StaticClass());
	TestTrue(TEXT("component-bound entry is an internal event node"), Fixture.Bound->bInternalEvent);
	TestTrue(TEXT("component-bound entry is not an unbound override"), !Fixture.Bound->bOverrideFunction);
	TestNotNull(TEXT("the generated binding function exists in the compiled class"),
		Fixture.Blueprint->GeneratedClass
			? Fixture.Blueprint->GeneratedClass->FindFunctionByName(Fixture.Bound->CustomFunctionName) : nullptr);
	const FString BindingsBefore = CaptureComponentBindings(Fixture.Blueprint);
	TestTrue(TEXT("the compiled class registers the ActionButton.OnClicked component binding"),
		BindingsBefore.Contains(TEXT("ActionButton|OnClicked")));

	const FString GraphBefore = CaptureNativeGraph(Fixture.Graph);
	const FString WidgetsBefore = CaptureWidgetTree(Fixture.Blueprint);
	const bool bDirtyBefore = Fixture.Package->IsDirty();

	FCortexGraphMigrationRetirePlan PlanValue;
	bool bReused = false;
	FCortexCommandResult Error;
	const TArray<FString> Selected = { Fixture.Bound->NodeGuid.ToString() };
	const bool bPreviewed = Plan(Fixture, Selected, PlanValue, bReused, Error);
	TestTrue(FString::Printf(TEXT("retire_entries accepts a component-bound delegate entry: %s"), *Error.ErrorMessage), bPreviewed);
	TestTrue(TEXT("component-bound entry is removable"), PlanValue.RemovableGuids.Contains(Fixture.Bound->NodeGuid.ToString()));
	TestTrue(TEXT("the component-bound entry island body is removable"),
		PlanValue.RemovableGuids.Contains(Fixture.BoundBody->NodeGuid.ToString()));
	TestFalse(TEXT("the unrelated cosmetic override is not removable"),
		PlanValue.RemovableGuids.Contains(Fixture.Retained->NodeGuid.ToString()));
	TestFalse(TEXT("the retained cosmetic override body is not removable"),
		PlanValue.RemovableGuids.Contains(Fixture.RetainedBody->NodeGuid.ToString()));
	TestEqual(TEXT("preview leaves the graph unchanged"), CaptureNativeGraph(Fixture.Graph), GraphBefore);
	TestEqual(TEXT("preview leaves the widget tree unchanged"), CaptureWidgetTree(Fixture.Blueprint), WidgetsBefore);
	TestEqual(TEXT("preview leaves the component delegate bindings unchanged"),
		CaptureComponentBindings(Fixture.Blueprint), BindingsBefore);
	TestEqual(TEXT("preview leaves the dirty state unchanged"), Fixture.Package->IsDirty(), bDirtyBefore);

	// Exact reviewed approval covers the same entry class and still refuses partial approval. The
	// approval phase only runs once the preview produced a removable set, so an unsupported entry
	// class fails on the legible preview assertion above instead of crashing on an empty approval.
	if (bPreviewed && !PlanValue.RemovableGuids.IsEmpty())
	{
		FCortexGraphMigrationRetirePlan ApprovedPlan;
		Error = FCortexCommandResult();
		const TArray<FString> ExactApproval = PlanValue.RemovableGuids;
		const bool bApproved = Plan(Fixture, Selected, ApprovedPlan, bReused, Error, true, ExactApproval);
		TestTrue(FString::Printf(TEXT("reviewed approval accepts the component-bound plan: %s"), *Error.ErrorMessage), bApproved);
		TestTrue(TEXT("reviewed component-bound plan is no longer awaiting approval"), !ApprovedPlan.bAwaitingApproval);
		TestTrue(TEXT("reviewed component-bound plan approves the exact removable set"),
			ApprovedPlan.ApprovedGuids == ApprovedPlan.RemovableGuids);

		FCortexGraphMigrationRetirePlan PartialPlan;
		Error = FCortexCommandResult();
		TArray<FString> PartialApproval = ExactApproval;
		PartialApproval.Pop();
		const bool bPartial = Plan(Fixture, Selected, PartialPlan, bReused, Error, true, PartialApproval);
		TestTrue(FString::Printf(TEXT("partial approval is refused for the component-bound plan: %s"), *Error.ErrorMessage), !bPartial);
	}

	TestNotNull(TEXT("the preserved cosmetic override survives the preview"),
		FCortexGraphMigrationOps::FindNodeByGuid(Fixture.Blueprint, Fixture.Retained->NodeGuid));
	TestTrue(TEXT("the preserved cosmetic override keeps its execution link"),
		Fixture.Retained->FindPin(TEXT("then"))
			&& Fixture.Retained->FindPin(TEXT("then"))->LinkedTo.Contains(Fixture.RetainedBody->FindPin(TEXT("execute"))));

	Fixture.Cleanup();
	return true;
}

// CortexSandbox #112: widget Construct/OnInitialized override bodies become obsolete once the
// reparented native parent implements the lifecycle. Before the guarded extension `retire_entries`
// refused every lifecycle member by name, so this test fails on the old entry eligibility.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexGraphMigrationRetireWidgetLifecycleEntryTest,
	"Cortex.Graph.Authoring.Migration.Retire.SupportsWidgetLifecycleEntry",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexGraphMigrationRetireWidgetLifecycleEntryTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	using namespace CortexGraphMigrationRetireTest;
	FGuardedEntryFixture Fixture;
	TestTrue(TEXT("natively reparented Widget fixture is created"), Fixture.Build(TEXT("BP_RetireWidgetLifecycleEntry")));
	if (!Fixture.IsComplete()) { Fixture.Cleanup(); return false; }
	TestTrue(TEXT("the guarded fixture compiles without a Blueprint error"),
		static_cast<int32>(Fixture.Blueprint->Status) != static_cast<int32>(BS_Error));

	// A plain UUserWidget parent is not proof that native behaviour replaced the retired body.
	TestTrue(TEXT("the lifecycle fixture parent is not plain UUserWidget"),
		Fixture.Blueprint->ParentClass && Fixture.Blueprint->ParentClass != UUserWidget::StaticClass());
	TestTrue(TEXT("the lifecycle fixture parent is a native widget class"),
		Fixture.Blueprint->ParentClass && Fixture.Blueprint->ParentClass->IsChildOf(UUserWidget::StaticClass())
			&& Fixture.Blueprint->ParentClass->HasAnyClassFlags(CLASS_Native));

	TestEqual(TEXT("Construct entry overrides the widget lifecycle declaration"),
		Fixture.Construct->EventReference.GetMemberName(), FName(TEXT("Construct")));
	TestTrue(TEXT("Construct entry is an override event"), Fixture.Construct->bOverrideFunction);
	TestTrue(TEXT("Construct entry is not an internal event"), !Fixture.Construct->bInternalEvent);
	TestTrue(TEXT("Construct entry is a plain event node, not a custom event"), !Fixture.Construct->IsA<UK2Node_CustomEvent>());
	TestEqual(TEXT("OnInitialized entry overrides the widget lifecycle declaration"),
		Fixture.Initialized->EventReference.GetMemberName(), FName(TEXT("OnInitialized")));
	TestTrue(TEXT("OnInitialized entry is an override event"), Fixture.Initialized->bOverrideFunction);
	TestTrue(TEXT("OnInitialized entry is a plain event node, not a custom event"), !Fixture.Initialized->IsA<UK2Node_CustomEvent>());

	const FString GraphBefore = CaptureNativeGraph(Fixture.Graph);
	const FString WidgetsBefore = CaptureWidgetTree(Fixture.Blueprint);

	FCortexGraphMigrationRetirePlan PlanValue;
	bool bReused = false;
	FCortexCommandResult Error;
	const TArray<FString> Selected = {
		Fixture.Construct->NodeGuid.ToString(), Fixture.Initialized->NodeGuid.ToString() };
	const bool bPreviewed = Plan(Fixture, Selected, PlanValue, bReused, Error);
	TestTrue(FString::Printf(TEXT("retire_entries accepts widget lifecycle entries: %s"), *Error.ErrorMessage), bPreviewed);
	TestTrue(TEXT("Construct entry is removable"), PlanValue.RemovableGuids.Contains(Fixture.Construct->NodeGuid.ToString()));
	TestTrue(TEXT("Construct island body is removable"),
		PlanValue.RemovableGuids.Contains(Fixture.ConstructBody->NodeGuid.ToString()));
	TestTrue(TEXT("OnInitialized entry is removable"), PlanValue.RemovableGuids.Contains(Fixture.Initialized->NodeGuid.ToString()));
	TestTrue(TEXT("OnInitialized island body is removable"),
		PlanValue.RemovableGuids.Contains(Fixture.InitializedBody->NodeGuid.ToString()));
	TestFalse(TEXT("the unrelated cosmetic override is not removable"),
		PlanValue.RemovableGuids.Contains(Fixture.Retained->NodeGuid.ToString()));
	TestEqual(TEXT("preview leaves the graph unchanged"), CaptureNativeGraph(Fixture.Graph), GraphBefore);
	TestEqual(TEXT("preview leaves the widget tree unchanged"), CaptureWidgetTree(Fixture.Blueprint), WidgetsBefore);

	Fixture.Cleanup();
	return true;
}

// CortexSandbox #112: an unreferenced custom event is a callable generated function with no caller,
// so retiring its island severs no dispatch. Before the guarded extension `retire_entries` refused
// every UK2Node_CustomEvent, so this test fails on the old entry eligibility.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexGraphMigrationRetireUnreferencedCustomEventTest,
	"Cortex.Graph.Authoring.Migration.Retire.SupportsUnreferencedCustomEventEntry",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexGraphMigrationRetireUnreferencedCustomEventTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	using namespace CortexGraphMigrationRetireTest;
	FGuardedEntryFixture Fixture;
	TestTrue(TEXT("Widget fixture with an unreferenced custom event is created"),
		Fixture.Build(TEXT("BP_RetireUnreferencedCustomEvent")));
	if (!Fixture.IsComplete()) { Fixture.Cleanup(); return false; }
	TestTrue(TEXT("the guarded fixture compiles without a Blueprint error"),
		static_cast<int32>(Fixture.Blueprint->Status) != static_cast<int32>(BS_Error));

	TestTrue(TEXT("the custom event entry is not an override event"), !Fixture.Custom->bOverrideFunction);
	TestEqual(TEXT("the custom event entry carries its real custom function name"),
		Fixture.Custom->CustomFunctionName, FName(TEXT("OnLegacyTutorialEvent")));
	TestNotNull(TEXT("the custom event compiled into a callable generated function"),
		Fixture.Blueprint->GeneratedClass
			? Fixture.Blueprint->GeneratedClass->FindFunctionByName(Fixture.Custom->CustomFunctionName) : nullptr);
	int32 InAssetCallers = 0;
	for (const UEdGraphNode* Node : Fixture.Graph->Nodes)
	{
		const UK2Node_CallFunction* Call = Cast<UK2Node_CallFunction>(Node);
		if (Call && Call->FunctionReference.GetMemberName() == Fixture.Custom->CustomFunctionName) ++InAssetCallers;
	}
	TestEqual(TEXT("the fixture custom event has no in-asset caller"), InAssetCallers, 0);

	const FString GraphBefore = CaptureNativeGraph(Fixture.Graph);
	const FString WidgetsBefore = CaptureWidgetTree(Fixture.Blueprint);

	FCortexGraphMigrationRetirePlan PlanValue;
	bool bReused = false;
	FCortexCommandResult Error;
	const TArray<FString> Selected = { Fixture.Custom->NodeGuid.ToString() };
	const bool bPreviewed = Plan(Fixture, Selected, PlanValue, bReused, Error);
	TestTrue(FString::Printf(TEXT("retire_entries accepts an unreferenced custom event entry: %s"), *Error.ErrorMessage), bPreviewed);
	TestTrue(TEXT("unreferenced custom event entry is removable"), PlanValue.RemovableGuids.Contains(Fixture.Custom->NodeGuid.ToString()));
	TestTrue(TEXT("the custom event island body is removable"),
		PlanValue.RemovableGuids.Contains(Fixture.CustomBody->NodeGuid.ToString()));
	TestFalse(TEXT("the unrelated cosmetic override is not removable"),
		PlanValue.RemovableGuids.Contains(Fixture.Retained->NodeGuid.ToString()));
	TestEqual(TEXT("preview leaves the graph unchanged"), CaptureNativeGraph(Fixture.Graph), GraphBefore);
	TestEqual(TEXT("preview leaves the widget tree unchanged"), CaptureWidgetTree(Fixture.Blueprint), WidgetsBefore);

	Fixture.Cleanup();
	return true;
}

// CortexSandbox #112: the class-specific identity of a component-bound entry is published in the
// plan, so the reviewed approval binds the exact component property, delegate property and generated
// binding function, and every unresolved or ambiguous fact is refused before anything mutates.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexGraphMigrationRetireComponentBoundInventoryTest,
	"Cortex.Graph.Authoring.Migration.Retire.ComponentBoundInventoryAndRefusals",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexGraphMigrationRetireComponentBoundInventoryTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	using namespace CortexGraphMigrationRetireTest;
	FGuardedEntryFixture Fixture;
	TestTrue(TEXT("guarded component-bound fixture is created"),
		Fixture.Build(TEXT("BP_RetireComponentBoundInventory")));
	if (!Fixture.IsComplete()) { Fixture.Cleanup(); return false; }

	const FName BindingFunction = Fixture.Bound->CustomFunctionName;
	const FString Selected = Fixture.Bound->NodeGuid.ToString();
	FCortexGraphMigrationRetirePlan PlanValue;
	bool bReused = false;
	FCortexCommandResult Error;
	const bool bPreviewed = Plan(Fixture, { Selected }, PlanValue, bReused, Error);
	TestTrue(FString::Printf(TEXT("component-bound preview succeeds: %s"), *Error.ErrorMessage), bPreviewed);
	if (!bPreviewed) { Fixture.Cleanup(); return false; }

	TestEqual(TEXT("the plan publishes exactly one entry identity"), PlanValue.Entries.Num(), 1);
	if (PlanValue.Entries.Num() == 1)
	{
		const FCortexGraphMigrationRetireEntry& Entry = PlanValue.Entries[0];
		TestEqual(TEXT("the entry is classified as a component-bound event"),
			Entry.Kind, FString(TEXT("component_bound_event")));
		TestEqual(TEXT("the entry binds the compiled widget variable"),
			Entry.ComponentPropertyName, FString(TEXT("ActionButton")));
		TestEqual(TEXT("the entry binds the native OnClicked delegate"),
			Entry.DelegatePropertyName, FString(TEXT("OnClicked")));
		TestTrue(TEXT("the entry names the native delegate owner class"),
			Entry.DelegateOwnerClass.Contains(TEXT("Button")));
		TestEqual(TEXT("the entry names the generated binding function"),
			Entry.BindingFunctionName, BindingFunction.ToString());
	}
	TestTrue(TEXT("a component-bound entry publishes that a compile is required"),
		PlanValue.bRequiresCompile);

	const TSharedPtr<FJsonObject> Inventory = FCortexGraphMigrationOps::MakeRetirementInventory(PlanValue.ToJson());
	const TArray<TSharedPtr<FJsonValue>>* EntryValues = nullptr;
	TArray<FString> EntryLines;
	if (Inventory.IsValid() && Inventory->TryGetArrayField(TEXT("selected_entries"), EntryValues) && EntryValues)
	{
		for (const TSharedPtr<FJsonValue>& Value : *EntryValues) EntryLines.Add(Value->AsString());
	}
	TestEqual(TEXT("the inventory publishes one class-specific entry line"), EntryLines.Num(), 1);
	TestTrue(TEXT("the inventory line names the class, component, delegate and binding function"),
		EntryLines.Num() == 1 && EntryLines[0].Contains(TEXT("component_bound_event"))
			&& EntryLines[0].Contains(TEXT("ActionButton")) && EntryLines[0].Contains(TEXT("OnClicked"))
			&& EntryLines[0].Contains(BindingFunction.ToString()));
	bool bInventoryRequiresCompile = false;
	TestTrue(TEXT("the inventory publishes requires_compile=true"),
		Inventory.IsValid()
			&& Inventory->TryGetBoolField(TEXT("requires_compile"), bInventoryRequiresCompile)
			&& bInventoryRequiresCompile);

	// An unresolved component property is refused instead of being trusted by name.
	const FName BoundComponent = Fixture.Bound->ComponentPropertyName;
	const FName BoundDelegate = Fixture.Bound->DelegatePropertyName;
	Fixture.Bound->ComponentPropertyName = TEXT("MissingWidgetVariable");
	Error = FCortexCommandResult();
	TestFalse(TEXT("an unresolved component property is refused"),
		Plan(Fixture, { Selected }, PlanValue, bReused, Error));
	TestTrue(TEXT("the refusal names the unresolved component property"),
		Error.ErrorMessage.Contains(TEXT("does not resolve to an object property")));
	Fixture.Bound->ComponentPropertyName = BoundComponent;

	// An unresolved delegate is refused as well: a remapped or deleted delegate is never assumed safe.
	Error = FCortexCommandResult();
	Fixture.Bound->DelegatePropertyName = TEXT("OnMissingDelegate");
	TestFalse(TEXT("an unresolved delegate property is refused"),
		Plan(Fixture, { Selected }, PlanValue, bReused, Error));
	TestTrue(TEXT("the refusal names the unresolved delegate"),
		Error.ErrorMessage.Contains(TEXT("does not resolve to a multicast delegate property")));
	Fixture.Bound->DelegatePropertyName = BoundDelegate;

	// An in-asset reference to the generated binding function is refused.
	Error = FCortexCommandResult();
	UK2Node_CallFunction* BindingCaller = NewObject<UK2Node_CallFunction>(Fixture.Graph);
	BindingCaller->FunctionReference.SetSelfMember(BindingFunction);
	BindingCaller->CreateNewGuid();
	BindingCaller->AllocateDefaultPins();
	Fixture.Graph->AddNode(BindingCaller, true, false);
	TestFalse(TEXT("an in-asset reference to the generated binding function is refused"),
		Plan(Fixture, { Selected }, PlanValue, bReused, Error));
	TestTrue(TEXT("the refusal names the in-asset binding reference"),
		Error.ErrorMessage.Contains(TEXT("still reference its generated binding function")));

	// A second event node bound to the same component delegate is an ambiguous duplicate binding. It
	// is created through the same engine initializer the editor uses, so the duplicate is real.
	Error = FCortexCommandResult();
	UK2Node_ComponentBoundEvent* Duplicate = NewObject<UK2Node_ComponentBoundEvent>(Fixture.Graph, NAME_None, RF_Transactional);
	Duplicate->CreateNewGuid();
	Duplicate->InitializeComponentBoundEventParams(Fixture.ComponentProperty, Fixture.ClickedDelegate);
	Fixture.Graph->AddNode(Duplicate, true, false);
	Duplicate->AllocateDefaultPins();
	TestTrue(TEXT("the duplicate node really shares the component delegate pair"),
		Duplicate->ComponentPropertyName == Fixture.Bound->ComponentPropertyName
			&& Duplicate->DelegatePropertyName == Fixture.Bound->DelegatePropertyName
			&& Duplicate->NodeGuid != Fixture.Bound->NodeGuid);
	TestFalse(TEXT("an ambiguous duplicate component binding is refused"),
		Plan(Fixture, { Selected }, PlanValue, bReused, Error));
	TestTrue(TEXT("the refusal reports the ambiguous duplicate binding"),
		Error.ErrorMessage.Contains(TEXT("ambiguous duplicate binding")));

	Fixture.Cleanup();
	return true;
}

// CortexSandbox #112: the removed component-bound node leaves its generated dynamic binding behind
// until a compile rebuilds the binding array, so a compiled apply must prove the binding and the
// generated function are gone instead of inferring it from the node removal.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexGraphMigrationRetireComponentBoundCompiledApplyTest,
	"Cortex.Graph.Authoring.Migration.Retire.ComponentBoundCompiledApplyClearsBinding",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexGraphMigrationRetireComponentBoundCompiledApplyTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	using namespace CortexGraphMigrationRetireTest;
	FGuardedEntryFixture Fixture;
	TestTrue(TEXT("guarded component-bound fixture is created"),
		Fixture.Build(TEXT("BP_RetireComponentBoundCompiled")));
	if (!Fixture.IsComplete()) { Fixture.Cleanup(); return false; }

	const FGuid BoundGuid = Fixture.Bound->NodeGuid;
	const FGuid BoundBodyGuid = Fixture.BoundBody->NodeGuid;
	const FName BindingFunction = Fixture.Bound->CustomFunctionName;
	const FString BindingsBefore = CaptureComponentBindings(Fixture.Blueprint);
	const FString WidgetsBefore = CaptureWidgetTree(Fixture.Blueprint);
	TestTrue(TEXT("the fixture registers the component binding before retirement"),
		BindingsBefore.Contains(TEXT("ActionButton|OnClicked"))
			&& BindingsBefore.Contains(BindingFunction.ToString()));

	TArray<FString> Approved;
	TSharedPtr<FJsonObject> Request;
	FCortexCommandResult Error;
	TestTrue(FString::Printf(TEXT("the guarded apply request prepares: %s"), *Error.ErrorMessage),
		PrepareGuardedRequest(Fixture, { BoundGuid.ToString() }, TEXT("00000000-0000-0000-0000-000000112001"),
			Request, Approved, Error, /*bCompile=*/true, /*bSave=*/false));
	if (!Request.IsValid()) { Fixture.Cleanup(); return false; }
	TestTrue(TEXT("the reviewed approval covers the entry and its island body"),
		Approved.Contains(BoundGuid.ToString()) && Approved.Contains(BoundBodyGuid.ToString()));

	FCortexGraphPatchOutcome Outcome;
	Error = FCortexCommandResult();
	TestTrue(FString::Printf(TEXT("the compiled component-bound retirement applies: %s"), *Error.ErrorMessage),
		FCortexGraphPatchOps::Execute(Fixture.Blueprint, Request, Outcome, Error));
	TestEqual(TEXT("the apply reports applied"), Outcome.ApplyStatus, FString(TEXT("applied")));
	TestEqual(TEXT("exactly one target compile ran"), Outcome.TargetCompileCount, 1);
	TestEqual(TEXT("the target compiled"), Outcome.CompileStatus, FString(TEXT("compiled")));
	TestEqual(TEXT("the compiled class-specific readback matched"), Outcome.ReadbackStatus, FString(TEXT("matched")));
	TestEqual(TEXT("no rollback was required"), Outcome.RollbackStatus, FString(TEXT("not_requested")));
	TestNull(TEXT("the retired component-bound node is absent"),
		FCortexGraphMigrationOps::FindNodeByGuid(Fixture.Blueprint, BoundGuid));
	TestNull(TEXT("the retired island body is absent"),
		FCortexGraphMigrationOps::FindNodeByGuid(Fixture.Blueprint, BoundBodyGuid));
	TestNotNull(TEXT("the unrelated cosmetic override survives"),
		FCortexGraphMigrationOps::FindNodeByGuid(Fixture.Blueprint, Fixture.Retained->NodeGuid));
	TestEqual(TEXT("the widget tree is unchanged"), CaptureWidgetTree(Fixture.Blueprint), WidgetsBefore);

	const FString BindingsAfter = CaptureComponentBindings(Fixture.Blueprint);
	TestFalse(TEXT("the retired generated binding function is gone from the compiled binding array"),
		BindingsAfter.Contains(BindingFunction.ToString()));
	TestFalse(TEXT("the ActionButton.OnClicked binding is gone"), BindingsAfter.Contains(TEXT("ActionButton|OnClicked")));
	TestNull(TEXT("the compiled class no longer declares the retired binding function"),
		Fixture.Blueprint->GeneratedClass ? Fixture.Blueprint->GeneratedClass->FindFunctionByName(BindingFunction) : nullptr);

	Fixture.Cleanup();
	return true;
}

// CortexSandbox #112: `compile=false, save=false` stays an honest staging result. The removal and the
// retained graph are verified, but the stale generated binding is still registered and the response
// says so instead of reporting a runtime-safe outcome it cannot prove.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexGraphMigrationRetireComponentBoundStagedApplyTest,
	"Cortex.Graph.Authoring.Migration.Retire.ComponentBoundStagedApplyIsNotRuntimeSafe",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexGraphMigrationRetireComponentBoundStagedApplyTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	using namespace CortexGraphMigrationRetireTest;
	FGuardedEntryFixture Fixture;
	TestTrue(TEXT("guarded component-bound fixture is created"),
		Fixture.Build(TEXT("BP_RetireComponentBoundStaged")));
	if (!Fixture.IsComplete()) { Fixture.Cleanup(); return false; }

	const FGuid BoundGuid = Fixture.Bound->NodeGuid;
	const FName BindingFunction = Fixture.Bound->CustomFunctionName;
	const FString BindingsBefore = CaptureComponentBindings(Fixture.Blueprint);
	TestTrue(TEXT("the fixture registers the component binding before retirement"),
		BindingsBefore.Contains(BindingFunction.ToString()));

	TArray<FString> Approved;
	TSharedPtr<FJsonObject> Request;
	FCortexCommandResult Error;
	TestTrue(FString::Printf(TEXT("the staged apply request prepares: %s"), *Error.ErrorMessage),
		PrepareGuardedRequest(Fixture, { BoundGuid.ToString() }, TEXT("00000000-0000-0000-0000-000000112002"),
			Request, Approved, Error, /*bCompile=*/false, /*bSave=*/false));
	if (!Request.IsValid()) { Fixture.Cleanup(); return false; }

	FCortexGraphPatchOutcome Outcome;
	Error = FCortexCommandResult();
	TestTrue(FString::Printf(TEXT("the staged component-bound retirement applies: %s"), *Error.ErrorMessage),
		FCortexGraphPatchOps::Execute(Fixture.Blueprint, Request, Outcome, Error));
	TestEqual(TEXT("the staged apply reports applied"), Outcome.ApplyStatus, FString(TEXT("applied")));
	TestEqual(TEXT("the staged apply requests no compile"), Outcome.CompileStatus, FString(TEXT("not_requested")));
	TestEqual(TEXT("the staged apply runs no target compile"), Outcome.TargetCompileCount, 0);
	TestEqual(TEXT("the staged native readback matched"), Outcome.ReadbackStatus, FString(TEXT("matched")));
	TestNull(TEXT("the retired component-bound node is absent from the graph"),
		FCortexGraphMigrationOps::FindNodeByGuid(Fixture.Blueprint, BoundGuid));
	TestTrue(TEXT("the stale generated component binding is still registered after a staged apply"),
		CaptureComponentBindings(Fixture.Blueprint).Contains(BindingFunction.ToString()));
	TestNotNull(TEXT("the stale generated binding function still exists after a staged apply"),
		Fixture.Blueprint->GeneratedClass ? Fixture.Blueprint->GeneratedClass->FindFunctionByName(BindingFunction) : nullptr);

	Fixture.Cleanup();
	return true;
}

// CortexSandbox #112: a custom event compiles into a callable generated function, so it is only
// retirable while nothing in the asset references it and the editor cannot invoke it directly.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexGraphMigrationRetireCustomEventRefusalTest,
	"Cortex.Graph.Authoring.Migration.Retire.CustomEventRefusesCallerAndCallInEditor",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexGraphMigrationRetireCustomEventRefusalTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	using namespace CortexGraphMigrationRetireTest;

	{
		FGuardedEntryFixture Fixture;
		TestTrue(TEXT("guarded custom-event fixture is created"),
			Fixture.Build(TEXT("BP_RetireCustomCaller")));
		if (Fixture.IsComplete())
		{
			const FString Selected = Fixture.Custom->NodeGuid.ToString();
			FCortexGraphMigrationRetirePlan PlanValue;
			bool bReused = false;
			FCortexCommandResult Error;
			TestTrue(TEXT("the unreferenced custom event previews before a caller exists"),
				Plan(Fixture, { Selected }, PlanValue, bReused, Error));

			UK2Node_CallFunction* Caller = NewObject<UK2Node_CallFunction>(Fixture.Graph);
			Caller->FunctionReference.SetSelfMember(Fixture.Custom->CustomFunctionName);
			Caller->CreateNewGuid();
			Caller->AllocateDefaultPins();
			Fixture.Graph->AddNode(Caller, true, false);
			Error = FCortexCommandResult();
			TestFalse(TEXT("a custom event with a live in-asset caller is refused"),
				Plan(Fixture, { Selected }, PlanValue, bReused, Error));
			TestTrue(TEXT("the caller refusal names the referencing node"),
				Error.ErrorMessage.Contains(TEXT("still referenced in this asset"))
					&& Error.ErrorMessage.Contains(Caller->NodeGuid.ToString()));
		}
		Fixture.Cleanup();
	}

	{
		FGuardedEntryFixture Fixture;
		TestTrue(TEXT("guarded call-in-editor fixture is created"),
			Fixture.Build(TEXT("BP_RetireCustomCallInEditor")));
		if (Fixture.IsComplete())
		{
			const FString Selected = Fixture.Custom->NodeGuid.ToString();
			FCortexGraphMigrationRetirePlan PlanValue;
			bool bReused = false;
			FCortexCommandResult Error;
			Fixture.Custom->bCallInEditor = true;
			TestFalse(TEXT("a call-in-editor custom event is refused"),
				Plan(Fixture, { Selected }, PlanValue, bReused, Error));
			TestTrue(TEXT("the refusal explains the editor-callable event"),
				Error.ErrorMessage.Contains(TEXT("callable in the editor")));
			Fixture.Custom->bCallInEditor = false;
			Error = FCortexCommandResult();
			TestTrue(TEXT("the same custom event is retirable once the editor-callable flag is cleared"),
				Plan(Fixture, { Selected }, PlanValue, bReused, Error));
		}
		Fixture.Cleanup();
	}
	return true;
}

// CortexSandbox #112: one request may retire a mixed set of guarded entry classes while the unrelated
// cosmetic override, the retained graph links and the designer widget tree stay provably unchanged.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexGraphMigrationRetireMixedGuardedApplyTest,
	"Cortex.Graph.Authoring.Migration.Retire.MixedGuardedApplyPreservesRetainedState",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexGraphMigrationRetireMixedGuardedApplyTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	using namespace CortexGraphMigrationRetireTest;
	FGuardedEntryFixture Fixture;
	TestTrue(TEXT("guarded mixed fixture is created"), Fixture.Build(TEXT("BP_RetireMixedGuarded")));
	if (!Fixture.IsComplete()) { Fixture.Cleanup(); return false; }

	const FName BindingFunction = Fixture.Bound->CustomFunctionName;
	const FName CustomFunction = Fixture.Custom->CustomFunctionName;
	const FGuid RetainedGuid = Fixture.Retained->NodeGuid;
	const FGuid RetainedBodyGuid = Fixture.RetainedBody->NodeGuid;
	const TArray<FGuid> RemovedGuids = {
		Fixture.Bound->NodeGuid, Fixture.BoundBody->NodeGuid,
		Fixture.Construct->NodeGuid, Fixture.ConstructBody->NodeGuid,
		Fixture.Initialized->NodeGuid, Fixture.InitializedBody->NodeGuid,
		Fixture.Custom->NodeGuid, Fixture.CustomBody->NodeGuid };
	const FString WidgetsBefore = CaptureWidgetTree(Fixture.Blueprint);
	TSet<FGuid> RetainedSet = { RetainedGuid, RetainedBodyGuid };
	const FString RetainedBefore = CaptureNativeGraph(Fixture.Graph, &RetainedSet);

	const TArray<FString> Selected = {
		Fixture.Bound->NodeGuid.ToString(), Fixture.Construct->NodeGuid.ToString(),
		Fixture.Initialized->NodeGuid.ToString(), Fixture.Custom->NodeGuid.ToString() };

	FCortexGraphMigrationRetirePlan PlanValue;
	bool bReused = false;
	FCortexCommandResult Error;
	TestTrue(FString::Printf(TEXT("the mixed guarded preview succeeds: %s"), *Error.ErrorMessage),
		Plan(Fixture, Selected, PlanValue, bReused, Error));
	TestEqual(TEXT("the plan publishes one identity per selected entry"), PlanValue.Entries.Num(), 4);
	TSet<FString> Kinds;
	for (const FCortexGraphMigrationRetireEntry& Entry : PlanValue.Entries) Kinds.Add(Entry.Kind);
	TestTrue(TEXT("the plan publishes each distinct guarded entry class"),
		Kinds.Contains(FString(TEXT("component_bound_event"))) && Kinds.Contains(FString(TEXT("lifecycle_event")))
			&& Kinds.Contains(FString(TEXT("custom_event"))) && Kinds.Num() == 3);
	TestTrue(TEXT("the mixed plan requires a compile"), PlanValue.bRequiresCompile);

	TArray<FString> Approved;
	TSharedPtr<FJsonObject> Request;
	Error = FCortexCommandResult();
	TestTrue(FString::Printf(TEXT("the mixed guarded apply request prepares: %s"), *Error.ErrorMessage),
		PrepareGuardedRequest(Fixture, Selected, TEXT("00000000-0000-0000-0000-000000112003"),
			Request, Approved, Error, /*bCompile=*/true, /*bSave=*/false));
	if (!Request.IsValid()) { Fixture.Cleanup(); return false; }

	FCortexGraphPatchOutcome Outcome;
	Error = FCortexCommandResult();
	TestTrue(FString::Printf(TEXT("the mixed guarded retirement applies: %s"), *Error.ErrorMessage),
		FCortexGraphPatchOps::Execute(Fixture.Blueprint, Request, Outcome, Error));
	TestEqual(TEXT("the mixed apply reports applied"), Outcome.ApplyStatus, FString(TEXT("applied")));
	TestEqual(TEXT("the mixed apply compiled once"), Outcome.TargetCompileCount, 1);
	TestEqual(TEXT("the mixed compiled readback matched"), Outcome.ReadbackStatus, FString(TEXT("matched")));
	for (const FGuid& RemovedGuid : RemovedGuids)
	{
		TestNull(FString::Printf(TEXT("retired node %s is absent"), *RemovedGuid.ToString()),
			FCortexGraphMigrationOps::FindNodeByGuid(Fixture.Blueprint, RemovedGuid));
	}
	TestNotNull(TEXT("the cosmetic override survives"), FCortexGraphMigrationOps::FindNodeByGuid(Fixture.Blueprint, RetainedGuid));
	TestNotNull(TEXT("the cosmetic override body survives"), FCortexGraphMigrationOps::FindNodeByGuid(Fixture.Blueprint, RetainedBodyGuid));
	TestEqual(TEXT("the retained cosmetic island is unchanged"), CaptureNativeGraph(Fixture.Graph, &RetainedSet), RetainedBefore);
	TestEqual(TEXT("the designer widget tree is unchanged"), CaptureWidgetTree(Fixture.Blueprint), WidgetsBefore);
	TestFalse(TEXT("the retired component binding is gone from the compiled class"),
		CaptureComponentBindings(Fixture.Blueprint).Contains(BindingFunction.ToString()));
	TestNull(TEXT("the retired custom event function is gone from the compiled class"),
		Fixture.Blueprint->GeneratedClass ? Fixture.Blueprint->GeneratedClass->FindFunctionByName(CustomFunction) : nullptr);

	Fixture.Cleanup();
	return true;
}

// CortexSandbox #112: the removal journal is the recovery contract for the class-specific identity a
// component-bound node carries, so an apply fault must restore the binding identity itself instead of
// relying on an authoring fingerprint that cannot see it.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexGraphMigrationRetireComponentBoundApplyFaultTest,
	"Cortex.Graph.Authoring.Migration.Retire.ComponentBoundApplyFaultRestoresIdentity",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexGraphMigrationRetireComponentBoundApplyFaultTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	using namespace CortexGraphMigrationRetireTest;
	FGuardedEntryFixture Fixture;
	TestTrue(TEXT("guarded component-bound fixture is created"),
		Fixture.Build(TEXT("BP_RetireComponentBoundApplyFault")));
	if (!Fixture.IsComplete()) { Fixture.Cleanup(); return false; }

	const FGuid BoundGuid = Fixture.Bound->NodeGuid;
	const FName BindingFunction = Fixture.Bound->CustomFunctionName;
	const FName BoundComponent = Fixture.Bound->ComponentPropertyName;
	const FName BoundDelegate = Fixture.Bound->DelegatePropertyName;
	UClass* BoundDelegateOwner = Fixture.Bound->DelegateOwnerClass;
	const FString GraphBefore = CaptureNativeGraph(Fixture.Graph);
	const FString BindingsBefore = CaptureComponentBindings(Fixture.Blueprint);

	TArray<FString> Approved;
	TSharedPtr<FJsonObject> Request;
	FCortexCommandResult Error;
	TestTrue(FString::Printf(TEXT("the guarded apply request prepares: %s"), *Error.ErrorMessage),
		PrepareGuardedRequest(Fixture, { BoundGuid.ToString() }, TEXT("00000000-0000-0000-0000-000000112004"),
			Request, Approved, Error, /*bCompile=*/false, /*bSave=*/false));
	if (!Request.IsValid()) { Fixture.Cleanup(); return false; }

	FCortexGraphPatchOps::SetApplyFaultPointForTesting(TEXT("migration_retire_after_first_removal"));
	FCortexGraphPatchOutcome Outcome;
	Error = FCortexCommandResult();
	TestFalse(TEXT("the injected apply fault fails the retirement"),
		FCortexGraphPatchOps::Execute(Fixture.Blueprint, Request, Outcome, Error));
	FCortexGraphPatchOps::SetApplyFaultPointForTesting(NAME_None);
	TestEqual(TEXT("the journal restored the transaction"), Outcome.RollbackStatus, FString(TEXT("restored")));
	TestFalse(TEXT("the restored asset is not blocked"), Outcome.bBlocked);
	UK2Node_ComponentBoundEvent* Restored = Cast<UK2Node_ComponentBoundEvent>(
		FCortexGraphMigrationOps::FindNodeByGuid(Fixture.Blueprint, BoundGuid));
	TestNotNull(TEXT("the retired component-bound node was restored"), Restored);
	if (Restored)
	{
		TestEqual(TEXT("the restored node keeps its component property"), Restored->ComponentPropertyName, BoundComponent);
		TestEqual(TEXT("the restored node keeps its delegate property"), Restored->DelegatePropertyName, BoundDelegate);
		TestEqual(TEXT("the restored node keeps its delegate owner"), Restored->DelegateOwnerClass.Get(), BoundDelegateOwner);
		TestEqual(TEXT("the restored node keeps its generated binding function"), Restored->CustomFunctionName, BindingFunction);
	}
	TestEqual(TEXT("the restored graph matches the pre-request snapshot"), CaptureNativeGraph(Fixture.Graph), GraphBefore);
	TestEqual(TEXT("the generated component binding is untouched by the rolled-back apply"),
		CaptureComponentBindings(Fixture.Blueprint), BindingsBefore);

	Fixture.Cleanup();
	return true;
}

// CortexSandbox #112: a compiled apply whose class-specific readback fails must roll back the removed
// component binding as well, so the new generated-artefact comparison seam is proven recoverable.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexGraphMigrationRetireComponentBoundReadbackFaultTest,
	"Cortex.Graph.Authoring.Migration.Retire.ComponentBoundReadbackFaultRollsBackBinding",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexGraphMigrationRetireComponentBoundReadbackFaultTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	using namespace CortexGraphMigrationRetireTest;
	FGuardedEntryFixture Fixture;
	TestTrue(TEXT("guarded component-bound fixture is created"),
		Fixture.Build(TEXT("BP_RetireComponentBoundReadbackFault")));
	if (!Fixture.IsComplete()) { Fixture.Cleanup(); return false; }

	const FGuid BoundGuid = Fixture.Bound->NodeGuid;
	const FName BindingFunction = Fixture.Bound->CustomFunctionName;
	const FString BindingsBefore = CaptureComponentBindings(Fixture.Blueprint);

	TArray<FString> Approved;
	TSharedPtr<FJsonObject> Request;
	FCortexCommandResult Error;
	TestTrue(FString::Printf(TEXT("the guarded apply request prepares: %s"), *Error.ErrorMessage),
		PrepareGuardedRequest(Fixture, { BoundGuid.ToString() }, TEXT("00000000-0000-0000-0000-000000112005"),
			Request, Approved, Error, /*bCompile=*/true, /*bSave=*/false));
	if (!Request.IsValid()) { Fixture.Cleanup(); return false; }

	FCortexGraphMigrationOps::SetRetirementReadbackFaultForTesting(TEXT("retire_after_binding"));
	FCortexGraphPatchOutcome Outcome;
	Error = FCortexCommandResult();
	TestFalse(TEXT("the injected class-specific readback fault fails the retirement"),
		FCortexGraphPatchOps::Execute(Fixture.Blueprint, Request, Outcome, Error));
	FCortexGraphMigrationOps::ClearRetirementReadbackFaultForTesting();
	TestEqual(TEXT("the class-specific readback reported a mismatch"), Outcome.ReadbackStatus, FString(TEXT("mismatched")));
	TestEqual(TEXT("the journal and the recovery compile restored the transaction"),
		Outcome.RollbackStatus, FString(TEXT("restored")));
	TestFalse(TEXT("the restored asset is not blocked"), Outcome.bBlocked);
	UK2Node_ComponentBoundEvent* Restored = Cast<UK2Node_ComponentBoundEvent>(
		FCortexGraphMigrationOps::FindNodeByGuid(Fixture.Blueprint, BoundGuid));
	TestNotNull(TEXT("the compiled retirement was rolled back to the component-bound node"), Restored);
	if (Restored)
	{
		TestEqual(TEXT("the restored node keeps its generated binding function"), Restored->CustomFunctionName, BindingFunction);
	}
	TestTrue(TEXT("the recovery compile registered the component binding again"),
		CaptureComponentBindings(Fixture.Blueprint).Contains(BindingFunction.ToString()));
	TestEqual(TEXT("the rolled-back compiled retirement restores the exact component binding set"),
		CaptureComponentBindings(Fixture.Blueprint), BindingsBefore);
	TestNotNull(TEXT("the recovery compile re-declares the binding function"),
		Fixture.Blueprint->GeneratedClass ? Fixture.Blueprint->GeneratedClass->FindFunctionByName(BindingFunction) : nullptr);

	Fixture.Cleanup();
	return true;
}

// CortexSandbox #112: a plain `UUserWidget` parent is no proof that native behaviour replaced a
// retired lifecycle body, so the lifecycle class is refused for it even though the node shape is a
// legitimate inherited override.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexGraphMigrationRetireLifecyclePlainParentTest,
	"Cortex.Graph.Authoring.Migration.Retire.LifecycleEntryRefusesPlainUserWidgetParent",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexGraphMigrationRetireLifecyclePlainParentTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	using namespace CortexGraphMigrationRetireTest;
	FGuardedEntryFixture Fixture;
	TestTrue(TEXT("guarded lifecycle fixture is created"), Fixture.Build(TEXT("BP_RetireLifecyclePlainParent")));
	if (!Fixture.IsComplete()) { Fixture.Cleanup(); return false; }

	const TArray<FString> Selected = {
		Fixture.Construct->NodeGuid.ToString(), Fixture.Initialized->NodeGuid.ToString() };
	FCortexGraphMigrationRetirePlan PlanValue;
	bool bReused = false;
	FCortexCommandResult Error;
	TestTrue(TEXT("the natively reparented lifecycle entries preview before the parent changes"),
		Plan(Fixture, Selected, PlanValue, bReused, Error));

	UClass* const NativeParent = Fixture.Blueprint->ParentClass;
	Fixture.Blueprint->ParentClass = UUserWidget::StaticClass();
	Error = FCortexCommandResult();
	TestFalse(TEXT("a plain UUserWidget parent refuses the lifecycle entries"),
		Plan(Fixture, Selected, PlanValue, bReused, Error));
	TestTrue(TEXT("the refusal explains that the parent is not a native Widget class"),
		Error.ErrorMessage.Contains(TEXT("is not a native Widget class that owns the lifecycle hook")));
	Fixture.Blueprint->ParentClass = NativeParent;

	// The native parent is the only difference, so the identical request is accepted again.
	Error = FCortexCommandResult();
	TestTrue(TEXT("the native parent accepts the same lifecycle entries again"),
		Plan(Fixture, Selected, PlanValue, bReused, Error));

	Fixture.Cleanup();
	return true;
}

// CortexSandbox #112: `UK2Node_CustomEvent` compiles into a BlueprintCallable/Public generated
// function, so a caller in another loaded Blueprint is a real call site. A retirement that can see
// such a caller must refuse it instead of deleting the entry and severing the cross-asset call.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexGraphMigrationRetireCustomEventExternalCallerTest,
	"Cortex.Graph.Authoring.Migration.Retire.CustomEventRefusesExternalCaller",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexGraphMigrationRetireCustomEventExternalCallerTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	using namespace CortexGraphMigrationRetireTest;
	FGuardedEntryFixture Fixture;
	TestTrue(TEXT("guarded custom-event fixture is created"),
		Fixture.Build(TEXT("BP_RetireExternalCallerTarget")));
	if (!Fixture.IsComplete()) { Fixture.Cleanup(); return false; }

	const FString Selected = Fixture.Custom->NodeGuid.ToString();
	FCortexGraphMigrationRetirePlan PlanValue;
	bool bReused = false;
	FCortexCommandResult Error;
	TestTrue(TEXT("the custom event is retirable while no external caller exists"),
		Plan(Fixture, { Selected }, PlanValue, bReused, Error));
	TestTrue(TEXT("the unreferenced custom event island is approved for removal"),
		PlanValue.RemovableGuids.Contains(Selected));

	const FString TargetFilename = Fixture.Filename();
	TestTrue(TEXT("the target asset is saved before the external caller exists"), Fixture.SaveToDisk());
	const TArray<uint8> TargetBytesBefore = ReadBytes(TargetFilename);

	FExternalCallerFixture External;
	TestTrue(TEXT("a second Widget Blueprint calls the generated custom event function"),
		External.Build(TEXT("BP_RetireExternalCallerHost"), Fixture.Blueprint, Fixture.Custom->CustomFunctionName));
	const FString ExternalFilename = External.Filename();
	if (!External.Caller)
	{
		External.Cleanup();
		Fixture.Cleanup();
		return false;
	}
	TestTrue(TEXT("the external caller asset is saved"), External.SaveToDisk());
	const TArray<uint8> ExternalBytesBefore = ReadBytes(ExternalFilename);

	// Every oracle is captured with the external caller already loaded and saved, so only the refused
	// retirement could still change any of them.
	const FString GraphBefore = CaptureNativeGraph(Fixture.Graph);
	const FString WidgetsBefore = CaptureWidgetTree(Fixture.Blueprint);
	const FString BindingsBefore = CaptureComponentBindings(Fixture.Blueprint);
	const bool bDirtyBefore = Fixture.Package->IsDirty();

	Error = FCortexCommandResult();
	TestFalse(TEXT("preview refuses to retire a custom event with a live external caller"),
		Plan(Fixture, { Selected }, PlanValue, bReused, Error));
	TestEqual(TEXT("the external caller refusal is INVALID_OPERATION"),
		Error.ErrorCode, FString(CortexErrorCodes::InvalidOperation));
	TestTrue(TEXT("the external caller refusal is diagnostic"), !Error.ErrorMessage.IsEmpty());

	Error = FCortexCommandResult();
	FCortexGraphPreparedPatch Preview;
	TestFalse(TEXT("the patch preview refuses the external caller before any mutation"),
		PreviewRetirement(Fixture, { Selected }, TEXT("00000000-0000-0000-0000-000000112101"), Preview, Error));
	TestEqual(TEXT("the refused patch preview reports INVALID_OPERATION"),
		Error.ErrorCode, FString(CortexErrorCodes::InvalidOperation));

	TestEqual(TEXT("the refused preview leaves the target graph unchanged"),
		CaptureNativeGraph(Fixture.Graph), GraphBefore);
	TestEqual(TEXT("the refused preview leaves the designer widget tree unchanged"),
		CaptureWidgetTree(Fixture.Blueprint), WidgetsBefore);
	TestEqual(TEXT("the refused preview leaves the compiled component bindings unchanged"),
		CaptureComponentBindings(Fixture.Blueprint), BindingsBefore);
	TestEqual(TEXT("the refused preview leaves the dirty state unchanged"),
		Fixture.Package->IsDirty(), bDirtyBefore);
	TestNotNull(TEXT("the live external caller is still present in the second asset"),
		FCortexGraphMigrationOps::FindNodeByGuid(External.Blueprint, External.Caller->NodeGuid));
	TestTrue(TEXT("the target asset bytes are unchanged"), SameBytes(TargetBytesBefore, ReadBytes(TargetFilename)));
	TestTrue(TEXT("the external caller asset bytes are unchanged"),
		SameBytes(ExternalBytesBefore, ReadBytes(ExternalFilename)));

	External.Cleanup();
	Fixture.Cleanup();
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexGraphMigrationRetireUnresolvedCallerTest,
	"Cortex.Graph.Authoring.Migration.Retire.UnresolvedCallerRefusesApproval",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexGraphMigrationRetireUnresolvedCallerTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	using namespace CortexGraphMigrationRetireTest;
	FGuardedEntryFixture Fixture;
	if (!TestTrue(TEXT("target fixture is created"), Fixture.Build(TEXT("BP_RetireUnresolvedCallerTarget"))))
	{
		Fixture.Cleanup();
		return false;
	}
	FExternalCallerFixture External;
	if (!TestTrue(TEXT("foreign caller is created"), External.Build(TEXT("BP_RetireUnresolvedCallerHost"),
		Fixture.Blueprint, Fixture.Custom->CustomFunctionName)))
	{
		External.Cleanup();
		Fixture.Cleanup();
		return false;
	}
	const FString Selected = Fixture.Custom->NodeGuid.ToString();
	const FString Before = CaptureNativeGraph(Fixture.Graph);
	FCortexGraphMigrationRetirePlan PlanValue;
	FCortexCommandResult Error;
	bool bReused = false;
	External.Caller->FunctionReference.SetDirect(Fixture.Custom->CustomFunctionName, FGuid(), nullptr, false);
	TestFalse(TEXT("a matching foreign call with an unresolved owner cannot authorize retirement"),
		Plan(Fixture, { Selected }, PlanValue, bReused, Error));
	TestTrue(TEXT("unresolved call ownership reports incomplete caller evidence"),
		Error.ErrorMessage.Contains(TEXT("cannot be ruled out")));
	External.Caller->FunctionReference.SetDirect(TEXT("UnrelatedFunction"), FGuid(), nullptr, false);

	UK2Node_CreateDelegate* Delegate = NewObject<UK2Node_CreateDelegate>(External.Graph, NAME_None, RF_Transactional);
	Delegate->CreateNewGuid();
	Delegate->AllocateDefaultPins();
	Delegate->SetFunction(Fixture.Custom->CustomFunctionName);
	External.Graph->AddNode(Delegate, true, false);
	UEdGraphPin* SelfPin = Delegate->FindPin(UEdGraphSchema_K2::PN_Self);
	UEdGraphPin* ExecPin = External.Caller->FindPin(TEXT("then"));
	UEdGraphPin* OtherExecPin = External.Caller->FindPin(TEXT("execute"));
	if (!SelfPin || !ExecPin || !OtherExecPin)
	{
		AddError(TEXT("delegate scope fixture pins are missing"));
		External.Cleanup();
		Fixture.Cleanup();
		return false;
	}
	SelfPin->PinName = TEXT("MissingSelf");
	TestFalse(TEXT("a matching delegate without a self pin cannot authorize retirement"),
		Plan(Fixture, { Selected }, PlanValue, bReused, Error));
	SelfPin->PinName = UEdGraphSchema_K2::PN_Self;
	SelfPin->MakeLinkTo(ExecPin);
	TestFalse(TEXT("a matching delegate with a non-object scope cannot authorize retirement"),
		Plan(Fixture, { Selected }, PlanValue, bReused, Error));
	SelfPin->MakeLinkTo(OtherExecPin);
	TestFalse(TEXT("a matching delegate with multiple scopes cannot authorize retirement"),
		Plan(Fixture, { Selected }, PlanValue, bReused, Error));
	SelfPin->BreakAllPinLinks();
	Delegate->SetFunction(TEXT("UnrelatedFunction"));
	TestTrue(TEXT("unresolved references to unrelated names do not block retirement"),
		Plan(Fixture, { Selected }, PlanValue, bReused, Error));
	TestEqual(TEXT("caller investigation never mutates the target"), CaptureNativeGraph(Fixture.Graph), Before);
	External.Cleanup();
	Fixture.Cleanup();
	return true;
}

// CortexSandbox #112 second review: the referencer guard keys on `FindPackage`, so a referencer
// whose package is resident but whose Blueprint asset is not loaded was silently treated as "no
// caller" — the loaded-Blueprint scan cannot see an unloaded asset. Root docs require fail-closed
// on an uninspectable referencer, and a preview must never auto-load one to inspect it.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexGraphMigrationRetireResidentReferencerTest,
	"Cortex.Graph.Authoring.Migration.Retire.RefusesResidentReferencerWithoutLoadedAsset",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexGraphMigrationRetireResidentReferencerTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	using namespace CortexGraphMigrationRetireTest;
	FGuardedEntryFixture Fixture;
	TestTrue(TEXT("guarded custom-event fixture is created"),
		Fixture.Build(TEXT("BP_RetireResidentReferencerTarget")));
	if (!Fixture.IsComplete()) { Fixture.Cleanup(); return false; }

	const FString Selected = Fixture.Custom->NodeGuid.ToString();
	const FString TargetFilename = Fixture.Filename();
	TestTrue(TEXT("the target asset is saved before the referencer exists"), Fixture.SaveToDisk());

	FExternalCallerFixture External;
	const bool bReferencerBuilt = External.Build(TEXT("BP_RetireResidentReferencerHost"),
		Fixture.Blueprint, Fixture.Custom->CustomFunctionName);
	TestTrue(TEXT("a second Widget Blueprint calls the target's generated custom event function"), bReferencerBuilt);
	const FString ExternalFilename = External.Filename();
	const FString ExternalPackageName = External.PackageName();
	const FString ExternalObjectPath = External.ObjectPath();
	if (!bReferencerBuilt)
	{
		External.Cleanup();
		Fixture.Cleanup();
		return false;
	}
	TestTrue(TEXT("the referencer asset is saved"), External.SaveToDisk());
	const TArray<uint8> TargetBytesBefore = ReadBytes(TargetFilename);
	const TArray<uint8> ExternalBytesBefore = ReadBytes(ExternalFilename);

	// The loophole state: the referencer's package stays resident while its Blueprint asset is
	// unloaded, which `FindPackage` alone cannot tell apart from an inspectable referencer.
	TestTrue(TEXT("the referencer package stays resident while its Blueprint asset is unloaded"),
		External.UnloadAssetKeepingPackageResident());
	TestNotNull(TEXT("the referencer package is resident at preview time"), FindPackage(nullptr, *ExternalPackageName));
	TestNull(TEXT("the referencer Blueprint asset is not loaded at preview time"),
		FindObject<UBlueprint>(nullptr, *ExternalObjectPath));
	TestTrue(TEXT("the referencer package still exists on disk"), FPackageName::DoesPackageExist(ExternalPackageName));

	FString RegistryFailure;
	const bool bRegistryKnowsReferencer = ScanSavedReferencers({ TargetFilename, ExternalFilename },
		Fixture.Package->GetFName(), FName(*ExternalPackageName), RegistryFailure);
	TestTrue(FString::Printf(TEXT("fixture precondition: the Asset Registry reports the saved referencer: %s"),
		*RegistryFailure), bRegistryKnowsReferencer);
	if (!bRegistryKnowsReferencer)
	{
		External.Cleanup();
		Fixture.Cleanup();
		return false;
	}

	// Only a refused preview can still change any oracle captured below.
	const FString GraphBefore = CaptureNativeGraph(Fixture.Graph);
	const FString WidgetsBefore = CaptureWidgetTree(Fixture.Blueprint);
	const FString BindingsBefore = CaptureComponentBindings(Fixture.Blueprint);
	UPackage* const ResidentReferencerBefore = FindPackage(nullptr, *ExternalPackageName);
	const bool bTargetDirtyBefore = Fixture.Package->IsDirty();
	const bool bReferencerDirtyBefore = ResidentReferencerBefore && ResidentReferencerBefore->IsDirty();

	FOperations Operations;
	Operations.Begin();
	FCortexCommandResult Error;
	FCortexGraphMigrationRetirePlan PlanValue;
	bool bReused = false;
	const bool bPlanned = Plan(Fixture, { Selected }, PlanValue, bReused, Error);
	FCortexGraphPreparedPatch Preview;
	Error = FCortexCommandResult();
	const bool bPreviewed = PreviewRetirement(Fixture, { Selected },
		TEXT("00000000-0000-0000-0000-000000112201"), Preview, Error);
	Operations.End();

	TestFalse(TEXT("the preview refuses an uninspectable resident referencer instead of assuming there is no caller"),
		bPlanned);
	TestEqual(TEXT("the uninspectable referencer refusal is INVALID_OPERATION"),
		Error.ErrorCode, FString(CortexErrorCodes::InvalidOperation));
	TestTrue(TEXT("the uninspectable referencer refusal names the referencer and why it could not be inspected"),
		ReferencerRefusalIsDiagnostic(Error, ExternalPackageName));
	TestFalse(TEXT("the selected custom event is not approved for removal while the referencer is uninspectable"),
		PlanValue.RemovableGuids.Contains(Selected));
	TestFalse(TEXT("the patch preview refuses the uninspectable resident referencer before any mutation"), bPreviewed);
	TestEqual(TEXT("the refused patch preview reports INVALID_OPERATION"),
		Error.ErrorCode, FString(CortexErrorCodes::InvalidOperation));

	TestNull(TEXT("the refused preview does not load the referencer Blueprint asset"),
		FindObject<UBlueprint>(nullptr, *ExternalObjectPath));
	UPackage* const ResidentReferencerAfter = FindPackage(nullptr, *ExternalPackageName);
	TestNotNull(TEXT("the referencer package stays resident after the refused preview"), ResidentReferencerAfter);
	TestEqual(TEXT("the refused preview leaves the referencer package dirty state unchanged"),
		ResidentReferencerAfter && ResidentReferencerAfter->IsDirty(), bReferencerDirtyBefore);
	TestEqual(TEXT("the refused preview leaves the target graph unchanged"),
		CaptureNativeGraph(Fixture.Graph), GraphBefore);
	TestEqual(TEXT("the refused preview leaves the designer widget tree unchanged"),
		CaptureWidgetTree(Fixture.Blueprint), WidgetsBefore);
	TestEqual(TEXT("the refused preview leaves the compiled component bindings unchanged"),
		CaptureComponentBindings(Fixture.Blueprint), BindingsBefore);
	TestEqual(TEXT("the refused preview leaves the target package dirty state unchanged"),
		Fixture.Package->IsDirty(), bTargetDirtyBefore);
	TestEqual(TEXT("the refused preview runs no editor Blueprint compilation"), Operations.EditorCompiles, 0);
	TestEqual(TEXT("the refused preview requests no target compile"), Operations.TargetCompiles, 0);
	TestEqual(TEXT("the refused preview requests no recovery compile"), Operations.RecoveryCompiles, 0);
	TestEqual(TEXT("the refused preview saves nothing"), Operations.Saves, 0);
	TestTrue(TEXT("the target asset bytes are unchanged"), SameBytes(TargetBytesBefore, ReadBytes(TargetFilename)));
	TestTrue(TEXT("the referencer asset bytes are unchanged"),
		SameBytes(ExternalBytesBefore, ReadBytes(ExternalFilename)));

	External.Cleanup();
	Fixture.Cleanup();
	PurgeReferencerPackage(ExternalPackageName);
	return true;
}

// CortexSandbox #112 second review: a referencer package that is not resident when the preview runs
// was auto-loaded so its graphs could be scanned, and UE 5.8 can compile that Blueprint as part of
// the load. A dry run must never add packages to memory to answer the caller question: the
// referencer is uninspectable without loading it, so the preview refuses and leaves it alone.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexGraphMigrationRetireNonresidentReferencerTest,
	"Cortex.Graph.Authoring.Migration.Retire.RefusesNonresidentReferencerWithoutLoadingIt",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexGraphMigrationRetireNonresidentReferencerTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	using namespace CortexGraphMigrationRetireTest;
	FGuardedEntryFixture Fixture;
	TestTrue(TEXT("guarded custom-event fixture is created"),
		Fixture.Build(TEXT("BP_RetireNonresidentReferencerTarget")));
	if (!Fixture.IsComplete()) { Fixture.Cleanup(); return false; }

	const FString Selected = Fixture.Custom->NodeGuid.ToString();
	const FString TargetFilename = Fixture.Filename();
	TestTrue(TEXT("the target asset is saved before the referencer exists"), Fixture.SaveToDisk());

	FExternalCallerFixture External;
	const bool bReferencerBuilt = External.Build(TEXT("BP_RetireNonresidentReferencerHost"),
		Fixture.Blueprint, Fixture.Custom->CustomFunctionName);
	TestTrue(TEXT("a second Widget Blueprint calls the target's generated custom event function"), bReferencerBuilt);
	const FString ExternalFilename = External.Filename();
	const FString ExternalPackageName = External.PackageName();
	const FString ExternalObjectPath = External.ObjectPath();
	if (!bReferencerBuilt)
	{
		External.Cleanup();
		Fixture.Cleanup();
		return false;
	}
	TestTrue(TEXT("the referencer asset is saved"), External.SaveToDisk());
	const TArray<uint8> TargetBytesBefore = ReadBytes(TargetFilename);
	const TArray<uint8> ExternalBytesBefore = ReadBytes(ExternalFilename);

	// The nonresident state: only the referencer's bytes exist, so any inspection would have to load it.
	TestTrue(TEXT("the referencer is fully unloaded, leaving only its bytes on disk"), External.UnloadCompletely());
	TestNull(TEXT("the referencer package is not resident at preview time"), FindPackage(nullptr, *ExternalPackageName));
	TestTrue(TEXT("the referencer package still exists on disk"), FPackageName::DoesPackageExist(ExternalPackageName));

	FString RegistryFailure;
	const bool bRegistryKnowsReferencer = ScanSavedReferencers({ TargetFilename, ExternalFilename },
		Fixture.Package->GetFName(), FName(*ExternalPackageName), RegistryFailure);
	TestTrue(FString::Printf(TEXT("fixture precondition: the Asset Registry reports the saved referencer: %s"),
		*RegistryFailure), bRegistryKnowsReferencer);
	if (!bRegistryKnowsReferencer)
	{
		Fixture.Cleanup();
		return false;
	}

	// Only a refused preview can still change any oracle captured below.
	const FString GraphBefore = CaptureNativeGraph(Fixture.Graph);
	const FString WidgetsBefore = CaptureWidgetTree(Fixture.Blueprint);
	const FString BindingsBefore = CaptureComponentBindings(Fixture.Blueprint);
	const bool bTargetDirtyBefore = Fixture.Package->IsDirty();

	FOperations Operations;
	Operations.Begin();
	FCortexCommandResult Error;
	FCortexGraphMigrationRetirePlan PlanValue;
	bool bReused = false;
	const bool bPlanned = Plan(Fixture, { Selected }, PlanValue, bReused, Error);
	FCortexGraphPreparedPatch Preview;
	Error = FCortexCommandResult();
	const bool bPreviewed = PreviewRetirement(Fixture, { Selected },
		TEXT("00000000-0000-0000-0000-000000112202"), Preview, Error);
	Operations.End();

	TestFalse(TEXT("the preview refuses a saved nonresident referencer instead of loading it"), bPlanned);
	TestEqual(TEXT("the nonresident referencer refusal is INVALID_OPERATION"),
		Error.ErrorCode, FString(CortexErrorCodes::InvalidOperation));
	TestTrue(TEXT("the nonresident referencer refusal names the referencer and why it could not be inspected"),
		ReferencerRefusalIsDiagnostic(Error, ExternalPackageName));
	TestFalse(TEXT("the selected custom event is not approved for removal while the referencer is not inspectable"),
		PlanValue.RemovableGuids.Contains(Selected));
	TestFalse(TEXT("the patch preview refuses the nonresident referencer before any mutation"), bPreviewed);
	TestEqual(TEXT("the refused patch preview reports INVALID_OPERATION"),
		Error.ErrorCode, FString(CortexErrorCodes::InvalidOperation));

	TestNull(TEXT("the refused preview does not load the referencer package"),
		FindPackage(nullptr, *ExternalPackageName));
	TestNull(TEXT("the refused preview does not load the referencer Blueprint asset"),
		FindObject<UBlueprint>(nullptr, *ExternalObjectPath));
	TestEqual(TEXT("the refused preview leaves the target graph unchanged"),
		CaptureNativeGraph(Fixture.Graph), GraphBefore);
	TestEqual(TEXT("the refused preview leaves the designer widget tree unchanged"),
		CaptureWidgetTree(Fixture.Blueprint), WidgetsBefore);
	TestEqual(TEXT("the refused preview leaves the compiled component bindings unchanged"),
		CaptureComponentBindings(Fixture.Blueprint), BindingsBefore);
	TestEqual(TEXT("the refused preview leaves the target package dirty state unchanged"),
		Fixture.Package->IsDirty(), bTargetDirtyBefore);
	TestEqual(TEXT("the refused preview runs no editor Blueprint compilation"), Operations.EditorCompiles, 0);
	TestEqual(TEXT("the refused preview requests no target compile"), Operations.TargetCompiles, 0);
	TestEqual(TEXT("the refused preview requests no recovery compile"), Operations.RecoveryCompiles, 0);
	TestEqual(TEXT("the refused preview saves nothing"), Operations.Saves, 0);
	TestTrue(TEXT("the target asset bytes are unchanged"), SameBytes(TargetBytesBefore, ReadBytes(TargetFilename)));
	TestTrue(TEXT("the referencer asset bytes are unchanged"),
		SameBytes(ExternalBytesBefore, ReadBytes(ExternalFilename)));

	External.Cleanup();
	Fixture.Cleanup();
	PurgeReferencerPackage(ExternalPackageName);
	return true;
}

// CortexSandbox #112: a component-bound entry names a component property and a delegate owner, so
// the two must describe the same binding. A property whose compiled class cannot host the named
// delegate is not a live binding at all; the preview must refuse it instead of authorizing removal.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexGraphMigrationRetireComponentBoundMismatchTest,
	"Cortex.Graph.Authoring.Migration.Retire.ComponentBoundRefusesIncompatibleDelegateOwner",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexGraphMigrationRetireComponentBoundMismatchTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	using namespace CortexGraphMigrationRetireTest;
	FGuardedEntryFixture Fixture;
	TestTrue(TEXT("guarded fixture with an unrelated compiled widget variable is created"),
		Fixture.Build(TEXT("BP_RetireComponentBoundMismatch"), true));
	if (!Fixture.IsComplete() || !Fixture.IncompatibleComponentProperty) { Fixture.Cleanup(); return false; }

	const FString Filename = Fixture.Filename();
	TestTrue(TEXT("the fixture asset is saved before the mismatched binding exists"), Fixture.SaveToDisk());
	const TArray<uint8> DiskBefore = ReadBytes(Filename);

	UK2Node_ComponentBoundEvent* Mismatched =
		NewObject<UK2Node_ComponentBoundEvent>(Fixture.Graph, NAME_None, RF_Transactional);
	Mismatched->CreateNewGuid();
	Mismatched->InitializeComponentBoundEventParams(Fixture.IncompatibleComponentProperty, Fixture.ClickedDelegate);
	Fixture.Graph->AddNode(Mismatched, true, false);
	Mismatched->AllocateDefaultPins();
	UK2Node_CallFunction* MismatchedBody = Fixture.AddBody(Mismatched);
	TestFalse(TEXT("the compiled component property class cannot host the bound native delegate"),
		Fixture.IncompatibleComponentProperty->PropertyClass->IsChildOf(UButton::StaticClass()));
	TestTrue(TEXT("the mismatched node names the incompatible component property"),
		Mismatched->ComponentPropertyName == FName(TEXT("IncompatiblePanel")));
	TestTrue(TEXT("the mismatched node still names the native button delegate and its owner"),
		Mismatched->DelegatePropertyName == FName(TEXT("OnClicked"))
			&& Mismatched->DelegateOwnerClass == UButton::StaticClass());
	TestNotNull(TEXT("the mismatched binding owns a genuine downstream body"), MismatchedBody);
	if (!MismatchedBody)
	{
		Fixture.Cleanup();
		return false;
	}

	const FString Selected = Mismatched->NodeGuid.ToString();
	const FString GraphBefore = CaptureNativeGraph(Fixture.Graph);
	const FString WidgetsBefore = CaptureWidgetTree(Fixture.Blueprint);
	const FString BindingsBefore = CaptureComponentBindings(Fixture.Blueprint);
	const bool bDirtyBefore = Fixture.Package->IsDirty();

	FCortexGraphMigrationRetirePlan PlanValue;
	bool bReused = false;
	FCortexCommandResult Error;
	TestFalse(TEXT("a component property that cannot host the bound delegate is refused"),
		Plan(Fixture, { Selected }, PlanValue, bReused, Error));
	TestEqual(TEXT("the mismatched component binding refusal is INVALID_OPERATION"),
		Error.ErrorCode, FString(CortexErrorCodes::InvalidOperation));
	TestTrue(TEXT("the mismatched component binding refusal is diagnostic"), !Error.ErrorMessage.IsEmpty());

	Error = FCortexCommandResult();
	FCortexGraphPreparedPatch Preview;
	TestFalse(TEXT("the patch preview refuses the mismatched component binding before any mutation"),
		PreviewRetirement(Fixture, { Selected }, TEXT("00000000-0000-0000-0000-000000112102"), Preview, Error));
	TestEqual(TEXT("the refused mismatched preview reports INVALID_OPERATION"),
		Error.ErrorCode, FString(CortexErrorCodes::InvalidOperation));

	TestEqual(TEXT("the refused preview leaves the graph unchanged"),
		CaptureNativeGraph(Fixture.Graph), GraphBefore);
	TestEqual(TEXT("the refused preview leaves the designer widget tree unchanged"),
		CaptureWidgetTree(Fixture.Blueprint), WidgetsBefore);
	TestEqual(TEXT("the refused preview leaves the compiled component bindings unchanged"),
		CaptureComponentBindings(Fixture.Blueprint), BindingsBefore);
	TestEqual(TEXT("the refused preview leaves the dirty state unchanged"),
		Fixture.Package->IsDirty(), bDirtyBefore);
	TestNotNull(TEXT("the mismatched binding node is still in the graph"),
		FCortexGraphMigrationOps::FindNodeByGuid(Fixture.Blueprint, Mismatched->NodeGuid));
	TestTrue(TEXT("the staged asset bytes are unchanged"), SameBytes(DiskBefore, ReadBytes(Filename)));

	Fixture.Cleanup();
	return true;
}

// CortexSandbox #112/#113: the observed page keeps obsolete non-entry nodes the default ownership
// partition blocks or shares. `migration.source.additional_node_guids` is the reviewed, explicitly
// bounded way to name them, and the class-specific exception may only lift ownership rules the engine
// proves removable. `UK2Node_Knot` is that class: `IsCompilerRelevant()` returns false (no compiled
// artefact), it declares no serialized state, `GetSubGraphs()` is `UK2Node`'s (no bound graph) and it
// overrides no `DestroyNode`, so `FBlueprintEditorUtils::RemoveNode` -> `Schema->BreakNodeLinks` ->
// `UEdGraphNode::DestroyNode` -> `UEdGraph::RemoveNode` stays inside the named graph. Everything else
// the observed page needs (delegate nodes owned by their delegate declaration, latent calls, the
// tunnel-derived macro instance whose `UK2Node_Tunnel::DestroyNode` unlinks the twinned tunnels of a
// graph the instance does not own) stays refused: an unproved class is never converted to removable
// merely because its GUID appears in the request.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexGraphMigrationRetireAdditionalKnotTest,
	"Cortex.Graph.Authoring.Migration.Retire.AdditionalRerouteKnotJoinsReviewedRemoval",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexGraphMigrationRetireAdditionalKnotTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	using namespace CortexGraphMigrationRetireTest;
	FFixture Fixture;
	TestTrue(TEXT("retirement fixture with a cosmetic hook is created"),
		Fixture.Build(TEXT("BP_RetireAdditionalKnot"), /*bRetainProducer=*/true));
	if (!Fixture.Blueprint) { Fixture.Cleanup(); return false; }

	// Two chained reroute knots between the selected entry and its body: both sit inside the island and
	// neither has a retained consumer, so both are removable once the reviewed request names them.
	UK2Node_Knot* InnerKnot = RerouteThroughKnot(Fixture, Fixture.Alpha, TEXT("then"), Fixture.AlphaBody, TEXT("execute"));
	UK2Node_Knot* OuterKnot = InnerKnot
		? RerouteThroughKnot(Fixture, Fixture.Alpha, TEXT("then"), InnerKnot, TEXT("InputPin")) : nullptr;
	TestNotNull(TEXT("the selected entry's execution path is rerouted through a knot chain"), OuterKnot);
	if (!InnerKnot || !OuterKnot) { Fixture.Cleanup(); return false; }
	TestTrue(TEXT("the rerouted path starts at the selected entry's then pin"),
		Fixture.Alpha->FindPin(TEXT("then"))->LinkedTo.Contains(OuterKnot->GetInputPin()));
	TestTrue(TEXT("the inner reroute still carries an execution link"),
		InnerKnot->GetInputPin()->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec);

	const FString AlphaGuid = Fixture.Alpha->NodeGuid.ToString();
	const FString InnerGuid = InnerKnot->NodeGuid.ToString();
	const FString OuterGuid = OuterKnot->NodeGuid.ToString();
	const TArray<FString> Entries = { AlphaGuid };
	const TArray<FString> RequestedKnots = { InnerGuid, OuterGuid };

	// An empty request list is a request for nothing, not a missing field: the default class rule keeps
	// every unlisted knot blocked.
	FCortexGraphPreparedPatch EmptyPreview;
	FCortexCommandResult Error;
	TestTrue(FString::Printf(TEXT("an empty additional list previews under the default rules: %s"), *Error.ErrorMessage),
		PreviewAdditionalRetirement(Fixture, Entries, {}, TEXT("00000000-0000-0000-0000-000000114001"), EmptyPreview, Error));
	TestTrue(TEXT("an empty additional list publishes an empty canonical list"),
		JsonStringArray(EmptyPreview.RetirementPlan, TEXT("additional_guids")).IsEmpty());
	TestFalse(TEXT("an empty additional list leaves the unlisted inner knot blocked"),
		JsonStringArray(EmptyPreview.RetirementPlan, TEXT("removable_guids")).Contains(InnerGuid));
	TestFalse(TEXT("an empty additional list leaves the unlisted outer knot blocked"),
		JsonStringArray(EmptyPreview.RetirementPlan, TEXT("removable_guids")).Contains(OuterGuid));

	FCortexGraphMigrationRetirePlan Unapproved;
	bool bReused = false;
	Error = FCortexCommandResult();
	TestTrue(FString::Printf(TEXT("the additional-knot preview succeeds: %s"), *Error.ErrorMessage),
		PlanWithAdditionalNodes(Fixture, Entries, RequestedKnots, Unapproved, bReused, Error));
	TestTrue(TEXT("both explicitly requested reroute knots join the removable set"),
		Unapproved.RemovableGuids.Contains(InnerGuid) && Unapproved.RemovableGuids.Contains(OuterGuid));
	TestTrue(TEXT("the unlisted entry body stays removable"),
		Unapproved.RemovableGuids.Contains(Fixture.AlphaBodyGuid.ToString()));
	TestFalse(TEXT("the cosmetic hook stays out of the removal set"),
		Unapproved.RemovableGuids.Contains(Fixture.RetainedGuid.ToString()));
	TestFalse(TEXT("the retained cosmetic body stays out of the removal set"),
		Unapproved.RemovableGuids.Contains(Fixture.RetainedBodyGuid.ToString()));
	TestFalse(TEXT("the shared producer stays out of the removal set"),
		Unapproved.RemovableGuids.Contains(Fixture.ProducerGuid.ToString()));

	// The durable plan carries the canonical requested GUIDs and the exact class/reason per node, so the
	// normalized hash and the readback cannot silently change the selection.
	const TSharedPtr<FJsonObject> PlanJson = Unapproved.ToJson();
	TestEqual(TEXT("the plan publishes the canonical requested additional GUIDs"),
		FString::Join(JsonStringArray(PlanJson, TEXT("additional_guids")), TEXT(",")),
		FString::Join(CanonicalGuids(RequestedKnots), TEXT(",")));
	const TArray<TSharedPtr<FJsonValue>>* AdditionalNodes = nullptr;
	TestTrue(TEXT("the plan publishes one reviewed additional node per requested GUID"),
		PlanJson.IsValid() && PlanJson->TryGetArrayField(TEXT("additional_nodes"), AdditionalNodes)
			&& AdditionalNodes && AdditionalNodes->Num() == RequestedKnots.Num());
	if (AdditionalNodes)
	{
		TArray<FString> NodeGuids;
		for (const TSharedPtr<FJsonValue>& Value : *AdditionalNodes)
		{
			const TSharedPtr<FJsonObject> NodeJson = Value.IsValid() ? Value->AsObject() : nullptr;
			FString NodeGuid, ClassPath, Reason;
			TestTrue(TEXT("a reviewed additional node carries its GUID, class and reason"),
				NodeJson.IsValid() && NodeJson->TryGetStringField(TEXT("node_guid"), NodeGuid)
					&& NodeJson->TryGetStringField(TEXT("class_path"), ClassPath)
					&& NodeJson->TryGetStringField(TEXT("reason"), Reason) && !Reason.IsEmpty());
			TestEqual(TEXT("an admitted additional node names the exact engine class it removes"),
				ClassPath, UK2Node_Knot::StaticClass()->GetPathName());
			NodeGuids.Add(NodeGuid);
		}
		NodeGuids.Sort();
		TestEqual(TEXT("the reviewed additional nodes are the requested reroute knots"),
			FString::Join(NodeGuids, TEXT(",")), FString::Join(CanonicalGuids(RequestedKnots), TEXT(",")));
	}
	FCortexGraphMigrationRetirePlan RoundTrip;
	Error = FCortexCommandResult();
	TestTrue(TEXT("the durable plan with additional nodes round-trips"),
		FCortexGraphMigrationRetirePlan::FromJson(PlanJson, RoundTrip, Error));
	TestEqual(TEXT("round-trip keeps the canonical additional GUID list"),
		FString::Join(JsonStringArray(RoundTrip.ToJson(), TEXT("additional_guids")), TEXT(",")),
		FString::Join(JsonStringArray(PlanJson, TEXT("additional_guids")), TEXT(",")));

	// The caller reviews the inventory, so the requested additional nodes must be published there with
	// their class and the reason they were admitted.
	const TSharedPtr<FJsonObject> Inventory = FCortexGraphMigrationOps::MakeRetirementInventory(Unapproved.ToJson());
	TestNotNull(TEXT("the reviewed retirement inventory is created"), Inventory.Get());
	const TArray<TSharedPtr<FJsonValue>>* InventoryAdditional = nullptr;
	TestTrue(TEXT("the inventory publishes one reviewed line per requested additional node"),
		Inventory.IsValid() && Inventory->TryGetArrayField(TEXT("additional_nodes"), InventoryAdditional)
			&& InventoryAdditional && InventoryAdditional->Num() == RequestedKnots.Num());
	if (InventoryAdditional)
	{
		for (const TSharedPtr<FJsonValue>& Value : *InventoryAdditional)
		{
			const FString Line = Value.IsValid() ? Value->AsString() : FString();
			TestTrue(FString::Printf(TEXT("the inventory line names a requested knot and its class: %s"), *Line),
				Line.Contains(TEXT("K2Node_Knot")) && (Line.Contains(InnerGuid) || Line.Contains(OuterGuid)));
		}
	}

	// The reviewed preflight hash is built from the normalized plan, so two requests that differ only in
	// the order of the requested GUIDs must not produce two different reviewed intents.
	FCortexGraphPreparedPatch ForwardPreview;
	FCortexGraphPreparedPatch ReversedPreview;
	Error = FCortexCommandResult();
	TestTrue(FString::Printf(TEXT("the forward-ordered reviewed preview succeeds: %s"), *Error.ErrorMessage),
		PreviewAdditionalRetirement(Fixture, Entries, { InnerGuid, OuterGuid },
			TEXT("00000000-0000-0000-0000-000000114002"), ForwardPreview, Error));
	Error = FCortexCommandResult();
	TestTrue(FString::Printf(TEXT("the reverse-ordered reviewed preview succeeds: %s"), *Error.ErrorMessage),
		PreviewAdditionalRetirement(Fixture, Entries, { OuterGuid, InnerGuid },
			TEXT("00000000-0000-0000-0000-000000114002"), ReversedPreview, Error));
	TestEqual(TEXT("the reviewed preflight hash does not depend on the requested GUID order"),
		ReversedPreview.ValidationHash, ForwardPreview.ValidationHash);
	TestEqual(TEXT("both orders publish the same canonical requested list"),
		FString::Join(JsonStringArray(ReversedPreview.RetirementPlan, TEXT("additional_guids")), TEXT(",")),
		FString::Join(JsonStringArray(ForwardPreview.RetirementPlan, TEXT("additional_guids")), TEXT(",")));

	// Approval stays strictly bounded: an approval that drops a requested additional node is refused
	// before anything is deleted.
	TArray<FString> TruncatedApproval = Unapproved.RemovableGuids;
	TruncatedApproval.Remove(InnerGuid);
	TruncatedApproval.Remove(OuterGuid);
	const FString HashBefore = FCortexGraphPatchState::ComputeFingerprint(Fixture.Blueprint)
		->GetStringField(TEXT("graph_authoring_hash"));
	const int32 NodesBefore = Fixture.Graph->Nodes.Num();
	const bool bDirtyBefore = Fixture.Package->IsDirty();
	FCortexGraphMigrationRetirePlan RefusedPlan;
	Error = FCortexCommandResult();
	TestFalse(TEXT("an approval that omits an explicitly requested additional node is refused"),
		PlanWithAdditionalNodes(Fixture, Entries, RequestedKnots, RefusedPlan, bReused, Error, true, TruncatedApproval));
	TestEqual(TEXT("the bounded approval refusal is INVALID_OPERATION"),
		Error.ErrorCode, FString(CortexErrorCodes::InvalidOperation));
	TestTrue(TEXT("the bounded approval refusal demands the exact removable set"),
		Error.ErrorMessage.Contains(TEXT("must exactly equal the removable set")));
	TestTrue(TEXT("the bounded approval refusal names the omitted requested node"),
		Error.ErrorMessage.Contains(InnerGuid) || Error.ErrorMessage.Contains(OuterGuid));
	TestEqual(TEXT("the refused bounded approval leaves the graph hash unchanged"),
		FCortexGraphPatchState::ComputeFingerprint(Fixture.Blueprint)->GetStringField(TEXT("graph_authoring_hash")),
		HashBefore);
	TestEqual(TEXT("the refused bounded approval leaves the node count unchanged"),
		Fixture.Graph->Nodes.Num(), NodesBefore);
	TestEqual(TEXT("the refused bounded approval leaves the dirty state unchanged"),
		Fixture.Package->IsDirty(), bDirtyBefore);

	// The full reviewed path: apply the exact published set and prove the cosmetic hook survived.
	TSharedPtr<FJsonObject> Request;
	TArray<FString> Approved;
	Error = FCortexCommandResult();
	TestTrue(FString::Printf(TEXT("the two-stage additional-node request is prepared: %s"), *Error.ErrorMessage),
		PrepareAdditionalRetirementRequest(Fixture, Entries, RequestedKnots,
			TEXT("00000000-0000-0000-0000-000000114003"), Request, Approved, Error));
	TestTrue(TEXT("the reviewed approval covers both explicitly requested knots"),
		Approved.Contains(InnerGuid) && Approved.Contains(OuterGuid));
	const TSet<FGuid> RetainedGuids = { Fixture.RetainedGuid, Fixture.RetainedBodyGuid, Fixture.ProducerGuid };
	const FGuid InnerKnotGuidValue = InnerKnot->NodeGuid;
	const FString RetainedBefore = CaptureNativeGraph(Fixture.Graph, &RetainedGuids);
	FCortexGraphPatchOutcome Outcome;
	Error = FCortexCommandResult();
	TestTrue(FString::Printf(TEXT("the reviewed additional-node retirement applies: %s"), *Error.ErrorMessage),
		FCortexGraphPatchOps::Execute(Fixture.Blueprint, Request, Outcome, Error));
	TestEqual(TEXT("the additional-node retirement reports applied"), Outcome.ApplyStatus, FString(TEXT("applied")));
	TestEqual(TEXT("the additional-node retirement readback matches"), Outcome.ReadbackStatus, FString(TEXT("matched")));
	for (const FString& GuidText : Approved)
	{
		FGuid Guid;
		FGuid::Parse(GuidText, Guid);
		TestNull(FString::Printf(TEXT("approved node %s is absent from the graph and asset"), *GuidText),
			FCortexGraphMigrationOps::FindNodeByGuid(Fixture.Blueprint, Guid));
	}
	TestNull(TEXT("the explicitly requested reroute knot is gone"),
		FCortexGraphMigrationOps::FindNodeByGuid(Fixture.Blueprint, InnerKnotGuidValue));
	TestNotNull(TEXT("the cosmetic hook remains"),
		FCortexGraphMigrationOps::FindNodeByGuid(Fixture.Blueprint, Fixture.RetainedGuid));
	TestNotNull(TEXT("the retained cosmetic body remains"),
		FCortexGraphMigrationOps::FindNodeByGuid(Fixture.Blueprint, Fixture.RetainedBodyGuid));
	TestNotNull(TEXT("the shared producer remains"),
		FCortexGraphMigrationOps::FindNodeByGuid(Fixture.Blueprint, Fixture.ProducerGuid));
	TestEqual(TEXT("the cosmetic hook, its body and the shared producer are unchanged"),
		CaptureNativeGraph(Fixture.Graph, &RetainedGuids), RetainedBefore);
	TestTrue(TEXT("the retained cosmetic execution link survives the additional-node retirement"),
		Fixture.Retained->FindPin(TEXT("then"))->LinkedTo.Contains(Fixture.RetainedBody->FindPin(TEXT("execute"))));
	TestTrue(TEXT("the retained cosmetic data link survives the additional-node retirement"),
		Fixture.Producer->FindPin(TEXT("ReturnValue"))->LinkedTo.Contains(Fixture.RetainedBody->FindPin(TEXT("InString"))));
	TestTrue(TEXT("the additional-node retirement leaves the package dirty"), Fixture.Package->IsDirty());

	Fixture.Cleanup();
	return true;
}

// CortexSandbox #113: the additional-node request is a reviewed contract, so a malformed or foreign
// list must be refused before the partition runs, and an active play session refuses the whole patch
// before any additional-node work happens.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexGraphMigrationRetireAdditionalRequestContractTest,
	"Cortex.Graph.Authoring.Migration.Retire.RefusesMalformedAdditionalNodeRequests",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexGraphMigrationRetireAdditionalRequestContractTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	using namespace CortexGraphMigrationRetireTest;
	FFixture Fixture;
	TestTrue(TEXT("retirement fixture is created"), Fixture.Build(TEXT("BP_RetireAdditionalContract"), true));
	if (!Fixture.Blueprint) { Fixture.Cleanup(); return false; }

	UK2Node_Knot* Knot = RerouteThroughKnot(Fixture, Fixture.Alpha, TEXT("then"), Fixture.AlphaBody, TEXT("execute"));
	TestNotNull(TEXT("the selected entry's execution path is rerouted through a knot"), Knot);
	if (!Knot) { Fixture.Cleanup(); return false; }
	const FString AlphaGuid = Fixture.Alpha->NodeGuid.ToString();
	const FString KnotGuid = Knot->NodeGuid.ToString();
	const TArray<FString> Entries = { AlphaGuid };

	// A second graph of the same asset owns a node the request tries to name.
	UEdGraph* OtherGraph = NewObject<UEdGraph>(Fixture.Blueprint);
	OtherGraph->GraphGuid = FGuid::NewGuid();
	Fixture.Blueprint->FunctionGraphs.Add(OtherGraph);
	UK2Node_CallFunction* ForeignNode = NewObject<UK2Node_CallFunction>(OtherGraph);
	ForeignNode->CreateNewGuid();
	OtherGraph->AddNode(ForeignNode, true, false);
	const FString ForeignGuid = ForeignNode->NodeGuid.ToString();
	const FString AbsentGuid = FGuid::NewGuid().ToString();

	auto ExpectRefused = [&](const TCHAR* Label, const TArray<FString>& AdditionalGuids,
		const FString& ExpectedCode, const TArray<FString>& Fragments)
	{
		const FString CaseHashBefore = FCortexGraphPatchState::ComputeFingerprint(Fixture.Blueprint)
			->GetStringField(TEXT("graph_authoring_hash"));
		const int32 CaseNodesBefore = Fixture.Graph->Nodes.Num();
		const bool bCaseDirtyBefore = Fixture.Package->IsDirty();
		FCortexGraphMigrationRetirePlan CasePlan;
		bool bCaseReused = false;
		FCortexCommandResult CaseError;
		const bool bPlanned = PlanWithAdditionalNodes(Fixture, Entries, AdditionalGuids,
			CasePlan, bCaseReused, CaseError);
		TestFalse(FString::Printf(TEXT("%s is refused"), Label), bPlanned);
		TestEqual(FString::Printf(TEXT("%s reports the expected error code"), Label),
			CaseError.ErrorCode, ExpectedCode);
		for (const FString& Fragment : Fragments)
		{
			TestTrue(FString::Printf(TEXT("%s refusal names '%s': %s"), Label, *Fragment, *CaseError.ErrorMessage),
				CaseError.ErrorMessage.Contains(Fragment));
		}
		TestEqual(FString::Printf(TEXT("%s leaves the graph hash unchanged"), Label),
			FCortexGraphPatchState::ComputeFingerprint(Fixture.Blueprint)
				->GetStringField(TEXT("graph_authoring_hash")), CaseHashBefore);
		TestEqual(FString::Printf(TEXT("%s leaves the node count unchanged"), Label),
			Fixture.Graph->Nodes.Num(), CaseNodesBefore);
		TestEqual(FString::Printf(TEXT("%s leaves the dirty state unchanged"), Label),
			Fixture.Package->IsDirty(), bCaseDirtyBefore);
	};

	ExpectRefused(TEXT("a repeated additional GUID"), { KnotGuid, KnotGuid },
		FString(CortexErrorCodes::InvalidField),
		{ TEXT("additional_node_guids"), TEXT("unique") });
	ExpectRefused(TEXT("an additional GUID that repeats a selected entry"), { AlphaGuid },
		FString(CortexErrorCodes::InvalidField),
		{ TEXT("selected entry"), AlphaGuid });
	ExpectRefused(TEXT("an additional entry that is not a GUID"), { TEXT("not-a-guid") },
		FString(CortexErrorCodes::InvalidField),
		{ TEXT("additional_node_guids"), TEXT("GUID") });
	ExpectRefused(TEXT("an additional GUID absent from the asset"), { AbsentGuid },
		FString(CortexErrorCodes::InvalidOperation),
		{ AbsentGuid, TEXT("not present") });
	ExpectRefused(TEXT("an additional GUID owned by another graph of the asset"), { ForeignGuid },
		FString(CortexErrorCodes::InvalidOperation),
		{ ForeignGuid, OtherGraph->GraphGuid.ToString(), TEXT("another graph") });

	// An active play or simulate session refuses the patch through the real apply shell before any
	// mutation. `FCortexGraphPatchOps::Preflight` is a pure planning call (a preview legitimately
	// succeeds during PIE), so the eligibility shell that owns the play-session guard is exercised by
	// `Execute` with a fully prepared reviewed request.
	UWorld* const EditorWorld = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
	TestNotNull(TEXT("the editor world is available for the play-session precondition"), EditorWorld);
	if (EditorWorld)
	{
		TSharedPtr<FJsonObject> PlayRequest;
		TArray<FString> PlayApproved;
		FCortexCommandResult PlayError;
		TestTrue(FString::Printf(TEXT("the reviewed play-session request is prepared: %s"), *PlayError.ErrorMessage),
			PrepareAdditionalRetirementRequest(Fixture, Entries, { KnotGuid },
				TEXT("00000000-0000-0000-0000-000000114101"), PlayRequest, PlayApproved, PlayError));
		const FString PlayHashBefore = FCortexGraphPatchState::ComputeFingerprint(Fixture.Blueprint)
			->GetStringField(TEXT("graph_authoring_hash"));
		const int32 PlayNodesBefore = Fixture.Graph->Nodes.Num();
		const bool bPlayDirtyBefore = Fixture.Package->IsDirty();
		UWorld* const PlayWorldBefore = GEditor->PlayWorld;
		GEditor->PlayWorld = EditorWorld;
		FCortexGraphPatchOutcome PlayOutcome;
		PlayError = FCortexCommandResult();
		const bool bPlayApplied = FCortexGraphPatchOps::Execute(Fixture.Blueprint, PlayRequest, PlayOutcome, PlayError);
		GEditor->PlayWorld = PlayWorldBefore;
		TestFalse(TEXT("an active play session refuses a request that names additional nodes"), bPlayApplied);
		TestEqual(TEXT("the play-session refusal is INVALID_OPERATION"),
			PlayError.ErrorCode, FString(CortexErrorCodes::InvalidOperation));
		TestTrue(TEXT("the play-session refusal names the play or simulate session"),
			PlayError.ErrorMessage.Contains(TEXT("play or simulate session")));
		TestEqual(TEXT("the refused play-session apply reports that nothing was requested"),
			PlayOutcome.ApplyStatus, FString(TEXT("not_requested")));
		TestEqual(TEXT("the refused play-session apply leaves the graph hash unchanged"),
			FCortexGraphPatchState::ComputeFingerprint(Fixture.Blueprint)
				->GetStringField(TEXT("graph_authoring_hash")), PlayHashBefore);
		TestEqual(TEXT("the refused play-session apply leaves the node count unchanged"),
			Fixture.Graph->Nodes.Num(), PlayNodesBefore);
		TestEqual(TEXT("the refused play-session apply leaves the dirty state unchanged"),
			Fixture.Package->IsDirty(), bPlayDirtyBefore);

		// With the session over, the very same prepared request finally reaches the review shell.
		FCortexGraphPreparedPatch EndedPreview;
		FCortexCommandResult EndedError;
		TestTrue(FString::Printf(TEXT("the same request previews once the play session ends: %s"), *EndedError.ErrorMessage),
			FCortexGraphPatchOps::Preflight(Fixture.Blueprint, PlayRequest, EndedPreview, EndedError));
	}

	Fixture.Cleanup();
	return true;
}

// CortexSandbox #113: the additional-node list may only join nodes of the selected entries' ownership
// island, and it never lifts the class rule for a node it does not name.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexGraphMigrationRetireAdditionalIslandBoundaryTest,
	"Cortex.Graph.Authoring.Migration.Retire.RefusesAdditionalNodeOutsideTheOwnershipIsland",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexGraphMigrationRetireAdditionalIslandBoundaryTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	using namespace CortexGraphMigrationRetireTest;
	FFixture Fixture;
	TestTrue(TEXT("retirement fixture with a cosmetic hook is created"),
		Fixture.Build(TEXT("BP_RetireAdditionalIsland"), /*bRetainProducer=*/true));
	if (!Fixture.Blueprint) { Fixture.Cleanup(); return false; }

	// A knot inside the retained cosmetic hook's own chain: a real node of the named graph that the
	// selected entry's ownership island does not contain.
	UK2Node_Knot* CosmeticKnot = RerouteThroughKnot(Fixture, Fixture.Retained, TEXT("then"),
		Fixture.RetainedBody, TEXT("execute"));
	UK2Node_Knot* IslandKnot = RerouteThroughKnot(Fixture, Fixture.Alpha, TEXT("then"),
		Fixture.AlphaBody, TEXT("execute"));
	TestTrue(TEXT("the cosmetic hook's execution path is rerouted through a knot"),
		CosmeticKnot != nullptr && IslandKnot != nullptr);
	if (!CosmeticKnot || !IslandKnot) { Fixture.Cleanup(); return false; }
	const FString AlphaGuid = Fixture.Alpha->NodeGuid.ToString();
	const FString CosmeticKnotGuid = CosmeticKnot->NodeGuid.ToString();
	const FString IslandKnotGuid = IslandKnot->NodeGuid.ToString();

	const FString HashBefore = FCortexGraphPatchState::ComputeFingerprint(Fixture.Blueprint)
		->GetStringField(TEXT("graph_authoring_hash"));
	const int32 NodesBefore = Fixture.Graph->Nodes.Num();
	const bool bDirtyBefore = Fixture.Package->IsDirty();
	FCortexGraphMigrationRetirePlan RefusedPlan;
	bool bReused = false;
	FCortexCommandResult Error;
	TestFalse(TEXT("a requested additional node outside the selected entries' island is refused"),
		PlanWithAdditionalNodes(Fixture, { AlphaGuid }, { CosmeticKnotGuid }, RefusedPlan, bReused, Error));
	TestEqual(TEXT("the outside-island refusal is INVALID_OPERATION"),
		Error.ErrorCode, FString(CortexErrorCodes::InvalidOperation));
	TestTrue(TEXT("the outside-island refusal names the requested node and the island"),
		Error.ErrorMessage.Contains(CosmeticKnotGuid) && Error.ErrorMessage.Contains(TEXT("island")));
	TestEqual(TEXT("the refused outside-island request leaves the graph hash unchanged"),
		FCortexGraphPatchState::ComputeFingerprint(Fixture.Blueprint)
			->GetStringField(TEXT("graph_authoring_hash")), HashBefore);
	TestEqual(TEXT("the refused outside-island request leaves the node count unchanged"),
		Fixture.Graph->Nodes.Num(), NodesBefore);
	TestEqual(TEXT("the refused outside-island request leaves the dirty state unchanged"),
		Fixture.Package->IsDirty(), bDirtyBefore);

	// The old rule holds for every node the request does not name: an unlisted in-island knot stays
	// blocked by class, and the reviewed approval of the published removable set still refuses.
	FCortexGraphMigrationRetirePlan UnlistedPlan;
	Error = FCortexCommandResult();
	TestTrue(FString::Printf(TEXT("the request without an additional list previews: %s"), *Error.ErrorMessage),
		Plan(Fixture, { AlphaGuid }, UnlistedPlan, bReused, Error));
	TestTrue(TEXT("an unlisted in-island knot is published as blocked"),
		UnlistedPlan.Blocked.ContainsByPredicate(
			[&](const FCortexGraphPruneNode& Node) { return Node.NodeGuid == IslandKnotGuid; }));
	TestFalse(TEXT("an unlisted in-island knot is never removable"),
		UnlistedPlan.RemovableGuids.Contains(IslandKnotGuid));
	Error = FCortexCommandResult();
	const TArray<FString> UnlistedRemovable = UnlistedPlan.RemovableGuids;
	TestFalse(TEXT("the reviewed approval of a set with an unlisted blocked knot is refused"),
		Plan(Fixture, { AlphaGuid }, RefusedPlan, bReused, Error, true, UnlistedRemovable));
	TestEqual(TEXT("the unlisted blocked knot refusal is INVALID_OPERATION"),
		Error.ErrorCode, FString(CortexErrorCodes::InvalidOperation));

	Fixture.Cleanup();
	return true;
}

// CortexSandbox #113: the reviewed additional-node exception exists because the observed page cannot
// retire without this family. Each admitted class is proven removal-local inside the named graph:
// the engine's own `FBlueprintEditorUtils::RemoveNode` breaks the node's links and calls `DestroyNode`,
// which none of them overrides in a way that reaches outside the node.
//   - `UK2Node_MacroInstance`: `MacroGraphReference` is a shared reference to a graph the instance does
//     not own (`GetSubGraphs()` is `UEdGraphNode`'s empty override - only `UK2Node_Composite` returns a
//     bound graph), and `UK2Node_Tunnel::DestroyNode`'s twin unlink cannot apply because
//     `InputSinkNode`/`OutputSourceNode` are assigned for composite boundary nodes only.
//   - a latent `UK2Node_CallFunction`: the pending action lives in the running world's
//     `FLatentActionManager` (removed per object on reinstancing, `KismetReinstanceUtilities.cpp:805`)
//     and the asset-side trace is the compiled latent statement plus the generated class's debug UUID
//     association, both rebuilt by the recompile an admitted node requires.
//   - `UK2Node_CreateDelegate` / `UK2Node_AddDelegate` / `UK2Node_CallDelegate`: the delegate
//     declaration lives on the owning class the node references, and none of them overrides
//     `GetDynamicBindingClass()` (`UComponentDelegateBinding` is registered by component-bound event
//     nodes only), so no generated binding object is attached to them.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexGraphMigrationRetireAdditionalClassAdmissionTest,
	"Cortex.Graph.Authoring.Migration.Retire.AdmitsProvenAdditionalNodeClasses",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexGraphMigrationRetireAdditionalClassAdmissionTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	using namespace CortexGraphMigrationRetireTest;
	FFixture Fixture;
	TestTrue(TEXT("retirement fixture with a cosmetic hook is created"),
		Fixture.Build(TEXT("BP_RetireAdditionalClasses"), /*bRetainProducer=*/true));
	if (!Fixture.Blueprint) { Fixture.Cleanup(); return false; }
	const FString AlphaGuid = Fixture.Alpha->NodeGuid.ToString();

	// The page's admitted non-entry chain (latent call, delegate create/add pair, engine macro
	// instance) is built through engine APIs and linked into the selected entry's island.
	FAdmittedNodeChain Chain;
	const bool bChainBuilt = Chain.Build(Fixture);
	TestTrue(FString::Printf(TEXT("the page-shaped admitted chain is built: %s"), *Chain.Failure), bChainBuilt);
	if (!Chain.IsComplete()) { Fixture.Cleanup(); return false; }
	TestTrue(TEXT("the engine marks the observed call as latent"), Chain.Latent->IsLatentFunction());
	TestNotNull(TEXT("the delegate create node exposes a delegate output"),
		FindTypedPin(Chain.CreateDelegate, EGPD_Output, UEdGraphSchema_K2::PC_Delegate));
	TestNotNull(TEXT("the delegate add node exposes a delegate input"),
		FindTypedPin(Chain.AddDelegate, EGPD_Input, UEdGraphSchema_K2::PC_Delegate));
	// The binding is compile-valid, not merely wired: the add node's Target resolves to the Button the
	// delegate is declared on, the create node's delegate pin resolves the expected signature, and its
	// selected function is a real parameterless callable function visible in that scope.
	UEdGraphPin* const AddedDelegateTarget = Chain.AddDelegate->FindPin(UEdGraphSchema_K2::PN_Self);
	TestTrue(TEXT("the add node's Target pin is connected to the Button object"),
		AddedDelegateTarget && AddedDelegateTarget->LinkedTo.Num() > 0);
	TestNotNull(TEXT("the create node's delegate pin resolves the expected signature"),
		Chain.CreateDelegate->GetDelegateSignature());
	TestTrue(TEXT("the create node names a real callable function"),
		Chain.CreateDelegate->GetFunctionName() == FName(TEXT("SetFocus")));
	TestTrue(TEXT("the create node's scope sees the selected function"),
		Chain.CreateDelegate->GetScopeClass()
			&& Chain.CreateDelegate->GetScopeClass()->FindFunctionByName(TEXT("SetFocus")) != nullptr);
	TestNotNull(TEXT("the macro instance exposes an execution input"),
		FindTypedPin(Chain.Macro, EGPD_Input, UEdGraphSchema_K2::PC_Exec));
	TestTrue(TEXT("the macro instance owns no bound subgraph"), Chain.Macro->GetSubGraphs().IsEmpty());
	TestTrue(FString::Printf(
		TEXT("the fixture macro instance is the engine IsValid macro graph the page holds (chosen: %s)"),
		*Chain.MacroGraphName), Chain.bMacroGraphIsIsValid);
	TestTrue(TEXT("the macro instance belongs to the resolved engine macro graph"),
		Chain.Macro->GetMacroGraph() == Chain.MacroGraph);
	TestTrue(TEXT("the macro instance's data input is fed by the real object source, or the macro declares none"),
		Chain.bMacroDataInputFed);
	TestNull(TEXT("the macro instance has no input sink twin"), Chain.Macro->InputSinkNode.Get());
	TestNull(TEXT("the macro instance has no output source twin"), Chain.Macro->OutputSourceNode.Get());
	UK2Node_CallFunction* const Latent = Chain.Latent;
	UK2Node_CreateDelegate* const CreateDelegate = Chain.CreateDelegate;
	UK2Node_AddDelegate* const AddDelegate = Chain.AddDelegate;
	UK2Node_MacroInstance* const Macro = Chain.Macro;
	UEdGraph* const MacroGraph = Chain.MacroGraph;
	const FString MacroGraphBefore = CaptureNativeGraph(MacroGraph);
	TestTrue(TEXT("the referenced macro graph is a real engine macro graph"), !MacroGraphBefore.IsEmpty());
	// Detail-screen tail: AssignDelegate creates a separate custom event on placement; removing
	// the assign node must leave that event alone. UMG's PlayAnimation is an exact async-proxy class.
	FMulticastDelegateProperty* const Clicked =
		FindFProperty<FMulticastDelegateProperty>(UButton::StaticClass(), TEXT("OnClicked"));
	if (!Clicked) { Fixture.Cleanup(); return false; }
	UK2Node_AssignDelegate* const Assign = NewObject<UK2Node_AssignDelegate>(Fixture.Graph, NAME_None, RF_Transactional);
	Assign->SetFromProperty(Clicked, /*bSelfContext=*/false, UButton::StaticClass());
	Assign->CreateNewGuid();
	Assign->AllocateDefaultPins();
	Fixture.Graph->AddNode(Assign, true, false);
	Assign->PostPlacedNewNode();
	UEdGraphPin* const AssignedDelegate = FindTypedPin(Assign, EGPD_Input, UEdGraphSchema_K2::PC_Delegate);
	UK2Node_CustomEvent* const CompanionEvent = AssignedDelegate && AssignedDelegate->LinkedTo.Num() == 1
		? Cast<UK2Node_CustomEvent>(AssignedDelegate->LinkedTo[0]->GetOwningNode()) : nullptr;
	TestNotNull(TEXT("assign placement creates an independently owned companion event"), CompanionEvent);
	UClass* const AnimationClass = LoadClass<UK2Node_BaseAsyncTask>(nullptr,
		TEXT("/Script/UMGEditor.K2Node_PlayAnimation"));
	TestNotNull(TEXT("UMG's exact PlayAnimation node class loads"), AnimationClass);
	if (!CompanionEvent || !AnimationClass) { Fixture.Cleanup(); return false; }
	UK2Node_BaseAsyncTask* const Animation =
		NewObject<UK2Node_BaseAsyncTask>(Fixture.Graph, AnimationClass, NAME_None, RF_Transactional);
	Animation->CreateNewGuid();
	Animation->AllocateDefaultPins();
	Fixture.Graph->AddNode(Animation, true, false);
	FString TailFailure;
	UEdGraphPin* const MacroExit = FindTypedPin(Macro, EGPD_Output, UEdGraphSchema_K2::PC_Exec);
	UEdGraphPin* const AnimationEntry = FindTypedPin(Animation, EGPD_Input, UEdGraphSchema_K2::PC_Exec);
	TestTrue(TEXT("detail-shaped assign and animation belong to the selected execution island"),
		ConnectTypedPins(Fixture.Graph->GetSchema(), MacroExit, Assign->FindPin(TEXT("execute")),
			TEXT("macro to assign"), TailFailure)
		&& ConnectTypedPins(Fixture.Graph->GetSchema(), Assign->FindPin(TEXT("then")), AnimationEntry,
			TEXT("assign to animation"), TailFailure));
	TestTrue(FString::Printf(TEXT("the detail tail is connected: %s"), *TailFailure), TailFailure.IsEmpty());
	TestTrue(TEXT("animation node owns no bound graph"), Animation->GetSubGraphs().IsEmpty());
	TestTrue(TEXT("assign node owns no bound graph"), Assign->GetSubGraphs().IsEmpty());
	const FGuid CompanionGuid = CompanionEvent->NodeGuid;

	const TArray<FString> Requested = { Latent->NodeGuid.ToString(), CreateDelegate->NodeGuid.ToString(),
		AddDelegate->NodeGuid.ToString(), Macro->NodeGuid.ToString(), Assign->NodeGuid.ToString(),
		Animation->NodeGuid.ToString() };
	FCortexGraphMigrationRetirePlan PlanValue;
	bool bReused = false;
	FCortexCommandResult Error;
	TestTrue(FString::Printf(TEXT("the reviewed preview admits the proven additional classes: %s"), *Error.ErrorMessage),
		PlanWithAdditionalNodes(Fixture, { AlphaGuid }, Requested, PlanValue, bReused, Error));
	TestTrue(TEXT("every admitted additional node joins the removable set"),
		PlanValue.RemovableGuids.Contains(Latent->NodeGuid.ToString())
			&& PlanValue.RemovableGuids.Contains(CreateDelegate->NodeGuid.ToString())
			&& PlanValue.RemovableGuids.Contains(AddDelegate->NodeGuid.ToString())
			&& PlanValue.RemovableGuids.Contains(Macro->NodeGuid.ToString())
			&& PlanValue.RemovableGuids.Contains(Assign->NodeGuid.ToString())
			&& PlanValue.RemovableGuids.Contains(Animation->NodeGuid.ToString()));
	TestTrue(TEXT("the retained cosmetic hook stays out of the removal set"),
		!PlanValue.RemovableGuids.Contains(Fixture.RetainedGuid.ToString())
			&& !PlanValue.RemovableGuids.Contains(Fixture.RetainedBodyGuid.ToString()));
	TestTrue(TEXT("an admitted node the compiler consumes requires the recompile"), PlanValue.bRequiresCompile);

	const TSharedPtr<FJsonObject> PlanJson = PlanValue.ToJson();
	TestEqual(TEXT("the plan publishes the canonical requested additional GUIDs"),
		FString::Join(JsonStringArray(PlanJson, TEXT("additional_guids")), TEXT(",")),
		FString::Join(CanonicalGuids(Requested), TEXT(",")));
	auto AdditionalNodeJson = [&](const FString& Guid) -> TSharedPtr<FJsonObject>
	{
		const TArray<TSharedPtr<FJsonValue>>* Nodes = nullptr;
		if (!PlanJson.IsValid() || !PlanJson->TryGetArrayField(TEXT("additional_nodes"), Nodes) || !Nodes) return nullptr;
		for (const TSharedPtr<FJsonValue>& Value : *Nodes)
		{
			const TSharedPtr<FJsonObject> NodeJson = Value.IsValid() ? Value->AsObject() : nullptr;
			FString NodeGuid;
			if (NodeJson.IsValid() && NodeJson->TryGetStringField(TEXT("node_guid"), NodeGuid) && NodeGuid == Guid)
			{
				return NodeJson;
			}
		}
		return nullptr;
	};
	auto AdmittedClassOf = [&](const FString& Guid) -> FString
	{
		const TSharedPtr<FJsonObject> NodeJson = AdditionalNodeJson(Guid);
		FString ClassPath;
		if (NodeJson.IsValid()) NodeJson->TryGetStringField(TEXT("class_path"), ClassPath);
		return ClassPath;
	};
	auto AdmittedReasonOf = [&](const FString& Guid) -> FString
	{
		const TSharedPtr<FJsonObject> NodeJson = AdditionalNodeJson(Guid);
		FString Reason;
		if (NodeJson.IsValid()) NodeJson->TryGetStringField(TEXT("reason"), Reason);
		return Reason;
	};
	TestEqual(TEXT("the latent call is admitted as its exact engine class"),
		AdmittedClassOf(Latent->NodeGuid.ToString()), UK2Node_CallFunction::StaticClass()->GetPathName());
	TestEqual(TEXT("the create node is admitted as its exact engine class"),
		AdmittedClassOf(CreateDelegate->NodeGuid.ToString()), UK2Node_CreateDelegate::StaticClass()->GetPathName());
	TestEqual(TEXT("the add node is admitted as its exact engine class"),
		AdmittedClassOf(AddDelegate->NodeGuid.ToString()), UK2Node_AddDelegate::StaticClass()->GetPathName());
	TestEqual(TEXT("the macro instance is admitted as its exact engine class"),
		AdmittedClassOf(Macro->NodeGuid.ToString()), UK2Node_MacroInstance::StaticClass()->GetPathName());
	TestEqual(TEXT("the assign node is admitted as its exact engine class"),
		AdmittedClassOf(Assign->NodeGuid.ToString()), UK2Node_AssignDelegate::StaticClass()->GetPathName());
	TestEqual(TEXT("the widget animation node is admitted as its exact editor class"),
		AdmittedClassOf(Animation->NodeGuid.ToString()), AnimationClass->GetPathName());
	for (const FString& Guid : Requested)
	{
		TestTrue(FString::Printf(TEXT("the admitted additional node %s publishes its admitting proof"), *Guid),
			!AdmittedReasonOf(Guid).IsEmpty());
	}
	const TSharedPtr<FJsonObject> Inventory = FCortexGraphMigrationOps::MakeRetirementInventory(PlanJson);
	const TArray<TSharedPtr<FJsonValue>>* InventoryAdditional = nullptr;
	TestTrue(TEXT("the inventory publishes the reviewed additional nodes with class and reason"),
		Inventory.IsValid() && Inventory->TryGetArrayField(TEXT("additional_nodes"), InventoryAdditional)
			&& InventoryAdditional && InventoryAdditional->Num() == Requested.Num());
	if (InventoryAdditional)
	{
		FString Joined;
		for (const TSharedPtr<FJsonValue>& Value : *InventoryAdditional)
		{
			Joined += Value.IsValid() ? Value->AsString() : FString();
			Joined += TEXT("\n");
		}
		TestTrue(TEXT("the inventory names the macro instance class"),
			Joined.Contains(TEXT("K2Node_MacroInstance")));
		TestTrue(TEXT("the inventory names the delegate classes"),
			Joined.Contains(TEXT("K2Node_CreateDelegate")) && Joined.Contains(TEXT("K2Node_AddDelegate")));
		TestTrue(TEXT("the inventory names the latent call class"),
			Joined.Contains(TEXT("K2Node_CallFunction")));
	}

	// The staged reviewed path: a compile-consuming class must never be reported as runtime-safe
	// without the recompile, and the retained cosmetic hook must survive the removal.
	TSharedPtr<FJsonObject> Request;
	TArray<FString> Approved;
	Error = FCortexCommandResult();
	TestTrue(FString::Printf(TEXT("the two-stage class-admission request is prepared: %s"), *Error.ErrorMessage),
		PrepareAdditionalRetirementRequest(Fixture, { AlphaGuid }, Requested,
			TEXT("00000000-0000-0000-0000-000000114201"), Request, Approved, Error, /*bCompile=*/false));
	TestTrue(TEXT("the reviewed approval covers the latent, delegate, macro and animation nodes"),
		Approved.Contains(Latent->NodeGuid.ToString())
			&& Approved.Contains(CreateDelegate->NodeGuid.ToString())
			&& Approved.Contains(AddDelegate->NodeGuid.ToString())
			&& Approved.Contains(Macro->NodeGuid.ToString())
			&& Approved.Contains(Assign->NodeGuid.ToString())
			&& Approved.Contains(Animation->NodeGuid.ToString()));
	FCortexGraphPatchOutcome Outcome;
	Error = FCortexCommandResult();
	TestTrue(FString::Printf(TEXT("the reviewed class-admitted retirement applies: %s"), *Error.ErrorMessage),
		FCortexGraphPatchOps::Execute(Fixture.Blueprint, Request, Outcome, Error));
	TestEqual(TEXT("the class-admitted retirement reports applied"), Outcome.ApplyStatus, FString(TEXT("applied")));
	TestEqual(TEXT("the class-admitted retirement readback matches"), Outcome.ReadbackStatus, FString(TEXT("matched")));
	TestEqual(TEXT("the staged class-admitted retirement requests no compile"),
		Outcome.CompileStatus, FString(TEXT("not_requested")));
	TestTrue(TEXT("the staged class-admitted retirement reports that a compile is required before the result is runtime-safe"),
		Outcome.Diagnostics.ContainsByPredicate(
			[](const FString& Line) { return Line.Contains(TEXT("runtime-safe")); }));
	for (const FString& GuidText : Approved)
	{
		FGuid Guid;
		FGuid::Parse(GuidText, Guid);
		TestNull(FString::Printf(TEXT("admitted node %s is absent from the graph and asset"), *GuidText),
			FCortexGraphMigrationOps::FindNodeByGuid(Fixture.Blueprint, Guid));
	}
	TestNotNull(TEXT("the cosmetic hook remains after the class-admitted retirement"),
		FCortexGraphMigrationOps::FindNodeByGuid(Fixture.Blueprint, Fixture.RetainedGuid));
	TestNotNull(TEXT("the retained cosmetic body remains after the class-admitted retirement"),
		FCortexGraphMigrationOps::FindNodeByGuid(Fixture.Blueprint, Fixture.RetainedBodyGuid));
	TestTrue(TEXT("the retained cosmetic execution link survives the class-admitted retirement"),
		Fixture.Retained->FindPin(TEXT("then"))->LinkedTo.Contains(Fixture.RetainedBody->FindPin(TEXT("execute"))));
	TestTrue(TEXT("the referenced macro graph is byte-identical after the instance removal"),
		CaptureNativeGraph(MacroGraph) == MacroGraphBefore);
	TestNotNull(TEXT("removing the assign node preserves its separate custom event"),
		FCortexGraphMigrationOps::FindNodeByGuid(Fixture.Blueprint, CompanionGuid));

	Fixture.Cleanup();
	// Detail's old calendar call has an unconnected execution input but consumes a value from the
	// selected island. It is not execution-reachable; explicitly approving that dead-end consumer
	// must close the data boundary without admitting arbitrary nodes outside the island.
	FFixture Detached;
	TestTrue(TEXT("disconnected-consumer fixture is created"), Detached.Build(TEXT("BP_RetireDetachedConsumer"), true));
	if (!Detached.Blueprint) { Detached.Cleanup(); return false; }
	UK2Node_CallFunction* const Disconnected =
		Detached.AddCall(UKismetSystemLibrary::StaticClass()->FindFunctionByName(TEXT("PrintString")));
	if (!Disconnected) { Detached.Cleanup(); return false; }
	const FGuid DisconnectedNodeGuid = Disconnected->NodeGuid;
	const FString DisconnectedGuid = DisconnectedNodeGuid.ToString();
	FCortexGraphMigrationRetirePlan DetachedPlan;
	bool bDetachedReused = false;
	FCortexCommandResult DetachedError;
	TestFalse(TEXT("an unconnected call outside the island stays refused"),
		PlanWithAdditionalNodes(Detached, { Detached.AlphaGuid.ToString() }, { DisconnectedGuid },
			DetachedPlan, bDetachedReused, DetachedError));
	TestTrue(TEXT("the old call consumes the selected island's string producer"),
		Detached.LinkPins(Detached.Producer, TEXT("ReturnValue"), Disconnected, TEXT("InString")));
	TestTrue(TEXT("the obsolete call can be driven by a retained path in the refusal case"),
		Detached.LinkPins(Detached.RetainedBody, TEXT("then"), Disconnected, TEXT("execute")));
	DetachedError = FCortexCommandResult();
	TestFalse(TEXT("a call with a retained execution input is never admitted as disconnected"),
		PlanWithAdditionalNodes(Detached, { Detached.AlphaGuid.ToString() }, { DisconnectedGuid },
			DetachedPlan, bDetachedReused, DetachedError));
	Disconnected->FindPin(TEXT("execute"))->BreakAllPinLinks();
	UK2Node_CallFunction* const Downstream =
		Detached.AddCall(UKismetSystemLibrary::StaticClass()->FindFunctionByName(TEXT("PrintString")));
	TestTrue(TEXT("the obsolete call can feed another path in the refusal case"),
		Downstream && Detached.LinkPins(Disconnected, TEXT("then"), Downstream, TEXT("execute")));
	DetachedError = FCortexCommandResult();
	TestFalse(TEXT("a call with a linked output is never admitted as a dead end"),
		PlanWithAdditionalNodes(Detached, { Detached.AlphaGuid.ToString() }, { DisconnectedGuid },
			DetachedPlan, bDetachedReused, DetachedError));
	Disconnected->FindPin(TEXT("then"))->BreakAllPinLinks();
	TestTrue(TEXT("the obsolete call has no executable entry"),
		Disconnected->FindPin(TEXT("execute")) && Disconnected->FindPin(TEXT("execute"))->LinkedTo.IsEmpty());
	TestTrue(TEXT("the obsolete call has no linked output"),
		Disconnected->FindPin(TEXT("then")) && Disconnected->FindPin(TEXT("then"))->LinkedTo.IsEmpty());
	TSharedPtr<FJsonObject> DetachedRequest;
	TArray<FString> DetachedApproved;
	DetachedError = FCortexCommandResult();
	TestTrue(FString::Printf(TEXT("the reviewed disconnected consumer is prepared: %s"), *DetachedError.ErrorMessage),
		PrepareAdditionalRetirementRequest(Detached, { Detached.AlphaGuid.ToString() }, { DisconnectedGuid },
			TEXT("00000000-0000-0000-0000-000000114202"), DetachedRequest, DetachedApproved, DetachedError,
			/*bCompile=*/false));
	TestTrue(TEXT("the detached consumer joins the exact approved set"),
		DetachedApproved.Contains(DisconnectedGuid));
	FCortexGraphPatchOutcome DetachedOutcome;
	DetachedError = FCortexCommandResult();
	TestTrue(FString::Printf(TEXT("the reviewed disconnected consumer retires: %s"), *DetachedError.ErrorMessage),
		FCortexGraphPatchOps::Execute(Detached.Blueprint, DetachedRequest, DetachedOutcome, DetachedError));
	TestEqual(TEXT("disconnected-consumer readback matches"), DetachedOutcome.ReadbackStatus, FString(TEXT("matched")));
	TestNull(TEXT("the disconnected call was retired"), FCortexGraphMigrationOps::FindNodeByGuid(
		Detached.Blueprint, DisconnectedNodeGuid));
	TestNotNull(TEXT("the shared string producer remains"), FCortexGraphMigrationOps::FindNodeByGuid(
		Detached.Blueprint, Detached.ProducerGuid));
	TestTrue(TEXT("the unrelated cosmetic execution link survives"),
		Detached.Retained->FindPin(TEXT("then"))->LinkedTo.Contains(Detached.RetainedBody->FindPin(TEXT("execute"))));
	Detached.Cleanup();

	// A preserved calendar notification can leave its former body disconnected. Review the
	// setter as an exact root, not as an additional node of an unrelated entry's island.
	FFixture Orphan;
	TestTrue(TEXT("disconnected-root fixture is created"), Orphan.Build(TEXT("BP_RetireDisconnectedRoot"), true));
	if (!Orphan.Blueprint) { Orphan.Cleanup(); return false; }
	FEdGraphPinType BoolType;
	BoolType.PinCategory = UEdGraphSchema_K2::PC_Boolean;
	TestTrue(TEXT("the detached setter has a real Blueprint property"),
		FBlueprintEditorUtils::AddMemberVariable(Orphan.Blueprint, TEXT("bDetached"), BoolType));
	UK2Node_VariableSet* const Root = NewObject<UK2Node_VariableSet>(Orphan.Graph, NAME_None, RF_Transactional);
	Root->VariableReference.SetSelfMember(TEXT("bDetached"));
	Root->CreateNewGuid();
	Root->AllocateDefaultPins();
	Orphan.Graph->AddNode(Root, true, false);
	UK2Node_CallFunction* const Tail = Orphan.AddCall(
		UKismetSystemLibrary::StaticClass()->FindFunctionByName(TEXT("PrintString")));
	if (!Root || !Tail) { Orphan.Cleanup(); return false; }
	const FGuid RootGuid = Root->NodeGuid;
	const FGuid TailGuid = Tail->NodeGuid;
	TestTrue(TEXT("the disconnected root has a live obsolete execution tail"),
		Orphan.LinkPins(Root, TEXT("then"), Tail, TEXT("execute")));
	TestTrue(TEXT("the tail consumes a producer also used by retained presentation"),
		Orphan.LinkPins(Orphan.Producer, TEXT("ReturnValue"), Tail, TEXT("InString")));
	TestTrue(TEXT("the root is refused while reachable from a retained execution path"),
		Orphan.LinkPins(Orphan.RetainedBody, TEXT("then"), Root, TEXT("execute")));
	FCortexGraphMigrationRetirePlan OrphanPlan;
	bool bOrphanReused = false;
	FCortexCommandResult OrphanError;
	TestFalse(TEXT("a connected setter cannot be retired as a disconnected root"),
		Plan(Orphan, { RootGuid.ToString() }, OrphanPlan, bOrphanReused, OrphanError));
	Root->FindPin(TEXT("execute"))->BreakAllPinLinks();
	OrphanError = FCortexCommandResult();
	TestTrue(FString::Printf(TEXT("the detached setter and tail are discoverable: %s"),
		*OrphanError.ErrorMessage),
		Plan(Orphan, { RootGuid.ToString() }, OrphanPlan, bOrphanReused, OrphanError));
	TestTrue(TEXT("the approved island includes the root and its execution tail"),
		OrphanPlan.RemovableGuids.Contains(RootGuid.ToString())
			&& OrphanPlan.RemovableGuids.Contains(TailGuid.ToString()));
	TestFalse(TEXT("the shared producer remains outside the cut"),
		OrphanPlan.RemovableGuids.Contains(Orphan.ProducerGuid.ToString()));
	TSharedPtr<FJsonObject> OrphanRequest;
	TArray<FString> OrphanApproved;
	OrphanError = FCortexCommandResult();
	TestTrue(FString::Printf(TEXT("the detached-root approval is prepared: %s"),
		*OrphanError.ErrorMessage),
		PrepareRetirementRequest(Orphan, { RootGuid.ToString() },
			TEXT("00000000-0000-0000-0000-000000114203"), OrphanRequest, OrphanApproved, OrphanError));
	if (OrphanRequest && OrphanApproved.Contains(RootGuid.ToString())
		&& OrphanApproved.Contains(TailGuid.ToString()))
	{
		FCortexGraphPatchOutcome OrphanOutcome;
		OrphanError = FCortexCommandResult();
		TestTrue(FString::Printf(TEXT("the reviewed detached tail is removed: %s"),
			*OrphanError.ErrorMessage),
			FCortexGraphPatchOps::Execute(Orphan.Blueprint, OrphanRequest, OrphanOutcome, OrphanError));
		TestEqual(TEXT("the detached-tail readback matches"), OrphanOutcome.ReadbackStatus, FString(TEXT("matched")));
		TestNull(TEXT("the disconnected root is absent"),
			FCortexGraphMigrationOps::FindNodeByGuid(Orphan.Blueprint, RootGuid));
		TestNull(TEXT("its obsolete tail is absent"),
			FCortexGraphMigrationOps::FindNodeByGuid(Orphan.Blueprint, TailGuid));
		TestNotNull(TEXT("the retained cosmetic hook survives"),
			FCortexGraphMigrationOps::FindNodeByGuid(Orphan.Blueprint, Orphan.RetainedGuid));
		TestNotNull(TEXT("the shared data producer survives"),
			FCortexGraphMigrationOps::FindNodeByGuid(Orphan.Blueprint, Orphan.ProducerGuid));
	}
	Orphan.Cleanup();
	return true;
}

// CortexSandbox #113: an admitted class the compiler consumes must be removed through the reviewed
// path *with* the recompile the plan demands, and the compiled result must be read back: the retired
// override is no longer implemented by the generated class, the retained cosmetic override still is,
// the generated state really changed and the referenced macro graph and cosmetic links are untouched.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexGraphMigrationRetireAdditionalCompiledApplyTest,
	"Cortex.Graph.Authoring.Migration.Retire.AppliesClassAdmittedRetirementWithCompile",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexGraphMigrationRetireAdditionalCompiledApplyTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	using namespace CortexGraphMigrationRetireTest;
	FFixture Fixture;
	TestTrue(TEXT("retirement fixture with a cosmetic hook is created"),
		Fixture.Build(TEXT("BP_RetireAdmittedCompile"), /*bRetainProducer=*/true));
	if (!Fixture.Blueprint) { Fixture.Cleanup(); return false; }

	FAdmittedNodeChain Chain;
	const bool bChainBuilt = Chain.Build(Fixture);
	TestTrue(FString::Printf(TEXT("the page-shaped admitted chain is built: %s"), *Chain.Failure), bChainBuilt);
	if (!Chain.IsComplete()) { Fixture.Cleanup(); return false; }
	const FString AlphaGuid = Fixture.Alpha->NodeGuid.ToString();
	const TArray<FString> Requested = Chain.Guids();
	TestTrue(FString::Printf(
		TEXT("the compiled fixture uses the engine IsValid macro graph the page holds (chosen: %s)"),
		*Chain.MacroGraphName), Chain.bMacroGraphIsIsValid);

	// Baseline compile: the generated class implements both override events, so the retirement has a
	// real compiled artefact to clear.
	FKismetEditorUtilities::CompileBlueprint(Fixture.Blueprint);
	TestTrue(TEXT("the fixture compiles before the retirement"),
		static_cast<int32>(Fixture.Blueprint->Status) != static_cast<int32>(BS_Error));
	UClass* const GeneratedBefore = Fixture.Blueprint->GeneratedClass;
	TestNotNull(TEXT("the retired override is implemented by the compiled class before the retirement"),
		GeneratedBefore
			? GeneratedBefore->FindFunctionByName(TEXT("OnLegacyAlpha"), EIncludeSuperFlag::ExcludeSuper) : nullptr);
	TestNotNull(TEXT("the retained override is implemented by the compiled class before the retirement"),
		GeneratedBefore
			? GeneratedBefore->FindFunctionByName(TEXT("OnRetainedEvent"), EIncludeSuperFlag::ExcludeSuper) : nullptr);
	const FString GeneratedDigestBefore = FCortexGraphPatchState::ComputeGeneratedStateDigest(Fixture.Blueprint);
	const FString MacroGraphBefore = CaptureNativeGraph(Chain.MacroGraph);
	const TSet<FGuid> RetainedGuids = { Fixture.RetainedGuid, Fixture.RetainedBodyGuid, Fixture.ProducerGuid };
	const FString RetainedBefore = CaptureNativeGraph(Fixture.Graph, &RetainedGuids);

	TSharedPtr<FJsonObject> Request;
	TArray<FString> Approved;
	FCortexCommandResult Error;
	TestTrue(FString::Printf(TEXT("the compiled class-admitted request is prepared: %s"), *Error.ErrorMessage),
		PrepareAdditionalRetirementRequest(Fixture, { AlphaGuid }, Requested,
			TEXT("00000000-0000-0000-0000-000000114301"), Request, Approved, Error, /*bCompile=*/true));
	TestTrue(TEXT("the reviewed approval covers the admitted chain"),
		Approved.Contains(Chain.Latent->NodeGuid.ToString())
			&& Approved.Contains(Chain.CreateDelegate->NodeGuid.ToString())
			&& Approved.Contains(Chain.AddDelegate->NodeGuid.ToString())
			&& Approved.Contains(Chain.Macro->NodeGuid.ToString()));
	FOperations Operations;
	Operations.Begin();
	FCortexGraphPatchOutcome Outcome;
	Error = FCortexCommandResult();
	TestTrue(FString::Printf(TEXT("the compiled class-admitted retirement applies: %s"), *Error.ErrorMessage),
		FCortexGraphPatchOps::Execute(Fixture.Blueprint, Request, Outcome, Error));
	Operations.End();
	TestEqual(TEXT("the compiled retirement reports applied"), Outcome.ApplyStatus, FString(TEXT("applied")));
	TestEqual(TEXT("the compiled retirement reports the compile"), Outcome.CompileStatus, FString(TEXT("compiled")));
	TestEqual(TEXT("the compiled retirement requests exactly one target compile"), Outcome.TargetCompileCount, 1);
	TestEqual(TEXT("the compiled retirement performs exactly one target compile"), Operations.TargetCompiles, 1);
	TestEqual(TEXT("the compiled retirement performs no recovery compile"), Operations.RecoveryCompiles, 0);
	TestEqual(TEXT("the compiled retirement readback matches"), Outcome.ReadbackStatus, FString(TEXT("matched")));
	TestEqual(TEXT("the compiled retirement needs no rollback"), Outcome.RollbackStatus, FString(TEXT("not_requested")));
	TestFalse(TEXT("the compiled retirement saves nothing"), Outcome.bSaved);
	TestTrue(TEXT("the compiled retirement leaves the Blueprint compiling"),
		static_cast<int32>(Fixture.Blueprint->Status) != static_cast<int32>(BS_Error));

	// Generated-class readback: the retired override is gone, the retained cosmetic override survives
	// and the compiled state really changed.
	UClass* const GeneratedAfter = Fixture.Blueprint->GeneratedClass;
	TestNotNull(TEXT("the generated class survives the compiled retirement"), GeneratedAfter);
	TestNull(TEXT("the retired override is no longer implemented by the generated class"),
		GeneratedAfter
			? GeneratedAfter->FindFunctionByName(TEXT("OnLegacyAlpha"), EIncludeSuperFlag::ExcludeSuper) : nullptr);
	TestNotNull(TEXT("the retained cosmetic override is still implemented by the generated class"),
		GeneratedAfter
			? GeneratedAfter->FindFunctionByName(TEXT("OnRetainedEvent"), EIncludeSuperFlag::ExcludeSuper) : nullptr);
	TestNotEqual(TEXT("the compiled retirement rebuilt the generated state"),
		FCortexGraphPatchState::ComputeGeneratedStateDigest(Fixture.Blueprint), GeneratedDigestBefore);
	TestTrue(TEXT("the admitted classes register no generated component delegate binding"),
		CaptureComponentBindings(Fixture.Blueprint).IsEmpty());
	for (const FString& GuidText : Approved)
	{
		FGuid Guid;
		FGuid::Parse(GuidText, Guid);
		TestNull(FString::Printf(TEXT("admitted node %s is absent after the compiled retirement"), *GuidText),
			FCortexGraphMigrationOps::FindNodeByGuid(Fixture.Blueprint, Guid));
	}
	TestEqual(TEXT("the referenced macro graph is byte-identical after the compiled retirement"),
		CaptureNativeGraph(Chain.MacroGraph), MacroGraphBefore);
	TestEqual(TEXT("the cosmetic hook, its body and the shared producer are unchanged"),
		CaptureNativeGraph(Fixture.Graph, &RetainedGuids), RetainedBefore);
	TestTrue(TEXT("the retained cosmetic execution link survives the compiled retirement"),
		Fixture.Retained->FindPin(TEXT("then"))->LinkedTo.Contains(Fixture.RetainedBody->FindPin(TEXT("execute"))));
	TestTrue(TEXT("the retired package is dirty after the compiled retirement"), Fixture.Package->IsDirty());

	Fixture.Cleanup();
	return true;
}

// CortexSandbox #113: a fault after approved removals, after all removals or during the retirement
// readback must restore the exact pre-apply graph, including the explicitly requested additional
// nodes and the referenced macro graph.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexGraphMigrationRetireAdditionalClassRollbackTest,
	"Cortex.Graph.Authoring.Migration.Retire.RollsBackClassAdmittedRetirementOnFault",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexGraphMigrationRetireAdditionalClassRollbackTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	using namespace CortexGraphMigrationRetireTest;
	struct FFaultCase
	{
		const TCHAR* Name;
		bool bReadback;
		bool bCompile;
	};
	const FFaultCase Faults[] = {
		{ TEXT("migration_retire_after_first_removal"), false, false },
		{ TEXT("migration_retire_after_removals"), false, false },
		// The readback fault runs with the compile the admitted plan requires, so the rollback has to
		// restore the generated class through a recovery compile, not only the authoring graph.
		{ TEXT("retire_after_removal"), true, true }
	};
	bool bAllPassed = true;
	for (int32 Index = 0; Index < UE_ARRAY_COUNT(Faults); ++Index)
	{
		FFixture Fixture;
		const FString AssetName = FString::Printf(TEXT("BP_RetireAdmittedRollback_%d"), Index);
		bAllPassed &= TestTrue(TEXT("class-admitted rollback fixture is created"),
			Fixture.Build(*AssetName, /*bRetainProducer=*/true));
		if (!Fixture.Blueprint) { EndFixtureCase(Fixture); continue; }
		FAdmittedNodeChain Chain;
		const bool bRollbackChainBuilt = Chain.Build(Fixture);
		bAllPassed &= TestTrue(FString::Printf(TEXT("class-admitted rollback chain is built: %s"), *Chain.Failure),
			bRollbackChainBuilt);
		if (!Chain.IsComplete()) { EndFixtureCase(Fixture); continue; }
		if (Faults[Index].bCompile)
		{
			FKismetEditorUtilities::CompileBlueprint(Fixture.Blueprint);
			bAllPassed &= TestTrue(TEXT("the compiled rollback fixture compiles before the fault"),
				static_cast<int32>(Fixture.Blueprint->Status) != static_cast<int32>(BS_Error));
		}
		const FString AlphaGuid = Fixture.Alpha->NodeGuid.ToString();
		const FString GraphBefore = CaptureNativeGraph(Fixture.Graph);
		const FString MacroGraphBefore = CaptureNativeGraph(Chain.MacroGraph);
		const FString GeneratedBefore = FCortexGraphPatchState::ComputeGeneratedStateDigest(Fixture.Blueprint);
		UClass* const GeneratedBaseline = Fixture.Blueprint->GeneratedClass;
		if (Faults[Index].bCompile)
		{
			bAllPassed &= TestNotNull(TEXT("the compiled baseline implements the retired override"),
				GeneratedBaseline
					? GeneratedBaseline->FindFunctionByName(TEXT("OnLegacyAlpha"), EIncludeSuperFlag::ExcludeSuper) : nullptr);
			bAllPassed &= TestNotNull(TEXT("the compiled baseline implements the retained override"),
				GeneratedBaseline
					? GeneratedBaseline->FindFunctionByName(TEXT("OnRetainedEvent"), EIncludeSuperFlag::ExcludeSuper) : nullptr);
		}
		const bool bDirtyBefore = Fixture.Package->IsDirty();
		TSharedPtr<FJsonObject> Request;
		TArray<FString> Approved;
		FCortexCommandResult Error;
		const FString PatchId = FString::Printf(TEXT("00000000-0000-0000-0000-00000011440%d"), Index);
		if (!PrepareAdditionalRetirementRequest(Fixture, { AlphaGuid }, Chain.Guids(), *PatchId,
			Request, Approved, Error, /*bCompile=*/Faults[Index].bCompile))
		{
			bAllPassed &= TestFalse(FString::Printf(TEXT("%s request preparation failed: %s"), Faults[Index].Name,
				*Error.ErrorMessage), true);
			EndFixtureCase(Fixture);
			continue;
		}
		FOperations Operations;
		Operations.Begin();
		if (Faults[Index].bReadback)
		{
			FCortexGraphMigrationOps::SetRetirementReadbackFaultForTesting(FName(Faults[Index].Name));
		}
		else
		{
			FCortexGraphPatchOps::SetApplyFaultPointForTesting(FName(Faults[Index].Name));
		}
		FCortexGraphPatchOutcome Outcome;
		Error = FCortexCommandResult();
		bAllPassed &= TestFalse(FString::Printf(TEXT("%s causes Execute to fail"), Faults[Index].Name),
			FCortexGraphPatchOps::Execute(Fixture.Blueprint, Request, Outcome, Error));
		Operations.End();
		FCortexGraphPatchOps::SetApplyFaultPointForTesting(NAME_None);
		FCortexGraphMigrationOps::ClearRetirementReadbackFaultForTesting();
		bAllPassed &= TestEqual(FString::Printf(TEXT("%s restores the transaction"), Faults[Index].Name),
			Outcome.RollbackStatus, FString(TEXT("restored")));
		bAllPassed &= TestFalse(FString::Printf(TEXT("%s does not block the asset"), Faults[Index].Name),
			Outcome.bBlocked);
		bAllPassed &= TestEqual(FString::Printf(TEXT("%s restores the native graph snapshot"), Faults[Index].Name),
			CaptureNativeGraph(Fixture.Graph), GraphBefore);
		bAllPassed &= TestEqual(FString::Printf(TEXT("%s restores the referenced macro graph"), Faults[Index].Name),
			CaptureNativeGraph(Chain.MacroGraph), MacroGraphBefore);
		bAllPassed &= TestEqual(FString::Printf(TEXT("%s restores the dirty baseline"), Faults[Index].Name),
			Fixture.Package->IsDirty(), bDirtyBefore);
		for (const FString& GuidText : Approved)
		{
			FGuid Guid;
			FGuid::Parse(GuidText, Guid);
			bAllPassed &= TestNotNull(FString::Printf(TEXT("%s restores approved GUID %s"), Faults[Index].Name, *GuidText),
				FCortexGraphMigrationOps::FindNodeByGuid(Fixture.Blueprint, Guid));
		}
		// The create node's own selection and scope are node state the authoring fingerprint cannot see:
		// removal clears the selection while the delegate output is unlinked, so the rollback has to bring
		// it back or a later compile reports a nameless create event.
		bAllPassed &= TestTrue(FString::Printf(TEXT("%s restores the create node's selected function"),
			Faults[Index].Name),
			Chain.CreateDelegate && Chain.CreateDelegate->GetFunctionName() == FName(TEXT("SetFocus")));
		bAllPassed &= TestNotNull(FString::Printf(TEXT("%s restores the create node's scope"),
			Faults[Index].Name),
			Chain.CreateDelegate ? Chain.CreateDelegate->GetScopeClass() : nullptr);
		bAllPassed &= TestNotNull(FString::Printf(TEXT("%s restores the create node's delegate signature"),
			Faults[Index].Name),
			Chain.CreateDelegate ? Chain.CreateDelegate->GetDelegateSignature() : nullptr);
		if (Faults[Index].bCompile)
		{
			// The compiled fault has to undo the generated class as well: one target compile happened
			// before the fault, the rollback recompiles, and the generated state comes back identical.
			bAllPassed &= TestEqual(FString::Printf(TEXT("%s reports the compile it performed"), Faults[Index].Name),
				Outcome.CompileStatus, FString(TEXT("compiled")));
			bAllPassed &= TestEqual(FString::Printf(TEXT("%s compiles exactly once before the fault"),
				Faults[Index].Name), Outcome.TargetCompileCount, 1);
			bAllPassed &= TestEqual(FString::Printf(TEXT("%s observes one target compile"), Faults[Index].Name),
				Operations.TargetCompiles, 1);
			bAllPassed &= TestTrue(FString::Printf(TEXT("%s runs a recovery compile"), Faults[Index].Name),
				Outcome.RecoveryCompileCount >= 1 && Operations.RecoveryCompiles >= 1);
			bAllPassed &= TestEqual(FString::Printf(TEXT("%s restores the generated state digest"), Faults[Index].Name),
				FCortexGraphPatchState::ComputeGeneratedStateDigest(Fixture.Blueprint), GeneratedBefore);
			UClass* const GeneratedAfter = Fixture.Blueprint->GeneratedClass;
			bAllPassed &= TestNotNull(FString::Printf(TEXT("%s restores the retired override in the generated class"),
				Faults[Index].Name),
				GeneratedAfter
					? GeneratedAfter->FindFunctionByName(TEXT("OnLegacyAlpha"), EIncludeSuperFlag::ExcludeSuper) : nullptr);
			bAllPassed &= TestNotNull(FString::Printf(TEXT("%s keeps the retained override in the generated class"),
				Faults[Index].Name),
				GeneratedAfter
					? GeneratedAfter->FindFunctionByName(TEXT("OnRetainedEvent"), EIncludeSuperFlag::ExcludeSuper) : nullptr);
		}
		else
		{
			bAllPassed &= TestEqual(FString::Printf(TEXT("%s performs no target compile"), Faults[Index].Name),
				Operations.TargetCompiles, 0);
			bAllPassed &= TestEqual(FString::Printf(TEXT("%s performs no recovery compile"), Faults[Index].Name),
				Operations.RecoveryCompiles, 0);
		}
		EndFixtureCase(Fixture);
	}
	FCortexGraphPatchOps::SetApplyFaultPointForTesting(NAME_None);
	FCortexGraphMigrationOps::ClearRetirementReadbackFaultForTesting();
	return bAllPassed;
}

// CortexSandbox #113 (Task 2 Step 3): the persistence outcomes of a class-admitted retirement - a
// verified save, a failed save that keeps the verified in-memory result dirty, and a post-save
// verification failure that reports a committed file requiring reopen instead of a rollback.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexGraphMigrationRetireAdditionalClassSaveTest,
	"Cortex.Graph.Authoring.Migration.Retire.PersistsClassAdmittedRetirementAndReportsSaveFailures",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexGraphMigrationRetireAdditionalClassSaveTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	using namespace CortexGraphMigrationRetireTest;
	bool bAllPassed = true;

	// (a) A verified save commits the compiled retirement.
	{
		FFixture Fixture;
		bAllPassed &= TestTrue(TEXT("verified-save fixture is created"),
			Fixture.Build(TEXT("BP_RetireAdmittedSave"), /*bRetainProducer=*/true));
		if (Fixture.Blueprint)
		{
			FAdmittedNodeChain Chain;
			const bool bSaveChainBuilt = Chain.Build(Fixture);
			bAllPassed &= TestTrue(FString::Printf(TEXT("verified-save chain is built: %s"), *Chain.Failure),
				bSaveChainBuilt);
			if (Chain.IsComplete())
			{
				const FString Filename = Fixture.Filename();
				bAllPassed &= TestTrue(TEXT("the class-admitted baseline saves"), Fixture.SaveToDisk());
				const TArray<uint8> BaselineBytes = ReadBytes(Filename);
				bAllPassed &= TestFalse(TEXT("the baseline is clean before the retirement"), Fixture.Package->IsDirty());
				TSharedPtr<FJsonObject> Request;
				TArray<FString> Approved;
				FCortexCommandResult Error;
				bAllPassed &= TestTrue(FString::Printf(TEXT("the save request is prepared: %s"), *Error.ErrorMessage),
					PrepareAdditionalRetirementRequest(Fixture, { Fixture.Alpha->NodeGuid.ToString() }, Chain.Guids(),
						TEXT("00000000-0000-0000-0000-000000114501"), Request, Approved, Error,
						/*bCompile=*/true, /*bSave=*/true));
				FOperations Operations;
				Operations.Begin();
				FCortexGraphPatchOutcome Outcome;
				bAllPassed &= TestTrue(FString::Printf(TEXT("the compiled retirement saves: %s"), *Error.ErrorMessage),
					FCortexGraphPatchOps::Execute(Fixture.Blueprint, Request, Outcome, Error));
				Operations.End();
				bAllPassed &= TestEqual(TEXT("the saved retirement reports applied"),
					Outcome.ApplyStatus, FString(TEXT("applied")));
				bAllPassed &= TestEqual(TEXT("the saved retirement reports the compile"),
					Outcome.CompileStatus, FString(TEXT("compiled")));
				bAllPassed &= TestEqual(TEXT("the saved retirement reports matched readback"),
					Outcome.ReadbackStatus, FString(TEXT("matched")));
				bAllPassed &= TestEqual(TEXT("the saved retirement reports saved"),
					Outcome.SaveStatus, FString(TEXT("saved")));
				bAllPassed &= TestEqual(TEXT("the saved retirement reports verified persistence"),
					Outcome.PostSaveStatus, FString(TEXT("verified")));
				bAllPassed &= TestTrue(TEXT("the saved retirement reports bSaved"), Outcome.bSaved);
				bAllPassed &= TestEqual(TEXT("one target compile precedes the save"), Operations.TargetCompiles, 1);
				bAllPassed &= TestEqual(TEXT("one explicit package save occurs"), Operations.Saves, 1);
				bAllPassed &= TestFalse(TEXT("the verified saved package is clean"), Fixture.Package->IsDirty());
				bAllPassed &= TestFalse(TEXT("the committed file carries the retirement"),
					SameBytes(BaselineBytes, ReadBytes(Filename)));
			}
			EndFixtureCase(Fixture);
		}
	}

	// (b) A failed save keeps the verified in-memory retirement dirty and never claims a rollback.
	{
		FFixture Fixture;
		bAllPassed &= TestTrue(TEXT("save-failure fixture is created"),
			Fixture.Build(TEXT("BP_RetireAdmittedSaveFail"), /*bRetainProducer=*/true));
		if (Fixture.Blueprint)
		{
			FAdmittedNodeChain Chain;
			const bool bSaveFailChainBuilt = Chain.Build(Fixture);
			bAllPassed &= TestTrue(FString::Printf(TEXT("save-failure chain is built: %s"), *Chain.Failure),
				bSaveFailChainBuilt);
			if (Chain.IsComplete())
			{
				const FString Filename = Fixture.Filename();
				bAllPassed &= TestTrue(TEXT("the save-failure baseline saves"), Fixture.SaveToDisk());
				const TArray<uint8> BaselineBytes = ReadBytes(Filename);
				TSharedPtr<FJsonObject> Request;
				TArray<FString> Approved;
				FCortexCommandResult Error;
				bAllPassed &= TestTrue(FString::Printf(TEXT("the save-failure request is prepared: %s"), *Error.ErrorMessage),
					PrepareAdditionalRetirementRequest(Fixture, { Fixture.Alpha->NodeGuid.ToString() }, Chain.Guids(),
						TEXT("00000000-0000-0000-0000-000000114502"), Request, Approved, Error,
						/*bCompile=*/true, /*bSave=*/true));
				FOperations Operations;
				Operations.Begin();
				FCortexGraphPatchOps::SetSaveFaultForTesting(true);
				FCortexGraphPatchOutcome Outcome;
				const bool bApplied = FCortexGraphPatchOps::Execute(Fixture.Blueprint, Request, Outcome, Error);
				FCortexGraphPatchOps::SetSaveFaultForTesting(false);
				Operations.End();
				bAllPassed &= TestFalse(TEXT("an injected save failure fails the retirement"), bApplied);
				bAllPassed &= TestEqual(TEXT("the save failure reports the save error code"),
					Error.ErrorCode, FString(CortexErrorCodes::SaveFailed));
				bAllPassed &= TestEqual(TEXT("the save failure keeps the verified in-memory result"),
					Outcome.ApplyStatus, FString(TEXT("applied")));
				bAllPassed &= TestEqual(TEXT("the save failure keeps the authoritative readback"),
					Outcome.ReadbackStatus, FString(TEXT("matched")));
				bAllPassed &= TestEqual(TEXT("the save failure reports the failed save"),
					Outcome.SaveStatus, FString(TEXT("failed")));
				bAllPassed &= TestEqual(TEXT("the save failure never claims post-save verification"),
					Outcome.PostSaveStatus, FString(TEXT("not_requested")));
				bAllPassed &= TestFalse(TEXT("the save failure keeps bSaved false"), Outcome.bSaved);
				bAllPassed &= TestNotEqual(TEXT("the save failure is never reported as a rollback"),
					Outcome.RollbackStatus, FString(TEXT("restored")));
				bAllPassed &= TestEqual(TEXT("the save failure performs zero real saves"), Operations.Saves, 0);
				bAllPassed &= TestTrue(TEXT("the save failure keeps the verified result dirty"),
					Fixture.Package->IsDirty());
				bAllPassed &= TestTrue(TEXT("the save failure leaves the file untouched"),
					SameBytes(BaselineBytes, ReadBytes(Filename)));
			}
			EndFixtureCase(Fixture);
		}
	}

	// (c) A post-save verification failure reports the committed file, never a rollback.
	{
		FFixture Fixture;
		bAllPassed &= TestTrue(TEXT("post-save fixture is created"),
			Fixture.Build(TEXT("BP_RetireAdmittedPostSave"), /*bRetainProducer=*/true));
		if (Fixture.Blueprint)
		{
			FAdmittedNodeChain Chain;
			const bool bPostSaveChainBuilt = Chain.Build(Fixture);
			bAllPassed &= TestTrue(FString::Printf(TEXT("post-save chain is built: %s"), *Chain.Failure),
				bPostSaveChainBuilt);
			if (Chain.IsComplete())
			{
				const FString Filename = Fixture.Filename();
				bAllPassed &= TestTrue(TEXT("the post-save baseline saves"), Fixture.SaveToDisk());
				const TArray<uint8> BaselineBytes = ReadBytes(Filename);
				TSharedPtr<FJsonObject> Request;
				TArray<FString> Approved;
				FCortexCommandResult Error;
				bAllPassed &= TestTrue(FString::Printf(TEXT("the post-save request is prepared: %s"), *Error.ErrorMessage),
					PrepareAdditionalRetirementRequest(Fixture, { Fixture.Alpha->NodeGuid.ToString() }, Chain.Guids(),
						TEXT("00000000-0000-0000-0000-000000114503"), Request, Approved, Error,
						/*bCompile=*/true, /*bSave=*/true));
				FOperations Operations;
				Operations.Begin();
				FCortexGraphPatchOps::SetPostSaveVerificationFaultForTesting(TEXT("asset_file"));
				FCortexGraphPatchOutcome Outcome;
				const bool bApplied = FCortexGraphPatchOps::Execute(Fixture.Blueprint, Request, Outcome, Error);
				FCortexGraphPatchOps::SetPostSaveVerificationFaultForTesting(NAME_None);
				Operations.End();
				bAllPassed &= TestFalse(TEXT("a failed post-save verification fails the retirement"), bApplied);
				bAllPassed &= TestEqual(TEXT("the post-save failure reports the verification error code"),
					Error.ErrorCode, FString(CortexErrorCodes::VerificationFailed));
				bAllPassed &= TestEqual(TEXT("the post-save failure still reports the real save"),
					Outcome.SaveStatus, FString(TEXT("saved")));
				bAllPassed &= TestTrue(TEXT("the post-save failure keeps bSaved true"), Outcome.bSaved);
				bAllPassed &= TestEqual(TEXT("the post-save failure reports the failed verification"),
					Outcome.PostSaveStatus, FString(TEXT("failed")));
				bAllPassed &= TestEqual(TEXT("the post-save failure preserves the applied outcome"),
					Outcome.ApplyStatus, FString(TEXT("applied")));
				bAllPassed &= TestNotEqual(TEXT("the post-save failure is never reported as a rollback"),
					Outcome.RollbackStatus, FString(TEXT("restored")));
				bAllPassed &= TestEqual(TEXT("the post-save failure performs exactly one real save"), Operations.Saves, 1);
				bAllPassed &= TestTrue(TEXT("the post-save failure names the failed check"),
					Error.ErrorMessage.Contains(TEXT("asset_file")));
				bAllPassed &= TestTrue(TEXT("the post-save failure carries the restart guidance"),
					Error.ErrorMessage.Contains(TEXT("restart the Editor")));
				bAllPassed &= TestTrue(TEXT("the post-save failure reports the same diagnostic in the outcome"),
					FString::Join(Outcome.Diagnostics, TEXT(" | ")).Contains(TEXT("asset_file")));
				bAllPassed &= TestFalse(TEXT("the post-save failure keeps the committed package clean"),
					Fixture.Package->IsDirty());
				bAllPassed &= TestFalse(TEXT("the post-save failure leaves the committed file on disk"),
					SameBytes(BaselineBytes, ReadBytes(Filename)));
				bAllPassed &= TestTrue(TEXT("post-save verification failure blocks subsequent mutations"), Outcome.bBlocked);
				const FString CommittedGraph = CaptureNativeGraph(Fixture.Graph);
				const TArray<uint8> CommittedBytes = ReadBytes(Filename);
				FCortexCommandRouter Router;
				Router.RegisterDomain(TEXT("graph"), TEXT("Cortex Graph"), TEXT("1.0.1"), MakeShared<FCortexGraphCommandHandler>());
				const FCortexCommandResult SecondApply = Router.Execute(TEXT("graph.apply_patch"), Request);
				bAllPassed &= TestFalse(TEXT("the public route refuses a second apply after committed verification failure"), SecondApply.bSuccess);
				bAllPassed &= TestTrue(TEXT("the second apply is refused by the mutation guard"),
					SecondApply.ErrorMessage.Contains(TEXT("blocked")));
				bAllPassed &= TestEqual(TEXT("the refused second apply preserves committed memory"), CaptureNativeGraph(Fixture.Graph), CommittedGraph);
				bAllPassed &= TestTrue(TEXT("the refused second apply preserves committed disk bytes"),
					SameBytes(CommittedBytes, ReadBytes(Filename)));
			}
			EndFixtureCase(Fixture);
		}
	}

	return bAllPassed;
}

// CortexSandbox #113 (Task 2 Step 3): after a class-admitted retirement the same reviewed request
// replays as an idempotent no-op, while a partially reintroduced identity is refused.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexGraphMigrationRetireAdditionalClassReplayTest,
	"Cortex.Graph.Authoring.Migration.Retire.ReplaysClassAdmittedRetirementAndRefusesPartialReplay",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexGraphMigrationRetireAdditionalClassReplayTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	using namespace CortexGraphMigrationRetireTest;
	FFixture Fixture;
	TestTrue(TEXT("retirement fixture with a cosmetic hook is created"),
		Fixture.Build(TEXT("BP_RetireAdmittedReplay"), /*bRetainProducer=*/true));
	if (!Fixture.Blueprint) { Fixture.Cleanup(); return false; }
	FAdmittedNodeChain Chain;
	const bool bChainBuilt = Chain.Build(Fixture);
	TestTrue(FString::Printf(TEXT("the page-shaped admitted chain is built: %s"), *Chain.Failure), bChainBuilt);
	if (!Chain.IsComplete()) { Fixture.Cleanup(); return false; }
	const FString AlphaGuid = Fixture.Alpha->NodeGuid.ToString();
	const TArray<FString> Requested = Chain.Guids();
	const FString LatentGuid = Chain.Latent->NodeGuid.ToString();

	TSharedPtr<FJsonObject> Request;
	TArray<FString> Approved;
	FCortexCommandResult Error;
	TestTrue(FString::Printf(TEXT("the replay request is prepared: %s"), *Error.ErrorMessage),
		PrepareAdditionalRetirementRequest(Fixture, { AlphaGuid }, Requested,
			TEXT("00000000-0000-0000-0000-000000114601"), Request, Approved, Error));
	FCortexGraphPatchOutcome Outcome;
	Error = FCortexCommandResult();
	TestTrue(FString::Printf(TEXT("the reviewed class-admitted retirement applies: %s"), *Error.ErrorMessage),
		FCortexGraphPatchOps::Execute(Fixture.Blueprint, Request, Outcome, Error));
	TestEqual(TEXT("the replay baseline retirement reports applied"),
		Outcome.ApplyStatus, FString(TEXT("applied")));

	// The all-absent replay removes nothing and republishes the same canonical removal set.
	FCortexGraphMigrationRetirePlan ReplayPlan;
	bool bReused = false;
	Error = FCortexCommandResult();
	TestTrue(FString::Printf(TEXT("the all-absent replay is accepted: %s"), *Error.ErrorMessage),
		PlanWithAdditionalNodes(Fixture, { AlphaGuid }, Requested, ReplayPlan, bReused, Error, true, Approved));
	TestTrue(TEXT("the all-absent replay is reported as reused"), bReused && ReplayPlan.bReused);
	TestFalse(TEXT("the reused plan is no longer awaiting approval"), ReplayPlan.bAwaitingApproval);
	TestEqual(TEXT("the reused plan publishes the exact approved removable set"),
		FString::Join(ReplayPlan.RemovableGuids, TEXT(",")), FString::Join(Approved, TEXT(",")));
	TestEqual(TEXT("the reused plan echoes the requested additional identities"),
		FString::Join(JsonStringArray(ReplayPlan.ToJson(), TEXT("additional_guids")), TEXT(",")),
		FString::Join(CanonicalGuids(Requested), TEXT(",")));

	// A partially reintroduced identity is refused instead of being retired twice.
	UK2Node_CallFunction* Reintroduced = NewObject<UK2Node_CallFunction>(Fixture.Graph, NAME_None, RF_Transactional);
	Reintroduced->NodeGuid = FGuid(LatentGuid);
	Fixture.Graph->AddNode(Reintroduced, true, false);
	FCortexGraphMigrationRetirePlan PartialPlan;
	Error = FCortexCommandResult();
	TestFalse(TEXT("a partially reintroduced additional identity is refused"),
		PlanWithAdditionalNodes(Fixture, { AlphaGuid }, Requested, PartialPlan, bReused, Error, true, Approved));
	TestEqual(TEXT("the partial replay refusal is INVALID_OPERATION"),
		Error.ErrorCode, FString(CortexErrorCodes::InvalidOperation));
	TestTrue(FString::Printf(TEXT("the partial replay refusal names the present and absent identities: %s"),
		*Error.ErrorMessage), Error.ErrorMessage.Contains(TEXT("retirement replay is partial"))
			&& Error.ErrorMessage.Contains(LatentGuid));

	Fixture.Cleanup();
	return true;
}


// CortexSandbox #113: class-specific admission never lifts structural ownership blocks. A bound
// subgraph, a bare tunnel and a requested node the cut cannot remove all keep their refusal.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexGraphMigrationRetireAdditionalUnadmittedClassTest,
	"Cortex.Graph.Authoring.Migration.Retire.RefusesUnadmittedAdditionalNodeClasses",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexGraphMigrationRetireAdditionalUnadmittedClassTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	using namespace CortexGraphMigrationRetireTest;
	FFixture Fixture;
	TestTrue(TEXT("retirement fixture is created"), Fixture.Build(TEXT("BP_RetireUnadmittedClasses"), true));
	if (!Fixture.Blueprint) { Fixture.Cleanup(); return false; }
	const FString AlphaGuid = Fixture.Alpha->NodeGuid.ToString();

	// A composite node owns the graph a removal would orphan: the structural reason is never lifted.
	UK2Node_Composite* Composite = NewObject<UK2Node_Composite>(Fixture.Graph, NAME_None, RF_Transactional);
	Composite->CreateNewGuid();
	Fixture.Graph->AddNode(Composite, true, false);
	Composite->PostPlacedNewNode();
	TestNotNull(TEXT("a real nested composite graph is created"), Composite->BoundGraph.Get());

	// A bare tunnel's pins belong to its twin gateway inside a bound graph.
	UK2Node_Tunnel* Tunnel = NewObject<UK2Node_Tunnel>(Fixture.Graph, NAME_None, RF_Transactional);
	Tunnel->CreateNewGuid();
	Tunnel->AllocateDefaultPins();
	Fixture.Graph->AddNode(Tunnel, true, false);

	auto ExpectRefusedNode = [&](const TCHAR* Label, UEdGraphNode* Node, const TArray<FString>& Fragments)
	{
		const FString CaseHashBefore = FCortexGraphPatchState::ComputeFingerprint(Fixture.Blueprint)
			->GetStringField(TEXT("graph_authoring_hash"));
		const int32 CaseNodesBefore = Fixture.Graph->Nodes.Num();
		const bool bCaseDirtyBefore = Fixture.Package->IsDirty();
		FCortexGraphMigrationRetirePlan CasePlan;
		bool bCaseReused = false;
		FCortexCommandResult CaseError;
		const bool bPlanned = PlanWithAdditionalNodes(Fixture, { AlphaGuid },
			{ Node->NodeGuid.ToString() }, CasePlan, bCaseReused, CaseError);
		TestFalse(FString::Printf(TEXT("%s is refused"), Label), bPlanned);
		TestEqual(FString::Printf(TEXT("%s reports INVALID_OPERATION"), Label),
			CaseError.ErrorCode, FString(CortexErrorCodes::InvalidOperation));
		TestTrue(FString::Printf(TEXT("%s refusal names the requested GUID: %s"), Label, *CaseError.ErrorMessage),
			CaseError.ErrorMessage.Contains(Node->NodeGuid.ToString()));
		TestTrue(FString::Printf(TEXT("%s refusal names class '%s': %s"), Label,
			*Node->GetClass()->GetName(), *CaseError.ErrorMessage),
			CaseError.ErrorMessage.Contains(Node->GetClass()->GetName()));
		for (const FString& Fragment : Fragments)
		{
			TestTrue(FString::Printf(TEXT("%s refusal names '%s': %s"), Label, *Fragment, *CaseError.ErrorMessage),
				CaseError.ErrorMessage.Contains(Fragment));
		}
		TestEqual(FString::Printf(TEXT("%s leaves the graph hash unchanged"), Label),
			FCortexGraphPatchState::ComputeFingerprint(Fixture.Blueprint)
				->GetStringField(TEXT("graph_authoring_hash")), CaseHashBefore);
		TestEqual(FString::Printf(TEXT("%s leaves the node count unchanged"), Label),
			Fixture.Graph->Nodes.Num(), CaseNodesBefore);
		TestEqual(FString::Printf(TEXT("%s leaves the dirty state unchanged"), Label),
			Fixture.Package->IsDirty(), bCaseDirtyBefore);
	};

	ExpectRefusedNode(TEXT("a composite node that owns its bound graph"), Composite,
		{ TEXT("not admitted"), TEXT("bound subgraph") });
	ExpectRefusedNode(TEXT("a bare tunnel whose pins belong to its twin"), Tunnel,
		{ TEXT("not admitted"), TEXT("tunnel") });
	// A requested node the cut cannot remove refuses with the partition reason instead of being
	// silently dropped. This mirrors the page's shared cast: `OldPathProducer` sits in the island only
	// as a data producer of the selected body, while its execution input comes from the retained
	// cosmetic body, so approving it would sever a retained execution link.
	UK2Node_CallFunction* OldPathProducer = Fixture.AddCall(
		UKismetSystemLibrary::StaticClass()->FindFunctionByName(TEXT("GetConsoleVariableStringValue")));
	TestNotNull(TEXT("the old-path producer exists"), OldPathProducer);
	TestTrue(TEXT("the old-path producer carries an execution input"),
		OldPathProducer && OldPathProducer->FindPin(TEXT("execute")));
	TestTrue(TEXT("the old-path producer feeds the selected entry's body"),
		OldPathProducer && Fixture.LinkPins(OldPathProducer, TEXT("ReturnValue"), Fixture.AlphaBody, TEXT("InString")));
	TestTrue(TEXT("the old-path producer is driven by the retained cosmetic body"),
		Fixture.LinkPins(Fixture.RetainedBody, TEXT("then"), OldPathProducer, TEXT("execute")));
	const FString HashBefore = FCortexGraphPatchState::ComputeFingerprint(Fixture.Blueprint)
		->GetStringField(TEXT("graph_authoring_hash"));
	const int32 NodesBefore = Fixture.Graph->Nodes.Num();
	const bool bDirtyBefore = Fixture.Package->IsDirty();
	if (!OldPathProducer)
	{
		Fixture.Cleanup();
		return false;
	}
	FCortexGraphMigrationRetirePlan CutPlan;
	bool bReused = false;
	FCortexCommandResult Error;
	TestFalse(TEXT("a requested island node fed by a retained execution link is refused"),
		PlanWithAdditionalNodes(Fixture, { AlphaGuid }, { OldPathProducer->NodeGuid.ToString() },
			CutPlan, bReused, Error));
	TestEqual(TEXT("the retained-execution-link refusal is INVALID_OPERATION"),
		Error.ErrorCode, FString(CortexErrorCodes::InvalidOperation));
	TestTrue(FString::Printf(TEXT("the cut refusal names the node, the reason and the retained producer: %s"),
		*Error.ErrorMessage), Error.ErrorMessage.Contains(OldPathProducer->NodeGuid.ToString())
			&& Error.ErrorMessage.Contains(TEXT("cannot be removed"))
			&& Error.ErrorMessage.Contains(Fixture.RetainedBodyGuid.ToString()));
	TestEqual(TEXT("the refused cut request leaves the graph hash unchanged"),
		FCortexGraphPatchState::ComputeFingerprint(Fixture.Blueprint)
			->GetStringField(TEXT("graph_authoring_hash")), HashBefore);
	TestEqual(TEXT("the refused cut request leaves the node count unchanged"),
		Fixture.Graph->Nodes.Num(), NodesBefore);
	TestEqual(TEXT("the refused cut request leaves the dirty state unchanged"),
		Fixture.Package->IsDirty(), bDirtyBefore);

	Fixture.Cleanup();
	return true;
}

// CortexSandbox #113: an approved consumer whose create-delegate producer stays outside the removal set
// is the one boundary where a removal can cut a retained node's delegate link and clear its selection.
// Retirement refuses that boundary before mutating anything - the producer is either a blocked class or a
// shared producer - so a retained node is never left in a state the compiler cannot resolve.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexGraphMigrationRetireRetainedDelegateProducerTest,
	"Cortex.Graph.Authoring.Migration.Retire.RefusesApprovedConsumerOfRetainedDelegateProducer",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexGraphMigrationRetireRetainedDelegateProducerTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	using namespace CortexGraphMigrationRetireTest;
	FFixture Fixture;
	TestTrue(TEXT("retirement fixture with a cosmetic hook is created"),
		Fixture.Build(TEXT("BP_RetireRetainedDelegateProducer"), /*bRetainProducer=*/true));
	if (!Fixture.Blueprint) { Fixture.Cleanup(); return false; }
	const UEdGraphSchema* const Schema = Fixture.Graph->GetSchema();
	FAdmittedNodeChain Chain;
	const bool bChainBuilt = Chain.Build(Fixture);
	TestTrue(FString::Printf(TEXT("the page-shaped admitted chain is built: %s"), *Chain.Failure), bChainBuilt);
	if (!Chain.IsComplete() || !Schema) { EndFixtureCase(Fixture); return false; }

	// A second consumer of the same create node, driven by the retained cosmetic hook: the producer now has
	// an approved consumer inside the selected island and a retained one outside it.
	FMulticastDelegateProperty* const ClickedDelegate =
		FindFProperty<FMulticastDelegateProperty>(UButton::StaticClass(), TEXT("OnClicked"));
	UK2Node_AddDelegate* const RetainedConsumer =
		NewObject<UK2Node_AddDelegate>(Fixture.Graph, NAME_None, RF_Transactional);
	if (ClickedDelegate)
	{
		RetainedConsumer->SetFromProperty(ClickedDelegate, /*bSelfContext=*/false, UButton::StaticClass());
	}
	RetainedConsumer->CreateNewGuid();
	RetainedConsumer->AllocateDefaultPins();
	Fixture.Graph->AddNode(RetainedConsumer, true, false);
	UEdGraphPin* const CreatedDelegateOut = FindTypedPin(Chain.CreateDelegate, EGPD_Output, UEdGraphSchema_K2::PC_Delegate);
	UEdGraphPin* const RetainedDelegateIn = FindTypedPin(RetainedConsumer, EGPD_Input, UEdGraphSchema_K2::PC_Delegate);
	TestNotNull(TEXT("the retained consumer exposes a delegate input"), RetainedDelegateIn);
	TestTrue(TEXT("the retained consumer is driven by the retained cosmetic body"),
		Fixture.LinkPins(Fixture.RetainedBody, TEXT("then"), RetainedConsumer, TEXT("execute")));
	TestTrue(TEXT("both consumers share the one create node"),
		CreatedDelegateOut && RetainedDelegateIn
			&& Schema->TryCreateConnection(CreatedDelegateOut, RetainedDelegateIn));

	const FString AlphaGuid = Fixture.Alpha->NodeGuid.ToString();
	const FString CreateDelegateGuid = Chain.CreateDelegate->NodeGuid.ToString();
	const FString AddDelegateGuid = Chain.AddDelegate->NodeGuid.ToString();
	const FString RetainedConsumerGuid = RetainedConsumer->NodeGuid.ToString();
	const FString HashBefore = FCortexGraphPatchState::ComputeFingerprint(Fixture.Blueprint)
		->GetStringField(TEXT("graph_authoring_hash"));
	const int32 NodesBefore = Fixture.Graph->Nodes.Num();
	const bool bDirtyBefore = Fixture.Package->IsDirty();
	const FString GraphBefore = CaptureNativeGraph(Fixture.Graph);

	// Case A: the producer is not requested, so the default class rule blocks it and the reviewed
	// retirement refuses instead of cutting its link.
	FCortexGraphMigrationRetirePlan UnlistedPlan;
	bool bReused = false;
	FCortexCommandResult Error;
	const TArray<FString> UnlistedRequest = { Chain.Latent->NodeGuid.ToString(), AddDelegateGuid,
		Chain.Macro->NodeGuid.ToString() };
	TestTrue(FString::Printf(TEXT("the unlisted-producer preview succeeds: %s"), *Error.ErrorMessage),
		PlanWithAdditionalNodes(Fixture, { AlphaGuid }, UnlistedRequest, UnlistedPlan, bReused, Error));
	TestTrue(TEXT("an unlisted create node in the island is published as blocked"),
		UnlistedPlan.Blocked.ContainsByPredicate(
			[&](const FCortexGraphPruneNode& Node) { return Node.NodeGuid == CreateDelegateGuid; }));
	const TArray<FString> UnlistedRemovable = UnlistedPlan.RemovableGuids;
	Error = FCortexCommandResult();
	TestFalse(TEXT("the reviewed retirement refuses the blocked retained producer"),
		PlanWithAdditionalNodes(Fixture, { AlphaGuid }, UnlistedRequest, UnlistedPlan, bReused, Error,
			true, UnlistedRemovable));
	TestEqual(TEXT("the blocked-producer refusal is INVALID_OPERATION"),
		Error.ErrorCode, FString(CortexErrorCodes::InvalidOperation));

	// Case B: the producer is requested, so class admission applies, but the retained consumer keeps it out
	// of the removable set and the request is refused with the cut reason naming that consumer.
	FCortexGraphMigrationRetirePlan ListedPlan;
	Error = FCortexCommandResult();
	TestFalse(TEXT("a requested producer a retained consumer depends on is refused"),
		PlanWithAdditionalNodes(Fixture, { AlphaGuid }, Chain.Guids(), ListedPlan, bReused, Error));
	TestEqual(TEXT("the retained-producer refusal is INVALID_OPERATION"),
		Error.ErrorCode, FString(CortexErrorCodes::InvalidOperation));
	TestTrue(FString::Printf(TEXT("the refusal names the producer and its retained consumer: %s"), *Error.ErrorMessage),
		Error.ErrorMessage.Contains(CreateDelegateGuid) && Error.ErrorMessage.Contains(TEXT("cannot be removed"))
			&& Error.ErrorMessage.Contains(RetainedConsumerGuid));

	// Neither refusal touched the graph: the retained producer keeps its selection, its links and the exact
	// authoring state, so no runtime or compile state was silently degraded.
	TestEqual(TEXT("the refused boundary leaves the graph hash unchanged"),
		FCortexGraphPatchState::ComputeFingerprint(Fixture.Blueprint)->GetStringField(TEXT("graph_authoring_hash")),
		HashBefore);
	TestEqual(TEXT("the refused boundary leaves the node count unchanged"), Fixture.Graph->Nodes.Num(), NodesBefore);
	TestEqual(TEXT("the refused boundary leaves the dirty state unchanged"),
		Fixture.Package->IsDirty(), bDirtyBefore);
	TestEqual(TEXT("the refused boundary leaves the exact graph unchanged"), CaptureNativeGraph(Fixture.Graph), GraphBefore);
	TestTrue(TEXT("the retained producer keeps its selected function"),
		Chain.CreateDelegate->GetFunctionName() == FName(TEXT("SetFocus")));
	TestTrue(TEXT("the retained producer keeps resolving its scope and signature"),
		CreatedDelegateOut && CreatedDelegateOut->LinkedTo.Num() == 2
			&& Chain.CreateDelegate->GetScopeClass() && Chain.CreateDelegate->GetDelegateSignature());

	EndFixtureCase(Fixture);
	return true;
}

// CortexSandbox #113: naming a node in the request never overrides the ownership cut. A reroute knot a
// retained consumer still depends on must stay out of the removable set and the request must refuse.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexGraphMigrationRetireAdditionalRetainedConsumerTest,
	"Cortex.Graph.Authoring.Migration.Retire.RefusesAdditionalNodeWithRetainedConsumer",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexGraphMigrationRetireAdditionalRetainedConsumerTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	using namespace CortexGraphMigrationRetireTest;
	FFixture Fixture;
	TestTrue(TEXT("retirement fixture with a cosmetic hook is created"),
		Fixture.Build(TEXT("BP_RetireAdditionalShared"), /*bRetainProducer=*/true));
	if (!Fixture.Blueprint) { Fixture.Cleanup(); return false; }
	const UEdGraphSchema* Schema = Fixture.Graph->GetSchema();

	// The ListView-style shape: one pure producer feeds the selected body and the retained cosmetic
	// body, so a reroute knot on that data path has a consumer the retirement must retain.
	UK2Node_Knot* SharedKnot = AddRerouteKnot(Fixture);
	UEdGraphPin* ProducerOut = Fixture.Producer->FindPin(TEXT("ReturnValue"));
	UEdGraphPin* AlphaBodyIn = Fixture.AlphaBody->FindPin(TEXT("InString"));
	UEdGraphPin* RetainedBodyIn = Fixture.RetainedBody->FindPin(TEXT("InString"));
	TestNotNull(TEXT("the data reroute knot exists"), SharedKnot);
	TestTrue(TEXT("the shared producer feeds the retained cosmetic body before the reroute"),
		ProducerOut && RetainedBodyIn && ProducerOut->LinkedTo.Contains(RetainedBodyIn));
	if (!SharedKnot || !ProducerOut || !AlphaBodyIn || !RetainedBodyIn || !Schema)
	{
		Fixture.Cleanup();
		return false;
	}
	ProducerOut->BreakLinkTo(AlphaBodyIn);
	TestTrue(TEXT("the data reroute knot carries the shared producer's output"),
		Schema->TryCreateConnection(ProducerOut, SharedKnot->GetInputPin()));
	TestTrue(TEXT("the data reroute knot feeds the selected entry's body"),
		Schema->TryCreateConnection(SharedKnot->GetOutputPin(), AlphaBodyIn));
	TestTrue(TEXT("the data reroute knot also feeds the retained cosmetic body"),
		Schema->TryCreateConnection(SharedKnot->GetOutputPin(), RetainedBodyIn));
	TestTrue(TEXT("the retained cosmetic body is now fed by the reroute knot"),
		RetainedBodyIn->LinkedTo.Contains(SharedKnot->GetOutputPin()));

	const FString AlphaGuid = Fixture.Alpha->NodeGuid.ToString();
	const FString SharedKnotGuid = SharedKnot->NodeGuid.ToString();
	const FString HashBefore = FCortexGraphPatchState::ComputeFingerprint(Fixture.Blueprint)
		->GetStringField(TEXT("graph_authoring_hash"));
	const int32 NodesBefore = Fixture.Graph->Nodes.Num();
	const bool bDirtyBefore = Fixture.Package->IsDirty();
	FCortexGraphMigrationRetirePlan RefusedPlan;
	bool bReused = false;
	FCortexCommandResult Error;
	TestFalse(TEXT("a requested additional node a retained consumer depends on is refused"),
		PlanWithAdditionalNodes(Fixture, { AlphaGuid }, { SharedKnotGuid }, RefusedPlan, bReused, Error));
	TestEqual(TEXT("the retained-consumer refusal is INVALID_OPERATION"),
		Error.ErrorCode, FString(CortexErrorCodes::InvalidOperation));
	TestTrue(TEXT("the retained-consumer refusal names the requested node"),
		Error.ErrorMessage.Contains(SharedKnotGuid));
	TestTrue(FString::Printf(TEXT("the retained-consumer refusal is explicable: %s"), *Error.ErrorMessage),
		Error.ErrorMessage.Contains(TEXT("cannot be removed")));
	TestTrue(TEXT("the retained-consumer refusal names the retained consumer that blocks it"),
		Error.ErrorMessage.Contains(TEXT("consumed by retained consumer"))
			&& Error.ErrorMessage.Contains(Fixture.RetainedBodyGuid.ToString()));
	TestEqual(TEXT("the refused retained-consumer request leaves the graph hash unchanged"),
		FCortexGraphPatchState::ComputeFingerprint(Fixture.Blueprint)
			->GetStringField(TEXT("graph_authoring_hash")), HashBefore);
	TestEqual(TEXT("the refused retained-consumer request leaves the node count unchanged"),
		Fixture.Graph->Nodes.Num(), NodesBefore);
	TestEqual(TEXT("the refused retained-consumer request leaves the dirty state unchanged"),
		Fixture.Package->IsDirty(), bDirtyBefore);
	TestNotNull(TEXT("the retained consumer is still in the graph"),
		FCortexGraphMigrationOps::FindNodeByGuid(Fixture.Blueprint, Fixture.RetainedBodyGuid));
	TestNotNull(TEXT("the requested reroute knot is still in the graph"),
		FCortexGraphMigrationOps::FindNodeByGuid(Fixture.Blueprint, SharedKnot->NodeGuid));

	Fixture.Cleanup();
	return true;
}

namespace CortexGraphMigrationRetireTest
{
/**
 * The node GUIDs one published partition list names, read from the canonical `<guid> <class> (...)`
 * prefix of every line, so a comparison is only ever about identities.
 */
TArray<FString> LineGuidPrefixes(const TArray<FString>& Lines)
{
	TArray<FString> Guids;
	for (const FString& Line : Lines)
	{
		int32 Space = INDEX_NONE;
		if (Line.FindChar(TEXT(' '), Space) && Space > 0) Guids.Add(Line.Left(Space));
	}
	Guids.Sort();
	return Guids;
}

/**
 * One fixture that carries all three review-critical record families of a retirement inventory past
 * the shared diagnostics bound.
 *
 * Every unit is built through real engine APIs on the selected entry's execution island: a removable
 * body on the exec chain consumes the output of an unrequested reroute knot (a blocked class), and
 * the data producer feeding that knot is itself used by a consumer outside the island. The partition
 * therefore has to publish, per unit, one *shared* producer, one *blocked* knot and one *boundary*
 * edge from the blocked knot into the removable body - exactly the families a native trim would have
 * silently reduced to fifteen lines while still reporting `complete: true`.
 *
 * The record lists are read back from the nodes the fixture really created, so a wiring failure is
 * reported instead of leaving the test to compare against an assumed set.
 */
struct FPartitionBoundaryRecords
{
	/** Canonical ascending GUIDs of the shared data producers, one per unit. */
	TArray<FString> SharedGuids;
	/** Canonical ascending GUIDs of the blocked reroute knots, one per unit. */
	TArray<FString> BlockedGuids;
	/** Canonical ascending `from.pin -> to.pin` boundary edges, one per unit. */
	TArray<FString> BoundaryEdges;
	/** Canonical ascending GUIDs of the removable bodies that consume the blocked knots. */
	TArray<FString> BodyGuids;
	/** Why `Build` returned false: the first stage that could not be wired, with the observed pins. */
	FString Failure;

	bool Build(FFixture& Fixture, const int32 Units)
	{
		Failure.Reset();
		SharedGuids.Reset();
		BlockedGuids.Reset();
		BoundaryEdges.Reset();
		BodyGuids.Reset();
		if (Units <= 0) { Failure = TEXT("the record count must be positive"); return false; }
		const UEdGraphSchema* const Schema = Fixture.Graph ? Fixture.Graph->GetSchema() : nullptr;
		if (!Schema) { Failure = TEXT("the fixture graph has no schema"); return false; }
		UFunction* const ConvTarget = UKismetStringLibrary::StaticClass()->FindFunctionByName(TEXT("Conv_IntToString"));
		UFunction* const PrintTarget = UKismetSystemLibrary::StaticClass()->FindFunctionByName(TEXT("PrintString"));
		if (!ConvTarget || !PrintTarget) { Failure = TEXT("the engine call targets are not available"); return false; }
		if (!Fixture.AlphaBody) { Failure = TEXT("the selected entry body is missing"); return false; }

		UK2Node_CallFunction* Previous = Fixture.AlphaBody;
		for (int32 Index = 0; Index < Units; ++Index)
		{
			UK2Node_CallFunction* const Body = Fixture.AddCall(PrintTarget);
			UK2Node_CallFunction* const Producer = Fixture.AddCall(ConvTarget);
			UK2Node_CallFunction* const RetainedConsumer = Fixture.AddCall(PrintTarget);
			UK2Node_Knot* const BlockedKnot = AddRerouteKnot(Fixture);
			if (!Body || !Producer || !RetainedConsumer || !BlockedKnot)
			{
				Failure = FString::Printf(TEXT("unit %d: the fixture nodes could not be created"), Index);
				return false;
			}
			UEdGraphPin* const KnotIn = BlockedKnot->GetInputPin();
			UEdGraphPin* const KnotOut = BlockedKnot->GetOutputPin();
			UEdGraphPin* const BodyIn = Body->FindPin(TEXT("InString"));
			if (!KnotIn || !KnotOut || !BodyIn)
			{
				Failure = FString::Printf(TEXT("unit %d: a knot or body pin is missing [knot: %s] [body: %s]"), Index,
					*PinSummary(BlockedKnot), *PinSummary(Body));
				return false;
			}
			if (!ConnectTypedPins(Schema, Previous->FindPin(TEXT("then")), Body->FindPin(TEXT("execute")),
				TEXT("partition exec chain"), Failure)) return false;
			// The unrequested reroute stays blocked, so the producer feeding it is retained by a consumer
			// the partition never offers for removal, and the knot's own edge into the removable body is
			// the boundary edge the caller has to accept.
			if (!ConnectTypedPins(Schema, Producer->FindPin(TEXT("ReturnValue")), KnotIn,
				TEXT("partition blocked reroute input"), Failure)) return false;
			if (!ConnectTypedPins(Schema, KnotOut, BodyIn, TEXT("partition boundary data edge"), Failure)) return false;
			if (!ConnectTypedPins(Schema, Producer->FindPin(TEXT("ReturnValue")),
				RetainedConsumer->FindPin(TEXT("InString")), TEXT("partition retained consumer"), Failure)) return false;
			SharedGuids.Add(Producer->NodeGuid.ToString());
			BlockedGuids.Add(BlockedKnot->NodeGuid.ToString());
			BoundaryEdges.Add(FString::Printf(TEXT("%s.%s -> %s.%s"), *BlockedKnot->NodeGuid.ToString(),
				*KnotOut->PinName.ToString(), *Body->NodeGuid.ToString(), *BodyIn->PinName.ToString()));
			BodyGuids.Add(Body->NodeGuid.ToString());
			Previous = Body;
		}
		SharedGuids.Sort();
		BlockedGuids.Sort();
		BoundaryEdges.Sort();
		BodyGuids.Sort();
		return true;
	}
};

/** The graph-authoring hash of the live fixture, so purity assertions compare the real asset state. */
FString LiveRetireHash(UBlueprint* Blueprint)
{
	const TSharedPtr<FJsonObject> Fingerprint = FCortexGraphPatchState::ComputeFingerprint(Blueprint);
	return Fingerprint.IsValid() ? Fingerprint->GetStringField(TEXT("graph_authoring_hash")) : FString();
}

/** The shared refusal of one unpublishable inventory, asserted on both the message and its cause. */
void AssertUnpublishableInventoryRefusal(
	FAutomationTestBase& Test,
	const FCortexCommandResult& Result,
	const FPartitionBoundaryRecords& Records,
	const int32 Units)
{
	const FString Context = FString::Printf(TEXT("%s: %s"), *Result.ErrorCode, *Result.ErrorMessage);
	Test.TestFalse(FString::Printf(TEXT("the unpublishable inventory is refused [%s]"), *Context), Result.bSuccess);
	Test.TestEqual(TEXT("the unpublishable inventory refusal is LIMIT_EXCEEDED"),
		Result.ErrorCode, FString(CortexErrorCodes::LimitExceeded));
	Test.TestTrue(FString::Printf(TEXT("the refusal names the bridge response limit [%s]"), *Result.ErrorMessage),
		Result.ErrorMessage.Contains(FString::Printf(TEXT("%d"), FCortexGraphMigrationOps::BridgeResponseCharLimit)));
	Test.TestTrue(TEXT("the refusal carries the published outcome envelope"), Result.ErrorDetails.IsValid());
	Test.TestFalse(TEXT("the refusal publishes no shared identity list"),
		Result.ErrorDetails.IsValid() && Result.ErrorDetails->HasField(TEXT("shared")));
	Test.TestFalse(TEXT("the refusal publishes no blocked identity list"),
		Result.ErrorDetails.IsValid() && Result.ErrorDetails->HasField(TEXT("blocked_nodes")));
	Test.TestFalse(TEXT("the refusal publishes no boundary edge list"),
		Result.ErrorDetails.IsValid() && Result.ErrorDetails->HasField(TEXT("external_edges")));
	Test.TestFalse(TEXT("the refusal publishes no removable set"),
		Result.ErrorDetails.IsValid() && Result.ErrorDetails->HasField(TEXT("removable")));
	Test.TestFalse(TEXT("the refusal publishes no completeness claim"),
		Result.ErrorDetails.IsValid() && Result.ErrorDetails->HasField(TEXT("complete")));
	Test.TestFalse(TEXT("the refusal publishes no approval state"),
		Result.ErrorDetails.IsValid() && Result.ErrorDetails->HasField(TEXT("awaiting_approval")));

	const TSharedPtr<FJsonObject>* Cause = nullptr;
	const bool bHasCause = Result.ErrorDetails.IsValid()
		&& Result.ErrorDetails->TryGetObjectField(TEXT("error_details"), Cause) && Cause && Cause->IsValid();
	Test.TestTrue(TEXT("the refusal carries the native cause"), bHasCause);
	if (!bHasCause) return;
	Test.TestEqual(TEXT("the cause publishes the bridge response limit"),
		(*Cause)->GetIntegerField(TEXT("response_char_limit")), FCortexGraphMigrationOps::BridgeResponseCharLimit);
	Test.TestEqual(TEXT("the cause publishes the publishable inventory bound"),
		(*Cause)->GetIntegerField(TEXT("publishable_inventory_chars")),
		FCortexGraphMigrationOps::MaxPublishableRetirementInventoryChars);
	Test.TestTrue(TEXT("the cause reports a measured inventory above the bound"),
		(*Cause)->GetIntegerField(TEXT("inventory_chars"))
			> FCortexGraphMigrationOps::MaxPublishableRetirementInventoryChars);
	Test.TestEqual(TEXT("the cause reports the published shared node count"),
		(*Cause)->GetIntegerField(TEXT("shared_nodes")), Records.SharedGuids.Num());
	Test.TestEqual(TEXT("the cause reports the published blocked node count"),
		(*Cause)->GetIntegerField(TEXT("blocked_nodes")), Records.BlockedGuids.Num());
	Test.TestEqual(TEXT("the cause reports the published boundary edge count"),
		(*Cause)->GetIntegerField(TEXT("boundary_edges")), Records.BoundaryEdges.Num());
	Test.TestEqual(TEXT("the cause reports the reviewed node count"),
		(*Cause)->GetIntegerField(TEXT("reviewed_nodes")), Units * 2);
	Test.TestFalse(TEXT("the cause never claims completeness"), (*Cause)->GetBoolField(TEXT("complete")));
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexGraphMigrationRetireCompleteInventoryBoundTest,
	"Cortex.Graph.Authoring.Migration.Retire.PublishesCompleteInventoryBeyondDiagnosticsBound",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexGraphMigrationRetireCompleteInventoryBoundTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	using namespace CortexGraphMigrationRetireTest;
	FFixture Fixture;
	TestTrue(TEXT("the complete-inventory fixture is created"), Fixture.Build(TEXT("BP_RetireCompleteInventory")));
	if (!Fixture.Blueprint) { Fixture.Cleanup(); return false; }

	const int32 Units = 20;
	FPartitionBoundaryRecords Records;
	TestTrue(FString::Printf(TEXT("the boundary records are built: %s"), *Records.Failure), Records.Build(Fixture, Units));
	if (Records.SharedGuids.Num() != Units || Records.BlockedGuids.Num() != Units
		|| Records.BoundaryEdges.Num() != Units)
	{
		Fixture.Cleanup();
		return false;
	}
	// The families are deliberately larger than the shared diagnostics bound, so the assertions below are
	// about the bound and not about a set that simply fits under it.

	// Twenty cached compiler messages: the explicitly non-critical diagnostics list stays bounded and
	// says so, while the review-critical identity lists are never conflated with it.
	for (int32 Index = 0; Index < 20; ++Index)
	{
		UK2Node_CallFunction* const Diagnostic =
			Fixture.AddCall(UKismetSystemLibrary::StaticClass()->FindFunctionByName(TEXT("PrintString")));
		if (!Diagnostic) { Fixture.Cleanup(); return false; }
		Diagnostic->bHasCompilerMessage = true;
		Diagnostic->ErrorMsg = FString::Printf(TEXT("retirement boundary diagnostic %d"), Index);
	}

	const TArray<FString> Entries = { Fixture.AlphaGuid.ToString(), Fixture.BetaGuid.ToString() };
	const FString HashBefore = LiveRetireHash(Fixture.Blueprint);
	const FString GraphBefore = CaptureNativeGraph(Fixture.Graph);
	const int32 NodesBefore = Fixture.Graph ? Fixture.Graph->Nodes.Num() : 0;
	const bool bDirtyBefore = Fixture.Package->IsDirty();

	FCortexGraphMigrationRetirePlan PlanValue;
	bool bReused = false;
	FCortexCommandResult Error;
	TestTrue(FString::Printf(TEXT("the complete inventory previews: %s"), *Error.ErrorMessage),
		Plan(Fixture, Entries, PlanValue, bReused, Error));
	TestTrue(TEXT("the completed partition claims completeness"), PlanValue.bComplete);
	const TSharedPtr<FJsonObject> Inventory = FCortexGraphMigrationOps::MakeRetirementInventory(PlanValue.ToJson());
	TestNotNull(TEXT("the retirement inventory is created"), Inventory.Get());
	if (!Inventory.IsValid()) { Fixture.Cleanup(); return false; }

	TestTrue(TEXT("the inventory claims completeness"), Inventory->GetBoolField(TEXT("complete")));
	TestTrue(TEXT("the non-critical diagnostics list is still bounded and flagged"),
		Inventory->GetBoolField(TEXT("preexisting_diagnostics_truncated")));
	TestTrue(TEXT("the bounded diagnostics list carries its omission marker"),
		JsonStringArray(Inventory, TEXT("preexisting_diagnostics")).Contains(TEXT("additional compiler diagnostics omitted")));

	const TArray<FString> SharedLines = JsonStringArray(Inventory, TEXT("shared"));
	const TArray<FString> BlockedLines = JsonStringArray(Inventory, TEXT("blocked_nodes"));
	const TArray<FString> EdgeLines = JsonStringArray(Inventory, TEXT("external_edges"));
	const TArray<FString> EntryLines = JsonStringArray(Inventory, TEXT("selected_entries"));
	const TArray<FString> DiagnosticLines = JsonStringArray(Inventory, TEXT("preexisting_diagnostics"));
	TestTrue(TEXT("the non-critical diagnostics list stays bounded"), DiagnosticLines.Num() <= 16);
	TestTrue(TEXT("every review-critical identity list is longer than the bounded diagnostics list"),
		DiagnosticLines.Num() < SharedLines.Num() && DiagnosticLines.Num() < BlockedLines.Num()
			&& DiagnosticLines.Num() < EdgeLines.Num());
	TestEqual(TEXT("every shared record is published"), SharedLines.Num(), Units);
	TestEqual(TEXT("every blocked record is published"), BlockedLines.Num(), Units);
	TestEqual(TEXT("every boundary edge is published"), EdgeLines.Num(), Units);
	TestEqual(TEXT("every selected entry is published"), EntryLines.Num(), Entries.Num());
	TestEqual(TEXT("the published shared identities are exactly the shared producers"),
		FString::Join(LineGuidPrefixes(SharedLines), TEXT(",")), FString::Join(Records.SharedGuids, TEXT(",")));
	TestEqual(TEXT("the published blocked identities are exactly the blocked reroutes"),
		FString::Join(LineGuidPrefixes(BlockedLines), TEXT(",")), FString::Join(Records.BlockedGuids, TEXT(",")));
	TArray<FString> PublishedEdges = EdgeLines;
	PublishedEdges.Sort();
	TestEqual(TEXT("the published boundary edges are exactly the covered edges"),
		FString::Join(PublishedEdges, TEXT("\n")), FString::Join(Records.BoundaryEdges, TEXT("\n")));
	// `complete: true` must mean the review-critical display is whole: no identity list may carry an
	// omission marker, or the caller would approve a set it never saw.
	const FString OmissionMarker = TEXT("additional compiler diagnostics omitted");
	TestFalse(TEXT("the shared identity list carries no omission marker"), SharedLines.Contains(OmissionMarker));
	TestFalse(TEXT("the blocked identity list carries no omission marker"), BlockedLines.Contains(OmissionMarker));
	TestFalse(TEXT("the boundary edge list carries no omission marker"), EdgeLines.Contains(OmissionMarker));
	TestFalse(TEXT("the selected entry list carries no omission marker"), EntryLines.Contains(OmissionMarker));

	// The accepted side of the transport boundary: the inventory this preview publishes really fits the
	// publishable budget, so the refusal below is only for what could not be carried whole.
	int32 EncodedChars = 0;
	TestTrue(TEXT("the published inventory is measurable"),
		FCortexGraphMigrationOps::EncodedResponseChars(Inventory, EncodedChars));
	TestTrue(FString::Printf(TEXT("the accepted inventory fits the publishable bound (%d of %d)"),
		EncodedChars, FCortexGraphMigrationOps::MaxPublishableRetirementInventoryChars),
		EncodedChars > 0 && EncodedChars <= FCortexGraphMigrationOps::MaxPublishableRetirementInventoryChars);

	TestEqual(TEXT("the discovery preview changes no fingerprint"), LiveRetireHash(Fixture.Blueprint), HashBefore);
	TestEqual(TEXT("the discovery preview leaves every node, pin, flag and link"),
		CaptureNativeGraph(Fixture.Graph), GraphBefore);
	TestEqual(TEXT("the discovery preview leaves the node count"),
		Fixture.Graph ? Fixture.Graph->Nodes.Num() : -1, NodesBefore);
	TestEqual(TEXT("the discovery preview leaves the dirty baseline"), Fixture.Package->IsDirty(), bDirtyBefore);

	Fixture.Cleanup();
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexGraphMigrationRetireRefusesUnpublishableInventoryTest,
	"Cortex.Graph.Authoring.Migration.Retire.RefusesInventoryTheBridgeCouldNotPublish",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexGraphMigrationRetireRefusesUnpublishableInventoryTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	using namespace CortexGraphMigrationRetireTest;
	FFixture Fixture;
	TestTrue(TEXT("the oversized-inventory fixture is created"), Fixture.Build(TEXT("BP_RetireOversizedInventory")));
	if (!Fixture.Blueprint) { Fixture.Cleanup(); return false; }

	// Far past the bridge budget: 120 units of the three record families encode to well over the
	// publishable bound, so the whole inventory cannot be handed to the caller unclipped.
	const int32 Units = 120;
	FPartitionBoundaryRecords Records;
	TestTrue(FString::Printf(TEXT("the oversized boundary records are built: %s"), *Records.Failure), Records.Build(Fixture, Units));
	if (Records.SharedGuids.Num() != Units || Records.BlockedGuids.Num() != Units
		|| Records.BoundaryEdges.Num() != Units)
	{
		Fixture.Cleanup();
		return false;
	}

	const FString HashBefore = LiveRetireHash(Fixture.Blueprint);
	const FString GraphBefore = CaptureNativeGraph(Fixture.Graph);
	const int32 NodesBefore = Fixture.Graph ? Fixture.Graph->Nodes.Num() : 0;
	const bool bDirtyBefore = Fixture.Package->IsDirty();

	// The published route exactly as a caller reaches it: the preview must refuse instead of publishing a
	// complete inventory the bridge would clip while keeping `complete: true`.
	TSharedPtr<FJsonObject> Request = MakeShared<FJsonObject>();
	Request->SetStringField(TEXT("asset_path"), Fixture.Blueprint->GetPathName());
	Request->SetStringField(TEXT("patch_id"), TEXT("00000000-0000-0000-0000-000000115501"));
	Request->SetObjectField(TEXT("expected_fingerprint"), FCortexGraphPatchState::ComputeFingerprint(Fixture.Blueprint));
	Request->SetArrayField(TEXT("nodes"), {});
	Request->SetArrayField(TEXT("connections"), {});
	Request->SetArrayField(TEXT("pin_updates"), {});
	Request->SetBoolField(TEXT("dry_run"), true);
	Request->SetBoolField(TEXT("compile"), false);
	Request->SetBoolField(TEXT("save"), false);
	Request->SetBoolField(TEXT("allow_noop"), false);
	Request->SetObjectField(TEXT("migration"),
		Fixture.Migration({ Fixture.AlphaGuid.ToString(), Fixture.BetaGuid.ToString() }));

	FCortexCommandRouter Router;
	Router.RegisterDomain(TEXT("graph"), TEXT("Cortex Graph"), TEXT("1.0.1"), MakeShared<FCortexGraphCommandHandler>());
	const FCortexCommandResult Result = Router.Execute(TEXT("graph.apply_patch"), Request);
	AssertUnpublishableInventoryRefusal(*this, Result, Records, Units);

	// Fail closed before approval and before mutation: the asset is exactly the fixture again, and the
	// planner route refuses the same inventory identically.
	FCortexGraphMigrationRetirePlan PlanValue;
	bool bReused = false;
	FCortexCommandResult PlanError;
	TestFalse(TEXT("the planner route refuses the same unpublishable inventory"),
		Plan(Fixture, { Fixture.AlphaGuid.ToString(), Fixture.BetaGuid.ToString() }, PlanValue, bReused, PlanError));
	TestEqual(TEXT("the planner refusal is LIMIT_EXCEEDED"),
		PlanError.ErrorCode, FString(CortexErrorCodes::LimitExceeded));

	TestEqual(TEXT("the refused preview changes no fingerprint"), LiveRetireHash(Fixture.Blueprint), HashBefore);
	TestEqual(TEXT("the refused preview leaves every node, pin, flag and link"),
		CaptureNativeGraph(Fixture.Graph), GraphBefore);
	TestEqual(TEXT("the refused preview leaves the node count"),
		Fixture.Graph ? Fixture.Graph->Nodes.Num() : -1, NodesBefore);
	TestEqual(TEXT("the refused preview leaves the dirty baseline"), Fixture.Package->IsDirty(), bDirtyBefore);

	Fixture.Cleanup();
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexGraphMigrationRetireDiscoveryHashCannotAuthorizeApplyTest,
	"Cortex.Graph.Authoring.Migration.Retire.DiscoveryHashCannotAuthorizeApply",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexGraphMigrationRetireDiscoveryHashCannotAuthorizeApplyTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	using namespace CortexGraphMigrationRetireTest;
	FFixture Fixture;
	TestTrue(TEXT("the hash-sequencing fixture is created"), Fixture.Build(TEXT("BP_RetireDiscoveryHash")));
	if (!Fixture.Blueprint) { Fixture.Cleanup(); return false; }

	const TArray<FString> Entries = { Fixture.AlphaGuid.ToString(), Fixture.BetaGuid.ToString() };
	TSharedPtr<FJsonObject> Request = MakeShared<FJsonObject>();
	Request->SetStringField(TEXT("asset_path"), Fixture.Blueprint->GetPathName());
	Request->SetStringField(TEXT("patch_id"), TEXT("00000000-0000-0000-0000-000000115601"));
	Request->SetObjectField(TEXT("expected_fingerprint"), FCortexGraphPatchState::ComputeFingerprint(Fixture.Blueprint));
	Request->SetArrayField(TEXT("nodes"), {});
	Request->SetArrayField(TEXT("connections"), {});
	Request->SetArrayField(TEXT("pin_updates"), {});
	Request->SetBoolField(TEXT("dry_run"), true);
	Request->SetBoolField(TEXT("compile"), false);
	Request->SetBoolField(TEXT("save"), false);
	Request->SetBoolField(TEXT("allow_noop"), false);
	Request->SetObjectField(TEXT("migration"), Fixture.Migration(Entries));

	// Step 1: the discovery preview, which names no approved set and can only be reviewed.
	FCortexGraphPreparedPatch Discovery;
	FCortexCommandResult Error;
	TestTrue(FString::Printf(TEXT("the discovery preview succeeds: %s"), *Error.ErrorMessage),
		FCortexGraphPatchOps::Preflight(Fixture.Blueprint, Request, Discovery, Error));
	bool bDiscoveryAwaiting = false;
	TestTrue(TEXT("the discovery preview publishes its awaiting-approval state"),
		Discovery.RetirementPlan.IsValid()
			&& Discovery.RetirementPlan->TryGetBoolField(TEXT("awaiting_approval"), bDiscoveryAwaiting)
			&& bDiscoveryAwaiting);
	const FString DiscoveryHash = Discovery.ValidationHash;
	const TArray<FString> Approved = CanonicalGuids(JsonStringArray(Discovery.RetirementPlan, TEXT("removable_guids")));
	TestTrue(TEXT("the discovery preview publishes the removable set to review"), Approved.Num() > 0);

	// Step 2: the reviewed preview, which echoes exactly the published removable set.
	TSharedPtr<FJsonObject> ReviewedMigration = Fixture.Migration(Entries);
	TArray<TSharedPtr<FJsonValue>> ApprovalValues;
	for (const FString& Guid : Approved) ApprovalValues.Add(MakeShared<FJsonValueString>(Guid));
	ReviewedMigration->SetArrayField(TEXT("approved_node_guids"), ApprovalValues);
	Request->SetObjectField(TEXT("migration"), ReviewedMigration);
	FCortexGraphPreparedPatch Reviewed;
	Error = FCortexCommandResult();
	TestTrue(FString::Printf(TEXT("the reviewed preview succeeds: %s"), *Error.ErrorMessage),
		FCortexGraphPatchOps::Preflight(Fixture.Blueprint, Request, Reviewed, Error));
	bool bReviewedAwaiting = true;
	TestTrue(TEXT("the reviewed preview publishes its approved state"),
		Reviewed.RetirementPlan.IsValid()
			&& Reviewed.RetirementPlan->TryGetBoolField(TEXT("awaiting_approval"), bReviewedAwaiting)
			&& !bReviewedAwaiting);
	TestTrue(TEXT("the reviewed preview hash is the discovery hash of a different intent"),
		!DiscoveryHash.IsEmpty() && Reviewed.ValidationHash != DiscoveryHash);

	// Step 3a: the discovery hash cannot authorize the approved apply.
	Request->SetBoolField(TEXT("dry_run"), false);
	Request->SetStringField(TEXT("expected_validation_hash"), DiscoveryHash);
	FCortexCommandRouter Router;
	Router.RegisterDomain(TEXT("graph"), TEXT("Cortex Graph"), TEXT("1.0.1"), MakeShared<FCortexGraphCommandHandler>());
	const FString HashBefore = LiveRetireHash(Fixture.Blueprint);
	const int32 NodesBefore = Fixture.Graph ? Fixture.Graph->Nodes.Num() : 0;
	const FCortexCommandResult Refused = Router.Execute(TEXT("graph.apply_patch"), Request);
	TestFalse(FString::Printf(TEXT("the discovery hash cannot authorize the apply [%s: %s]"),
		*Refused.ErrorCode, *Refused.ErrorMessage), Refused.bSuccess);
	TestEqual(TEXT("the stale-token refusal is STALE_PRECONDITION"),
		Refused.ErrorCode, FString(CortexErrorCodes::StalePrecondition));
	TestTrue(FString::Printf(TEXT("the stale-token refusal names the reviewed intent [%s]"), *Refused.ErrorMessage),
		Refused.ErrorMessage.Contains(TEXT("expected_validation_hash")));
	TestEqual(TEXT("the stale-token refusal changes no fingerprint"), LiveRetireHash(Fixture.Blueprint), HashBefore);
	TestEqual(TEXT("the stale-token refusal removes no node"),
		Fixture.Graph ? Fixture.Graph->Nodes.Num() : -1, NodesBefore);

	// Step 3b: the reviewed hash is accepted and the approved entries are really retired.
	Request->SetStringField(TEXT("expected_validation_hash"), Reviewed.ValidationHash);
	FCortexGraphPatchOutcome Outcome;
	Error = FCortexCommandResult();
	TestTrue(FString::Printf(TEXT("the reviewed hash authorizes the apply: %s"), *Error.ErrorMessage),
		FCortexGraphPatchOps::Execute(Fixture.Blueprint, Request, Outcome, Error));
	TestEqual(TEXT("the authorized apply reports applied"), Outcome.ApplyStatus, FString(TEXT("applied")));
	TestEqual(TEXT("the authorized apply reads back the native state"), Outcome.ReadbackStatus, FString(TEXT("matched")));
	TestNull(TEXT("the approved entry node is really gone"),
		FCortexGraphMigrationOps::FindNodeByGuid(Fixture.Blueprint, Fixture.AlphaGuid));
	TestNull(TEXT("the other approved entry node is really gone"),
		FCortexGraphMigrationOps::FindNodeByGuid(Fixture.Blueprint, Fixture.BetaGuid));
	TestFalse(TEXT("the authorized apply never claims persistence"), Outcome.bSaved);

	Fixture.Cleanup();
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexGraphMigrationRetireClosedOrphanTest,
	"Cortex.Graph.Authoring.Migration.Retire.ClosedOrphanComponent",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexGraphMigrationRetireClosedOrphanTest::RunTest(const FString& Parameters)
{
	using namespace CortexGraphMigrationRetireTest;
	FFixture Fixture;
	if (!TestTrue(TEXT("fixture built"), Fixture.Build(TEXT("BP_RetireClosedOrphan"), true))) return false;
	UK2Node_CustomEvent* OldUpdate = NewObject<UK2Node_CustomEvent>(Fixture.Graph);
	OldUpdate->CustomFunctionName = TEXT("OldOrphanUpdate");
	OldUpdate->CreateNewGuid();
	OldUpdate->AllocateDefaultPins();
	Fixture.Graph->AddNode(OldUpdate, true, false);
	FKismetEditorUtilities::CompileBlueprint(Fixture.Blueprint);
	UK2Node_CallFunction* Router = Fixture.AddCall(
		UCortexGraphRetireLegacyWidget::StaticClass()->FindFunctionByName(TEXT("GetRetireButtonTarget")));
	UBlueprint* StandardMacros = LoadObject<UBlueprint>(nullptr,
		TEXT("/Engine/EditorBlueprintResources/StandardMacros.StandardMacros"));
	UEdGraph* MacroGraph = nullptr;
	if (StandardMacros)
	{
		TArray<UEdGraph*> Graphs;
		StandardMacros->GetAllGraphs(Graphs);
		for (UEdGraph* Graph : Graphs)
			if (Graph && Graph->GetName() == TEXT("IsValid")) { MacroGraph = Graph; break; }
	}
	if (!TestNotNull(TEXT("real shared IsValid macro found"), MacroGraph)) { Fixture.Cleanup(); return false; }
	UK2Node_MacroInstance* Macro = NewObject<UK2Node_MacroInstance>(Fixture.Graph);
	Macro->SetMacroGraph(MacroGraph);
	Macro->CreateNewGuid();
	Macro->AllocateDefaultPins();
	Fixture.Graph->AddNode(Macro, true, false);
	FMulticastDelegateProperty* Clicked = FindFProperty<FMulticastDelegateProperty>(UButton::StaticClass(), TEXT("OnClicked"));
	if (!TestNotNull(TEXT("real delegate found"), Clicked)) { Fixture.Cleanup(); return false; }
	UK2Node_AssignDelegate* Assign = NewObject<UK2Node_AssignDelegate>(Fixture.Graph);
	Assign->SetFromProperty(Clicked, false, UButton::StaticClass());
	Assign->CreateNewGuid();
	Assign->AllocateDefaultPins();
	Fixture.Graph->AddNode(Assign, true, false);
	UK2Node_CreateDelegate* Create = NewObject<UK2Node_CreateDelegate>(Fixture.Graph);
	Create->CreateNewGuid();
	Create->AllocateDefaultPins();
	Fixture.Graph->AddNode(Create, true, false);
	Create->SetFunction(OldUpdate->CustomFunctionName);
	UK2Node_CallFunction* Update = NewObject<UK2Node_CallFunction>(Fixture.Graph);
	Update->FunctionReference.SetSelfMember(OldUpdate->CustomFunctionName);
	Update->CreateNewGuid();
	Update->AllocateDefaultPins();
	Fixture.Graph->AddNode(Update, true, false);
	const UEdGraphSchema* Schema = Fixture.Graph->GetSchema();
	FString Failure;
	auto Link = [&](UEdGraphPin* From, UEdGraphPin* To)
	{
		return ConnectTypedPins(Schema, From, To, TEXT("closed orphan fixture"), Failure);
	};
	const bool bConnected = Link(Router->FindPin(TEXT("ReturnValue")), Macro->FindPin(TEXT("InputObject")))
		&& Link(Router->FindPin(TEXT("ReturnValue")), Assign->FindPin(UEdGraphSchema_K2::PN_Self))
		&& Link(Macro->FindPin(TEXT("Is Valid")), Assign->FindPin(TEXT("execute")))
		&& Link(Macro->FindPin(TEXT("Is Not Valid")), Update->FindPin(TEXT("execute")))
		&& Link(Assign->FindPin(TEXT("then")), Update->FindPin(TEXT("execute")))
		&& Link(FindTypedPin(Create, EGPD_Output, UEdGraphSchema_K2::PC_Delegate),
			FindTypedPin(Assign, EGPD_Input, UEdGraphSchema_K2::PC_Delegate));
	if (!TestTrue(FString::Printf(TEXT("all six real links connect: %s"), *Failure), bConnected))
	{ Fixture.Cleanup(); return false; }
	TestTrue(TEXT("macro entry remains disconnected"), FindTypedPin(Macro, EGPD_Input, UEdGraphSchema_K2::PC_Exec)->LinkedTo.IsEmpty());
	const TArray<FString> Orphans = {Router->NodeGuid.ToString(), Macro->NodeGuid.ToString(),
		Assign->NodeGuid.ToString(), Create->NodeGuid.ToString(), Update->NodeGuid.ToString()};
	const TArray<FString> Entries = {Fixture.AlphaGuid.ToString(), Fixture.BetaGuid.ToString(), OldUpdate->NodeGuid.ToString()};
	TestTrue(TEXT("disk-backed fixture saved"), Fixture.SaveToDisk());
	const TArray<uint8> DiskBefore = ReadBytes(Fixture.Filename());
	Fixture.Package->SetDirtyFlag(true);
	auto Migration = [&](const TArray<FString>& Inventory)
	{
		return WithAdditionalNodeGuids(Fixture.Migration(Entries), Inventory, TEXT("orphan_component_node_guids"));
	};
	auto RefusesWithoutMutation = [&](const TCHAR* Label, const TArray<FString>& Inventory)
	{
		const FString Hash = FCortexGraphPatchState::ComputeFingerprint(Fixture.Blueprint)->GetStringField(TEXT("graph_authoring_hash"));
		const int32 Count = Fixture.Graph->Nodes.Num();
		const bool bDirty = Fixture.Package->IsDirty();
		FCortexGraphMigrationRetirePlan PlanValue;
		FCortexCommandResult Error;
		bool bReused = false;
		TestFalse(Label, FCortexGraphMigrationOps::PlanRetirement(Fixture.Blueprint, Migration(Inventory), PlanValue, bReused, Error));
		TestEqual(TEXT("refusal preserves graph"), FCortexGraphPatchState::ComputeFingerprint(Fixture.Blueprint)->GetStringField(TEXT("graph_authoring_hash")), Hash);
		TestEqual(TEXT("refusal preserves node count"), Fixture.Graph->Nodes.Num(), Count);
		TestEqual(TEXT("refusal preserves dirty state"), Fixture.Package->IsDirty(), bDirty);
		TestTrue(TEXT("refusal preserves disk"), SameBytes(DiskBefore, ReadBytes(Fixture.Filename())));
	};
	TArray<FString> Omitted = Orphans;
	Omitted.Remove(Create->NodeGuid.ToString());
	RefusesWithoutMutation(TEXT("omitted connected member refused"), Omitted);
	TArray<FString> Duplicate = Orphans;
	Duplicate.Add(Orphans[0]);
	RefusesWithoutMutation(TEXT("duplicate request refused"), Duplicate);
	UK2Node_CallFunction* DuplicateOwner = Fixture.AddCall(UKismetSystemLibrary::StaticClass()->FindFunctionByName(TEXT("PrintString")));
	DuplicateOwner->NodeGuid = Router->NodeGuid;
	RefusesWithoutMutation(TEXT("duplicate graph identity refused"), Orphans);
	Fixture.Graph->RemoveNode(DuplicateOwner);
	DuplicateOwner->MarkAsGarbage();
	TArray<FString> Forbidden = Orphans;
	Forbidden.Add(Fixture.RetainedGuid.ToString());
	RefusesWithoutMutation(TEXT("authored entry cannot be an orphan member"), Forbidden);
	UK2Node_VariableGet* Unproved = NewObject<UK2Node_VariableGet>(Fixture.Graph);
	Unproved->VariableReference.SetSelfMember(TEXT("RetireButtonTarget"));
	Unproved->CreateNewGuid();
	Unproved->AllocateDefaultPins();
	Fixture.Graph->AddNode(Unproved, true, false);
	Forbidden = Orphans;
	Forbidden.Add(Unproved->NodeGuid.ToString());
	RefusesWithoutMutation(TEXT("unproved class refused even when disconnected"), Forbidden);
	Fixture.Graph->RemoveNode(Unproved);
	Unproved->MarkAsGarbage();
	UK2Node_CallFunction* Unresolved = NewObject<UK2Node_CallFunction>(Fixture.Graph);
	Unresolved->FunctionReference.SetSelfMember(TEXT("MissingUnreviewedFunction"));
	Unresolved->CreateNewGuid();
	Unresolved->AllocateDefaultPins();
	Fixture.Graph->AddNode(Unresolved, true, false);
	Forbidden = Orphans;
	Forbidden.Add(Unresolved->NodeGuid.ToString());
	RefusesWithoutMutation(TEXT("unresolved ordinary call without retired-function proof refused"), Forbidden);
	Fixture.Graph->RemoveNode(Unresolved);
	Unresolved->MarkAsGarbage();
	UEdGraphPin* OutsideExec = Fixture.Retained->FindPin(TEXT("then"));
	UEdGraphPin* InsideExec = FindTypedPin(Macro, EGPD_Input, UEdGraphSchema_K2::PC_Exec);
	OutsideExec->MakeLinkTo(InsideExec);
	RefusesWithoutMutation(TEXT("execution boundary refused"), Orphans);
	OutsideExec->BreakLinkTo(InsideExec);
	UEdGraphPin* OutsideData = Fixture.Producer->FindPin(TEXT("InInt"));
	Router->FindPin(TEXT("ReturnValue"))->MakeLinkTo(OutsideData);
	RefusesWithoutMutation(TEXT("data boundary refused even when schema-incompatible"), Orphans);
	Router->FindPin(TEXT("ReturnValue"))->BreakLinkTo(OutsideData);
	UEdGraphPin* OutsideDelegate = FindTypedPin(OldUpdate, EGPD_Output, UEdGraphSchema_K2::PC_Delegate);
	UEdGraphPin* InsideDelegate = FindTypedPin(Assign, EGPD_Input, UEdGraphSchema_K2::PC_Delegate);
	if (!TestNotNull(TEXT("event exposes real delegate boundary pin"), OutsideDelegate)) { Fixture.Cleanup(); return false; }
	OutsideDelegate->MakeLinkTo(InsideDelegate);
	RefusesWithoutMutation(TEXT("delegate boundary refused"), Orphans);
	OutsideDelegate->BreakLinkTo(InsideDelegate);
	UEdGraphPin* CreatedOutput = FindTypedPin(Create, EGPD_Output, UEdGraphSchema_K2::PC_Delegate);
	InsideDelegate->LinkedTo.RemoveSingle(CreatedOutput);
	RefusesWithoutMutation(TEXT("nonreciprocal physical link refused"), Orphans);
	InsideDelegate->LinkedTo.AddUnique(CreatedOutput);
	UK2Node_CallFunction* OutsideCaller = NewObject<UK2Node_CallFunction>(Fixture.Graph);
	OutsideCaller->FunctionReference.SetSelfMember(OldUpdate->CustomFunctionName);
	OutsideCaller->CreateNewGuid();
	OutsideCaller->AllocateDefaultPins();
	Fixture.Graph->AddNode(OutsideCaller, true, false);
	RefusesWithoutMutation(TEXT("semantic caller outside final removable set refused"), Orphans);
	Fixture.Graph->RemoveNode(OutsideCaller);
	OutsideCaller->MarkAsGarbage();
	const FString SharedMacroBefore = CaptureNativeGraph(MacroGraph);
	const TSet<FGuid> Retained = {Fixture.RetainedGuid, Fixture.RetainedBodyGuid, Fixture.ProducerGuid};
	const FString RetainedBefore = CaptureNativeGraph(Fixture.Graph, &Retained);
	TSharedPtr<FJsonObject> Request;
	TArray<FString> Approved;
	FCortexCommandResult Error;
	if (!TestTrue(TEXT("full closed component preview and exact approval succeed"),
		PrepareAdditionalRetirementRequest(Fixture, Entries, {}, TEXT("00000000-0000-0000-0000-000000114801"),
			Request, Approved, Error, true, false, Orphans))) { Fixture.Cleanup(); return false; }
	FCortexGraphPreparedPatch Preview;
	TestTrue(TEXT("reviewed preview remains valid"), FCortexGraphPatchOps::Preflight(Fixture.Blueprint, Request, Preview, Error));
	TestEqual(TEXT("all five proofs published"), JsonStringArray(
		FCortexGraphMigrationOps::MakeRetirementInventory(Preview.RetirementPlan), TEXT("orphan_component_nodes")).Num(), 5);
	TestTrue(TEXT("compiled statements and generated function require compilation"), Preview.RetirementPlan->GetBoolField(TEXT("requires_compile")));
	Router->NodePosX++;
	const FString StaleHash = FCortexGraphPatchState::ComputeFingerprint(Fixture.Blueprint)->GetStringField(TEXT("graph_authoring_hash"));
	const int32 StaleCount = Fixture.Graph->Nodes.Num();
	const bool bStaleDirty = Fixture.Package->IsDirty();
	FCortexGraphPatchOutcome Outcome;
	TestFalse(TEXT("stale approval refused"), FCortexGraphPatchOps::Execute(Fixture.Blueprint, Request, Outcome, Error));
	TestEqual(TEXT("stale refusal preserves graph"), FCortexGraphPatchState::ComputeFingerprint(Fixture.Blueprint)->GetStringField(TEXT("graph_authoring_hash")), StaleHash);
	TestEqual(TEXT("stale refusal preserves node count"), Fixture.Graph->Nodes.Num(), StaleCount);
	TestEqual(TEXT("stale refusal preserves dirty state"), Fixture.Package->IsDirty(), bStaleDirty);
	TestTrue(TEXT("stale refusal preserves disk"), SameBytes(DiskBefore, ReadBytes(Fixture.Filename())));
	Router->NodePosX--;
	TestTrue(TEXT("reviewed atomic removal applies"), FCortexGraphPatchOps::Execute(Fixture.Blueprint, Request, Outcome, Error));
	TestTrue(TEXT("final compiled Blueprint is valid"), Fixture.Blueprint->Status == BS_UpToDate
		|| Fixture.Blueprint->Status == BS_UpToDateWithWarnings);
	TestNull(TEXT("retired generated function removed by final compile"),
		Fixture.Blueprint->GeneratedClass->FindFunctionByName(TEXT("OldOrphanUpdate")));
	for (const FString& GuidText : Approved)
	{
		FGuid Guid;
		FGuid::Parse(GuidText, Guid);
		TestNull(TEXT("approved identity removed"), FCortexGraphMigrationOps::FindNodeByGuid(Fixture.Blueprint, Guid));
	}
	TestEqual(TEXT("hover presentation and shared producer preserved"), CaptureNativeGraph(Fixture.Graph, &Retained), RetainedBefore);
	TestEqual(TEXT("shared macro graph untouched"), CaptureNativeGraph(MacroGraph), SharedMacroBefore);
	TestTrue(TEXT("unsaved apply keeps original disk"), SameBytes(DiskBefore, ReadBytes(Fixture.Filename())));
	TestTrue(TEXT("unsaved apply keeps package dirty"), Fixture.Package->IsDirty());
	Fixture.Cleanup();
	return true;
}

#endif

