#include "Misc/AutomationTest.h"
#include "Misc/PackageName.h"
#include "Tests/CortexUMGPropertyBindingTestUtils.h"
#include "Editor.h"
#include "UObject/UnrealType.h"
#include "Operations/CortexUMGPropertyBindingOps.h"

namespace
{
FString BindingText(const FDelegateEditorBinding& Binding)
{
	FString Text;
	FDelegateEditorBinding::StaticStruct()->ExportText(Text, &Binding, nullptr, nullptr, PPF_None, nullptr);
	return Text;
}
} // namespace

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexUMGPropertyBindingClearTest,
								 "Cortex.UMG.PropertyBinding.ClearPreservesAuthoredState",
								 EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FCortexUMGPropertyBindingClearTest::RunTest(const FString& Parameters)
{
	FCortexUMGPropertyBindingFixture Fixture;
	Fixture.Seed();
	const FString RetainedBefore = BindingText(Fixture.Blueprint->Bindings[1]);
	UWidget* Parent = Fixture.Progress->GetParent();
	const FLinearColor Color = Fixture.Progress->GetFillColorAndOpacity();
	const FWidgetAnimationBinding AnimationBinding = Fixture.Animation->AnimationBindings[0];
	const TSharedPtr<FJsonObject> State = Fixture.State();
	if (!TestTrue(TEXT("Complete serialized inspection is available"), State.IsValid()))
	{
		return false;
	}
	TestTrue(TEXT("Reader reports complete"), State->GetBoolField(TEXT("reader_complete")));
	const FCortexCommandResult Result = Fixture.Router.Execute(TEXT("umg.set_property_binding"), Fixture.WriteParams());
	if (!TestTrue(TEXT("Clear succeeds"), Result.bSuccess))
	{
		return false;
	}
	TestTrue(TEXT("Actual removal reported"), Result.Data->GetBoolField(TEXT("changed")));
	TestTrue(TEXT("Serialized post-state is absent"), Result.Data->HasTypedField<EJson::Null>(TEXT("binding")));
	TestEqual(TEXT("Retained binding exact identity"), BindingText(Fixture.Blueprint->Bindings[0]), RetainedBefore);
	TestEqual(TEXT("Only one binding retained"), Fixture.Blueprint->Bindings.Num(), 1);
	TestTrue(TEXT("Widget object preserved"),
			 Fixture.Blueprint->WidgetTree->FindWidget(TEXT("ProgressDisplay")) == Fixture.Progress);
	TestTrue(TEXT("Hierarchy preserved"), Fixture.Progress->GetParent() == Parent);
	TestEqual(TEXT("Literal percent preserved"), Fixture.Progress->GetPercent(), 0.25f);
	TestEqual(TEXT("Style preserved"), Fixture.Progress->GetFillColorAndOpacity(), Color);
	TestTrue(TEXT("Animation identity preserved"), Fixture.Blueprint->Animations[0] == Fixture.Animation);
	TestEqual(TEXT("Animation range preserved"),
			  Fixture.Animation->MovieScene->GetPlaybackRange().GetUpperBoundValue().Value, 24);
	TestEqual(TEXT("Animation binding target preserved"), Fixture.Animation->AnimationBindings[0].WidgetName,
			  AnimationBinding.WidgetName);
	TestEqual(TEXT("Animation binding GUID preserved"), Fixture.Animation->AnimationBindings[0].AnimationGuid,
			  AnimationBinding.AnimationGuid);
	TestTrue(TEXT("Animation possessable preserved"),
			 Fixture.Animation->MovieScene->FindPossessable(AnimationBinding.AnimationGuid) != nullptr);
	const FCortexCommandResult Repeat = Fixture.Router.Execute(TEXT("umg.set_property_binding"), Fixture.WriteParams());
	TestTrue(TEXT("Already clear is successful no-op"), Repeat.bSuccess && !Repeat.Data->GetBoolField(TEXT("changed")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexUMGPropertyBindingInspectionTest,
								 "Cortex.UMG.PropertyBinding.InspectBrokenAndOrphanRecords",
								 EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FCortexUMGPropertyBindingInspectionTest::RunTest(const FString& Parameters)
{
	FCortexUMGPropertyBindingFixture Fixture;
	Fixture.Seed();
	FDelegateEditorBinding Orphan = Fixture.Blueprint->Bindings[0];
	Orphan.ObjectName = TEXT("DeletedWidget");
	FProperty* Source = FindFProperty<FProperty>(UUserWidget::StaticClass(), TEXT("RenderOpacity"));
	Orphan.SourcePath.Segments.Add(FEditorPropertyPathSegment(Source));
	FStructProperty* GuidProperty =
		FindFProperty<FStructProperty>(FEditorPropertyPathSegment::StaticStruct(), TEXT("MemberGuid"));
	const FGuid BrokenGuid = FGuid::NewGuid();
	*GuidProperty->ContainerPtrToValuePtr<FGuid>(&Orphan.SourcePath.Segments[0]) = BrokenGuid;
	Fixture.Blueprint->Bindings.Add(Orphan);
	const TSharedPtr<FJsonObject> State = Fixture.State();
	if (!TestTrue(TEXT("Serialized inspection exists"), State.IsValid()))
	{
		return false;
	}
	const TArray<TSharedPtr<FJsonValue>>& Bindings = State->GetArrayField(TEXT("bindings"));
	TestEqual(TEXT("All records including orphan returned"), Bindings.Num(), 3);
	if (Bindings.Num() != 3)
	{
		return false;
	}
	const TSharedPtr<FJsonObject> Segment = Bindings[2]->AsObject()->GetArrayField(TEXT("source_path"))[0]->AsObject();
	TestEqual(TEXT("Raw source name survives broken GUID"), Segment->GetStringField(TEXT("member_name")),
			  FString(TEXT("RenderOpacity")));
	TestEqual(TEXT("Raw GUID survives"), Segment->GetStringField(TEXT("member_guid")),
			  BrokenGuid.ToString(EGuidFormats::DigitsWithHyphens));
	TestEqual(TEXT("Orphan target survives"), Bindings[2]->AsObject()->GetStringField(TEXT("widget_name")),
			  FString(TEXT("DeletedWidget")));
	const FCortexCommandResult WidgetRead = Fixture.Router.Execute(TEXT("umg.get_widget"), Fixture.ReadParams(true));
	TestTrue(TEXT("Widget inspection succeeds"), WidgetRead.bSuccess);
	if (WidgetRead.bSuccess)
	{
		TestEqual(
			TEXT("Widget scope excludes unrelated records"),
			WidgetRead.Data->GetObjectField(TEXT("property_binding_state"))->GetArrayField(TEXT("bindings")).Num(), 1);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexUMGPropertyBindingRefusalTest,
								 "Cortex.UMG.PropertyBinding.RefusesMalformedStaleAndAmbiguous",
								 EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FCortexUMGPropertyBindingRefusalTest::RunTest(const FString& Parameters)
{
	FCortexUMGPropertyBindingFixture Fixture;
	Fixture.Seed();
	if (!TestTrue(TEXT("Inspection available"), Fixture.State().IsValid()))
	{
		return false;
	}
	const FString Before = BindingText(Fixture.Blueprint->Bindings[0]);
	const EBlueprintStatus Status = Fixture.Blueprint->Status;
	for (const TCHAR* Field : {TEXT("binding"), TEXT("expected_fingerprint")})
	{
		TSharedPtr<FJsonObject> Params = Fixture.WriteParams();
		Params->RemoveField(Field);
		const FCortexCommandResult Result = Fixture.Router.Execute(TEXT("umg.set_property_binding"), Params);
		TestFalse(TEXT("Required field omission refuses"), Result.bSuccess);
	}
	for (const TSharedPtr<FJsonValue>& Value :
		 {TSharedPtr<FJsonValue>(MakeShared<FJsonValueObject>(MakeShared<FJsonObject>())),
		  TSharedPtr<FJsonValue>(MakeShared<FJsonValueString>(TEXT("")))})
	{
		TestFalse(TEXT("Empty/malformed is not clear"),
				  Fixture.Router.Execute(TEXT("umg.set_property_binding"), Fixture.WriteParams(Value)).bSuccess);
	}
	for (const TCHAR* Field : {TEXT("cursor"), TEXT("limit"), TEXT("offset")})
	{
		TSharedPtr<FJsonObject> Params = Fixture.WriteParams();
		Params->SetField(Field, MakeShared<FJsonValueNull>());
		TestFalse(TEXT("Null pagination field refuses"),
				  Fixture.Router.Execute(TEXT("umg.set_property_binding"), Params).bSuccess);
	}
	TSharedPtr<FJsonObject> Missing = Fixture.WriteParams();
	Missing->SetStringField(TEXT("widget_name"), TEXT("Missing"));
	TestFalse(TEXT("Missing widget refuses"),
			  Fixture.Router.Execute(TEXT("umg.set_property_binding"), Missing).bSuccess);
	TSharedPtr<FJsonObject> CleanGuard = Fixture.WriteParams();
	Fixture.Blueprint->GetPackage()->SetDirtyFlag(true);
	TestEqual(TEXT("Clean-to-dirty guard refuses"),
			  Fixture.Router.Execute(TEXT("umg.set_property_binding"), CleanGuard).ErrorCode,
			  CortexErrorCodes::StalePrecondition);
	TSharedPtr<FJsonObject> Stale = Fixture.WriteParams();
	Fixture.Blueprint->Bindings[1].FunctionName = TEXT("UnsavedChangedSource");
	const FCortexCommandResult StaleResult = Fixture.Router.Execute(TEXT("umg.set_property_binding"), Stale);
	TestEqual(TEXT("Dirty-to-dirty signature mismatch"), StaleResult.ErrorCode, CortexErrorCodes::StalePrecondition);
	TestTrue(TEXT("Dirty refusal preserves dirty state"), Fixture.Blueprint->GetPackage()->IsDirty());
	Fixture.Blueprint->GetPackage()->ClearDirtyFlag();
	const FDelegateEditorBinding DuplicateRecord = Fixture.Blueprint->Bindings[0];
	Fixture.Blueprint->Bindings.Add(DuplicateRecord);
	const FCortexCommandResult Duplicate =
		Fixture.Router.Execute(TEXT("umg.set_property_binding"), Fixture.WriteParams());
	TestEqual(TEXT("Duplicate exact target refuses"), Duplicate.ErrorCode, FString(TEXT("PROPERTY_BINDING_AMBIGUOUS")));
	TestEqual(TEXT("Target binding preserved"), BindingText(Fixture.Blueprint->Bindings[0]), Before);
	TestEqual(TEXT("Blueprint status preserved"), Fixture.Blueprint->Status, Status);
	TestFalse(TEXT("Refusals never dirty"), Fixture.Blueprint->GetPackage()->IsDirty());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexUMGPropertyBindingExactWidgetNameTest,
								 "Cortex.UMG.PropertyBinding.ExactWidgetNamePreservesCaseOnlyOrphans",
								 EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FCortexUMGPropertyBindingExactWidgetNameTest::RunTest(const FString& Parameters)
{
	FCortexUMGPropertyBindingFixture Fixture;
	Fixture.Seed();
	const FString Target = BindingText(Fixture.Blueprint->Bindings[0]);
	const FString Retained = BindingText(Fixture.Blueprint->Bindings[1]);
	const EBlueprintStatus Status = Fixture.Blueprint->Status;
	for (const TCHAR* Name : {TEXT("progressdisplay"), TEXT("Progressdisplay")})
	{
		TSharedPtr<FJsonObject> Params = Fixture.WriteParams();
		Params->SetStringField(TEXT("widget_name"), Name);
		const FCortexCommandResult Refused = Fixture.Router.Execute(TEXT("umg.set_property_binding"), Params);
		if (!TestEqual(TEXT("Case-mismatched exact target refuses"), Refused.ErrorCode,
					   CortexErrorCodes::WidgetNotFound))
		{
			return false;
		}
	}
	TestTrue(TEXT("Refused target retains every serialized field"),
			 BindingText(Fixture.Blueprint->Bindings[0]).Equals(Target, ESearchCase::CaseSensitive));
	TestTrue(TEXT("Refusal preserves the other serialized record"),
			 BindingText(Fixture.Blueprint->Bindings[1]).Equals(Retained, ESearchCase::CaseSensitive));
	TestEqual(TEXT("Case refusal preserves Blueprint status"), Fixture.Blueprint->Status, Status);
	TestFalse(TEXT("Case refusal does not dirty"), Fixture.Blueprint->GetPackage()->IsDirty());

	FDelegateEditorBinding Orphan = Fixture.Blueprint->Bindings[0];
	Orphan.ObjectName = TEXT("progressdisplay");
	Fixture.Blueprint->Bindings.Add(Orphan);
	const FString OrphanText = BindingText(Orphan);
	const TSharedPtr<FJsonObject> State = Fixture.State();
	if (!TestTrue(TEXT("Case-only orphan remains inspectable"), State.IsValid()))
	{
		return false;
	}
	const TArray<TSharedPtr<FJsonValue>>& Diagnostics = State->GetArrayField(TEXT("diagnostics"));
	TestTrue(TEXT("Case-only orphan is diagnosed with its raw target"),
			 Diagnostics.Num() == 1 &&
				 Diagnostics[0]->AsString().Contains(TEXT("progressdisplay"), ESearchCase::CaseSensitive));
	const FCortexCommandResult Scoped = Fixture.Router.Execute(TEXT("umg.get_widget"), Fixture.ReadParams(true));
	if (!TestTrue(TEXT("Canonical widget inspection succeeds"), Scoped.bSuccess))
	{
		return false;
	}
	const TArray<TSharedPtr<FJsonValue>>& ScopedRecords =
		Scoped.Data->GetObjectField(TEXT("property_binding_state"))->GetArrayField(TEXT("bindings"));
	TestTrue(TEXT("Canonical widget scope excludes case-only orphan"),
			 ScopedRecords.Num() == 1 &&
				 ScopedRecords[0]->AsObject()->GetStringField(TEXT("widget_name"))
					 .Equals(TEXT("ProgressDisplay"), ESearchCase::CaseSensitive));
	const FCortexCommandResult Cleared =
		Fixture.Router.Execute(TEXT("umg.set_property_binding"), Fixture.WriteParams());
	if (!TestTrue(TEXT("Canonical clear is not confused with the case-only orphan"), Cleared.bSuccess) ||
		!TestEqual(TEXT("Only canonical record removed"), Fixture.Blueprint->Bindings.Num(), 2))
	{
		return false;
	}
	TestTrue(TEXT("Canonical clear preserves the other widget record"),
			 BindingText(Fixture.Blueprint->Bindings[0]).Equals(Retained, ESearchCase::CaseSensitive));
	TestTrue(TEXT("Canonical clear preserves raw case-only orphan identity"),
			 BindingText(Fixture.Blueprint->Bindings[1]).Equals(OrphanText, ESearchCase::CaseSensitive));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexUMGPropertyBindingAuthoringTest,
								 "Cortex.UMG.PropertyBinding.CreatesReplacesAndValidatesSources",
								 EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FCortexUMGPropertyBindingAuthoringTest::RunTest(const FString& Parameters)
{
	FCortexUMGPropertyBindingFixture Fixture;
	const FCortexCommandResult Created =
		Fixture.Router.Execute(TEXT("umg.set_property_binding"), Fixture.WriteParams(Fixture.Property()));
	if (!TestTrue(TEXT("Property binding creation succeeds"), Created.bSuccess))
	{
		return false;
	}
	TestEqual(TEXT("Authored path identity"), Fixture.Blueprint->Bindings[0].SourcePath.Segments[0].GetMemberName(),
			  FName(TEXT("RenderOpacity")));
	const FCortexCommandResult Repeat =
		Fixture.Router.Execute(TEXT("umg.set_property_binding"), Fixture.WriteParams(Fixture.Property()));
	TestTrue(TEXT("Identical set is no-op"), Repeat.bSuccess && !Repeat.Data->GetBoolField(TEXT("changed")));
	// Legacy source must not leak through replacement and be reinterpreted by the compiler.
	Fixture.Blueprint->Bindings[0].SourceProperty = TEXT("RenderOpacity");
	const FCortexCommandResult Replaced =
		Fixture.Router.Execute(TEXT("umg.set_property_binding"), Fixture.WriteParams(Fixture.Function()));
	if (!TestTrue(TEXT("Compatible pure function replaces"), Replaced.bSuccess))
	{
		return false;
	}
	TestEqual(TEXT("Legacy source cleared"), Fixture.Blueprint->Bindings[0].SourceProperty, NAME_None);
	TestEqual(TEXT("Function kind"), Fixture.Blueprint->Bindings[0].Kind, EBindingKind::Function);
	FKismetEditorUtilities::CompileBlueprint(Fixture.Blueprint.Get());
	TestEqual(TEXT("Requested function survives compile"), Fixture.Blueprint->Bindings[0].FunctionName,
			  FName(TEXT("GetRenderOpacity")));
	TSharedPtr<FJsonObject> Generated = Fixture.WriteParams(Fixture.Function(TEXT("GetElapsedPercent")));
	TestTrue(TEXT("Blueprint-defined function replaces"),
			 Fixture.Router.Execute(TEXT("umg.set_property_binding"), Generated).bSuccess);
	TestEqual(TEXT("Existing graph GUID retained"), Fixture.Blueprint->Bindings[0].MemberGuid,
			  Fixture.Blueprint->FunctionGraphs[0]->GraphGuid);
	const FString GeneratedBefore = BindingText(Fixture.Blueprint->Bindings[0]);
	TSharedPtr<FJsonObject> InvalidTarget = Fixture.WriteParams(Fixture.Property());
	InvalidTarget->SetStringField(TEXT("property_name"), TEXT("MissingAttribute"));
	TestFalse(TEXT("Unknown target attribute refuses"),
			  Fixture.Router.Execute(TEXT("umg.set_property_binding"), InvalidTarget).bSuccess);
	TestEqual(TEXT("Invalid target preserves binding"), BindingText(Fixture.Blueprint->Bindings[0]), GeneratedBefore);
	for (const TSharedPtr<FJsonValue>& Invalid :
		 {Fixture.Property(TEXT("MissingMember")), Fixture.Property(TEXT("Visibility")),
		  Fixture.Function(TEXT("SetRenderOpacity")), Fixture.Function(TEXT("MissingFunction"))})
	{
		TestFalse(TEXT("Invalid source refuses"),
				  Fixture.Router.Execute(TEXT("umg.set_property_binding"), Fixture.WriteParams(Invalid)).bSuccess);
		TestEqual(TEXT("Invalid source preserves serialized record"), BindingText(Fixture.Blueprint->Bindings[0]),
				  GeneratedBefore);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexUMGPropertyBindingUndoTest, "Cortex.UMG.PropertyBinding.UndoRedoExactArray",
								 EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FCortexUMGPropertyBindingUndoTest::RunTest(const FString& Parameters)
{
	FCortexUMGPropertyBindingFixture Fixture;
	Fixture.Seed();
	const FString Before = BindingText(Fixture.Blueprint->Bindings[0]);
	const FString Retained = BindingText(Fixture.Blueprint->Bindings[1]);
	const FCortexCommandResult Result = Fixture.Router.Execute(TEXT("umg.set_property_binding"), Fixture.WriteParams());
	if (!TestTrue(TEXT("Clear succeeds"), Result.bSuccess))
	{
		return false;
	}
	const FCortexCommandResult Repeat = Fixture.Router.Execute(TEXT("umg.set_property_binding"), Fixture.WriteParams());
	TestTrue(TEXT("Repeated clear is no-op"), Repeat.bSuccess && !Repeat.Data->GetBoolField(TEXT("changed")));
	TSharedPtr<FJsonObject> Invalid = Fixture.WriteParams();
	Invalid->RemoveField(TEXT("expected_fingerprint"));
	TestFalse(TEXT("Missing guard refuses after clear"),
			  Fixture.Router.Execute(TEXT("umg.set_property_binding"), Invalid).bSuccess);
	// One undo must reach the real mutation, not a no-op/refusal transaction.
	GEditor->UndoTransaction();
	TestEqual(TEXT("Undo restores both records"), Fixture.Blueprint->Bindings.Num(), 2);
	if (Fixture.Blueprint->Bindings.Num() == 2)
	{
		TestEqual(TEXT("Undo restores exact target"), BindingText(Fixture.Blueprint->Bindings[0]), Before);
		TestEqual(TEXT("Undo restores exact retained record"), BindingText(Fixture.Blueprint->Bindings[1]), Retained);
	}
	GEditor->RedoTransaction();
	TestEqual(TEXT("Redo reapplies exact removal"), Fixture.Blueprint->Bindings.Num(), 1);
	TestEqual(TEXT("Redo retains identity"), BindingText(Fixture.Blueprint->Bindings[0]), Retained);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexUMGPropertyBindingSchemaTest,
								 "Cortex.UMG.PropertyBinding.ReaderSchemaFailsClosed",
								 EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FCortexUMGPropertyBindingSchemaTest::RunTest(const FString& Parameters)
{
	UScriptStruct* Schema = FEditorPropertyPathSegment::StaticStruct();
	const FProperty* Owner = FindFProperty<FProperty>(Schema, TEXT("Struct"));
	const FProperty* Name = FindFProperty<FProperty>(Schema, TEXT("MemberName"));
	const FProperty* Guid = FindFProperty<FProperty>(Schema, TEXT("MemberGuid"));
	const FProperty* Discriminator = FindFProperty<FProperty>(Schema, TEXT("IsProperty"));
	TestTrue(TEXT("Supported raw identity schema accepted"),
			 FCortexUMGPropertyBindingOps::ValidateSegmentFields(Owner, Name, Guid, Discriminator));
	TestFalse(TEXT("Missing serialized identity metadata refuses"),
			  FCortexUMGPropertyBindingOps::ValidateSegmentFields(Owner, Name, nullptr, Discriminator));
	TestFalse(TEXT("Wrong owner metadata refuses without interpreting storage"),
			  FCortexUMGPropertyBindingOps::ValidateSegmentFields(Name, Name, Guid, Discriminator));
	TestFalse(
		TEXT("Wrong struct identity refuses"),
		FCortexUMGPropertyBindingOps::ValidateSegmentFields(
			Owner, Name, FindFProperty<FProperty>(UWidget::StaticClass(), TEXT("RenderTransform")), Discriminator));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexUMGPropertyBindingRecoveryTest,
								 "Cortex.UMG.PropertyBinding.RecoveryRestoresExactSnapshot",
								 EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FCortexUMGPropertyBindingRecoveryTest::RunTest(const FString& Parameters)
{
	FCortexUMGPropertyBindingFixture Fixture;
	Fixture.Seed();
	const TArray<FDelegateEditorBinding> Original = Fixture.Blueprint->Bindings;
	const EBlueprintStatus Status = Fixture.Blueprint->Status;
	Fixture.Blueprint->Bindings.RemoveAt(0);
	Fixture.Blueprint->Bindings[0].SourceProperty = TEXT("WrongRetainedIdentity");
	Fixture.Blueprint->GetPackage()->SetDirtyFlag(true);
	TestTrue(TEXT("Recovery verifies restored snapshot"),
			 FCortexUMGPropertyBindingOps::RestoreBindingArray(Fixture.Blueprint.Get(), Original, false));
	TestFalse(TEXT("Recovery restores original clean state"), Fixture.Blueprint->GetPackage()->IsDirty());
	TestEqual(TEXT("Recovery does not notify or compile"), Fixture.Blueprint->Status, Status);
	if (!TestEqual(TEXT("Recovery restores both records"), Fixture.Blueprint->Bindings.Num(), Original.Num()))
	{
		return false;
	}
	for (int32 Index = 0; Index < Original.Num(); ++Index)
	{
		TestEqual(TEXT("Recovery restores every serialized field"), BindingText(Fixture.Blueprint->Bindings[Index]),
				  BindingText(Original[Index]));
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexUMGPropertyBindingCompiledGuardTest,
								 "Cortex.UMG.PropertyBinding.RejectsIncompleteOrMismatchedCompiledGuard",
								 EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FCortexUMGPropertyBindingCompiledGuardTest::RunTest(const FString& Parameters)
{
	FCortexUMGPropertyBindingFixture Fixture;
	Fixture.Seed();
	const FString Target = BindingText(Fixture.Blueprint->Bindings[0]);
	TSharedPtr<FJsonObject> Mismatch = Fixture.WriteParams();
	const TSharedPtr<FJsonObject> Guard = Mismatch->GetObjectField(TEXT("expected_fingerprint"));
	Guard->SetNumberField(TEXT("compiled_signature_crc"), Guard->GetNumberField(TEXT("compiled_signature_crc")) + 1);
	const FCortexCommandResult Refused = Fixture.Router.Execute(TEXT("umg.set_property_binding"), Mismatch);
	if (!TestEqual(TEXT("Compiled fingerprint mismatch refuses"), Refused.ErrorCode,
				   CortexErrorCodes::StalePrecondition))
	{
		return false;
	}
	TSharedPtr<FJsonObject> Partial = Fixture.WriteParams();
	Partial->GetObjectField(TEXT("expected_fingerprint"))->RemoveField(TEXT("compiled_signature_crc"));
	TestEqual(TEXT("Incomplete Blueprint fingerprint refuses"),
			  Fixture.Router.Execute(TEXT("umg.set_property_binding"), Partial).ErrorCode,
			  CortexErrorCodes::InvalidField);
	TestEqual(TEXT("Invalid guard preserves target identity"), BindingText(Fixture.Blueprint->Bindings[0]), Target);
	TestEqual(TEXT("Invalid guard preserves retained records"), Fixture.Blueprint->Bindings.Num(), 2);
	TestFalse(TEXT("Invalid guard never dirties"), Fixture.Blueprint->GetPackage()->IsDirty());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexUMGPropertyBindingJsonTypesTest,
								 "Cortex.UMG.PropertyBinding.RefusesCoercedJsonTypes",
								 EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FCortexUMGPropertyBindingJsonTypesTest::RunTest(const FString& Parameters)
{
	FCortexUMGPropertyBindingFixture Fixture;
	Fixture.Seed();
	const FString Before = BindingText(Fixture.Blueprint->Bindings[0]);
	TSharedPtr<FJsonObject> Read = Fixture.ReadParams();
	Read->SetNumberField(TEXT("include_property_bindings"), 1);
	TestEqual(TEXT("Inspection flag must be an actual boolean"),
			  Fixture.Router.Execute(TEXT("umg.get_tree"), Read).ErrorCode, CortexErrorCodes::InvalidField);
	TSharedPtr<FJsonObject> Name = Fixture.WriteParams();
	Name->SetBoolField(TEXT("property_name"), false);
	TestEqual(TEXT("Target name must be an actual string"),
			  Fixture.Router.Execute(TEXT("umg.set_property_binding"), Name).ErrorCode, CortexErrorCodes::InvalidField);
	TSharedPtr<FJsonObject> Guard = Fixture.WriteParams();
	Guard->GetObjectField(TEXT("expected_fingerprint"))->SetNumberField(TEXT("is_dirty"), 0);
	TestEqual(TEXT("Guard boolean cannot be coerced from a number"),
			  Fixture.Router.Execute(TEXT("umg.set_property_binding"), Guard).ErrorCode,
			  CortexErrorCodes::InvalidField);
	TSharedPtr<FJsonObject> Version = Fixture.WriteParams();
	Version->GetObjectField(TEXT("expected_fingerprint"))
		->GetObjectField(TEXT("domain_signature"))
		->SetBoolField(TEXT("version"), true);
	TestEqual(TEXT("Signature version cannot be coerced from a boolean"),
			  Fixture.Router.Execute(TEXT("umg.set_property_binding"), Version).ErrorCode,
			  CortexErrorCodes::InvalidField);
	TestEqual(TEXT("Malformed types preserve target"), BindingText(Fixture.Blueprint->Bindings[0]), Before);
	TestEqual(TEXT("Malformed types preserve array"), Fixture.Blueprint->Bindings.Num(), 2);
	TestFalse(TEXT("Malformed types never dirty"), Fixture.Blueprint->GetPackage()->IsDirty());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexUMGPropertyBindingNameBoundsTest,
								 "Cortex.UMG.PropertyBinding.HandlesNativeNameBoundsWithoutCrashing",
								 EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FCortexUMGPropertyBindingNameBoundsTest::RunTest(const FString& Parameters)
{
	FCortexUMGPropertyBindingFixture Fixture;
	Fixture.Seed();
	const FString LongName = FString::ChrN(NAME_SIZE, TEXT('X'));
	const FString Before = BindingText(Fixture.Blueprint->Bindings[0]);
	for (const TCHAR* Field : {TEXT("property_name"), TEXT("widget_name"), TEXT("asset_path")})
	{
		TSharedPtr<FJsonObject> Request = Fixture.WriteParams();
		Request->SetStringField(Field, FCString::Strcmp(Field, TEXT("asset_path")) == 0 ? TEXT("/Game/Temp/") + LongName
																						: LongName);
		TestEqual(TEXT("Overlong native name refuses before lookup"),
				  Fixture.Router.Execute(TEXT("umg.set_property_binding"), Request).ErrorCode,
				  CortexErrorCodes::InvalidField);
	}
	for (const TSharedPtr<FJsonValue>& Source : {Fixture.Property(*LongName), Fixture.Function(*LongName)})
	{
		TestEqual(TEXT("Overlong source refuses before native lookup"),
				  Fixture.Router.Execute(TEXT("umg.set_property_binding"), Fixture.WriteParams(Source)).ErrorCode,
				  CortexErrorCodes::InvalidField);
	}
	TSharedPtr<FJsonObject> MissingCompanion = Fixture.WriteParams(Fixture.Property());
	MissingCompanion->SetStringField(TEXT("property_name"), FString::ChrN(NAME_SIZE - 1, TEXT('Y')));
	TestEqual(TEXT("Long unknown attribute cannot overflow derived delegate lookup"),
			  Fixture.Router.Execute(TEXT("umg.set_property_binding"), MissingCompanion).ErrorCode,
			  CortexErrorCodes::InvalidField);
	TestEqual(TEXT("Name refusals preserve target"), BindingText(Fixture.Blueprint->Bindings[0]), Before);
	TestFalse(TEXT("Name refusals never dirty"), Fixture.Blueprint->GetPackage()->IsDirty());
	FDelegateEditorBinding Orphan = Fixture.Blueprint->Bindings[0];
	Orphan.ObjectName = LongName;
	Fixture.Blueprint->Bindings.Add(Orphan);
	const TSharedPtr<FJsonObject> Read = Fixture.State();
	if (!TestTrue(TEXT("Overlong serialized orphan remains inspectable"), Read.IsValid()))
	{
		return false;
	}
	TestEqual(TEXT("Raw orphan identity is not narrowed to a native name"),
			  Read->GetArrayField(TEXT("bindings"))[2]->AsObject()->GetStringField(TEXT("widget_name")), LongName);
	return true;
}
