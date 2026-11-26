/*
  Host test of the web login (HeishaMon/src/common/webauth.cpp).

  The real webauth.cpp and webserver.cpp are compiled with g++ (see
  run_tests.py). Unit tests cover the building blocks (password hash
  against hashlib vectors, sessions, lockout, Host and Origin checks);
  the end-to-end tests feed raw HTTP requests through the real webserver
  into a callback that does what webserver_cb() in HeishaMon.ino does for
  the decision (webauth_request) and for the login routes, including the
  bypass attempts. Only the HTML pages are not the real ones.
*/
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <limits.h>
#include <functional>
#include <set>
#include <string>
#include <vector>

#include "webserver.h"
#include "webauth.h"
#include "vectors.h"

static unsigned long now_ms = 1000;
unsigned long millis(void) { return ++now_ms; }
void log_message(char *) {}

/* ---- SHA-256 for the host (FIPS 180-4), stands in for BearSSL / mbedtls ---- */

struct sha256_t {
  uint32_t h[8];
  uint8_t buf[64];
  uint64_t len;
  size_t used;
};

static const uint32_t K[64] = {
  0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01, 0x243185be,
  0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa,
  0x5cb0a9dc, 0x76f988da, 0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967, 0x27b70a85,
  0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3,
  0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070, 0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f,
  0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2 };

static uint32_t ror(uint32_t x, int n) { return (x >> n) | (x << (32 - n)); }

static void sha_block(sha256_t *c, const uint8_t *p) {
  uint32_t w[64];
  for(int i = 0; i < 16; i++) {
    w[i] = (uint32_t)p[4 * i] << 24 | (uint32_t)p[4 * i + 1] << 16 | (uint32_t)p[4 * i + 2] << 8 | p[4 * i + 3];
  }
  for(int i = 16; i < 64; i++) {
    uint32_t s0 = ror(w[i - 15], 7) ^ ror(w[i - 15], 18) ^ (w[i - 15] >> 3);
    uint32_t s1 = ror(w[i - 2], 17) ^ ror(w[i - 2], 19) ^ (w[i - 2] >> 10);
    w[i] = w[i - 16] + s0 + w[i - 7] + s1;
  }
  uint32_t a = c->h[0], b = c->h[1], cc = c->h[2], d = c->h[3], e = c->h[4], f = c->h[5], g = c->h[6], h = c->h[7];
  for(int i = 0; i < 64; i++) {
    uint32_t t1 = h + (ror(e, 6) ^ ror(e, 11) ^ ror(e, 25)) + ((e & f) ^ (~e & g)) + K[i] + w[i];
    uint32_t t2 = (ror(a, 2) ^ ror(a, 13) ^ ror(a, 22)) + ((a & b) ^ (a & cc) ^ (b & cc));
    h = g; g = f; f = e; e = d + t1; d = cc; cc = b; b = a; a = t1 + t2;
  }
  c->h[0] += a; c->h[1] += b; c->h[2] += cc; c->h[3] += d; c->h[4] += e; c->h[5] += f; c->h[6] += g; c->h[7] += h;
}

static void sha_init(sha256_t *c) {
  static const uint32_t iv[8] = { 0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19 };
  memcpy(c->h, iv, sizeof(iv));
  c->len = 0;
  c->used = 0;
}

static void sha_update(sha256_t *c, const uint8_t *p, size_t n) {
  c->len += n;
  while(n > 0) {
    size_t take = 64 - c->used < n ? 64 - c->used : n;
    memcpy(c->buf + c->used, p, take);
    c->used += take;
    p += take;
    n -= take;
    if(c->used == 64) {
      sha_block(c, c->buf);
      c->used = 0;
    }
  }
}

static void sha_final(sha256_t *c, uint8_t out[32]) {
  uint64_t bits = c->len * 8;
  uint8_t pad = 0x80;
  sha_update(c, &pad, 1);
  pad = 0;
  while(c->used != 56) {
    sha_update(c, &pad, 1);
  }
  uint8_t l[8];
  for(int i = 0; i < 8; i++) {
    l[i] = (uint8_t)(bits >> (56 - 8 * i));
  }
  sha_update(c, l, 8);
  for(int i = 0; i < 8; i++) {
    out[4 * i] = c->h[i] >> 24; out[4 * i + 1] = c->h[i] >> 16; out[4 * i + 2] = c->h[i] >> 8; out[4 * i + 3] = c->h[i];
  }
}

/* ---- platform of webauth.cpp ---- */

static unsigned long auth_now = 5000;  // clock of the login, moved by the tests
static uint64_t rng = 0x9e3779b97f4a7c15ULL;
static int saves = 0, yields = 0, closes = 0;

void webauth_platform_random(uint8_t *buf, size_t len) {
  for(size_t i = 0; i < len; i++) {
    rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17;
    buf[i] = (uint8_t)(rng >> 24);
  }
}
void webauth_platform_sha256(const uint8_t *a, size_t alen, const uint8_t *b, size_t blen, uint8_t out[32]) {
  sha256_t c;
  sha_init(&c);
  sha_update(&c, a, alen);
  if(blen > 0) {
    sha_update(&c, b, blen);
  }
  sha_final(&c, out);
}
void webauth_platform_yield(void) { yields++; }
unsigned long webauth_platform_millis(void) { return auth_now; }
void webauth_platform_save(const webauth_store_t *) { saves++; }
void webauth_platform_close_others(struct webserver_t *keep) {
  closes++;
  webserver_close_others(keep);
}

/* ---- checks ---- */

static int checks = 0, failed = 0;

#define CHECK(cond, ...) do { \
  checks++; \
  if(!(cond)) { \
    failed++; \
    printf("FAIL %s:%d: %s\n  ", __FILE__, __LINE__, #cond); \
    printf(__VA_ARGS__); \
    printf("\n"); \
  } \
} while(0)

static bool contains(const std::string &s, const std::string &part) { return s.find(part) != std::string::npos; }

static std::string hex(const uint8_t *p, size_t n) {
  std::string s;
  char t[3];
  for(size_t i = 0; i < n; i++) {
    snprintf(t, sizeof(t), "%02x", p[i]);
    s += t;
  }
  return s;
}

/* ---- fake client (as tests/webserver) ---- */

static std::vector<std::string> pieces;
static size_t piece = 0, piece_at = 0;
static std::string output;

static int f_write(const unsigned char *b, int n) { output.append((const char *)b, n); return n; }
static int f_write_P(const char *b, int n) { output.append(b, n); return n; }
static int f_available() { return piece < pieces.size(); }
static int f_connected() { return 1; }
static void f_stop() {}
static int f_read(uint8_t *b, int n) {
  std::string &p = pieces[piece];
  int l = (int)(p.size() - piece_at) < n ? (int)(p.size() - piece_at) : n;
  memcpy(b, p.data() + piece_at, l);
  piece_at += l;
  if(piece_at >= p.size()) {
    piece++;
    piece_at = 0;
  }
  return l;
}
static WiFiClient fake = { f_write, f_write_P, f_available, f_connected, f_stop, f_read };

static int s_write(const unsigned char *, int n) { return n; }
static int s_write_P(const char *, int n) { return n; }
static int s_available() { return 0; }
static WiFiClient idle_client = { s_write, s_write_P, s_available, f_connected, f_stop, f_read };

/* ---- the callback: the decision part of webserver_cb() ---- */

static const char *DEVICE_HOSTS = "192.168.1.5,192.168.4.1,heishamon,heishamon.local";
static uint32_t client_ip = 0x0501a8c0;
static std::string events;

