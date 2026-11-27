"""Host test of HTTPS for the web interface (ESP32 only).

1. Builds mbedtls 3.6.6, the version of the ESP32 core CI uses (arduino-esp32
   3.3.11, ESP-IDF 5.5), from its release tarball (checked by SHA-256,
   kept in build/), and compiles HeishaMon/src/common/httpscert.cpp and
   httpstls.cpp against it with test_https.cpp: certificate parameters,
   generation, upload checks, admission, a TLS connection stepped one
   handshake step at a time with partial sends.
2. Python's ssl module (OpenSSL) connects to the same code: the generated
   certificate is accepted for the hostname, hostname.local and the IP
   address once it is trusted, and a second connection resumes the session.
3. Static checks of the firmware glue that cannot run on the host (the
   socket loop of the ESP32 web server, the start in setup()).

Run: python3 tests/https/run_tests.py
Offline: put mbedtls-3.6.6.tar.bz2 into tests/https/build/ first.
"""

import hashlib
import os
import re
import shutil
import socket
import ssl
import subprocess
import sys
import tarfile
import urllib.request
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
HERE = Path(__file__).resolve().parent
OUT = HERE / "build"
OUT.mkdir(parents=True, exist_ok=True)
SKETCH = ROOT / "HeishaMon"
COMMON = SKETCH / "src" / "common"

MBEDTLS_VERSION = "3.6.6"
MBEDTLS_SHA256 = "8fb65fae8dcae5840f793c0a334860a411f884cc537ea290ce1c52bb64ca007a"
MBEDTLS_URL = f"https://github.com/Mbed-TLS/mbedtls/releases/download/mbedtls-{MBEDTLS_VERSION}/mbedtls-{MBEDTLS_VERSION}.tar.bz2"

compiler = shutil.which("g++")
if not compiler:
    raise SystemExit("g++ is required")


def mbedtls() -> Path:
    src = OUT / f"mbedtls-{MBEDTLS_VERSION}"
    libs = [src / "library" / n for n in ("libmbedtls.a", "libmbedx509.a", "libmbedcrypto.a")]
    if all(p.exists() for p in libs):
        return src
    tarball = OUT / f"mbedtls-{MBEDTLS_VERSION}.tar.bz2"
    if not tarball.exists():
        print(f"downloading {MBEDTLS_URL}")
        cafile = os.environ.get("SSL_CERT_FILE") or os.environ.get("REQUESTS_CA_BUNDLE")
        ctx = ssl.create_default_context(cafile=cafile) if cafile else ssl.create_default_context()
        with urllib.request.urlopen(MBEDTLS_URL, context=ctx) as r:
            tarball.write_bytes(r.read())
    digest = hashlib.sha256(tarball.read_bytes()).hexdigest()
    if digest != MBEDTLS_SHA256:
        tarball.unlink()
        raise SystemExit(f"FAIL: {tarball.name} has SHA-256 {digest}, expected {MBEDTLS_SHA256}")
    with tarfile.open(tarball) as t:
        t.extractall(OUT, filter="data")
    jobs = str(os.cpu_count() or 2)
    subprocess.run(["make", "-C", str(src / "library"), "-j" + jobs, "libmbedtls.a", "libmbedx509.a", "libmbedcrypto.a"],
                   check=True, stdout=subprocess.DEVNULL)
    return src


src = mbedtls()
executable = OUT / "test_https"
subprocess.run(
    [compiler, "-std=c++17", "-g", "-O1", "-Wall", "-Wextra", "-Wno-unused-parameter", "-Wno-missing-field-initializers",
     "-fsanitize=address,undefined", "-fno-sanitize-recover=undefined", "-DHTTPS_HOST_TEST",
     "-I" + str(COMMON), "-I" + str(src / "include"),
     str(HERE / "test_https.cpp"), str(COMMON / "httpscert.cpp"), str(COMMON / "httpstls.cpp"),
     str(src / "library" / "libmbedtls.a"), str(src / "library" / "libmbedx509.a"), str(src / "library" / "libmbedcrypto.a"),
     "-o", str(executable)],
    check=True,
)
subprocess.run([str(executable)], check=True)

