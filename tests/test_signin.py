import base64
import hashlib
import json
import os
import shutil
import stat
import struct
import subprocess
import sys
import tempfile
import time
import unittest
from unittest import mock

sys.path.insert(0, os.environ.get("SIGNIN_SRC") or os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "src"))
import signin


def iso(stamp):
    return time.strftime("%Y-%m-%dT%H:%M:%S.1234567Z", time.gmtime(stamp))


def xsts_doc(token, exp, xid="2533274800000001", gtg="Tester", uhs="123"):
    claim = {"uhs": uhs}
    if xid:
        claim.update(xid=xid, gtg=gtg)
    doc = {"Token": token, "DisplayClaims": {"xui": [claim]}}
    if exp:
        doc["NotAfter"] = iso(exp)
    return 200, doc


class StateDirTest(unittest.TestCase):
    def setUp(self):
        self.dir = tempfile.mkdtemp(prefix="signin test ")
        self.old = signin.DATA_DIR
        signin.set_state_dir(self.dir)

    def tearDown(self):
        signin.set_state_dir(self.old)
        shutil.rmtree(self.dir)

    def write_tokens(self, **fields):
        with open(signin.TOKEN_PATH, "w") as f:
            f.write("".join("%s=%s\n" % kv for kv in fields.items()))


class Es256Test(unittest.TestCase):
    def on_curve(self, pt):
        b = 0x5AC635D8AA3A93E7B3EBBD55769886BC651D06B0CC53B0F63BCE3C3E27D2604B
        x, y = pt
        return (y * y - (x * x * x - 3 * x + b)) % signin._P == 0

    def test_group_order(self):
        self.assertTrue(self.on_curve(signin._G))
        self.assertIsNone(signin._ec_mul(signin._N, signin._G))
        self.assertEqual(signin._ec_mul(signin._N + 1, signin._G), signin._G)

    def test_known_public_key(self):
        # RFC 6979 A.2.5 P-256 key pair.
        d = 0xC9AFA9D845BA75166B5C215767B1D6934E50C3DB36E89B127B8A622B120F6721
        self.assertEqual(signin._ec_mul(d, signin._G), (
            0x60FED4BA255A9D31C961EB74C6356D68C049B8923B61FA6CE669622E60F29FB6,
            0x7903FE1008B8BC99A41AE9E95628BC64F2F1B20C2D7E9F5177A3C294D4462299))

    def test_signature_verifies(self):
        d, pub = signin.es256_keypair()
        self.assertTrue(self.on_curve(pub))
        msg = b"dungeons2forlinux"
        r, s = signin.es256_sign(d, msg)
        z = int.from_bytes(hashlib.sha256(msg).digest(), "big")
        w = pow(s, -1, signin._N)
        pt = signin._ec_add(signin._ec_mul(z * w % signin._N, signin._G),
                            signin._ec_mul(r * w % signin._N, pub))
        self.assertEqual(pt[0] % signin._N, r)

    @unittest.skipUnless(shutil.which("openssl"), "openssl not installed")
    def test_openssl_accepts_signature(self):
        d, (x, y) = signin.es256_keypair()
        msg = b"checked by an independent implementation"
        r, s = signin.es256_sign(d, msg)
        spki = bytes.fromhex("3059301306072a8648ce3d020106082a8648ce3d030107034200") + \
            b"\x04" + x.to_bytes(32, "big") + y.to_bytes(32, "big")

        def der_int(v):
            raw = v.to_bytes(33, "big").lstrip(b"\0")
            if raw[0] & 0x80:
                raw = b"\0" + raw
            return b"\x02" + bytes([len(raw)]) + raw
        body = der_int(r) + der_int(s)
        with tempfile.TemporaryDirectory() as tmp:
            pem = os.path.join(tmp, "pub.pem")
            with open(pem, "w") as f:
                f.write("-----BEGIN PUBLIC KEY-----\n%s\n-----END PUBLIC KEY-----\n" % base64.b64encode(spki).decode())
            with open(os.path.join(tmp, "sig"), "wb") as f:
                f.write(b"\x30" + bytes([len(body)]) + body)
            with open(os.path.join(tmp, "msg"), "wb") as f:
                f.write(msg)
            res = subprocess.run(["openssl", "dgst", "-sha256", "-verify", pem, "-signature",
                                  os.path.join(tmp, "sig"), os.path.join(tmp, "msg")],
                                 capture_output=True)
        self.assertEqual(res.returncode, 0, res.stdout + res.stderr)


