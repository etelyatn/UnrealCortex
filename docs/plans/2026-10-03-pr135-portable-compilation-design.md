# PR #135: portable compilation corrections

Retain Boshchuk's three bounded behaviors without redesign:

| Behavior | Value / design fit | Route / acceptance |
|---|---|---|
| Windows-only safe-file/export assertions | Keep existing assertions and non-Windows skips; avoid unreachable tails | Submitted code; compile and focused safe-file/export regression runs |
| Dispatcher iterator presence check | Original loop always broke; preserve ExcludeSuper and set-only flag | Submitted code; source equivalence review and analysis runtime smoke |
| Explicit socket headers | Dereferenced USkeletalMeshSocket needs its owning header | Submitted code; compile touched files individually and socket authoring regression run |

PR #144 is an explicitly linked prerequisite: three owning includes for FKismetEditorUtilities, UTimelineTemplate and graph retirement types. Standalone #135 non-unity compilation failed at exactly those three unrelated files, while all five #135 files compiled. Retain #144 separately, preserving its author and PR identity. No #136 code imported into this candidate.

PR #142 owns the existing automation parent/child naming collision: Cortex.Data.Export.PathSafety is not a normal scheduled leaf. Do not claim a suite pass exercised its moved parent assertions or silently fix the unrelated naming problem.

No commands, schemas, dependencies, durable formats or runtime ownership change. Existing module system references and toolkit remain accurate. Installed Linux UE validation is contributor evidence, not local Windows proof.
