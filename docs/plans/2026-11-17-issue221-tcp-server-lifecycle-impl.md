# Issue 221 implementation: TCP server lifecycle and batch envelope

Design: [2026-11-17-issue221-tcp-server-lifecycle-design.md](2026-11-17-issue221-tcp-server-lifecycle-design.md).

## Changes

- `Source/CortexCore/Public/CortexTcpServer.h`, `Private/CortexTcpServer.cpp`:
  - `ReceiveBuffers` is now `TMap<FSocket*, TArray<uint8>>`. `ProcessSingleClient` reads with `Recv` until the socket would block, up to `MaxMessageSize` bytes per tick. A `false` from `Recv` marks the peer closed. Complete lines already received are still processed, and then the client is retired by `ProcessClientData` → `DestroyClientSocket`, which fires `ClientDisconnectCallback`.
  - Each line is UTF-8-decoded as a whole. A line longer than `MaxMessageSize` bytes, or an unterminated remainder longer than that, is answered with `LIMIT_EXCEEDED` (no `id`, because the request was never parsed) and the client is retired. The misleading "truncating" warning is gone.
  - `AliveToken` (`TSharedPtr<bool>`) is created when `Start()` binds and reset first in `Stop()`. The deferred callback captures a `TWeakPtr` and is a no-op once the token is gone. `Stop()` no longer resets `NextDeferredId`.
  - `CheckDeferredTimeouts` reports `CortexErrorCodes::Timeout`.
- `Source/CortexEditor/Private/Operations/CortexEditorPIEOps.cpp`: `StartPIE`, `StopPIE` and `RestartPIE` return `INVALID_OPERATION` when no deferred callback is supplied. The check runs after their state checks and before any PIE request or state change.
- `Source/CortexCore/Private/CortexCommandRouter.cpp`: batch entries add `warnings` on success and `details` on failure. A file-local `WarningsToJson` is shared with `ResultToJson`.

## Tests

- `Cortex.Core.TcpServer.ReapsGracefullyClosedClient`: a real loopback client connects, becomes active, and closes. The server's client count returns to 0 and the disconnect callback fires once.
- `Cortex.Core.TcpServer.RejectsOversizedRequest`: a scripted `Recv` socket feeds `MaxMessageSize + 1` bytes, once unterminated and once followed by a newline. Each case retires the client with `LIMIT_EXCEEDED` and never dispatches.
- `Cortex.Core.TcpServer.AcceptsRequestAtMaxMessageSize`: a JSON request of exactly `MaxMessageSize` bytes is dispatched and the socket stays open.
- `Cortex.Core.TcpServer.StaleDeferredCallbackAfterRestartIsIgnored`: the server starts, defers a request, stops and restarts. Invoking the first run's callback then sends nothing to the second run's client, and that client's request stays pending.
- `Cortex.Core.Deferred.PerCommandTimeout`: the timed-out response code is now `TIMEOUT`.
- `Cortex.Editor.PIE.{StartPIE,StopPIE,RestartPIE}.RejectsMissingCallback`: each returns `INVALID_OPERATION`, is not deferred, queues no play session and leaves the tracked state unchanged.
- `Cortex.Core.Batch.EntryCarriesDetailsAndWarnings`: a probe domain returns a stale-precondition failure with `current_fingerprint` and a success with a warning. Both survive in the batch entries.
- `Cortex.Core.TcpServer.ProcessesBufferedRequestsWithoutNewData` now seeds raw bytes, to match the new buffer type.

## Verification

See [../verification/2026-11-17-issue221.md](../verification/2026-11-17-issue221.md).
