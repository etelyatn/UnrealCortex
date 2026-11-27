#include "Operations/CortexEditorStatsOps.h"

#include "Containers/Ticker.h"
#include "CortexCommandRouter.h"
#include "CortexEditorModule.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "HAL/PlatformTime.h"
#include "Math/UnrealMathUtility.h"
#include "Misc/Optional.h"
#include "Misc/ScopeLock.h"
#include "Templates/SharedPointer.h"

#if STATS
#include "Stats/Stats.h"
#include "Stats/StatsData.h"
#include "Tasks/Pipe.h"
#include "Tasks/Task.h"
#include <atomic>

// The pipe every engine stats command runs on (StatsCommand.cpp declares it the same way). It is exported
// from Core but has no public header.
extern CORE_API UE::Tasks::FPipe GStatsPipe;
#endif

namespace CortexEditorStatsPrivate
{
	constexpr int32 MinFrames = 1;
	constexpr int32 MaxFrames = 600;
	constexpr int32 DefaultFrames = 60;
	constexpr int32 MinMaxDepth = 1;
	constexpr int32 MaxMaxDepth = 64;
	constexpr int32 DefaultMaxDepth = 6;
	constexpr double MinTimeoutSeconds = 1.0;
	constexpr double MaxTimeoutSeconds = 300.0;
	constexpr double DefaultTimeoutSeconds = 30.0;
	constexpr int32 MaxFilterEntries = 32;
	/** Margin added to the capture deadline so the transport outlives it and can deliver the TIMEOUT answer. */
	[[maybe_unused]] constexpr double TransportGraceSeconds = 5.0;
	/** Hard cap on emitted nodes; hitting it sets truncated. */
	[[maybe_unused]] constexpr int32 MaxResultNodes = 5000;

	struct FCaptureParams
	{
		int32 Frames = DefaultFrames;
		TArray<FString> Filters;
		int32 MaxDepth = DefaultMaxDepth;
		double MinMs = 0.0;
		double TimeoutSeconds = DefaultTimeoutSeconds;
	};

	/** Plain copied result node: no stats-system types, safe to hand to the game thread. */
	struct FNodeResult
	{
		FString Name;
		FString Description;
		double InclusiveMs = 0.0;
		double ExclusiveMs = 0.0;
		double CallsPerFrame = 0.0;
		TArray<FNodeResult> Children;
	};

	struct FCaptureResult
	{
		int32 FrameCount = 0;
		int64 FirstFrame = 0;
		int64 LastFrame = 0;
		double FrameAvgMs = 0.0;
		double FrameMaxMs = 0.0;
		TArray<FNodeResult> Nodes;
		bool bTruncated = false;
		/** Non-empty: the capture was refused on the stats pipe (raw-stats mode) and completes with INVALID_OPERATION. */
		FString Refusal;
	};

	FCortexCommandResult MakeInvalidField(const FString& Message)
	{
		return FCortexCommandRouter::Error(CortexErrorCodes::InvalidField, Message);
	}

	/** Absent or JSON-null keeps the default. Anything else must be a finite number in range. */
	bool TryReadNumber(
		const TSharedPtr<FJsonObject>& Params,
		const TCHAR* Field,
		const FString& Expectation,
		double Min,
		double Max,
		bool bIntegerOnly,
		double& InOutValue,
		FCortexCommandResult& OutError)
	{
		const TSharedPtr<FJsonValue>* Value = Params->Values.Find(Field);
		if (Value == nullptr || !Value->IsValid() || (*Value)->IsNull())
		{
			return true;
		}
		if ((*Value)->Type != EJson::Number)
		{
			OutError = MakeInvalidField(FString::Printf(TEXT("%s must be %s"), Field, *Expectation));
			return false;
		}
		const double Number = (*Value)->AsNumber();
		if (!FMath::IsFinite(Number)
			|| Number < Min
			|| Number > Max
			|| (bIntegerOnly && Number != FMath::FloorToDouble(Number)))
		{
			OutError = MakeInvalidField(FString::Printf(TEXT("%s must be %s"), Field, *Expectation));
			return false;
		}
		InOutValue = Number;
		return true;
	}

