/*
  Host test of the HTTPS pieces (HeishaMon/src/common/httpscert.cpp and
  httpstls.cpp), compiled with g++ against mbedtls 3.6 (the version of the
  ESP32 core, see run_tests.py).

  Without arguments: unit tests and a TLS connection over a socket pair,
  both ends stepped in this thread (handshake one step at a time, a large
  answer through httpstls_write() with partial sends).
  --store <host> <ip> <file>: write a generated store file (for the Python
  client test).
  --serve <file> <connections>: a small non-blocking server in the way
  webserver_loop() drives it; prints the port, answers each request with
  a fixed body. run_tests.py connects with Python's ssl (OpenSSL), checks
  the certificate like a browser that trusts it would, and the session
  resumption.
*/
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
#include <algorithm>
#include <string>
#include <vector>

#include "httpscert.h"
#include "httpstls.h"

#include "mbedtls/ctr_drbg.h"
#include "mbedtls/entropy.h"
#include "mbedtls/pk.h"
#include "mbedtls/rsa.h"
#include "mbedtls/ecp.h"
#include "mbedtls/ssl.h"
#include "mbedtls/x509_crt.h"
#include "mbedtls/oid.h"

unsigned long httpstls_platform_millis(void) {
  struct timeval tv;
  gettimeofday(&tv, NULL);
  return (unsigned long)(tv.tv_sec * 1000UL + tv.tv_usec / 1000UL);
}

static int failures = 0;
#define CHECK(cond) do { if(!(cond)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); failures++; } } while(0)

static void *rng(void) {
  void *r = httpstls_random_ctx();
  if(r == NULL) {
    printf("FAIL: random generator\n");
    exit(1);
  }
  return r;
}

static uint32_t ip4(int a, int b, int c, int d) {
  return (uint32_t)a | ((uint32_t)b << 8) | ((uint32_t)c << 16) | ((uint32_t)d << 24);
}

static std::string make_store(const char *host, uint32_t ip, time_t now) {
  httpscert_params_t p;
  httpscert_params(&p, host, &ip, 1, now);
  std::vector<char> out(HTTPSCERT_FILE_MAX);
  int n = httpscert_generate(&p, httpstls_random, rng(), out.data(), out.size());
  if(n <= 0) {
    printf("FAIL: generate %d\n", n);
    exit(1);
  }
  return std::string(out.data(), n);
}

static std::string block(const std::string &s, const char *begin) {
  size_t b = s.find(begin);
  if(b == std::string::npos) return "";
  size_t e = s.find("-----END ", b);
  e = s.find('\n', e);
  return s.substr(b, e - b + 1);
}

/* a self-signed certificate for any key, for the key type tests */
static std::string self_signed(mbedtls_pk_context *key, std::string *key_pem) {
  mbedtls_x509write_cert crt;
  mbedtls_x509write_crt_init(&crt);
  mbedtls_x509write_crt_set_version(&crt, MBEDTLS_X509_CRT_VERSION_3);
  mbedtls_x509write_crt_set_md_alg(&crt, MBEDTLS_MD_SHA256);
  mbedtls_x509write_crt_set_subject_key(&crt, key);
  mbedtls_x509write_crt_set_issuer_key(&crt, key);
  mbedtls_x509write_crt_set_subject_name(&crt, "CN=test");
  mbedtls_x509write_crt_set_issuer_name(&crt, "CN=test");
  unsigned char serial[1] = { 5 };
  mbedtls_x509write_crt_set_serial_raw(&crt, serial, 1);
  mbedtls_x509write_crt_set_validity(&crt, "20240101000000", "20340101000000");
  std::vector<unsigned char> buf(4096);
  int r = mbedtls_x509write_crt_pem(&crt, buf.data(), buf.size(), httpstls_random, rng());
  mbedtls_x509write_crt_free(&crt);
  CHECK(r == 0);
  std::vector<unsigned char> kb(4096);
  CHECK(mbedtls_pk_write_key_pem(key, kb.data(), kb.size()) == 0);
  *key_pem = (const char *)kb.data();
  return (const char *)buf.data();
}

