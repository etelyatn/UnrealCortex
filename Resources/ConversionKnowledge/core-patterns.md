# Core UE C++ Patterns

## Logic vs Cosmetic — What to Convert

The deciding factor is NOT the function name or node count. It is: **does game logic depend on this value?**

### Moves to C++ (logic-driven)
Any operation where the value affects game state or other logic depends on it:
- `SetVisibility(false)` to hide an object during a game state change (dead, inactive) — logic controls it
- `SetLocation` to move an actor based on player input or game rules — logic depends on position
- `SetColor` where the color encodes game state (team color, damage flash) — logic depends on color
- Even a single node, if it's logic-driven, belongs in C++

### Stays in Blueprint (purely cosmetic)
Operations where NO other logic reads or reacts to the value:
- `SetColor` for decorative appearance nothing else checks — artist-owned
- `SetLocation/SetScale3D` to arrange mesh pieces for a door frame or wall — decorative geometry
- `SetMaterial`, `SetStaticMesh`, `SetLightColor` for visual appearance — artist-owned
- `CreateDynamicMaterialInstance` + parameter setters — visual tuning
- Procedural geometry (door frames, wall segments) where no gameplay depends on positions

**Key:** Same function, different intent. `SetVisibility` that hides a game object as part of state → C++. `SetVisibility` for a cosmetic fade nobody reads → Blueprint.

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