# --- 2. OpenSSL as the browser ---
store = OUT / "store.pem"
subprocess.run([str(executable), "--store", "HeishaMon", "127.0.0.1", str(store)], check=True)
text = store.read_text()
cert = text[text.index("-----BEGIN CERTIFICATE-----"):text.index("-----END CERTIFICATE-----") + 25] + "\n"
(OUT / "cert.pem").write_text(cert)
names = ["heishamon", "heishamon.local", "HeishaMon.local", "127.0.0.1"]
server = subprocess.Popen([str(executable), "--serve", str(store), str(len(names) + 3)], stdout=subprocess.PIPE, text=True)
try:
    port = int(server.stdout.readline())

    trusted = ssl.create_default_context(cafile=str(OUT / "cert.pem"))

    def fetch(name, session=None):
        ctx = trusted
        with socket.create_connection(("127.0.0.1", port), timeout=10) as raw:
            with ctx.wrap_socket(raw, server_hostname=name, session=session) as s:
                s.sendall(f"GET / HTTP/1.1\r\nHost: {name}\r\n\r\n".encode())
                data = b""
                while True:
                    chunk = s.recv(65536)
                    if not chunk:
                        break
                    data += chunk
                return data, s.session, s.session_reused, s.version(), s.cipher()

    first = None
    for name in names:
        data, session, reused, version, cipher = fetch(name)
        if not data.startswith(b"HTTP/1.1 200 OK") or not data.endswith(b"x" * 1000) or len(data.split(b"\r\n\r\n", 1)[1]) != 50000:
            sys.exit(f"FAIL: answer over TLS for {name}")
        if version != "TLSv1.2" or "ECDHE-ECDSA" not in cipher[0]:
            sys.exit(f"FAIL: protocol {version} {cipher}")
        first = first or session
    data, session, reused, version, cipher = fetch("heishamon.local", session=first)
    if not reused:
        sys.exit("FAIL: the second connection did not resume the session (every request would need a full handshake)")
    # a name that is not in the certificate is refused, like a browser does
    try:
        fetch("other.local")
        sys.exit("FAIL: a name outside the certificate was accepted")
    except ssl.SSLCertVerificationError:
        pass
    # untrusted (the first visit of a browser): the handshake itself works
    try:
        ctx = ssl.create_default_context()
        with socket.create_connection(("127.0.0.1", port), timeout=10) as raw:
            ctx.wrap_socket(raw, server_hostname="heishamon")
        sys.exit("FAIL: a self-signed certificate was trusted without being installed")
    except ssl.SSLCertVerificationError:
        pass
    if server.wait(timeout=20) != 0:
        sys.exit("FAIL: the test server failed")
finally:
    if server.poll() is None:
        server.kill()
print("PASS: OpenSSL accepts the certificate for its names and resumes sessions")

# --- 3. firmware glue (static) ---
def need(text, pattern, what):
    if not re.search(pattern, text, re.S):
        sys.exit(f"FAIL: {what}")


ws = (COMMON / "webserver.cpp").read_text()
loop = ws[ws.index("void webserver_loop(void) {"):ws.index("int8_t webserver_start(")]
# a TLS slot never reads the socket through WiFiClient (its buffer would steal TLS records)
need(loop, r"if\(clients\[i\]\.data\.tlsconn != NULL\) \{\s*size = webserver_tls_read\(", "webserver_loop() must read TLS slots through httpstls")
need(ws, r"static web_tx_status_t web_tx_flush\(.*?if\(client->tlsconn != NULL\) \{\s*n = httpstls_write\(", "web_tx_flush() must send TLS slots through httpstls_write()")
need(loop, r"httpstls_handshake\(", "webserver_loop() must step the handshake")
need(loop, r"if\(tls_spent < WEB_TLS_STEP_BUDGET_MS\)", "webserver_loop() must stop making handshake steps in a pass after a long one")
need(loop, r"HTTPSTLS_HANDSHAKE_TIMEOUT_MS", "webserver_loop() must time out slow handshakes")
need(ws, r"request\.tls = client->tls;", "the request must say whether it came over TLS")
need(ws, r"httpstls_conn_free\(", "a TLS slot must be freed")
if ws.count("#include \"httpstls.h\"") != 1 or not re.search(r"#if defined\(ESP32\)\s*\n\s*#include \"httpstls.h\"", ws):
    sys.exit("FAIL: httpstls.h only for the ESP32")
