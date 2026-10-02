#include "Misc/AutomationTest.h"
#include "CortexCommandRouter.h"
#include "CortexAssetFingerprint.h"
#include "CortexUMGCommandHandler.h"
#include "Dom/JsonObject.h"
#include "WidgetBlueprint.h"
#include "Blueprint/WidgetTree.h"
#include "Components/TextBlock.h"
#include "Components/CanvasPanel.h"
#include "Components/PanelSlot.h"
#include "Blueprint/UserWidget.h"
#include "Editor.h"
#include "Editor/Transactor.h"
#include "CortexUMGAnimationBindingTestUtils.h"
#include "UObject/UnrealType.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "Misc/ScopeExit.h"
#if __has_include("UIComponentWidgetBlueprintExtension.h")
#include "UIComponentWidgetBlueprintExtension.h"
#include "Extensions/UIComponentContainer.h"
#include "Extensions/UIComponents/NavigationUIComponent.h"
#endif

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexUMGWidgetVariableTest,
	"Cortex.UMG.WidgetVariable",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexUMGWidgetVariableTest::RunTest(const FString& Parameters)
{
	UPackage* TestPackage = CreatePackage(TEXT("/Temp/CortexUMGWidgetVariableTest"));
	// NOTE: no PKG_PlayInEditor here — FTransaction::IsTransient() treats any record
	// containing a PIE object as transient and pops the whole transaction from the undo
	// queue, which would make the transaction-capture assertions below impossible.

	UWidgetBlueprint* WBP = NewObject<UWidgetBlueprint>(
		TestPackage, UWidgetBlueprint::StaticClass(), TEXT("WBP_VariableTest"),
		RF_Public | RF_Standalone | RF_Transactional);
	WBP->ParentClass = UUserWidget::StaticClass();
	WBP->WidgetTree = NewObject<UWidgetTree>(WBP, UWidgetTree::StaticClass(), TEXT("WidgetTree"));
	UCanvasPanel* RootCanvas = WBP->WidgetTree->ConstructWidget<UCanvasPanel>(UCanvasPanel::StaticClass(), TEXT("Root"));
	WBP->WidgetTree->RootWidget = RootCanvas;
	UTextBlock* DesignWidget = WBP->WidgetTree->ConstructWidget<UTextBlock>(UTextBlock::StaticClass(), TEXT("CommonTextBlock_147"));
	DesignWidget->bIsVariable = false;
	RootCanvas->AddChild(DesignWidget);

	const FString AssetPath = WBP->GetPathName();

	FCortexCommandRouter Router;
	Router.RegisterDomain(TEXT("umg"), TEXT("Cortex UMG"), TEXT("1.0.1"),
		MakeShared<FCortexUMGCommandHandler>());

	TSharedPtr<FJsonObject> GetParams = MakeShared<FJsonObject>();
	GetParams->SetStringField(TEXT("asset_path"), AssetPath);
	GetParams->SetStringField(TEXT("widget_name"), TEXT("CommonTextBlock_147"));
	FCortexCommandResult GetResult = Router.Execute(TEXT("umg.get_widget"), GetParams);
	TestTrue(TEXT("get_widget succeeds"), GetResult.bSuccess);
	if (GetResult.bSuccess && GetResult.Data.IsValid())
	{
		bool bIsVariable = true;
		TestTrue(TEXT("get_widget exposes is_variable"), GetResult.Data->TryGetBoolField(TEXT("is_variable"), bIsVariable));
		TestFalse(TEXT("designer widget starts as non-variable"), bIsVariable);
	}

	TSharedPtr<FJsonObject> SetParams = MakeShared<FJsonObject>();
	SetParams->SetStringField(TEXT("asset_path"), AssetPath);
	SetParams->SetStringField(TEXT("widget_name"), TEXT("CommonTextBlock_147"));
	SetParams->SetBoolField(TEXT("is_variable"), true);
	FCortexCommandResult SetResult = Router.Execute(TEXT("umg.set_widget_variable"), SetParams);
	TestTrue(TEXT("set_widget_variable succeeds"), SetResult.bSuccess);
	TSharedPtr<FJsonObject> ResultFingerprint;
	if (SetResult.bSuccess && SetResult.Data.IsValid())
	{
		TestFalse(TEXT("was_variable reports false"), SetResult.Data->GetBoolField(TEXT("was_variable")));
		TestTrue(TEXT("is_variable reports true"), SetResult.Data->GetBoolField(TEXT("is_variable")));
		TestTrue(TEXT("changed is true"), SetResult.Data->GetBoolField(TEXT("changed")));
		TestTrue(TEXT("fingerprint present"), SetResult.Data->HasField(TEXT("fingerprint")));
		ResultFingerprint = SetResult.Data->GetObjectField(TEXT("fingerprint"));
	}
	TestTrue(TEXT("widget object mutated"), DesignWidget->bIsVariable);

	// The bIsVariable bit must be captured by the transaction through Widget->Modify(),
	// not lost by recording a different object. Executing editor Undo on a transient
	// Widget Blueprint can crash UE 5.6 in Kismet post-undo fixup (see
	// CortexUMGUndoRedoTest), so prove the transaction captures the widget object
	// through the transactor instead of executing Undo.
	if (GEditor == nullptr || GEditor->Trans == nullptr || !GEditor->CanTransact())
	{
		AddInfo(TEXT("Editor undo system not available - skipping transaction-capture assertions"));
	}
	else
	{
		const int32 QueueLength = GEditor->Trans->GetQueueLength();
		const FTransaction* LastTransaction = QueueLength > 0
			? GEditor->Trans->GetTransaction(QueueLength - 1)
			: nullptr;
		TestNotNull(TEXT("set_widget_variable records a transaction"), LastTransaction);
		TestTrue(TEXT("transaction captures the widget object (Widget->Modify)"),
			LastTransaction != nullptr && LastTransaction->ContainsObject(DesignWidget));
	}

	TSharedPtr<FJsonObject> RepeatParams = MakeShared<FJsonObject>();
	RepeatParams->SetStringField(TEXT("asset_path"), AssetPath);
	RepeatParams->SetStringField(TEXT("widget_name"), TEXT("CommonTextBlock_147"));
	RepeatParams->SetBoolField(TEXT("is_variable"), true);
	FCortexCommandResult Repeat = Router.Execute(TEXT("umg.set_widget_variable"), RepeatParams);
	TestTrue(TEXT("idempotent set succeeds"), Repeat.bSuccess);
	if (Repeat.bSuccess && Repeat.Data.IsValid())
	{
		TestFalse(TEXT("no-op reports changed false"), Repeat.Data->GetBoolField(TEXT("changed")));
	}

	// Reverse transition true -> false with a matching expected_fingerprint from the mutation
	// result: proves the guard PASSES a correct fingerprint AND the reverse path mutates.
	TSharedPtr<FJsonObject> ReverseParams = MakeShared<FJsonObject>();
	ReverseParams->SetStringField(TEXT("asset_path"), AssetPath);
	ReverseParams->SetStringField(TEXT("widget_name"), TEXT("CommonTextBlock_147"));
	ReverseParams->SetBoolField(TEXT("is_variable"), false);
	ReverseParams->SetObjectField(TEXT("expected_fingerprint"), ResultFingerprint);
	FCortexCommandResult Reverse = Router.Execute(TEXT("umg.set_widget_variable"), ReverseParams);
	TestTrue(TEXT("reverse transition with matching fingerprint succeeds"), Reverse.bSuccess);
	if (Reverse.bSuccess && Reverse.Data.IsValid())
	{
		TestTrue(TEXT("reverse was_variable reports true"), Reverse.Data->GetBoolField(TEXT("was_variable")));
		TestFalse(TEXT("reverse is_variable reports false"), Reverse.Data->GetBoolField(TEXT("is_variable")));
		TestTrue(TEXT("reverse changed is true"), Reverse.Data->GetBoolField(TEXT("changed")));
	}
	TestFalse(TEXT("widget object mutated back"), DesignWidget->bIsVariable);

	// A saved-hash-only comparison misses unsaved editor changes. A fingerprint captured while the
	// package is clean must become stale when the same package transitions to dirty, even though its
	// on-disk hash has not changed.
	TestPackage->SetDirtyFlag(false);
	TSharedPtr<FJsonObject> CleanFingerprint = MakePackageNameAssetFingerprint(
		TestPackage->GetName(), TestPackage->IsDirty()).ToJson();
	TestPackage->SetDirtyFlag(true);
	TSharedPtr<FJsonObject> DirtyMismatchParams = MakeShared<FJsonObject>();
	DirtyMismatchParams->SetStringField(TEXT("asset_path"), AssetPath);
	DirtyMismatchParams->SetStringField(TEXT("widget_name"), TEXT("CommonTextBlock_147"));
	DirtyMismatchParams->SetBoolField(TEXT("is_variable"), true);
	DirtyMismatchParams->SetObjectField(TEXT("expected_fingerprint"), CleanFingerprint);
	FCortexCommandResult DirtyMismatch = Router.Execute(TEXT("umg.set_widget_variable"), DirtyMismatchParams);
	TestFalse(TEXT("clean fingerprint rejects write after package becomes dirty"), DirtyMismatch.bSuccess);
	TestEqual(TEXT("dirty mismatch error code"), DirtyMismatch.ErrorCode, CortexErrorCodes::StalePrecondition);
	TestFalse(TEXT("dirty mismatch does not mutate widget"), DesignWidget->bIsVariable);

	// Dirty-to-dirty changes must also invalidate the guard. Capture the command's current
	// fingerprint, make another unsaved widget-tree edit without changing package dirty state, then
	// prove the stale fingerprint cannot authorize the variable mutation.
	TSharedPtr<FJsonObject> DirtyFingerprintParams = MakeShared<FJsonObject>();
	DirtyFingerprintParams->SetStringField(TEXT("asset_path"), AssetPath);
	DirtyFingerprintParams->SetStringField(TEXT("widget_name"), TEXT("CommonTextBlock_147"));
	DirtyFingerprintParams->SetBoolField(TEXT("is_variable"), false);
	FCortexCommandResult DirtyFingerprintResult = Router.Execute(
		TEXT("umg.set_widget_variable"), DirtyFingerprintParams);
	TestTrue(TEXT("dirty no-op returns current fingerprint"), DirtyFingerprintResult.bSuccess);
	TSharedPtr<FJsonObject> DirtyFingerprint = DirtyFingerprintResult.bSuccess && DirtyFingerprintResult.Data.IsValid()
		? DirtyFingerprintResult.Data->GetObjectField(TEXT("fingerprint")) : nullptr;
	DesignWidget->SetIsEnabled(false);
	TSharedPtr<FJsonObject> DirtyToDirtyParams = MakeShared<FJsonObject>();
	DirtyToDirtyParams->SetStringField(TEXT("asset_path"), AssetPath);
	DirtyToDirtyParams->SetStringField(TEXT("widget_name"), TEXT("CommonTextBlock_147"));
	DirtyToDirtyParams->SetBoolField(TEXT("is_variable"), true);
	DirtyToDirtyParams->SetObjectField(TEXT("expected_fingerprint"), DirtyFingerprint);
	FCortexCommandResult DirtyToDirty = Router.Execute(TEXT("umg.set_widget_variable"), DirtyToDirtyParams);
	TestFalse(TEXT("dirty fingerprint rejects a later unsaved widget edit"), DirtyToDirty.bSuccess);
	TestEqual(TEXT("dirty-to-dirty mismatch error code"),
		DirtyToDirty.ErrorCode, CortexErrorCodes::StalePrecondition);
	TestFalse(TEXT("dirty-to-dirty mismatch does not mutate widget"), DesignWidget->bIsVariable);

	TSharedPtr<FJsonObject> StaleParams = MakeShared<FJsonObject>();
	StaleParams->SetStringField(TEXT("asset_path"), AssetPath);
	StaleParams->SetStringField(TEXT("widget_name"), TEXT("CommonTextBlock_147"));
	StaleParams->SetBoolField(TEXT("is_variable"), true);
	TSharedPtr<FJsonObject> StaleFingerprint = MakeShared<FJsonObject>();
	StaleFingerprint->SetStringField(TEXT("package_saved_hash"), TEXT("definitely-stale-hash"));
	StaleParams->SetObjectField(TEXT("expected_fingerprint"), StaleFingerprint);
	FCortexCommandResult Stale = Router.Execute(TEXT("umg.set_widget_variable"), StaleParams);
	TestFalse(TEXT("stale fingerprint guard rejects write"), Stale.bSuccess);
	TestEqual(TEXT("stale error code"), Stale.ErrorCode, CortexErrorCodes::StalePrecondition);

	TSharedPtr<FJsonObject> MissingParams = MakeShared<FJsonObject>();
	MissingParams->SetStringField(TEXT("asset_path"), AssetPath);
	MissingParams->SetStringField(TEXT("widget_name"), TEXT("NotAWidget"));
	MissingParams->SetBoolField(TEXT("is_variable"), true);
	FCortexCommandResult Missing = Router.Execute(TEXT("umg.set_widget_variable"), MissingParams);
	TestFalse(TEXT("unknown widget fails"), Missing.bSuccess);
	TestEqual(TEXT("error code"), Missing.ErrorCode, CortexErrorCodes::WidgetNotFound);

	WBP->MarkAsGarbage();
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexUMGRenameWidgetTest,
    "Cortex.UMG.RenameWidget", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexUMGRenameWidgetTest::RunTest(const FString& Parameters)
{
    FCortexUMGAnimationBindingFixture Fixture(*this);
    UWidgetBlueprint* WBP = Fixture.Blueprint.Get();
    UWidget* Widget = WBP->WidgetTree->FindWidget(TEXT("BodySizeBox"));
    if (!TestNotNull(TEXT("existing animated widget"), Widget)) return false;
    // USizeBox deliberately defaults to a designer-only widget. The dependent getter must
    // reference a real compiled member, rather than a fabricated or cache-only dependency.
    Widget->bIsVariable = true;
    FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(WBP);
    FKismetEditorUtilities::CompileBlueprint(WBP);
    if (!TestTrue(TEXT("variable target fixture compiles"), WBP->Status == BS_UpToDate
        || WBP->Status == BS_UpToDateWithWarnings)
        || !TestNotNull(TEXT("compiled target contains widget property"),
            FindFProperty<FObjectPropertyBase>(WBP->GeneratedClass, TEXT("BodySizeBox")))) return false;
    FDelegateEditorBinding Binding;
    Binding.ObjectName = TEXT("BodySizeBox");
    Binding.PropertyName = TEXT("Visibility");
    Binding.FunctionName = TEXT("GetVisibility");
    WBP->Bindings.Add(Binding);
    TSharedPtr<FJsonObject> GetFingerprint = MakeShared<FJsonObject>();
    GetFingerprint->SetStringField(TEXT("asset_path"), WBP->GetPathName());
    GetFingerprint->SetStringField(TEXT("widget_name"), TEXT("BodySizeBox"));
    GetFingerprint->SetBoolField(TEXT("is_variable"), Widget->bIsVariable);
    FCortexCommandResult State = Fixture.Router.Execute(TEXT("umg.set_widget_variable"), GetFingerprint);
    if (!TestTrue(TEXT("tree fingerprint available"), State.bSuccess)) return false;
    const TSharedPtr<FJsonObject> Before = State.Data->GetObjectField(TEXT("fingerprint"));
    const bool bDirtyBefore = WBP->GetPackage()->IsDirty();
    UWidgetAnimation* Animation = WBP->Animations[0];
    const FGuid AnimationGuid = Animation->AnimationBindings[0].AnimationGuid;
    const int32 TrackCount = static_cast<const UMovieScene*>(Animation->MovieScene)->GetBindings()[0].GetTracks().Num();
    TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
    Params->SetStringField(TEXT("asset_path"), WBP->GetPathName());
    Params->SetStringField(TEXT("widget_name"), TEXT("BodySizeBox"));
    Params->SetStringField(TEXT("new_name"), TEXT("RenamedBodySizeBox"));
    TestFalse(TEXT("fingerprint required"), Fixture.Router.Execute(TEXT("umg.rename_widget"), Params).bSuccess);
    Params->SetObjectField(TEXT("expected_fingerprint"), Before);
    UPackage* ChildPackage = CreatePackage(*(TEXT("/Temp/CortexRenameChild_") + FGuid::NewGuid().ToString(EGuidFormats::Digits)));
    UBlueprint* Child = NewObject<UBlueprint>(ChildPackage, TEXT("RenameChild"));
    Child->ParentClass = WBP->GeneratedClass;
    ChildPackage->SetDirtyFlag(false);
    FCortexCommandResult ChildRefusal = Fixture.Router.Execute(TEXT("umg.rename_widget"), Params);
    TestFalse(TEXT("loaded child refuses target-only rename"), ChildRefusal.bSuccess);
    TestEqual(TEXT("loaded child refusal explains containment"), ChildRefusal.ErrorCode, CortexErrorCodes::InvalidOperation);
    TestTrue(TEXT("loaded child refusal names child"), ChildRefusal.ErrorMessage.Contains(Child->GetPathName()));
    TestEqual(TEXT("child refusal preserves target name"), Widget->GetName(), FString(TEXT("BodySizeBox")));
    TestEqual(TEXT("child refusal preserves target dirty state"), WBP->GetPackage()->IsDirty(), bDirtyBefore);
    TestEqual(TEXT("child refusal preserves child name"), Child->GetName(), FString(TEXT("RenameChild")));
    TestTrue(TEXT("child refusal preserves child parent"), Child->ParentClass == WBP->GeneratedClass);
    TestFalse(TEXT("child refusal leaves child clean"), ChildPackage->IsDirty());
    Child->ParentClass = UUserWidget::StaticClass();
    Child->MarkAsGarbage();

    UPackage* DependentPackage = CreatePackage(*(TEXT("/Temp/CortexRenameDependent_") + FGuid::NewGuid().ToString(EGuidFormats::Digits)));
    UBlueprint* Dependent = FKismetEditorUtilities::CreateBlueprint(UObject::StaticClass(),
        DependentPackage, TEXT("RenameDependent"), BPTYPE_Normal,
        UBlueprint::StaticClass(), UBlueprintGeneratedClass::StaticClass(), TEXT("CortexWidgetRenameTest"));
    if (!TestNotNull(TEXT("real dependent Blueprint created"), Dependent)) return false;
    UEdGraph* DependentGraph = FBlueprintEditorUtils::CreateNewGraph(Dependent, TEXT("DependentGraph"),
        UEdGraph::StaticClass(), UEdGraphSchema_K2::StaticClass());
    Dependent->UbergraphPages.Add(DependentGraph);
    UK2Node_VariableGet* Reference = NewObject<UK2Node_VariableGet>(DependentGraph);
    Reference->VariableReference.SetExternalMember(TEXT("BodySizeBox"), WBP->GeneratedClass);
    Reference->CreateNewGuid();
    DependentGraph->AddNode(Reference, false, false);
    if (!TestNotNull(TEXT("dependent getter resolves real target widget property"), Reference->GetPropertyForVariable())) return false;
    Reference->AllocateDefaultPins();
    Dependent->bCachedDependenciesUpToDate = false;
    TArray<UBlueprint*> DiscoveredDependents;
    FBlueprintEditorUtils::FindDependentBlueprints(WBP, DiscoveredDependents);
    TestTrue(TEXT("engine discovers actual dependent graph reference"), DiscoveredDependents.Contains(Dependent));
    DependentPackage->SetDirtyFlag(false);
    const FGuid ReferenceGuid = Reference->NodeGuid;
    FCortexCommandResult DependentRefusal = Fixture.Router.Execute(TEXT("umg.rename_widget"), Params);
    TestFalse(TEXT("external dependent graph reference refuses rename"), DependentRefusal.bSuccess);
    TestEqual(TEXT("dependent refusal reports containment"), DependentRefusal.ErrorCode, CortexErrorCodes::InvalidOperation);
    TestTrue(TEXT("dependent refusal names dependent asset"), DependentRefusal.ErrorMessage.Contains(Dependent->GetPathName()));
    TestEqual(TEXT("dependent refusal preserves target name"), Widget->GetName(), FString(TEXT("BodySizeBox")));
    TestEqual(TEXT("dependent refusal preserves target binding"), WBP->Bindings.Last().ObjectName, FString(TEXT("BodySizeBox")));
    TestEqual(TEXT("dependent refusal preserves target animation"), Animation->AnimationBindings[0].WidgetName, FName(TEXT("BodySizeBox")));
    TestEqual(TEXT("dependent refusal preserves target dirty state"), WBP->GetPackage()->IsDirty(), bDirtyBefore);
    TestEqual(TEXT("dependent refusal preserves dependent asset name"), Dependent->GetName(), FString(TEXT("RenameDependent")));
    TestEqual(TEXT("dependent refusal preserves graph identity"), Reference->NodeGuid, ReferenceGuid);
    TestEqual(TEXT("dependent refusal preserves variable reference"), Reference->VariableReference.GetMemberName(), FName(TEXT("BodySizeBox")));
    TestFalse(TEXT("dependent refusal leaves dependent clean"), DependentPackage->IsDirty());
    DependentGraph->RemoveNode(Reference);
    Reference->MarkAsGarbage();
    FEdGraphPinType InstanceType;
    InstanceType.PinCategory = UEdGraphSchema_K2::PC_Object;
    InstanceType.PinSubCategoryObject = WBP->GeneratedClass;
    if (!TestTrue(TEXT("class-instance variable created"),
        FBlueprintEditorUtils::AddMemberVariable(Dependent, TEXT("WidgetInstance"), InstanceType))) return false;
    UK2Node_VariableGet* InstanceGetter = NewObject<UK2Node_VariableGet>(DependentGraph);
    InstanceGetter->VariableReference.SetSelfMember(TEXT("WidgetInstance"));
    InstanceGetter->CreateNewGuid();
    DependentGraph->AddNode(InstanceGetter, false, false);
    InstanceGetter->AllocateDefaultPins();
    FKismetEditorUtilities::CompileBlueprint(Dependent);
    if (!TestTrue(TEXT("class-instance dependent compiles"), Dependent->Status != BS_Error)) return false;
    if (!TestNotNull(TEXT("real class-instance getter property"), InstanceGetter->GetPropertyForVariable())) return false;
    TArray<UStruct*> InstanceDependencies;
    TestTrue(TEXT("instance getter reports target class dependency"), InstanceGetter->HasExternalDependencies(&InstanceDependencies));
    TestTrue(TEXT("instance getter type includes target class"), InstanceDependencies.Contains(WBP->GeneratedClass));
    TestFalse(TEXT("instance getter does not refer to widget member"), InstanceGetter->ReferencesVariable(TEXT("BodySizeBox"), nullptr));
    Dependent->bCachedDependenciesUpToDate = false;
    DiscoveredDependents.Reset();
    FBlueprintEditorUtils::FindDependentBlueprints(WBP, DiscoveredDependents);
    TestTrue(TEXT("engine discovers class-instance-only dependency"), DiscoveredDependents.Contains(Dependent));
    Params->SetStringField(TEXT("new_name"), TEXT("BorderBody"));
    TestFalse(TEXT("collision refused"), Fixture.Router.Execute(TEXT("umg.rename_widget"), Params).bSuccess);
    Params->SetStringField(TEXT("new_name"), TEXT("Body Size Box"));
    TestFalse(TEXT("sanitizing refused"), Fixture.Router.Execute(TEXT("umg.rename_widget"), Params).bSuccess);
    TSharedPtr<FJsonObject> WrongCaseParams = MakeShared<FJsonObject>();
    WrongCaseParams->SetStringField(TEXT("asset_path"), WBP->GetPathName());
    WrongCaseParams->SetStringField(TEXT("widget_name"), TEXT("bodysizebox"));
    WrongCaseParams->SetStringField(TEXT("new_name"), TEXT("BodySizeBox"));
    WrongCaseParams->SetObjectField(TEXT("expected_fingerprint"), Before);
    FCortexCommandResult WrongCaseResult = Fixture.Router.Execute(TEXT("umg.rename_widget"), WrongCaseParams);
    TestFalse(TEXT("wrong-case widget_name refused"), WrongCaseResult.bSuccess);
    TestEqual(TEXT("wrong-case widget_name error code"), WrongCaseResult.ErrorCode, CortexErrorCodes::WidgetNotFound);

    Params->SetStringField(TEXT("widget_name"), TEXT("BodySizeBox"));
    Params->SetStringField(TEXT("new_name"), TEXT("bodysizebox"));
    FCortexCommandResult CaseOnlyResult = Fixture.Router.Execute(TEXT("umg.rename_widget"), Params);
    TestFalse(TEXT("case-only new_name refused"), CaseOnlyResult.bSuccess);
    TestEqual(TEXT("case-only new_name error code"), CaseOnlyResult.ErrorCode, CortexErrorCodes::InvalidOperation);
    TestEqual(TEXT("refusals preserve object identity"), Widget->GetName(), FString(TEXT("BodySizeBox")));
    TestEqual(TEXT("refusals preserve binding reference"), WBP->Bindings.Last().ObjectName, FString(TEXT("BodySizeBox")));
    TestEqual(TEXT("refusals preserve animation reference"), Animation->AnimationBindings[0].WidgetName, FName(TEXT("BodySizeBox")));
    TestEqual(TEXT("refusals preserve dirty state"), WBP->GetPackage()->IsDirty(), bDirtyBefore);
    DependentPackage->SetDirtyFlag(false);
    TestFalse(TEXT("class-instance dependent is clean immediately before rename"), DependentPackage->IsDirty());
    Params->SetStringField(TEXT("new_name"), TEXT("RenamedBodySizeBox"));
    FCortexCommandResult Renamed = Fixture.Router.Execute(TEXT("umg.rename_widget"), Params);
    if (!TestTrue(TEXT("guarded rename succeeds"), Renamed.bSuccess)) return false;
    TestFalse(TEXT("class-instance-only dependent stays clean after allowed rename"), DependentPackage->IsDirty());
    TestEqual(TEXT("instance getter member preserved"), InstanceGetter->VariableReference.GetMemberName(), FName(TEXT("WidgetInstance")));
    TestEqual(TEXT("class-instance-only dependency keeps its variable"), Dependent->NewVariables[0].VarName, FName(TEXT("WidgetInstance")));
    TestEqual(TEXT("class-instance-only dependency keeps asset identity"), Dependent->GetName(), FString(TEXT("RenameDependent")));
    TestTrue(TEXT("rename reports changed"), Renamed.Data->GetBoolField(TEXT("changed")));
    TestTrue(TEXT("rename returns fresh tree fingerprint"), Renamed.Data->HasField(TEXT("fingerprint")));
    TestTrue(TEXT("same widget remains in same tree"), WBP->WidgetTree->FindWidget(TEXT("RenamedBodySizeBox")) == Widget);
    TestNull(TEXT("old widget name gone"), WBP->WidgetTree->FindWidget(TEXT("BodySizeBox")));
    TestEqual(TEXT("property binding reference renamed"), WBP->Bindings.Last().ObjectName, FString(TEXT("RenamedBodySizeBox")));
    TestEqual(TEXT("animation target renamed"), Animation->AnimationBindings[0].WidgetName, FName(TEXT("RenamedBodySizeBox")));
    TestEqual(TEXT("animation GUID preserved"), Animation->AnimationBindings[0].AnimationGuid, AnimationGuid);
    TestEqual(TEXT("movie scene possessable renamed"), Animation->MovieScene->FindPossessable(AnimationGuid)->GetName(), FString(TEXT("RenamedBodySizeBox")));
    TestEqual(TEXT("animation tracks preserved"), static_cast<const UMovieScene*>(Animation->MovieScene)->GetBindings()[0].GetTracks().Num(), TrackCount);
    Params->SetStringField(TEXT("widget_name"), TEXT("RenamedBodySizeBox"));
    Params->SetStringField(TEXT("new_name"), TEXT("AnotherName"));
    FCortexCommandResult Stale = Fixture.Router.Execute(TEXT("umg.rename_widget"), Params);
    TestFalse(TEXT("stale tree fingerprint refused"), Stale.bSuccess);
    TestEqual(TEXT("stale error reported"), Stale.ErrorCode, CortexErrorCodes::StalePrecondition);
    TestEqual(TEXT("stale refusal preserves widget"), Widget->GetName(), FString(TEXT("RenamedBodySizeBox")));
    TestEqual(TEXT("stale refusal preserves binding"), WBP->Bindings.Last().ObjectName, FString(TEXT("RenamedBodySizeBox")));
    Params->SetStringField(TEXT("new_name"), TEXT("RenamedBodySizeBox"));
    Params->SetObjectField(TEXT("expected_fingerprint"), Renamed.Data->GetObjectField(TEXT("fingerprint")));
    FCortexCommandResult NoOp = Fixture.Router.Execute(TEXT("umg.rename_widget"), Params);
    TestTrue(TEXT("same-name rename succeeds without mutation"), NoOp.bSuccess);
    if (NoOp.bSuccess) TestFalse(TEXT("same-name reports unchanged"), NoOp.Data->GetBoolField(TEXT("changed")));
    WBP->CachedDependents.Remove(Dependent);
    Dependent->CachedDependencies.Remove(WBP);
    Dependent->MarkAsGarbage();
    return true;
}

#if __has_include("UIComponentWidgetBlueprintExtension.h")
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexUMGRenameWidgetComponentDependentTest,
    "Cortex.UMG.RenameWidgetComponentDependent",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexUMGRenameWidgetComponentDependentTest::RunTest(const FString& Parameters)
{
    FCortexUMGAnimationBindingFixture Fixture(*this);
    UWidgetBlueprint* WBP = Fixture.Blueprint.Get();
    UWidget* Widget = WBP->WidgetTree->FindWidget(TEXT("BodySizeBox"));
    if (!TestNotNull(TEXT("component owner widget exists"), Widget)) return false;
    UUIComponentWidgetBlueprintExtension* Extension =
        UWidgetBlueprintExtension::RequestExtension<UUIComponentWidgetBlueprintExtension>(WBP);
    FText ComponentError;
    UUIComponent* Component = Extension->AddComponent(
        UNavigationUIComponent::StaticClass(), Widget->GetFName(), ComponentError);
    if (!TestNotNull(TEXT("real navigation component created"), Component)) return false;
    const FName ComponentMember = UUIComponentContainer::GetPropertyNameForComponent(
        Component, Widget->GetFName());
    FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(WBP);
    FKismetEditorUtilities::CompileBlueprint(WBP);
    if (!TestEqual(TEXT("component widget compiles without warnings"), WBP->Status, BS_UpToDate)
        || !TestNotNull(TEXT("component generated member exists"),
            FindFProperty<FObjectPropertyBase>(WBP->GeneratedClass, ComponentMember))) return false;

    UPackage* DependentPackage = CreatePackage(*(TEXT("/Temp/CortexRenameComponentDependent_")
        + FGuid::NewGuid().ToString(EGuidFormats::Digits)));
    UBlueprint* Dependent = FKismetEditorUtilities::CreateBlueprint(UObject::StaticClass(),
        DependentPackage, TEXT("ComponentDependent"), BPTYPE_Normal,
        UBlueprint::StaticClass(), UBlueprintGeneratedClass::StaticClass(), TEXT("CortexWidgetRenameTest"));
    if (!TestNotNull(TEXT("component dependent Blueprint created"), Dependent)) return false;
    ON_SCOPE_EXIT
    {
        WBP->CachedDependents.Remove(Dependent);
        Dependent->CachedDependencies.Remove(WBP);
        Dependent->MarkAsGarbage();
        DependentPackage->SetDirtyFlag(false);
        DependentPackage->MarkAsGarbage();
    };
    UEdGraph* Graph = FBlueprintEditorUtils::CreateNewGraph(Dependent, TEXT("ComponentGraph"),
        UEdGraph::StaticClass(), UEdGraphSchema_K2::StaticClass());
    Dependent->UbergraphPages.Add(Graph);
    UK2Node_VariableGet* Reference = NewObject<UK2Node_VariableGet>(Graph);
    Reference->VariableReference.SetExternalMember(ComponentMember, WBP->GeneratedClass);
    Reference->CreateNewGuid();
    Graph->AddNode(Reference, false, false);
    if (!TestNotNull(TEXT("external getter resolves real component property"),
        Reference->GetPropertyForVariable())) return false;
    Reference->AllocateDefaultPins();
    TestFalse(TEXT("component getter does not reference bare widget member"),
        Reference->ReferencesVariable(Widget->GetFName(), nullptr));
    TestTrue(TEXT("component getter references generated component member"),
        Reference->ReferencesVariable(ComponentMember, nullptr));
    Dependent->bCachedDependenciesUpToDate = false;
    TArray<UBlueprint*> Dependents;
    FBlueprintEditorUtils::FindDependentBlueprints(WBP, Dependents);
    if (!TestTrue(TEXT("engine discovers component-only dependent"), Dependents.Contains(Dependent))) return false;

    WBP->GetPackage()->SetDirtyFlag(false);
    DependentPackage->SetDirtyFlag(false);
    TSharedPtr<FJsonObject> StateParams = MakeShared<FJsonObject>();
    StateParams->SetStringField(TEXT("asset_path"), WBP->GetPathName());
    StateParams->SetStringField(TEXT("widget_name"), Widget->GetName());
    StateParams->SetBoolField(TEXT("is_variable"), Widget->bIsVariable);
    FCortexCommandResult State = Fixture.Router.Execute(TEXT("umg.set_widget_variable"), StateParams);
    if (!TestTrue(TEXT("component tree fingerprint available"), State.bSuccess)) return false;
    const FGuid ReferenceGuid = Reference->NodeGuid;
    TSharedPtr<FJsonObject> RenameParams = MakeShared<FJsonObject>();
    RenameParams->SetStringField(TEXT("asset_path"), WBP->GetPathName());
    RenameParams->SetStringField(TEXT("widget_name"), Widget->GetName());
    RenameParams->SetStringField(TEXT("new_name"), TEXT("RenamedBodySizeBox"));
    RenameParams->SetObjectField(TEXT("expected_fingerprint"), State.Data->GetObjectField(TEXT("fingerprint")));
    FCortexCommandResult Result = Fixture.Router.Execute(TEXT("umg.rename_widget"), RenameParams);
    TestFalse(TEXT("external component member refuses target-only rename"), Result.bSuccess);
    TestEqual(TEXT("component refusal reports containment"), Result.ErrorCode, CortexErrorCodes::InvalidOperation);
    TestEqual(TEXT("component refusal preserves widget name"), Widget->GetName(), FString(TEXT("BodySizeBox")));
    TestTrue(TEXT("component refusal preserves component target"),
        Extension->GetComponent(UNavigationUIComponent::StaticClass(), TEXT("BodySizeBox")) == Component);
    TestEqual(TEXT("component refusal preserves external member"), Reference->VariableReference.GetMemberName(), ComponentMember);
    TestEqual(TEXT("component refusal preserves external node identity"), Reference->NodeGuid, ReferenceGuid);
    TestFalse(TEXT("component refusal leaves target clean"), WBP->GetPackage()->IsDirty());
    TestFalse(TEXT("component refusal leaves dependent clean"), DependentPackage->IsDirty());
    return true;
}
#endif

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexUMGRenameWidgetSlotFirstAnimationBindingTest,
    "Cortex.UMG.RenameWidgetSlotFirstAnimationBinding",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexUMGRenameWidgetSlotFirstAnimationBindingTest::RunTest(const FString& Parameters)
{
    FCortexUMGAnimationBindingFixture Fixture(*this);
    UWidgetBlueprint* WBP = Fixture.Blueprint.Get();
    if (!TestNotNull(TEXT("fixture blueprint exists"), WBP))
    {
        return false;
    }

    UWidget* TargetWidget = WBP->WidgetTree ? WBP->WidgetTree->FindWidget(TEXT("BodySizeBox")) : nullptr;
    if (!TestNotNull(TEXT("target widget exists in tree"), TargetWidget))
    {
        return false;
    }

    UPanelSlot* TargetSlot = TargetWidget->Slot;
    if (!TestNotNull(TEXT("target widget has valid slot"), TargetSlot))
    {
        return false;
    }

    TargetWidget->bIsVariable = true;
    FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(WBP);
    FKismetEditorUtilities::CompileBlueprint(WBP);
    if (!TestTrue(TEXT("fixture compiles"), WBP->Status == BS_UpToDate || WBP->Status == BS_UpToDateWithWarnings))
    {
        return false;
    }

    if (!TestTrue(TEXT("fixture has animations"), WBP->Animations.Num() > 0))
    {
        return false;
    }

    UWidgetAnimation* Animation = WBP->Animations[0];
    if (!TestNotNull(TEXT("fixture animation exists"), Animation)
        || !TestNotNull(TEXT("animation movie scene exists"), Animation->MovieScene.Get()))
    {
        return false;
    }

    UMovieScene* MovieScene = Animation->MovieScene;
    if (!TestTrue(TEXT("animation has initial direct binding"), Animation->AnimationBindings.Num() > 0))
    {
        return false;
    }

    // Existing direct binding on BodySizeBox: snapshot track identities to avoid retaining
    // an internal FMovieSceneBinding pointer across MovieScene TArray reallocations
    const FGuid DirectGuid = Animation->AnimationBindings[0].AnimationGuid;
    TArray<UMovieSceneTrack*> InitialDirectTracks;
    {
        const FMovieSceneBinding* DirectBindingPtr = MovieScene->FindBinding(DirectGuid);
        if (!TestNotNull(TEXT("initial direct movie scene binding exists"), DirectBindingPtr))
        {
            return false;
        }
        for (UMovieSceneTrack* Track : DirectBindingPtr->GetTracks())
        {
            InitialDirectTracks.Add(Track);
        }
    }
    const int32 InitialDirectTrackCount = InitialDirectTracks.Num();

    // Create slot possessable and authored track for BodySizeBox.Slot
    const FName SlotWidgetName = TEXT("CanvasSlot");
    const FGuid SlotGuid = MovieScene->AddPossessable(TEXT("BodySizeBox.Slot"), TargetSlot->GetClass());

    UMovieSceneFloatTrack* SlotTrack = MovieScene->AddTrack<UMovieSceneFloatTrack>(SlotGuid);
    SlotTrack->SetPropertyNameAndPath(FName("LayoutData"), TEXT("LayoutData.Offsets.Left"));
    UMovieSceneFloatSection* SlotSection = Cast<UMovieSceneFloatSection>(SlotTrack->CreateNewSection());
    SlotTrack->AddSection(*SlotSection);
    SlotSection->SetRange(TRange<FFrameNumber>(FFrameNumber(120), FFrameNumber(720)));
    SlotSection->GetChannel().AddKeys(
        { FFrameNumber(120), FFrameNumber(720) },
        { FMovieSceneFloatValue(10.0f), FMovieSceneFloatValue(50.0f) });
    const TRange<FFrameNumber> InitialSlotSectionRange = SlotSection->GetRange();
    const int32 InitialSlotTrackCount = 1;
    const TRange<FFrameNumber> InitialPlaybackRange = MovieScene->GetPlaybackRange();

    // Insert slot binding at index 0, BEFORE the direct binding (index 1) for the same widget
    FWidgetAnimationBinding SlotBinding;
    SlotBinding.WidgetName = TEXT("BodySizeBox");
    SlotBinding.SlotWidgetName = SlotWidgetName;
    SlotBinding.AnimationGuid = SlotGuid;
    SlotBinding.bIsRootWidget = false;
    Animation->AnimationBindings.Insert(SlotBinding, 0);

    TestEqual(TEXT("binding 0 is slot binding for BodySizeBox"),
        Animation->AnimationBindings[0].WidgetName, FName(TEXT("BodySizeBox")));
    TestEqual(TEXT("binding 0 has slot name"),
        Animation->AnimationBindings[0].SlotWidgetName, SlotWidgetName);
    TestEqual(TEXT("binding 1 is direct binding for BodySizeBox"),
        Animation->AnimationBindings[1].WidgetName, FName(TEXT("BodySizeBox")));
    TestEqual(TEXT("binding 1 has no slot name"),
        Animation->AnimationBindings[1].SlotWidgetName, FName(NAME_None));

    UClass* WidgetClass = WBP->GeneratedClass.Get() ? static_cast<UClass*>(WBP->GeneratedClass.Get()) : UUserWidget::StaticClass();
    UUserWidget* PreviewWidget = NewObject<UUserWidget>(WBP, WidgetClass);
    if (!TestNotNull(TEXT("preview widget instance created"), PreviewWidget))
    {
        return false;
    }
    UObject* PreRenameSlotObj = Animation->AnimationBindings[0].FindRuntimeObject(
        *WBP->WidgetTree, *PreviewWidget, Animation, nullptr);
    TestEqual(TEXT("pre-rename slot binding resolves to slot"), PreRenameSlotObj, (UObject*)TargetSlot);

    UObject* PreRenameDirectObj = Animation->AnimationBindings[1].FindRuntimeObject(
        *WBP->WidgetTree, *PreviewWidget, Animation, nullptr);
    TestEqual(TEXT("pre-rename direct binding resolves to widget"), PreRenameDirectObj, (UObject*)TargetWidget);

    TSharedPtr<FJsonObject> GetFingerprint = MakeShared<FJsonObject>();
    GetFingerprint->SetStringField(TEXT("asset_path"), WBP->GetPathName());
    GetFingerprint->SetStringField(TEXT("widget_name"), TEXT("BodySizeBox"));
    GetFingerprint->SetBoolField(TEXT("is_variable"), TargetWidget->bIsVariable);
    FCortexCommandResult State = Fixture.Router.Execute(TEXT("umg.set_widget_variable"), GetFingerprint);
    if (!TestTrue(TEXT("tree fingerprint available"), State.bSuccess) || !State.Data.IsValid())
    {
        if (PreviewWidget)
        {
            PreviewWidget->MarkAsGarbage();
        }
        return false;
    }
    const TSharedPtr<FJsonObject> ExpectedFingerprint = State.Data->GetObjectField(TEXT("fingerprint"));

    if (GEditor && GEditor->Trans && GEditor->CanTransact())
    {
        GEditor->ResetTransaction(FText::FromString(TEXT("Cortex UMG SlotFirst Rename Setup")));
    }

    TSharedPtr<FJsonObject> RenameParams = MakeShared<FJsonObject>();
    RenameParams->SetStringField(TEXT("asset_path"), WBP->GetPathName());
    RenameParams->SetStringField(TEXT("widget_name"), TEXT("BodySizeBox"));
    RenameParams->SetStringField(TEXT("new_name"), TEXT("RenamedBodySizeBox"));
    RenameParams->SetObjectField(TEXT("expected_fingerprint"), ExpectedFingerprint);

    FCortexCommandResult RenameResult = Fixture.Router.Execute(TEXT("umg.rename_widget"), RenameParams);
    TestTrue(TEXT("rename command reports success"), RenameResult.bSuccess);
    if (RenameResult.bSuccess && RenameResult.Data.IsValid())
    {
        TestTrue(TEXT("rename reports changed true"), RenameResult.Data->GetBoolField(TEXT("changed")));
    }

    UWidget* RenamedWidget = WBP->WidgetTree->FindWidget(TEXT("RenamedBodySizeBox"));
    TestNotNull(TEXT("renamed widget exists in tree"), RenamedWidget);
    TestEqual(TEXT("renamed widget is same instance"), RenamedWidget, TargetWidget);
    TestNull(TEXT("old widget name not in tree"), WBP->WidgetTree->FindWidget(TEXT("BodySizeBox")));

    // Assertions on Binding 0 (slot binding)
    TestEqual(TEXT("slot binding target widget name updated"),
        Animation->AnimationBindings[0].WidgetName, FName(TEXT("RenamedBodySizeBox")));
    TestEqual(TEXT("slot binding slot widget name preserved"),
        Animation->AnimationBindings[0].SlotWidgetName, SlotWidgetName);
    TestEqual(TEXT("slot binding animation GUID preserved"),
        Animation->AnimationBindings[0].AnimationGuid, SlotGuid);

    UObject* PostRenameSlotObj = Animation->AnimationBindings[0].FindRuntimeObject(
        *WBP->WidgetTree, *PreviewWidget, Animation, nullptr);
    TestNotNull(TEXT("slot binding resolves runtime object after rename"), PostRenameSlotObj);
    TestEqual(TEXT("slot binding resolves to renamed widget slot"),
        PostRenameSlotObj, (UObject*)TargetSlot);

    // Assertions on Binding 1 (direct binding): verify rename reaches later bindings for the same widget
    TestEqual(TEXT("direct binding target widget name updated after slot binding"),
        Animation->AnimationBindings[1].WidgetName, FName(TEXT("RenamedBodySizeBox")));
    TestEqual(TEXT("direct binding slot widget name remains None"),
        Animation->AnimationBindings[1].SlotWidgetName, FName(NAME_None));
    TestEqual(TEXT("direct binding animation GUID preserved"),
        Animation->AnimationBindings[1].AnimationGuid, DirectGuid);

    UObject* PostRenameDirectObj = Animation->AnimationBindings[1].FindRuntimeObject(
        *WBP->WidgetTree, *PreviewWidget, Animation, nullptr);
    TestNotNull(TEXT("direct binding resolves runtime object after rename"), PostRenameDirectObj);
    TestEqual(TEXT("direct binding resolves to renamed widget object"),
        PostRenameDirectObj, (UObject*)RenamedWidget);

    // MovieScene possessable and binding assertions
    FMovieScenePossessable* DirectPossessable = MovieScene->FindPossessable(DirectGuid);
    TestNotNull(TEXT("direct possessable exists"), DirectPossessable);
    if (DirectPossessable)
    {
        TestEqual(TEXT("direct possessable renamed to match new widget name"),
            DirectPossessable->GetName(), FString(TEXT("RenamedBodySizeBox")));
    }

    FMovieScenePossessable* SlotPossessable = MovieScene->FindPossessable(SlotGuid);
    TestNotNull(TEXT("slot possessable exists"), SlotPossessable);

    const FMovieSceneBinding* PostSlotMSBinding = MovieScene->FindBinding(SlotGuid);
    TestNotNull(TEXT("slot movie scene binding preserved"), PostSlotMSBinding);
    if (PostSlotMSBinding)
    {
        TestEqual(TEXT("slot track count preserved"),
            PostSlotMSBinding->GetTracks().Num(), InitialSlotTrackCount);
        if (PostSlotMSBinding->GetTracks().Num() > 0)
        {
            TestEqual(TEXT("slot track identity preserved"),
                PostSlotMSBinding->GetTracks()[0], static_cast<UMovieSceneTrack*>(SlotTrack));
        }
    }

    TestEqual(TEXT("slot section count preserved"), SlotTrack->GetAllSections().Num(), 1);
    if (SlotTrack->GetAllSections().Num() > 0)
    {
        TestEqual(TEXT("slot section identity preserved"),
            SlotTrack->GetAllSections()[0], static_cast<UMovieSceneSection*>(SlotSection));
    }
    TestEqual(TEXT("slot section range preserved"), SlotSection->GetRange(), InitialSlotSectionRange);

    const FMovieSceneFloatChannel& PostSlotChannel = SlotSection->GetChannel();
    TestEqual(TEXT("slot channel key count preserved"), PostSlotChannel.GetNumKeys(), 2);
    TArrayView<const FFrameNumber> SlotKeyTimes = PostSlotChannel.GetTimes();
    TArrayView<const FMovieSceneFloatValue> SlotKeyValues = PostSlotChannel.GetValues();
    if (TestTrue(TEXT("slot channel key array valid"), SlotKeyTimes.Num() == 2 && SlotKeyValues.Num() == 2))
    {
        TestEqual(TEXT("slot key 0 time preserved"), SlotKeyTimes[0], FFrameNumber(120));
        TestEqual(TEXT("slot key 0 value preserved"), SlotKeyValues[0].Value, 10.0f);
        TestEqual(TEXT("slot key 1 time preserved"), SlotKeyTimes[1], FFrameNumber(720));
        TestEqual(TEXT("slot key 1 value preserved"), SlotKeyValues[1].Value, 50.0f);
    }
    const FMovieSceneBinding* PostDirectMSBinding = MovieScene->FindBinding(DirectGuid);
    TestNotNull(TEXT("direct movie scene binding preserved"), PostDirectMSBinding);
    if (PostDirectMSBinding)
    {
        TestEqual(TEXT("direct track count preserved"),
            PostDirectMSBinding->GetTracks().Num(), InitialDirectTrackCount);
        for (int32 TrackIdx = 0; TrackIdx < InitialDirectTrackCount && TrackIdx < PostDirectMSBinding->GetTracks().Num(); ++TrackIdx)
        {
            TestEqual(TEXT("direct track identity preserved"),
                PostDirectMSBinding->GetTracks()[TrackIdx], InitialDirectTracks[TrackIdx]);
        }
    }
    TestEqual(TEXT("movie scene playback range preserved"),
        MovieScene->GetPlaybackRange(), InitialPlaybackRange);

    // Undo / Redo transaction assertions:
    // Isolates whether the rename operation's entire effect (including later animation bindings
    // and possessables) was captured atomically in a single transaction.
    if (GEditor && GEditor->Trans && GEditor->CanTransact())
    {
        const bool bUndoSuccess = GEditor->UndoTransaction();
        TestTrue(TEXT("undo rename transaction succeeds"), bUndoSuccess);

        UWidget* RestoredWidget = WBP->WidgetTree->FindWidget(TEXT("BodySizeBox"));
        TestNotNull(TEXT("restored widget exists under old name after undo"), RestoredWidget);
        TestNull(TEXT("renamed widget gone after undo"), WBP->WidgetTree->FindWidget(TEXT("RenamedBodySizeBox")));
        TestEqual(TEXT("restored widget is same instance after undo"), RestoredWidget, TargetWidget);

        TestEqual(TEXT("slot binding target widget name restored after undo"),
            Animation->AnimationBindings[0].WidgetName, FName(TEXT("BodySizeBox")));
        TestEqual(TEXT("direct binding target widget name restored after undo"),
            Animation->AnimationBindings[1].WidgetName, FName(TEXT("BodySizeBox")));

        UObject* UndoSlotObj = Animation->AnimationBindings[0].FindRuntimeObject(
            *WBP->WidgetTree, *PreviewWidget, Animation, nullptr);
        TestNotNull(TEXT("slot binding resolves runtime object after undo"), UndoSlotObj);
        TestEqual(TEXT("slot binding resolves to restored widget slot after undo"),
            UndoSlotObj, (UObject*)TargetSlot);

        UObject* UndoDirectObj = Animation->AnimationBindings[1].FindRuntimeObject(
            *WBP->WidgetTree, *PreviewWidget, Animation, nullptr);
        TestNotNull(TEXT("direct binding resolves runtime object after undo"), UndoDirectObj);
        TestEqual(TEXT("direct binding resolves to restored widget after undo"),
            UndoDirectObj, (UObject*)RestoredWidget);

        FMovieScenePossessable* UndoDirectPossessable = MovieScene->FindPossessable(DirectGuid);
        TestNotNull(TEXT("direct possessable exists after undo"), UndoDirectPossessable);
        if (UndoDirectPossessable)
        {
            TestEqual(TEXT("direct possessable name restored to old name after undo"),
                UndoDirectPossessable->GetName(), FString(TEXT("BodySizeBox")));
        }

        TestEqual(TEXT("slot binding GUID unchanged after undo"),
            Animation->AnimationBindings[0].AnimationGuid, SlotGuid);
        TestEqual(TEXT("direct binding GUID unchanged after undo"),
            Animation->AnimationBindings[1].AnimationGuid, DirectGuid);
        TestEqual(TEXT("slot track count unchanged after undo"),
            PostSlotMSBinding ? PostSlotMSBinding->GetTracks().Num() : 0, InitialSlotTrackCount);
        TestEqual(TEXT("direct track count unchanged after undo"),
            PostDirectMSBinding ? PostDirectMSBinding->GetTracks().Num() : 0, InitialDirectTrackCount);

        const bool bRedoSuccess = GEditor->RedoTransaction();
        TestTrue(TEXT("redo rename transaction succeeds"), bRedoSuccess);

        UWidget* RedoRenamedWidget = WBP->WidgetTree->FindWidget(TEXT("RenamedBodySizeBox"));
        TestNotNull(TEXT("renamed widget exists under new name after redo"), RedoRenamedWidget);
        TestNull(TEXT("old widget name gone after redo"), WBP->WidgetTree->FindWidget(TEXT("BodySizeBox")));

        TestEqual(TEXT("slot binding target widget name updated after redo"),
            Animation->AnimationBindings[0].WidgetName, FName(TEXT("RenamedBodySizeBox")));
        TestEqual(TEXT("direct binding target widget name updated after redo"),
            Animation->AnimationBindings[1].WidgetName, FName(TEXT("RenamedBodySizeBox")));

        UObject* RedoSlotObj = Animation->AnimationBindings[0].FindRuntimeObject(
            *WBP->WidgetTree, *PreviewWidget, Animation, nullptr);
        TestNotNull(TEXT("slot binding resolves runtime object after redo"), RedoSlotObj);
        TestEqual(TEXT("slot binding resolves to renamed widget slot after redo"),
            RedoSlotObj, (UObject*)TargetSlot);

        UObject* RedoDirectObj = Animation->AnimationBindings[1].FindRuntimeObject(
            *WBP->WidgetTree, *PreviewWidget, Animation, nullptr);
        TestNotNull(TEXT("direct binding resolves runtime object after redo"), RedoDirectObj);
        TestEqual(TEXT("direct binding resolves to renamed widget after redo"),
            RedoDirectObj, (UObject*)RedoRenamedWidget);

        FMovieScenePossessable* RedoDirectPossessable = MovieScene->FindPossessable(DirectGuid);
        TestNotNull(TEXT("direct possessable exists after redo"), RedoDirectPossessable);
        if (RedoDirectPossessable)
        {
            TestEqual(TEXT("direct possessable renamed after redo"),
                RedoDirectPossessable->GetName(), FString(TEXT("RenamedBodySizeBox")));
        }

        TestEqual(TEXT("slot binding GUID unchanged after redo"),
            Animation->AnimationBindings[0].AnimationGuid, SlotGuid);
        TestEqual(TEXT("direct binding GUID unchanged after redo"),
            Animation->AnimationBindings[1].AnimationGuid, DirectGuid);

        GEditor->ResetTransaction(FText::FromString(TEXT("Cortex UMG SlotFirst Rename Cleanup")));
    }

    if (PreviewWidget)
    {
        PreviewWidget->MarkAsGarbage();
    }

    return true;
}