static int upload(const std::string &cert, const std::string &key, std::string *out = NULL) {
  std::vector<char> buf(HTTPSCERT_FILE_MAX);
  int r = httpscert_from_upload(cert.data(), cert.size(), key.data(), key.size(), httpstls_random, rng(), buf.data(), buf.size());
  if(r > 0 && out) *out = std::string(buf.data(), r);
  return r;
}

static void test_params(void) {
  httpscert_params_t p;
  uint32_t ips[] = { ip4(192, 168, 1, 20), 0, ip4(192, 168, 1, 20), ip4(10, 0, 0, 2), ip4(1, 2, 3, 4), ip4(5, 6, 7, 8), ip4(9, 9, 9, 9) };
  httpscert_params(&p, "HeishaMon-Kitchen", ips, 7, 1780000000L);  // 2026-05-28
  CHECK(strcmp(p.host, "heishamon-kitchen") == 0);
  CHECK(strcmp(p.subject, "CN=heishamon-kitchen,O=HeishaMon") == 0);
  CHECK(strcmp(p.dns_local, "heishamon-kitchen.local") == 0);
  CHECK(p.nips == 4);  // 0 and the duplicate skipped, at most four
  CHECK(memcmp(p.ip[0], "\xc0\xa8\x01\x14", 4) == 0);
  CHECK(memcmp(p.ip[1], "\x0a\x00\x00\x02", 4) == 0);
  CHECK(strcmp(p.not_before, "20260527204000") == 0 || strncmp(p.not_before, "20260527", 8) == 0);
  CHECK(strncmp(p.not_after, "20360527", 8) == 0);

  // invalid names fall back
  httpscert_params(&p, "bad name;<x>", NULL, 0, 0);
  CHECK(strcmp(p.host, "heishamon") == 0);
  CHECK(p.nips == 0);
  httpscert_params(&p, "-x", NULL, 0, 0);
  CHECK(strcmp(p.host, "heishamon") == 0);
  httpscert_params(&p, "abcdefghijabcdefghijabcdefghijabc", NULL, 0, 0);  // 33
  CHECK(strcmp(p.host, "heishamon") == 0);
  // the clock is not set: from 2024-01-01
  CHECK(strcmp(p.not_before, "20240101000000") == 0);
  CHECK(strcmp(p.not_after, "20340101000000") == 0);
  // 29 February
  httpscert_params(&p, "h", NULL, 0, 1709251200L); // 2024-03-01 -> start 2024-02-29
  CHECK(strncmp(p.not_before, "20240229", 8) == 0);
  CHECK(strncmp(p.not_after, "20340228", 8) == 0);
  char t[15];
  httpscert_time_string(0, t);
  CHECK(strcmp(t, "19700101000000") == 0);
}

