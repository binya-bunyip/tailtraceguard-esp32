import json
import unittest
from pathlib import Path


CONTRACT_PATH = Path(__file__).resolve().parents[1] / "docs" / "telemetry.schema.json"


class TelemetryContractTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        with CONTRACT_PATH.open(encoding="utf-8") as contract_file:
            cls.contract = json.load(contract_file)

    def test_required_fields_match_documented_request(self):
        self.assertEqual(
            set(self.contract["required"]),
            {"observedAt", "observedAddress", "protocol", "rssiDbm", "frequencyMHz"},
        )

    def test_protocol_is_limited_to_documented_values(self):
        self.assertEqual(self.contract["properties"]["protocol"]["enum"], ["wifi", "bluetooth"])

    def test_request_metadata_matches_documented_endpoint(self):
        self.assertEqual(
            self.contract["x-endpoint"],
            "POST https://tail-guard.polsia.io/api/telemetry",
        )
        self.assertEqual(self.contract["x-authorization"], "Bearer <registered device token>")
        self.assertEqual(self.contract["x-content-type"], "application/json")

    def test_undocumented_timestamp_and_gps_formats_are_not_assumed(self):
        properties = self.contract["properties"]
        self.assertNotIn("type", properties["observedAt"])
        self.assertNotIn("type", properties["gps"])
        self.assertNotIn("gps", self.contract["required"])


if __name__ == "__main__":
    unittest.main()