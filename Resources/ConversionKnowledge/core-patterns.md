# Core UE C++ Patterns

## UPROPERTY Specifiers
- `EditAnywhere` — editable in both defaults and instances
- `BlueprintReadWrite` — read+write from Blueprint
- `BlueprintReadOnly` — read from Blueprint, write from C++ only
- `Category = "CategoryName"` — organizes in Details panel
- `meta = (AllowPrivateAccess = "true")` — expose private members to Blueprint

## Component Initialization (Constructor Only)
```cpp
// In constructor:
MeshComp = CreateDefaultSubobject<UStaticMeshComponent>(TEXT("MeshComp"));
RootComponent = MeshComp;

// Attachment:
ChildComp = CreateDefaultSubobject<USceneComponent>(TEXT("ChildComp"));
ChildComp->SetupAttachment(RootComponent);
```

## Lifecycle Order
1. **Constructor** — CreateDefaultSubobject, set defaults. NO gameplay logic.
2. **OnConstruction** — Called in editor and at runtime after transforms applied. For procedural generation.
3. **BeginPlay** — Runtime init. Safe to access other actors, world state.
4. **Tick** — Per-frame. Guard with `PrimaryActorTick.bCanEverTick = true` in constructor.

## Super:: Calls
Always call Super:: for overridden virtuals:
```cpp
void AMyActor::BeginPlay()
{
    Super::BeginPlay();
    // Your code here
}
```
