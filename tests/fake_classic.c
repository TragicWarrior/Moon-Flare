/*
 * Fake Midnite Classic 150 Modbus TCP server.
 * Listen 127.0.0.1:0, print "fake_classic: listening on 127.0.0.1:PORT".
 * FC3 from a canned golden register image. Unit 10 and 1; other units
 * get no reply. Writes (FC6/FC16) → exception 1. No libmodbus.
 */

#define _POSIX_C_SOURCE 200809L

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#define GOLDEN_REG_COUNT 4220

static uint16_t golden_regs[GOLDEN_REG_COUNT];

static uint16_t be16(const uint8_t *p)
{
    return (uint16_t)(((uint16_t)p[0] << 8) | p[1]);
}

static void put_be16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v >> 8);
    p[1] = (uint8_t)(v & 0xFFu);
}

static void init_golden(void)
{
    memset(golden_regs, 0, sizeof(golden_regs));
    golden_regs[4105] = 0x1122;
    golden_regs[4106] = 0x3344;
    golden_regs[4107] = 0x5566;
    golden_regs[4110] = 0xAABB;
    golden_regs[4111] = 0xCCDD;
    golden_regs[4114] = 1275;
    golden_regs[4116] = (uint16_t)(int16_t)(-155);
    golden_regs[4117] = 34;
    golden_regs[4118] = 2500;
    golden_regs[4119] = 0x0305;
    golden_regs[4124] = 452;
    golden_regs[4125] = 0x0001;
    golden_regs[4126] = 0x0200;
    golden_regs[4209] = 0x4C43;
}

static int send_all(int fd, const void *buf, size_t len)
{
    const uint8_t *p = buf;
    size_t sent = 0;
    while (sent < len) {
        ssize_t n = send(fd, p + sent, len - sent, MSG_NOSIGNAL);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        if (n == 0)
            return -1;
        sent += (size_t)n;
    }
    return 0;
}

static int recv_all(int fd, void *buf, size_t len)
{
    uint8_t *p = buf;
    size_t got = 0;
    while (got < len) {
        ssize_t n = recv(fd, p + got, len - got, 0);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        if (n == 0)
            return -1;
        got += (size_t)n;
    }
    return 0;
}

static int send_exception(int fd, uint16_t tid, uint8_t unit, uint8_t func, uint8_t code)
{
    uint8_t resp[9];
    put_be16(resp + 0, tid);
    put_be16(resp + 2, 0);
    put_be16(resp + 4, 3);
    resp[6] = unit;
    resp[7] = (uint8_t)(func | 0x80u);
    resp[8] = code;
    return send_all(fd, resp, 9);
}

static int handle_fc3(int fd, uint16_t tid, uint8_t unit,
                      uint16_t start, uint16_t qty)
{
    uint8_t resp[9 + 250];
    uint16_t i;
    uint16_t mbap_len;
    size_t nbytes;

    if (qty == 0 || qty > 125)
        return send_exception(fd, tid, unit, 0x03, 3);
    if ((uint32_t)start + qty > GOLDEN_REG_COUNT)
        return send_exception(fd, tid, unit, 0x03, 2);

    nbytes = (size_t)qty * 2u;
    mbap_len = (uint16_t)(3u + nbytes); /* unit + fc + bytecount + data */
    put_be16(resp + 0, tid);
    put_be16(resp + 2, 0);
    put_be16(resp + 4, mbap_len);
    resp[6] = unit;
    resp[7] = 0x03;
    resp[8] = (uint8_t)nbytes;
    for (i = 0; i < qty; i++)
        put_be16(resp + 9 + (size_t)i * 2u, golden_regs[start + i]);
    return send_all(fd, resp, 9u + nbytes);
}

/* One TCP session: keep serving until the peer closes. */
static void handle_client(int fd)
{
    for (;;) {
        uint8_t hdr[6];
        uint8_t body[256];
        uint16_t tid, proto, length;
        uint8_t unit, fc;

        if (recv_all(fd, hdr, 6) != 0)
            break;
        tid = be16(hdr + 0);
        proto = be16(hdr + 2);
        length = be16(hdr + 4);
        if (proto != 0 || length < 2 || length > sizeof(body))
            break;
        if (recv_all(fd, body, length) != 0)
            break;
        unit = body[0];
        fc = body[1];
        if (unit != 10 && unit != 1) {
            /* Wrong unit: no reply. */
            break;
        }
        if (fc == 0x03) {
            uint16_t start, qty;
            if (length < 6)
                break;
            start = be16(body + 2);
            qty = be16(body + 4);
            if (handle_fc3(fd, tid, unit, start, qty) != 0)
                break;
        } else if (fc == 0x06 || fc == 0x10) {
            if (send_exception(fd, tid, unit, fc, 1) != 0)
                break;
        } else {
            if (send_exception(fd, tid, unit, fc, 1) != 0)
                break;
        }
    }
    close(fd);
}

int main(void)
{
    int server_fd;
    int one = 1;
    struct sockaddr_in addr;
    socklen_t alen = (socklen_t)sizeof(addr);

    init_golden();
    server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd < 0) {
        perror("socket");
        return 1;
    }
    (void)setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0;
    if (bind(server_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0 ||
        listen(server_fd, 8) < 0) {
        perror("bind/listen");
        close(server_fd);
        return 1;
    }
    if (getsockname(server_fd, (struct sockaddr *)&addr, &alen) != 0) {
        perror("getsockname");
        close(server_fd);
        return 1;
    }
    printf("fake_classic: listening on 127.0.0.1:%u\n",
           (unsigned)ntohs(addr.sin_port));
    fflush(stdout);

    for (;;) {
        int cfd = accept(server_fd, NULL, NULL);
        if (cfd < 0) {
            if (errno == EINTR)
                continue;
            break;
        }
        handle_client(cfd);
    }
    close(server_fd);
    return 0;
}