static int8_t callback(struct webserver_t *client, void *data) {
  bool other = (client != &clients[0].data);
  switch(client->step) {
    case WEBSERVER_CLIENT_REQUEST: {
      webauth_env_t env = { client_ip, DEVICE_HOSTS };
      int8_t d = webauth_request(client, (struct webserver_request_t *)data, &env);
      char tmp[48];
      snprintf(tmp, sizeof(tmp), "%s r=%d;", d == WEBSERVER_ACCEPT ? "ACCEPT" : d == WEBSERVER_REJECT ? "REJECT" : "CLOSE-REQ", client->route);
      events += tmp;
      return d;
    }
    case WEBSERVER_CLIENT_ARGS: {
      struct arguments_t *a = (struct arguments_t *)data;
      switch(client->route) {
        case WEBAUTH_ROUTE_LOGIN_PAGE: case WEBAUTH_ROUTE_LOGIN_POST: case WEBAUTH_ROUTE_SECURITY_PAGE: case WEBAUTH_ROUTE_SECURITY_POST:
          return webauth_args(client, a);
      }
      events += "ARG " + std::string((const char *)a->name) + ";";
      return 0;
    }
    case WEBSERVER_CLIENT_CREATE_HEADER: {
      return webauth_create_header(client, (struct header_t *)data);
    }
    case WEBSERVER_CLIENT_WRITE: {
      if(client->content == 0) {
        events += "WRITE;";
      }
      switch(client->route) {
        case WEBAUTH_ROUTE_LOGIN_POST: case WEBAUTH_ROUTE_LOGOUT: case WEBAUTH_ROUTE_SECURITY_POST:
        case WEBAUTH_ROUTE_TO_LOGIN: case WEBAUTH_ROUTE_UNAUTHORIZED: case WEBAUTH_ROUTE_FORBIDDEN: case WEBAUTH_ROUTE_TOO_MANY: {
          if(webauth_write_answer(client) == 1) {
            std::string k = std::string("KEY ") + webauth_form_apikey(client);
            webserver_send(client, 200, (char *)"text/plain", 0);
            webserver_send_content(client, (char *)k.c_str(), k.size());
            webauth_close(client);
          }
          return 0;
        }
      }
      if(client->content == 0) {
        char tmp[48];
        int n = snprintf(tmp, sizeof(tmp), "ok route=%d m=%d", client->route, webauth_query_message(client));
        webserver_send(client, 200, (char *)"text/plain", 0);
        webserver_send_content(client, tmp, n);
      }
      return 0;
    }
    case WEBSERVER_CLIENT_CLOSE: {
      events += other ? "CLOSE-OTHER;" : "CLOSE;";
      webauth_close(client);
      client->userdata = NULL;
      return 0;
    }
  }
  return 0;
}

struct result_t {
  std::string events;
  std::string output;
  int status;
  uint8_t websocket;
  std::string header(const std::string &name) const {
    size_t at = output.find("\r\n" + name + ": ");
    if(at == std::string::npos) {
      return "";
    }
    at += 4 + name.size();
    return output.substr(at, output.find("\r\n", at) - at);
  }
  std::string cookie() const {  // value of the session cookie that was set
    std::string c = header("Set-Cookie");
    std::string pre = std::string(WEBAUTH_COOKIE_NAME) + "=";
    if(c.compare(0, pre.size(), pre) != 0) {
      return "";
    }
    return c.substr(pre.size(), c.find(';') - pre.size());
  }
  std::string body() const {
    size_t at = output.find("\r\n\r\n");
    return at == std::string::npos ? "" : output.substr(at + 4);
  }
};

static result_t run(const std::vector<std::string> &p) {
  pieces = p;
  piece = 0;
  piece_at = 0;
  output.clear();
  events.clear();
  uint8_t ws = 0;
  struct webserver_t *c = &clients[0].data;
  webserver_reset_client(c);
  c->client = &fake;
  c->callback = callback;
  c->step = WEBSERVER_CLIENT_CONNECTING;
  c->lastseen = millis();
  int idle = 0;
  for(int i = 0; i < 100000 && c->step != 0 && idle < 20; i++) {
    webserver_loop();
    if(c->is_websocket) {
      ws = 1;
    }
    if(piece >= pieces.size()) {
      idle++;
    }
  }
  if(c->step != 0) {  // an open websocket: close it like the device would
    c->step = WEBSERVER_CLIENT_CLOSE;
    webserver_loop();
  }
  result_t r;
  r.events = events;
  r.output = output;
  r.status = output.size() > 12 ? atoi(output.substr(9, 3).c_str()) : 0;
  r.websocket = ws;
  return r;
}

static result_t run(const std::string &req) { return run(std::vector<std::string>{ req }); }

/*
 * Whole, split in two at every offset and byte by byte: the same result.
 * prepare() puts the login state back before every run.
 */
static result_t run_split(const std::string &name, const std::string &req, std::function<void()> prepare) {
  prepare();
  result_t whole = run(req);
  int bad = 0;
  for(size_t s = 1; s < req.size(); s++) {
    prepare();
    result_t r = run(std::vector<std::string>{ req.substr(0, s), req.substr(s) });
    if(r.events != whole.events || r.output != whole.output) {
      if(bad++ == 0) {
        printf("  %s split at %zu:\n    %s\n  whole:\n    %s\n", name.c_str(), s, r.events.c_str(), whole.events.c_str());
      }
    }
  }
  std::vector<std::string> bytes;
  for(char ch : req) {
    bytes.push_back(std::string(1, ch));
  }
  prepare();
  result_t r = run(bytes);
  if(r.events != whole.events || r.output != whole.output) {
    bad++;
    printf("  %s byte by byte:\n    %s\n", name.c_str(), r.events.c_str());
  }
  CHECK(bad == 0, "%s: %d splits differ", name.c_str(), bad);
  prepare();
  return run(req);
}

/* ---- requests ---- */

static const std::string HOST = "192.168.1.5";
static const std::string SAME = "http://192.168.1.5";

static std::string get(const std::string &path, const std::string &headers = "", const std::string &host = HOST) {
  return "GET " + path + " HTTP/1.1\r\n" + (host.empty() ? "" : "Host: " + host + "\r\n") + headers + "\r\n";
}

static std::string post(const std::string &path, const std::string &body, const std::string &headers = "", const std::string &host = HOST) {
  return "POST " + path + " HTTP/1.1\r\n" + (host.empty() ? "" : "Host: " + host + "\r\n") + headers +
         "Content-Type: application/x-www-form-urlencoded\r\nContent-Length: " + std::to_string(body.size()) + "\r\n\r\n" + body;
}

static std::string origin(const std::string &o = SAME) { return "Origin: " + o + "\r\n"; }
static std::string cookie_hdr(const std::string &token) { return "Cookie: " + std::string(WEBAUTH_COOKIE_NAME) + "=" + token + "\r\n"; }
static std::string bearer(const std::string &key) { return "Authorization: Bearer " + key + "\r\n"; }

static const std::string WS = "Connection: Upgrade\r\nUpgrade: websocket\r\nSec-WebSocket-Version: 13\r\nSec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n";

static const std::string PASSWORD = "correct horse battery";
static const std::string PASSWORD_FORM = "action=password&password=correct+horse+battery&repeat=correct+horse+battery";

static std::string login() {  // a fresh session token
  result_t r = run(post("/login", "password=correct+horse+battery", origin()));
  return r.cookie();
}

static void fresh(bool with_password) {
  auth_now += WEBAUTH_LOCK_FORGET_MS + 1;  // nothing of an earlier test is left
  webauth_reset();
  rng = 0x9e3779b97f4a7c15ULL;
  if(with_password) {
    webauth_set_password(PASSWORD.c_str());
  }
}

static std::string multipart_fw(size_t size, const std::string &headers, const std::string &host = HOST) {
  std::string B = "----WebKitFormBoundaryX3vY7qP0aBcD";
  std::string body = "--" + B + "\r\nContent-Disposition: form-data; name=\"firmware\"; filename=\"fw.bin\"\r\n\r\n" + std::string(size, 'F') + "\r\n--" + B + "--\r\n";
  return "POST /firmware HTTP/1.1\r\n" + (host.empty() ? "" : "Host: " + host + "\r\n") + headers + "Content-Type: multipart/form-data; boundary=" + B +
         "\r\nContent-Length: " + std::to_string(body.size()) + "\r\n\r\n" + body;
}

/* ------------------------------------------------------------------ */

