#include "zcta.h"

#include <ctype.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <zlib.h>

#ifndef MF_DATADIR
#define MF_DATADIR "/usr/local/share/moon-flare"
#endif

#define ZCTA_MAGIC "#moonflare-zcta "

void zcta_paths(char *state_tab, size_t scap, char *meta, size_t mcap,
                char *base_tab, size_t bcap)
{
    const char *dir = getenv("MF_WEATHER_DIR");
    const char *st = getenv("STATE_DIRECTORY");
    const char *base = getenv("MF_ZCTA_BASELINE");
    char d[256];

    if (dir && dir[0])
        snprintf(d, sizeof(d), "%s", dir);
    else
        snprintf(d, sizeof(d), "%s/weathergov",
                 st && st[0] ? st : "/var/lib/moonflare");
    if (state_tab && scap)
        snprintf(state_tab, scap, "%s/zcta.txt", d);
    if (meta && mcap)
        snprintf(meta, mcap, "%s/zcta.meta", d);
    if (base_tab && bcap)
        snprintf(base_tab, bcap, "%s",
                 base && base[0] ? base : MF_DATADIR "/zcta.txt");
}

const char *zcta_pick(const char *state_tab, const char *base_tab)
{
    return access(state_tab, R_OK) == 0 ? state_tab : base_tab;
}

int zcta_file_year(const char *path)
{
    FILE *f = fopen(path, "r");
    char line[64];
    int year = 0;

    if (!f)
        return 0;
    if (fgets(line, sizeof(line), f) &&
        strncmp(line, ZCTA_MAGIC, strlen(ZCTA_MAGIC)) == 0)
        year = atoi(line + strlen(ZCTA_MAGIC));
    fclose(f);
    return year;
}

int zcta_lookup(const char *path, const char *zip, double *lat, double *lon)
{
    FILE *f = fopen(path, "r");
    char line[96];
    size_t zl = strlen(zip);
    int rc = -1;

    if (!f)
        return -2;
    while (fgets(line, sizeof(line), f))
    {
        char *a, *b;

        if (line[0] == '#' || strncmp(line, zip, zl) != 0 || line[zl] != '\t')
            continue;
        a = line + zl + 1;
        b = strchr(a, '\t');
        if (!b)
            break;
        *lat = strtod(a, NULL);
        *lon = strtod(b + 1, NULL);
        rc = 0;
        break;
    }
    fclose(f);
    return rc;
}

/* Split one line on `delim`, dropping empty (whitespace-only) fields. */
static int split_fields(char *line, char delim, char **f, int max)
{
    int n = 0;
    char *p = line;

    while (p && n < max)
    {
        char *e = strchr(p, delim);
        char *s = p, *t;

        if (e)
            *e = '\0';
        while (*s && isspace((unsigned char)*s))
            s++;
        t = s + strlen(s);
        while (t > s && isspace((unsigned char)t[-1]))
            *--t = '\0';
        if (*s)
            f[n++] = s;
        p = e ? e + 1 : NULL;
    }
    return n;
}

int zcta_from_gazetteer(const char *txt, size_t len, int year,
                        char **out, size_t *outlen)
{
    char *copy, *line, *next, delim;
    size_t cap = len + 64, off = 0;
    int rows = 0, first = 1;
    char *buf;

    if (!txt || !out || !outlen)
        return -1;
    copy = malloc(len + 1);
    buf = malloc(cap);
    if (!copy || !buf)
    {
        free(copy);
        free(buf);
        return -1;
    }
    memcpy(copy, txt, len);
    copy[len] = '\0';
    off = (size_t)snprintf(buf, cap, ZCTA_MAGIC "%d\n", year);
    delim = strchr(copy, '|') && strchr(copy, '|') < strchr(copy, '\n') ? '|' : '\t';
    for (line = copy; line && *line; line = next)
    {
        char *f[16];
        int n;

        next = strchr(line, '\n');
        if (next)
            *next++ = '\0';
        if (first)                      /* header row */
        {
            first = 0;
            continue;
        }
        n = split_fields(line, delim, f, 16);
        if (n < 3 || strlen(f[0]) != 5 || !isdigit((unsigned char)f[0][0]))
            continue;
        if (off + 64 >= cap)
            break;
        off += (size_t)snprintf(buf + off, cap - off, "%s\t%s\t%s\n",
                                f[0], f[n - 2], f[n - 1]);
        rows++;
    }
    free(copy);
    if (rows == 0)
    {
        free(buf);
        return -1;
    }
    *out = buf;
    *outlen = off;
    return rows;
}

