import hashlib
import importlib
import os
import sys
import tempfile
import unittest
from pathlib import Path

from fastapi.testclient import TestClient


class MseedServerTest(unittest.TestCase):
    def setUp(self):
        self.temp_dir = tempfile.TemporaryDirectory()
        os.environ["MSEED_DATA_ROOT"] = self.temp_dir.name
        sys.modules.pop("mseed_server", None)
        self.server = importlib.import_module("mseed_server")
        self.client_context = TestClient(self.server.app)
        self.client = self.client_context.__enter__()

    def tearDown(self):
        self.client_context.__exit__(None, None, None)
        self.temp_dir.cleanup()

    def test_device_config_status_and_scoped_file_flow(self):
        response = self.client.put("/coalmine/devices/02", json={"name": "EG628-02"})
        self.assertEqual(response.status_code, 200)

        config = {
            "parameters": {
                "tri_on": 5.5,
                "tri_off": 2,
                "nsta": 50,
                "nlta": 500,
                "detect_ch": 1,
                "qualified_ch": 1,
                "energy_thre": 0.25,
                "amp_thre": 0.25,
                "intrach": 100,
                "interch": 250,
            },
            "upload_server_url": "http://server.example:443",
        }
        response = self.client.put("/coalmine/devices/02/config", json=config)
        self.assertEqual(response.status_code, 200)
        self.assertEqual(response.json()["version"], 1)

        response = self.client.get("/coalmine/devices/02/config/latest?after_version=0")
        self.assertEqual(response.status_code, 200)
        self.assertEqual(response.json()["parameters"]["tri_on"], 5.5)
        self.assertEqual(
            self.client.get("/coalmine/devices/02/config/latest?after_version=1").status_code,
            204,
        )

        response = self.client.post(
            "/coalmine/devices/02/config/applied",
            json={"version": 1, "success": True, "message": "runtime applied"},
        )
        self.assertEqual(response.json()["status"], "applied")

        heartbeat = {
            "program_running": True,
            "program_version": "1.0.0",
            "network_state": "online",
            "local_ip": "192.168.111.2",
            "applied_config_version": 1,
            "stations": [{
                "station_number": 1,
                "ip": "192.168.111.10",
                "port": 5000,
                "connected": True,
                "online_channels": 12,
                "total_channels": 16,
                "gps": 5,
            }],
        }
        self.assertEqual(
            self.client.post("/coalmine/devices/02/heartbeat", json=heartbeat).status_code,
            200,
        )
        device = self.client.get("/coalmine/devices/02").json()
        self.assertTrue(device["online"])
        self.assertEqual(device["status"]["stations"][0]["online_channels"], 12)
        self.assertEqual(device["status"]["stations"][0]["ip"], "192.168.111.10")
        self.assertEqual(device["status"]["stations"][0]["port"], 5000)
        self.assertEqual(device["applied_config_version"], 1)

        body = b"complete-mseed-test"
        name = "event.mseed"
        upload_url = f"/coalmine/mseed/upload?device_id=02&name={name}"
        response = self.client.put(
            upload_url,
            content=body,
            headers={
                "X-Device-ID": "02",
                "Content-Range": f"bytes 0-{len(body) - 1}/{len(body)}",
            },
        )
        self.assertEqual(response.status_code, 201)
        self.assertTrue((Path(self.temp_dir.name) / "files" / "02" / name).is_file())

        item = self.client.get("/coalmine/mseed/02/next?after_id=0").json()
        self.assertEqual(item["device_id"], "02")
        self.assertEqual(item["sha256"], hashlib.sha256(body).hexdigest())
        response = self.client.get(
            f"/coalmine/mseed/02/files/{item['id']}/{name}"
        )
        self.assertEqual(response.content, body)


if __name__ == "__main__":
    unittest.main()
