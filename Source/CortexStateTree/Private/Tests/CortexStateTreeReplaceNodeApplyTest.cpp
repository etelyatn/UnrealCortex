#include "Misc/AutomationTest.h"
#include "Misc/EngineVersionComparison.h"
#include "CortexStateTreeMigrationFixture.h"
#include "Dom/JsonValue.h"
#include "Editor.h"
#include "Editor/Transactor.h"

#if !UE_VERSION_OLDER_THAN(5, 7, 0)
namespace
{
const TArray<TSharedPtr<FJsonValue>>& MigApplyArray(const TSharedPtr<FJsonObject>& Object, const TCHAR* Field)
{
	static const TArray<TSharedPtr<FJsonValue>> Empty;
	const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
	return Object.IsValid() && Object->TryGetArrayField(Field, Values) && Values != nullptr ? *Values : Empty;
}

TSharedPtr<FJsonObject> MigApplyObject(const TSharedPtr<FJsonObject>& Object, const TCHAR* Field)
{
	const TSharedPtr<FJsonObject>* Child = nullptr;
	return Object.IsValid() && Object->TryGetObjectField(Field, Child) && Child != nullptr ? *Child : nullptr;
}

FString MigApplyString(const TSharedPtr<FJsonObject>& Object, const TCHAR* Field)
{
	FString Value;
	if (Object.IsValid())
	{
		Object->TryGetStringField(Field, Value);
	}
	return Value;
}

TSharedPtr<FJsonObject> MigApplyFindNode(const TSharedPtr<FJsonObject>& Data, const FGuid& NodeId)
{
	const FString Wanted = NodeId.ToString(EGuidFormats::DigitsWithHyphens);
	for (const TSharedPtr<FJsonValue>& Value : MigApplyArray(Data, TEXT("nodes")))
	{
		if (MigApplyString(Value->AsObject(), TEXT("node_id")) == Wanted)
		{
			return Value->AsObject();
		}
	}
	return nullptr;
}

/** Every path of one node's as_target/as_source, so a binding count and its property names are comparable. */
TArray<FString> MigApplyBindingPaths(const TSharedPtr<FJsonObject>& Node)
{
	TArray<FString> Paths;
	for (const TCHAR* Side : { TEXT("as_target"), TEXT("as_source") })
	{
		for (const TSharedPtr<FJsonValue>& Value : MigApplyArray(Node, Side))
		{
			const TSharedPtr<FJsonObject> Entry = Value->AsObject();
			const TSharedPtr<FJsonObject> Target = MigApplyObject(Entry, TEXT("target"));
			const TSharedPtr<FJsonObject> Source = MigApplyObject(Entry, TEXT("source"));
			Paths.Add(FString::Printf(TEXT("%s|%s=>%s|%d"), Side, *MigApplyString(Source, TEXT("path")), *MigApplyString(Target, TEXT("path")),
				static_cast<int32>(Entry->GetNumberField(TEXT("binding_index")))));
		}
	}
	return Paths;
}

TSharedPtr<FJsonObject> MigApplyParams(
	CortexSTTest::FMigrationFixture& Fixture,
	const UScriptStruct* NewStruct,
	const TMap<FString, FString>& PropertyMap,
	const bool bCompile = true,
	const bool bSave = false)
{
	TSharedPtr<FJsonObject> Params = Fixture.MutationParams();
	Params->SetStringField(TEXT("node_id"), Fixture.BpTaskId.ToString(EGuidFormats::DigitsWithHyphens));
	Params->SetStringField(TEXT("new_node_struct"), NewStruct->GetPathName());
	TSharedPtr<FJsonObject> Map = MakeShared<FJsonObject>();
	for (const TPair<FString, FString>& Entry : PropertyMap)
	{
		Map->SetStringField(Entry.Key, Entry.Value);
	}
	Params->SetObjectField(TEXT("property_map"), Map);
	Params->SetBoolField(TEXT("compile"), bCompile);
	Params->SetBoolField(TEXT("save"), bSave);
	return Params;
}

TSharedPtr<FJsonObject> MigApplyReadNode(CortexSTTest::FMigrationFixture& Fixture)
{
	TSharedPtr<FJsonObject> Read = Fixture.BaseParams();
	Read->SetStringField(TEXT("node_id"), Fixture.BpTaskId.ToString(EGuidFormats::DigitsWithHyphens));
	const FCortexCommandResult Result = Fixture.Run(TEXT("get_node_bindings"), Read);
	return Result.bSuccess && MigApplyArray(Result.Data, TEXT("nodes")).Num() == 1 ? MigApplyArray(Result.Data, TEXT("nodes"))[0]->AsObject() : nullptr;
}
}
#endif

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexStateTreeReplaceNodeApplyTest,
	"Cortex.StateTree.ReplaceNode.Apply",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexStateTreeReplaceNodeApplyTest::RunTest(const FString& Parameters)
{
#if UE_VERSION_OLDER_THAN(5, 7, 0)
	FCortexStateTreeCommandHandler Handler;
	TSharedPtr<FJsonObject> Params = CortexStateTreeTest::Params();
	Params->SetStringField(TEXT("asset_path"), TEXT("/Game/Temp/NotUsed"));
	TestEqual(TEXT("older engines report NOT_AVAILABLE"), Handler.Execute(TEXT("replace_node"), Params).ErrorCode, CortexErrorCodes::NotAvailable);
	return true;
#else
	// ── Identity map: the values and both bindings survive, the node ID does not change ──
	{
		CortexSTTest::FMigrationFixture Fixture;
		if (!Fixture.Build(*this, /*bCompile=*/true))
		{
			return false;
		}
		FStateTreeEditorNode* BpNode = Fixture.NodePtr(Fixture.BpTaskId);
		if (!TestNotNull(TEXT("the Blueprint node exists"), BpNode)
			|| !TestTrue(TEXT("Label is set to a non-default value"), CortexSTTest::SetInstanceValueText(*BpNode, TEXT("Label"), TEXT("x")))
			|| !TestTrue(TEXT("Count stays at its default"), Fixture.NodePtr(Fixture.BpTaskId)->InstanceObject != nullptr))
		{
			return false;
		}
		UObject* OldInstance = BpNode->InstanceObject;
		const UClass* BpClass = OldInstance->GetClass();
		const TSharedPtr<FJsonObject> BeforeNode = MigApplyReadNode(Fixture);
		if (!TestNotNull(TEXT("the node reads before the call"), BeforeNode.Get()))
		{
			return false;
		}
		const TArray<FString> BeforePaths = MigApplyBindingPaths(BeforeNode);
		TestEqual(TEXT("two bindings touch the Blueprint node"), BeforePaths.Num(), 2);

		TMap<FString, FString> Map;
		Map.Add(TEXT("Count"), TEXT("Count"));
		Map.Add(TEXT("ResultOut"), TEXT("ResultOut"));
		const FCortexCommandResult Result = Fixture.Run(TEXT("replace_node"), MigApplyParams(Fixture, FCortexSTTestNativeTask::StaticStruct(), Map));
		if (!TestTrue(TEXT("the replacement succeeds"), Result.bSuccess))
		{
			AddError(Result.ErrorMessage);
			return false;
		}

		TestEqual(TEXT("the tree compiles"), MigApplyString(Result.Data, TEXT("compile_status")), FString(TEXT("success")));
		TestEqual(TEXT("the compile has no errors"), static_cast<int32>(Result.Data->GetNumberField(TEXT("error_count"))), 0);
		TestEqual(TEXT("the whole-tree binding count is unchanged"), static_cast<int32>(MigApplyObject(Result.Data, TEXT("bindings"))->GetNumberField(TEXT("after_count"))), 2);
		TestFalse(TEXT("nothing was saved"), Result.Data->GetBoolField(TEXT("saved")));
		TestEqual(TEXT("one node was replaced"), MigApplyArray(Result.Data, TEXT("replaced")).Num(), 1);

		FStateTreeEditorNode* Replaced = Fixture.NodePtr(Fixture.BpTaskId);
		if (!TestNotNull(TEXT("the editor node keeps its ID"), Replaced))
		{
			return false;
		}
		TestEqual(TEXT("the node struct is the native task"), static_cast<const UScriptStruct*>(Replaced->Node.GetScriptStruct()), static_cast<const UScriptStruct*>(FCortexSTTestNativeTask::StaticStruct()));
		TestEqual(TEXT("the instance data is the native task's"), static_cast<const UScriptStruct*>(Replaced->Instance.GetScriptStruct()), static_cast<const UScriptStruct*>(FCortexSTTestNativeTaskInstanceData::StaticStruct()));
		TestTrue(TEXT("the old instance object is no longer referenced by the node"), Replaced->InstanceObject == nullptr);
		TestEqual(TEXT("the node keeps its name"), Replaced->GetName().ToString(), FString(TEXT("BP_T")));
		const FCortexSTTestNativeTaskInstanceData& Instance = Replaced->Instance.Get<FCortexSTTestNativeTaskInstanceData>();
		TestEqual(TEXT("the non-default Label was carried over"), Instance.Label, FString(TEXT("x")));
		TestEqual(TEXT("the default Count was carried over"), Instance.Count, 0);
		TestTrue(TEXT("the tree is ready to run"), Fixture.Context.StateTree->IsReadyToRun());
		TestTrue(TEXT("the package is dirty"), Fixture.Context.StateTree->GetOutermost()->IsDirty());

		const TSharedPtr<FJsonObject> AfterNode = MigApplyReadNode(Fixture);
		if (TestNotNull(TEXT("the node reads after the call"), AfterNode.Get()))
		{
			TestFalse(TEXT("the node is no longer a Blueprint class"), MigApplyObject(AfterNode, TEXT("node_type"))->GetBoolField(TEXT("is_blueprint")));
			TestEqual(TEXT("the node type is a struct now"), MigApplyString(MigApplyObject(AfterNode, TEXT("node_type")), TEXT("kind")), FString(TEXT("struct")));
			TestEqual(TEXT("the node struct is reported"), MigApplyString(MigApplyObject(AfterNode, TEXT("node_type")), TEXT("struct_path")), FCortexSTTestNativeTask::StaticStruct()->GetPathName());
			TestTrue(TEXT("every binding side still resolves"),
				[&AfterNode]()
				{
					for (const TCHAR* Side : { TEXT("as_target"), TEXT("as_source") })
					{
						for (const TSharedPtr<FJsonValue>& Value : MigApplyArray(AfterNode, Side))
						{
							const TSharedPtr<FJsonObject> Entry = Value->AsObject();
							if (!MigApplyObject(Entry, TEXT("source"))->GetBoolField(TEXT("resolved"))
								|| !MigApplyObject(Entry, TEXT("target"))->GetBoolField(TEXT("resolved")))
							{
								return false;
							}
						}
					}
					return true;
				}());
			const TArray<FString> AfterPaths = MigApplyBindingPaths(AfterNode);
			TestEqual(TEXT("both bindings still touch the node"), AfterPaths.Num(), 2);
			for (const FString& Path : BeforePaths)
			{
				TestTrue(FString::Printf(TEXT("binding is unchanged: %s"), *Path), AfterPaths.Contains(Path));
			}
		}
		// The old object is not reachable from any node any more.
		bool bOldInstanceReferenced = false;
		TArray<FCortexSTNodeLocation> AllNodes;
		CortexST::CollectEditorNodes(Fixture.Context, AllNodes);
		for (const FCortexSTNodeLocation& Location : AllNodes)
		{
			bOldInstanceReferenced = bOldInstanceReferenced || Location.Node->InstanceObject == OldInstance;
		}
		TestFalse(TEXT("no node references the old instance object"), bOldInstanceReferenced);
		TestTrue(TEXT("the replaced class is not the Blueprint class"), BpClass->HasAnyClassFlags(CLASS_CompiledFromBlueprint));
	}

	// ── Renaming map: Amount/Produced, bindings rewritten to the new names ──
	{
		CortexSTTest::FMigrationFixture Fixture;
		if (!Fixture.Build(*this, /*bCompile=*/true))
		{
			return false;
		}
		TMap<FString, FString> Map;
		Map.Add(TEXT("Count"), TEXT("Amount"));
		Map.Add(TEXT("ResultOut"), TEXT("Produced"));
		const FCortexCommandResult Result = Fixture.Run(TEXT("replace_node"), MigApplyParams(Fixture, FCortexSTTestRenamedTask::StaticStruct(), Map));
		if (!TestTrue(TEXT("the renaming replacement succeeds"), Result.bSuccess))
		{
			AddError(Result.ErrorMessage);
			return false;
		}
		TestEqual(TEXT("the renamed tree compiles"), MigApplyString(Result.Data, TEXT("compile_status")), FString(TEXT("success")));
		TestEqual(TEXT("the renamed tree keeps both bindings"), static_cast<int32>(MigApplyObject(Result.Data, TEXT("bindings"))->GetNumberField(TEXT("after_count"))), 2);
		TestTrue(TEXT("the renamed tree is ready to run"), Fixture.Context.StateTree->IsReadyToRun());

		const TSharedPtr<FJsonObject> Node = MigApplyReadNode(Fixture);
		if (TestNotNull(TEXT("the renamed node reads"), Node.Get()))
		{
			bool bAmount = false;
			bool bProduced = false;
			for (const TSharedPtr<FJsonValue>& Value : MigApplyArray(Node, TEXT("as_target")))
			{
				bAmount = bAmount || MigApplyString(MigApplyObject(Value->AsObject(), TEXT("target")), TEXT("path")) == TEXT("Amount");
			}
			for (const TSharedPtr<FJsonValue>& Value : MigApplyArray(Node, TEXT("as_source")))
			{
				bProduced = bProduced || MigApplyString(MigApplyObject(Value->AsObject(), TEXT("source")), TEXT("path")) == TEXT("Produced");
			}
			TestTrue(TEXT("the input binding targets the renamed property"), bAmount);
			TestTrue(TEXT("the output binding reads the renamed property"), bProduced);
		}
	}

	// ── One undo restores the Blueprint node with all its bindings ──
	{
		CortexSTTest::FMigrationFixture Fixture;
		if (!Fixture.Build(*this, /*bCompile=*/true))
		{
			return false;
		}
		const int32 UndoBefore = GEditor != nullptr && GEditor->Trans != nullptr ? GEditor->Trans->GetQueueLength() : 0;
		TMap<FString, FString> Map;
		Map.Add(TEXT("Count"), TEXT("Count"));
		const FCortexCommandResult Result = Fixture.Run(TEXT("replace_node"), MigApplyParams(Fixture, FCortexSTTestNativeTask::StaticStruct(), Map));
		if (!TestTrue(TEXT("the undo fixture replacement succeeds"), Result.bSuccess))
		{
			AddError(Result.ErrorMessage);
			return false;
		}
		TestEqual(TEXT("exactly one undo record was added"), GEditor->Trans->GetQueueLength(), UndoBefore + 1);
		TestTrue(TEXT("the node is native before the undo"),
			Fixture.NodePtr(Fixture.BpTaskId) != nullptr && Fixture.NodePtr(Fixture.BpTaskId)->Node.GetScriptStruct() == FCortexSTTestNativeTask::StaticStruct());

		TestTrue(TEXT("undo succeeds"), GEditor->UndoTransaction());
		FStateTreeEditorNode* Restored = Fixture.NodePtr(Fixture.BpTaskId);
		if (!TestNotNull(TEXT("the node still exists after the undo"), Restored))
		{
			return false;
		}
		TestEqual(TEXT("the undo restores the wrapper node struct"),
			Restored->Node.GetScriptStruct() != nullptr ? Restored->Node.GetScriptStruct()->GetName() : FString(),
			FString(TEXT("StateTreeBlueprintTaskWrapper")));
		if (TestNotNull(TEXT("the undo restores the Blueprint instance object"), Restored->InstanceObject.Get()))
		{
			TestEqual(TEXT("the restored instance is the Blueprint class"), Restored->InstanceObject->GetClass(), Fixture.TaskBlueprint->GeneratedClass.Get());
		}
		TestEqual(TEXT("the undo restores every binding"), CortexSTCompat::GetEditorBindingCount(*Fixture.Context.EditorData), 2);
		const TSharedPtr<FJsonObject> Node = MigApplyReadNode(Fixture);
		if (TestNotNull(TEXT("the restored node reads"), Node.Get()))
		{
			TestTrue(TEXT("the restored node is a Blueprint class again"), MigApplyObject(Node, TEXT("node_type"))->GetBoolField(TEXT("is_blueprint")));
			TestEqual(TEXT("the restored node has both bindings"), MigApplyBindingPaths(Node).Num(), 2);
		}
	}
	// ── The compiler's implicit Context binding is a property of the usage: it is listed before and after a same-usage rename ──
	{
		CortexSTTest::FMigrationFixture ContextFixture;
		ContextFixture.SchemaPath = TEXT("/Script/CortexStateTree.CortexSTTestContextSchema");
		TArray<CortexSTTest::FBlueprintVariable> Variables;
		Variables.Add({ FName(TEXT("ContextData")), UEdGraphSchema_K2::PC_Struct, TEXT("Context"), FString(), true, FCortexSTTestContextData::StaticStruct() });
		if (!ContextFixture.Build(*this, /*bCompile=*/true, Variables, /*bCreateBindings=*/false))
		{
			return false;
		}

		// Every implicit context target path of the node (no stored binding backs them).
		auto ImplicitContextPaths = [](const TSharedPtr<FJsonObject>& Node)
		{
			TArray<FString> Paths;
			for (const TSharedPtr<FJsonValue>& Value : MigApplyArray(Node, TEXT("as_target")))
			{
				const TSharedPtr<FJsonObject> Entry = Value->AsObject();
				if (Entry->GetBoolField(TEXT("implicit_context")))
				{
					Paths.Add(MigApplyString(MigApplyObject(Entry, TEXT("target")), TEXT("path")));
				}
			}
			return Paths;
		};

		const TArray<FString> BeforePaths = ImplicitContextPaths(MigApplyReadNode(ContextFixture));
		TestTrue(TEXT("the implicit Context binding exists before the swap"), BeforePaths.Contains(TEXT("ContextData")));
		{
			// The default all-nodes page must include a node whose only binding is the compiler's implicit context entry.
			const FCortexCommandResult AllNodes = ContextFixture.Run(TEXT("get_node_bindings"), ContextFixture.BaseParams());
			bool bListed = false;
			if (TestTrue(TEXT("the context fixture all-nodes page succeeds"), AllNodes.bSuccess))
			{
				for (const TSharedPtr<FJsonValue>& Value : MigApplyArray(AllNodes.Data, TEXT("nodes")))
				{
					bListed = bListed || MigApplyString(Value->AsObject(), TEXT("node_id")) == ContextFixture.BpTaskId.ToString(EGuidFormats::DigitsWithHyphens);
				}
				TestEqual(TEXT("the all-nodes page has no stored binding"), static_cast<int32>(AllNodes.Data->GetNumberField(TEXT("whole_tree_binding_count"))), 0);
			}
			TestTrue(TEXT("a node with only an implicit context entry appears in the all-nodes page"), bListed);
		}

		TSharedPtr<FJsonObject> Params = ContextFixture.MutationParams();
		Params->SetStringField(TEXT("node_id"), ContextFixture.BpTaskId.ToString(EGuidFormats::DigitsWithHyphens));
		Params->SetStringField(TEXT("new_node_struct"), FCortexSTTestContextRenamedTask::StaticStruct()->GetPathName());
		TSharedPtr<FJsonObject> Map = MakeShared<FJsonObject>();
		Map->SetStringField(TEXT("ContextData"), TEXT("RenamedContext"));
		Params->SetObjectField(TEXT("property_map"), Map);
		const FCortexCommandResult Result = ContextFixture.Run(TEXT("replace_node"), Params);
		if (!TestTrue(TEXT("the same-usage Context swap applies"), Result.bSuccess))
		{
			AddError(Result.ErrorMessage);
			return false;
		}
		TestEqual(TEXT("the Context swap compiles"), MigApplyString(Result.Data, TEXT("compile_status")), FString(TEXT("success")));
		TestEqual(TEXT("the Context swap stores no binding"), static_cast<int32>(MigApplyObject(Result.Data, TEXT("bindings"))->GetNumberField(TEXT("before_count"))), 0);

		const TArray<FString> AfterPaths = ImplicitContextPaths(MigApplyReadNode(ContextFixture));
		TestTrue(TEXT("the implicit Context binding exists after the swap"), AfterPaths.Contains(TEXT("RenamedContext")));
		TestFalse(TEXT("the old Context property name is gone"), AfterPaths.Contains(TEXT("ContextData")));
	}

	// ── F1/F2/F3/F8: values and flags are carried by content, not only by type ──
	{
		CortexSTTest::FMigrationFixture Fixture;
		if (!Fixture.Build(*this, /*bCompile=*/true))
		{
			return false;
		}
		FStateTreeEditorNode* Node = Fixture.NodePtr(Fixture.BpTaskId);
		UStateTreeTaskBlueprintBase* Instance = Node != nullptr ? Cast<UStateTreeTaskBlueprintBase>(Node->InstanceObject.Get()) : nullptr;
		if (!TestNotNull(TEXT("the Blueprint instance object exists"), Instance))
		{
			return false;
		}
		UClass* BlueprintClass = Instance->GetClass();
		UStateTreeTaskBlueprintBase* ClassDefaults = Cast<UStateTreeTaskBlueprintBase>(BlueprintClass->GetDefaultObject());
		if (!TestNotNull(TEXT("the Blueprint class defaults exist"), ClassDefaults))
		{
			return false;
		}
		// F2: the whole class runs as a background task (value == its own CDO) while the native destination defaults to true.
		Instance->bConsideredForCompletion = false;
		ClassDefaults->bConsideredForCompletion = false;
		TestTrue(TEXT("the Blueprint class disables completion"), ClassDefaults->bConsideredForCompletion == false);
		// F8: a packed UObject bool whose native counterpart sits at a different bit offset.
		TestTrue(TEXT("the packed bool is set"), CortexSTTest::SetInstanceValueText(*Node, TEXT("bFlag"), TEXT("true")));
		// F3: the author disabled the task on the wrapper node struct.
		Node->Node.GetMutable<FStateTreeBlueprintTaskWrapper>().bTaskEnabled = false;

		TMap<FString, FString> Map;
		for (const TCHAR* Name : { TEXT("Count"), TEXT("Label"), TEXT("ResultOut"), TEXT("bFlag") })
		{
			Map.Add(Name, Name);
		}
		TSharedPtr<FJsonObject> DryRun = MigApplyParams(Fixture, FCortexSTTestNativeTask::StaticStruct(), Map);
		DryRun->SetBoolField(TEXT("dry_run"), true);
		const FCortexCommandResult Planned = Fixture.Run(TEXT("replace_node"), DryRun);
		if (TestTrue(TEXT("the fidelity dry run succeeds"), Planned.bSuccess))
		{
			const TArray<TSharedPtr<FJsonValue>>* Plan = nullptr;
			Planned.Data->TryGetArrayField(TEXT("property_plan"), Plan);
			bool bCompletionCopied = false;
			bool bFlagCopied = false;
			for (const TSharedPtr<FJsonValue>& Value : Plan != nullptr ? *Plan : TArray<TSharedPtr<FJsonValue>>())
			{
				const TSharedPtr<FJsonObject> Entry = Value->AsObject();
				const FString Property = MigApplyString(Entry, TEXT("old_property"));
				if (Property == TEXT("bConsideredForCompletion"))
				{
					bCompletionCopied = MigApplyString(Entry, TEXT("status")) == TEXT("copied");
				}
				if (Property == TEXT("bFlag"))
				{
					bFlagCopied = MigApplyString(Entry, TEXT("status")) == TEXT("copied");
				}
			}
			TestTrue(TEXT("F2: a flag equal to its own class default but not the destination default is copied"), bCompletionCopied);
			TestTrue(TEXT("the packed bool is planned as copied"), bFlagCopied);
		}

		const FCortexCommandResult Result = Fixture.Run(TEXT("replace_node"), MigApplyParams(Fixture, FCortexSTTestNativeTask::StaticStruct(), Map));
		if (!TestTrue(TEXT("the fidelity replacement succeeds"), Result.bSuccess))
		{
			AddError(Result.ErrorMessage);
			return false;
		}
		FStateTreeEditorNode* Replaced = Fixture.NodePtr(Fixture.BpTaskId);
		if (!TestNotNull(TEXT("the fidelity node keeps its ID"), Replaced))
		{
			return false;
		}
		const FStateTreeTaskBase& NativeBase = Replaced->Node.Get<FStateTreeTaskBase>();
		TestFalse(TEXT("F3: the author-disabled flag is carried to the native node"), NativeBase.bTaskEnabled);
		TestFalse(TEXT("F2: the native completion flag is the old value, not the native default"), NativeBase.bConsideredForCompletion);
		const FCortexSTTestNativeTaskInstanceData& Values = Replaced->Instance.Get<FCortexSTTestNativeTaskInstanceData>();
		TestTrue(TEXT("F8: the packed bool arrives set"), Values.bFlag);
		TestEqual(TEXT("F8: the neighbouring bool is untouched by the packed copy"), static_cast<int32>(Values.ResultOut), 0);
	}

	// ── F9: two same-type context candidates: the name decides the source, so a same-usage rename must refuse ──
	{
		CortexSTTest::FMigrationFixture Fixture;
		Fixture.SchemaPath = TEXT("/Script/CortexStateTree.CortexSTTestTwoContextSchema");
		TArray<CortexSTTest::FBlueprintVariable> Variables;
		Variables.Add({ FName(TEXT("Actor")), UEdGraphSchema_K2::PC_Struct, TEXT("Context"), FString(), true, FCortexSTTestContextData::StaticStruct() });
		if (!Fixture.Build(*this, /*bCompile=*/true, Variables, /*bCreateBindings=*/false))
		{
			return false;
		}

		TSharedPtr<FJsonObject> Identity = Fixture.MutationParams();
		Identity->SetStringField(TEXT("node_id"), Fixture.BpTaskId.ToString(EGuidFormats::DigitsWithHyphens));
		Identity->SetStringField(TEXT("new_node_struct"), FCortexSTTestTwoContextTask::StaticStruct()->GetPathName());
		Identity->SetBoolField(TEXT("dry_run"), true);
		TSharedPtr<FJsonObject> IdentityMap = MakeShared<FJsonObject>();
		IdentityMap->SetStringField(TEXT("Actor"), TEXT("Actor"));
		Identity->SetObjectField(TEXT("property_map"), IdentityMap);
		TestTrue(TEXT("an identity Context rename keeps the resolved source and is accepted"), Fixture.Run(TEXT("replace_node"), Identity).bSuccess);

		TSharedPtr<FJsonObject> Renamed = Fixture.MutationParams();
		Renamed->SetStringField(TEXT("node_id"), Fixture.BpTaskId.ToString(EGuidFormats::DigitsWithHyphens));
		Renamed->SetStringField(TEXT("new_node_struct"), FCortexSTTestTwoContextTask::StaticStruct()->GetPathName());
		Renamed->SetBoolField(TEXT("dry_run"), true);
		TSharedPtr<FJsonObject> RenamedMap = MakeShared<FJsonObject>();
		RenamedMap->SetStringField(TEXT("Actor"), TEXT("AIController"));
		Renamed->SetObjectField(TEXT("property_map"), RenamedMap);
		const FCortexCommandResult Refused = Fixture.Run(TEXT("replace_node"), Renamed);
		TestFalse(TEXT("F9: a Context rename that changes the resolved source refuses"), Refused.bSuccess);
		TestEqual(TEXT("F9: a Context source change is a type mismatch"), Refused.ErrorCode, CortexErrorCodes::TypeMismatch);
		bool bNamesSources = false;
		const TArray<TSharedPtr<FJsonValue>>* Mismatches = nullptr;
		if (Refused.ErrorDetails.IsValid() && Refused.ErrorDetails->TryGetArrayField(TEXT("type_mismatches"), Mismatches) && Mismatches != nullptr)
		{
			for (const TSharedPtr<FJsonValue>& Value : *Mismatches)
			{
				const FString Reason = MigApplyString(Value->AsObject(), TEXT("reason"));
				bNamesSources = bNamesSources || (Reason.Contains(TEXT("implicit Context source")));
			}
		}
		TestTrue(TEXT("F9: the refusal names the context source change"), bNamesSources);
	}

	// ── F10: an indexed root segment is a valid binding target ──
	{
		CortexSTTest::FMigrationFixture Fixture;
		TArray<CortexSTTest::FBlueprintVariable> Variables;
		Variables.Add({ FName(TEXT("Values")), UEdGraphSchema_K2::PC_Int, FString(), FString(), true, nullptr, EPinContainerType::Array });
		if (!Fixture.Build(*this, /*bCompile=*/false, Variables, /*bCreateBindings=*/false))
		{
			return false;
		}
		FStateTreeEditorNode* Node = Fixture.NodePtr(Fixture.BpTaskId);
		if (!TestNotNull(TEXT("the indexed fixture node exists"), Node)
			|| !TestTrue(TEXT("the array value is stored"), CortexSTTest::SetInstanceValueText(*Node, TEXT("Values"), TEXT("(7,8)"))))
		{
			return false;
		}
		FPropertyBindingPath IndexedTarget(Fixture.BpTaskId, FName(TEXT("Values")));
		IndexedTarget.GetMutableSegments()[0].SetArrayIndex(0);
		Fixture.Context.EditorData->AddPropertyBinding(
			FPropertyBindingPath(Fixture.NativeAId, FName(TEXT("ResultOut"))), IndexedTarget);
		Fixture.Context.StateTree->MarkPackageDirty();
		const FCortexCommandResult Compiled = Fixture.Compile();
		if (!TestTrue(TEXT("the indexed-root fixture compiles"), Compiled.bSuccess))
		{
			AddError(Compiled.ErrorMessage);
			return false;
		}

		TMap<FString, FString> Map;
		Map.Add(TEXT("Values"), TEXT("Values"));
		Map.Add(TEXT("Count"), TEXT("Count"));
		Map.Add(TEXT("ResultOut"), TEXT("ResultOut"));
		const FCortexCommandResult Result = Fixture.Run(TEXT("replace_node"), MigApplyParams(Fixture, FCortexSTTestNativeTask::StaticStruct(), Map));
		if (!TestTrue(TEXT("F10: a replacement with an indexed root binding succeeds"), Result.bSuccess))
		{
			AddError(FString::Printf(TEXT("%s %s"), *Result.ErrorCode, *Result.ErrorMessage));
			return false;
		}
		const TSharedPtr<FJsonObject> NodeReport = MigApplyReadNode(Fixture);
		if (TestNotNull(TEXT("the indexed node reads back"), NodeReport.Get()))
		{
			bool bIndexed = false;
			for (const TSharedPtr<FJsonValue>& Value : MigApplyArray(NodeReport, TEXT("as_target")))
			{
				const TSharedPtr<FJsonObject> Entry = Value->AsObject();
				const TSharedPtr<FJsonObject> Target = MigApplyObject(Entry, TEXT("target"));
				const TArray<TSharedPtr<FJsonValue>>& Segments = MigApplyArray(Target, TEXT("segments"));
				bIndexed = bIndexed || (!Segments.IsEmpty()
					&& MigApplyString(Segments[0]->AsObject(), TEXT("name")) == TEXT("Values")
					&& static_cast<int32>(Segments[0]->AsObject()->GetNumberField(TEXT("array_index"))) == 0
					&& Target->GetBoolField(TEXT("resolved")));
			}
			TestTrue(TEXT("the indexed target still resolves at index 0"), bIndexed);
		}
	}

	return true;
#endif
}
