#!/usr/bin/env python3
"""Signs in with a Microsoft account and caches the Xbox tokens xgameruntime.dll
hands to Minecraft Dungeons II.

    python3 signin.py            refresh the cache if needed
    python3 signin.py --force    refresh even if the cache is valid
    python3 signin.py --status   show what is cached, without secrets
"""
import base64
import calendar
import fcntl
import hashlib
import http.client
import json
import os
import secrets
import struct
import subprocess
import sys
import time
import urllib.error
import urllib.parse
import urllib.request
import uuid

CLIENT_ID = "00000000497C1B94"  # the Minecraft launcher's public app id
SCOPE = "service::user.auth.xboxlive.com::MBI_SSL"


def _state_dir(argv):
    if "--state" in argv[:-1]:
        return argv[argv.index("--state") + 1]
    return os.path.dirname(os.path.abspath(__file__))


def set_state_dir(path):
    global DATA_DIR, TOKEN_PATH, CODE_PATH, ERROR_PATH, LOCK_PATH
    DATA_DIR = path
    TOKEN_PATH = os.path.join(path, "tokens.txt")
    CODE_PATH = os.path.join(path, "login-code.txt")
    ERROR_PATH = os.path.join(path, "login-error.txt")
    LOCK_PATH = os.path.join(path, ".signin.lock")


set_state_dir(_state_dir(sys.argv))

XBOX_RP = "http://xboxlive.com"
MINECRAFT_RP = "rp://api.minecraftservices.com/"
PLAYFAB_RP = "http://playfab.xboxlive.com/"  # needs a device token
MIN_VALID_S = 120
TIMEOUT_S = 30


def log(message):
    print("signin: %s" % message, file=sys.stderr)


class AuthError(Exception):
    pass


class NetworkError(AuthError):
    pass


def request(url, form=None, payload=None):
    if payload is not None:
        return post_raw(url, json.dumps(payload).encode(), {})
    return post_raw(url, urllib.parse.urlencode(form).encode(),
                    {"Content-Type": "application/x-www-form-urlencoded"})


def post_raw(url, body, headers):
    hdrs = {"Content-Type": "application/json", "Accept": "application/json"}
    hdrs.update(headers)
    req = urllib.request.Request(url, data=body, headers=hdrs, method="POST")
    try:
        with urllib.request.urlopen(req, timeout=TIMEOUT_S) as resp:
            status, raw = resp.status, resp.read()
    except urllib.error.HTTPError as exc:
        status, raw = exc.code, exc.read()
    except (urllib.error.URLError, OSError, http.client.HTTPException) as exc:
        raise NetworkError("cannot reach %s: %s" % (urllib.parse.urlsplit(url).netloc, exc)) from None
    try:
        doc = json.loads(raw.decode() or "{}")
    except (UnicodeDecodeError, json.JSONDecodeError):
        doc = {}
    return status, doc if isinstance(doc, dict) else {}


def not_after(doc):
    try:
        return calendar.timegm(time.strptime(doc["NotAfter"][:19], "%Y-%m-%dT%H:%M:%S"))
    except (KeyError, TypeError, ValueError):
        return 0


# ECDSA over NIST P-256 (ES256), for the device-token request.
_P = 0xFFFFFFFF00000001000000000000000000000000FFFFFFFFFFFFFFFFFFFFFFFF
_N = 0xFFFFFFFF00000000FFFFFFFFFFFFFFFFBCE6FAADA7179E84F3B9CAC2FC632551
_G = (0x6B17D1F2E12C4247F8BCE6E563A440F277037D812DEB33A0F4A13945D898C296,
      0x4FE342E2FE1A7F9B8EE7EB4A7C0F9E162BCE33576B315ECECBB6406837BF51F5)


def _ec_add(p, q):
    if p is None:
        return q
    if q is None:
        return p
    if p[0] == q[0] and (p[1] + q[1]) % _P == 0:
        return None
    if p == q:
        m = (3 * p[0] * p[0] - 3) * pow(2 * p[1], _P - 2, _P) % _P
    else:
        m = (q[1] - p[1]) * pow(q[0] - p[0], _P - 2, _P) % _P
    x = (m * m - p[0] - q[0]) % _P
    return x, (m * (p[0] - x) - p[1]) % _P


def _ec_mul(k, point):
    result = None
    while k:
        if k & 1:
            result = _ec_add(result, point)
        point = _ec_add(point, point)
        k >>= 1
    return result


def es256_keypair():
    d = secrets.randbelow(_N - 1) + 1
    return d, _ec_mul(d, _G)


def es256_sign(d, message):
    z = int.from_bytes(hashlib.sha256(message).digest(), "big")
    while True:
        k = secrets.randbelow(_N - 1) + 1
        r = _ec_mul(k, _G)[0] % _N
        s = pow(k, _N - 2, _N) * (z + r * d) % _N
        if r and s:
            return r, s


def _b64url(raw):
    return base64.urlsafe_b64encode(raw).rstrip(b"=").decode()


