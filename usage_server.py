import argparse
from datetime import datetime, timedelta, timezone
from functools import partial
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import json
import os
from pathlib import Path
import sqlite3
from threading import RLock
from urllib.parse import parse_qs, urlsplit


MAX_REQUEST_SIZE = 16_384


def parse_timestamp(value):
    if not isinstance(value, str):
        raise ValueError("timestamp must be an ISO 8601 string")
    try:
        timestamp = datetime.fromisoformat(value.replace("Z", "+00:00"))
    except ValueError as error:
        raise ValueError("timestamp must be an ISO 8601 string") from error
    if timestamp.tzinfo is None or timestamp.utcoffset() is None:
        raise ValueError("timestamp must include a timezone")
    timestamp = timestamp.astimezone(timezone.utc)
    delta = timestamp - datetime(1970, 1, 1, tzinfo=timezone.utc)
    return delta.days * 86_400_000_000 + delta.seconds * 1_000_000 + delta.microseconds


def validate_identifier(value, name):
    if not isinstance(value, str) or not value.strip() or len(value) > 128:
        raise ValueError(f"{name} must be a non-empty string of at most 128 characters")
    return value


class UsageStore:
    def __init__(self, database_path, heartbeat_timeout_seconds=300):
        if heartbeat_timeout_seconds <= 0:
            raise ValueError("heartbeat timeout must be positive")
        self.heartbeat_timeout_us = heartbeat_timeout_seconds * 1_000_000
        if database_path != ":memory:":
            Path(database_path).parent.mkdir(parents=True, exist_ok=True)
        self.connection = sqlite3.connect(database_path, check_same_thread=False)
        self.connection.execute(
            """
            CREATE TABLE IF NOT EXISTS usage_events (
                client_id TEXT NOT NULL,
                session_id TEXT NOT NULL,
                event_id TEXT NOT NULL,
                occurred_at_us INTEGER NOT NULL,
                PRIMARY KEY (client_id, session_id, event_id)
            )
            """
        )
        self.connection.execute(
            "CREATE INDEX IF NOT EXISTS usage_events_client_time "
            "ON usage_events (client_id, occurred_at_us)"
        )
        self.connection.commit()
        self.lock = RLock()

    def record_event(self, client_id, session_id, event_id, timestamp):
        client_id = validate_identifier(client_id, "client_id")
        session_id = validate_identifier(session_id, "session_id")
        event_id = validate_identifier(event_id, "event_id")
        occurred_at_us = parse_timestamp(timestamp)
        with self.lock:
            cursor = self.connection.execute(
                "INSERT OR IGNORE INTO usage_events "
                "(client_id, session_id, event_id, occurred_at_us) VALUES (?, ?, ?, ?)",
                (client_id, session_id, event_id, occurred_at_us),
            )
            self.connection.commit()
            return cursor.rowcount == 1

    def summary(self, client_id):
        client_id = validate_identifier(client_id, "client_id")
        with self.lock:
            rows = self.connection.execute(
                "SELECT session_id, occurred_at_us FROM usage_events "
                "WHERE client_id = ? ORDER BY session_id, occurred_at_us",
                (client_id,),
            ).fetchall()

        active_days = set()
        duration_us = 0
        last_by_session = {}
        for session_id, occurred_at_us in rows:
            active_days.add(
                (
                    datetime(1970, 1, 1, tzinfo=timezone.utc)
                    + timedelta(microseconds=occurred_at_us)
                ).date().isoformat()
            )
            previous = last_by_session.get(session_id)
            if previous is not None:
                gap = occurred_at_us - previous
                if 0 < gap <= self.heartbeat_timeout_us:
                    duration_us += gap
            last_by_session[session_id] = occurred_at_us

        return {
            "client_id": client_id,
            "active_days": len(active_days),
            "effective_duration_seconds": duration_us / 1_000_000,
        }

    def close(self):
        with self.lock:
            self.connection.close()


class UsageRequestHandler(BaseHTTPRequestHandler):
    def __init__(self, *args, store, **kwargs):
        self.store = store
        super().__init__(*args, **kwargs)

    def _send_json(self, status, payload):
        body = json.dumps(payload).encode("utf-8")
        self.send_response(status)
        self.send_header("Content-Type", "application/json; charset=utf-8")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def do_POST(self):
        if urlsplit(self.path).path != "/api/usage-events":
            self._send_json(404, {"error": "not found"})
            return
        try:
            content_length = int(self.headers.get("Content-Length", "0"))
            if content_length <= 0 or content_length > MAX_REQUEST_SIZE:
                raise ValueError("request body must be between 1 and 16384 bytes")
            payload = json.loads(self.rfile.read(content_length))
            if not isinstance(payload, dict):
                raise ValueError("request body must be a JSON object")
            created = self.store.record_event(
                payload.get("client_id"),
                payload.get("session_id"),
                payload.get("event_id"),
                payload.get("timestamp"),
            )
        except (ValueError, json.JSONDecodeError, UnicodeDecodeError) as error:
            self._send_json(400, {"error": str(error)})
            return
        self._send_json(201 if created else 200, {"recorded": created})

    def do_GET(self):
        parsed = urlsplit(self.path)
        if parsed.path != "/api/usage-summary":
            self._send_json(404, {"error": "not found"})
            return
        client_ids = parse_qs(parsed.query).get("client_id", [])
        if len(client_ids) != 1:
            self._send_json(400, {"error": "client_id query parameter is required"})
            return
        try:
            self._send_json(200, self.store.summary(client_ids[0]))
        except ValueError as error:
            self._send_json(400, {"error": str(error)})

    def log_message(self, format, *args):
        return


def create_server(host, port, store):
    handler = partial(UsageRequestHandler, store=store)
    return ThreadingHTTPServer((host, port), handler)


def main():
    parser = argparse.ArgumentParser(description="Collect client usage events")
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=8000)
    parser.add_argument("--database", default=os.environ.get("LOGSERVER_DB", "usage.db"))
    parser.add_argument("--heartbeat-timeout-seconds", type=int, default=300)
    args = parser.parse_args()

    store = UsageStore(args.database, args.heartbeat_timeout_seconds)
    server = create_server(args.host, args.port, store)
    try:
        print(f"Usage server listening on {args.host}:{args.port}")
        server.serve_forever()
    except KeyboardInterrupt:
        pass
    finally:
        server.server_close()
        store.close()


if __name__ == "__main__":
    main()