class DeviceTokenTest(unittest.TestCase):
    def capture(self, fn, *args):
        seen = {}

        def fake_post(url, body, headers):
            seen.update(url=url, body=body, headers=headers)
            return 200, {"Token": "issued"}
        with mock.patch.object(signin, "post_raw", fake_post):
            self.assertEqual(fn(*args), "issued")
        return seen

    def verify(self, seen, path, pub):
        raw = base64.b64decode(seen["headers"]["Signature"])
        self.assertEqual(seen["headers"]["x-xbl-contract-version"], "1")
        self.assertEqual(len(raw), 4 + 8 + 64)
        signed = b"\0".join([raw[:4], raw[4:12], b"POST", path, b"", seen["body"]]) + b"\0"
        r, s = int.from_bytes(raw[12:44], "big"), int.from_bytes(raw[44:], "big")
        z = int.from_bytes(hashlib.sha256(signed).digest(), "big")
        w = pow(s, -1, signin._N)
        pt = signin._ec_add(signin._ec_mul(z * w % signin._N, signin._G),
                            signin._ec_mul(r * w % signin._N, pub))
        self.assertEqual(pt[0] % signin._N, r)

    def test_signature_header(self):
        seen = self.capture(signin.device_token)
        self.assertEqual(seen["url"], "https://device.auth.xboxlive.com/device/authenticate")
        raw = base64.b64decode(seen["headers"]["Signature"])
        self.assertEqual(struct.unpack(">I", raw[:4])[0], 1)
        filetime = struct.unpack(">Q", raw[4:12])[0]
        self.assertLess(abs(filetime / 10_000_000 - 11644473600 - time.time()), 60)
        key = json.loads(seen["body"])["Properties"]["ProofKey"]
        unb64 = lambda v: int.from_bytes(base64.urlsafe_b64decode(v + "=" * (-len(v) % 4)), "big")
        self.verify(seen, b"/device/authenticate", (unb64(key["x"]), unb64(key["y"])))

    def test_rejection(self):
        with mock.patch.object(signin, "post_raw", return_value=(400, {})):
            with self.assertRaises(signin.AuthError):
                signin.device_token()


class HelpersTest(unittest.TestCase):
    def test_not_after(self):
        self.assertEqual(signin.not_after({"NotAfter": "2026-09-30T04:58:51.2004971Z"}), 1790744331)
        self.assertEqual(signin.not_after({"NotAfter": "1970-01-01T00:00:10Z"}), 10)
        for bad in ({}, {"NotAfter": None}, {"NotAfter": "soon"}, {"NotAfter": 5}):
            self.assertEqual(signin.not_after(bad), 0, bad)

    def test_rps_ticket(self):
        self.assertEqual(signin.rps_ticket("eyJabc"), "d=eyJabc")
        self.assertEqual(signin.rps_ticket("EwA"), "t=EwA")

    def test_state_dir_argument(self):
        self.assertEqual(signin._state_dir(["x.py", "--state", "/s d"]), "/s d")
        self.assertEqual(signin._state_dir(["x.py", "--state"]), os.path.dirname(os.path.abspath(signin.__file__)))

    def test_cache_valid(self):
        now = int(time.time())
        self.assertTrue(signin.cache_valid({"exp": str(now + 3600), "xbox": "t"}))
        self.assertFalse(signin.cache_valid({"exp": str(now + 60), "xbox": "t"}))
        self.assertFalse(signin.cache_valid({"exp": str(now + 3600), "xbox": ""}))
        self.assertFalse(signin.cache_valid({"exp": "later", "xbox": "t"}))
        self.assertFalse(signin.cache_valid({}))

    def test_xerr_hint(self):
        self.assertIn("no Xbox profile", signin.xerr_text({"XErr": 2148916233}))
        self.assertEqual(signin.xerr_text({"XErr": 5}), "XErr 5")
        self.assertEqual(signin.xerr_text({}), "XErr None")


class PostRawTest(unittest.TestCase):
    def fails_with(self, exc):
        with mock.patch("urllib.request.urlopen", side_effect=exc):
            with self.assertRaises(signin.NetworkError):
                signin.post_raw("https://login.live.com/x", b"", {})

    def test_network_errors(self):
        import http.client
        import urllib.error
        self.fails_with(urllib.error.URLError("refused"))
        self.fails_with(TimeoutError())
        self.fails_with(http.client.IncompleteRead(b""))
        self.fails_with(http.client.BadStatusLine("x"))

    def test_non_json_and_non_dict(self):
        class Resp:
            status = 200

            def __init__(self, raw):
                self.raw = raw

            def read(self):
                return self.raw

            def __enter__(self):
                return self

            def __exit__(self, *a):
                return False
        for raw, want in ((b"<html>", {}), (b"[1]", {}), (b"\xff", {}), (b"", {}), (b'{"a":1}', {"a": 1})):
            with mock.patch("urllib.request.urlopen", return_value=Resp(raw)):
                self.assertEqual(signin.post_raw("https://x/", b"", {}), (200, want))


