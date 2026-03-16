# Latent Node Patterns

## Delay → FTimerHandle
```cpp
// Header:
FTimerHandle DelayHandle;

// Implementation:
GetWorldTimerManager().SetTimer(DelayHandle, this, &AMyActor::OnDelayComplete, 2.0f, false);
// or with lambda:
GetWorldTimerManager().SetTimer(DelayHandle, [this]() { /* code */ }, 2.0f, false);
```

## Timeline → UTimelineComponent
```cpp
// Header:
UPROPERTY()
TObjectPtr<UTimelineComponent> MyTimeline;

UPROPERTY()
TObjectPtr<UCurveFloat> MyCurve;

FOnTimelineFloat OnTimelineUpdate;
FOnTimelineEvent OnTimelineFinished;

UFUNCTION()
void HandleTimelineUpdate(float Value);

UFUNCTION()
void HandleTimelineFinished();

// Constructor:
MyTimeline = CreateDefaultSubobject<UTimelineComponent>(TEXT("MyTimeline"));

// BeginPlay:
if (MyCurve)
{
    OnTimelineUpdate.BindDynamic(this, &AMyActor::HandleTimelineUpdate);
    MyTimeline->AddInterpFloat(MyCurve, OnTimelineUpdate);
    OnTimelineFinished.BindDynamic(this, &AMyActor::HandleTimelineFinished);
    MyTimeline->SetTimelineFinishedFunc(OnTimelineFinished);
}
MyTimeline->PlayFromStart();
```

## Async Load → FStreamableManager
```cpp
FStreamableManager& Loader = UAssetManager::GetStreamableManager();
Loader.RequestAsyncLoad(SoftPath, FStreamableDelegate::CreateUObject(
    this, &AMyActor::OnAssetLoaded));
```