	bool ParseParams(const TSharedPtr<FJsonObject>& Params, FCaptureParams& Out, FCortexCommandResult& OutError)
	{
		if (!Params.IsValid())
		{
			return true;
		}

		double Frames = Out.Frames;
		if (!TryReadNumber(Params, TEXT("frames"),
				FString::Printf(TEXT("an integer from %d to %d"), MinFrames, MaxFrames),
				MinFrames, MaxFrames, true, Frames, OutError))
		{
			return false;
		}
		Out.Frames = static_cast<int32>(Frames);

		double MaxDepth = Out.MaxDepth;
		if (!TryReadNumber(Params, TEXT("max_depth"),
				FString::Printf(TEXT("an integer from %d to %d"), MinMaxDepth, MaxMaxDepth),
				MinMaxDepth, MaxMaxDepth, true, MaxDepth, OutError))
		{
			return false;
		}
		Out.MaxDepth = static_cast<int32>(MaxDepth);

		if (!TryReadNumber(Params, TEXT("min_ms"), TEXT("a number >= 0"),
				0.0, MAX_flt, false, Out.MinMs, OutError))
		{
			return false;
		}

		if (!TryReadNumber(Params, TEXT("timeout_seconds"),
				FString::Printf(TEXT("a number from %g to %g"), MinTimeoutSeconds, MaxTimeoutSeconds),
				MinTimeoutSeconds, MaxTimeoutSeconds, false, Out.TimeoutSeconds, OutError))
		{
			return false;
		}

		const TSharedPtr<FJsonValue>* FilterValue = Params->Values.Find(TEXT("filter"));
		if (FilterValue != nullptr && FilterValue->IsValid() && !(*FilterValue)->IsNull())
		{
			if ((*FilterValue)->Type != EJson::Array)
			{
				OutError = MakeInvalidField(TEXT("filter must be an array of strings"));
				return false;
			}
			const TArray<TSharedPtr<FJsonValue>>& Entries = (*FilterValue)->AsArray();
			if (Entries.Num() > MaxFilterEntries)
			{
				OutError = MakeInvalidField(FString::Printf(TEXT("filter accepts at most %d entries"), MaxFilterEntries));
				return false;
			}
			for (const TSharedPtr<FJsonValue>& Entry : Entries)
			{
				FString Text;
				if (!Entry.IsValid() || Entry->Type != EJson::String || (Text = Entry->AsString().TrimStartAndEnd()).IsEmpty())
				{
					OutError = MakeInvalidField(TEXT("filter must be an array of non-empty strings"));
					return false;
				}
				Out.Filters.Add(MoveTemp(Text));
			}
		}
		return true;
	}

	double Round4(double Value)
	{
		return FMath::RoundToDouble(Value * 10000.0) / 10000.0;
	}

	TSharedPtr<FJsonObject> NodeToJson(const FNodeResult& Node)
	{
		TSharedPtr<FJsonObject> Json = MakeShared<FJsonObject>();
		Json->SetStringField(TEXT("name"), Node.Name);
		Json->SetStringField(TEXT("description"), Node.Description);
		Json->SetNumberField(TEXT("inclusive_ms"), Round4(Node.InclusiveMs));
		Json->SetNumberField(TEXT("exclusive_ms"), Round4(Node.ExclusiveMs));
		Json->SetNumberField(TEXT("calls_per_frame"), Round4(Node.CallsPerFrame));
		TArray<TSharedPtr<FJsonValue>> Children;
		Children.Reserve(Node.Children.Num());
		for (const FNodeResult& Child : Node.Children)
		{
			Children.Add(MakeShared<FJsonValueObject>(NodeToJson(Child)));
		}
		Json->SetArrayField(TEXT("children"), Children);
		return Json;
	}