class FilesTest(StateDirTest):
    def test_write_private(self):
        signin.write_private(signin.TOKEN_PATH, "a=1\n")
        self.assertEqual(stat.S_IMODE(os.stat(signin.TOKEN_PATH).st_mode), 0o600)
        self.assertEqual(os.listdir(self.dir), ["tokens.txt"])
        signin.write_private(signin.TOKEN_PATH, "a=2\n")
        self.assertEqual(signin.read_tokens(), {"a": "2"})

    def test_read_tokens(self):
        self.assertEqual(signin.read_tokens(), {})
        with open(signin.TOKEN_PATH, "wb") as f:
            f.write(b"xbox=XBL3.0 x=1;t=a\njunk\nrefresh=r\xff\n")
        tokens = signin.read_tokens()
        self.assertEqual(tokens["xbox"], "XBL3.0 x=1;t=a")
        self.assertTrue(tokens["refresh"].startswith("r"))

    def test_status(self):
        with mock.patch("sys.stdout"):
            self.assertEqual(signin.status(), 1)
        self.write_tokens(exp="garbage", xbox="x")
        with mock.patch("sys.stdout"):
            self.assertEqual(signin.status(), 0)


class SignInTest(StateDirTest):

    def fake_services(self, pf_fails=False, mc_response=None, msa_expires=None, xsts_exp=True):
        exp = int(time.time()) + 50000 if xsts_exp else 0
        calls = []
        self.xsts_props = {}

        def request(url, form=None, payload=None):
            calls.append(url)
            if "oauth20_token" in url:
                doc = {"access_token": "new-msa", "refresh_token": "new-refresh"}
                if msa_expires is not None:
                    doc["expires_in"] = msa_expires
                return 200, doc
            if "user/authenticate" in url:
                return 200, {"Token": "user-token"}
            rp = payload["RelyingParty"]
            self.xsts_props.setdefault(rp, []).append(payload["Properties"])
            if rp == signin.XBOX_RP:
                return xsts_doc("xbox-jwe", exp)
            if rp == signin.MINECRAFT_RP:
                return mc_response or xsts_doc("mc-jwe", exp and exp + 10, xid=None)
            if rp == signin.PLAYFAB_RP:
                return (401, {"XErr": 5}) if pf_fails else xsts_doc("pf-jwe", exp and exp + 20, xid=None)
            raise AssertionError(url)
        patches = [mock.patch.object(signin, "request", request),
                   mock.patch.object(signin, "device_token", return_value="device"),
                   mock.patch("sys.stderr")]
        for p in patches:
            p.start()
            self.addCleanup(p.stop)
        return exp, calls

    def test_refresh_writes_all_tokens(self):
        exp, _ = self.fake_services()
        self.write_tokens(exp="0", xbox="old", refresh="old-refresh")
        self.assertEqual(signin.main([]), 0)
        tokens = signin.read_tokens()
        self.assertEqual(int(tokens["exp"]), exp)
        self.assertEqual(tokens["xuid"], "2533274800000001")
        self.assertEqual(tokens["gamertag"], "Tester")
        self.assertTrue(tokens["xbox"].startswith("XBL3.0 x=123;"))
        self.assertTrue(tokens["mc"] and tokens["pf"])
        self.assertEqual((tokens["msa"], tokens["refresh"]), ("new-msa", "new-refresh"))
        self.assertFalse(os.path.exists(signin.ERROR_PATH))
        self.assertNotIn("DeviceToken", self.xsts_props[signin.XBOX_RP][0])
        self.assertEqual(self.xsts_props[signin.PLAYFAB_RP][0]["DeviceToken"], "device")
        self.assertEqual(stat.S_IMODE(os.stat(signin.TOKEN_PATH).st_mode), 0o600)

    def test_valid_cache_is_kept(self):
        _, calls = self.fake_services()
        self.write_tokens(exp=str(int(time.time()) + 3600), xbox="x")
        self.assertEqual(signin.main([]), 0)
        self.assertEqual(calls, [])
        with mock.patch.object(signin, "msa_device_code", side_effect=signin.AuthError("code expired")):
            self.assertEqual(signin.main(["--force"]), 1)
        with open(signin.ERROR_PATH) as f:
            self.assertEqual(f.read(), "code expired\n")

    def test_optional_token_failure_degrades(self):
        self.fake_services(pf_fails=True, mc_response=(200, {"Token": "t", "DisplayClaims": {}}))
        self.write_tokens(refresh="r")
        self.assertEqual(signin.main([]), 0)
        tokens = signin.read_tokens()
        self.assertEqual((tokens["pf"], tokens["mc"]), ("", ""))
        self.assertIn("XErr 5", tokens["pf_error"])
        self.assertIn("minecraftservices", tokens["mc_error"])

    def test_msa_expiry_caps_cache(self):
        self.fake_services(msa_expires=3600)
        self.write_tokens(refresh="r")
        self.assertEqual(signin.main([]), 0)
        self.assertLessEqual(int(signin.read_tokens()["exp"]), int(time.time()) + 3600)

    def test_xsts_expiry_caps_long_msa(self):
        exp, _ = self.fake_services(msa_expires=10 ** 6)
        self.write_tokens(refresh="r")
        self.assertEqual(signin.main([]), 0)
        self.assertEqual(int(signin.read_tokens()["exp"]), exp)

    def test_unknown_expiry_is_an_hour(self):
        self.fake_services(msa_expires=10 ** 6, xsts_exp=False)
        self.write_tokens(refresh="r")
        self.assertEqual(signin.main([]), 0)
        self.assertAlmostEqual(int(signin.read_tokens()["exp"]), int(time.time()) + 3600, delta=5)

    def test_unexpected_exception_still_reports(self):
        for exc in (RuntimeError("boom"), AttributeError("x"), OSError("disk")):
            with mock.patch.object(signin, "msa_refresh", side_effect=exc), mock.patch("sys.stderr"):
                self.write_tokens(refresh="r")
                self.assertEqual(signin.main([]), 1)
                with open(signin.ERROR_PATH) as f:
                    self.assertIn(type(exc).__name__, f.read())

    def test_unreadable_cache_reports(self):
        with mock.patch.object(signin, "read_tokens", side_effect=PermissionError()), mock.patch("sys.stderr"):
            self.assertEqual(signin.main([]), 1)
        with open(signin.ERROR_PATH) as f:
            self.assertIn("PermissionError", f.read())

    def test_rejected_refresh_falls_back_to_device_code(self):
        with mock.patch.object(signin, "request", return_value=(400, {"error": "invalid_grant"})), \
                mock.patch.object(signin, "msa_device_code", side_effect=signin.AuthError("no code")), \
                mock.patch("sys.stderr"):
            self.write_tokens(refresh="r")
            self.assertEqual(signin.main([]), 1)
            with open(signin.ERROR_PATH) as f:
                self.assertEqual(f.read(), "no code\n")

    def test_refresh_server_error(self):
        with mock.patch.object(signin, "request", return_value=(503, {})), mock.patch("sys.stderr"):
            with self.assertRaises(signin.AuthError):
                signin.msa_refresh("r")

    def test_line_break_in_value_is_rejected(self):
        self.fake_services()
        with mock.patch.object(signin, "xsts", return_value=("XBL3.0 x=1;a\nb", {"xid": "1"}, 0)):
            with self.assertRaises(signin.AuthError):
                signin.sign_in({"access_token": "a"})
        self.assertFalse(os.path.exists(signin.TOKEN_PATH))


