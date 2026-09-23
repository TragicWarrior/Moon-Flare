#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE

#include "classic_codec.h"

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

static int g_fail;
static int g_port;
static pid_t g_child = -1;

#define CHECK(cond, msg) do { \
    if (!(cond)) { fprintf(stderr, "FAIL: %s\n", msg); g_fail++; return; } \
} while (0)

static uint16_t be16(const uint8_t *p)
{
    return (uint16_t)(((uint16_t)p[0] << 8) | p[1]);
}

static void put_be16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v >> 8);
    p[1] = (uint8_t)(v & 0xFFu);
}

static int send_all(int fd, const void *buf, size_t len)
{
    const uint8_t *p = buf;
    size_t sent = 0;
    while (sent < len)
    {
        ssize_t n = send(fd, p + sent, len - sent, MSG_NOSIGNAL);
        if (n <= 0)
            return -1;
        sent += (size_t)n;
    }
    return 0;
}

static int recv_all(int fd, void *buf, size_t len)
{
    uint8_t *p = buf;
    size_t got = 0;
    while (got < len)
    {
        ssize_t n = recv(fd, p + got, len - got, 0);
        if (n <= 0)
            return -1;
        got += (size_t)n;
    }
    return 0;
}

static int tcp_connect(int port)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in a;
    if (fd < 0)
        return -1;
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    a.sin_port = htons((uint16_t)port);
    if (connect(fd, (struct sockaddr *)&a, sizeof(a)) != 0)
    {
        close(fd);
        return -1;
    }
    return fd;
}

static int fc3_read(int fd, uint8_t unit, uint16_t start, uint16_t qty,
                    uint16_t *out)
{
    uint8_t req[12];
    uint8_t hdr[6];
    uint8_t body[256];
    uint16_t length;
    uint16_t i;
    static uint16_t tid = 1;

    put_be16(req + 0, tid++);
    put_be16(req + 2, 0);
    put_be16(req + 4, 6);
    req[6] = unit;
    req[7] = 0x03;
    put_be16(req + 8, start);
    put_be16(req + 10, qty);
    if (send_all(fd, req, 12) != 0)
        return -1;
    if (recv_all(fd, hdr, 6) != 0)
        return -1;
    length = be16(hdr + 4);
    if (length < 3 || length > sizeof(body))
        return -1;
    if (recv_all(fd, body, length) != 0)
        return -1;
    if (body[1] != 0x03)
        return -1;
    if (body[2] != (uint8_t)(qty * 2u))
        return -1;
    for (i = 0; i < qty; i++)
        out[i] = be16(body + 3 + (size_t)i * 2u);
    return 0;
}

static void spawn_server(const char *bin)
{
    int sp[2];
    char line[128];
    FILE *fp;
    unsigned port = 0;
    int i;

    if (pipe(sp) != 0)
    {
        perror("pipe");
        exit(1);
    }
    g_child = fork();
    if (g_child < 0)
    {
        perror("fork");
        exit(1);
    }
    if (g_child == 0)
    {
        dup2(sp[1], STDOUT_FILENO);
        close(sp[0]);
        close(sp[1]);
        execl(bin, bin, (char *)NULL);
        _exit(127);
    }
    close(sp[1]);
    fp = fdopen(sp[0], "r");
    if (!fp || !fgets(line, sizeof(line), fp))
    {
        fprintf(stderr, "FAIL: no listen line from fake_classic\n");
        exit(1);
    }
    /* keep fp open so the pipe doesn't SIGPIPE the child; we don't read more */
    if (sscanf(line, "fake_classic: listening on 127.0.0.1:%u", &port) != 1 ||
        port == 0)
    {
        fprintf(stderr, "FAIL: parse listen line: %s", line);
        exit(1);
    }
    g_port = (int)port;
    for (i = 0; i < 50; i++)
    {
        int fd = tcp_connect(g_port);
        if (fd >= 0)
        {
            close(fd);
            return;
        }
        usleep(20000);
    }
    fprintf(stderr, "FAIL: server not accepting\n");
    exit(1);
}

static void stop_server(void)
{
    int i;
    if (g_child <= 0)
        return;
    kill(g_child, SIGTERM);
    for (i = 0; i < 20; i++)
    {
        if (waitpid(g_child, NULL, WNOHANG) == g_child)
            return;
        usleep(20000);
    }
    kill(g_child, SIGKILL);
    waitpid(g_child, NULL, 0);
}

static void test_volts_watts_kwh(void)
{
    int fd = tcp_connect(g_port);
    uint16_t r[1];
    CHECK(fd >= 0, "connect volts");
    CHECK(fc3_read(fd, 10, 4114, 1, r) == 0, "read 4114");
    CHECK(r[0] == 1275, "volts 1275");
    CHECK(fc3_read(fd, 10, 4118, 1, r) == 0, "read 4118");
    CHECK(r[0] == 2500, "watts 2500");
    CHECK(fc3_read(fd, 10, 4117, 1, r) == 0, "read 4117");
    CHECK(r[0] == 34, "kwh 34");
    close(fd);
}