static void test_generate_and_parse(void) {
  uint32_t ips[] = { ip4(192, 168, 1, 20), ip4(10, 1, 2, 3) };
  httpscert_params_t p;
  httpscert_params(&p, "HeishaMon", ips, 2, 1780000000L);
  std::vector<char> out(HTTPSCERT_FILE_MAX);
  int n = httpscert_generate(&p, httpstls_random, rng(), out.data(), out.size());
  CHECK(n > 0);
  std::string store(out.data(), n);
  CHECK(store.rfind("HMTLS1 generated heishamon\n-----BEGIN CERTIFICATE-----", 0) == 0);
  CHECK(store.find("PRIVATE KEY-----") != std::string::npos);
  CHECK(n < 2000);
  // too small an output buffer: an error, no overflow
  std::vector<char> small(300);
  CHECK(httpscert_generate(&p, httpstls_random, rng(), small.data(), small.size()) == HTTPSCERT_ERR_MEMORY);

  mbedtls_x509_crt crt;
  mbedtls_pk_context key;
  mbedtls_x509_crt_init(&crt);
  mbedtls_pk_init(&key);
  httpscert_info_t info;
  CHECK(httpscert_parse(store.data(), store.size(), httpstls_random, rng(), &crt, &key, &info) == HTTPSCERT_OK);
  CHECK(info.generated == 1);
  CHECK(strcmp(info.host, "heishamon") == 0);
  CHECK(strcmp(info.subject, "CN=heishamon, O=HeishaMon") == 0);
  CHECK(strcmp(info.key_type, "EC P-256") == 0);
  CHECK(strncmp(info.not_before, "2026-05-27", 10) == 0);
  CHECK(strncmp(info.not_after, "2036-05-27", 10) == 0);
  CHECK(strlen(info.fingerprint) == 95);
  CHECK(strstr(info.fingerprint, "PRIVATE") == NULL);
  // certificate contents: version 3, not a CA, serverAuth, digitalSignature, the names
  CHECK(crt.version == 3);
  CHECK(mbedtls_x509_crt_check_extended_key_usage(&crt, MBEDTLS_OID_SERVER_AUTH, MBEDTLS_OID_SIZE(MBEDTLS_OID_SERVER_AUTH)) == 0);
  CHECK(mbedtls_x509_crt_check_key_usage(&crt, MBEDTLS_X509_KU_DIGITAL_SIGNATURE) == 0);
  CHECK(crt.serial.len == 16 && (crt.serial.p[0] & 0x80) == 0);
  std::vector<std::string> names;
  int ipcount = 0, ipfound = 0;
  for(const mbedtls_x509_sequence *s = &crt.subject_alt_names; s != NULL && s->buf.p != NULL; s = s->next) {
    mbedtls_x509_subject_alternative_name san;
    if(mbedtls_x509_parse_subject_alt_name(&s->buf, &san) == 0) {
      if(san.type == MBEDTLS_X509_SAN_DNS_NAME) {
        names.push_back(std::string((const char *)san.san.unstructured_name.p, san.san.unstructured_name.len));
      } else if(san.type == MBEDTLS_X509_SAN_IP_ADDRESS) {
        CHECK(san.san.unstructured_name.len == 4);
        if(memcmp(san.san.unstructured_name.p, "\xc0\xa8\x01\x14", 4) == 0) ipfound++;
        ipcount++;
      }
      mbedtls_x509_free_subject_alt_name(&san);
    }
  }
  CHECK(names.size() == 2);
  CHECK(std::find(names.begin(), names.end(), "heishamon") != names.end());
  CHECK(std::find(names.begin(), names.end(), "heishamon.local") != names.end());
  CHECK(ipcount == 2 && ipfound == 1);
  // the self-signature is valid, for the names in it
  uint32_t flags = 0;
  CHECK(mbedtls_x509_crt_verify(&crt, &crt, NULL, "heishamon.local", &flags, NULL, NULL) == 0);
  CHECK(mbedtls_x509_crt_verify(&crt, &crt, NULL, "other.local", &flags, NULL, NULL) != 0);
  mbedtls_x509_crt_free(&crt);
  mbedtls_pk_free(&key);

  // two certificates differ (key and serial are random)
  std::string again(out.data(), httpscert_generate(&p, httpstls_random, rng(), out.data(), out.size()));
  CHECK(again != store);
  httpscert_info_t info2;
  CHECK(httpscert_parse(again.data(), again.size(), httpstls_random, rng(), NULL, NULL, &info2) == HTTPSCERT_OK);
  CHECK(strcmp(info.fingerprint, info2.fingerprint) != 0);

  // hostname change
  CHECK(!httpscert_needs_new(&info, "HeishaMon"));
  CHECK(!httpscert_needs_new(&info, "heishamon"));
  CHECK(httpscert_needs_new(&info, "garage"));
  CHECK(!httpscert_needs_new(&info, "bad name"));  // falls back to "heishamon" like the certificate

  // damaged store files
  CHECK(httpscert_parse("", 0, httpstls_random, rng(), NULL, NULL, NULL) == HTTPSCERT_ERR_FORMAT);
  std::string bad = store;
  bad[0] = 'X';
  CHECK(httpscert_parse(bad.data(), bad.size(), httpstls_random, rng(), NULL, NULL, NULL) == HTTPSCERT_ERR_FORMAT);
  bad = "HMTLS1 other\n" + store.substr(store.find('\n') + 1);
  CHECK(httpscert_parse(bad.data(), bad.size(), httpstls_random, rng(), NULL, NULL, NULL) == HTTPSCERT_ERR_FORMAT);
  bad = store.substr(0, store.find("-----BEGIN EC PRIVATE KEY"));
  CHECK(httpscert_parse(bad.data(), bad.size(), httpstls_random, rng(), NULL, NULL, NULL) == HTTPSCERT_ERR_KEY);
  // a key of another certificate in the file
  std::string other_key = block(again, "-----BEGIN EC PRIVATE KEY");
  bad = store.substr(0, store.find("-----BEGIN EC PRIVATE KEY")) + other_key;
  CHECK(httpscert_parse(bad.data(), bad.size(), httpstls_random, rng(), NULL, NULL, NULL) == HTTPSCERT_ERR_MISMATCH);
  // a broken certificate
  bad = store;
  bad[bad.find("-----BEGIN CERTIFICATE-----") + 100] = '!';
  CHECK(httpscert_parse(bad.data(), bad.size(), httpstls_random, rng(), NULL, NULL, NULL) != HTTPSCERT_OK);
  // the parse stops at the length given (no NUL needed)
  CHECK(httpscert_parse(store.data(), store.size() - 40, httpstls_random, rng(), NULL, NULL, NULL) != HTTPSCERT_OK);
}

