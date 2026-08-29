/*
 *  test_ws.c — Synthetic tests for the WebSocket layer of xdrd.
 *
 *  Uses socketpair() to simulate a TCP connection without network or serial
 *  hardware.  Each test creates a pair of connected fds, exercises the WS
 *  functions, verifies the output, then closes the pair.
 *
 *  Compile via:  make test
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#define OPENSSL_API_COMPAT 0x10100000L
#include <openssl/sha.h>

/* Functions under test (defined in xdrd_lib.o) */
extern int  ws_handshake(int fd);
extern int  ws_read_frame(int fd, char* buf, int maxlen);
extern int  ws_write_frame(int fd, const char* data, int len);
extern int  auth_hash(char* salt, char* password, char* hash);
extern char* auth_salt(void);

/* ── Helpers ──────────────────────────────────────────────────────────────── */

static int sv[2]; /* socketpair: sv[0]=SERVER side, sv[1]=CLIENT side */
#define SERVER sv[0]
#define CLIENT sv[1]

static void mk_pair(void)
{
    if(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) < 0)
    {
        perror("socketpair");
        exit(EXIT_FAILURE);
    }
}

static void close_pair(void)
{
    close(sv[0]);
    close(sv[1]);
}

/* Read exactly n bytes from fd (blocking). Returns 1 on success. */
static int read_exact(int fd, void* buf, size_t n)
{
    size_t total = 0;
    while(total < n)
    {
        ssize_t r = read(fd, (char*)buf + total, n - total);
        if(r <= 0) return 0;
        total += r;
    }
    return 1;
}

/* Build a masked WS text frame in dst.  Returns frame length. */
static int make_masked_frame(unsigned char* dst, const char* payload,
                             int plen, const unsigned char mask[4])
{
    int i, pos = 0;
    dst[pos++] = 0x81;                        /* FIN=1, opcode=text */
    dst[pos++] = (unsigned char)(0x80 | plen); /* MASK=1, length     */
    dst[pos++] = mask[0];
    dst[pos++] = mask[1];
    dst[pos++] = mask[2];
    dst[pos++] = mask[3];
    for(i = 0; i < plen; i++)
        dst[pos++] = (unsigned char)payload[i] ^ mask[i % 4];
    return pos;
}

/* Build a WS control frame (ping/close) with no payload. */
static int make_ctrl_frame(unsigned char* dst, unsigned char opcode)
{
    dst[0] = (unsigned char)(0x80 | opcode); /* FIN=1 */
    dst[1] = 0x00;                           /* MASK=0, len=0 */
    return 2;
}

/* ── Test runner ──────────────────────────────────────────────────────────── */

static int passed = 0, failed = 0;

#define RUN(label, fn) do {                         \
    int _r = (fn)();                                \
    printf("[%s] %s\n", _r ? "PASS" : "FAIL", label); \
    if(_r) passed++; else failed++;                 \
} while(0)

/* ── Test cases ───────────────────────────────────────────────────────────── */

/*
 * Valid HTTP upgrade using the RFC 6455 example key.
 * Expected Sec-WebSocket-Accept: s3pPLMBiTxaQ9kYGzzhZRbK+xOo=
 */
static int test_handshake_valid(void)
{
    mk_pair();

    const char *req =
        "GET / HTTP/1.1\r\n"
        "Host: localhost\r\n"
        "Upgrade: websocket\r\n"
        "Connection: Upgrade\r\n"
        "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
        "Sec-WebSocket-Version: 13\r\n"
        "\r\n";

    write(CLIENT, req, strlen(req));

    int result = ws_handshake(SERVER);
    if(!result) { close_pair(); return 0; }

    char resp[512] = {0};
    ssize_t n = read(CLIENT, resp, sizeof(resp) - 1);
    if(n <= 0) { close_pair(); return 0; }

    int ok = (strstr(resp, "101 Switching Protocols") != NULL)
          && (strstr(resp, "s3pPLMBiTxaQ9kYGzzhZRbK+xOo=") != NULL);

    close_pair();
    return ok;
}

/*
 * HTTP request missing Sec-WebSocket-Key must be rejected (return 0).
 */