	FCortexCommandResult ResultToCommandResult(const FCaptureResult& Result)
	{
		if (!Result.Refusal.IsEmpty())
		{
			return FCortexCommandRouter::Error(CortexErrorCodes::InvalidOperation, Result.Refusal);
		}
		TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
		Data->SetNumberField(TEXT("frame_count"), Result.FrameCount);
		Data->SetNumberField(TEXT("first_frame"), static_cast<double>(Result.FirstFrame));
		Data->SetNumberField(TEXT("last_frame"), static_cast<double>(Result.LastFrame));

		TSharedPtr<FJsonObject> FrameTime = MakeShared<FJsonObject>();
		FrameTime->SetNumberField(TEXT("avg"), Round4(Result.FrameAvgMs));
		FrameTime->SetNumberField(TEXT("max"), Round4(Result.FrameMaxMs));
		Data->SetObjectField(TEXT("frame_time_ms"), FrameTime);

		TArray<TSharedPtr<FJsonValue>> Nodes;
		Nodes.Reserve(Result.Nodes.Num());
		for (const FNodeResult& Node : Result.Nodes)
		{
			Nodes.Add(MakeShared<FJsonValueObject>(NodeToJson(Node)));
		}
		Data->SetArrayField(TEXT("nodes"), Nodes);
		Data->SetBoolField(TEXT("truncated"), Result.bTruncated);
		return FCortexCommandRouter::Success(Data);
	}
}

#if STATS
namespace CortexEditorStatsPrivate
{
	/**
	 * Per-capture aggregate of one call-tree node over all captured frames. Totals stay in int64, so neither the
	 * uint32 packed duration of FRawStatStackNode::MergeAdd (which carries into the call count on overflow for
	 * ~600 frames of a ns-resolution cycle counter) nor the integer rounding of FRawStatStackNode::Divide
	 * (which turns a once-per-capture call into 0) can corrupt the figures.
	 */
	struct FAccumNode
	{
		FName RawName;
		int64 Cycles = 0;
		int64 ExclusiveCycles = 0;
		int64 Calls = 0;
		TMap<FName, TUniquePtr<FAccumNode>> Children;
	};

	void ReadPacked(const FStatMessage& Meta, int64& OutCycles, int64& OutCalls)
	{
		OutCycles = 0;
		OutCalls = 0;
		if (Meta.NameAndInfo.GetField<EStatDataType>() == EStatDataType::ST_int64
			&& Meta.NameAndInfo.GetFlag(EStatMetaFlags::IsPackedCCAndDuration))
		{
			const int64 Packed = Meta.GetValue_int64();
			OutCycles = FromPackedCallCountDuration_Duration(Packed);
			OutCalls = FromPackedCallCountDuration_CallCount(Packed);
		}
	}

	/** Adds one frame's tree (Src) into the aggregate (Dest). Stats pipe only. */
	void AccumulateNode(FAccumNode& Dest, const FRawStatStackNode& Src)
	{
		int64 Cycles = 0;
		int64 Calls = 0;
		ReadPacked(Src.Meta, Cycles, Calls);
		Dest.Cycles += Cycles;
		Dest.Calls += Calls;

		int64 ChildCycles = 0;
		for (const TPair<FName, FRawStatStackNode*>& Pair : Src.Children)
		{
			const FRawStatStackNode& Child = *Pair.Value;
			int64 ThisChildCycles = 0;
			int64 ThisChildCalls = 0;
			ReadPacked(Child.Meta, ThisChildCycles, ThisChildCalls);
			ChildCycles += ThisChildCycles;

			TUniquePtr<FAccumNode>& DestChild = Dest.Children.FindOrAdd(Pair.Key);
			if (!DestChild.IsValid())
			{
				DestChild = MakeUnique<FAccumNode>();
				DestChild->RawName = Child.Meta.NameAndInfo.GetRawName();
			}
			AccumulateNode(*DestChild, Child);
		}
		Dest.ExclusiveCycles += FMath::Max<int64>(0, Cycles - ChildCycles);
	}

