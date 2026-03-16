# Animation Blueprint Patterns

## AnimInstance Base
```cpp
UCLASS()
class UMyAnimInstance : public UAnimInstance
{
    GENERATED_BODY()

public:
    virtual void NativeInitializeAnimation() override;
    virtual void NativeUpdateAnimation(float DeltaSeconds) override;

    UPROPERTY(BlueprintReadOnly, Category = "Animation")
    float Speed;

    UPROPERTY(BlueprintReadOnly, Category = "Animation")
    bool bIsInAir;
};
```

## State Machine Variables
AnimBP state machines read properties. Expose state as BlueprintReadOnly:
```cpp
void UMyAnimInstance::NativeUpdateAnimation(float DeltaSeconds)
{
    APawn* Owner = TryGetPawnOwner();
    if (!Owner) return;

    Speed = Owner->GetVelocity().Size();
    bIsInAir = Owner->GetMovementComponent()->IsFalling();
}
```

## Anim Notifies
```cpp
// In AnimInstance or AnimNotify subclass:
UFUNCTION()
void AnimNotify_FootStep(); // Name must match notify name in montage

// Or as a separate class:
UCLASS()
class UAnimNotify_CustomEffect : public UAnimNotify
{
    GENERATED_BODY()
    virtual void Notify(USkeletalMeshComponent* MeshComp,
        UAnimSequenceBase* Animation, const FAnimNotifyEventReference& EventReference) override;
};
```