static void test_upload(void) {
  std::string a = make_store("a", ip4(10, 0, 0, 1), 1780000000L);
  std::string b = make_store("b", ip4(10, 0, 0, 2), 1780000000L);
  std::string cert_a = block(a, "-----BEGIN CERTIFICATE"), key_a = block(a, "-----BEGIN EC PRIVATE KEY");
  std::string cert_b = block(b, "-----BEGIN CERTIFICATE"), key_b = block(b, "-----BEGIN EC PRIVATE KEY");
  std::string out;
  CHECK(upload(cert_a, key_a, &out) > 0);
  CHECK(out.rfind("HMTLS1 uploaded\n-----BEGIN CERTIFICATE-----", 0) == 0);
  httpscert_info_t info;
  CHECK(httpscert_parse(out.data(), out.size(), httpstls_random, rng(), NULL, NULL, &info) == HTTPSCERT_OK);
  CHECK(info.generated == 0 && !httpscert_needs_new(&info, "anything"));

  CHECK(upload(cert_a, key_b) == HTTPSCERT_ERR_MISMATCH);
  CHECK(upload("", key_a) == HTTPSCERT_ERR_CERT);
  CHECK(upload("hello", key_a) == HTTPSCERT_ERR_CERT);
  CHECK(upload(cert_a, "") == HTTPSCERT_ERR_KEY);
  CHECK(upload(key_a, cert_a) == HTTPSCERT_ERR_CERT);   // swapped fields
  CHECK(upload(cert_a, cert_a) == HTTPSCERT_ERR_KEY);
  // text around the PEM (a pasted file with comments, Windows line ends)
  std::string crlf_cert, crlf_key;
  for(char c : cert_a) { if(c == '\n') crlf_cert += '\r'; crlf_cert += c; }
  for(char c : key_a) { if(c == '\n') crlf_key += '\r'; crlf_key += c; }
  CHECK(upload("subject=CN=a\r\n" + crlf_cert + "trailing", "Bag Attributes\n" + crlf_key) > 0);
  // with the chain: the chain is kept and sent, the first one must match
  std::string with_chain;
  CHECK(upload(cert_a + cert_b, key_a, &with_chain) > 0);
  mbedtls_x509_crt crt;
  mbedtls_x509_crt_init(&crt);
  CHECK(httpscert_parse(with_chain.data(), with_chain.size(), httpstls_random, rng(), &crt, NULL, NULL) == HTTPSCERT_OK);
  CHECK(crt.next != NULL);
  mbedtls_x509_crt_free(&crt);
  CHECK(upload(cert_b + cert_a, key_a) == HTTPSCERT_ERR_MISMATCH);
  // too large
  std::string huge(HTTPSCERT_CERT_MAX + 1, 'A');
  CHECK(upload(huge, key_a) == HTTPSCERT_ERR_SIZE);
  CHECK(upload(cert_a, std::string(HTTPSCERT_KEY_MAX + 1, 'A')) == HTTPSCERT_ERR_SIZE);
  // a key with a password is not accepted (it could not be used without it)
  std::string enc = "-----BEGIN ENCRYPTED PRIVATE KEY-----\nMIGbMFcGCSqGSIb3DQEFDTBKMCkGCSqGSIb3DQEFDDAcBAhLz2qc5m0SuwICCAAw\n-----END ENCRYPTED PRIVATE KEY-----\n";
  CHECK(upload(cert_a, enc) == HTTPSCERT_ERR_KEY);

  // key types: RSA 2048 yes, RSA 1024 and P-384 no
  struct { int rsa_bits; mbedtls_ecp_group_id curve; int expect; } kinds[] = {
    { 2048, MBEDTLS_ECP_DP_NONE, 1 }, { 1024, MBEDTLS_ECP_DP_NONE, HTTPSCERT_ERR_KEYTYPE }, { 0, MBEDTLS_ECP_DP_SECP384R1, HTTPSCERT_ERR_KEYTYPE },
  };
  for(auto &k : kinds) {
    mbedtls_pk_context key;
    mbedtls_pk_init(&key);
    if(k.rsa_bits) {
      CHECK(mbedtls_pk_setup(&key, mbedtls_pk_info_from_type(MBEDTLS_PK_RSA)) == 0);
      CHECK(mbedtls_rsa_gen_key(mbedtls_pk_rsa(key), httpstls_random, rng(), k.rsa_bits, 65537) == 0);
    } else {
      CHECK(mbedtls_pk_setup(&key, mbedtls_pk_info_from_type(MBEDTLS_PK_ECKEY)) == 0);
      CHECK(mbedtls_ecp_gen_key(k.curve, mbedtls_pk_ec(key), httpstls_random, rng()) == 0);
    }
    std::string kp, cp = self_signed(&key, &kp);
    int r = upload(cp, kp);
    CHECK(k.expect == 1 ? r > 0 : r == k.expect);
    mbedtls_pk_free(&key);
  }
  CHECK(strlen(httpscert_error_text(HTTPSCERT_ERR_MISMATCH)) > 0);
}