static int test_handshake_no_key(void)
{
    mk_pair();

    const char *req =
        "GET / HTTP/1.1\r\n"
        "Host: localhost\r\n"
        "Upgrade: websocket\r\n"
        "Connection: Upgrade\r\n"
        "\r\n";

    write(CLIENT, req, strlen(req));

    int result = ws_handshake(SERVER);
    close_pair();
    return (result == 0);
}

/*
 * Case-insensitive header matching: key in lowercase must still be found.
 */
static int test_handshake_key_case_insensitive(void)
{
    mk_pair();

    const char *req =
        "GET / HTTP/1.1\r\n"
        "Host: localhost\r\n"
        "Upgrade: websocket\r\n"
        "Connection: Upgrade\r\n"
        "sec-websocket-key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
        "Sec-WebSocket-Version: 13\r\n"
        "\r\n";

    write(CLIENT, req, strlen(req));

    int result = ws_handshake(SERVER);
    if(!result) { close_pair(); return 0; }

    char resp[512] = {0};
    ssize_t n = read(CLIENT, resp, sizeof(resp) - 1);
    if(n <= 0) { close_pair(); return 0; }

    int ok = (strstr(resp, "101") != NULL);
    close_pair();
    return ok;
}

/*
 * Write a short text frame with ws_write_frame (unmasked, server→client),
 * read it back with ws_read_frame from the other fd.
 */
static int test_frame_roundtrip_short(void)
{
    mk_pair();

    const char *msg = "T87500\n";
    ws_write_frame(SERVER, msg, (int)strlen(msg));

    char buf[256];
    int n = ws_read_frame(CLIENT, buf, sizeof(buf));

    int ok = (n == (int)strlen(msg)) && (memcmp(buf, msg, n) == 0);
    close_pair();
    return ok;
}

/*
 * Payload >= 126 bytes must use the extended-length encoding (2-byte len).
 * Verify that ws_write_frame + ws_read_frame round-trip a 200-byte payload.
 */
static int test_frame_roundtrip_extended_length(void)
{
    mk_pair();

    char payload[201];
    memset(payload, 'X', 200);
    payload[200] = 0;

    ws_write_frame(SERVER, payload, 200);

    char buf[256];
    int n = ws_read_frame(CLIENT, buf, sizeof(buf));

    int ok = (n == 200) && (memcmp(buf, payload, 200) == 0);
    close_pair();
    return ok;
}

/*
 * Client→server frames are always masked (RFC 6455 §6.1).
 * Craft a masked frame manually and verify ws_read_frame unmasks correctly.
 *
 * Mask: DE AD BE EF
 * Plaintext: "T87500\n"
 */
static int test_frame_masked(void)
{
    mk_pair();

    const char *plaintext = "T87500\n";
    const int plen = 7;
    const unsigned char mask[4] = {0xDE, 0xAD, 0xBE, 0xEF};

    unsigned char frame[64];
    int flen = make_masked_frame(frame, plaintext, plen, mask);

    /* CLIENT sends masked frame → SERVER reads it */
    write(CLIENT, frame, flen);

    char buf[256];
    int n = ws_read_frame(SERVER, buf, sizeof(buf));

    int ok = (n == plen) && (memcmp(buf, plaintext, plen) == 0);
    close_pair();
    return ok;
}

/*
 * A WebSocket close frame (opcode 0x8) must cause ws_read_frame to return 0.
 */
static int test_frame_close(void)
{
    mk_pair();

    unsigned char frame[4];
    int flen = make_ctrl_frame(frame, 0x8);
    write(CLIENT, frame, flen);

    char buf[64];
    int n = ws_read_frame(SERVER, buf, sizeof(buf));

    close_pair();
    return (n == 0);
}

/*
 * A WebSocket ping frame (opcode 0x9) must be answered with a pong (0x0A)
 * transparently, and ws_read_frame must return the *next* data frame.
 *
 * Flow:
 *   CLIENT → SERVER : ping frame
 *   CLIENT → SERVER : text frame "ping_ok\n"
 *   ws_read_frame(SERVER) sends pong, then returns "ping_ok\n"
 *   CLIENT reads pong from SERVER
 */