	/** Turns the aggregate into the filtered, depth-limited, plain result tree. Stats pipe only (reads stat name metadata). */
	class FNodeBuilder
	{
	public:
		FNodeBuilder(const FCaptureParams& InParams, int32 InFrameCount)
			: Params(InParams)
			, FrameCount(static_cast<double>(FMath::Max(InFrameCount, 1)))
		{
		}

		bool WasTruncated() const
		{
			return bTruncated;
		}

		void BuildRoots(const FAccumNode& Root, TArray<FNodeResult>& Out)
		{
			if (Params.Filters.Num() == 0)
			{
				for (const FAccumNode* Child : SortedChildren(Root))
				{
					FNodeResult Result;
					if (Build(*Child, 1, Result))
					{
						Out.Add(MoveTemp(Result));
					}
				}
				return;
			}
			CollectMatches(Root, Out);
		}

	private:
		struct FLabel
		{
			FString Name;
			FString Description;
		};

		const FCaptureParams& Params;
		const double FrameCount;
		TMap<FName, FLabel> Labels;
		int32 Emitted = 0;
		bool bTruncated = false;

		FLabel GetLabel(FName RawName)
		{
			if (const FLabel* Found = Labels.Find(RawName))
			{
				return *Found;
			}
			FLabel Label;
			Label.Name = FStatNameAndInfo::GetShortNameFrom(RawName).ToString();
			Label.Description = FStatNameAndInfo::GetDescriptionFrom(RawName);
			Label.Description.TrimStartInline();
			Labels.Add(RawName, Label);
			return Label;
		}

		double ToMs(int64 Cycles) const
		{
			return FPlatformTime::ToMilliseconds64(static_cast<uint64>(FMath::Max<int64>(Cycles, 0))) / FrameCount;
		}

		bool Matches(const FLabel& Label) const
		{
			for (const FString& Filter : Params.Filters)
			{
				if (Label.Name.Contains(Filter, ESearchCase::IgnoreCase) || Label.Description.Contains(Filter, ESearchCase::IgnoreCase))
				{
					return true;
				}
			}
			return false;
		}

		static TArray<const FAccumNode*> SortedChildren(const FAccumNode& Node)
		{
			TArray<const FAccumNode*> Sorted;
			Sorted.Reserve(Node.Children.Num());
			for (const TPair<FName, TUniquePtr<FAccumNode>>& Pair : Node.Children)
			{
				Sorted.Add(Pair.Value.Get());
			}
			Sorted.Sort([](const FAccumNode& A, const FAccumNode& B)
			{
				return A.Cycles != B.Cycles ? A.Cycles > B.Cycles : A.RawName.Compare(B.RawName) < 0;
			});
			return Sorted;
		}

		/** Depth 1 is a root. Returns false when the node is dropped (below min_ms or past the node cap). */
		bool Build(const FAccumNode& Node, int32 Depth, FNodeResult& Out)
		{
			const double InclusiveMs = ToMs(Node.Cycles);
			if (InclusiveMs < Params.MinMs)
			{
				return false;
			}
			if (Emitted >= MaxResultNodes)
			{
				bTruncated = true;
				return false;
			}
			++Emitted;

			const FLabel Label = GetLabel(Node.RawName);
			Out.Name = Label.Name;
			Out.Description = Label.Description;
			Out.InclusiveMs = InclusiveMs;
			Out.ExclusiveMs = ToMs(Node.ExclusiveCycles);
			Out.CallsPerFrame = static_cast<double>(Node.Calls) / FrameCount;

			if (Depth >= Params.MaxDepth)
			{
				if (Node.Children.Num() > 0)
				{
					bTruncated = true;
				}
				return true;
			}
			for (const FAccumNode* Child : SortedChildren(Node))
			{
				FNodeResult ChildResult;
				if (Build(*Child, Depth + 1, ChildResult))
				{
					Out.Children.Add(MoveTemp(ChildResult));
				}
			}
			return true;
		}