static void test_admit(void) {
  // the cap
  CHECK(!httpstls_admit(HTTPSTLS_MAX_CONN, 1 << 20, 1 << 22, true));
  // record buffers in PSRAM: only a little internal RAM per connection
  CHECK(httpstls_admit(0, HTTPSTLS_INTERNAL_RESERVE + HTTPSTLS_CONN_INTERNAL_BYTES, 4 << 20, true));
  CHECK(!httpstls_admit(0, HTTPSTLS_INTERNAL_RESERVE + HTTPSTLS_CONN_INTERNAL_BYTES - 1, 4 << 20, true));
  // PSRAM full or not used: everything internal
  CHECK(!httpstls_admit(0, HTTPSTLS_INTERNAL_RESERVE + HTTPSTLS_CONN_INTERNAL_BYTES, 2 * HTTPSTLS_BUFFER_BYTES - 1, true));
  CHECK(!httpstls_admit(0, 60000, 4 << 20, false));
  CHECK(httpstls_admit(2, HTTPSTLS_INTERNAL_RESERVE + HTTPSTLS_CONN_BYTES, 0, false));
  CHECK(!httpstls_admit(1, HTTPSTLS_INTERNAL_RESERVE + HTTPSTLS_CONN_BYTES - 1, 0, false));
  CHECK(HTTPSTLS_MAX_CONN < 5);  // WEBSERVER_MAX_CLIENTS: slots stay for HTTP (hotspot, Home Assistant)
}

/* ---- a TLS connection over a socket pair, both ends in this thread ---- */

struct client_t {
  int fd;
  mbedtls_ssl_context ssl;
  mbedtls_ssl_config conf;
  mbedtls_x509_crt ca;
};

static int cli_send(void *ctx, const unsigned char *buf, size_t len) {
  int n = send(*(int *)ctx, buf, len, MSG_DONTWAIT);
  if(n < 0) return (errno == EAGAIN || errno == EWOULDBLOCK) ? MBEDTLS_ERR_SSL_WANT_WRITE : MBEDTLS_ERR_SSL_INTERNAL_ERROR;
  return n;
}

static int cli_recv(void *ctx, unsigned char *buf, size_t len) {
  int n = recv(*(int *)ctx, buf, len, MSG_DONTWAIT);
  if(n < 0) return (errno == EAGAIN || errno == EWOULDBLOCK) ? MBEDTLS_ERR_SSL_WANT_READ : MBEDTLS_ERR_SSL_INTERNAL_ERROR;
  return n;
}

