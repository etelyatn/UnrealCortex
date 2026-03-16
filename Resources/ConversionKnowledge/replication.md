# Replication Patterns

## Replicated Properties
```cpp
// Header:
UPROPERTY(Replicated)
float Health;

// Implementation — REQUIRED:
void AMyActor::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
    Super::GetLifetimeReplicatedProps(OutLifetimeProps);
    DOREPLIFETIME(AMyActor, Health);
}
```

## RepNotify
```cpp
// Header:
UPROPERTY(ReplicatedUsing = OnRep_Health)
float Health;

UFUNCTION()
void OnRep_Health();
```

## RPCs
```cpp
UFUNCTION(Server, Reliable)
void ServerRequestAction(FVector Target);
void ServerRequestAction_Implementation(FVector Target);

UFUNCTION(Client, Reliable)
void ClientShowEffect(FVector Location);
void ClientShowEffect_Implementation(FVector Location);

UFUNCTION(NetMulticast, Unreliable)
void MulticastPlaySound(USoundBase* Sound);
void MulticastPlaySound_Implementation(USoundBase* Sound);
```

## Constructor Requirements
```cpp
AMyActor::AMyActor()
{
    bReplicates = true;
    // For components:
    // GetMesh()->SetIsReplicated(true);
}
```