		/** Matching nodes become roots; a matching node's subtree is not searched again. */
		void CollectMatches(const FAccumNode& Node, TArray<FNodeResult>& Out)
		{
			for (const FAccumNode* Child : SortedChildren(Node))
			{
				if (Matches(GetLabel(Child->RawName)))
				{
					FNodeResult Result;
					if (Build(*Child, 1, Result))
					{
						Out.Add(MoveTemp(Result));
					}
				}
				else
				{
					CollectMatches(*Child, Out);
				}
			}
		}
	};

	class FCapture;

	/** Game-thread only: the capture whose deferred callback has not completed yet. */
	TSharedPtr<FCapture, ESPMode::ThreadSafe> GActiveCapture;
	/** Game-thread only: set once a capture touched the stats pipe, so shutdown only drains when needed. */
	bool bPipeEverUsed = false;
	/** Game-thread only: set first thing in Shutdown, so no completion callback can admit a new capture. */
	bool bShuttingDown = false;

	/**
	 * Thread ownership:
	 *  - Game thread: Callback, bCompleted, TickerHandle, GActiveCapture.
	 *  - Immutable once the Begin task is launched (read on the pipe too): Params, DeadlineSeconds.
	 *  - Stats pipe (serialised, never concurrent with itself): Root, NumFrames, FirstFrame, LastFrame,
	 *    FrameTime*, Handle, bSubscribed and every call into FStatsThreadState / FThreadStats.
	 *  - Shared: Mailbox (guarded by MailboxLock), FramesCaptured and State (atomics). State is the single
	 *    arbiter of the deadline: Capturing -> NthFrameClaimed (pipe, last frame arrived by the deadline) or
	 *    Capturing -> TimedOut (game-thread ticker, deadline passed first). Exactly one transition wins.
	 * Lifetime: the game thread (GActiveCapture + ticker) and every queued pipe task hold a strong reference,
	 * so the object outlives the delegate subscription, which the pipe removes before its last task ends.
	 */
	class FCapture : public TSharedFromThis<FCapture, ESPMode::ThreadSafe>
	{
	public:
		FCapture(const FCaptureParams& InParams, FDeferredResponseCallback InCallback)
			: Params(InParams)
			, Callback(MoveTemp(InCallback))
		{
		}

		~FCapture()
		{
			ensureMsgf(!bSubscribed, TEXT("FCortexEditorStatsOps capture destroyed while still subscribed to the stats thread"));
		}

		/** Game thread. */
		void Start()
		{
			check(IsInGameThread());
			const TSharedRef<FCapture, ESPMode::ThreadSafe> Self = AsShared();
			DeadlineSeconds = FPlatformTime::Seconds() + Params.TimeoutSeconds;
			GActiveCapture = Self;
			bPipeEverUsed = true;
			GStatsPipe.Launch(TEXT("CortexCaptureStatsBegin"), [Self]()
			{
				Self->BeginOnStatsPipe();
			});
			TickerHandle = FTSTicker::GetCoreTicker().AddTicker(
				FTickerDelegate::CreateLambda([Self](float DeltaTime)
				{
					(void)DeltaTime;
					return Self->TickOnGameThread();
				}),
				0.0f);
		}

		/** Game thread. Module shutdown: completes the callback and unsubscribes. */
		void AbortForShutdown()
		{
			check(IsInGameThread());
			FTSTicker::RemoveTicker(TickerHandle);
			RequestEndOnStatsPipe();
			Complete(FCortexCommandRouter::Error(
				CortexErrorCodes::EditorNotAvailable,
				TEXT("Stats capture cancelled: the CortexEditor module is shutting down")));
		}

	private:
		const FCaptureParams Params;

