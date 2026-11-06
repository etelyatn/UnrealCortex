# Declared command contract enforcement — implementation

**Date:** 2026-10-14
**Design:** `2026-10-14-declared-command-contract-design.md`

## New

| File | What |
|---|---|
| `Source/CortexCore/Public/CortexCommandContract.h` | `ValidateParams(Info, Params, OutError)` and `ValidateDeclaredParams(Infos, Command, Params, OutError)` |
| `Source/CortexCore/Private/CortexCommandContract.cpp` | Unknown-key rejection (skipping `_`-prefixed router keys), required-presence check, actionable `INVALID_FIELD` messages |
| `Source/CortexUMG/Private/Tests/CortexUMGSetterContractTest.cpp` | `Cortex.UMG.SetterContract` |
| `Source/CortexData/Private/Tests/CortexDataSelectorContractTest.cpp` | `Cortex.Data.SelectorContract` |
| `MCP/tests/test_capabilities_contract.py` | Six capability-inventory contract tests |

## Changed

| File | What |
|---|---|
| `Source/CortexCore/Public/ICortexDomainHandler.h` | `FCortexCommandInfo::bStrictParams` + `StrictParams()` (after `bRollbackSafe`, so aggregate initialization of the table is unchanged) |
| `Source/CortexUMG/Private/CortexUMGCommandHandler.cpp` | Declarations hoisted into one `static` table shared by `GetSupportedCommands()` and validation (forward-declared for `Execute`); validation call at the top of `Execute`; `StrictParams()` on `set_text`/`set_font`/`set_padding`/`set_anchor`/`set_alignment`/`set_size`; `set_anchor` gained `min`/`max`/`alignment`/`offset` with `preset` demoted to optional; `set_font` gained `outline_size`/`outline_color`; `set_padding`'s `padding` description now states both accepted forms |
| `Source/CortexUMG/Private/Operations/CortexUMGWidgetPropertyOps.cpp` | Pre-mutation semantic refusals: `SetAnchor` (unknown preset token → `INVALID_PROPERTY_VALUE`; no anchor input → `INVALID_FIELD`), `SetAlignment` (no input; unknown token), `SetSize` (no input; unknown `size_rule`), `SetFont` (no input). `SetAnchor`'s transaction now begins after validation; the preset/min/max/alignment/offset application order is unchanged |
| `Source/CortexData/Private/CortexDataCommandHandler.cpp` | Same table extraction + validation call; `StrictParams()` on `list_data_assets` and `search_assets` |
| `Source/CortexData/Private/Tests/CortexDataAssetCrudTest.cpp` | `Cortex.Data.DataAsset.ListClassFilterRemoved` asserted the old silent-drop contract (success + unfiltered count); it now asserts the refusal |
| `MCP/src/tools/umg/composites.py` | `padding` dict is emitted as the declared nested object instead of flattened top-level edge keys |
| `MCP/tests/fixtures/capabilities_cache_full.json` + `MCP/src/cortex_mcp/_fallback_generated.py` | Capability mirror migrated to the completed declarations and regenerated (`scripts/sync_fallback.py --from-fixture`) |
| `MCP/tests/fixtures/main_menu_batch_request.json` | Recorded batch carried the flat padding shape; now nested |
| `MCP/tests/test_umg_composites.py` | Padding assertion targets the nested object; the recorded-batch comparison now compares a styled command's params, so a shape drift cannot hide behind a count-only check |

## Test-first evidence

Every refusal assertion was watched failing against the unmodified handlers before the fix:
`Cortex.UMG.SetterContract` produced 11 failures (flat `top`; missing `padding`; unknown `anchor`;
unknown preset token; empty anchor/alignment/size/font requests; missing `text`) and
`Cortex.Data.SelectorContract` 5 (retired `class_filter` on both commands; undeclared `path_filter`),
while the "declared form still works" assertions already passed — proving the fixtures were valid and
only the enforcement was missing. The Python padding test failed with `KeyError: 'padding'`.

## Verification

| Target | Result |
|---|---|
| `CortexSandboxEditor Win64 Development` | build exit 0, no new warnings |
| `Cortex.UMG+` | 137 completed, all `Success` (the one test left in flight by the runner is a latent-test artifact: it reproduced with a *different* test before this change and passes 1/1 in isolation) |
| `Cortex.UMG.SetterContract+` | 1/1 |
| `Cortex.Data+` | 131/131 |
| `Cortex.Core+` | 205/205 |
| Python `-m "not e2e and not scenario and not stress"` | 1093 passed (baseline 1087 + 6 new) |
| Live MCP probes | see `docs/verification/2026-10-14-mcp-benchmark-remediation.md` |

**Run identity and retained evidence** (CortexSandbox project-repo paths; commit `ec163181`): candidate
build `Saved/Logs/ubt_green3.txt` (exit 0; the only warnings are the pre-existing `C4996`
`GetObjectsWithPackage` deprecations in CortexData tests). Native runs
`Saved/TestLogs/AutomationTest_2026-10-14_090406.log` (`Cortex.UMG.SetterContract+`),
`…_090317.log` (`Cortex.Data+`), `…_090439.log` (`Cortex.Core+`, "205 tests performed"),
`…_090553.log` (the isolated rename test) and `…_090940.log` (full `Cortex+`, "1877 tests performed").
The declaration totals (497 required / 526 optional) are a source count over
`Source/**/*CommandHandler.cpp`; the Python capability fixture is a partial mirror of the same commit
and is not the source of those totals.

Two independent read-only reviews covered the native contract/declarations and the Python/docs changes;
both returned no production defect. Their two actionable findings were applied: the stale
`main_menu_batch_request.json` padding shape and three documentation claims that overstated what the
implementation guarantees.

## Not done

Enforcing the contract for every routed command (`TD-MCP-005`). It is one call site, but it needs a
full declaration/handler audit and a replay of real callers first.