static int test_frame_ping_pong(void)
{
    mk_pair();

    /* Ping frame with no payload */
    unsigned char ping[4];
    int ping_len = make_ctrl_frame(ping, 0x9);
    write(CLIENT, ping, ping_len);

    /* Follow-up text frame the function must return */
    const char *msg = "ping_ok\n";
    ws_write_frame(CLIENT, msg, (int)strlen(msg));

    char buf[256];
    int n = ws_read_frame(SERVER, buf, sizeof(buf));

    if(n != (int)strlen(msg) || memcmp(buf, msg, n) != 0)
    {
        close_pair();
        return 0;
    }

    /* Verify SERVER sent a pong (opcode 0x0A) back to CLIENT */
    unsigned char pong[4];
    if(!read_exact(CLIENT, pong, 2)) { close_pair(); return 0; }

    int ok = ((pong[0] & 0x0F) == 0x0A); /* opcode = pong */
    close_pair();
    return ok;
}

/*
 * A fragmented text message (FIN=0 first frame + FIN=1 continuation frame)
 * must be reassembled into one message (RFC 6455 §5.4).
 */
static int test_frame_fragmented(void)
{
    mk_pair();

    const unsigned char mask[4] = {0x11, 0x22, 0x33, 0x44};
    unsigned char frame[64];
    const char *p1 = "T87";
    const char *p2 = "500\n";
    int i, pos;

    /* Fragment 1: FIN=0, opcode=text, payload "T87" */
    pos = 0;
    frame[pos++] = 0x01;
    frame[pos++] = 0x80 | 3;
    memcpy(frame + pos, mask, 4); pos += 4;
    for(i = 0; i < 3; i++) frame[pos++] = (unsigned char)p1[i] ^ mask[i % 4];
    write(CLIENT, frame, pos);

    /* Fragment 2: FIN=1, opcode=continuation, payload "500\n" */
    pos = 0;
    frame[pos++] = 0x80;
    frame[pos++] = 0x80 | 4;
    memcpy(frame + pos, mask, 4); pos += 4;
    for(i = 0; i < 4; i++) frame[pos++] = (unsigned char)p2[i] ^ mask[i % 4];
    write(CLIENT, frame, pos);

    char buf[256];
    int n = ws_read_frame(SERVER, buf, sizeof(buf));

    int ok = (n == 7) && (memcmp(buf, "T87500\n", 7) == 0);
    close_pair();
    return ok;
}

/*
 * A control frame declaring a payload > 125 bytes violates RFC 6455 §5.5 and
 * cannot be resynced — ws_read_frame must return -1 (error).
 */
static int test_ctrl_frame_oversized(void)
{
    mk_pair();

    unsigned char frame[4];
    frame[0] = 0x89; /* FIN=1, ping */
    frame[1] = 126;  /* extended length */
    frame[2] = 0x00;
    frame[3] = 200;  /* 200-byte payload */
    write(CLIENT, frame, 4);

    char buf[64];
    int n = ws_read_frame(SERVER, buf, sizeof(buf));

    close_pair();
    return (n == -1);
}

/*
 * An empty text frame is legal (RFC 6455) and must be skipped — not returned
 * as 0, which callers treat as disconnect. The next data frame is returned.
 */
static int test_frame_empty_skipped(void)
{
    mk_pair();

    unsigned char empty[2] = {0x81, 0x00};
    write(CLIENT, empty, 2);

    const char *msg = "next\n";
    ws_write_frame(CLIENT, msg, (int)strlen(msg));

    char buf[64];
    int n = ws_read_frame(SERVER, buf, sizeof(buf));

    int ok = (n == (int)strlen(msg)) && (memcmp(buf, msg, n) == 0);
    close_pair();
    return ok;
}

/*
 * auth_hash must accept the SHA1(salt + password) hex digest.
 */
static int test_auth_hash_correct(void)
{
    const char *salt = "AbCdEfGh01234567";
    const char *pass = "testpassword";

    SHA_CTX ctx;
    unsigned char sha[SHA_DIGEST_LENGTH];
    char sha_str[SHA_DIGEST_LENGTH * 2 + 1];
    int i;

    SHA1_Init(&ctx);
    SHA1_Update(&ctx, (const unsigned char*)salt, strlen(salt));
    SHA1_Update(&ctx, (const unsigned char*)pass,  strlen(pass));
    SHA1_Final(sha, &ctx);

    for(i = 0; i < SHA_DIGEST_LENGTH; i++)
        sprintf(sha_str + i * 2, "%02x", sha[i]);

    return auth_hash((char*)salt, (char*)pass, sha_str);
}