		// Game thread.
		FDeferredResponseCallback Callback;
		bool bCompleted = false;
		double DeadlineSeconds = 0.0;
		FTSTicker::FDelegateHandle TickerHandle;

		// Shared.
		FCriticalSection MailboxLock;
		TOptional<FCaptureResult> Mailbox;
		std::atomic<int32> FramesCaptured{ 0 };

		enum ECaptureState : int32
		{
			Capturing,
			NthFrameClaimed,
			TimedOut,
		};
		std::atomic<int32> State{ Capturing };

		// Stats pipe.
		FAccumNode Root;
		FDelegateHandle Handle;
		bool bSubscribed = false;
		int32 NumFrames = 0;
		int64 FirstFrame = 0;
		int64 LastFrame = 0;
		double FrameTimeSumMs = 0.0;
		double FrameTimeMaxMs = 0.0;

		// ---- Stats pipe ----------------------------------------------------------------------------------

		void BeginOnStatsPipe()
		{
			FStatsThreadState& Stats = FStatsThreadState::GetLocalState();
			// Raw-stats mode (stat dumpspam) routes packets to NewRawStatPacket instead of building frames, so
			// NewFrameDelegate would never fire. The engine itself branches on NewRawStatPacket.IsBound()
			// (StatsData.cpp:1063); every engine path that enables raw stats binds it (StatsCommand.cpp:1827-1829,
			// StatsFile.cpp:435), while the mode flag itself (FThreadStats::bIsRawStatsActive) is private.
			// Refuse instead of waiting for a deadline; engine state is left untouched.
			if (Stats.NewRawStatPacket.IsBound())
			{
				FCaptureResult Refused;
				Refused.Refusal = TEXT("Stats raw-stats mode is active (stat dumpspam), which delivers raw packets instead of frames, so a frame capture is impossible. Stop it first (stat dumpspam -stop)");
				FScopeLock Lock(&MailboxLock);
				Mailbox = MoveTemp(Refused);
				return;
			}
			FThreadStats::PrimaryEnableAdd();
			Handle = Stats.NewFrameDelegate.AddRaw(this, &FCapture::OnNewFrame);
			bSubscribed = true;
		}

		/** Idempotent: balanced with BeginOnStatsPipe exactly once. */
		void EndOnStatsPipe()
		{
			if (!bSubscribed)
			{
				return;
			}
			bSubscribed = false;
			FStatsThreadState::GetLocalState().NewFrameDelegate.Remove(Handle);
			Handle.Reset();
			FThreadStats::PrimaryEnableSubtract();
			Root.Children.Empty();
		}

		void OnNewFrame(int64 TargetFrame)
		{
			if (!bSubscribed)
			{
				return;
			}
			// The arrival time is taken before any work on the frame, so building the result cannot make an
			// on-time frame late.
			const double ArrivalSeconds = FPlatformTime::Seconds();
			// Keeps this object alive until the function returns, including after the result reaches the game thread.
			const TSharedRef<FCapture, ESPMode::ThreadSafe> Pin = AsShared();

			if (State.load(std::memory_order_acquire) != Capturing)
			{
				// The ticker already timed the capture out.
				EndOnStatsPipe();
				return;
			}

			const bool bLastFrame = (NumFrames + 1) >= Params.Frames;
			if (bLastFrame)
			{
				// Claim the success only if the last frame arrived by the deadline AND the ticker has not timed out
				// first. Otherwise no result is produced and the ticker completes with TIMEOUT.
				int32 Expected = Capturing;
				if (ArrivalSeconds > DeadlineSeconds
					|| !State.compare_exchange_strong(Expected, NthFrameClaimed, std::memory_order_acq_rel))
				{
					EndOnStatsPipe();
					return;
				}
			}

			FStatsThreadState& Stats = FStatsThreadState::GetLocalState();
			{
				FRawStatStackNode FrameRoot;
				Stats.UncondenseStackStats(TargetFrame, FrameRoot);
				AccumulateNode(Root, FrameRoot);
			}

			// Cycles are 32-bit values widened to int64, so a wrap between the first and last scope reads negative.
			int64 FrameCycles = Stats.GetFastThreadFrameTime(TargetFrame, EThreadType::Game);
			if (FrameCycles < 0)
			{
				FrameCycles += (static_cast<int64>(1) << 32);
			}
			const double FrameMs = FPlatformTime::ToMilliseconds64(static_cast<uint64>(FrameCycles));
			FrameTimeSumMs += FrameMs;
			FrameTimeMaxMs = FMath::Max(FrameTimeMaxMs, FrameMs);

			if (NumFrames == 0)
			{
				FirstFrame = TargetFrame;
			}
			LastFrame = TargetFrame;
			++NumFrames;
			FramesCaptured.store(NumFrames, std::memory_order_relaxed);

			if (bLastFrame)
			{
				FCaptureResult Result = BuildResult();
				EndOnStatsPipe();
				FScopeLock Lock(&MailboxLock);
				Mailbox = MoveTemp(Result);
			}
		}