static void test_crypto() {
  for(const auto &v : sha256_vectors) {
    uint8_t out[32];
    webauth_platform_sha256((const uint8_t *)v.msg.data(), v.msg.size(), NULL, 0, out);
    CHECK(hex(out, 32) == v.hex, "sha256 of %zu bytes", v.msg.size());
    // the two-part form (a then b) is the hash of a and b one after the other
    if(v.msg.size() > 3) {
      webauth_platform_sha256((const uint8_t *)v.msg.data(), 3, (const uint8_t *)v.msg.data() + 3, v.msg.size() - 3, out);
      CHECK(hex(out, 32) == v.hex, "sha256 in two parts");
    }
  }
  for(const auto &v : pbkdf2_vectors) {
    uint8_t out[32];
    yields = 0;
    webauth_pbkdf2_sha256(v.password.data(), v.password.size(), (const uint8_t *)v.salt.data(), v.salt.size(), v.iterations, out);
    CHECK(hex(out, 32) == v.hex, "pbkdf2 %s/%u: %s", v.password.c_str(), v.iterations, hex(out, 32).c_str());
    CHECK(v.iterations < 64 || yields >= (int)(v.iterations / 32) - 1, "no yield in %u iterations", v.iterations);
  }
  uint8_t a[16], b[16];
  memset(a, 7, sizeof(a));
  memcpy(b, a, sizeof(b));
  CHECK(webauth_equal(a, b, 16), "equal");
  b[15] ^= 1;
  CHECK(!webauth_equal(a, b, 16), "last byte");
  b[15] ^= 1;
  b[0] ^= 0x80;
  CHECK(!webauth_equal(a, b, 16), "first byte");

  // a stored password: random salt, the iteration count with it, checked against itself
  fresh(false);
  CHECK(!webauth_login_on() && !webauth_check_password(""), "no password");
  CHECK(!webauth_set_password("short"), "too short");
  CHECK(!webauth_set_password(std::string(65, 'x').c_str()), "too long");
  saves = 0;
  CHECK(webauth_set_password(PASSWORD.c_str()) && webauth_login_on() && saves == 1, "set");
  CHECK(webauth_store.iterations == WEBAUTH_ITERATIONS, "iterations stored");
  {
    uint8_t expect[32];
    webauth_pbkdf2_sha256(PASSWORD.data(), PASSWORD.size(), webauth_store.salt, WEBAUTH_SALT_BYTES, webauth_store.iterations, expect);
    CHECK(webauth_equal(expect, webauth_store.password_hash, 32), "stored hash is PBKDF2 of the password");
  }
  std::string salt1 = hex(webauth_store.salt, 16);
  CHECK(webauth_check_password(PASSWORD.c_str()), "right");
  CHECK(!webauth_check_password("correct horse batterY"), "wrong");
  CHECK(!webauth_check_password(""), "empty");
  webauth_set_password(PASSWORD.c_str());
  CHECK(hex(webauth_store.salt, 16) != salt1, "new salt for every password");

  // a stored record is only taken over when it is complete and valid
  webauth_store_t copy = webauth_store;
  webauth_reset();
  CHECK(webauth_load(&copy, sizeof(copy)) == 1 && webauth_check_password(PASSWORD.c_str()), "load");
  CHECK(webauth_load(&copy, sizeof(copy) - 1) == 0, "short file");
  webauth_store_t bad = copy;
  bad.magic ^= 1;
  CHECK(webauth_load(&bad, sizeof(bad)) == 0, "magic");
  bad = copy;
  bad.iterations = 0;
  CHECK(webauth_load(&bad, sizeof(bad)) == 0, "no iterations");

  // API key: only the hash is kept, the key is checked in constant time
  char key[33];
  webauth_new_apikey(key);
  CHECK(strlen(key) == 32 && webauth_check_apikey(key, 32), "key %s", key);
  CHECK(!contains(std::string((const char *)&webauth_store, sizeof(webauth_store)), key), "key itself stored");
  std::string wrong = key;
  wrong[31] = wrong[31] == '0' ? '1' : '0';
  CHECK(!webauth_check_apikey(wrong.c_str(), 32), "wrong key");
  CHECK(!webauth_check_apikey(key, 31), "short key");
  char key2[33];
  webauth_new_apikey(key2);
  CHECK(strcmp(key, key2) != 0 && !webauth_check_apikey(key, 32) && webauth_check_apikey(key2, 32), "regenerate");
  webauth_delete_apikey();
  CHECK(!webauth_check_apikey(key2, 32), "deleted");
}

static void test_sessions() {
  fresh(true);
  uint8_t t[4][WEBAUTH_TOKEN_BYTES];
  for(int i = 0; i < 4; i++) {
    CHECK(webauth_session_new(t[i]) == i, "slot %d", i);
  }
  CHECK(hex(t[0], 16) != hex(t[1], 16), "tokens differ");
  for(int i = 0; i < 4; i++) {
    CHECK(webauth_session_find(t[i]) == i, "find %d", i);
  }
  uint8_t other[WEBAUTH_TOKEN_BYTES];
  memcpy(other, t[0], sizeof(other));
  other[7] ^= 0x10;
  CHECK(webauth_session_find(other) == -1, "a wrong token");
  // the table is full: the one unused longest goes
  auth_now += 10;
  uint8_t t5[WEBAUTH_TOKEN_BYTES];
  CHECK(webauth_session_new(t5) == 0 && webauth_session_find(t[0]) == -1 && webauth_session_find(t[1]) == 1, "evict oldest");
  webauth_session_end(1);
  CHECK(webauth_session_find(t[1]) == -1, "ended");
  webauth_sessions_clear();
  CHECK(webauth_session_find(t[2]) == -1 && webauth_session_find(t5) == -1, "cleared");

  // idle timeout
  int8_t s = webauth_session_new(t[0]);
  auth_now += WEBAUTH_SESSION_IDLE_MS - 1;
  CHECK(webauth_session_find(t[0]) == s, "just before idle timeout");
  auth_now += 1;
  CHECK(webauth_session_find(t[0]) == -1, "idle timeout");

  // cookie parsing: by name, in any position, never a similar name, a cut value or a wrong length
  s = webauth_session_new(t[0]);
  std::string tok = hex(t[0], 16);
  CHECK(webauth_cookie_session(("hmsession=" + tok).c_str()) == s, "alone");
  CHECK(webauth_cookie_session(("theme=dark; hmsession=" + tok + "; lang=de").c_str()) == s, "middle");
  CHECK(webauth_cookie_session(("a=1;hmsession=" + tok).c_str()) == s, "no space");
  CHECK(webauth_cookie_session(("hmsession=\"" + tok + "\"").c_str()) == s, "quoted");
  CHECK(webauth_cookie_session(("hmsession=bad; hmsession=" + tok).c_str()) == s, "second one");
  CHECK(webauth_cookie_session(("xhmsession=" + tok).c_str()) == -1, "longer name");
  CHECK(webauth_cookie_session(("hmsessionx=" + tok).c_str()) == -1, "name with suffix");
  CHECK(webauth_cookie_session(("other=hmsession=" + tok).c_str()) == -1, "in a value");
  CHECK(webauth_cookie_session(("hmsession=" + tok.substr(0, 31)).c_str()) == -1, "short");
  CHECK(webauth_cookie_session(("hmsession=" + tok + "0").c_str()) == -1, "long");
  CHECK(webauth_cookie_session(("hmsession=" + tok.substr(0, 31) + "g").c_str()) == -1, "not hex");
  CHECK(webauth_cookie_session("") == -1 && webauth_cookie_session("hmsession") == -1 && webauth_cookie_session("hmsession=") == -1, "empty");
  std::string upper = tok;
  for(char &c : upper) {
    c = (char)toupper(c);
  }
  CHECK(webauth_cookie_session(("hmsession=" + upper).c_str()) == s, "hex in capitals");

  // the clock wraps (unsigned long): sessions still end on time
  auth_now = ULONG_MAX - 1000;
  s = webauth_session_new(t[1]);
  auth_now += 2000;  // wrapped
  CHECK(webauth_session_find(t[1]) == s, "valid across the wrap");
  auth_now += WEBAUTH_SESSION_IDLE_MS;
  CHECK(webauth_session_find(t[1]) == -1, "idle timeout across the wrap");
  auth_now = 100000;
}