class DeviceCodeTest(StateDirTest):
    def run_flow(self, polls):
        start = (200, {"device_code": "dc", "user_code": "ABCD", "interval": 1, "expires_in": 900,
                       "verification_uri": "https://www.microsoft.com/link"})
        replies = iter([start] + polls)
        seen_code = []

        def request(url, form=None, payload=None):
            reply = next(replies)
            if isinstance(reply, Exception):
                raise reply
            if form.get("device_code"):
                seen_code.append(os.path.exists(signin.CODE_PATH))
            return reply
        with mock.patch.object(signin, "request", request), mock.patch("time.sleep") as sleep, \
                mock.patch("subprocess.Popen"), mock.patch("sys.stderr"):
            try:
                return signin.msa_device_code(), sleep, seen_code
            finally:
                self.assertFalse(os.path.exists(signin.CODE_PATH))

    def test_success_after_pending_and_blips(self):
        doc, sleep, seen_code = self.run_flow([
            (400, {"error": "authorization_pending"}),
            signin.NetworkError("cannot reach login.live.com"),
            (400, {"error": "slow_down"}),
            (200, {"access_token": "a", "refresh_token": "r"})])
        self.assertEqual(doc["access_token"], "a")
        self.assertEqual([c.args[0] for c in sleep.call_args_list], [5, 5, 5, 10])
        self.assertTrue(all(seen_code))

    def test_declined(self):
        with self.assertRaises(signin.AuthError) as cm:
            self.run_flow([(400, {"error": "authorization_declined"})])
        self.assertIn("authorization_declined", str(cm.exception))

    def test_times_out(self):
        clock = iter(range(0, 10000, 400))
        with mock.patch("time.time", lambda: next(clock)):
            with self.assertRaises(signin.AuthError) as cm:
                self.run_flow([(400, {"error": "authorization_pending"})] * 10)
        self.assertIn("timed out", str(cm.exception))


if __name__ == "__main__":
    unittest.main()
