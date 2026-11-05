#include "Misc/AutomationTest.h"

#include "Animation/AnimBlueprint.h"
#include "Animation/AnimBlueprintGeneratedClass.h"
#include "AnimGraphNode_SequencePlayer.h"
#include "AnimationGraph.h"
#include "CortexAnimationAdoptTestTypes.h"
#include "CortexCommandRouter.h"
#include "CortexCoreModule.h"
#include "CortexTypes.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "EdGraphSchema_K2.h"
#include "K2Node_VariableGet.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Misc/Guid.h"
#include "Modules/ModuleManager.h"
#include "UObject/CoreRedirects.h"
#include "UObject/Package.h"
#include "UObject/UnrealType.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

namespace
{
	UEdGraph* FindAnimGraph(const UBlueprint* Blueprint)
	{
		for (UEdGraph* Graph : Blueprint->FunctionGraphs)
		{
			if (Graph != nullptr && Graph->IsA<UAnimationGraph>())
			{
				return Graph;
			}
		}
		return nullptr;
	}

	/** A sequence player whose bool pin `bLoopAnimation` is exposed. */
	UAnimGraphNode_SequencePlayer* AddPlayer(UEdGraph* Graph)
	{
		FGraphNodeCreator<UAnimGraphNode_SequencePlayer> Creator(*Graph);
		UAnimGraphNode_SequencePlayer* Node = Creator.CreateNode();
		Creator.Finalize();
		for (FOptionalPinFromProperty& Optional : Node->ShowPinForProperties)
		{
			if (Optional.PropertyName == TEXT("bLoopAnimation"))
			{
				Optional.bShowPin = true;
			}
		}
		Node->ReconstructNode();
		return Node;
	}

	const FProperty* BindingsProperty(UObject* Node, UObject*& OutBinding)
	{
		const FObjectProperty* BindingProperty = FindFProperty<FObjectProperty>(Node->GetClass(), TEXT("Binding"));
		OutBinding = BindingProperty != nullptr ? BindingProperty->GetObjectPropertyValue_InContainer(Node) : nullptr;
		return OutBinding != nullptr ? FindFProperty<FProperty>(OutBinding->GetClass(), TEXT("PropertyBindings")) : nullptr;
	}

	FString ReadBindings(UObject* Node)
	{
		UObject* Binding = nullptr;
		const FProperty* Property = BindingsProperty(Node, Binding);
		FString Text;
		if (Property != nullptr)
		{
			Property->ExportText_InContainer(0, Text, Binding, nullptr, Binding, PPF_None);
		}
		return Text;
	}

	bool WriteBindings(UObject* Node, const FString& Text)
	{
		UObject* Binding = nullptr;
		const FProperty* Property = BindingsProperty(Node, Binding);
		return Property != nullptr && Property->ImportText_InContainer(*Text, Binding, Binding, PPF_None) != nullptr;
	}

	/** Adds a property-access binding PinProperty -> Path to the node's PropertyBindings map (reflection: the binding class is private to AnimGraph). */
	bool AddBinding(UObject* Node, const TCHAR* PinProperty, const TCHAR* Path)
	{
		UObject* Binding = nullptr;
		const FMapProperty* Map = CastField<FMapProperty>(BindingsProperty(Node, Binding));
		const FStructProperty* ValueStruct = Map != nullptr ? CastField<FStructProperty>(Map->ValueProp) : nullptr;
		const FNameProperty* KeyProperty = Map != nullptr ? CastField<FNameProperty>(Map->KeyProp) : nullptr;
		if (ValueStruct == nullptr || KeyProperty == nullptr)
		{
			return false;
		}
		const FNameProperty* NameField = CastField<FNameProperty>(ValueStruct->Struct->FindPropertyByName(TEXT("PropertyName")));
		const FArrayProperty* PathField = CastField<FArrayProperty>(ValueStruct->Struct->FindPropertyByName(TEXT("PropertyPath")));
		const FStrProperty* PathElement = PathField != nullptr ? CastField<FStrProperty>(PathField->Inner) : nullptr;
		const FBoolProperty* BoundField = CastField<FBoolProperty>(ValueStruct->Struct->FindPropertyByName(TEXT("bIsBound")));
		if (NameField == nullptr || PathElement == nullptr || BoundField == nullptr)
		{
			return false;
		}

		Binding->Modify();
		FScriptMapHelper Helper(Map, Map->ContainerPtrToValuePtr<void>(Binding));
		const int32 Index = Helper.AddDefaultValue_Invalid_NeedsRehash();
		KeyProperty->SetPropertyValue(Helper.GetKeyPtr(Index), FName(PinProperty));
		uint8* Value = Helper.GetValuePtr(Index);
		NameField->SetPropertyValue_InContainer(Value, FName(PinProperty));
		FScriptArrayHelper Array(PathField, PathField->ContainerPtrToValuePtr<void>(Value));
		const int32 Element = Array.AddValue();
		PathElement->SetPropertyValue(Array.GetRawPtr(Element), FString(Path));
		BoundField->SetPropertyValue_InContainer(Value, true);
		Helper.Rehash();
		return true;
	}