static void test_multi_and_codec(void)
{
    int fd = tcp_connect(g_port);
    uint16_t chunk[125];
    uint16_t wire[4220];
    uint16_t off;
    classic_registers_t input;
    classic_data_t data;
    classic_result_t rc;

    CHECK(fd >= 0, "connect codec");
    memset(wire, 0, sizeof(wire));
    for (off = 0; off < 4220; )
    {
        uint16_t qty = 125;
        if ((uint32_t)off + qty > 4220)
            qty = (uint16_t)(4220 - off);
        CHECK(fc3_read(fd, 10, off, qty, chunk) == 0, "chunk fc3");
        memcpy(wire + off, chunk, (size_t)qty * sizeof(uint16_t));
        off = (uint16_t)(off + qty);
    }
    close(fd);

    input.regs = wire;
    input.reg_count = 4220;
    rc = classic_decode(&input, &data);
    CHECK(rc == CLASSIC_OK, "decode");
    CHECK(data.battery_voltage_v > 127.4f && data.battery_voltage_v < 127.6f,
          "battery_voltage_v");
    CHECK(data.ibatt_raw == -155, "ibatt_raw");
    CHECK(data.watts == 2500, "watts");
    CHECK(data.kwh_today > 3.39f && data.kwh_today < 3.41f, "kwh_today");
    CHECK(data.lifetime_kwh == 0x02000001u, "lifetime_kwh");
    CHECK(data.classic_name == 0x4C43, "type check LC");
    CHECK(data.device_id == 0xCCDDAABBu, "device_id");
}

static void test_unit_ids(void)
{
    int fd;
    uint16_t r[1];
    uint8_t dummy;
    ssize_t n;

    fd = tcp_connect(g_port);
    CHECK(fd >= 0, "connect unit1");
    CHECK(fc3_read(fd, 1, 4118, 1, r) == 0, "unit 1");
    CHECK(r[0] == 2500, "watts unit 1");
    close(fd);

    fd = tcp_connect(g_port);
    CHECK(fd >= 0, "connect unit 99");
    {
        uint8_t req[12];
        put_be16(req + 0, 9);
        put_be16(req + 2, 0);
        put_be16(req + 4, 6);
        req[6] = 99;
        req[7] = 0x03;
        put_be16(req + 8, 4118);
        put_be16(req + 10, 1);
        CHECK(send_all(fd, req, 12) == 0, "send unit 99");
    }
    n = recv(fd, &dummy, 1, 0);
    CHECK(n == 0, "wrong unit closed");
    close(fd);
}

static void test_write_rejected(void)
{
    int fd = tcp_connect(g_port);
    uint8_t req[12];
    uint8_t hdr[6], body[8];
    uint16_t length;

    CHECK(fd >= 0, "connect write");
    put_be16(req + 0, 1);
    put_be16(req + 2, 0);
    put_be16(req + 4, 6);
    req[6] = 10;
    req[7] = 0x06;
    put_be16(req + 8, 4118);
    put_be16(req + 10, 9999);
    CHECK(send_all(fd, req, 12) == 0, "send fc6");
    CHECK(recv_all(fd, hdr, 6) == 0, "exc hdr");
    length = be16(hdr + 4);
    CHECK(length == 3, "exc length");
    CHECK(recv_all(fd, body, 3) == 0, "exc body");
    CHECK(body[1] == 0x86, "fc6 exception");
    CHECK(body[2] == 1, "illegal function");
    close(fd);
}

static void test_identity(void)
{
    int fd = tcp_connect(g_port);
    uint16_t mac[7];
    uint16_t name[1];

    CHECK(fd >= 0, "connect id");
    CHECK(fc3_read(fd, 10, 4105, 7, mac) == 0, "mac+id");
    CHECK(mac[0] == 0x1122 && mac[1] == 0x3344 && mac[2] == 0x5566, "UNIT_MAC");
    CHECK(mac[5] == 0xAABB && mac[6] == 0xCCDD, "UNIT_Device_ID");
    CHECK(fc3_read(fd, 10, 4209, 1, name) == 0, "typecheck");
    CHECK(name[0] == 0x4C43, "0x4C43 LC");
    close(fd);
}

int main(int argc, char **argv)
{
    const char *bin;
    if (argc < 2)
    {
        fprintf(stderr, "usage: %s /path/to/fake_classic\n", argv[0]);
        return 2;
    }
    bin = argv[1];
    spawn_server(bin);
    test_volts_watts_kwh();
    test_multi_and_codec();
    test_unit_ids();
    test_write_rejected();
    test_identity();
    stop_server();
    if (g_fail)
    {
        fprintf(stderr, "%d check(s) failed\n", g_fail);
        return 1;
    }
    printf("fake_classic: ok (port %d)\n", g_port);
    return 0;
}
