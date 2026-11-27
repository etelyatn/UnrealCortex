// umg.get_live_widgets / umg.snapshot_live_widgets / umg.diff_live_widgets: runtime readback of live UserWidget
// instances in the PIE world and label-keyed snapshot/diff of that readback. The fixtures are test-only native
// UserWidget classes (CortexUMGLiveWidgetTestTypes.h) whose widget tree is built at runtime.

#include "Misc/AutomationTest.h"
#include "Tests/AutomationCommon.h"
#include "Tests/AutomationEditorCommon.h"
#include "CortexCommandRouter.h"
#include "CortexEngineCompat.h"
#include "CortexTypes.h"
#include "CortexUMGCommandHandler.h"
#include "Operations/CortexUMGLiveWidgetOps.h"
#include "CortexUMGLiveWidgetTestTypes.h"
#include "Blueprint/UserWidget.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Editor.h"
#include "Engine/World.h"
#include "GameFramework/PlayerController.h"
#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Guid.h"
#include "Misc/Paths.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "Widgets/SWidget.h"

namespace
{
	constexpr double LiveWidgetPIEWaitSeconds = 15.0;
	const TCHAR* const LiveBaseClassName = TEXT("CortexUMGLiveWidgetTestWidget");
	const TCHAR* const LiveBoundClassName = TEXT("CortexUMGLiveBoundTextTestWidget");

	/** Widgets and snapshot labels created by one test; released before PIE ends. */
	struct FLiveWidgetTestState
	{
		TArray<TWeakObjectPtr<UUserWidget>> Widgets;
		TArray<TSharedPtr<SWidget>> SlateHolds;
		TArray<FString> Labels;
	};

	using FLiveWidgetBody = TFunction<void(FAutomationTestBase&, FLiveWidgetTestState&)>;

	FCortexCommandResult RunUMG(const TCHAR* Command, const TSharedPtr<FJsonObject>& Params)
	{
		FCortexCommandRouter Router;
		Router.RegisterDomain(TEXT("umg"), TEXT("Cortex UMG"), TEXT("1.0.1"), MakeShared<FCortexUMGCommandHandler>());
		return Router.Execute(Command, Params);
	}