static void test_hosts() {
  fresh(false);
  const char *dev = DEVICE_HOSTS;
  const char *yes[] = { "192.168.1.5", "192.168.1.5:80", "192.168.4.1", "HeishaMon", "heishamon.local", "HEISHAMON.LOCAL:8080" };
  for(const char *h : yes) {
    CHECK(webauth_host_allowed(h, dev), "%s", h);
  }
  const char *no[] = { "", "evil.com", "192.168.1.5.evil.com", "heishamon.evil.com", "evil.heishamon.local", "192.168.1.50",
                       "192.168.1.5:", "192.168.1.5:80x", "[::1]", "[fe80::1]:80", "heishamon.", "0.0.0.0", ":80" };
  for(const char *h : no) {
    CHECK(!webauth_host_allowed(h, dev), "%s", h);
  }
  CHECK(!webauth_host_allowed(NULL, dev), "no Host");
  // empty list entries and 0.0.0.0 (not connected) never match
  CHECK(!webauth_host_allowed("0.0.0.0", "0.0.0.0,,heishamon"), "0.0.0.0");
  CHECK(!webauth_host_allowed("", ",,"), "empty entry");
  // extra names of the settings (reverse proxy)
  CHECK(!webauth_host_allowed("proxy.example.com", dev), "before");
  CHECK(webauth_set_hosts("proxy.example.com, heishamon.fritz.box"), "set hosts");
  CHECK(webauth_host_allowed("proxy.example.com", dev) && webauth_host_allowed("heishamon.fritz.box:80", dev), "extra");
  CHECK(!webauth_host_allowed("example.com", dev), "only whole names");
  CHECK(!webauth_set_hosts("a<script>"), "invalid characters");
  CHECK(!webauth_set_hosts(std::string(WEBAUTH_HOSTS_MAX, 'a').c_str()), "too long");

  // Origin must be http(s):// plus exactly the Host header
  CHECK(webauth_origin_matches("http://192.168.1.5", "192.168.1.5"), "same");
  CHECK(webauth_origin_matches("https://proxy.example.com", "proxy.example.com"), "https");
  CHECK(webauth_origin_matches("HTTP://HeishaMon.local", "heishamon.local"), "case");
  CHECK(webauth_origin_matches("http://192.168.1.5:8080", "192.168.1.5:8080"), "port");
  const char *bad[][2] = {
    { "null", "192.168.1.5" }, { "http://evil.com", "192.168.1.5" }, { "http://192.168.1.5.evil.com", "192.168.1.5" },
    { "http://192.168.1.5:81", "192.168.1.5" }, { "http://192.168.1.5/", "192.168.1.5" }, { "192.168.1.5", "192.168.1.5" },
    { "ftp://192.168.1.5", "192.168.1.5" }, { "http://", "" }, { "http://192.168.1.", "192.168.1.5" },
  };
  for(auto &b : bad) {
    CHECK(!webauth_origin_matches(b[0], b[1]), "%s vs %s", b[0], b[1]);
  }
  CHECK(!webauth_origin_matches("http://192.168.1.5", NULL), "no Host");
}

static void test_lockout() {
  fresh(true);
  uint32_t ip = 0x0a000001;
  // the first failures are free
  for(int i = 0; i < WEBAUTH_LOCK_FREE; i++) {
    CHECK(webauth_lock_check(ip), "try %d", i);
    webauth_lock_failed(ip);
  }
  // then the backoff doubles, up to the cap
  unsigned long expect = 1000;
  for(int i = 0; i < 12; i++) {
    unsigned long wait = webauth_lock_wait_ms(ip);
    CHECK(wait > 0 && wait <= expect, "fail %d: wait %lu, expected <= %lu", i, wait, expect);
    CHECK(!webauth_lock_check(ip), "locked %d", i);
    // a refused try does not count as a failure: the wait stays the same
    CHECK(webauth_lock_wait_ms(ip) == wait, "refused try counted");
    auth_now += wait;
    CHECK(webauth_lock_check(ip), "allowed again after %lu ms", wait);
    webauth_lock_failed(ip);
    expect = expect * 2 > WEBAUTH_LOCK_MAX_MS ? WEBAUTH_LOCK_MAX_MS : expect * 2;
  }
  CHECK(webauth_lock_wait_ms(ip) == WEBAUTH_LOCK_MAX_MS, "cap %lu", webauth_lock_wait_ms(ip));
  // another address is not affected
  CHECK(webauth_lock_check(0x0a000002), "other address");
  // never permanent: after the cap it may try again, a success clears it
  auth_now += WEBAUTH_LOCK_MAX_MS;
  CHECK(webauth_lock_check(ip), "after the cap");
  webauth_lock_success(ip);
  CHECK(webauth_lock_wait_ms(ip) == 0, "cleared");
  // failures are forgotten after a while
  webauth_lock_failed(ip); webauth_lock_failed(ip); webauth_lock_failed(ip); webauth_lock_failed(ip);
  CHECK(webauth_lock_wait_ms(ip) > 0, "locked again");
  auth_now += WEBAUTH_LOCK_FORGET_MS;
  CHECK(webauth_lock_wait_ms(ip) == 0, "forgotten");

  // the global budget: many addresses slow down, but only for a few seconds
  fresh(true);
  for(int i = 0; i < WEBAUTH_BUCKET_SIZE; i++) {
    CHECK(webauth_lock_check(0x0b000000 + i), "budget %d", i);
  }
  CHECK(!webauth_lock_check(0x0c000001), "budget used up");
  unsigned long wait = webauth_lock_wait_ms(0x0c000001);
  CHECK(wait > 0 && wait <= WEBAUTH_BUCKET_REFILL_MS, "wait %lu", wait);
  auth_now += WEBAUTH_BUCKET_REFILL_MS;
  CHECK(webauth_lock_check(0x0c000001), "one more after the refill");
  CHECK(!webauth_lock_check(0x0c000002), "and only one");
  auth_now += WEBAUTH_BUCKET_REFILL_MS * WEBAUTH_BUCKET_SIZE * 3;
  int n = 0;
  while(webauth_lock_check(0x0d000000)) {
    n++;
  }
  CHECK(n == WEBAUTH_BUCKET_SIZE, "refilled to %d", n);

  // bounded table: more addresses than entries push out the oldest
  fresh(true);
  for(uint32_t i = 0; i < WEBAUTH_LOCK_ENTRIES + 1; i++) {
    for(int k = 0; k < WEBAUTH_LOCK_FREE + 1; k++) {
      webauth_lock_failed(0x0e000000 + i);
    }
    auth_now += 1;
  }
  CHECK(webauth_lock_wait_ms(0x0e000000) == 0, "oldest pushed out");
  CHECK(webauth_lock_wait_ms(0x0e000000 + WEBAUTH_LOCK_ENTRIES) > 0, "newest kept");

  // backoff across the wrap of the clock
  fresh(true);
  auth_now = ULONG_MAX - 500;
  for(int k = 0; k < WEBAUTH_LOCK_FREE + 1; k++) {
    webauth_lock_failed(ip);
  }
  wait = webauth_lock_wait_ms(ip);
  CHECK(wait == 2000, "wait %lu", wait);
  auth_now += 1000;  // wrapped
  CHECK(webauth_lock_wait_ms(ip) == 1000 && !webauth_lock_check(ip), "across the wrap");
  auth_now += 1000;
  CHECK(webauth_lock_check(ip), "over across the wrap");
  auth_now = 200000;
}

