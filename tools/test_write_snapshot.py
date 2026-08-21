from __future__ import annotations

import unittest
from unittest.mock import patch

import write_snapshot


class ExporterPayloadTests(unittest.TestCase):
    def test_weekly_fields_drive_the_ring(self) -> None:
        state = {
            "codex_plan_type": "plus",
            "codex_model": "gpt-5.6",
            "codex_weekly_used_percent": 27,
            "codex_weekly_remaining_percent": 73,
            "codex_weekly_reset_at_epoch": 1_700_529_200,
            "codex_weekly_window_mins": 10_080,
            "codex_5h_remaining_percent": 99,
        }

        with patch.object(write_snapshot.time, "time", return_value=1_700_000_000):
            payload = write_snapshot.payload_from_exporter_state(
                state, next_poll_in=7_200
            )

        primary = payload["preferred"]["primary"]
        self.assertEqual(primary["remaining"], 73)
        self.assertEqual(primary["used"], 27)
        self.assertEqual(primary["windowMins"], 10_080)
        self.assertEqual(primary["resetsIn"], 529_200)
        self.assertEqual(payload["planLabel"], "PLUS")
        self.assertEqual(payload["modelLabel"], "GPT-5.6")
        self.assertEqual(payload["source"], "MQTT EXPORTER")
        self.assertEqual(payload["nextPollIn"], 7_200)

    def test_remaining_can_be_derived_from_weekly_used(self) -> None:
        payload = write_snapshot.payload_from_exporter_state(
            {
                "codex_weekly_used_percent": 84,
                "codex_weekly_window_mins": 10_080,
            },
            next_poll_in=7_200,
        )

        self.assertEqual(payload["preferred"]["primary"]["remaining"], 16)

    def test_missing_weekly_percentage_is_rejected(self) -> None:
        with self.assertRaisesRegex(RuntimeError, "7-day quota"):
            write_snapshot.payload_from_exporter_state(
                {
                    "codex_5h_remaining_percent": 73,
                    "codex_5h_window_mins": 300,
                }
            )

    def test_duration_recovers_current_exporter_field_name_drift(self) -> None:
        payload = write_snapshot.payload_from_exporter_state(
            {
                "codex_5h_used_percent": 27,
                "codex_5h_remaining_percent": 73,
                "codex_5h_window_mins": 10_080,
            }
        )

        self.assertEqual(payload["preferred"]["primary"]["remaining"], 73)
        self.assertEqual(payload["preferred"]["primary"]["windowMins"], 10_080)

    def test_refresh_interval_is_never_below_five_minutes(self) -> None:
        payload = write_snapshot.build_payload(
            remaining=73,
            used=27,
            window_mins=10_080,
            resets_in=60,
            plan="plus",
            plan_label="PLUS",
            next_poll_in=5,
        )

        self.assertEqual(payload["nextPollIn"], 300)


if __name__ == "__main__":
    unittest.main()