static void client_init(client_t *c, int fd, const std::string &cert, const char *hostname) {
  c->fd = fd;
  mbedtls_ssl_init(&c->ssl);
  mbedtls_ssl_config_init(&c->conf);
  mbedtls_x509_crt_init(&c->ca);
  CHECK(mbedtls_x509_crt_parse(&c->ca, (const unsigned char *)cert.c_str(), cert.size() + 1) == 0);
  CHECK(mbedtls_ssl_config_defaults(&c->conf, MBEDTLS_SSL_IS_CLIENT, MBEDTLS_SSL_TRANSPORT_STREAM, MBEDTLS_SSL_PRESET_DEFAULT) == 0);
  mbedtls_ssl_conf_rng(&c->conf, httpstls_random, rng());
  mbedtls_ssl_conf_authmode(&c->conf, MBEDTLS_SSL_VERIFY_REQUIRED);
  mbedtls_ssl_conf_ca_chain(&c->conf, &c->ca, NULL);
  CHECK(mbedtls_ssl_setup(&c->ssl, &c->conf) == 0);
  mbedtls_ssl_set_hostname(&c->ssl, hostname);
  mbedtls_ssl_set_bio(&c->ssl, &c->fd, cli_send, cli_recv, NULL);
}

static void client_free(client_t *c) {
  mbedtls_ssl_free(&c->ssl);
  mbedtls_ssl_config_free(&c->conf);
  mbedtls_x509_crt_free(&c->ca);
}