/* the route of each path, as webserver_cb() had them before this table */
static void test_routes() {
  fresh(false);
  struct { const char *method, *path; int route; } m[] = {
    { "GET", "/", 1 }, { "GET", "/modbus", 200 }, { "GET", "/json", 20 }, { "POST", "/json", 20 }, { "GET", "/reboot", 30 },
    { "GET", "/debug", 40 }, { "GET", "/wifiscan", 50 }, { "GET", "/dallasalias", 60 }, { "GET", "/removedallas", 190 },
    { "GET", "/dallasoffset", 195 }, { "GET", "/togglelog", 2 }, { "GET", "/togglehexdump", 3 }, { "GET", "/connecttest.txt", 80 },
    { "GET", "/ncsi.txt", 80 }, { "GET", "/redirect", 80 }, { "GET", "/fwlink", 80 }, { "GET", "/generate_204", 80 },
    { "GET", "/gen_204", 80 }, { "GET", "/popup", 80 }, { "GET", "/hotspot-detect.html", 81 }, { "GET", "/factoryreset", 90 },
    { "GET", "/gpio", 191 }, { "GET", "/command", 100 }, { "POST", "/command", 100 }, { "GET", "/login", 4 }, { "POST", "/login", 5 },
    { "GET", "/logout", 6 }, { "GET", "/security", 7 }, { "POST", "/security", 8 }, { "POST", "/savesettings", 110 },
    { "POST", "/saverules", 170 }, { "POST", "/cacert", 165 }, { "POST", "/firmware", 150 }, { "GET", "/settings", 120 },
    { "GET", "/getsettings", 130 }, { "GET", "/firmware", 140 }, { "GET", "/rules", 160 }, { "GET", "/cacert", 166 },
    { "GET", "/scandallas", 180 }, { "GET", "/nothing", 0 }, { "GET", "/savesettings", 0 }, { "GET", "/JSON", 0 },
    { "POST", "/settings", -1 }, { "POST", "/nothing", -1 }, { "POST", "/rules", -1 },
  };
  for(auto &x : m) {
    webserver_request_t req;
    memset(&req, 0, sizeof(req));
    req.method = strcmp(x.method, "POST") == 0 ? WEBSERVER_METHOD_POST : WEBSERVER_METHOD_GET;
    req.path = x.path;
    req.query = "";
    CHECK(webauth_find_route(&req) == x.route, "%s %s: %d", x.method, x.path, webauth_find_route(&req));
  }
  webserver_request_t ws;
  memset(&ws, 0, sizeof(ws));
  ws.websocket = 1;
  ws.path = "/ws";
  ws.query = "";
  CHECK(webauth_find_route(&ws) == 210, "websocket");
  // deny by default: a route that is not in the table needs the login and counts as state changing
  CHECK(webauth_route_flags(77) == WEBAUTH_STATE, "unknown route");
  // only these are public
  std::set<int> pub;
  for(uint8_t i = 0; i < webauth_routes_count; i++) {
    if(webauth_routes[i].flags & WEBAUTH_PUBLIC) {
      pub.insert(webauth_routes[i].route);
    }
  }
  CHECK(pub == std::set<int>({ 0, 4, 5, 80, 81, WEBAUTH_ROUTE_TO_LOGIN, WEBAUTH_ROUTE_UNAUTHORIZED, WEBAUTH_ROUTE_FORBIDDEN, WEBAUTH_ROUTE_TOO_MANY }),
        "public routes changed");
  // state changing: every POST route and the GET routes that change something
  int state[] = { 2, 3, 6, 8, 30, 60, 90, 100, 110, 111, 112, 113, 150, 165, 170, 180, 190, 191, 195 };
  for(int r : state) {
    CHECK(webauth_route_flags(r) & WEBAUTH_STATE, "route %d not state changing", r);
  }
  for(uint8_t i = 0; i < webauth_routes_count; i++) {
    if(webauth_routes[i].method == 2) {
      CHECK(webauth_routes[i].flags & WEBAUTH_STATE, "POST route %d", webauth_routes[i].route);
    }
    if(webauth_routes[i].flags & WEBAUTH_ANYHOST) {
      CHECK(webauth_routes[i].route == 80 || webauth_routes[i].route == 81, "any Host only for the captive portal");
    }
  }
}

static void test_login_off() {
  fresh(false);
  result_t r = run(get("/"));
  CHECK(r.status == 200 && contains(r.events, "ACCEPT r=1;"), "%s", r.events.c_str());
  r = run(get("/json", "", ""));  // HTTP/1.0 style, no Host: reading stays as it was
  CHECK(r.status == 200 && contains(r.events, "ACCEPT r=20;"), "%s", r.events.c_str());
  r = run(get("/json", "", "evil.com"));
  CHECK(r.status == 200, "reading with any Host: %s", r.events.c_str());
  // scripts (no Origin) keep working
  r = run(get("/command?SetHeatpump=1"));
  CHECK(r.status == 200 && contains(r.events, "ACCEPT r=100;ARG SetHeatpump;"), "%s", r.events.c_str());
  r = run(post("/savesettings", "a=1"));
  CHECK(r.status == 200 && contains(r.events, "ACCEPT r=110;ARG a;"), "%s", r.events.c_str());
  r = run(get("/reboot"));
  CHECK(r.status == 200 && contains(r.events, "ACCEPT r=30;"), "%s", r.events.c_str());
  // but nothing changes for another site or with a foreign Host (DNS rebinding)
  r = run(get("/command?SetHeatpump=1", "", "evil.com"));
  CHECK(r.status == 403 && !contains(r.events, "ARG"), "%s", r.events.c_str());
  r = run(get("/command?SetHeatpump=1", "", ""));
  CHECK(r.status == 403 && !contains(r.events, "ARG"), "no Host: %s", r.events.c_str());
  r = run(post("/savesettings", "a=1", origin("http://evil.com")));
  CHECK(r.status == 403 && !contains(r.events, "ARG"), "%s", r.events.c_str());
  r = run(post("/savesettings", "a=1", origin()));
  CHECK(r.status == 200, "%s", r.events.c_str());
  r = run(get("/reboot", origin("http://evil.com")));
  CHECK(r.status == 403, "%s", r.events.c_str());
  r = run(multipart_fw(100, origin("http://evil.com")));
  CHECK(r.status == 403 && !contains(r.events, "ARG"), "%s", r.events.c_str());
  // websocket of another site (cross-site websocket hijacking of the log)
  r = run(get("/ws", WS + origin("http://evil.com")));
  CHECK(r.status == 403 && r.websocket == 0, "%s", r.events.c_str());
  r = run(get("/ws", WS + origin()));
  CHECK(r.websocket == 1, "%s", r.events.c_str());
  // the login page says there is no password; a login goes to /security
  r = run(get("/login"));
  CHECK(r.status == 200 && contains(r.events, "ACCEPT r=4;"), "%s", r.events.c_str());
  r = run(post("/login", "password=x", origin()));
  CHECK(r.status == 302 && r.header("Location") == "/security" && r.cookie().empty(), "%s", r.output.c_str());
  r = run(get("/logout"));
  CHECK(r.status == 302 && r.header("Location") == "/login?m=13", "%s", r.output.c_str());
}