static uint32_t le32(const unsigned char *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 |
           (uint32_t)p[3] << 24;
}

static uint16_t le16(const unsigned char *p)
{
    return (uint16_t)(p[0] | p[1] << 8);
}

int zcta_unzip_single(const unsigned char *zip, size_t len,
                      char **out, size_t *outlen)
{
    size_t hdr, data, usize, cap;
    z_stream zs;
    char *buf;
    int zrc;

    /* Local file header: signature, then method at 8, sizes at 18/22,
       name and extra lengths at 26/28. */
    if (!zip || len < 30 || le32(zip) != 0x04034b50u)
        return -1;
    if (le16(zip + 8) != 8)             /* deflate only */
        return -1;
    hdr = 30u + le16(zip + 26) + le16(zip + 28);
    if (hdr >= len)
        return -1;
    data = hdr;
    usize = le32(zip + 22);             /* 0 when a data descriptor follows */
    cap = usize ? usize + 1 : len * 4 + 1;
    buf = malloc(cap);
    if (!buf)
        return -1;
    memset(&zs, 0, sizeof(zs));
    if (inflateInit2(&zs, -MAX_WBITS) != Z_OK)
    {
        free(buf);
        return -1;
    }
    zs.next_in = (unsigned char *)zip + data;
    zs.avail_in = (uInt)(len - data);
    zs.next_out = (unsigned char *)buf;
    zs.avail_out = (uInt)(cap - 1);
    for (;;)
    {
        zrc = inflate(&zs, Z_NO_FLUSH);
        if (zrc == Z_STREAM_END)
            break;
        if (zrc != Z_OK && zrc != Z_BUF_ERROR)
            break;
        if (zs.avail_out == 0)
        {
            size_t used = zs.total_out;
            char *nb = realloc(buf, cap * 2);

            if (!nb)
                break;
            buf = nb;
            cap *= 2;
            zs.next_out = (unsigned char *)buf + used;
            zs.avail_out = (uInt)(cap - 1 - used);
        }
        else if (zrc == Z_BUF_ERROR)
            break;                      /* truncated input */
    }
    inflateEnd(&zs);
    if (zrc != Z_STREAM_END)
    {
        free(buf);
        return -1;
    }
    buf[zs.total_out] = '\0';
    *out = buf;
    *outlen = zs.total_out;
    return 0;
}

int zcta_write_atomic(const char *path, const char *data, size_t len)
{
    char dir[256], tmp[300];
    char *slash;
    FILE *f;

    snprintf(dir, sizeof(dir), "%s", path);
    slash = strrchr(dir, '/');
    if (slash)
    {
        *slash = '\0';
        if (mkdir(dir, 0755) < 0 && errno != EEXIST)
            return -1;
    }
    snprintf(tmp, sizeof(tmp), "%s.tmp.%ld", path, (long)getpid());
    f = fopen(tmp, "w");
    if (!f)
        return -1;
    if (fwrite(data, 1, len, f) != len)
    {
        fclose(f);
        unlink(tmp);
        return -1;
    }
    if (fclose(f) != 0 || rename(tmp, path) < 0)
    {
        unlink(tmp);
        return -1;
    }
    return 0;
}

long zcta_meta_checked(const char *meta)
{
    FILE *f = fopen(meta, "r");
    long t = 0;

    if (!f)
        return 0;
    if (fscanf(f, "checked %ld", &t) != 1)
        t = 0;
    fclose(f);
    return t;
}

int zcta_meta_write(const char *meta, long checked)
{
    char buf[48];
    int n = snprintf(buf, sizeof(buf), "checked %ld\n", checked);

    return zcta_write_atomic(meta, buf, (size_t)n);
}