def device_token():
    d, (x, y) = es256_keypair()
    body = json.dumps({
        "RelyingParty": "http://auth.xboxlive.com",
        "TokenType": "JWT",
        "Properties": {
            "AuthMethod": "ProofOfPossession",
            "Id": "{%s}" % uuid.uuid4(),
            "DeviceType": "Win32",
            "SerialNumber": "{%s}" % uuid.uuid4(),
            "Version": "10.0.19041",
            "ProofKey": {"use": "sig", "alg": "ES256", "kty": "EC", "crv": "P-256",
                         "x": _b64url(x.to_bytes(32, "big")), "y": _b64url(y.to_bytes(32, "big"))},
        },
    }).encode()
    version = struct.pack(">I", 1)
    stamp = struct.pack(">Q", int((time.time() + 11644473600) * 10_000_000))
    # Signed data: version, FILETIME, method, path, authorization header and body, NUL-separated.
    signed = b"\0".join([version, stamp, b"POST", b"/device/authenticate", b"", body[:8192]]) + b"\0"
    r, s = es256_sign(d, signed)
    signature = base64.b64encode(version + stamp + r.to_bytes(32, "big") + s.to_bytes(32, "big")).decode()
    status, doc = post_raw("https://device.auth.xboxlive.com/device/authenticate", body,
                           {"x-xbl-contract-version": "1", "Signature": signature})
    if "Token" not in doc:
        raise AuthError("device authentication failed (HTTP %s, %s)" % (status, xerr_text(doc)))
    return doc["Token"]


def msa_refresh(refresh_token):
    status, doc = request("https://login.live.com/oauth20_token.srf", form={
        "client_id": CLIENT_ID, "grant_type": "refresh_token",
        "refresh_token": refresh_token, "scope": SCOPE})
    if "access_token" in doc:
        return doc
    if status >= 500:
        raise AuthError("Microsoft sign-in service error (HTTP %s)" % status)
    return None


def msa_device_code():
    status, start = request("https://login.live.com/oauth20_connect.srf", form={
        "client_id": CLIENT_ID, "scope": SCOPE, "response_type": "device_code"})
    if "device_code" not in start:
        raise AuthError("could not start Microsoft sign-in (HTTP %s)" % status)
    url = start.get("verification_uri") or "https://www.microsoft.com/link"
    show_code(url, start["user_code"])
    interval = max(int(start.get("interval", 5)), 5)
    deadline = time.time() + int(start.get("expires_in", 900))
    try:
        while time.time() < deadline:
            time.sleep(interval)
            try:
                _, doc = request("https://login.live.com/oauth20_token.srf", form={
                    "client_id": CLIENT_ID, "device_code": start["device_code"],
                    "grant_type": "urn:ietf:params:oauth:grant-type:device_code"})
            except NetworkError as exc:
                log("%s; retrying" % exc)
                continue
            if "access_token" in doc:
                return doc
            err = doc.get("error", "")
            if err == "slow_down":
                interval += 5
            elif err != "authorization_pending":
                raise AuthError("Microsoft sign-in failed: %s" % (err or "unknown error"))
        raise AuthError("Microsoft sign-in timed out")
    finally:
        remove(CODE_PATH)


def show_code(url, code):
    write_private(CODE_PATH, "%s\n%s\n" % (url, code))  # shown in-game by the DLL
    log("sign in at %s with code %s" % (url, code))
    try:
        subprocess.Popen(["xdg-open", url], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                         start_new_session=True)
    except OSError:
        pass


def rps_ticket(access_token):
    # "d=" for JWT access tokens, "t=" for compact ones.
    return ("d=" if access_token.startswith("eyJ") else "t=") + access_token


XERR_HINTS = {
    2148916233: "this Microsoft account has no Xbox profile; sign in at xbox.com once to create one",
    2148916235: "Xbox Live is not available in this account's country or region",
    2148916236: "this account needs adult verification (South Korea)",
    2148916237: "this account needs adult verification (South Korea)",
    2148916238: "this is a child account; an adult must add it to a Microsoft family",
}


def xerr_text(doc):
    xerr = doc.get("XErr")
    hint = XERR_HINTS.get(xerr) if isinstance(xerr, int) else None
    return "XErr %s: %s" % (xerr, hint) if hint else "XErr %s" % xerr


def xbox_user_token(access_token):
    status, doc = request("https://user.auth.xboxlive.com/user/authenticate", payload={
        "RelyingParty": "http://auth.xboxlive.com", "TokenType": "JWT",
        "Properties": {"AuthMethod": "RPS", "SiteName": "user.auth.xboxlive.com",
                       "RpsTicket": rps_ticket(access_token)}})
    if "Token" not in doc:
        raise AuthError("Xbox user authentication failed (HTTP %s, %s)" % (status, xerr_text(doc)))
    return doc["Token"]