static void test_first_password() {
  // drive-by takeover attempts of the first password: none sets it
  struct { const char *name; std::string req; } attacks[] = {
    { "cross-site POST", post("/security", PASSWORD_FORM, origin("http://evil.com")) },
    { "rebinding", post("/security", PASSWORD_FORM, origin("http://evil.com"), "evil.com") },
    { "rebinding without Origin", post("/security", PASSWORD_FORM, "", "evil.com") },
    { "rebinding to a look-alike", post("/security", PASSWORD_FORM, origin("http://192.168.1.5.evil.com"), "192.168.1.5.evil.com") },
    { "Origin null (sandboxed frame)", post("/security", PASSWORD_FORM, origin("null")) },
    { "Origin look-alike", post("/security", PASSWORD_FORM, origin("http://192.168.1.5.evil.com")) },
    { "cut Origin", post("/security", PASSWORD_FORM, origin(SAME + std::string(200, 'x'))) },
    { "no Host", post("/security", PASSWORD_FORM, origin(), "") },
    { "cut Host", post("/security", PASSWORD_FORM, "", HOST + std::string(100, '1')) },
  };
  for(auto &a : attacks) {
    fresh(false);
    result_t r = run(a.req);
    CHECK(r.status == 403 && !webauth_login_on() && r.cookie().empty(), "%s: %s", a.name, r.events.c_str());
  }
  fresh(false);
  result_t r = run(post("/security", "action=password&password=abcdefgh&repeat=abcdefgx", origin()));
  CHECK(r.header("Location") == "/security?m=6" && !webauth_login_on(), "mismatch: %s", r.output.c_str());
  r = run(post("/security", "action=password&password=abc&repeat=abc", origin()));
  CHECK(r.header("Location") == "/security?m=7" && !webauth_login_on(), "short: %s", r.output.c_str());

  // the owner sets it: split anywhere, the same answer, with the session cookie
  r = run_split("first password", post("/security", PASSWORD_FORM, origin()), []() { fresh(false); });
  CHECK(r.status == 302 && r.header("Location") == "/security?m=5" && webauth_login_on(), "%s", r.output.c_str());
  std::string c = r.header("Set-Cookie");
  CHECK(r.cookie().size() == 32 && c == std::string(WEBAUTH_COOKIE_NAME) + "=" + r.cookie() + "; Path=/; HttpOnly; SameSite=Strict", "%s", c.c_str());
  CHECK(r.header("Cache-Control") == "no-store", "%s", r.output.c_str());
  CHECK(webauth_check_password(PASSWORD.c_str()), "stored");
  std::string owner = r.cookie();
  CHECK(run(get("/", cookie_hdr(owner))).status == 200, "owner logged in");
  // a scripted setup without Origin (curl) works too
  fresh(false);
  r = run(post("/security", PASSWORD_FORM));
  CHECK(r.status == 302 && webauth_login_on(), "%s", r.output.c_str());

  // once set, a second "first password" is no longer possible
  fresh(true);
  r = run(post("/security", "action=password&password=attacker1&repeat=attacker1", origin()));
  CHECK(r.status == 401 && webauth_check_password(PASSWORD.c_str()), "%s", r.output.c_str());
}

/*
 * A request accepted while there was no password must not go on after the
 * password was set: the other connections are closed.
 */
static void test_close_others() {
  fresh(false);
  struct webserver_t *o = &clients[1].data;
  webserver_reset_client(o);
  o->client = &idle_client;
  o->callback = callback;
  o->step = WEBSERVER_CLIENT_WEBSOCKET;
  o->lastseen = millis();
  closes = 0;
  result_t r = run(post("/security", PASSWORD_FORM, origin()));
  CHECK(closes == 1 && o->step == 0 && contains(r.events, "CLOSE-OTHER;"), "%s", r.events.c_str());
  webserver_reset_client(o);
}

static void test_login_on() {
  fresh(true);
  // without a login: pages go to /login, the API answers 401
  result_t r = run(get("/"));
  CHECK(r.status == 302 && r.header("Location") == "/login" && !contains(r.events, "ACCEPT"), "%s", r.output.c_str());
  r = run(get("/json"));
  CHECK(r.status == 401 && contains(r.header("WWW-Authenticate"), "Bearer"), "%s", r.output.c_str());
  r = run(get("/command?SetHeatpump=1"));
  CHECK(r.status == 401 && !contains(r.events, "ARG"), "%s", r.events.c_str());
  r = run(get("/reboot"));
  CHECK(r.status == 401 && !contains(r.events, "ACCEPT"), "reboot: %s", r.events.c_str());
  r = run(get("/getsettings"));
  CHECK(r.status == 401, "%s", r.events.c_str());
  r = run(get("/login"));
  CHECK(r.status == 200 && contains(r.events, "ACCEPT r=4;"), "login page is public");
  r = run(get("/nothing"));
  CHECK(r.status == 200 && contains(r.events, "ACCEPT r=0;"), "404 is public");
  // the captive portal checks of a phone on the hotspot come with a foreign Host
  r = run(get("/generate_204", "", "connectivitycheck.gstatic.com"));
  CHECK(contains(r.events, "ACCEPT r=80;"), "%s", r.events.c_str());
  r = run(get("/hotspot-detect.html", "", "captive.apple.com"));
  CHECK(contains(r.events, "ACCEPT r=81;"), "%s", r.events.c_str());
  // with the login on, a foreign Host gets nothing at all (DNS rebinding)
  r = run(get("/login", "", "evil.com"));
  CHECK(r.status == 403, "%s", r.events.c_str());
  r = run(get("/nothing", "", "evil.com"));
  CHECK(r.status == 403, "%s", r.events.c_str());

  // login: wrong, then right; split anywhere
  r = run(post("/login", "password=wrong+password", origin()));
  CHECK(r.status == 302 && r.header("Location") == "/login?m=2" && r.cookie().empty(), "%s", r.output.c_str());
  r = run_split("login", post("/login", "password=correct+horse+battery", origin()), []() { fresh(true); });
  CHECK(r.status == 302 && r.header("Location") == "/" && r.cookie().size() == 32, "%s", r.output.c_str());
  CHECK(r.header("Set-Cookie") == "hmsession=" + r.cookie() + "; Path=/; HttpOnly; SameSite=Strict", "%s", r.output.c_str());
  r = run(post("/login", "password=correct+horse+battery", origin("http://evil.com")));
  CHECK(r.status == 403 && r.cookie().empty(), "login CSRF: %s", r.output.c_str());
  std::string tok = login();
  CHECK(tok.size() == 32, "login");

  r = run(get("/", cookie_hdr(tok)));
  CHECK(r.status == 200 && contains(r.events, "ACCEPT r=1;"), "%s", r.events.c_str());
  r = run(get("/json", cookie_hdr(tok)));
  CHECK(r.status == 200, "%s", r.events.c_str());
  r = run(get("/", "Cookie: theme=dark; " + std::string(WEBAUTH_COOKIE_NAME) + "=" + tok + "; lang=en\r\n"));
  CHECK(r.status == 200, "cookie in another position: %s", r.events.c_str());
  r = run(get("/", "Cookie: x" + std::string(WEBAUTH_COOKIE_NAME) + "=" + tok + "\r\n"));
  CHECK(r.status == 302, "other cookie name: %s", r.events.c_str());
  r = run(get("/", "Cookie: " + std::string(WEBAUTH_COOKIE_NAME) + "=" + tok + "; pad=" + std::string(400, 'p') + "\r\n"));
  CHECK(r.status == 302, "cut Cookie header: %s", r.events.c_str());
  r = run(get("/", cookie_hdr(std::string(32, '0'))));
  CHECK(r.status == 302, "made-up token");
  r = run(get("/", cookie_hdr(tok), "evil.com"));
  CHECK(r.status == 403, "rebinding with a valid cookie: %s", r.events.c_str());
  // a non-Bearer Authorization (old Basic) does not stand in for the cookie
  r = run(get("/json", "Authorization: Basic YWRtaW46aGVpc2hh\r\n"));
  CHECK(r.status == 401, "%s", r.events.c_str());

  // CSRF with the cookie
  r = run(post("/command", "SetHeatpump=1", cookie_hdr(tok)));
  CHECK(r.status == 403 && !contains(r.events, "ARG"), "POST without Origin: %s", r.events.c_str());
  r = run(post("/command", "SetHeatpump=1", cookie_hdr(tok) + origin("http://evil.com")));
  CHECK(r.status == 403, "%s", r.events.c_str());
  r = run(post("/command", "SetHeatpump=1", cookie_hdr(tok) + origin("null")));
  CHECK(r.status == 403, "%s", r.events.c_str());
  r = run(post("/command", "SetHeatpump=1", cookie_hdr(tok) + origin()));
  CHECK(r.status == 200 && contains(r.events, "ARG SetHeatpump;"), "%s", r.events.c_str());
  r = run(get("/command?SetHeatpump=1", cookie_hdr(tok)));
  CHECK(r.status == 200, "GET without Origin (SameSite=Strict): %s", r.events.c_str());
  const char *getstate[] = { "/reboot", "/togglelog", "/togglehexdump", "/factoryreset", "/gpio?pin=5&value=1", "/dallasalias?a=b",
                             "/removedallas?a", "/dallasoffset?a=1", "/scandallas", "/logout" };
  for(const char *p : getstate) {
    r = run(get(p, cookie_hdr(tok) + origin("http://evil.com")));
    CHECK(r.status == 403 && !contains(r.events, "ACCEPT"), "%s: %s", p, r.events.c_str());
  }
  r = run(post("/savesettings", "a=1", cookie_hdr(tok)));
  CHECK(r.status == 403, "%s", r.events.c_str());
  r = run(post("/saverules", "rules=1", cookie_hdr(tok) + origin()));
  CHECK(r.status == 200, "%s", r.events.c_str());

  // logout ends the session: the token is worth nothing afterwards
  r = run(get("/logout", cookie_hdr(tok)));
  CHECK(r.status == 302 && r.header("Location") == "/login?m=13" && contains(r.header("Set-Cookie"), "Max-Age=0"), "%s", r.output.c_str());
  r = run(get("/", cookie_hdr(tok)));
  CHECK(r.status == 302, "token reuse after logout: %s", r.events.c_str());

  // idle timeout and absolute lifetime
  tok = login();
  auth_now += WEBAUTH_SESSION_IDLE_MS - 1000;
  CHECK(run(get("/", cookie_hdr(tok))).status == 200, "before idle timeout");
  auth_now += WEBAUTH_SESSION_IDLE_MS - 1000;
  CHECK(run(get("/", cookie_hdr(tok))).status == 200, "use keeps it alive");
  auth_now += WEBAUTH_SESSION_IDLE_MS;
  CHECK(run(get("/", cookie_hdr(tok))).status == 302, "idle timeout");
  tok = login();
  int ok = 0;
  for(unsigned long t = 0; t < WEBAUTH_SESSION_MAX_MS + WEBAUTH_SESSION_IDLE_MS; t += WEBAUTH_SESSION_IDLE_MS / 2) {
    if(run(get("/", cookie_hdr(tok))).status == 200) {
      ok++;
    }
    auth_now += WEBAUTH_SESSION_IDLE_MS / 2;
  }
  CHECK(ok == (int)(WEBAUTH_SESSION_MAX_MS / (WEBAUTH_SESSION_IDLE_MS / 2)), "absolute lifetime: %d", ok);
  // more logins than sessions: the oldest goes
  std::string t[WEBAUTH_SESSIONS + 1];
  for(int i = 0; i <= WEBAUTH_SESSIONS; i++) {
    t[i] = login();
    auth_now += 10;
  }
  CHECK(run(get("/", cookie_hdr(t[0]))).status == 302 && run(get("/", cookie_hdr(t[WEBAUTH_SESSIONS]))).status == 200, "session table");
}

