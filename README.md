# logServer
CAE软件客户使用数据收集

## Usage collection API

Run the server with `python3 usage_server.py`. It listens on `127.0.0.1:8000`
and stores events in `usage.db` by default. Set `LOGSERVER_DB` or pass
`--database` to select another SQLite database. Clients should send activity
heartbeats to `POST /api/usage-events`:

```json
{
  "client_id": "anonymous-client-id",
  "session_id": "application-session-id",
  "event_id": "unique-event-id",
  "timestamp": "2026-01-01T12:00:00Z"
}
```

Event IDs make retries idempotent within a client session. Timestamps must be
ISO 8601 values with a timezone. The service returns a summary from
`GET /api/usage-summary?client_id=anonymous-client-id`. Active days are UTC
calendar days with at least one event. Effective duration sums intervals
between events in the same session only when the interval is positive and no
longer than the heartbeat timeout (five minutes by default). Longer gaps do
not count as usage. Configure the timeout with
`--heartbeat-timeout-seconds`.

Use anonymous client identifiers and do not send customer files, model data,
or other unnecessary personal information. The API is intended to run behind
an authenticated, TLS-protected deployment when exposed beyond localhost.

Run tests with `python3 -m unittest discover -s tests`.
