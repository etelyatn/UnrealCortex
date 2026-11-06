# PR145: same-session level asset identity

## Problem and acceptance

A blank `level.create_level(open=false)` world is saved but its resident UObject lacks `RF_Public`. Asset operations reject it until reload. Acceptance: both blank and template-created worlds are assets immediately, and same-session `rename_level` moves the map without reopening it.

## Decision

Retain contributor Victor/Boshchuk's PR #145 implementation: set `RF_Public | RF_Standalone` on the blank world before saving. No public command, schema, durable format, module ownership, or save-policy change. The regression covers template creation as a control; its exposed teardown warning requires a local initialized-state guard, retaining root/standalone release without calling cleanup on an uninitialized duplicate.

Adopt the attributed original commit on current main. Harden its test by replacing fixed content paths with the existing GUID-owned Saved-backed writable mount, adding explicit headers and scoped object cleanup, and asserting the renamed map file exists while the original map file is absent. A test-local latent command separates create, rename, cleanup, and release across deterministic engine frames; this preserves event order without sleeps, retries, or polling. No toolkit API update is necessary because no command contract or registration changes.

## Scope

Only blank-world flags, the contributor's consumer-visible regression, its directly exposed template teardown warning root, and evidence documents. No lifecycle refactor, new abstraction, retry, compatibility layer, or parent gitlink/toolkit publication.

Original: https://github.com/etelyatn/UnrealCortex/pull/145
Contributor revision: `a5fb580d28c91bbf610107860c2a6210879ded2a`.