static void test_connection(void) {
  std::string store = make_store("heishamon", ip4(127, 0, 0, 1), 1780000000L);
  CHECK(!httpstls_ready());
  CHECK(httpstls_conn_new(3) == NULL);  // no server yet
  CHECK(httpstls_server_init("HMTLS1 broken\n", 14) != HTTPSCERT_OK);
  CHECK(!httpstls_ready());
  CHECK(httpstls_server_init(store.data(), store.size()) == HTTPSCERT_OK);
  CHECK(httpstls_ready());
  std::string cert = block(store, "-----BEGIN CERTIFICATE");

  int sv[2];
  CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0);
  int small = 4096;
  setsockopt(sv[0], SOL_SOCKET, SO_SNDBUF, &small, sizeof(small));  // the server end fills up: partial sends
  setsockopt(sv[1], SOL_SOCKET, SO_RCVBUF, &small, sizeof(small));
  memset(&httpstls_stats, 0, sizeof(httpstls_stats));
  struct httpstls_conn_t *s = httpstls_conn_new(sv[0]);
  CHECK(s != NULL);
  CHECK(httpstls_stats.active == 1);
  client_t c;
  client_init(&c, sv[1], cert, "heishamon.local");

  // before the handshake nothing is read or written
  unsigned char tmp[64];
  CHECK(httpstls_read(s, tmp, sizeof(tmp)) == 0);
  CHECK(httpstls_write(s, tmp, 1) == 0);

  int steps = 0, cret = 1;
  while(!httpstls_handshake_done(s) && steps < 200) {
    int r = httpstls_handshake(s);  // one step, like webserver_loop()
    CHECK(r != HTTPSTLS_FAIL);
    steps++;
    if(r == HTTPSTLS_FAIL) { printf("server handshake -0x%04x after %d steps\n", -httpstls_stats.last_error, steps); break; }
    if(cret != 0) {
      cret = mbedtls_ssl_handshake(&c.ssl);
      if(!(cret == 0 || cret == MBEDTLS_ERR_SSL_WANT_READ || cret == MBEDTLS_ERR_SSL_WANT_WRITE)) printf("client handshake -0x%04x\n", -cret);
    }
  }
  while(cret != 0 && steps++ < 400) {
    cret = mbedtls_ssl_handshake(&c.ssl);
  }
  CHECK(httpstls_handshake_done(s));
  CHECK(cret == 0);
  CHECK(mbedtls_ssl_get_verify_result(&c.ssl) == 0);  // certificate and name accepted
  CHECK(steps > 3);
  CHECK(httpstls_stats.handshakes == 1);
  CHECK(strstr(mbedtls_ssl_get_ciphersuite(&c.ssl), "ECDHE-ECDSA") != NULL);
  CHECK(strstr(mbedtls_ssl_get_ciphersuite(&c.ssl), "GCM") != NULL || strstr(mbedtls_ssl_get_ciphersuite(&c.ssl), "CHACHA") != NULL);
  CHECK(strcmp(mbedtls_ssl_get_version(&c.ssl), "TLSv1.2") == 0);

  // request
  const char *req = "GET / HTTP/1.1\r\nHost: heishamon.local\r\n\r\n";
  CHECK(mbedtls_ssl_write(&c.ssl, (const unsigned char *)req, strlen(req)) == (int)strlen(req));
  unsigned char rb[2920];
  int got = 0;
  for(int i = 0; i < 100 && got == 0; i++) {
    got = httpstls_read(s, rb, sizeof(rb));
  }
  CHECK(got == (int)strlen(req) && memcmp(rb, req, got) == 0);

  // a large answer through a staging buffer like web_tx_flush(): up to 1500 bytes per call, the
  // same data again after 0, while more is appended at the end
  std::vector<unsigned char> answer(200000);
  for(size_t i = 0; i < answer.size(); i++) answer[i] = (unsigned char)(i * 7 + i / 251);
  size_t sent = 0, received = 0;
  int zeros = 0;
  std::vector<unsigned char> back;
  for(int round = 0; round < 200000 && received < answer.size(); round++) {
    if(sent < answer.size()) {
      size_t attempt = answer.size() - sent < 1500 ? answer.size() - sent : 1500;
      int w = httpstls_write(s, &answer[sent], attempt);
      CHECK(w >= 0);
      if(w == 0) zeros++;
      sent += (w > 0) ? (size_t)w : 0;
    }
    if(round % 3 == 0) {  // a slow reader
      int r = mbedtls_ssl_read(&c.ssl, rb, sizeof(rb));
      if(r > 0) {
        back.insert(back.end(), rb, rb + r);
        received += r;
      }
    }
  }
  CHECK(received == answer.size());
  CHECK(back == answer);
  CHECK(zeros > 0);  // the partial send path ran

  // the peer closes: read reports it
  mbedtls_ssl_close_notify(&c.ssl);
  int rr = 0;
  for(int i = 0; i < 50 && rr == 0; i++) rr = httpstls_read(s, rb, sizeof(rb));
  CHECK(rr == HTTPSTLS_FAIL);
  httpstls_conn_free(s);
  CHECK(httpstls_stats.active == 0);
  client_free(&c);
  close(sv[0]);
  close(sv[1]);

  // a client that does not trust the certificate (another one) fails, the server fails cleanly
  std::string other = make_store("heishamon", ip4(127, 0, 0, 1), 1780000000L);
  CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0);
  s = httpstls_conn_new(sv[0]);
  client_init(&c, sv[1], block(other, "-----BEGIN CERTIFICATE"), "heishamon");
  int sr = HTTPSTLS_AGAIN;
  cret = 1;
  for(int i = 0; i < 400 && sr == HTTPSTLS_AGAIN; i++) {
    sr = httpstls_handshake(s);
    if(cret == MBEDTLS_ERR_SSL_WANT_READ || cret == MBEDTLS_ERR_SSL_WANT_WRITE || cret == 1) {
      cret = mbedtls_ssl_handshake(&c.ssl);
      if(cret != 0 && cret != MBEDTLS_ERR_SSL_WANT_READ && cret != MBEDTLS_ERR_SSL_WANT_WRITE) {
        close(sv[1]);  // the browser gives up
      }
    }
  }
  CHECK(cret != 0);
  CHECK(sr == HTTPSTLS_FAIL);
  uint32_t failed = httpstls_stats.failed;
  httpstls_conn_free(s);
  CHECK(httpstls_stats.failed == failed + 1);
  client_free(&c);
  close(sv[0]);

  // the cap: the fourth connection is refused, nothing allocated
  int fds[HTTPSTLS_MAX_CONN + 1][2];
  struct httpstls_conn_t *cs[HTTPSTLS_MAX_CONN + 1];
  for(int i = 0; i <= HTTPSTLS_MAX_CONN; i++) {
    socketpair(AF_UNIX, SOCK_STREAM, 0, fds[i]);
    cs[i] = httpstls_conn_new(fds[i][0]);
  }
  for(int i = 0; i < HTTPSTLS_MAX_CONN; i++) CHECK(cs[i] != NULL);
  CHECK(cs[HTTPSTLS_MAX_CONN] == NULL);
  CHECK(httpstls_stats.active == HTTPSTLS_MAX_CONN);
  for(int i = 0; i <= HTTPSTLS_MAX_CONN; i++) {
    httpstls_conn_free(cs[i]);  // NULL is fine
    close(fds[i][0]);
    close(fds[i][1]);
  }
  CHECK(httpstls_stats.active == 0);
}