		FCaptureResult BuildResult()
		{
			FCaptureResult Result;
			Result.FrameCount = NumFrames;
			Result.FirstFrame = FirstFrame;
			Result.LastFrame = LastFrame;
			Result.FrameAvgMs = NumFrames > 0 ? FrameTimeSumMs / NumFrames : 0.0;
			Result.FrameMaxMs = FrameTimeMaxMs;
			FNodeBuilder Builder(Params, NumFrames);
			Builder.BuildRoots(Root, Result.Nodes);
			Result.bTruncated = Builder.WasTruncated();
			return Result;
		}

		// ---- Game thread ---------------------------------------------------------------------------------

		/** Queues the idempotent unsubscribe behind any pending Begin/frame work. */
		void RequestEndOnStatsPipe()
		{
			const TSharedRef<FCapture, ESPMode::ThreadSafe> Self = AsShared();
			GStatsPipe.Launch(TEXT("CortexCaptureStatsEnd"), [Self]()
			{
				Self->EndOnStatsPipe();
			});
		}

		/** Core ticker: returns false once completed. */
		bool TickOnGameThread()
		{
			if (bCompleted)
			{
				return false;
			}

			TOptional<FCaptureResult> Ready;
			{
				FScopeLock Lock(&MailboxLock);
				Ready = MoveTemp(Mailbox);
				Mailbox.Reset();
			}
			if (Ready.IsSet())
			{
				Complete(ResultToCommandResult(Ready.GetValue()));
				return false;
			}

			// The last frame arrived on time and its result is being built: wait for it, never time out.
			if (State.load(std::memory_order_acquire) == NthFrameClaimed)
			{
				return true;
			}
			if (FPlatformTime::Seconds() >= DeadlineSeconds)
			{
				int32 Expected = Capturing;
				if (State.compare_exchange_strong(Expected, TimedOut, std::memory_order_acq_rel))
				{
					CompleteWithTimeout(FramesCaptured.load(std::memory_order_relaxed));
					return false;
				}
				// Lost the claim to an on-time last frame: its result is on the way.
			}
			return true;
		}

		/** Game thread. The ticker won the deadline claim, or the last frame was late: TIMEOUT, capture released. */
		void CompleteWithTimeout(int32 Captured)
		{
			FCortexCommandResult Timeout = FCortexCommandRouter::Error(
				CortexErrorCodes::Timeout,
				FString::Printf(
					TEXT("Stats capture collected %d of %d frames within %.1f seconds. Stats frames stop arriving when the editor stalls."),
					Captured, Params.Frames, Params.TimeoutSeconds));
			Timeout.AddContext(TEXT("frames_captured"), Captured);
			Timeout.AddContext(TEXT("frames_requested"), Params.Frames);
			RequestEndOnStatsPipe();
			Complete(MoveTemp(Timeout));
		}