static void test_websocket() {
  fresh(true);
  std::string tok = login();
  result_t r = run(get("/ws", WS + origin()));
  CHECK(r.status == 401 && r.websocket == 0, "no login: %s", r.events.c_str());
  r = run(get("/ws", WS + cookie_hdr(tok)));
  CHECK(r.status == 403 && r.websocket == 0, "cookie without Origin: %s", r.events.c_str());
  r = run(get("/ws", WS + cookie_hdr(tok) + origin("http://evil.com")));
  CHECK(r.status == 403 && r.websocket == 0, "cross-site websocket hijacking: %s", r.events.c_str());
  r = run(get("/ws", WS + cookie_hdr(tok) + origin()));
  CHECK(r.websocket == 1 && contains(r.output, "101 Switching Protocols"), "%s", r.events.c_str());
  // the API key (no Origin, a program)
  r = run(post("/security", "action=apikey", cookie_hdr(tok) + origin()));
  std::string key = r.body().size() > 4 ? r.body().substr(r.body().find("KEY ") + 4, 32) : "";
  r = run(get("/ws", WS + bearer(key)));
  CHECK(r.websocket == 1, "bearer websocket: %s", r.events.c_str());
  r = run(get("/ws", WS + bearer(std::string(32, 'a'))));
  CHECK(r.status == 401 && r.websocket == 0, "wrong key: %s", r.events.c_str());
}

static std::string key_of(const result_t &r) {  // the key of the "KEY <key>" answer (chunked body)
  size_t at = r.body().find("KEY ");
  return at == std::string::npos ? "" : r.body().substr(at + 4, 32);
}

static std::string request_for(const webauth_route_t &rt, const std::string &headers, const std::string &host = HOST) {
  std::string path = rt.path;
  if(rt.method == 2) {
    return post(path, "x=1", headers, host);
  }
  return get(path, headers, host);
}

static void test_api_key() {
  fresh(true);
  std::string tok = login();
  result_t r = run(post("/security", "action=apikey", cookie_hdr(tok) + origin()));
  CHECK(r.status == 200 && contains(r.body(), "KEY ") && webauth_store.apikey_set, "%s", r.output.c_str());
  std::string key = r.body().substr(r.body().find("KEY ") + 4, 32);
  CHECK(key.size() == 32, "key %s", key.c_str());

  // the key on every route that needs the login, also with an Origin of another site (no cookie: no CSRF)
  int n = 0;
  for(uint8_t i = 0; i < webauth_routes_count; i++) {
    webauth_route_t rt = webauth_routes[i];
    if(rt.path == NULL || (rt.flags & WEBAUTH_PUBLIC) || rt.route == WEBAUTH_ROUTE_LOGOUT) {
      continue;
    }
    char want[24];
    snprintf(want, sizeof(want), "ACCEPT r=%d;", rt.route);
    r = run(request_for(rt, bearer(key)));
    CHECK(contains(r.events, want), "bearer on %s: %s", rt.path, r.events.c_str());
    r = run(request_for(rt, "Authorization: bearer " + key + "\r\n" + origin("http://evil.com")));
    CHECK(contains(r.events, want), "bearer with foreign Origin on %s: %s", rt.path, r.events.c_str());
    r = run(request_for(rt, ""));
    CHECK(!contains(r.events, "ACCEPT") && (r.status == 401 || r.status == 302), "no login on %s: %s", rt.path, r.events.c_str());
    r = run(request_for(rt, bearer(key), "evil.com"));
    CHECK(r.status == 403, "bearer with foreign Host on %s: %s", rt.path, r.events.c_str());
    n++;
  }
  CHECK(n >= 26, "%d routes", n);
  // the Home Assistant integration: GET /reboot and the firmware upload with the key
  r = run(get("/reboot", bearer(key)));
  CHECK(r.status == 200 && contains(r.events, "ACCEPT r=30;"), "%s", r.events.c_str());
  r = run(multipart_fw(8000, bearer(key)));
  CHECK(contains(r.events, "ACCEPT r=150;ARG firmware;"), "%s", r.events.c_str());

  // a cut Authorization header is not a key
  r = run(get("/json", bearer(key + std::string(300, ' ') + "x")));
  CHECK(r.status == 401, "%s", r.events.c_str());
  // a wrong key does not fall back to the cookie
  r = run(get("/json", bearer(std::string(32, 'b')) + cookie_hdr(tok)));
  CHECK(r.status == 401, "%s", r.events.c_str());
  // guessing keys: the address backs off, even the right key waits; others are not affected
  fresh(true);
  tok = login();
  key = key_of(run(post("/security", "action=apikey", cookie_hdr(tok) + origin())));
  client_ip = 0x6401a8c0;
  for(int i = 0; i < WEBAUTH_LOCK_FREE + 1; i++) {
    run(get("/json", bearer(std::string(32, 'c'))));
  }
  r = run(get("/json", bearer(key)));
  CHECK(r.status == 429 && !r.header("Retry-After").empty(), "%s", r.output.c_str());
  client_ip = 0x0501a8c0;
  CHECK(run(get("/json", bearer(key))).status == 200, "other address");
  // a new key: the old one stops working; delete
  std::string key2 = key_of(run(post("/security", "action=apikey", cookie_hdr(tok) + origin())));
  CHECK(key2 != key && run(get("/json", bearer(key))).status == 401 && run(get("/json", bearer(key2))).status == 200, "regenerate");
  r = run(post("/security", "action=apikey_delete", cookie_hdr(tok) + origin()));
  CHECK(r.header("Location") == "/security?m=10" && run(get("/json", bearer(key2))).status == 401, "delete");
  // the key itself is never in the stored record
  CHECK(!contains(std::string((const char *)&webauth_store, sizeof(webauth_store)), key2), "stored");
}