/* ---- server for the Python client ---- */

static int serve(const char *file, int connections) {
  FILE *f = fopen(file, "rb");
  if(!f) return 2;
  std::string store;
  char buf[4096];
  size_t n;
  while((n = fread(buf, 1, sizeof(buf), f)) > 0) store.append(buf, n);
  fclose(f);
  if(httpstls_server_init(store.data(), store.size()) != HTTPSCERT_OK) return 3;

  int ls = socket(AF_INET, SOCK_STREAM, 0);
  int yes = 1;
  setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
  struct sockaddr_in a;
  memset(&a, 0, sizeof(a));
  a.sin_family = AF_INET;
  a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  if(bind(ls, (struct sockaddr *)&a, sizeof(a)) != 0 || listen(ls, 4) != 0) return 4;
  socklen_t al = sizeof(a);
  getsockname(ls, (struct sockaddr *)&a, &al);
  printf("%d\n", ntohs(a.sin_port));
  fflush(stdout);

  std::string body(50000, 'x');
  for(int k = 0; k < connections; k++) {
    int fd = accept(ls, NULL, NULL);
    if(fd < 0) return 5;
    struct httpstls_conn_t *c = httpstls_conn_new(fd);
    if(c == NULL) return 6;
    unsigned long start = httpstls_platform_millis();
    int r = HTTPSTLS_AGAIN;
    while(r == HTTPSTLS_AGAIN && httpstls_platform_millis() - start < 10000) {
      r = httpstls_handshake(c);
      usleep(200);
    }
    if(r != HTTPSTLS_DONE) {
      fprintf(stderr, "handshake failed\n");
      httpstls_conn_free(c);
      close(fd);
      continue;
    }
    std::string req;
    unsigned char rb[2920];
    while(req.find("\r\n\r\n") == std::string::npos && httpstls_platform_millis() - start < 10000) {
      int got = httpstls_read(c, rb, sizeof(rb));
      if(got < 0) break;
      req.append((const char *)rb, got);
      if(got == 0) usleep(200);
    }
    std::string ans = "HTTP/1.1 200 OK\r\nContent-Length: " + std::to_string(body.size()) + "\r\nConnection: close\r\n\r\n" + body;
    size_t sent = 0;
    while(sent < ans.size() && httpstls_platform_millis() - start < 10000) {
      size_t attempt = ans.size() - sent < 1500 ? ans.size() - sent : 1500;
      int w = httpstls_write(c, (const unsigned char *)ans.data() + sent, attempt);
      if(w < 0) break;
      if(w == 0) usleep(200);
      sent += w;
    }
    httpstls_conn_free(c);
    close(fd);
  }
  fprintf(stderr, "handshakes=%u longest_step_ms=%u\n", httpstls_stats.handshakes, httpstls_stats.max_step_ms);
  return 0;
}

int main(int argc, char **argv) {
  setvbuf(stdout, NULL, _IONBF, 0);
  if(argc == 5 && strcmp(argv[1], "--store") == 0) {
    struct in_addr ia;
    inet_aton(argv[3], &ia);
    std::string s = make_store(argv[2], ia.s_addr, time(NULL));
    FILE *f = fopen(argv[4], "wb");
    fwrite(s.data(), 1, s.size(), f);
    fclose(f);
    return 0;
  }
  if(argc == 4 && strcmp(argv[1], "--serve") == 0) {
    return serve(argv[2], atoi(argv[3]));
  }
  test_params();
  test_generate_and_parse();
  test_upload();
  test_admit();
  test_connection();
  if(failures) {
    printf("%d failures\n", failures);
    return 1;
  }
  printf("PASS: https unit tests\n");
  return 0;
}
