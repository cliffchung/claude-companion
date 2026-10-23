# Bridge protocol

The optional bridge (`bridge/companion-bridge.py`) speaks plain HTTP. Every route except
`/healthz` and `/hook` needs the token, sent either as `Authorization: Bearer <token>` or as
`?token=<token>`. `EventSource` can't set headers, which is why the query form exists.

## Session object

```json
{
  "sessionId": "e0000001-0001-4001-8001-000000000001",
  "id": "e0000001",
  "cwd": "/Users/me/code/my-project",
  "repo": "my-project",
  "branch": "main",
  "kind": "background",
  "name": "fix the error banners",
  "state": "needs_you",
  "stateSource": "roster",
  "startedAt": 1788190149951,
  "lastEventAt": 1788190149951,
  "line": ""
}
```

- `state` is one of `needs_you`, `working`, `done`, `idle`, `ended`.
- `kind` is `background` (you can reply) or `interactive` (read-only).
- `line` holds the latest hook text, such as a notification message or the last assistant message.

## Routes

| Method | Path | Body / query | Returns |
|---|---|---|---|
| GET | `/events` | — | SSE stream of `snapshot` messages |
| GET | `/api/sessions` | — | one `snapshot` |
| POST | `/api/reply` | `{"sessionId","text"}` | `{"queued":true}` |
| POST | `/api/uploads/image` | raw image body; `?sessionId=&caption=` | `{"saved","attached"}` |
| POST | `/api/uploads/audio` | raw audio body; `?sessionId=` | `{"saved","transcribed":false}` |
| POST | `/hook` | Claude Code hook JSON plus `_label` | `{"ok":true}` (localhost only, no token) |
| GET | `/healthz` | — | `{"ok":true}` |

## Snapshot

```json
{ "type": "snapshot", "ts": 1792718066798,
  "counts": { "needs_you": 4, "working": 2, "done": 10 },
  "sessions": [ /* session objects, needs_you first */ ] }
```

The bridge pushes a new snapshot on every roster poll and every hook event, and sends a
`: ping` comment every 15 seconds so idle connections stay open.
