#include "Misc/AutomationTest.h"
#include "Misc/EngineVersionComparison.h"
#include "CortexStateTreeMigrationFixture.h"
#include "Dom/JsonValue.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "Editor.h"
#include "Editor/Transactor.h"

#if !UE_VERSION_OLDER_THAN(5, 7, 0)
namespace
{
const TArray<TSharedPtr<FJsonValue>>& MigPlanArray(const TSharedPtr<FJsonObject>& Object, const TCHAR* Field)
{
	static const TArray<TSharedPtr<FJsonValue>> Empty;
	const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
	return Object.IsValid() && Object->TryGetArrayField(Field, Values) && Values != nullptr ? *Values : Empty;
}

TSharedPtr<FJsonObject> MigPlanObject(const TSharedPtr<FJsonObject>& Object, const TCHAR* Field)
{
	const TSharedPtr<FJsonObject>* Child = nullptr;
	return Object.IsValid() && Object->TryGetObjectField(Field, Child) && Child != nullptr ? *Child : nullptr;
}

FString MigPlanString(const TSharedPtr<FJsonObject>& Object, const TCHAR* Field)
{
	FString Value;
	if (Object.IsValid())
	{
		Object->TryGetStringField(Field, Value);
	}
	return Value;
}

bool MigPlanIsNull(const TSharedPtr<FJsonObject>& Object, const TCHAR* Field)
{
	return Object.IsValid() && Object->HasField(Field) && Object->GetField<EJson::None>(Field)->IsNull();
}

/** The first array entry whose string field equals Value. */
TSharedPtr<FJsonObject> MigPlanFind(const TArray<TSharedPtr<FJsonValue>>& Entries, const TCHAR* Field, const FString& Value)
{
	for (const TSharedPtr<FJsonValue>& Entry : Entries)
	{
		const TSharedPtr<FJsonObject> Object = Entry->AsObject();
		if (MigPlanString(Object, Field) == Value)
		{
			return Object;
		}
	}
	return nullptr;
}

bool MigPlanContains(const TArray<TSharedPtr<FJsonValue>>& Values, const FString& Wanted)
{
	for (const TSharedPtr<FJsonValue>& Value : Values)
	{
		FString Text;
		if (Value->TryGetString(Text) && Text == Wanted)
		{
			return true;
		}
	}
	return false;
}

TSharedPtr<FJsonObject> MigPlanParams(
	CortexSTTest::FMigrationFixture& Fixture,
	const UScriptStruct* NewStruct,
	const bool bDryRun = true)
{
	TSharedPtr<FJsonObject> Params = Fixture.MutationParams();
	Params->SetStringField(TEXT("node_id"), Fixture.BpTaskId.ToString(EGuidFormats::DigitsWithHyphens));
	Params->SetStringField(TEXT("new_node_struct"), NewStruct->GetPathName());
	Params->SetBoolField(TEXT("dry_run"), bDryRun);
	return Params;
}

int32 MigPlanUndoQueueLength()
{
	return GEditor != nullptr && GEditor->Trans != nullptr ? GEditor->Trans->GetQueueLength() : 0;
}
}
#endif

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexStateTreeReplaceNodePlanTest,
	"Cortex.StateTree.ReplaceNode.Plan",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexStateTreeReplaceNodePlanTest::RunTest(const FString& Parameters)
{
#if UE_VERSION_OLDER_THAN(5, 7, 0)
	FCortexStateTreeCommandHandler Handler;
	TSharedPtr<FJsonObject> Params = CortexStateTreeTest::Params();
	Params->SetStringField(TEXT("asset_path"), TEXT("/Game/Temp/NotUsed"));
	const FCortexCommandResult Result = Handler.Execute(TEXT("replace_node"), Params);
	TestEqual(TEXT("older engines report NOT_AVAILABLE"), Result.ErrorCode, CortexErrorCodes::NotAvailable);
	return true;
#else
	CortexSTTest::FMigrationFixture Fixture;
	if (!Fixture.Build(*this, /*bCompile=*/true))
	{
		return false;
	}
	FStateTreeEditorNode* BpNode = Fixture.NodePtr(Fixture.BpTaskId);
	if (!TestNotNull(TEXT("Blueprint node exists"), BpNode)
		|| !TestTrue(TEXT("Label is set to a non-default value"), CortexSTTest::SetInstanceValueText(*BpNode, TEXT("Label"), TEXT("x"))))
	{
		return false;
	}

	UPackage* Package = Fixture.Context.StateTree->GetOutermost();
	Package->SetDirtyFlag(false);
	const TSharedPtr<FJsonObject> FingerprintBefore = CortexST::MakeFingerprint(Fixture.Context.StateTree);
	const int32 UndoBefore = MigPlanUndoQueueLength();
	const int32 BindingsBefore = CortexSTCompat::GetEditorBindingCount(*Fixture.Context.EditorData);
	const FString BpIdText = Fixture.BpTaskId.ToString(EGuidFormats::DigitsWithHyphens);

	// ── Dry run with an identity map: every editable property is copied, the internal variable is reported ──
	{
		TSharedPtr<FJsonObject> Params = MigPlanParams(Fixture, FCortexSTTestNativeTask::StaticStruct());
		Params->SetObjectField(TEXT("property_map"), MakeShared<FJsonObject>());
		const FCortexCommandResult Result = Fixture.Run(TEXT("replace_node"), Params);
		if (!TestTrue(TEXT("dry run with an identity map succeeds"), Result.bSuccess))
		{
			AddError(Result.ErrorMessage);
			return false;
		}
		TestTrue(TEXT("the result says it was a dry run"), Result.Data->GetBoolField(TEXT("dry_run")));
		TestTrue(TEXT("the plan would change the node"), Result.Data->GetBoolField(TEXT("would_change")));

		const TArray<TSharedPtr<FJsonValue>>& Plan = MigPlanArray(Result.Data, TEXT("property_plan"));
		for (const TCHAR* Name : { TEXT("Count"), TEXT("Label"), TEXT("ResultOut") })
		{
			const TSharedPtr<FJsonObject> Entry = MigPlanFind(Plan, TEXT("old_property"), Name);
			if (TestTrue(FString::Printf(TEXT("%s is planned"), Name), Entry.IsValid()))
			{
				TestEqual(FString::Printf(TEXT("%s is copied"), Name), MigPlanString(Entry, TEXT("status")), FString(TEXT("copied")));
				TestEqual(FString::Printf(TEXT("%s keeps its name"), Name), MigPlanString(Entry, TEXT("new_property")), FString(Name));
			}
		}
		const TSharedPtr<FJsonObject> Label = MigPlanFind(Plan, TEXT("old_property"), TEXT("Label"));
		TestTrue(TEXT("Label is reported non-default"), Label.IsValid() && Label->GetBoolField(TEXT("non_default")));
		const TSharedPtr<FJsonObject> Flag = MigPlanFind(Plan, TEXT("old_property"), TEXT("bConsideredForCompletion"));
		if (TestTrue(TEXT("the node-level flag bConsideredForCompletion is planned"), Flag.IsValid()))
		{
			TestEqual(TEXT("the node-level flag keeps its default"), MigPlanString(Flag, TEXT("status")), FString(TEXT("default_equal")));
			TestEqual(TEXT("the node-level flag is a node flag"), MigPlanString(Flag, TEXT("kind")), FString(TEXT("node_flag")));
		}
		TestTrue(TEXT("the internal variable is listed as Blueprint-only"), MigPlanContains(MigPlanArray(Result.Data, TEXT("blueprint_only_members")), TEXT("Internal")));
		TestFalse(TEXT("a Blueprint-only member never blocks"), MigPlanFind(Plan, TEXT("old_property"), TEXT("Internal")).IsValid());
		TestEqual(TEXT("nothing needs acknowledgment"), MigPlanArray(Result.Data, TEXT("required_acknowledgment")).Num(), 0);

		const TSharedPtr<FJsonObject> Bindings = MigPlanObject(Result.Data, TEXT("bindings"));
		TestEqual(TEXT("two bindings exist before the call"), static_cast<int32>(Bindings->GetNumberField(TEXT("before_count"))), 2);
		TestEqual(TEXT("both bindings of the node are planned for rewrite"), MigPlanArray(Bindings, TEXT("remapped")).Num(), 2);
		TestEqual(TEXT("no binding is unresolved"), MigPlanArray(Bindings, TEXT("unresolved")).Num(), 0);
	}

	// ── A native struct that lacks Label: the non-default value would be lost ──
	{
		TSharedPtr<FJsonObject> Params = MigPlanParams(Fixture, FCortexSTTestNoLabelTask::StaticStruct());
		const FCortexCommandResult Result = Fixture.Run(TEXT("replace_node"), Params);
		TestFalse(TEXT("losing a non-default Label refuses"), Result.bSuccess);
		TestEqual(TEXT("losing a non-default Label code"), Result.ErrorCode, CortexErrorCodes::PotentialDataLoss);
		TestTrue(TEXT("required_acknowledgment lists Label"), MigPlanContains(MigPlanArray(Result.ErrorDetails, TEXT("required_acknowledgment")), TEXT("Label")));

		TArray<TSharedPtr<FJsonValue>> Acknowledged;
		Acknowledged.Add(MakeShared<FJsonValueString>(TEXT("Label")));
		Params->SetArrayField(TEXT("acknowledged_losses"), Acknowledged);
		const FCortexCommandResult Ack = Fixture.Run(TEXT("replace_node"), Params);
		if (TestTrue(TEXT("the acknowledged loss is accepted"), Ack.bSuccess))
		{
			const TSharedPtr<FJsonObject> Label = MigPlanFind(MigPlanArray(Ack.Data, TEXT("property_plan")), TEXT("old_property"), TEXT("Label"));
			TestEqual(TEXT("Label is dropped on purpose"), MigPlanString(Label, TEXT("status")), FString(TEXT("dropped_acknowledged")));
			TestTrue(TEXT("the new property is null"), MigPlanIsNull(Label, TEXT("new_property")));
		}
	}

	// ── A renaming map rewrites the bindings: old GUID present, new GUID null ──
	{
		TSharedPtr<FJsonObject> Params = MigPlanParams(Fixture, FCortexSTTestRenamedTask::StaticStruct());
		TSharedPtr<FJsonObject> Map = MakeShared<FJsonObject>();
		Map->SetStringField(TEXT("Count"), TEXT("Amount"));
		Map->SetStringField(TEXT("ResultOut"), TEXT("Produced"));
		Params->SetObjectField(TEXT("property_map"), Map);
		const FCortexCommandResult Result = Fixture.Run(TEXT("replace_node"), Params);
		if (TestTrue(TEXT("a renaming dry run succeeds"), Result.bSuccess))
		{
			const TArray<TSharedPtr<FJsonValue>>& Remapped = MigPlanArray(MigPlanObject(Result.Data, TEXT("bindings")), TEXT("remapped"));
			const TSharedPtr<FJsonObject> Count = MigPlanFind(Remapped, TEXT("old_path"), TEXT("Count"));
			if (TestTrue(TEXT("the binding into Count is remapped"), Count.IsValid()))
			{
				TestEqual(TEXT("Count becomes Amount"), MigPlanString(Count, TEXT("new_path")), FString(TEXT("Amount")));
				TestEqual(TEXT("it is the target side"), MigPlanString(Count, TEXT("side")), FString(TEXT("target")));
				TestFalse(TEXT("the old property GUID is set"), MigPlanIsNull(Count, TEXT("old_property_guid")));
				TestTrue(TEXT("the new property GUID is null"), MigPlanIsNull(Count, TEXT("new_property_guid")));
				TestTrue(TEXT("the rewrite is a rename"), Count->GetBoolField(TEXT("renamed")));
			}
			const TSharedPtr<FJsonObject> ResultOut = MigPlanFind(Remapped, TEXT("old_path"), TEXT("ResultOut"));
			if (TestTrue(TEXT("the binding out of ResultOut is remapped"), ResultOut.IsValid()))
			{
				TestEqual(TEXT("ResultOut becomes Produced"), MigPlanString(ResultOut, TEXT("new_path")), FString(TEXT("Produced")));
				TestEqual(TEXT("it is the source side"), MigPlanString(ResultOut, TEXT("side")), FString(TEXT("source")));
			}
		}
	}

	// ── A binding whose property has no counterpart is refused, never dropped ──
	{
		TSharedPtr<FJsonObject> Params = MigPlanParams(Fixture, FCortexSTTestRenamedTask::StaticStruct());
		TSharedPtr<FJsonObject> Map = MakeShared<FJsonObject>();
		Map->SetStringField(TEXT("Count"), TEXT("Amount"));
		Params->SetObjectField(TEXT("property_map"), Map);
		const FCortexCommandResult Result = Fixture.Run(TEXT("replace_node"), Params);
		TestFalse(TEXT("an unresolved binding refuses"), Result.bSuccess);
		TestEqual(TEXT("an unresolved binding code"), Result.ErrorCode, CortexErrorCodes::VerificationFailed);
		const TArray<TSharedPtr<FJsonValue>>& Unresolved = MigPlanArray(Result.ErrorDetails, TEXT("unresolved_bindings"));
		if (TestEqual(TEXT("exactly the binding out of ResultOut is unresolved"), Unresolved.Num(), 1))
		{
			const TSharedPtr<FJsonObject> Entry = Unresolved[0]->AsObject();
			TestEqual(TEXT("it is the binding index 1"), static_cast<int32>(Entry->GetNumberField(TEXT("binding_index"))), 1);
			TestEqual(TEXT("it names ResultOut"), MigPlanString(Entry, TEXT("path")), FString(TEXT("ResultOut")));
		}
	}

	// ── Type, kind and selector refusals ──
	{
		const FCortexCommandResult Mismatch = Fixture.Run(TEXT("replace_node"), MigPlanParams(Fixture, FCortexSTTestMismatchTask::StaticStruct()));
		TestFalse(TEXT("a type mismatch refuses"), Mismatch.bSuccess);
		TestEqual(TEXT("a type mismatch code"), Mismatch.ErrorCode, CortexErrorCodes::TypeMismatch);
		TestNotNull(TEXT("the mismatch names Count"), MigPlanFind(MigPlanArray(Mismatch.ErrorDetails, TEXT("type_mismatches")), TEXT("old_property"), TEXT("Count")).Get());

		const FCortexCommandResult Kind = Fixture.Run(TEXT("replace_node"), MigPlanParams(Fixture, FCortexSTTestNativeCondition::StaticStruct()));
		TestFalse(TEXT("a condition struct for a task slot refuses"), Kind.bSuccess);
		TestEqual(TEXT("a slot kind mismatch code"), Kind.ErrorCode, CortexErrorCodes::InvalidField);

		TSharedPtr<FJsonObject> Both = MigPlanParams(Fixture, FCortexSTTestNativeTask::StaticStruct());
		Both->SetStringField(TEXT("blueprint_class"), Fixture.TaskBlueprint->GeneratedClass->GetPathName());
		TestEqual(TEXT("node_id together with blueprint_class refuses"), Fixture.Run(TEXT("replace_node"), Both).ErrorCode, CortexErrorCodes::InvalidField);

		TSharedPtr<FJsonObject> None = MigPlanParams(Fixture, FCortexSTTestNativeTask::StaticStruct());
		None->RemoveField(TEXT("node_id"));
		TestEqual(TEXT("neither node_id nor blueprint_class refuses"), Fixture.Run(TEXT("replace_node"), None).ErrorCode, CortexErrorCodes::InvalidField);

		TSharedPtr<FJsonObject> BothTypes = MigPlanParams(Fixture, FCortexSTTestNativeTask::StaticStruct());
		BothTypes->SetStringField(TEXT("new_node_class"), UCortexSTTestNativeBlueprintTask::StaticClass()->GetPathName());
		TestEqual(TEXT("new_node_struct together with new_node_class refuses"), Fixture.Run(TEXT("replace_node"), BothTypes).ErrorCode, CortexErrorCodes::InvalidField);

		TSharedPtr<FJsonObject> NoType = MigPlanParams(Fixture, FCortexSTTestNativeTask::StaticStruct());
		NoType->RemoveField(TEXT("new_node_struct"));
		TestEqual(TEXT("neither new type refuses"), Fixture.Run(TEXT("replace_node"), NoType).ErrorCode, CortexErrorCodes::InvalidField);

		TSharedPtr<FJsonObject> BlueprintClass = MigPlanParams(Fixture, FCortexSTTestNativeTask::StaticStruct());
		BlueprintClass->RemoveField(TEXT("new_node_struct"));
		BlueprintClass->SetStringField(TEXT("new_node_class"), Fixture.TaskBlueprint->GeneratedClass->GetPathName());
		const FCortexCommandResult BlueprintNew = Fixture.Run(TEXT("replace_node"), BlueprintClass);
		TestEqual(TEXT("a Blueprint new_node_class refuses"), BlueprintNew.ErrorCode, CortexErrorCodes::InvalidField);
		TestTrue(TEXT("the refusal says native only"), BlueprintNew.ErrorMessage.Contains(TEXT("native")));

		TSharedPtr<FJsonObject> Abstract = MigPlanParams(Fixture, FCortexSTTestNativeTask::StaticStruct());
		Abstract->RemoveField(TEXT("new_node_struct"));
		Abstract->SetStringField(TEXT("new_node_class"), UCortexSTTestAbstractBlueprintTask::StaticClass()->GetPathName());
		TestEqual(TEXT("an abstract new_node_class refuses"), Fixture.Run(TEXT("replace_node"), Abstract).ErrorCode, CortexErrorCodes::InvalidField);

		TSharedPtr<FJsonObject> MissingNode = MigPlanParams(Fixture, FCortexSTTestNativeTask::StaticStruct());
		MissingNode->SetStringField(TEXT("node_id"), FGuid::NewGuid().ToString(EGuidFormats::DigitsWithHyphens));
		TestEqual(TEXT("an unknown node_id refuses"), Fixture.Run(TEXT("replace_node"), MissingNode).ErrorCode, CortexErrorCodes::NodeNotFound);

		TSharedPtr<FJsonObject> BadMap = MigPlanParams(Fixture, FCortexSTTestRenamedTask::StaticStruct());
		TSharedPtr<FJsonObject> BadMapObject = MakeShared<FJsonObject>();
		BadMapObject->SetStringField(TEXT("Count"), TEXT("DoesNotExist"));
		BadMap->SetObjectField(TEXT("property_map"), BadMapObject);
		TestEqual(TEXT("a property_map target that does not exist refuses"), Fixture.Run(TEXT("replace_node"), BadMap).ErrorCode, CortexErrorCodes::PropertyNotFound);
	}

	// ── Stale or missing fingerprint ──
	{
		TSharedPtr<FJsonObject> Missing = MigPlanParams(Fixture, FCortexSTTestNativeTask::StaticStruct());
		Missing->RemoveField(TEXT("expected_fingerprint"));
		TestEqual(TEXT("a missing fingerprint refuses"), Fixture.Run(TEXT("replace_node"), Missing).ErrorCode, CortexErrorCodes::StalePrecondition);

		TSharedPtr<FJsonObject> Stale = MigPlanParams(Fixture, FCortexSTTestNativeTask::StaticStruct());
		Package->SetDirtyFlag(true);
		const FCortexCommandResult StaleResult = Fixture.Run(TEXT("replace_node"), Stale);
		Package->SetDirtyFlag(false);
		TestFalse(TEXT("a stale fingerprint refuses"), StaleResult.bSuccess);
		TestEqual(TEXT("a stale fingerprint code"), StaleResult.ErrorCode, CortexErrorCodes::StalePrecondition);
	}

	// ── Planning is pure ── (asserted before the second fixture, whose teardown resets the transactor)
	TestFalse(TEXT("planning does not dirty the package"), Package->IsDirty());
	TestEqual(TEXT("planning opens no transaction"), MigPlanUndoQueueLength(), UndoBefore);
	TestEqual(TEXT("planning keeps every binding"), CortexSTCompat::GetEditorBindingCount(*Fixture.Context.EditorData), BindingsBefore);
	TestTrue(TEXT("planning leaves the fingerprint unchanged"),
		FJsonValue::CompareEqual(*MakeShared<FJsonValueObject>(FingerprintBefore), *MakeShared<FJsonValueObject>(CortexST::MakeFingerprint(Fixture.Context.StateTree))));
	const FStateTreeEditorNode* StillBlueprint = Fixture.NodePtr(Fixture.BpTaskId);
	TestTrue(TEXT("the node is still the Blueprint node"), StillBlueprint != nullptr && StillBlueprint->InstanceObject != nullptr);
	(void)BpIdText;

	// ── A mapped property whose nested segment does not resolve on the new type is refused, never dropped ──
	{
		CortexSTTest::FMigrationFixture NestedFixture;
		TArray<CortexSTTest::FBlueprintVariable> Variables;
		Variables.Add({ FName(TEXT("Nested")), UEdGraphSchema_K2::PC_Struct, FString(), FString(), true, FCortexSTTestNested::StaticStruct() });
		if (!NestedFixture.Build(*this, /*bCompile=*/false, Variables, /*bCreateBindings=*/false))
		{
			return false;
		}
		FStateTreeEditorNode* Node = NestedFixture.NodePtr(NestedFixture.BpTaskId);
		if (!TestNotNull(TEXT("the nested fixture node exists"), Node)
			|| !TestTrue(TEXT("the nested value is stored"), CortexSTTest::SetInstanceValueText(*Node, TEXT("Nested"), TEXT("(Leaf=5)"))))
		{
			return false;
		}
		FPropertyBindingPath NestedTarget(NestedFixture.BpTaskId, FName(TEXT("Nested")));
		NestedTarget.AddPathSegment(FName(TEXT("Leaf")));
		NestedFixture.Context.EditorData->AddPropertyBinding(
			FPropertyBindingPath(NestedFixture.NativeAId, FName(TEXT("ResultOut"))), NestedTarget);
		NestedFixture.Context.StateTree->MarkPackageDirty();
		if (!TestTrue(TEXT("the nested fixture compiles"), NestedFixture.Compile().bSuccess))
		{
			return false;
		}

		TSharedPtr<FJsonObject> Params = NestedFixture.MutationParams();
		Params->SetStringField(TEXT("node_id"), NestedFixture.BpTaskId.ToString(EGuidFormats::DigitsWithHyphens));
		Params->SetStringField(TEXT("new_node_struct"), FCortexSTTestNestedSinkTask::StaticStruct()->GetPathName());
		Params->SetBoolField(TEXT("dry_run"), true);
		TSharedPtr<FJsonObject> Map = MakeShared<FJsonObject>();
		Map->SetStringField(TEXT("Nested"), TEXT("RenamedNested"));
		Map->SetStringField(TEXT("ResultOut"), TEXT("ResultOut"));
		Params->SetObjectField(TEXT("property_map"), Map);
		const FCortexCommandResult Result = NestedFixture.Run(TEXT("replace_node"), Params);
		TestFalse(TEXT("a nested segment that does not resolve refuses"), Result.bSuccess);
		// The destination type of the mapped property differs too, so the property-pair rule refuses first; the binding itself is
		// still reported as unresolved (an engine property model cannot produce a type-compatible first segment whose nested
		// property is missing, which is why the resolution failure is only reachable behind this refusal).
		TestEqual(TEXT("a nested segment refusal code"), Result.ErrorCode, CortexErrorCodes::TypeMismatch);
		bool bNamed = false;
		const TArray<TSharedPtr<FJsonValue>>* Unresolved = nullptr;
		if (Result.ErrorDetails.IsValid()
			&& Result.ErrorDetails->TryGetArrayField(TEXT("unresolved_bindings"), Unresolved) && Unresolved != nullptr)
		{
			TestEqual(TEXT("exactly the nested binding is unresolved"), Unresolved->Num(), 1);
			for (const TSharedPtr<FJsonValue>& Value : *Unresolved)
			{
				const TSharedPtr<FJsonObject> Entry = Value->AsObject();
				bNamed = bNamed || MigPlanString(Entry, TEXT("path")) == TEXT("Nested.Leaf");
			}
		}
		if (!bNamed)
		{
			FString Dump;
			if (Unresolved != nullptr)
			{
				for (const TSharedPtr<FJsonValue>& Value : *Unresolved)
				{
					const TSharedPtr<FJsonObject> Entry = Value->AsObject();
					Dump += FString::Printf(TEXT("[idx=%d path='%s' reason='%s']"),
						static_cast<int32>(Entry->GetNumberField(TEXT("binding_index"))),
						*MigPlanString(Entry, TEXT("path")), *MigPlanString(Entry, TEXT("reason")));
				}
			}
			AddError(FString::Printf(TEXT("unresolved entries: %s"), *Dump));
		}
		TestTrue(TEXT("the unresolved entry names the nested path"), bNamed);
		TestEqual(TEXT("the nested refusal leaves both bindings in place"), CortexSTCompat::GetEditorBindingCount(*NestedFixture.Context.EditorData), 1);
	}


	// ── Context usage parity: the compiler's implicit binding is a property of the usage, not of a stored binding ──
	{
		CortexSTTest::FMigrationFixture ContextFixture;
		ContextFixture.SchemaPath = TEXT("/Script/CortexStateTree.CortexSTTestContextSchema");
		TArray<CortexSTTest::FBlueprintVariable> Variables;
		Variables.Add({ FName(TEXT("ContextData")), UEdGraphSchema_K2::PC_Struct, TEXT("Context"), FString(), true, FCortexSTTestContextData::StaticStruct() });
		Variables.Add({ FName(TEXT("Payload")), UEdGraphSchema_K2::PC_Int, FString(), FString(), true });
		if (!ContextFixture.Build(*this, /*bCompile=*/true, Variables, /*bCreateBindings=*/false))
		{
			return false;
		}

		// Before: the Context property is reported as an implicit context binding, with no stored binding at all.
		{
			TSharedPtr<FJsonObject> Read = ContextFixture.BaseParams();
			Read->SetStringField(TEXT("node_id"), ContextFixture.BpTaskId.ToString(EGuidFormats::DigitsWithHyphens));
			const FCortexCommandResult Result = ContextFixture.Run(TEXT("get_node_bindings"), Read);
			if (TestTrue(TEXT("context fixture reads its bindings"), Result.bSuccess)
				&& TestEqual(TEXT("the context fixture node is reported"), MigPlanArray(Result.Data, TEXT("nodes")).Num(), 1))
			{
				const TSharedPtr<FJsonObject> Node = MigPlanArray(Result.Data, TEXT("nodes"))[0]->AsObject();
				bool bImplicit = false;
				for (const TSharedPtr<FJsonValue>& Value : MigPlanArray(Node, TEXT("as_target")))
				{
					const TSharedPtr<FJsonObject> Entry = Value->AsObject();
					bImplicit = bImplicit
						|| (Entry->GetBoolField(TEXT("implicit_context"))
							&& MigPlanString(MigPlanObject(Entry, TEXT("target")), TEXT("path")) == TEXT("ContextData"));
				}
				TestTrue(TEXT("the Context property is listed as an implicit context binding"), bImplicit);
			}
		}

		// A rename to another Context property keeps the usage: accepted.
		{
			TSharedPtr<FJsonObject> Params = ContextFixture.MutationParams();
			Params->SetStringField(TEXT("node_id"), ContextFixture.BpTaskId.ToString(EGuidFormats::DigitsWithHyphens));
			Params->SetStringField(TEXT("new_node_struct"), FCortexSTTestContextRenamedTask::StaticStruct()->GetPathName());
			Params->SetBoolField(TEXT("dry_run"), true);
			TSharedPtr<FJsonObject> Map = MakeShared<FJsonObject>();
			Map->SetStringField(TEXT("ContextData"), TEXT("RenamedContext"));
			Params->SetObjectField(TEXT("property_map"), Map);
			const FCortexCommandResult Result = ContextFixture.Run(TEXT("replace_node"), Params);
			if (!Result.bSuccess)
			{
				AddError(FString::Printf(TEXT("same-usage Context rename refused: %s %s"), *Result.ErrorCode, *Result.ErrorMessage));
			}
			if (TestTrue(TEXT("a same-usage Context rename is accepted"), Result.bSuccess))
			{
				const TSharedPtr<FJsonObject> Context = MigPlanFind(MigPlanArray(Result.Data, TEXT("property_plan")), TEXT("old_property"), TEXT("ContextData"));
				TestEqual(TEXT("the Context value is carried over"), MigPlanString(Context, TEXT("status")), FString(TEXT("copied")));
				TestEqual(TEXT("it lands on the new Context property"), MigPlanString(Context, TEXT("new_property")), FString(TEXT("RenamedContext")));
			}
		}

		// A rename to a Parameter property changes the usage: the implicit binding would be lost, so it refuses.
		{
			TSharedPtr<FJsonObject> Params = ContextFixture.MutationParams();
			Params->SetStringField(TEXT("node_id"), ContextFixture.BpTaskId.ToString(EGuidFormats::DigitsWithHyphens));
			Params->SetStringField(TEXT("new_node_struct"), FCortexSTTestContextRenamedTask::StaticStruct()->GetPathName());
			Params->SetBoolField(TEXT("dry_run"), true);
			TSharedPtr<FJsonObject> Map = MakeShared<FJsonObject>();
			Map->SetStringField(TEXT("ContextData"), TEXT("AsParameter"));
			Params->SetObjectField(TEXT("property_map"), Map);
			const FCortexCommandResult Result = ContextFixture.Run(TEXT("replace_node"), Params);
			TestFalse(TEXT("a Context to Parameter mapping refuses"), Result.bSuccess);
			TestEqual(TEXT("a Context to Parameter mapping code"), Result.ErrorCode, CortexErrorCodes::TypeMismatch);
			const TSharedPtr<FJsonObject> Mismatch = MigPlanFind(MigPlanArray(Result.ErrorDetails, TEXT("type_mismatches")), TEXT("old_property"), TEXT("ContextData"));
			TestTrue(TEXT("the mismatch names the usage difference"), Mismatch.IsValid() && MigPlanString(Mismatch, TEXT("reason")).Contains(TEXT("usage")));
		}
	}
	return true;
#endif
}
