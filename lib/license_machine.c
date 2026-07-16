/**
 * @file license_machine.c
 * @brief 硬件指纹采集与机器码派生（lib/license.c 与 tools/rkvc_lic.c 共享实现）。
 *
 * 这是机器码算法的唯一定义，确保签发端与校验端字节级一致。
 * 调用方须已调用 sodium_init()。
 */
#include "license_machine.h"

#include <ctype.h>
#include <dirent.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include <sodium.h>

static int read_file_text(const char *path, char *buf, size_t size)
{
    FILE *f = fopen(path, "rb");
    if (!f)
        return -1;
    size_t n = fread(buf, 1, size - 1, f);
    fclose(f);
    buf[n] = '\0';
    /* 去尾部空白/NUL（设备树属性以 NUL 结尾） */
    while (n > 0 && (buf[n - 1] == '\0' || isspace((unsigned char)buf[n - 1])))
        buf[--n] = '\0';
    return n > 0 ? (int)n : -1;
}

static int read_dt_serial(char *buf, size_t size)
{
    return read_file_text("/proc/device-tree/serial-number", buf, size);
}

static int read_otp_hex(char *buf, size_t size)
{
    FILE *f = fopen("/sys/bus/nvmem/devices/rockchip-otp0/nvmem", "rb");
    if (!f)
        return -1;
    uint8_t raw[32];
    size_t n = fread(raw, 1, sizeof(raw), f);
    fclose(f);
    if (n == 0)
        return -1;
    for (size_t i = 0; i < n; i++)
        snprintf(buf + i * 2, size - i * 2, "%02x", raw[i]);
    return (int)(n * 2);
}

static int read_first_mac(char *buf, size_t size)
{
    DIR *dir = opendir("/sys/class/net");
    if (!dir)
        return -1;
    int found = 0;
    struct dirent *ent;
    while (!found && (ent = readdir(dir)) != NULL) {
        if (ent->d_name[0] == '.')
            continue;
        char path[320];
        snprintf(path, sizeof(path), "/sys/class/net/%s/virtual", ent->d_name);
        /* 跳过虚拟接口（lo/docker/br-…） */
        if (access(path, F_OK) == 0)
            continue;
        snprintf(path, sizeof(path), "/sys/class/net/%s/address", ent->d_name);
        if (read_file_text(path, buf, size) > 0 &&
            strcmp(buf, "00:00:00:00:00:00") != 0) {
            found = 1;
        }
    }
    closedir(dir);
    return found ? (int)strlen(buf) : -1;
}

static void sha256_hex(const char *input, size_t input_len, char *out_hex)
{
    uint8_t digest[crypto_hash_sha256_BYTES];
    crypto_hash_sha256(digest, (const unsigned char *)input,
                       (unsigned long)input_len);
    for (size_t i = 0; i < sizeof(digest); i++)
        snprintf(out_hex + i * 2, 3, "%02x", digest[i]);
}

int lic_machine_id_hex(char *out_hex, size_t out_size)
{
    if (!out_hex || out_size < LIC_MACHINE_ID_HEX_LEN)
        return -1;

    char raw[256];
    const char *tag = NULL;

    if (read_dt_serial(raw, sizeof(raw)) > 0) {
        tag = "dt-serial";
    } else if (read_otp_hex(raw, sizeof(raw)) > 0) {
        tag = "otp";
    } else if (read_first_mac(raw, sizeof(raw)) > 0) {
        tag = "mac";
    } else {
        return -1;
    }

    /* 组合：tag + ':' + 原始值 → SHA-256 */
    char concat[512];
    int n = snprintf(concat, sizeof(concat), "%s:%s", tag, raw);
    if (n < 0 || (size_t)n >= sizeof(concat))
        return -1;

    sha256_hex(concat, (size_t)n, out_hex);
    return 0;
}