for name in ("httpscert.cpp", "httpstls.cpp"):
    if not (COMMON / name).read_text().split("*/", 1)[1].lstrip().startswith("#if defined(ESP32) || defined(HTTPS_HOST_TEST)"):
        sys.exit(f"FAIL: {name} must be compiled only for the ESP32 (the ESP8266 has no mbedtls)")

ino = (SKETCH / "HeishaMon.ino").read_text()
need(ino, r"#ifdef ESP32\s*\n\s*httpsSetup\(\);", "setup() must start HTTPS only on the ESP32")
hw = (SKETCH / "httpsweb.cpp").read_text()
if not hw.split("*/", 1)[1].lstrip().startswith("#include"):
    sys.exit("FAIL: httpsweb.cpp header")
need(hw, r"void httpsSetup\(void\) \{.*?if \(!heishamonSettings\.https\) \{.*?return;.*?httpstls_use_calloc\(\);", "httpsSetup(): the allocator changes only with HTTPS on")
need(hw, r"open\(HTTPS_FILE_TMP, \"w\"\).*?remove\(HTTPS_FILE\);\s*ok = LittleFS\.rename\(HTTPS_FILE_TMP, HTTPS_FILE\)", "the store file is written to a temporary file and renamed")
need(hw, r"httpscert_from_upload\(", "an upload is checked before it is stored")
# the private key never goes to the browser: the page and /getsettings only use the info struct
wf = (SKETCH / "webfunctions.cpp").read_text()
for text, fn in ((wf, "int handleHttps("), (hw, "void httpsSettingsJson(")):
    start = text.index(fn)
    body = text[start:text.index("\n}\n", start)]
    if "PRIVATE" in body or "HTTPS_FILE" in body or "readStore" in body or "LittleFS" in body:
        sys.exit(f"FAIL: {fn} reads the store file (the private key)")
need(wf, r"int getSettings\(.*?httpsSettingsJson\(jsonDoc\)", "/getsettings shows the certificate information")
need(ino, r"#ifdef ESP32\s*\n\s*httpsLoop\(\);", "loop() must run httpsLoop() on the ESP32")
need(ino, r"webauth_env_t env = \{ \(uint32_t\)client->client->remoteIP\(\), hosts, httpsRunning\(\), heishamonSettings\.http_mode, httpsClientOnHotspot\(client\) \};",
     "webserver_cb() must give webauth_request() the HTTPS state, the HTTP mode and the hotspot")
need(ino, r"#else\s*\n\s*webauth_env_t env = \{ \(uint32_t\)client->client->remoteIP\(\), hosts, 0, 0, 0 \};", "ESP8266: no HTTPS in the decision")
# the hotspot is the local address of the socket, nothing the client sends
need(hw, r"bool httpsClientOnHotspot\(struct webserver_t \*client\) \{.*?IPAddress ap = WiFi\.softAPIP\(\);.*?client->client->localIP\(\) == ap && remote\[0\] == ap\[0\]",
     "hotspot from the local address of the socket and a client in the hotspot subnet")
need(hw, r"static void requestReload\(void\) \{.*?webserver_tls_pause\(true\);", "a new certificate keeps the port open (HTTP mode stays in force)")
if re.search(r"requestReload\(void\) \{[^}]*state\.running = false", hw):
    sys.exit("FAIL: a reload must not switch the HTTP policy off")
js = (SKETCH / "htmlcode.h").read_text()
if "'ws://'+location.host" in js:
    sys.exit("FAIL: the page opens ws:// also when it was loaded over HTTPS")
print("PASS: firmware glue of HTTPS")
print("PASS: https host tests")
