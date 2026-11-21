import json
from http.client import HTTPConnection
from threading import Thread
import unittest

from usage_server import UsageStore, create_server


class UsageStoreTests(unittest.TestCase):
    def setUp(self):
        self.store = UsageStore(":memory:", heartbeat_timeout_seconds=300)

    def tearDown(self):
        self.store.close()

    def record(self, session_id, event_id, timestamp):
        return self.store.record_event("client-1", session_id, event_id, timestamp)

    def test_summary_counts_utc_days_and_only_connected_heartbeat_intervals(self):
        self.record("session-1", "1", "2026-01-01T23:58:00Z")
        self.record("session-1", "2", "2026-01-02T00:00:00Z")
        self.record("session-1", "3", "2026-01-02T00:20:00Z")
        self.record("session-2", "4", "2026-01-02T00:00:00Z")
        self.record("session-2", "5", "2026-01-02T00:01:00Z")

        self.assertEqual(
            self.store.summary("client-1"),
            {
                "client_id": "client-1",
                "active_days": 2,
                "effective_duration_seconds": 180,
            },
        )

    def test_duplicate_event_is_idempotent(self):
        self.assertTrue(
            self.record("session-1", "same", "2026-01-01T00:00:00Z")
        )
        self.assertFalse(
            self.record("session-1", "same", "2026-01-01T00:01:00Z")
        )
        self.assertEqual(self.store.summary("client-1")["active_days"], 1)

    def test_timestamp_requires_timezone(self):
        with self.assertRaisesRegex(ValueError, "include a timezone"):
            self.record("session-1", "1", "2026-01-01T00:00:00")


class UsageApiTests(unittest.TestCase):
    def setUp(self):
        self.store = UsageStore(":memory:")
        self.server = create_server("127.0.0.1", 0, self.store)
        self.thread = Thread(target=self.server.serve_forever)
        self.thread.start()
        self.connection = HTTPConnection("127.0.0.1", self.server.server_port)

    def tearDown(self):
        self.connection.close()
        self.server.shutdown()
        self.thread.join()
        self.server.server_close()
        self.store.close()

    def request(self, method, path, payload=None):
        body = json.dumps(payload) if payload is not None else None
        self.connection.request(
            method,
            path,
            body=body,
            headers={"Content-Type": "application/json"} if body else {},
        )
        response = self.connection.getresponse()
        return response.status, json.loads(response.read())

    def test_events_can_be_collected_and_summarized(self):
        status, result = self.request(
            "POST",
            "/api/usage-events",
            {
                "client_id": "anonymous-client",
                "session_id": "session-1",
                "event_id": "event-1",
                "timestamp": "2026-01-01T12:00:00Z",
            },
        )
        self.assertEqual((status, result), (201, {"recorded": True}))
        self.request(
            "POST",
            "/api/usage-events",
            {
                "client_id": "anonymous-client",
                "session_id": "session-1",
                "event_id": "event-2",
                "timestamp": "2026-01-01T12:01:00Z",
            },
        )

        status, summary = self.request(
            "GET", "/api/usage-summary?client_id=anonymous-client"
        )
        self.assertEqual(status, 200)
        self.assertEqual(summary["active_days"], 1)
        self.assertEqual(summary["effective_duration_seconds"], 60)

    def test_invalid_event_returns_bad_request(self):
        status, result = self.request("POST", "/api/usage-events", {})
        self.assertEqual(status, 400)
        self.assertIn("client_id", result["error"])


if __name__ == "__main__":
    unittest.main()
