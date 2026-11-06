# PR #142 automation test discovery

## Scope and decision

Adopt Boshchuk's 22 automation test-name changes from PR #142 (`11776c4fc946d2c5d591bb8e56703c316854c491`) on current main (`df0d2015d2decb8f33f5dea6837735843768791e`). Append the established `.Basic` suffix so existing test bodies become leaves in Unreal's automation report tree. Preserve test bodies, flags, and production behavior except for necessary repairs to obsolete test invocations.

`RemoveGraph.Basic` must exercise invalid targets through preview: apply requires fingerprint and validation-hash preconditions. Do not reorder or weaken the production mutation guard to satisfy old tests. PR #145's blank-world rename prerequisite already shipped via #162.

## Acceptance

- All 22 submitted new names are discoverable and execute in the native editor.
- Newly exposed tests pass without test warnings/errors; correct obsolete test calls rather than suppressing failures.
- Keep contributor attribution and record exercised build/runtime evidence.
- Increment the plugin patch release from 0.3.3 to 0.3.4 and integer version from 15 to 16.

## Boundaries

No command, MCP, toolkit, or durable-data contract changes. Additional current-main prefix collisions (`Cortex.UMG.AddWidget` and `Cortex.Blueprint.Compile`) are outside the submitted 22-name change and must be reported separately, not silently claimed resolved.
