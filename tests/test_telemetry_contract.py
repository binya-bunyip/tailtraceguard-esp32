import json
import unittest
from datetime import datetime
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
        properties = self.contract["properties"]
        self.assertEqual(properties["observedAt"], {"type": "string", "format": "date-time"})
        self.assertEqual(properties["observedAddress"]["type"], "string")
        self.assertEqual(properties["rssiDbm"]["type"], "integer")
        self.assertEqual(properties["frequencyMHz"]["type"], "number")

    def test_protocol_is_limited_to_documented_values(self):
        self.assertEqual(self.contract["properties"]["protocol"]["enum"], ["wifi", "bluetooth"])

    def test_wifi_request_has_contract_shape_and_timezone_timestamp(self):
        request = {
            "observedAt": "2026-01-15T12:30:45Z",
            "observedAddress": "02:00:00:00:00:01",
            "protocol": "wifi",
            "rssiDbm": -47,
            "frequencyMHz": 2412,
        }

        self.assert_valid_request(request)
        self.assertNotIn("gps", request)

    def test_documented_bluetooth_request_shape_can_include_gps(self):
        request = {
            "observedAt": "2026-01-15T12:30:45+00:00",
            "observedAddress": "02:00:00:00:00:02",
            "protocol": "bluetooth",
            "rssiDbm": -62,
            "frequencyMHz": 2402,
            "gps": {"latitude": 37.5, "longitude": -122.2},
        }

        self.assert_valid_request(request)

    def test_required_fields_and_timestamp_timezone_are_enforced(self):
        request = {
            "observedAt": "2026-01-15T12:30:45",
            "observedAddress": "02:00:00:00:00:01",
            "protocol": "wifi",
            "rssiDbm": -47,
            "frequencyMHz": 2412,
        }

        with self.assertRaises(AssertionError):
            self.assert_valid_request(request)

        del request["frequencyMHz"]
        request["observedAt"] = "2026-01-15T12:30:45Z"
        with self.assertRaises(AssertionError):
            self.assert_valid_request(request)

    def test_gps_coordinates_must_be_numeric(self):
        request = {
            "observedAt": "2026-01-15T12:30:45Z",
            "observedAddress": "02:00:00:00:00:01",
            "protocol": "wifi",
            "rssiDbm": -47,
            "frequencyMHz": 2412,
            "gps": {"latitude": "37.5", "longitude": -122.2},
        }

        with self.assertRaises(AssertionError):
            self.assert_valid_request(request)

    def test_request_metadata_matches_documented_endpoint(self):
        self.assertEqual(
            self.contract["x-endpoint"],
            "POST https://tail-guard.polsia.io/api/telemetry",
        )
        self.assertEqual(self.contract["x-authorization"], "Bearer <registered device token>")
        self.assertEqual(self.contract["x-content-type"], "application/json")

    def test_public_build_defaults_enable_ca_validation_without_credentials(self):
        defaults_path = Path(__file__).resolve().parents[1] / "sdkconfig.defaults"
        defaults = defaults_path.read_text(encoding="utf-8")

        self.assertIn("CONFIG_MBEDTLS_CERTIFICATE_BUNDLE=y", defaults)
        self.assertIn("CONFIG_MBEDTLS_CERTIFICATE_BUNDLE_DEFAULT_FULL=y", defaults)
        self.assertNotIn("CONFIG_ESP_WIFI_SSID=", defaults)
        self.assertNotIn("CONFIG_ESP_WIFI_PASSWORD=", defaults)

    def test_firmware_uses_configured_https_without_legacy_batches_or_secrets(self):
        firmware_path = Path(__file__).resolve().parents[1] / "main" / "main.c"
        firmware_source = firmware_path.read_text(encoding="utf-8")

        self.assertNotIn("device_id", firmware_source)
        self.assertNotIn("events", firmware_source)
        self.assertIn('"https://tail-guard.polsia.io/api/telemetry"', firmware_source)
        self.assertIn("esp_crt_bundle_attach", firmware_source)
        self.assertIn('\\"protocol\\":\\"wifi\\"', firmware_source)
        self.assertIn("2407 + channel * 5", firmware_source)
        self.assertIn('"%Y-%m-%dT%H:%M:%SZ"', firmware_source)
        self.assertIn('esp_http_client_set_header(client, "Authorization", authorization)', firmware_source)
        self.assertIn('nvs_set_str(handle, "device_token", g_device_token)', firmware_source)
        self.assertNotIn("YOUR_API_TOKEN", firmware_source)
        self.assertNotIn("your-tailtrace-guard.example.com", firmware_source)
        self.assertNotIn('\\"protocol\\":\\"bluetooth\\"', firmware_source)

    def assert_valid_request(self, request):
        properties = self.contract["properties"]
        self.assertTrue(set(self.contract["required"]).issubset(request))
        self.assertIsInstance(request["observedAt"], str)
        timestamp = datetime.fromisoformat(request["observedAt"].replace("Z", "+00:00"))
        self.assertIsNotNone(timestamp.tzinfo)
        self.assertTrue(request["observedAddress"])
        self.assertIn(request["protocol"], properties["protocol"]["enum"])
        self.assertIs(type(request["rssiDbm"]), int)
        self.assertIn(type(request["frequencyMHz"]), (int, float))

        if "gps" in request:
            gps = request["gps"]
            self.assertIsInstance(gps, dict)
            self.assertTrue(set(properties["gps"]["required"]).issubset(gps))
            self.assertIn(type(gps["latitude"]), (int, float))
            self.assertIn(type(gps["longitude"]), (int, float))


if __name__ == "__main__":
    unittest.main()