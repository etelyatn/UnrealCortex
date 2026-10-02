# Fixture-dependent automation crash prevention implementation

- `CortexAnimationInspectTest.cpp`: branch on the existing data-validity assertion in Sequence.Info, Montage.Info, Skeleton.Info, and AnimBlueprint.Info; return false before response access.
- `CortexMcpConfigTranslatorTest.cpp`: stop after failed file/JSON/server-object prerequisites and invalid array sizes; require valid nested server/environment objects before access.
- Production command handlers and translator are unchanged.

## Verification

The issue's reported missing-fixture crashes are the red evidence; no intentional editor crash is repeated. Build changed test modules, run existing Sandbox tests, and launch an isolated bare host using the built plugin to exercise actual missing-fixture failure paths. Expected bare-host outcome: recorded failures, normal automation completion, no shared-pointer assertion. Record observed outcomes in `docs/verification/2026-10-02-issue-150.md`.
