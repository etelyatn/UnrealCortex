# Cross-platform temporary fixture suffixes — PR179

## Problem and decision

Contributor @Boshchuk reported deterministic temporary-asset collisions on Linux UE5.8.2. UE5.8 `UnixPlatformGuid.cpp:27-62` confirms `FGuid::NewGuid()` uses UUIDv7: A contains the timestamp shifted right16; B's upper16 bits complete the timestamp. Digits formatting preserves A/B/C/D order. Left8 therefore changes only every65536ms; Left12 identifies the same millisecond rather than a random suffix.

Retain the submitted local correction: Right8 selects D's32 random bits; Right12 selects the lower16 bits of C plus D's32 bits, outside the variant mask. Preserve suffix lengths, hexadecimal format, fixture prefixes, package/mount ownership and existing cleanup. This is probabilistic naming, not a guarantee against every collision.

## Scope and acceptance

117 substitutions in41 existing test files across Animation, Blueprint, Graph, Material and StateTree. No production code, public API/schema, fixture lifecycle redesign, new helper, retries or fallback. No toolkit/system architecture synchronization required.

Acceptance: exact substitutions only; integrated build against merged PR178; complete rendering native suite; native GUID formatting and consumer-visible old-prefix collision versus successful new-suffix asset creation for both lengths; independent candidate review. Local Linux runtime is unavailable; distinguish contributor results from Windows evidence.

[Implementation and verification](2026-10-13-test-guid-suffix-impl.md).
