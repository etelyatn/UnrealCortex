# Issue 221: TCP server client reaping, request size limit, deferred callback lifetime

Request: https://github.com/etelyatn/UnrealCortex/issues/221, reported by @XpycT. The report reviewed the code at `f72a0ad` (0.4.6).

## Outcome and boundary

The TCP transport releases a client when the client disconnects. It enforces its declared request limit. A deferred callback can no longer reach a stopped or destroyed server, and a deferred timeout is reported as a timeout. `core.batch` does not start or stop PIE for a step it then reports as failed. Batch entries keep the handler's `details` and `warnings`. The wire protocol is unchanged except for three things: a new `LIMIT_EXCEEDED` refusal, the `TIMEOUT` code on deferred timeouts, and the two additive batch entry fields.

## Source basis

At base `b388afb6` (0.4.11), checked against the UE 5.8 engine source:

1. `FSocketBSD::HasPendingData` (`SocketsBSD.cpp:111`) returns `false` when `FIONREAD` reports 0 bytes. After a peer FIN the socket is readable but has 0 bytes, so `ProcessSingleClient` returns before calling `Recv`. `GetConnectionState` reports `SCS_Connected` while `select` says readable or writable. A closed client is therefore never reaped, `ClientDisconnectCallback` never fires, and `FCortexEditorCommandHandler::OnTcpClientDisconnected` never cancels injected input. `FSocketBSD::Recv` returns `false` for a 0-byte read (graceful close) and for every error except `SE_EWOULDBLOCK`, which returns `true` with 0 bytes.
2. The 2 MB check counts bytes per tick, the "truncating" log truncates nothing, and the per-client `FString` buffer has no cap. Each 64 KB chunk is also UTF-8-decoded on its own, so a multi-byte character split across chunks is corrupted.
3. The dispatcher lambda captures a raw `this`. Handlers keep it in tickers, world timers and PIE state, and `Stop()` cancels none of them. `Stop()` also resets `NextDeferredId` to 1, so after a restart a stale callback completes another client's request.
4. `CheckDeferredTimeouts` reports `INVALID_OPERATION`, although `CortexErrorCodes::Timeout` exists.
5. `HandleBatch` runs the step and only then replaces a deferred result with `INVALID_OPERATION`. Every other deferred producer refuses a missing callback before it has any effect: `FCortexDeferredExec::RunNextTick` (`run_python run_next_tick`, `live_coding_compile`), `wait_ready`, `inject_input_continuous` with a duration, `inject_input_sequence`, and `qa.interact`, `qa.move_to` and `qa.wait_for`. Only `FCortexEditorPIEOps::StartPIE`, `StopPIE` and `RestartPIE` request the PIE transition without a callback.
6. Batch entries copy only `error_code`/`error_message` or `data`.

## Selected design

1. **Reaping.** `ProcessSingleClient` reads with `Recv` until it would block. It no longer gates on `HasPendingData`. A `false` from `Recv` is a closed or failed peer. Complete lines already received are still processed, and then the client is retired through the existing `DestroyClientSocket` path, which fires the disconnect callback.
2. **Request limit.** The receive buffer holds raw bytes (`TArray<uint8>`). Each complete line is decoded as a whole, so the UTF-8 chunk-split corruption also goes away. A request whose line exceeds `MaxMessageSize` bytes, or an unterminated buffer that exceeds it, gets a `LIMIT_EXCEEDED` error and the client is closed. Exactly `MaxMessageSize` bytes is accepted. The per-tick read budget stays at `MaxMessageSize` so one client cannot monopolise a frame, and the misleading log line is removed.
3. **Callback lifetime.** `Start()` creates a shared alive token, and `Stop()` resets it. The dispatcher callback captures a weak reference and does nothing once the token is gone, which covers both `Stop()` and destruction (`~FCortexTcpServer` calls `Stop()`). `NextDeferredId` is no longer reset.
4. **Timeout code.** Deferred timeouts use `CortexErrorCodes::Timeout`.
5. **Batch and PIE.** `StartPIE`, `StopPIE` and `RestartPIE` refuse a missing callback with `INVALID_OPERATION` after their state checks and before any PIE request. This is the existing convention of every other deferred producer. The post-execution batch guard stays as the generic contract for any producer.
6. **Batch entries.** A failed entry adds `details` when the handler supplied them. A successful entry adds `warnings` when present. This mirrors `ResultToJson`.

Rejected for 5: a `Deferred()` flag on `FCortexCommandInfo` checked by `HandleBatch`. Several commands defer only conditionally: `run_python` with `run_next_tick`, `inject_input_continuous` with `duration_ms`, and `wait_ready` when the editor is not yet ready. A command-level flag would refuse their synchronous forms inside a batch, which work today.

## Behavior change visible to callers

A request line longer than 2 MB (2,097,152 bytes) used to work and only logged a warning. It is now refused with `LIMIT_EXCEEDED` and the connection is closed. The Python client sees either that error or a dropped connection.

## Acceptance

- A loopback client that connects and then closes is reaped within a few ticks, and the disconnect callback fires once.
- An unterminated request of `MaxMessageSize + 1` bytes, and a newline-terminated request of `MaxMessageSize + 1` bytes, are both refused with `LIMIT_EXCEEDED`, are not dispatched, and the client is retired. A request of exactly `MaxMessageSize` bytes is dispatched.
- A deferred callback issued before `Stop()` sends nothing to a client of the restarted server, and that client's request stays pending.
- A timed-out deferred request returns `TIMEOUT`.
- `start_pie`, `stop_pie` and `restart_pie` without a callback return `INVALID_OPERATION`, are not deferred, and request no PIE transition.
- A batch entry keeps `details.current_fingerprint` of a stale-precondition failure and the `warnings` of a successful step.
- Live editor: a closed MCP connection is reaped, an oversized request is refused, and `core.batch` with `editor.start_pie` does not start PIE.
