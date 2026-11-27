// editor.capture_stats averages the engine stats call tree over N frames and answers once through the deferred
// callback: field validation, the reply shape, the filter, one capture at a time, the deadline (TIMEOUT, capture
// released) and per-function cost of a known UFunction invoked through ProcessEvent every frame.

#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "CortexEditorCommandHandler.h"
#include "CortexEditorStatsProbe.h"
#include "CortexTypes.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "HAL/PlatformTime.h"
#include "Operations/CortexEditorStatsOps.h"
#include "UObject/Package.h"
#include "UObject/UObjectGlobals.h"

namespace
{
	const TCHAR* const StatsCaptureCommandName = TEXT("capture_stats");
	/** Upper bound for every latent wait in this file. Each capture's own deadline is shorter. */
	constexpr double StatsLatentWaitSeconds = 15.0;

	/** Collects the deferred responses of one capture. The callback runs on the game thread. */
	struct FStatsCaptureProbe
	{
		int32 CallbackCount = 0;
		TOptional<FCortexCommandResult> Result;
		double StartSeconds = 0.0;

		bool WaitExpired() const
		{
			return (FPlatformTime::Seconds() - StartSeconds) > StatsLatentWaitSeconds;
		}
	};

	TSharedPtr<FJsonObject> MakeStatsCaptureParams(int32 Frames, double TimeoutSeconds, const TArray<FString>& Filters = TArray<FString>())
	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetNumberField(TEXT("frames"), Frames);
		Params->SetNumberField(TEXT("timeout_seconds"), TimeoutSeconds);
		if (Filters.Num() > 0)
		{
			TArray<TSharedPtr<FJsonValue>> Values;
			for (const FString& Filter : Filters)
			{
				Values.Add(MakeShared<FJsonValueString>(Filter));
			}
			Params->SetArrayField(TEXT("filter"), Values);
		}
		return Params;
	}

	/** Starts a capture; the one deferred answer lands in Probe. */
	FCortexCommandResult StartStatsCapture(
		FCortexEditorCommandHandler& Handler,
		const TSharedPtr<FJsonObject>& Params,
		const TSharedRef<FStatsCaptureProbe>& Probe)
	{
		Probe->StartSeconds = FPlatformTime::Seconds();
		return Handler.Execute(StatsCaptureCommandName, Params, [Probe](FCortexCommandResult Result)
		{
			++Probe->CallbackCount;
			Probe->Result = MoveTemp(Result);
		});
	}

	bool StatsTextContains(const FString& Text, const FString& Needle)
	{
		return Text.Contains(Needle, ESearchCase::IgnoreCase);
	}

	/** Checks one node and its children against the documented shape. */
	void ExpectStatsNodeShape(FAutomationTestBase& Test, const TSharedPtr<FJsonValue>& NodeValue, int32 Depth)
	{
		const TSharedPtr<FJsonObject>* Node = nullptr;
		if (!Test.TestTrue(TEXT("node is an object"), NodeValue.IsValid() && NodeValue->TryGetObject(Node)))
		{
			return;
		}
		FString Text;
		double Number = 0.0;
		Test.TestTrue(TEXT("node has name"), (*Node)->TryGetStringField(TEXT("name"), Text));
		Test.TestTrue(TEXT("node has description"), (*Node)->TryGetStringField(TEXT("description"), Text));
		if (Test.TestTrue(TEXT("node has inclusive_ms"), (*Node)->TryGetNumberField(TEXT("inclusive_ms"), Number)))
		{
			Test.TestTrue(TEXT("inclusive_ms is not negative"), Number >= 0.0);
		}
		Test.TestTrue(TEXT("node has exclusive_ms"), (*Node)->TryGetNumberField(TEXT("exclusive_ms"), Number));
		if (Test.TestTrue(TEXT("node has calls_per_frame"), (*Node)->TryGetNumberField(TEXT("calls_per_frame"), Number)))
		{
			Test.TestTrue(TEXT("calls_per_frame is not negative"), Number >= 0.0);
		}
		const TArray<TSharedPtr<FJsonValue>>* Children = nullptr;
		if (Test.TestTrue(TEXT("node has children"), (*Node)->TryGetArrayField(TEXT("children"), Children)) && Depth < 2)
		{
			for (const TSharedPtr<FJsonValue>& Child : *Children)
			{
				ExpectStatsNodeShape(Test, Child, Depth + 1);
			}
		}
	}

	/** Validates the success reply envelope shared by every successful capture and returns its nodes. */
	const TArray<TSharedPtr<FJsonValue>>* ExpectStatsSuccessShape(
		FAutomationTestBase& Test, const FCortexCommandResult& Result, int32 ExpectedFrames)
	{
		if (!Test.TestTrue(TEXT("capture succeeded"), Result.bSuccess) || !Test.TestTrue(TEXT("reply has data"), Result.Data.IsValid()))
		{
			Test.AddInfo(FString::Printf(TEXT("capture error %s: %s"), *Result.ErrorCode, *Result.ErrorMessage));
			return nullptr;
		}
		double Number = 0.0;
		if (Test.TestTrue(TEXT("frame_count present"), Result.Data->TryGetNumberField(TEXT("frame_count"), Number)))
		{
			Test.TestEqual(TEXT("frame_count"), static_cast<int32>(Number), ExpectedFrames);
		}
		double FirstFrame = 0.0;
		double LastFrame = 0.0;
		if (Test.TestTrue(TEXT("first_frame present"), Result.Data->TryGetNumberField(TEXT("first_frame"), FirstFrame))
			&& Test.TestTrue(TEXT("last_frame present"), Result.Data->TryGetNumberField(TEXT("last_frame"), LastFrame)))
		{
			Test.TestTrue(TEXT("last_frame is not before first_frame"), LastFrame >= FirstFrame);
		}
		const TSharedPtr<FJsonObject>* FrameTime = nullptr;
		if (Test.TestTrue(TEXT("frame_time_ms present"), Result.Data->TryGetObjectField(TEXT("frame_time_ms"), FrameTime)))
		{
			double Average = 0.0;
			double Maximum = 0.0;
			if (Test.TestTrue(TEXT("frame_time_ms.avg present"), (*FrameTime)->TryGetNumberField(TEXT("avg"), Average))
				&& Test.TestTrue(TEXT("frame_time_ms.max present"), (*FrameTime)->TryGetNumberField(TEXT("max"), Maximum)))
			{
				Test.TestTrue(TEXT("frame_time_ms.avg is positive"), Average > 0.0);
				Test.TestTrue(TEXT("frame_time_ms.max is not below avg"), Maximum >= Average);
			}
		}
		bool bTruncated = false;
		Test.TestTrue(TEXT("truncated present"), Result.Data->TryGetBoolField(TEXT("truncated"), bTruncated));

		const TArray<TSharedPtr<FJsonValue>>* Nodes = nullptr;
		if (!Test.TestTrue(TEXT("nodes present"), Result.Data->TryGetArrayField(TEXT("nodes"), Nodes)))
		{
			return nullptr;
		}
		return Nodes;
	}

	bool StatsNodeMatchesText(const TSharedPtr<FJsonValue>& NodeValue, const FString& Needle)
	{
		const TSharedPtr<FJsonObject>* Node = nullptr;
		if (!NodeValue.IsValid() || !NodeValue->TryGetObject(Node))
		{
			return false;
		}
		FString Name;
		FString Description;
		(*Node)->TryGetStringField(TEXT("name"), Name);
		(*Node)->TryGetStringField(TEXT("description"), Description);
		return StatsTextContains(Name, Needle) || StatsTextContains(Description, Needle);
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexEditorStatsCaptureRejectsInvalidParamsTest,
	"Cortex.Editor.Stats.CaptureRejectsInvalidParams",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexEditorStatsCaptureRejectsInvalidParamsTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	FCortexEditorCommandHandler Handler;
	int32 CallbackCount = 0;

	const auto ExpectInvalid = [this, &Handler, &CallbackCount](const TCHAR* Label, const TSharedPtr<FJsonObject>& Params)
	{
		const FCortexCommandResult Result = Handler.Execute(
			StatsCaptureCommandName, Params, [&CallbackCount](FCortexCommandResult) { ++CallbackCount; });
		TestFalse(FString::Printf(TEXT("%s is refused"), Label), Result.bSuccess);
		TestFalse(FString::Printf(TEXT("%s is not deferred"), Label), Result.bIsDeferred);
		TestEqual(FString::Printf(TEXT("%s code"), Label), Result.ErrorCode, CortexErrorCodes::InvalidField);
		TestFalse(FString::Printf(TEXT("%s starts no capture"), Label), FCortexEditorStatsOps::IsCaptureActive());
	};
	const auto WithNumber = [](const TCHAR* Field, double Value)
	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetNumberField(Field, Value);
		return Params;
	};

	ExpectInvalid(TEXT("frames 0"), WithNumber(TEXT("frames"), 0.0));
	ExpectInvalid(TEXT("frames 601"), WithNumber(TEXT("frames"), 601.0));
	ExpectInvalid(TEXT("fractional frames"), WithNumber(TEXT("frames"), 2.5));
	ExpectInvalid(TEXT("negative min_ms"), WithNumber(TEXT("min_ms"), -0.5));
	ExpectInvalid(TEXT("max_depth 0"), WithNumber(TEXT("max_depth"), 0.0));
	ExpectInvalid(TEXT("timeout_seconds 0"), WithNumber(TEXT("timeout_seconds"), 0.0));
	ExpectInvalid(TEXT("timeout_seconds 301"), WithNumber(TEXT("timeout_seconds"), 301.0));

	TSharedPtr<FJsonObject> StringFrames = MakeShared<FJsonObject>();
	StringFrames->SetStringField(TEXT("frames"), TEXT("5"));
	ExpectInvalid(TEXT("string frames"), StringFrames);

	TSharedPtr<FJsonObject> StringFilter = MakeShared<FJsonObject>();
	StringFilter->SetStringField(TEXT("filter"), TEXT("FrameTime"));
	ExpectInvalid(TEXT("filter that is not an array"), StringFilter);

	TSharedPtr<FJsonObject> NumberFilterEntry = MakeShared<FJsonObject>();
	TArray<TSharedPtr<FJsonValue>> NumberEntries;
	NumberEntries.Add(MakeShared<FJsonValueNumber>(1.0));
	NumberFilterEntry->SetArrayField(TEXT("filter"), NumberEntries);
	ExpectInvalid(TEXT("filter entry that is not a string"), NumberFilterEntry);

	TSharedPtr<FJsonObject> BlankFilterEntry = MakeShared<FJsonObject>();
	TArray<TSharedPtr<FJsonValue>> BlankEntries;
	BlankEntries.Add(MakeShared<FJsonValueString>(TEXT("  ")));
	BlankFilterEntry->SetArrayField(TEXT("filter"), BlankEntries);
	ExpectInvalid(TEXT("filter entry that is blank"), BlankFilterEntry);

	TestEqual(TEXT("no refusal invoked the callback"), CallbackCount, 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexEditorStatsCaptureReturnsFramesTest,
	"Cortex.Editor.Stats.CaptureReturnsFrames",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexEditorStatsCaptureReturnsFramesTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	const TSharedRef<FCortexEditorCommandHandler> Handler = MakeShared<FCortexEditorCommandHandler>();
	const TSharedRef<FStatsCaptureProbe> Probe = MakeShared<FStatsCaptureProbe>();

	const FCortexCommandResult Started = StartStatsCapture(*Handler, MakeStatsCaptureParams(5, 10.0), Probe);
	if (!TestTrue(TEXT("capture is deferred"), Started.bIsDeferred))
	{
		AddInfo(FString::Printf(TEXT("start error %s: %s"), *Started.ErrorCode, *Started.ErrorMessage));
		return false;
	}
	TestTrue(TEXT("the transport outlives the capture deadline"), Started.DeferredTimeoutSeconds > 10.0);
	TestTrue(TEXT("the capture is active while pending"), FCortexEditorStatsOps::IsCaptureActive());

	ADD_LATENT_AUTOMATION_COMMAND(FFunctionLatentCommand([this, Handler, Probe]()
	{
		if (!Probe->Result.IsSet() && !Probe->WaitExpired())
		{
			return false;
		}
		if (!TestTrue(TEXT("capture answered"), Probe->Result.IsSet()))
		{
			return true;
		}
		TestEqual(TEXT("answered exactly once"), Probe->CallbackCount, 1);
		TestFalse(TEXT("the capture is released after the answer"), FCortexEditorStatsOps::IsCaptureActive());

		const TArray<TSharedPtr<FJsonValue>>* Nodes = ExpectStatsSuccessShape(*this, Probe->Result.GetValue(), 5);
		if (Nodes != nullptr && TestTrue(TEXT("nodes are not empty without a filter"), Nodes->Num() > 0))
		{
			for (const TSharedPtr<FJsonValue>& Node : *Nodes)
			{
				ExpectStatsNodeShape(*this, Node, 0);
			}
		}
		return true;
	}));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexEditorStatsCaptureFilterReturnsMatchingRootsTest,
	"Cortex.Editor.Stats.CaptureFilterReturnsMatchingRoots",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexEditorStatsCaptureFilterReturnsMatchingRootsTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	// STAT_FrameTime (STATGROUP_Engine, description "FrameTime") is scoped around the engine loop on every tick.
	const FString Filter = TEXT("FrameTime");
	const TSharedRef<FCortexEditorCommandHandler> Handler = MakeShared<FCortexEditorCommandHandler>();
	const TSharedRef<FStatsCaptureProbe> Probe = MakeShared<FStatsCaptureProbe>();

	const FCortexCommandResult Started = StartStatsCapture(*Handler, MakeStatsCaptureParams(5, 10.0, { Filter }), Probe);
	if (!TestTrue(TEXT("capture is deferred"), Started.bIsDeferred))
	{
		AddInfo(FString::Printf(TEXT("start error %s: %s"), *Started.ErrorCode, *Started.ErrorMessage));
		return false;
	}

	ADD_LATENT_AUTOMATION_COMMAND(FFunctionLatentCommand([this, Handler, Probe, Filter]()
	{
		if (!Probe->Result.IsSet() && !Probe->WaitExpired())
		{
			return false;
		}
		if (!TestTrue(TEXT("capture answered"), Probe->Result.IsSet()))
		{
			return true;
		}
		const TArray<TSharedPtr<FJsonValue>>* Nodes = ExpectStatsSuccessShape(*this, Probe->Result.GetValue(), 5);
		if (Nodes != nullptr && TestTrue(TEXT("the filter found matching roots"), Nodes->Num() > 0))
		{
			for (const TSharedPtr<FJsonValue>& Node : *Nodes)
			{
				TestTrue(TEXT("every root name or description contains the filter"), StatsNodeMatchesText(Node, Filter));
				ExpectStatsNodeShape(*this, Node, 0);
			}
		}
		return true;
	}));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexEditorStatsCaptureRejectsConcurrentTest,
	"Cortex.Editor.Stats.CaptureRejectsConcurrent",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexEditorStatsCaptureRejectsConcurrentTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	const TSharedRef<FCortexEditorCommandHandler> Handler = MakeShared<FCortexEditorCommandHandler>();
	const TSharedRef<FStatsCaptureProbe> First = MakeShared<FStatsCaptureProbe>();
	const TSharedRef<FStatsCaptureProbe> Second = MakeShared<FStatsCaptureProbe>();

	const FCortexCommandResult FirstStarted = StartStatsCapture(*Handler, MakeStatsCaptureParams(5, 10.0), First);
	if (!TestTrue(TEXT("the first capture is deferred"), FirstStarted.bIsDeferred))
	{
		AddInfo(FString::Printf(TEXT("start error %s: %s"), *FirstStarted.ErrorCode, *FirstStarted.ErrorMessage));
		return false;
	}

	// A second capture, through another handler as well: the capture is process-wide.
	FCortexEditorCommandHandler OtherHandler;
	const FCortexCommandResult SecondStarted = StartStatsCapture(OtherHandler, MakeStatsCaptureParams(5, 10.0), Second);
	TestFalse(TEXT("the second capture is refused"), SecondStarted.bSuccess);
	TestFalse(TEXT("the refusal is not deferred"), SecondStarted.bIsDeferred);
	TestEqual(TEXT("refusal code"), SecondStarted.ErrorCode, CortexErrorCodes::InvalidOperation);
	TestEqual(TEXT("the refused capture never answers through its callback"), Second->CallbackCount, 0);
	TestTrue(TEXT("the first capture is still running"), FCortexEditorStatsOps::IsCaptureActive());

	ADD_LATENT_AUTOMATION_COMMAND(FFunctionLatentCommand([this, Handler, First, Second]()
	{
		if (!First->Result.IsSet() && !First->WaitExpired())
		{
			return false;
		}
		if (!TestTrue(TEXT("the first capture answered"), First->Result.IsSet()))
		{
			return true;
		}
		TestEqual(TEXT("the first capture answered exactly once"), First->CallbackCount, 1);
		TestTrue(TEXT("the first capture succeeded"), First->Result.GetValue().bSuccess);
		TestEqual(TEXT("the refused capture still never answered"), Second->CallbackCount, 0);
		TestFalse(TEXT("the capture is released after the answer"), FCortexEditorStatsOps::IsCaptureActive());
		return true;
	}));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexEditorStatsCaptureTimeoutReleasesTest,
	"Cortex.Editor.Stats.CaptureTimeoutReleases",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexEditorStatsCaptureTimeoutReleasesTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	const TSharedRef<FCortexEditorCommandHandler> Handler = MakeShared<FCortexEditorCommandHandler>();
	const TSharedRef<FStatsCaptureProbe> TimedOut = MakeShared<FStatsCaptureProbe>();
	const TSharedRef<FStatsCaptureProbe> Next = MakeShared<FStatsCaptureProbe>();

	// 600 frames cannot arrive within one second at editor frame rates, so the deadline decides.
	const FCortexCommandResult Started = StartStatsCapture(*Handler, MakeStatsCaptureParams(600, 1.0), TimedOut);
	if (!TestTrue(TEXT("the capture is deferred"), Started.bIsDeferred))
	{
		AddInfo(FString::Printf(TEXT("start error %s: %s"), *Started.ErrorCode, *Started.ErrorMessage));
		return false;
	}

	// Stage 0: the deadline answer. Stage 1: settle, check it answered once, start the next capture.
	// Stage 2: the next capture succeeds.
	const TSharedRef<int32> Stage = MakeShared<int32>(0);
	const TSharedRef<double> SettleStart = MakeShared<double>(0.0);
	ADD_LATENT_AUTOMATION_COMMAND(FFunctionLatentCommand([this, Handler, TimedOut, Next, Stage, SettleStart]()
	{
		constexpr double SettleSeconds = 0.5;
		if (*Stage == 0)
		{
			if (!TimedOut->Result.IsSet() && !TimedOut->WaitExpired())
			{
				return false;
			}
			if (TestTrue(TEXT("the capture answered"), TimedOut->Result.IsSet()))
			{
				const FCortexCommandResult& Result = TimedOut->Result.GetValue();
				if (Result.bSuccess)
				{
					AddInfo(TEXT("The editor produced 600 stats frames within one second, so the deadline could not be observed."));
				}
				else
				{
					TestEqual(TEXT("the deadline answers TIMEOUT"), Result.ErrorCode, CortexErrorCodes::Timeout);
					if (TestTrue(TEXT("the timeout reports its progress"), Result.ErrorDetails.IsValid()))
					{
						double Requested = 0.0;
						TestTrue(TEXT("frames_requested is reported"), Result.ErrorDetails->TryGetNumberField(TEXT("frames_requested"), Requested));
						TestEqual(TEXT("frames_requested"), static_cast<int32>(Requested), 600);
						double Captured = 0.0;
						TestTrue(TEXT("frames_captured is reported"), Result.ErrorDetails->TryGetNumberField(TEXT("frames_captured"), Captured));
						TestTrue(TEXT("fewer frames than requested were captured"), Captured < 600.0);
					}
				}
			}
			TestFalse(TEXT("the capture is released at the deadline"), FCortexEditorStatsOps::IsCaptureActive());
			*SettleStart = FPlatformTime::Seconds();
			*Stage = 1;
			return false;
		}
		if (*Stage == 1)
		{
			if ((FPlatformTime::Seconds() - *SettleStart) < SettleSeconds)
			{
				return false;
			}
			TestEqual(TEXT("the deadline answered exactly once"), TimedOut->CallbackCount, 1);
			const FCortexCommandResult NextStarted = StartStatsCapture(*Handler, MakeStatsCaptureParams(5, 10.0), Next);
			if (!TestTrue(TEXT("a capture can start after the timeout"), NextStarted.bIsDeferred))
			{
				AddInfo(FString::Printf(TEXT("start error %s: %s"), *NextStarted.ErrorCode, *NextStarted.ErrorMessage));
				return true;
			}
			*Stage = 2;
			return false;
		}
		if (!Next->Result.IsSet() && !Next->WaitExpired())
		{
			return false;
		}
		if (TestTrue(TEXT("the next capture answered"), Next->Result.IsSet()))
		{
			TestEqual(TEXT("the next capture answered exactly once"), Next->CallbackCount, 1);
			ExpectStatsSuccessShape(*this, Next->Result.GetValue(), 5);
		}
		TestEqual(TEXT("the first capture never answered again"), TimedOut->CallbackCount, 1);
		TestFalse(TEXT("the capture is released after the next answer"), FCortexEditorStatsOps::IsCaptureActive());
		return true;
	}));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexEditorStatsCaptureFindsKnownFunctionTest,
	"Cortex.Editor.Stats.CaptureFindsKnownFunction",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexEditorStatsCaptureFindsKnownFunctionTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	UCortexEditorStatsProbe* ProbeObject = NewObject<UCortexEditorStatsProbe>(GetTransientPackage());
	if (!TestNotNull(TEXT("probe object created"), ProbeObject))
	{
		return false;
	}
	ProbeObject->AddToRoot();
	UFunction* ProbeFunction = ProbeObject->FindFunction(TEXT("CortexStatsProbeTick"));
	if (!TestNotNull(TEXT("probe function reflected"), ProbeFunction))
	{
		ProbeObject->RemoveFromRoot();
		ProbeObject->MarkAsGarbage();
		return false;
	}

	// ProcessEvent scopes every call with FScopeCycleCounterUObject(Function), a dynamic STATGROUP_UObjects stat
	// whose name carries the function name; that group is enabled by default, so no stat setting is changed here.
	const FString FunctionName = TEXT("CortexStatsProbeTick");
	const TSharedRef<FCortexEditorCommandHandler> Handler = MakeShared<FCortexEditorCommandHandler>();
	const TSharedRef<FStatsCaptureProbe> Probe = MakeShared<FStatsCaptureProbe>();

	const FCortexCommandResult Started = StartStatsCapture(*Handler, MakeStatsCaptureParams(20, 12.0, { FunctionName }), Probe);
	if (!TestTrue(TEXT("capture is deferred"), Started.bIsDeferred))
	{
		AddInfo(FString::Printf(TEXT("start error %s: %s"), *Started.ErrorCode, *Started.ErrorMessage));
		ProbeObject->RemoveFromRoot();
		ProbeObject->MarkAsGarbage();
		return false;
	}

	TWeakObjectPtr<UCortexEditorStatsProbe> WeakProbe(ProbeObject);
	ADD_LATENT_AUTOMATION_COMMAND(FFunctionLatentCommand([this, Handler, Probe, WeakProbe, FunctionName]()
	{
		UCortexEditorStatsProbe* Object = WeakProbe.Get();
		if (!Probe->Result.IsSet() && !Probe->WaitExpired())
		{
			// One call per engine tick while the capture is collecting frames.
			if (Object != nullptr)
			{
				Object->ProcessEvent(Object->FindFunction(TEXT("CortexStatsProbeTick")), nullptr);
			}
			return false;
		}

		if (TestTrue(TEXT("capture answered"), Probe->Result.IsSet()))
		{
			const TArray<TSharedPtr<FJsonValue>>* Nodes = ExpectStatsSuccessShape(*this, Probe->Result.GetValue(), 20);
			if (Nodes != nullptr && TestTrue(TEXT("the known function was found"), Nodes->Num() > 0))
			{
				for (const TSharedPtr<FJsonValue>& Node : *Nodes)
				{
					TestTrue(TEXT("every root is the known function"), StatsNodeMatchesText(Node, FunctionName));
					const TSharedPtr<FJsonObject>* NodeObject = nullptr;
					double CallsPerFrame = 0.0;
					if (Node.IsValid() && Node->TryGetObject(NodeObject)
						&& TestTrue(TEXT("root has calls_per_frame"), (*NodeObject)->TryGetNumberField(TEXT("calls_per_frame"), CallsPerFrame)))
					{
						TestTrue(TEXT("the function was called in the captured frames"), CallsPerFrame > 0.0);
					}
				}
			}
		}
		if (Object != nullptr)
		{
			TestTrue(TEXT("the probe function ran"), Object->CallCount > 0);
			Object->RemoveFromRoot();
			Object->MarkAsGarbage();
		}
		return true;
	}));
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