/*
 * auth_hash must reject a wrong password.
 */
static int test_auth_hash_wrong(void)
{
    const char *salt = "AbCdEfGh01234567";
    char bad_hash[41];
    memset(bad_hash, '0', 40);
    bad_hash[40] = 0;

    return (auth_hash((char*)salt, "wrongpassword", bad_hash) == 0);
}

/*
 * Full WS challenge–response auth flow:
 *   1. SERVER sends salt as a WS frame.
 *   2. CLIENT reads salt, computes SHA1(salt+password), sends hash back.
 *   3. SERVER reads hash and validates with auth_hash.
 *
 * Uses ws_write_frame / ws_read_frame for both directions to mirror what
 * ws_server_conn and a real WS client would do.
 */
static int test_auth_flow(void)
{
    mk_pair();

    const char *password = "secret";

    /* — SERVER side: generate and send salt — */
    char *salt = auth_salt();
    if(!salt) { close_pair(); return 0; }

    char salt_frame[32];
    snprintf(salt_frame, sizeof(salt_frame), "%s\n", salt);
    ws_write_frame(SERVER, salt_frame, (int)strlen(salt_frame));

    /* — CLIENT side: receive salt, compute hash, send back — */
    char recv_buf[64];
    int n = ws_read_frame(CLIENT, recv_buf, sizeof(recv_buf) - 1);
    if(n <= 0) { free(salt); close_pair(); return 0; }

    /* Strip trailing newline */
    if(n > 0 && recv_buf[n-1] == '\n') recv_buf[--n] = 0;

    SHA_CTX ctx;
    unsigned char sha[SHA_DIGEST_LENGTH];
    char sha_str[SHA_DIGEST_LENGTH * 2 + 1];
    int i;

    SHA1_Init(&ctx);
    SHA1_Update(&ctx, (unsigned char*)recv_buf, strlen(recv_buf));
    SHA1_Update(&ctx, (const unsigned char*)password, strlen(password));
    SHA1_Final(sha, &ctx);
    for(i = 0; i < SHA_DIGEST_LENGTH; i++)
        sprintf(sha_str + i * 2, "%02x", sha[i]);

    ws_write_frame(CLIENT, sha_str, 40);

    /* — SERVER side: read hash and validate — */
    char hash_recv[64];
    n = ws_read_frame(SERVER, hash_recv, sizeof(hash_recv) - 1);
    if(n < 40) { free(salt); close_pair(); return 0; }
    hash_recv[40] = 0;

    int ok = auth_hash(salt, (char*)password, hash_recv);
    free(salt);
    close_pair();
    return ok;
}

/* ── main ─────────────────────────────────────────────────────────────────── */

int main(void)
{
    printf("xdrd WebSocket synthetic tests\n");
    printf("══════════════════════════════\n");

    RUN("ws_handshake: valid upgrade (RFC 6455 example key)",  test_handshake_valid);
    RUN("ws_handshake: missing Sec-WebSocket-Key rejected",    test_handshake_no_key);
    RUN("ws_handshake: header name is case-insensitive",       test_handshake_key_case_insensitive);
    RUN("ws_frame:    write/read roundtrip (short payload)",   test_frame_roundtrip_short);
    RUN("ws_frame:    write/read roundtrip (200-byte, ext len)", test_frame_roundtrip_extended_length);
    RUN("ws_frame:    masked client frame correctly unmasked", test_frame_masked);
    RUN("ws_frame:    close frame returns 0",                  test_frame_close);
    RUN("ws_frame:    ping elicits pong, next frame returned", test_frame_ping_pong);
    RUN("ws_frame:    fragmented message reassembled",         test_frame_fragmented);
    RUN("ws_frame:    oversized control frame rejected",       test_ctrl_frame_oversized);
    RUN("ws_frame:    empty text frame skipped",               test_frame_empty_skipped);
    RUN("auth_hash:   correct hash accepted",                  test_auth_hash_correct);
    RUN("auth_hash:   wrong hash rejected",                    test_auth_hash_wrong);
    RUN("ws_auth:     full salt/SHA1 challenge-response flow", test_auth_flow);

    printf("══════════════════════════════\n");
    printf("%d passed, %d failed\n", passed, failed);
    return failed ? EXIT_FAILURE : EXIT_SUCCESS;
}
