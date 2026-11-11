#include "zip_writer.h"

#include <dirent.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define MAX_ENTRIES 256
#define MAX_NAME 256
#define MAX_FILE (64u * 1024u * 1024u)

typedef struct
{
    char name[MAX_NAME];
    uint32_t offset;
    uint32_t crc;
    uint32_t size;
    uint16_t time;
    uint16_t date;
} entry_t;

static uint32_t crc_table[256];
static int crc_ready = 0;

static void crc_init(void)
{
    for (uint32_t i = 0; i < 256; i++)
    {
        uint32_t c = i;
        for (int k = 0; k < 8; k++)
            c = (c & 1) ? (0xedb88320u ^ (c >> 1)) : (c >> 1);
        crc_table[i] = c;
    }
    crc_ready = 1;
}

static uint32_t crc32_of(const unsigned char *buf, size_t len)
{
    uint32_t c = 0xffffffffu;
    if (!crc_ready)
        crc_init();
    for (size_t i = 0; i < len; i++)
        c = crc_table[(c ^ buf[i]) & 0xff] ^ (c >> 8);
    return c ^ 0xffffffffu;
}

static int w16(FILE *f, uint16_t v)
{
    unsigned char b[2] = {(unsigned char)(v & 0xff), (unsigned char)(v >> 8)};
    return fwrite(b, 1, 2, f) == 2 ? 0 : -1;
}

static int w32(FILE *f, uint32_t v)
{
    unsigned char b[4] = {(unsigned char)(v & 0xff), (unsigned char)((v >> 8) & 0xff),
                          (unsigned char)((v >> 16) & 0xff), (unsigned char)((v >> 24) & 0xff)};
    return fwrite(b, 1, 4, f) == 4 ? 0 : -1;
}

static void dos_time(time_t t, uint16_t *dtime, uint16_t *ddate)
{
    struct tm *tm = localtime(&t);
    if (!tm)
    {
        *dtime = 0;
        *ddate = (1u << 5) | 1u;
        return;
    }
    *dtime = (uint16_t)((tm->tm_hour << 11) | (tm->tm_min << 5) | (tm->tm_sec / 2));
    *ddate = (uint16_t)(((tm->tm_year - 80) << 9) | ((tm->tm_mon + 1) << 5) | tm->tm_mday);
}

static unsigned char *read_file(const char *path, uint32_t *len)
{
    FILE *f = fopen(path, "rb");
    unsigned char *buf;
    long size;

    if (!f)
        return NULL;
    if (fseek(f, 0, SEEK_END) != 0 || (size = ftell(f)) < 0 || (unsigned long)size > MAX_FILE ||
        fseek(f, 0, SEEK_SET) != 0)
    {
        fclose(f);
        return NULL;
    }
    buf = malloc((size_t)size ? (size_t)size : 1);
    if (!buf)
    {
        fclose(f);
        return NULL;
    }
    if (size > 0 && fread(buf, 1, (size_t)size, f) != (size_t)size)
    {
        free(buf);
        fclose(f);
        return NULL;
    }
    fclose(f);
    *len = (uint32_t)size;
    return buf;
}

int zip_directory(const char *dir, const char *zip_path)
{
    DIR *d;
    struct dirent *de;
    entry_t entries[MAX_ENTRIES];
    int count = 0;
    char tmp[512];
    FILE *out;
    uint32_t cd_start, cd_size;

    if (!dir || !zip_path)
        return -1;

    d = opendir(dir);
    if (!d)
        return -1;

    snprintf(tmp, sizeof(tmp), "%s.tmp", zip_path);
    out = fopen(tmp, "wb");
    if (!out)
    {
        closedir(d);
        return -1;
    }

    while ((de = readdir(d)) != NULL && count < MAX_ENTRIES)
    {
        char path[768];
        struct stat st;
        unsigned char *data;
        uint32_t len = 0;

        if (de->d_name[0] == '.' || strlen(de->d_name) >= MAX_NAME)
            continue;
        snprintf(path, sizeof(path), "%s/%s", dir, de->d_name);
        if (stat(path, &st) != 0 || !S_ISREG(st.st_mode))
            continue;
        data = read_file(path, &len);
        if (!data)
            continue;

        entry_t *e = &entries[count];
        snprintf(e->name, sizeof(e->name), "%s", de->d_name);
        e->offset = (uint32_t)ftell(out);
        e->crc = crc32_of(data, len);
        e->size = len;
        dos_time(st.st_mtime, &e->time, &e->date);

        if (w32(out, 0x04034b50) || w16(out, 20) || w16(out, 0) || w16(out, 0) ||
            w16(out, e->time) || w16(out, e->date) || w32(out, e->crc) ||
            w32(out, e->size) || w32(out, e->size) || w16(out, (uint16_t)strlen(e->name)) ||
            w16(out, 0) || fwrite(e->name, 1, strlen(e->name), out) != strlen(e->name) ||
            (len > 0 && fwrite(data, 1, len, out) != len))
        {
            free(data);
            goto fail;
        }
        free(data);
        count++;
    }
    closedir(d);

    cd_start = (uint32_t)ftell(out);
    for (int i = 0; i < count; i++)
    {
        entry_t *e = &entries[i];
        if (w32(out, 0x02014b50) || w16(out, 20) || w16(out, 20) || w16(out, 0) || w16(out, 0) ||
            w16(out, e->time) || w16(out, e->date) || w32(out, e->crc) || w32(out, e->size) ||
            w32(out, e->size) || w16(out, (uint16_t)strlen(e->name)) || w16(out, 0) ||
            w16(out, 0) || w16(out, 0) || w16(out, 0) || w32(out, 0) || w32(out, e->offset) ||
            fwrite(e->name, 1, strlen(e->name), out) != strlen(e->name))
            goto fail;
    }
    cd_size = (uint32_t)ftell(out) - cd_start;

    if (w32(out, 0x06054b50) || w16(out, 0) || w16(out, 0) || w16(out, (uint16_t)count) ||
        w16(out, (uint16_t)count) || w32(out, cd_size) || w32(out, cd_start) || w16(out, 0))
        goto fail;

    if (fclose(out) != 0)
    {
        unlink(tmp);
        return -1;
    }
    if (rename(tmp, zip_path) != 0)
    {
        unlink(tmp);
        return -1;
    }
    return 0;

fail:
    fclose(out);
    unlink(tmp);
    return -1;
}