	TSharedPtr<FJsonObject> MakeClassParams(const TCHAR* ClassName)
	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("widget_class"), ClassName);
		return Params;
	}

	TSharedPtr<FJsonObject> MakeLabelParams(const FString& Label)
	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("label"), Label);
		return Params;
	}

	FString NewLabel(const TCHAR* Stem)
	{
		return FString::Printf(TEXT("%s_%s"), Stem, *FGuid::NewGuid().ToString(EGuidFormats::Digits).Right(8));
	}

	FString SnapshotFilePath(const FString& Label)
	{
		return FPaths::Combine(
			FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir()),
			TEXT("Cortex"), TEXT("WidgetSnapshots"), Label + TEXT(".json"));
	}

	UWorld* GetPlayWorld()
	{
		return GEditor != nullptr ? GEditor->PlayWorld.Get() : nullptr;
	}

	TSharedPtr<FJsonObject> SubObject(const TSharedPtr<FJsonObject>& Object, const TCHAR* Field)
	{
		const TSharedPtr<FJsonObject>* Sub = nullptr;
		return (Object.IsValid() && Object->TryGetObjectField(Field, Sub) && Sub != nullptr) ? *Sub : nullptr;
	}

	int32 ArrayCount(const TSharedPtr<FJsonObject>& Object, const TCHAR* Field)
	{
		const TArray<TSharedPtr<FJsonValue>>* Array = nullptr;
		return (Object.IsValid() && Object->TryGetArrayField(Field, Array) && Array != nullptr) ? Array->Num() : -1;
	}

	TSharedPtr<FJsonObject> InstanceAt(const FCortexCommandResult& Result, const int32 Index)
	{
		const TArray<TSharedPtr<FJsonValue>>* Instances = nullptr;
		if (!Result.Data.IsValid() || !Result.Data->TryGetArrayField(TEXT("instances"), Instances) || Instances == nullptr
			|| !Instances->IsValidIndex(Index))
		{
			return nullptr;
		}
		return (*Instances)[Index]->AsObject();
	}

	TSharedPtr<FJsonObject> ChildWidget(const TSharedPtr<FJsonObject>& Instance, const TCHAR* Name)
	{
		return SubObject(SubObject(Instance, TEXT("widgets")), Name);
	}

	FString ErrorText(const FCortexCommandResult& Result)
	{
		return FString::Printf(TEXT("%s: %s"), *Result.ErrorCode, *Result.ErrorMessage);
	}

	bool ExpectSuccess(FAutomationTestBase& Test, const TCHAR* What, const FCortexCommandResult& Result)
	{
		const bool bOk = Result.bSuccess && Result.Data.IsValid();
		Test.TestTrue(FString::Printf(TEXT("%s should succeed (%s)"), What, *ErrorText(Result)), bOk);
		return bOk;
	}

	TFunction<bool()> WaitForPIEPlaying(FAutomationTestBase* Test)
	{
		TSharedRef<double> StartTime = MakeShared<double>(0.0);
		return [Test, StartTime]()
		{
			if (*StartTime == 0.0)
			{
				*StartTime = FPlatformTime::Seconds();
			}
			const UWorld* PlayWorld = GetPlayWorld();
			if (PlayWorld != nullptr && PlayWorld->HasBegunPlay() && PlayWorld->GetFirstPlayerController() != nullptr)
			{
				return true;
			}
			if (FPlatformTime::Seconds() - *StartTime > LiveWidgetPIEWaitSeconds)
			{
				Test->AddError(TEXT("Timed out waiting for PIE to begin play"));
				return true;
			}
			return false;
		};
	}

	TFunction<bool()> WaitForPIEStopped(FAutomationTestBase* Test)
	{
		TSharedRef<double> StartTime = MakeShared<double>(0.0);
		return [Test, StartTime]()
		{
			if (*StartTime == 0.0)
			{
				*StartTime = FPlatformTime::Seconds();
			}
			if (GetPlayWorld() == nullptr)
			{
				return true;
			}
			if (FPlatformTime::Seconds() - *StartTime > LiveWidgetPIEWaitSeconds)
			{
				Test->AddError(TEXT("Timed out waiting for PIE to stop"));
				return true;
			}
			return false;
		};
	}

	/**
	 * Creates a fixture widget in the PIE world. With bBuildSlate it is added to the viewport and its Slate tree is
	 * held (the UWidget side only keeps weak references to Slate widgets); without it the widget stays unbuilt.
	 */
	UUserWidget* SpawnLiveWidget(FAutomationTestBase& Test, FLiveWidgetTestState& State, UClass* WidgetClass, const bool bBuildSlate)
	{
		UWorld* World = GetPlayWorld();
		if (World == nullptr)
		{
			Test.AddError(TEXT("No PIE world to create the fixture widget in"));
			return nullptr;
		}
		UUserWidget* Widget = CreateWidget<UUserWidget>(World, TSubclassOf<UUserWidget>(WidgetClass));
		if (Widget == nullptr)
		{
			Test.AddError(TEXT("CreateWidget returned null for the fixture widget"));
			return nullptr;
		}
		State.Widgets.Add(Widget);
		if (bBuildSlate)
		{
			Widget->AddToViewport();
			State.SlateHolds.Add(Widget->TakeWidget());
		}
		return Widget;
	}

	/** Runs before PIE ends: the Slate holders reference their UserWidget, and a leaked widget keeps the PIE world alive. */
	void CleanupLiveWidgetTest(FLiveWidgetTestState& State)
	{
		for (const TWeakObjectPtr<UUserWidget>& Weak : State.Widgets)
		{
			if (UUserWidget* Widget = Weak.Get())
			{
				Widget->RemoveFromParent();
			}
		}
		State.SlateHolds.Empty();
		State.Widgets.Empty();
		for (const FString& Label : State.Labels)
		{
			IFileManager::Get().Delete(*SnapshotFilePath(Label), false, true, true);
		}
		State.Labels.Empty();
	}

	/** PIE start, the body in one frame, widget/snapshot cleanup, PIE stop. */
	void RunInPIE(FAutomationTestBase* Test, FLiveWidgetBody Body)
	{
		TSharedRef<FLiveWidgetTestState> State = MakeShared<FLiveWidgetTestState>();
		ADD_LATENT_AUTOMATION_COMMAND(FStartPIECommand(false));
		ADD_LATENT_AUTOMATION_COMMAND(FFunctionLatentCommand(WaitForPIEPlaying(Test)));
		ADD_LATENT_AUTOMATION_COMMAND(FFunctionLatentCommand([Test, State, Body]()
		{
			Body(*Test, *State);
			return true;
		}));
		ADD_LATENT_AUTOMATION_COMMAND(FFunctionLatentCommand([State]()
		{
			CleanupLiveWidgetTest(*State);
			return true;
		}));
		ADD_LATENT_AUTOMATION_COMMAND(FEndPlayMapCommand());
		ADD_LATENT_AUTOMATION_COMMAND(FFunctionLatentCommand(WaitForPIEStopped(Test)));
	}

	/** Appends a copy of the first captured instance to the snapshot file, as if one more instance had existed. */
	bool AppendCopyOfFirstSnapshotInstance(FAutomationTestBase& Test, const FString& Label)
	{
		const FString Path = SnapshotFilePath(Label);
		FString Text;
		TSharedPtr<FJsonObject> Root;
		if (!FFileHelper::LoadFileToString(Text, *Path)
			|| !FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Text), Root) || !Root.IsValid())
		{
			Test.AddError(FString::Printf(TEXT("Could not read the snapshot file %s"), *Path));
			return false;
		}
		TArray<TSharedPtr<FJsonValue>> Instances = Root->GetArrayField(TEXT("instances"));
		if (Instances.IsEmpty())
		{
			Test.AddError(TEXT("The snapshot file holds no instance to copy"));
			return false;
		}
		TSharedPtr<FJsonObject> Copy = MakeShared<FJsonObject>();
		for (const auto& Pair : Instances[0]->AsObject()->Values)
		{
			Copy->SetField(CortexEngineCompat::JsonKeyToString(Pair.Key), Pair.Value);
		}
		Copy->SetNumberField(TEXT("index"), Instances.Num());
		Copy->SetStringField(TEXT("name"), TEXT("RemovedInstance"));
		Instances.Add(MakeShared<FJsonValueObject>(Copy));
		Root->SetArrayField(TEXT("instances"), Instances);

		FString Output;
		const TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> Writer =
			TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Output);
		FJsonSerializer::Serialize(Root.ToSharedRef(), Writer);
		if (!FFileHelper::SaveStringToFile(Output, *Path))
		{
			Test.AddError(FString::Printf(TEXT("Could not rewrite the snapshot file %s"), *Path));
			return false;
		}
		return true;
	}

	TSharedPtr<FJsonObject> MakeSnapshotParams(const FString& Label, const TCHAR* ClassName)
	{
		TSharedPtr<FJsonObject> Params = MakeClassParams(ClassName);
		Params->SetStringField(TEXT("label"), Label);
		return Params;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexUMGLiveWidgetsRequiresPIETest,
	"Cortex.UMG.LiveWidgets.RequiresPIE",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexUMGLiveWidgetsRequiresPIETest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	if (GetPlayWorld() != nullptr)
	{
		AddInfo(TEXT("PIE is already active in this editor session; the not-in-PIE contract cannot be observed"));
		return true;
	}

	const FCortexCommandResult Get = RunUMG(TEXT("umg.get_live_widgets"), MakeClassParams(LiveBaseClassName));
	TestFalse(TEXT("get_live_widgets outside PIE must fail"), Get.bSuccess);
	TestEqual(TEXT("get_live_widgets outside PIE error code"), Get.ErrorCode, CortexErrorCodes::PIENotActive);

	const FString Label = NewLabel(TEXT("cortex_requires_pie"));
	const FCortexCommandResult Snapshot = RunUMG(TEXT("umg.snapshot_live_widgets"), MakeSnapshotParams(Label, LiveBaseClassName));
	TestFalse(TEXT("snapshot_live_widgets outside PIE must fail"), Snapshot.bSuccess);
	TestEqual(TEXT("snapshot_live_widgets outside PIE error code"), Snapshot.ErrorCode, CortexErrorCodes::PIENotActive);
	TestFalse(TEXT("a refused snapshot must not leave a file"), IFileManager::Get().FileExists(*SnapshotFilePath(Label)));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexUMGLiveWidgetsReadsNamedChildrenTest,
	"Cortex.UMG.LiveWidgets.ReadsNamedChildren",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexUMGLiveWidgetsReadsNamedChildrenTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	RunInPIE(this, [](FAutomationTestBase& Test, FLiveWidgetTestState& State)
	{
		if (SpawnLiveWidget(Test, State, UCortexUMGLiveWidgetTestWidget::StaticClass(), true) == nullptr)
		{
			return;
		}

		TSharedPtr<FJsonObject> Params = MakeClassParams(LiveBaseClassName);
		Params->SetArrayField(TEXT("widgets"), {
			MakeShared<FJsonValueString>(TEXT("Title")),
			MakeShared<FJsonValueString>(TEXT("Icon")),
			MakeShared<FJsonValueString>(TEXT("NoSuchWidget"))});
		const FCortexCommandResult Result = RunUMG(TEXT("umg.get_live_widgets"), Params);
		if (!ExpectSuccess(Test, TEXT("get_live_widgets"), Result))
		{
			return;
		}
		Test.TestEqual(TEXT("one live instance"), static_cast<int32>(Result.Data->GetNumberField(TEXT("count"))), 1);
		Test.TestFalse(TEXT("not truncated"), Result.Data->GetBoolField(TEXT("truncated")));

		const TSharedPtr<FJsonObject> Instance = InstanceAt(Result, 0);
		if (!Test.TestTrue(TEXT("instance 0 present"), Instance.IsValid()))
		{
			return;
		}
		Test.TestEqual(TEXT("instance index"), static_cast<int32>(Instance->GetNumberField(TEXT("index"))), 0);
		Test.TestEqual(TEXT("instance class"), Instance->GetStringField(TEXT("class")), FString(LiveBaseClassName));

		const TSharedPtr<FJsonObject> Title = ChildWidget(Instance, TEXT("Title"));
		if (Test.TestTrue(TEXT("Title is read although bIsVariable is false"), Title.IsValid()))
		{
			Test.TestEqual(TEXT("Title text"), Title->GetStringField(TEXT("text")), FString(TEXT("Hello")));
			Test.TestEqual(TEXT("Title text source once the Slate widget exists"), Title->GetStringField(TEXT("text_source")), FString(TEXT("slate")));
			Test.TestTrue(TEXT("Title reports a visibility"), !Title->GetStringField(TEXT("visibility")).IsEmpty());
			Test.TestTrue(TEXT("Title reports is_enabled"), Title->HasField(TEXT("is_enabled")));
			Test.TestTrue(TEXT("Title reports render_opacity"), Title->HasField(TEXT("render_opacity")));
			Test.TestTrue(TEXT("Title style has a font size"), SubObject(Title, TEXT("style")).IsValid()
				&& SubObject(Title, TEXT("style"))->HasField(TEXT("size")));
		}

		const TSharedPtr<FJsonObject> Icon = ChildWidget(Instance, TEXT("Icon"));
		if (Test.TestTrue(TEXT("Icon is read"), Icon.IsValid()))
		{
			const TSharedPtr<FJsonObject> IconStyle = SubObject(Icon, TEXT("style"));
			Test.TestTrue(TEXT("Icon style carries brush_resource"), IconStyle.IsValid() && IconStyle->HasField(TEXT("brush_resource")));
			Test.TestFalse(TEXT("an image has no text"), Icon->HasField(TEXT("text")));
		}

		const TSharedPtr<FJsonObject> Properties = SubObject(Instance, TEXT("properties"));
		if (Test.TestTrue(TEXT("properties present"), Properties.IsValid()))
		{
			Test.TestEqual(TEXT("properties.Score"), static_cast<int32>(Properties->GetNumberField(TEXT("Score"))), 42);
			Test.TestFalse(TEXT("widget references are omitted from properties"), Properties->HasField(TEXT("TitleRef")));
		}

		Test.TestEqual(TEXT("the unknown name is listed once in missing"), ArrayCount(Instance, TEXT("missing")), 1);
		const TArray<TSharedPtr<FJsonValue>>* Missing = nullptr;
		if (Instance->TryGetArrayField(TEXT("missing"), Missing) && Missing != nullptr && Missing->Num() == 1)
		{
			Test.TestEqual(TEXT("missing entry"), (*Missing)[0]->AsString(), FString(TEXT("NoSuchWidget")));
		}
		Test.TestFalse(TEXT("a missing child is not in widgets"), ChildWidget(Instance, TEXT("NoSuchWidget")).IsValid());

		// Default selection: every named widget in the instance tree.
		const FCortexCommandResult All = RunUMG(TEXT("umg.get_live_widgets"), MakeClassParams(LiveBaseClassName));
		if (ExpectSuccess(Test, TEXT("get_live_widgets without widgets"), All))
		{
			const TSharedPtr<FJsonObject> AllInstance = InstanceAt(All, 0);
			Test.TestTrue(TEXT("default reads Root"), ChildWidget(AllInstance, TEXT("Root")).IsValid());
			Test.TestTrue(TEXT("default reads Title"), ChildWidget(AllInstance, TEXT("Title")).IsValid());
			Test.TestTrue(TEXT("default reads Icon"), ChildWidget(AllInstance, TEXT("Icon")).IsValid());
			Test.TestEqual(TEXT("default has nothing missing"), ArrayCount(AllInstance, TEXT("missing")), 0);
		}

		TSharedPtr<FJsonObject> NoProperties = MakeClassParams(LiveBaseClassName);
		NoProperties->SetBoolField(TEXT("include_properties"), false);
		const FCortexCommandResult Bare = RunUMG(TEXT("umg.get_live_widgets"), NoProperties);
		if (ExpectSuccess(Test, TEXT("get_live_widgets without properties"), Bare))
		{
			Test.TestFalse(TEXT("include_properties=false drops properties"), InstanceAt(Bare, 0)->HasField(TEXT("properties")));
		}
	});
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexUMGLiveWidgetsNoInstancesIsEmptyTest,
	"Cortex.UMG.LiveWidgets.NoInstancesIsEmpty",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexUMGLiveWidgetsNoInstancesIsEmptyTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	RunInPIE(this, [](FAutomationTestBase& Test, FLiveWidgetTestState& State)
	{
		(void)State;
		const FCortexCommandResult Result = RunUMG(TEXT("umg.get_live_widgets"), MakeClassParams(LiveBaseClassName));
		if (!ExpectSuccess(Test, TEXT("get_live_widgets with no instances"), Result))
		{
			return;
		}
		Test.TestEqual(TEXT("count is 0"), static_cast<int32>(Result.Data->GetNumberField(TEXT("count"))), 0);
		Test.TestEqual(TEXT("instances is empty"), ArrayCount(Result.Data, TEXT("instances")), 0);
		Test.TestFalse(TEXT("not truncated"), Result.Data->GetBoolField(TEXT("truncated")));
	});
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexUMGLiveWidgetsSnapshotThenDiffIdenticalTest,
	"Cortex.UMG.LiveWidgets.SnapshotThenDiffIdentical",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexUMGLiveWidgetsSnapshotThenDiffIdenticalTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	RunInPIE(this, [](FAutomationTestBase& Test, FLiveWidgetTestState& State)
	{
		if (SpawnLiveWidget(Test, State, UCortexUMGLiveWidgetTestWidget::StaticClass(), true) == nullptr)
		{
			return;
		}
		const FString Label = NewLabel(TEXT("cortex_identical"));
		State.Labels.Add(Label);

		const FCortexCommandResult Snapshot = RunUMG(TEXT("umg.snapshot_live_widgets"), MakeSnapshotParams(Label, LiveBaseClassName));
		if (!ExpectSuccess(Test, TEXT("snapshot_live_widgets"), Snapshot))
		{
			return;
		}
		Test.TestEqual(TEXT("snapshot label"), Snapshot.Data->GetStringField(TEXT("label")), Label);
		Test.TestEqual(TEXT("snapshot instance_count"), static_cast<int32>(Snapshot.Data->GetNumberField(TEXT("instance_count"))), 1);
		Test.TestFalse(TEXT("snapshot not truncated"), Snapshot.Data->GetBoolField(TEXT("truncated")));
		const int32 EntryCount = static_cast<int32>(Snapshot.Data->GetNumberField(TEXT("entry_count")));
		Test.TestTrue(TEXT("snapshot captured entries"), EntryCount > 0);
		Test.TestTrue(TEXT("snapshot file exists under Saved/Cortex/WidgetSnapshots"), IFileManager::Get().FileExists(*SnapshotFilePath(Label)));

		// diff takes only the label: the selectors come from the snapshot file.
		const FCortexCommandResult Diff = RunUMG(TEXT("umg.diff_live_widgets"), MakeLabelParams(Label));
		if (!ExpectSuccess(Test, TEXT("diff_live_widgets"), Diff))
		{
			return;
		}
		const int32 Total = static_cast<int32>(Diff.Data->GetNumberField(TEXT("total_entries")));
		Test.TestTrue(TEXT("diff compared entries"), Total > 0);
		Test.TestEqual(TEXT("diff compared exactly the captured entries"), Total, EntryCount);
		Test.TestEqual(TEXT("diff summary"), Diff.Data->GetStringField(TEXT("summary")), FString::Printf(TEXT("0 of %d entries differ"), Total));
		Test.TestEqual(TEXT("no differing entries"), static_cast<int32>(Diff.Data->GetNumberField(TEXT("differing_entries"))), 0);
		Test.TestTrue(TEXT("identical"), Diff.Data->GetBoolField(TEXT("identical")));
		Test.TestTrue(TEXT("complete"), Diff.Data->GetBoolField(TEXT("complete")));
		Test.TestEqual(TEXT("no differences listed"), ArrayCount(Diff.Data, TEXT("differences")), 0);
		Test.TestEqual(TEXT("nothing added"), ArrayCount(Diff.Data, TEXT("added_instances")), 0);
		Test.TestEqual(TEXT("nothing removed"), ArrayCount(Diff.Data, TEXT("removed_instances")), 0);
	});
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexUMGLiveWidgetsDiffReportsChangedTextTest,
	"Cortex.UMG.LiveWidgets.DiffReportsChangedText",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexUMGLiveWidgetsDiffReportsChangedTextTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	RunInPIE(this, [](FAutomationTestBase& Test, FLiveWidgetTestState& State)
	{
		UCortexUMGLiveWidgetTestWidget* Widget = Cast<UCortexUMGLiveWidgetTestWidget>(
			SpawnLiveWidget(Test, State, UCortexUMGLiveWidgetTestWidget::StaticClass(), true));
		if (Widget == nullptr || Widget->TitleRef == nullptr)
		{
			Test.AddError(TEXT("Fixture widget or its Title is missing"));
			return;
		}
		const FString Label = NewLabel(TEXT("cortex_changed_text"));
		State.Labels.Add(Label);

		if (!ExpectSuccess(Test, TEXT("snapshot_live_widgets"),
				RunUMG(TEXT("umg.snapshot_live_widgets"), MakeSnapshotParams(Label, LiveBaseClassName))))
		{
			return;
		}

		Widget->TitleRef->SetText(FText::FromString(TEXT("Bye")));

		const FCortexCommandResult Diff = RunUMG(TEXT("umg.diff_live_widgets"), MakeLabelParams(Label));
		if (!ExpectSuccess(Test, TEXT("diff_live_widgets"), Diff))
		{
			return;
		}
		Test.TestEqual(TEXT("exactly one differing entry"), static_cast<int32>(Diff.Data->GetNumberField(TEXT("differing_entries"))), 1);
		Test.TestFalse(TEXT("a changed readback is not identical"), Diff.Data->GetBoolField(TEXT("identical")));
		Test.TestTrue(TEXT("both readbacks are complete"), Diff.Data->GetBoolField(TEXT("complete")));

		const TArray<TSharedPtr<FJsonValue>>* Differences = nullptr;
		if (Test.TestTrue(TEXT("differences array"), Diff.Data->TryGetArrayField(TEXT("differences"), Differences) && Differences != nullptr)
			&& Test.TestEqual(TEXT("one difference listed"), Differences->Num(), 1))
		{
			const TSharedPtr<FJsonObject> Difference = (*Differences)[0]->AsObject();
			Test.TestEqual(TEXT("difference key"), Difference->GetStringField(TEXT("key")), FString(TEXT("[0].widgets.Title.text")));
			Test.TestEqual(TEXT("difference before"), Difference->GetStringField(TEXT("before")), FString(TEXT("Hello")));
			Test.TestEqual(TEXT("difference after"), Difference->GetStringField(TEXT("after")), FString(TEXT("Bye")));
		}
	});
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexUMGLiveWidgetsRejectsBadLabelTest,
	"Cortex.UMG.LiveWidgets.RejectsBadLabel",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexUMGLiveWidgetsRejectsBadLabelTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	const TArray<FString> BadLabels = {
		TEXT("../x"), TEXT(""), TEXT("a/b"), TEXT("a\\b"), TEXT("."), TEXT(".."), TEXT("has space"),
		FString::ChrN(65, TEXT('a'))};
	for (const FString& Label : BadLabels)
	{
		const FCortexCommandResult Snapshot = RunUMG(TEXT("umg.snapshot_live_widgets"), MakeSnapshotParams(Label, LiveBaseClassName));
		TestFalse(FString::Printf(TEXT("snapshot label '%s' must be refused"), *Label), Snapshot.bSuccess);
		TestEqual(FString::Printf(TEXT("snapshot label '%s' error code"), *Label), Snapshot.ErrorCode, CortexErrorCodes::InvalidField);

		const FCortexCommandResult Diff = RunUMG(TEXT("umg.diff_live_widgets"), MakeLabelParams(Label));
		TestFalse(FString::Printf(TEXT("diff label '%s' must be refused"), *Label), Diff.bSuccess);
		TestEqual(FString::Printf(TEXT("diff label '%s' error code"), *Label), Diff.ErrorCode, CortexErrorCodes::InvalidField);
	}

	// Every allowed character, at the maximum length, passes label validation (the snapshot is then simply absent).
	const FString GoodLabel = FString::ChrN(60, TEXT('a')) + TEXT("_.-9");
	const FCortexCommandResult Good = RunUMG(TEXT("umg.diff_live_widgets"), MakeLabelParams(GoodLabel));
	TestFalse(TEXT("an absent snapshot cannot be diffed"), Good.bSuccess);
	TestEqual(TEXT("a well-formed label reaches the file lookup"), Good.ErrorCode, CortexErrorCodes::FileNotFound);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexUMGLiveWidgetsDiffMissingSnapshotTest,
	"Cortex.UMG.LiveWidgets.DiffMissingSnapshot",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexUMGLiveWidgetsDiffMissingSnapshotTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	const FString Label = NewLabel(TEXT("cortex_never_written"));
	const FCortexCommandResult Diff = RunUMG(TEXT("umg.diff_live_widgets"), MakeLabelParams(Label));
	TestFalse(TEXT("diff of an absent snapshot must fail"), Diff.bSuccess);
	TestEqual(TEXT("absent snapshot error code"), Diff.ErrorCode, CortexErrorCodes::FileNotFound);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexUMGLiveWidgetsBoundTextReportsSlateSourceTest,
	"Cortex.UMG.LiveWidgets.BoundTextReportsSlateSource",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexUMGLiveWidgetsBoundTextReportsSlateSourceTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	RunInPIE(this, [](FAutomationTestBase& Test, FLiveWidgetTestState& State)
	{
		// Created first (lower FName number): its Slate tree exists, so the delegate-bound value is effective.
		const UUserWidget* Built = SpawnLiveWidget(Test, State, UCortexUMGLiveBoundTextTestWidget::StaticClass(), true);
		// Created second and never built: no Slate widget, so only the stored property value is available.
		const UUserWidget* Unbuilt = SpawnLiveWidget(Test, State, UCortexUMGLiveBoundTextTestWidget::StaticClass(), false);
		if (Built == nullptr || Unbuilt == nullptr)
		{
			return;
		}

		TSharedPtr<FJsonObject> Params = MakeClassParams(LiveBoundClassName);
		Params->SetArrayField(TEXT("widgets"), {MakeShared<FJsonValueString>(TEXT("Title"))});
		const FCortexCommandResult Result = RunUMG(TEXT("umg.get_live_widgets"), Params);
		if (!ExpectSuccess(Test, TEXT("get_live_widgets"), Result))
		{
			return;
		}
		Test.TestEqual(TEXT("two live instances"), static_cast<int32>(Result.Data->GetNumberField(TEXT("count"))), 2);

		const TSharedPtr<FJsonObject> BuiltTitle = ChildWidget(InstanceAt(Result, 0), TEXT("Title"));
		if (Test.TestTrue(TEXT("built instance Title present"), BuiltTitle.IsValid()))
		{
			Test.TestEqual(TEXT("built Title reports the bound value"), BuiltTitle->GetStringField(TEXT("text")), FString(TEXT("Bound")));
			Test.TestEqual(TEXT("built Title text source"), BuiltTitle->GetStringField(TEXT("text_source")), FString(TEXT("slate")));
			const TArray<TSharedPtr<FJsonValue>>* Bindings = nullptr;
			bool bHasTextBinding = false;
			if (BuiltTitle->TryGetArrayField(TEXT("bindings"), Bindings) && Bindings != nullptr)
			{
				for (const TSharedPtr<FJsonValue>& Binding : *Bindings)
				{
					bHasTextBinding |= Binding->AsString() == TEXT("Text");
				}
			}
			Test.TestTrue(TEXT("built Title lists the Text binding"), bHasTextBinding);
		}

		const TSharedPtr<FJsonObject> UnbuiltTitle = ChildWidget(InstanceAt(Result, 1), TEXT("Title"));
		if (Test.TestTrue(TEXT("unbuilt instance Title present"), UnbuiltTitle.IsValid()))
		{
			Test.TestEqual(TEXT("unbuilt Title reports the stored value"), UnbuiltTitle->GetStringField(TEXT("text")), FString(TEXT("Hello")));
			Test.TestEqual(TEXT("unbuilt Title text source"), UnbuiltTitle->GetStringField(TEXT("text_source")), FString(TEXT("property")));
		}
	});
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexUMGLiveWidgetsDiffCountMismatchTest,
	"Cortex.UMG.LiveWidgets.DiffCountMismatch",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexUMGLiveWidgetsDiffCountMismatchTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	RunInPIE(this, [](FAutomationTestBase& Test, FLiveWidgetTestState& State)
	{
		if (SpawnLiveWidget(Test, State, UCortexUMGLiveWidgetTestWidget::StaticClass(), true) == nullptr)
		{
			return;
		}
		const FString Label = NewLabel(TEXT("cortex_count_mismatch"));
		State.Labels.Add(Label);
		if (!ExpectSuccess(Test, TEXT("snapshot of one instance"),
				RunUMG(TEXT("umg.snapshot_live_widgets"), MakeSnapshotParams(Label, LiveBaseClassName))))
		{
			return;
		}

		// A second live instance after the snapshot: reported as added, its entries count as differing.
		if (SpawnLiveWidget(Test, State, UCortexUMGLiveWidgetTestWidget::StaticClass(), true) == nullptr)
		{
			return;
		}
		const FCortexCommandResult Added = RunUMG(TEXT("umg.diff_live_widgets"), MakeLabelParams(Label));
		if (ExpectSuccess(Test, TEXT("diff with an added instance"), Added))
		{
			Test.TestEqual(TEXT("one added instance"), ArrayCount(Added.Data, TEXT("added_instances")), 1);
			Test.TestEqual(TEXT("no removed instance"), ArrayCount(Added.Data, TEXT("removed_instances")), 0);
			Test.TestFalse(TEXT("a count mismatch is not identical"), Added.Data->GetBoolField(TEXT("identical")));
			Test.TestTrue(TEXT("both readbacks are complete"), Added.Data->GetBoolField(TEXT("complete")));
			const int32 Differing = static_cast<int32>(Added.Data->GetNumberField(TEXT("differing_entries")));
			Test.TestTrue(TEXT("the added instance's entries count as differing"), Differing > 0);

			const TArray<TSharedPtr<FJsonValue>>* AddedList = nullptr;
			if (Added.Data->TryGetArrayField(TEXT("added_instances"), AddedList) && AddedList != nullptr && AddedList->Num() == 1)
			{
				const TSharedPtr<FJsonObject> Entry = (*AddedList)[0]->AsObject();
				Test.TestEqual(TEXT("added instance index"), static_cast<int32>(Entry->GetNumberField(TEXT("index"))), 1);
				Test.TestEqual(TEXT("every differing entry belongs to the added instance"),
					static_cast<int32>(Entry->GetNumberField(TEXT("entry_count"))), Differing);
			}
		}

		// Two live instances, a snapshot that holds three: the extra stored instance is reported as removed.
		if (!ExpectSuccess(Test, TEXT("snapshot of two instances"),
				RunUMG(TEXT("umg.snapshot_live_widgets"), MakeSnapshotParams(Label, LiveBaseClassName)))
			|| !AppendCopyOfFirstSnapshotInstance(Test, Label))
		{
			return;
		}
		const FCortexCommandResult Removed = RunUMG(TEXT("umg.diff_live_widgets"), MakeLabelParams(Label));
		if (ExpectSuccess(Test, TEXT("diff with a removed instance"), Removed))
		{
			Test.TestEqual(TEXT("one removed instance"), ArrayCount(Removed.Data, TEXT("removed_instances")), 1);
			Test.TestEqual(TEXT("no added instance"), ArrayCount(Removed.Data, TEXT("added_instances")), 0);
			Test.TestFalse(TEXT("a count mismatch is not identical"), Removed.Data->GetBoolField(TEXT("identical")));
			Test.TestTrue(TEXT("removed entries count as differing"), Removed.Data->GetNumberField(TEXT("differing_entries")) > 0);
		}
	});
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexUMGLiveWidgetsTruncatedSnapshotIncompleteTest,
	"Cortex.UMG.LiveWidgets.TruncatedSnapshotIncomplete",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexUMGLiveWidgetsTruncatedSnapshotIncompleteTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	RunInPIE(this, [](FAutomationTestBase& Test, FLiveWidgetTestState& State)
	{
		for (int32 Index = 0; Index < 2; ++Index)
		{
			if (SpawnLiveWidget(Test, State, UCortexUMGLiveWidgetTestWidget::StaticClass(), true) == nullptr)
			{
				return;
			}
		}

		TSharedPtr<FJsonObject> GetParams = MakeClassParams(LiveBaseClassName);
		GetParams->SetNumberField(TEXT("max_instances"), 1);
		const FCortexCommandResult Get = RunUMG(TEXT("umg.get_live_widgets"), GetParams);
		if (ExpectSuccess(Test, TEXT("get_live_widgets with max_instances 1"), Get))
		{
			Test.TestEqual(TEXT("one instance returned"), static_cast<int32>(Get.Data->GetNumberField(TEXT("count"))), 1);
			Test.TestTrue(TEXT("the cut is reported"), Get.Data->GetBoolField(TEXT("truncated")));
		}

		const FString Label = NewLabel(TEXT("cortex_truncated"));
		State.Labels.Add(Label);
		TSharedPtr<FJsonObject> SnapshotParams = MakeSnapshotParams(Label, LiveBaseClassName);
		SnapshotParams->SetNumberField(TEXT("max_instances"), 1);
		const FCortexCommandResult Snapshot = RunUMG(TEXT("umg.snapshot_live_widgets"), SnapshotParams);
		if (!ExpectSuccess(Test, TEXT("truncated snapshot"), Snapshot))
		{
			return;
		}
		Test.TestTrue(TEXT("snapshot reports truncated"), Snapshot.Data->GetBoolField(TEXT("truncated")));
		Test.TestEqual(TEXT("snapshot kept one instance"), static_cast<int32>(Snapshot.Data->GetNumberField(TEXT("instance_count"))), 1);

		// The diff re-reads with the snapshot's own max_instances, so the values match but the comparison is partial.
		const FCortexCommandResult Diff = RunUMG(TEXT("umg.diff_live_widgets"), MakeLabelParams(Label));
		if (!ExpectSuccess(Test, TEXT("diff of a truncated snapshot"), Diff))
		{
			return;
		}
		Test.TestEqual(TEXT("the compared instance matches"), static_cast<int32>(Diff.Data->GetNumberField(TEXT("differing_entries"))), 0);
		Test.TestFalse(TEXT("a truncated comparison is not complete"), Diff.Data->GetBoolField(TEXT("complete")));
		Test.TestFalse(TEXT("identical requires complete"), Diff.Data->GetBoolField(TEXT("identical")));
		const int32 Total = static_cast<int32>(Diff.Data->GetNumberField(TEXT("total_entries")));
		Test.TestTrue(TEXT("entries were compared"), Total > 0);
		Test.TestEqual(TEXT("summary"), Diff.Data->GetStringField(TEXT("summary")), FString::Printf(TEXT("0 of %d entries differ"), Total));
	});
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexUMGLiveWidgetsDiffIsCaseSensitiveTest,
	"Cortex.UMG.LiveWidgets.DiffIsCaseSensitive",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexUMGLiveWidgetsDiffIsCaseSensitiveTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	RunInPIE(this, [](FAutomationTestBase& Test, FLiveWidgetTestState& State)
	{
		UCortexUMGLiveWidgetTestWidget* Widget = Cast<UCortexUMGLiveWidgetTestWidget>(
			SpawnLiveWidget(Test, State, UCortexUMGLiveWidgetTestWidget::StaticClass(), true));
		if (Widget == nullptr || Widget->TitleRef == nullptr)
		{
			Test.AddError(TEXT("Fixture widget or its Title is missing"));
			return;
		}
		const FString Label = NewLabel(TEXT("cortex_case_sensitive"));
		State.Labels.Add(Label);
		if (!ExpectSuccess(Test, TEXT("snapshot_live_widgets"),
				RunUMG(TEXT("umg.snapshot_live_widgets"), MakeSnapshotParams(Label, LiveBaseClassName))))
		{
			return;
		}

		// Only the case changes: "Hello" -> "HELLO".
		Widget->TitleRef->SetText(FText::FromString(TEXT("HELLO")));

		const FCortexCommandResult Diff = RunUMG(TEXT("umg.diff_live_widgets"), MakeLabelParams(Label));
		if (!ExpectSuccess(Test, TEXT("diff_live_widgets"), Diff))
		{
			return;
		}
		Test.TestEqual(TEXT("a case-only change is one differing entry"), static_cast<int32>(Diff.Data->GetNumberField(TEXT("differing_entries"))), 1);
		Test.TestFalse(TEXT("a case-only change is not identical"), Diff.Data->GetBoolField(TEXT("identical")));
		const TArray<TSharedPtr<FJsonValue>>* Differences = nullptr;
		if (Diff.Data->TryGetArrayField(TEXT("differences"), Differences) && Differences != nullptr && Differences->Num() == 1)
		{
			const TSharedPtr<FJsonObject> Difference = (*Differences)[0]->AsObject();
			Test.TestEqual(TEXT("difference key"), Difference->GetStringField(TEXT("key")), FString(TEXT("[0].widgets.Title.text")));
			Test.TestEqual(TEXT("difference before"), Difference->GetStringField(TEXT("before")), FString(TEXT("Hello")));
			Test.TestEqual(TEXT("difference after"), Difference->GetStringField(TEXT("after")), FString(TEXT("HELLO")));
		}
		else
		{
			Test.AddError(TEXT("Expected exactly one listed difference"));
		}
	});
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexUMGLiveWidgetsFlattenKeepsEmptyAndEscapesTest,
	"Cortex.UMG.LiveWidgets.FlattenKeepsEmptyAndEscapes",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexUMGLiveWidgetsFlattenKeepsEmptyAndEscapesTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	const TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetNumberField(TEXT("a.b"), 1);
	const TSharedRef<FJsonObject> Nested = MakeShared<FJsonObject>();
	Nested->SetNumberField(TEXT("b"), 2);
	Root->SetObjectField(TEXT("a"), Nested);
	Root->SetObjectField(TEXT("e"), MakeShared<FJsonObject>());
	Root->SetArrayField(TEXT("l"), TArray<TSharedPtr<FJsonValue>>());
	Root->SetNumberField(TEXT("k[0]"), 3);
	Root->SetNumberField(TEXT("bs\\"), 4);

	TMap<FString, TSharedPtr<FJsonValue>> Entries;
	FCortexUMGLiveWidgetOps::FlattenValue(FString(), MakeShared<FJsonValueObject>(Root), Entries);

	TestEqual(TEXT("every member is its own entry, empty containers included"), Entries.Num(), 6);
	TestTrue(TEXT("literal key a.b is escaped"), Entries.Contains(TEXT("a\\.b")));
	TestTrue(TEXT("nested a -> b keeps the plain path"), Entries.Contains(TEXT("a.b")));
	TestTrue(TEXT("literal brackets are escaped"), Entries.Contains(TEXT("k\\[0\\]")));
	TestTrue(TEXT("a backslash is escaped"), Entries.Contains(TEXT("bs\\\\")));
	if (!Entries.Contains(TEXT("e")) || !Entries.Contains(TEXT("l")) || !Entries.Contains(TEXT("a\\.b")) || !Entries.Contains(TEXT("a.b")))
	{
		AddError(TEXT("Missing flattened entries"));
		return true;
	}
	TestEqual(TEXT("literal a.b value"), Entries[TEXT("a\\.b")]->AsNumber(), 1.0);
	TestEqual(TEXT("nested a -> b value"), Entries[TEXT("a.b")]->AsNumber(), 2.0);

	const TSharedPtr<FJsonValue> EmptyObject = Entries[TEXT("e")];
	const TSharedPtr<FJsonValue> EmptyArray = Entries[TEXT("l")];
	TestEqual(TEXT("empty object leaf type"), static_cast<int32>(EmptyObject->Type), static_cast<int32>(EJson::Object));
	TestEqual(TEXT("empty array leaf type"), static_cast<int32>(EmptyArray->Type), static_cast<int32>(EJson::Array));
	TestTrue(TEXT("an empty object equals an empty object"), FCortexUMGLiveWidgetOps::LeavesEqual(EmptyObject, MakeShared<FJsonValueObject>(MakeShared<FJsonObject>())));
	TestFalse(TEXT("an empty object differs from an empty array"), FCortexUMGLiveWidgetOps::LeavesEqual(EmptyObject, EmptyArray));
	TestFalse(TEXT("an empty object differs from the string {}"), FCortexUMGLiveWidgetOps::LeavesEqual(EmptyObject, MakeShared<FJsonValueString>(TEXT("{}"))));
	TestFalse(TEXT("an empty array differs from the string []"), FCortexUMGLiveWidgetOps::LeavesEqual(EmptyArray, MakeShared<FJsonValueString>(TEXT("[]"))));

	// A property that goes from empty to non-empty is indexed entries instead of the empty leaf, so the keys differ.
	TMap<FString, TSharedPtr<FJsonValue>> Filled;
	const TSharedRef<FJsonObject> FilledRoot = MakeShared<FJsonObject>();
	FilledRoot->SetArrayField(TEXT("l"), {MakeShared<FJsonValueNumber>(1)});
	FCortexUMGLiveWidgetOps::FlattenValue(FString(), MakeShared<FJsonValueObject>(FilledRoot), Filled);
	TestTrue(TEXT("a filled array flattens to indexed entries only"), Filled.Contains(TEXT("l[0]")) && !Filled.Contains(TEXT("l")));

	TestTrue(TEXT("identical strings are equal"),
		FCortexUMGLiveWidgetOps::LeavesEqual(MakeShared<FJsonValueString>(TEXT("Hello")), MakeShared<FJsonValueString>(TEXT("Hello"))));
	TestFalse(TEXT("strings compare case-sensitively"),
		FCortexUMGLiveWidgetOps::LeavesEqual(MakeShared<FJsonValueString>(TEXT("Hello")), MakeShared<FJsonValueString>(TEXT("HELLO"))));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexUMGLiveWidgetsDiffRejectsMalformedSnapshotTest,
	"Cortex.UMG.LiveWidgets.DiffRejectsMalformedSnapshot",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexUMGLiveWidgetsDiffRejectsMalformedSnapshotTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	struct FMalformedCase
	{
		const TCHAR* Name;
		const TCHAR* FormatVersion;
		const TCHAR* Truncated;
		const TCHAR* Instances;
	};
	const FMalformedCase Cases[] = {
		{TEXT("fractional format_version"), TEXT("1.5"), TEXT("false"), TEXT("[]")},
		{TEXT("string format_version"), TEXT("\"1\""), TEXT("false"), TEXT("[]")},
		{TEXT("null instance"), TEXT("1"), TEXT("false"), TEXT("[null]")},
		{TEXT("non-object instance"), TEXT("1"), TEXT("false"), TEXT("[1]")},
		{TEXT("string truncated"), TEXT("1"), TEXT("\"no\""), TEXT("[]")},
		{TEXT("missing truncated"), TEXT("1"), nullptr, TEXT("[]")},
	};

	const auto WriteSnapshot = [this](const FString& Label, const FMalformedCase& Case)
	{
		const FString TruncatedField = Case.Truncated != nullptr ? FString::Printf(TEXT("\"truncated\":%s,"), Case.Truncated) : FString();
		const FString Json = FString::Printf(
			TEXT("{\"format_version\":%s,\"widget_class\":\"%s\",\"include_properties\":true,\"max_instances\":10,%s\"instances\":%s}"),
			Case.FormatVersion, LiveBaseClassName, *TruncatedField, Case.Instances);
		if (!FFileHelper::SaveStringToFile(Json, *SnapshotFilePath(Label)))
		{
			AddError(FString::Printf(TEXT("Could not write the malformed snapshot %s"), *Label));
		}
	};

	for (const FMalformedCase& Case : Cases)
	{
		const FString Label = NewLabel(TEXT("cortex_malformed"));
		WriteSnapshot(Label, Case);
		const FCortexCommandResult Diff = RunUMG(TEXT("umg.diff_live_widgets"), MakeLabelParams(Label));
		TestFalse(FString::Printf(TEXT("%s must be refused"), Case.Name), Diff.bSuccess);
		TestEqual(FString::Printf(TEXT("%s error code"), Case.Name), Diff.ErrorCode, CortexErrorCodes::InvalidField);
		IFileManager::Get().Delete(*SnapshotFilePath(Label), false, true, true);
	}

	// A well-formed file passes validation and then meets the PIE requirement.
	if (GetPlayWorld() == nullptr)
	{
		const FMalformedCase Valid = {TEXT("valid"), TEXT("1"), TEXT("false"), TEXT("[]")};
		const FString Label = NewLabel(TEXT("cortex_wellformed"));
		WriteSnapshot(Label, Valid);
		const FCortexCommandResult Diff = RunUMG(TEXT("umg.diff_live_widgets"), MakeLabelParams(Label));
		TestFalse(TEXT("a valid snapshot outside PIE fails"), Diff.bSuccess);
		TestEqual(TEXT("a valid snapshot reaches the PIE check"), Diff.ErrorCode, CortexErrorCodes::PIENotActive);
		IFileManager::Get().Delete(*SnapshotFilePath(Label), false, true, true);
	}
	return true;
}