static void test_uploads() {
  fresh(true);
  // a large rejected upload is closed at once, not read to the end
  result_t r = run(multipart_fw(10000, ""));
  CHECK(contains(r.events, "CLOSE-REQ") && r.output.empty() && !contains(r.events, "WRITE"), "%s", r.events.c_str());
  r = run(multipart_fw(10000, "", ""));
  CHECK(r.output.empty(), "%s", r.events.c_str());
  std::string tok = login();
  r = run(multipart_fw(10000, cookie_hdr(tok)));  // cookie but no Origin: CSRF rule, also closed
  CHECK(contains(r.events, "CLOSE-REQ") && r.output.empty(), "%s", r.events.c_str());
  r = run(multipart_fw(10000, cookie_hdr(tok) + origin()));
  CHECK(contains(r.events, "ACCEPT r=150;ARG firmware;"), "%s", r.events.c_str());
  // a small rejected POST gets its answer
  r = run(multipart_fw(100, ""));
  CHECK(r.status == 401, "%s", r.events.c_str());
  // the same while the login is off (another site)
  fresh(false);
  r = run(multipart_fw(10000, origin("http://evil.com")));
  CHECK(contains(r.events, "CLOSE-REQ") && r.output.empty(), "%s", r.events.c_str());
}

static void test_security_forms() {
  fresh(true);
  std::string tok = login();
  // password change needs the current password
  result_t r = run(post("/security", "action=password&current=wrong&password=new+password+1&repeat=new+password+1", cookie_hdr(tok) + origin()));
  CHECK(r.header("Location") == "/security?m=2" && webauth_check_password(PASSWORD.c_str()), "%s", r.output.c_str());
  r = run(post("/security", "action=password&password=new+password+1&repeat=new+password+1", cookie_hdr(tok) + origin()));
  CHECK(r.header("Location") == "/security?m=2" && webauth_check_password(PASSWORD.c_str()), "no current: %s", r.output.c_str());
  r = run(post("/security", "action=password&current=correct+horse+battery&password=new+password+1&repeat=new+password+1", cookie_hdr(tok)));
  CHECK(r.status == 403, "CSRF: %s", r.output.c_str());
  r = run_split("change", post("/security", "action=password&current=correct+horse+battery&password=new+password+1&repeat=new+password+1",
                               cookie_hdr(tok) + origin()),
                [&tok]() {
                  fresh(true);
                  tok = login();
                });
  CHECK(r.header("Location") == "/security?m=5" && webauth_check_password("new password 1") && r.cookie().size() == 32, "%s", r.output.c_str());
  CHECK(run(get("/", cookie_hdr(tok))).status == 302, "old session ended");
  CHECK(run(get("/", cookie_hdr(r.cookie()))).status == 200, "new session");
  CHECK(run(post("/login", "password=correct+horse+battery", origin())).cookie().empty(), "old password");
  CHECK(!run(post("/login", "password=new+password+1", origin())).cookie().empty(), "new password");

  // extra host names
  tok = r.cookie();
  r = run(post("/security", "action=hosts&hosts=proxy.example.com", cookie_hdr(tok) + origin()));
  CHECK(r.header("Location") == "/security?m=11", "%s", r.output.c_str());
  CHECK(run(get("/login", "", "proxy.example.com")).status == 200, "proxy name");
  r = run(post("/security", "action=hosts&hosts=%3Cscript%3E", cookie_hdr(tok) + origin()));
  CHECK(r.header("Location") == "/security?m=12", "%s", r.output.c_str());
  // an overlong field is not cut and used
  r = run(post("/security", "action=password&current=" + std::string(100, 'a') + "&password=new+password+1&repeat=new+password+1", cookie_hdr(tok) + origin()));
  CHECK(r.header("Location") == "/security?m=7", "%s", r.output.c_str());
  r = run(post("/security", "action=nothing", cookie_hdr(tok) + origin()));
  CHECK(r.header("Location") == "/security?m=12", "%s", r.output.c_str());

  // switching the login off needs the current password, then everything is open as before
  r = run(post("/security", "action=remove&current=wrong", cookie_hdr(tok) + origin()));
  CHECK(r.header("Location") == "/security?m=2" && webauth_login_on(), "%s", r.output.c_str());
  r = run(post("/security", "action=remove&current=new+password+1", cookie_hdr(tok) + origin()));
  CHECK(r.header("Location") == "/security?m=8" && !webauth_login_on() && contains(r.header("Set-Cookie"), "Max-Age=0"), "%s", r.output.c_str());
  CHECK(run(get("/")).status == 200, "open again");

  // the query message of the pages
  fresh(false);
  r = run(get("/login?m=13"));
  CHECK(contains(r.output, "ok route=4 m=13"), "%s", r.output.c_str());
}

static void test_login_lockout() {
  fresh(true);
  client_ip = 0x0a01a8c0;
  for(int i = 0; i < WEBAUTH_LOCK_FREE; i++) {
    CHECK(run(post("/login", "password=guess", origin())).header("Location") == "/login?m=2", "guess %d", i);
  }
  result_t r = run(post("/login", "password=correct+horse+battery", origin()));
  CHECK(r.status == 429 && r.header("Retry-After") == "1" && r.cookie().empty(), "%s", r.output.c_str());
  auth_now += 1000;
  r = run(post("/login", "password=guess", origin()));
  CHECK(r.header("Location") == "/login?m=2", "%s", r.output.c_str());
  r = run(post("/login", "password=correct+horse+battery", origin()));
  CHECK(r.status == 429 && r.header("Retry-After") == "2", "%s", r.output.c_str());
  // the owner on another address is not locked out
  client_ip = 0x0b01a8c0;
  CHECK(!run(post("/login", "password=correct+horse+battery", origin())).cookie().empty(), "owner");
  // and after the wait the right password works and clears the counter
  client_ip = 0x0a01a8c0;
  auth_now += 2000;
  CHECK(!run(post("/login", "password=correct+horse+battery", origin())).cookie().empty(), "after the wait");
  CHECK(webauth_lock_wait_ms(client_ip) == 0, "cleared");
  // the current password of /security counts as well
  std::string tok = login();
  for(int i = 0; i < WEBAUTH_LOCK_FREE; i++) {
    r = run(post("/security", "action=remove&current=guess", cookie_hdr(tok) + origin()));
    CHECK(r.header("Location") == "/security?m=2", "%s", r.output.c_str());
  }
  r = run(post("/security", "action=remove&current=correct+horse+battery", cookie_hdr(tok) + origin()));
  CHECK(r.status == 429 && webauth_login_on(), "%s", r.output.c_str());
  client_ip = 0x0501a8c0;
}

int main(int argc, char **argv) {
  if(argc > 1 && strcmp(argv[1], "--routes") == 0) {
    for(uint8_t i = 0; i < webauth_routes_count; i++) {
      printf("%d\n", webauth_routes[i].route);
    }
    return 0;
  }
  webserver_start(80, callback, 0);
  printf("webauth host test, WEBSERVER_BUFFER_SIZE %d\n", WEBSERVER_BUFFER_SIZE);
  webauth_reset();
  test_crypto();
  test_sessions();
  test_hosts();
  test_lockout();
  test_routes();
  test_login_off();
  test_first_password();
  test_close_others();
  test_login_on();
  test_websocket();
  test_api_key();
  test_uploads();
  test_security_forms();
  test_login_lockout();
  printf("%d checks, %d failed\n", checks, failed);
  return failed == 0 ? 0 : 1;
}