def xsts(user_token, relying_party, device=None):
    props = {"SandboxId": "RETAIL", "UserTokens": [user_token]}
    if device:
        props["DeviceToken"] = device
    status, doc = request("https://xsts.auth.xboxlive.com/xsts/authorize", payload={
        "RelyingParty": relying_party, "TokenType": "JWT", "Properties": props})
    try:
        token, claim = doc["Token"], doc["DisplayClaims"]["xui"][0]
        return "XBL3.0 x=%s;%s" % (claim["uhs"], token), claim, not_after(doc)
    except (KeyError, IndexError, TypeError):
        raise AuthError("Xbox token for %s failed (HTTP %s, %s)" % (relying_party, status, xerr_text(doc))) from None


def optional_token(label, fetch):
    try:
        token, _, exp = fetch()
        return token, exp, ""
    except AuthError as exc:
        log("%s token unavailable: %s" % (label, exc))
        return "", 0, str(exc)[:200]


def sign_in(msa):
    user = xbox_user_token(msa["access_token"])
    xbox, claim, xbox_exp = xsts(user, XBOX_RP)
    mc, mc_exp, mc_error = optional_token("Minecraft", lambda: xsts(user, MINECRAFT_RP))
    pf, pf_exp, pf_error = optional_token("PlayFab", lambda: xsts(user, PLAYFAB_RP, device_token()))
    # The cache expires with its first token, or after an hour if none has an expiry.
    exps = [e for e in (xbox_exp, mc_exp, pf_exp) if e] or [int(time.time()) + 3600]
    try:
        exps.append(int(time.time()) + int(msa["expires_in"]))
    except (KeyError, TypeError, ValueError):
        pass
    fields = {
        "exp": str(min(exps)),
        "xuid": str(claim.get("xid", "0")),
        "gamertag": str(claim.get("gtg", "Player")),
        "xbox": xbox, "mc": mc, "pf": pf,
        "msa": msa["access_token"],
        "refresh": msa.get("refresh_token", ""),
        "mc_error": mc_error, "pf_error": pf_error,
    }
    for key, value in fields.items():
        if "\n" in value or "\r" in value:
            raise AuthError("unexpected line break in %s" % key)
    write_private(TOKEN_PATH, "".join("%s=%s\n" % kv for kv in fields.items()))


def read_tokens():
    try:
        with open(TOKEN_PATH, encoding="utf-8", errors="replace") as handle:
            return dict(line.rstrip("\n").split("=", 1) for line in handle if "=" in line)
    except FileNotFoundError:
        return {}


def write_private(path, text):
    tmp = "%s.%d.tmp" % (path, os.getpid())
    fd = os.open(tmp, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
    try:
        with os.fdopen(fd, "w", encoding="utf-8") as handle:
            handle.write(text)
            handle.flush()
            os.fsync(handle.fileno())
        os.replace(tmp, path)
    except BaseException:
        remove(tmp)
        raise


def remove(path):
    try:
        os.unlink(path)
    except FileNotFoundError:
        pass


def cache_valid(tokens):
    try:
        exp = int(tokens.get("exp", "0"))
    except ValueError:
        return False
    return exp > time.time() + MIN_VALID_S and bool(tokens.get("xbox"))


def status():
    tokens = read_tokens()
    if not tokens:
        print("not signed in (%s)" % TOKEN_PATH)
        return 1
    try:
        exp = int(tokens.get("exp", "0"))
    except ValueError:
        exp = 0
    print("gamertag: %s" % tokens.get("gamertag", "?"))
    print("expires:  %s (%s)" % (time.strftime("%Y-%m-%d %H:%M", time.localtime(exp)),
                                 "valid" if cache_valid(tokens) else "needs refresh"))
    for key, label in (("xbox", "Xbox Live"), ("mc", "Minecraft"), ("pf", "PlayFab")):
        error = tokens.get(key + "_error")
        print("%-10s %s" % (label + ":", "ok" if tokens.get(key) else "missing" + (" (%s)" % error if error else "")))
    return 0


def main(argv):
    if "--status" in argv:
        return status()
    force = "--force" in argv
    os.makedirs(DATA_DIR, mode=0o700, exist_ok=True)
    try:
        os.chmod(DATA_DIR, 0o700)
    except OSError:
        pass
    with open(LOCK_PATH, "a") as lock:
        fcntl.flock(lock, fcntl.LOCK_EX)
        remove(ERROR_PATH)
        try:
            tokens = read_tokens()
            if cache_valid(tokens) and not force:
                return 0
            msa = msa_refresh(tokens["refresh"]) if tokens.get("refresh") else None
            sign_in(msa or msa_device_code())
        except AuthError as exc:
            message = str(exc)
        except (KeyError, IndexError, TypeError, ValueError) as exc:
            message = "unexpected response from the sign-in service (%s)" % type(exc).__name__
        except Exception as exc:
            message = "sign-in helper error (%s)" % type(exc).__name__
        else:
            return 0
        # The DLL stops waiting when this file appears.
        write_private(ERROR_PATH, message[:400] + "\n")
        log("failed: %s" % message)
        return 1
    return 0


if __name__ == "__main__":
    os.umask(0o077)
    sys.exit(main(sys.argv[1:]))
