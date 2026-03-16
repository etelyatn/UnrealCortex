# Delegates & Event Dispatchers

## Blueprint Event Dispatcher → C++ Dynamic Multicast Delegate

Declaration (header):
```cpp
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FOnHealthChanged, float, NewHealth);

UPROPERTY(BlueprintAssignable, Category = "Events")
FOnHealthChanged OnHealthChanged;
```

## Binding (BeginPlay or later, never Constructor):
```cpp
// Bind to own delegate:
OnHealthChanged.AddDynamic(this, &AMyActor::HandleHealthChanged);

// Bind to component delegate:
TriggerComp->OnComponentBeginOverlap.AddDynamic(this, &AMyActor::OnOverlap);
```

## UFUNCTION Requirement
All delegate-bound functions MUST be UFUNCTION():
```cpp
UFUNCTION()
void HandleHealthChanged(float NewHealth);

UFUNCTION()
void OnOverlap(UPrimitiveComponent* OverlappedComponent, AActor* OtherActor,
    UPrimitiveComponent* OtherComp, int32 OtherBodyIndex,
    bool bFromSweep, const FHitResult& SweepResult);
```

## Blueprint K2Node_CreateDelegate Pattern
When JSON contains `K2Node_CreateDelegate`, the BP is binding a delegate at runtime.
Translate to `AddDynamic` / `BindDynamic` calls.