	struct FRedirectScope
	{
		FRedirectScope(const UClass* Owner, const TCHAR* OldName, const TCHAR* NewName)
		{
			const FName Package = Owner->GetOutermost()->GetFName();
			const FName OuterName = Owner->GetFName();
			Redirects.Add(FCoreRedirect(ECoreRedirectFlags::Type_Property,
				FCoreRedirectObjectName(FName(OldName), OuterName, Package),
				FCoreRedirectObjectName(FName(NewName), OuterName, Package)));
			FCoreRedirects::AddRedirectList(Redirects, TEXT("CortexAnimationAdoptBindingTest"));
		}
		~FRedirectScope() { FCoreRedirects::RemoveRedirectList(Redirects, TEXT("CortexAnimationAdoptBindingTest")); }

		TArray<FCoreRedirect> Redirects;
	};

	FCortexCommandResult Adopt(const UBlueprint* Blueprint, const bool bDryRun)
	{
		const TSharedRef<FJsonObject> Mapping = MakeShared<FJsonObject>();
		Mapping->SetStringField(TEXT("bp_variable"), TEXT("bPowerOn"));
		Mapping->SetStringField(TEXT("native_property"), TEXT("bIsPowered"));
		const TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("asset_path"), Blueprint->GetPathName());
		Params->SetArrayField(TEXT("mappings"), {MakeShared<FJsonValueObject>(Mapping)});
		Params->SetBoolField(TEXT("dry_run"), bDryRun);
		FCortexCoreModule& Core = FModuleManager::GetModuleChecked<FCortexCoreModule>(TEXT("CortexCore"));
		return Core.GetCommandRouter().Execute(TEXT("blueprint.adopt_native_member"), Params);
	}

	bool HasVariable(const UBlueprint* Blueprint, const TCHAR* Name)
	{
		return FBlueprintEditorUtils::FindNewVariableIndex(Blueprint, FName(Name)) != INDEX_NONE;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexAnimationAdoptMemberBindingsTest,
	"Cortex.Animation.AdoptMemberBindings",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexAnimationAdoptMemberBindingsTest::RunTest(const FString& Parameters)
{
	UPackage* Package = CreatePackage(*FString::Printf(TEXT("/Game/Temp/AnimAdopt_%s"), *FGuid::NewGuid().ToString(EGuidFormats::Digits).Right(8)));
	UAnimBlueprint* AnimBlueprint = Cast<UAnimBlueprint>(FKismetEditorUtilities::CreateBlueprint(
		UCortexAnimAdoptInstance::StaticClass(), Package, TEXT("AnimAdopt"), BPTYPE_Normal,
		UAnimBlueprint::StaticClass(), UAnimBlueprintGeneratedClass::StaticClass()));
	if (!TestNotNull(TEXT("Anim Blueprint"), AnimBlueprint))
	{
		return false;
	}
	AnimBlueprint->bIsTemplate = true;

	FEdGraphPinType BoolType;
	BoolType.PinCategory = UEdGraphSchema_K2::PC_Boolean;
	FBlueprintEditorUtils::AddMemberVariable(AnimBlueprint, TEXT("bPowerOn"), BoolType, TEXT("true"));
	UEdGraph* AnimGraph = FindAnimGraph(AnimBlueprint);
	if (!TestNotNull(TEXT("Anim graph"), AnimGraph))
	{
		return false;
	}

	// An anim node whose bool pin is bound to the variable by a property-access path, and a get node feeding an exposed pin.
	UAnimGraphNode_SequencePlayer* Bound = AddPlayer(AnimGraph);
	UAnimGraphNode_SequencePlayer* Fed = AddPlayer(AnimGraph);
	FGraphNodeCreator<UK2Node_VariableGet> GetCreator(*AnimGraph);
	UK2Node_VariableGet* Get = GetCreator.CreateNode();
	Get->VariableReference.SetSelfMember(TEXT("bPowerOn"));
	GetCreator.Finalize();
	UEdGraphPin* FedPin = Fed->FindPin(TEXT("bLoopAnimation"));
	if (TestNotNull(TEXT("Exposed bool pin"), FedPin))
	{
		AnimGraph->GetSchema()->TryCreateConnection(Get->GetValuePin(), FedPin);
	}
	TestTrue(TEXT("The property binding is written"), AddBinding(Bound, TEXT("bLoopAnimation"), TEXT("bPowerOn")));
	const FString BindingsBefore = ReadBindings(Bound);
	TestTrue(TEXT("The binding names the variable"), BindingsBefore.Contains(TEXT("bPowerOn")));

	FKismetEditorUtilities::CompileBlueprint(AnimBlueprint);
	TestTrue(TEXT("The baseline compiles"), AnimBlueprint->Status != BS_Error);
	const FGuid GetGuid = Get->NodeGuid;
	const FGuid BoundGuid = Bound->NodeGuid;
	const FRedirectScope Redirect(UCortexAnimAdoptInstance::StaticClass(), TEXT("bPowerOn"), TEXT("bIsPowered"));

	// The plan sees both references: the property-access binding and the exposed-pin get node.
	{
		const FCortexCommandResult Dry = Adopt(AnimBlueprint, /*bDryRun=*/true);
		if (TestTrue(TEXT("Dry run succeeds"), Dry.bSuccess) && Dry.Data.IsValid())
		{
			const TArray<TSharedPtr<FJsonValue>>* Mappings = nullptr;
			Dry.Data->TryGetArrayField(TEXT("mappings"), Mappings);
			const TArray<TSharedPtr<FJsonValue>>* Nodes = nullptr;
			if (TestTrue(TEXT("mappings"), Mappings != nullptr && Mappings->Num() == 1)
				&& TestTrue(TEXT("referencing_nodes"), (*Mappings)[0]->AsObject()->TryGetArrayField(TEXT("referencing_nodes"), Nodes) && Nodes != nullptr))
			{
				TSet<FString> Kinds;
				for (const TSharedPtr<FJsonValue>& Node : *Nodes)
				{
					Kinds.Add(Node->AsObject()->GetStringField(TEXT("kind")));
				}
				TestTrue(TEXT("The anim binding is listed"), Kinds.Contains(TEXT("anim_node_binding")));
				TestTrue(TEXT("The get node is listed"), Kinds.Contains(TEXT("k2_variable_node")));
			}
		}
	}

	// Fault injection: the binding path is corrupted right after the engine retargeted it. The readback fails closed and
	// the operation-owned binding state is put back with the rest of the Blueprint.
	{
		const FDelegateHandle Handle = FBlueprintEditorUtils::OnRenameVariableReferencesEvent.AddLambda(
			[Bound](UBlueprint*, UClass*, const FName&, const FName& NewName)
			{
				FString Text = ReadBindings(Bound);
				Text.ReplaceInline(*NewName.ToString(), TEXT("Bogus"));
				WriteBindings(Bound, Text);
			});
		const FCortexCommandResult Failed = Adopt(AnimBlueprint, /*bDryRun=*/false);
		FBlueprintEditorUtils::OnRenameVariableReferencesEvent.Remove(Handle);

		TestFalse(TEXT("A corrupted binding fails the adoption"), Failed.bSuccess);
		TestEqual(TEXT("READBACK_MISMATCH"), Failed.ErrorCode, CortexErrorCodes::ReadbackMismatch);
		FString RollbackStatus;
		if (Failed.ErrorDetails.IsValid())
		{
			Failed.ErrorDetails->TryGetStringField(TEXT("rollback_status"), RollbackStatus);
		}
		if (Failed.ErrorDetails.IsValid())
		{
			FString Dump;
			const TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> Writer = TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Dump);
			Failed.ErrorDetails->RemoveField(TEXT("diagnostics"));
			Failed.ErrorDetails->RemoveField(TEXT("mappings"));
			FJsonSerializer::Serialize(Failed.ErrorDetails.ToSharedRef(), Writer);
			AddInfo(Dump);
		}
		TestEqual(TEXT("rollback_status"), RollbackStatus, FString(TEXT("restored")));
		TestTrue(TEXT("The variable is back"), HasVariable(AnimBlueprint, TEXT("bPowerOn")));
		// The journal restore rebuilds the graphs; node and graph pointers from before the call are stale.
		AnimGraph = FindAnimGraph(AnimBlueprint);
		if (!TestNotNull(TEXT("Anim graph after the rollback"), AnimGraph))
		{
			return false;
		}
		UEdGraphNode* BoundAfter = nullptr;
		UEdGraphNode* GetAfter = nullptr;
		for (UEdGraphNode* Node : AnimGraph->Nodes)
		{
			if (Node != nullptr && Node->NodeGuid == BoundGuid) { BoundAfter = Node; }
			if (Node != nullptr && Node->NodeGuid == GetGuid) { GetAfter = Node; }
		}
		if (TestNotNull(TEXT("The bound node survived"), BoundAfter))
		{
			TestEqual(TEXT("The binding path is restored"), ReadBindings(BoundAfter), BindingsBefore);
		}
		if (const UK2Node_VariableGet* GetNode = Cast<UK2Node_VariableGet>(GetAfter))
		{
			TestEqual(TEXT("The get node names the old variable"), GetNode->GetVarName(), FName(TEXT("bPowerOn")));
		}
		else
		{
			AddError(TEXT("The get node did not survive the rollback"));
		}
	}

	// A renamed adoption: the binding path and the exposed-pin node follow.
	{
		const FCortexCommandResult Result = Adopt(AnimBlueprint, /*bDryRun=*/false);
		if (TestTrue(TEXT("Adoption succeeds"), Result.bSuccess) && Result.Data.IsValid())
		{
			TestEqual(TEXT("readback matched"), Result.Data->GetStringField(TEXT("readback_status")), FString(TEXT("matched")));
			const FString Compile = Result.Data->GetStringField(TEXT("compile_status"));
			TestTrue(TEXT("compile clean"), Compile == TEXT("success") || Compile == TEXT("warning"));
		}
		TestFalse(TEXT("The variable is gone"), HasVariable(AnimBlueprint, TEXT("bPowerOn")));

		UEdGraphNode* BoundAfter = nullptr;
		UK2Node_VariableGet* GetAfter = nullptr;
		for (UEdGraphNode* Node : AnimGraph->Nodes)
		{
			if (Node != nullptr && Node->NodeGuid == BoundGuid) { BoundAfter = Node; }
			if (Node != nullptr && Node->NodeGuid == GetGuid) { GetAfter = Cast<UK2Node_VariableGet>(Node); }
		}
		if (TestNotNull(TEXT("The bound node survived"), BoundAfter))
		{
			const FString After = ReadBindings(BoundAfter);
			TestTrue(TEXT("The binding path follows to the native property"), After.Contains(TEXT("bIsPowered")));
			TestFalse(TEXT("The binding no longer names the old variable"), After.Contains(TEXT("bPowerOn")));
		}
		if (TestNotNull(TEXT("The get node survived"), GetAfter))
		{
			TestEqual(TEXT("The pin node follows"), GetAfter->GetVarName(), FName(TEXT("bIsPowered")));
			const FProperty* Resolved = GetAfter->VariableReference.ResolveMember<FProperty>(AnimBlueprint);
			TestTrue(TEXT("It resolves to the native property"), Resolved != nullptr && Resolved->GetOwnerClass() == UCortexAnimAdoptInstance::StaticClass());
		}
		TestTrue(TEXT("The Anim Blueprint compiled without errors"), AnimBlueprint->Status != BS_Error);
	}

	if (AnimBlueprint->GeneratedClass != nullptr)
	{
		AnimBlueprint->GeneratedClass->MarkAsGarbage();
	}
	if (AnimBlueprint->SkeletonGeneratedClass != nullptr)
	{
		AnimBlueprint->SkeletonGeneratedClass->MarkAsGarbage();
	}
	AnimBlueprint->MarkAsGarbage();
	return true;
}