		/** Exactly-once completion. */
		void Complete(FCortexCommandResult Result)
		{
			check(IsInGameThread());
			if (bCompleted)
			{
				return;
			}
			bCompleted = true;
			const TSharedRef<FCapture, ESPMode::ThreadSafe> Pin = AsShared();
			if (GActiveCapture.Get() == this)
			{
				GActiveCapture.Reset();
			}
			FDeferredResponseCallback Local = MoveTemp(Callback);
			Callback = nullptr;
			if (Local)
			{
				Local(MoveTemp(Result));
			}
		}
	};
}
#endif // STATS

FCortexCommandResult FCortexEditorStatsOps::CaptureStats(const TSharedPtr<FJsonObject>& Params, FDeferredResponseCallback DeferredCallback)
{
	using namespace CortexEditorStatsPrivate;

	FCaptureParams Parsed;
	FCortexCommandResult ParamError;
	if (!ParseParams(Params, Parsed, ParamError))
	{
		return ParamError;
	}

#if STATS
	check(IsInGameThread());
	if (!FThreadStats::WillEverCollectData())
	{
		return FCortexCommandRouter::Error(CortexErrorCodes::InvalidOperation,
			TEXT("Stats collection is disabled for this editor process, so there is nothing to capture"));
	}
	if (!DeferredCallback)
	{
		return FCortexCommandRouter::Error(CortexErrorCodes::InvalidOperation,
			TEXT("editor.capture_stats answers through a deferred callback; this call has none"));
	}
	if (bShuttingDown)
	{
		return FCortexCommandRouter::Error(CortexErrorCodes::InvalidOperation,
			TEXT("The CortexEditor module is shutting down; stats captures are closed"));
	}
	if (GActiveCapture.IsValid())
	{
		return FCortexCommandRouter::Error(CortexErrorCodes::InvalidOperation,
			TEXT("A stats capture is already running; wait for it to finish before starting another"));
	}

	const double TransportTimeoutSeconds = Parsed.TimeoutSeconds + TransportGraceSeconds;
	const TSharedRef<FCapture, ESPMode::ThreadSafe> Capture =
		MakeShared<FCapture, ESPMode::ThreadSafe>(Parsed, MoveTemp(DeferredCallback));
	Capture->Start();

	FCortexCommandResult Deferred;
	Deferred.bIsDeferred = true;
	Deferred.DeferredTimeoutSeconds = TransportTimeoutSeconds;
	return Deferred;
#else
	(void)DeferredCallback;
	return FCortexCommandRouter::Error(CortexErrorCodes::InvalidOperation,
		TEXT("Stats are compiled out of this editor build (STATS=0), so editor.capture_stats is unavailable"));
#endif
}

void FCortexEditorStatsOps::Shutdown()
{
#if STATS
	using namespace CortexEditorStatsPrivate;

	// Close admission before any callback runs, so a completion callback cannot start a new capture.
	bShuttingDown = true;
	if (!bPipeEverUsed)
	{
		return;
	}
	const TSharedPtr<FCapture, ESPMode::ThreadSafe> Active = GActiveCapture;
	if (Active.IsValid())
	{
		Active->AbortForShutdown();
	}
	// The pipe is FIFO: once this barrier ran, every earlier Begin/End task (module code, including the End task
	// queued above) has finished and nothing of this module is left queued on the pipe. No timeout: the engine
	// drains GStatsPipe at shutdown the same way (StatsSystemTypes.cpp:1329), and returning early would leave
	// queued lambdas referencing module code at unload.
	UE::Tasks::FTask Barrier = GStatsPipe.Launch(TEXT("CortexCaptureStatsDrain"), []() {});
	Barrier.Wait();
#endif
}

bool FCortexEditorStatsOps::IsCaptureActive()
{
#if STATS
	return CortexEditorStatsPrivate::GActiveCapture.IsValid();
#else
	return false;
#endif
}
