# Blueprint Flow Node Translations

## FlipFlop → bool toggle
```cpp
// Member:
bool bFlipFlopState = false;

// Usage:
bFlipFlopState = !bFlipFlopState;
if (bFlipFlopState) { /* A */ } else { /* B */ }
```

## DoOnce → bool guard
```cpp
// Member:
bool bHasDoneOnce = false;

// Usage:
if (!bHasDoneOnce)
{
    bHasDoneOnce = true;
    // Execute once
}

// Reset: bHasDoneOnce = false;
```

## Gate → bool flag
```cpp
// Member:
bool bGateOpen = true; // or false, depending on BP default

// Open: bGateOpen = true;
// Close: bGateOpen = false;
// Toggle: bGateOpen = !bGateOpen;
// Enter:
if (bGateOpen) { /* pass through */ }
```

## MultiGate → index counter
```cpp
// Member:
int32 MultiGateIndex = 0;

// Usage (sequential):
switch (MultiGateIndex)
{
case 0: /* Output 0 */ break;
case 1: /* Output 1 */ break;
case 2: /* Output 2 */ break;
}
MultiGateIndex = (MultiGateIndex + 1) % NumOutputs;

// For random: MultiGateIndex = FMath::RandRange(0, NumOutputs - 1);
```

## ForEachLoopWithBreak → ranged for with break
```cpp
for (const auto& Element : Array)
{
    if (ShouldBreak(Element))
    {
        break;
    }
    // Loop body
}
```
