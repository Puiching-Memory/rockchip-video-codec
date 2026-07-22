/*
 * rkvc_lic - rkvc 1机1码 授权辅助工具（独立，仅依赖 libsodium）。
 *
 * 客户机分发版（编译期定义 RKVC_LIC_MACHINE_ONLY）：
 *   rkvc_lic machine-id                              打印本机机器码（hex）
 *   rkvc_lic verify    -f <license.lic> -k <key>     校验签名+机器码
 *
 * 完整版（打包方内部使用）额外支持：
 *   rkvc_lic genkey    -o <dir>                      生成 Ed25519 密钥对
 *   rkvc_lic issue     -m <hex> -k <secret.key>
 *                      [-p N] -o <file>              签发注册码
 *   rkvc_lic inspect   -f <license.lic>              解析注册码字段
 *
 * 编译时定义 RKVC_LIC_MACHINE_ONLY 可裁剪为客户机采集版：
 * 仅保留 machine-id 与 verify，移除 genkey/issue/inspect，避免把私钥签发
 * 能力随可移植包分发给终端客户。
 *
 * 密钥为原始二进制文件：secret.key（64 字节）、public.key（32 字节），
 * 均由完整版 'rkvc_lic genkey' 产出。许可证 blob 布局见 lib/license.c（104 字节，小端）。
 */
#define _GNU_SOURCE 1

#include <sodium.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "license_layout.h"
#include "license_machine.h"
#include "license_b64.h"

/* 短名指向共享布局常量（license_layout.h），与 lib/license.c 单一来源 */
#define MAGIC      RKVC_LICENSE_MAGIC
#define SIGNED_LEN RKVC_LICENSE_SIGNED_LEN
#define SIG_LEN    RKVC_LICENSE_SIG_LEN
#define BLOB_SIZE  (SIGNED_LEN + SIG_LEN)
#define PRODUCT    1u

/* 编译期断言：blob 布局假设须与 libsodium 常量一致 */
_Static_assert(SIG_LEN == 64u, "Ed25519 signature must be 64 bytes");
_Static_assert(BLOB_SIZE == 104u, "license blob must be 104 bytes");
_Static_assert(crypto_sign_PUBLICKEYBYTES == 32u, "Ed25519 public key must be 32 bytes");
_Static_assert(crypto_sign_SECRETKEYBYTES == 64u, "Ed25519 secret key must be 64 bytes");

/* ── 小端 ──────────────────────────────────────────────────────── */
static void put_u32(uint8_t *p, uint32_t v) {
    p[0]=(uint8_t)v; p[1]=(uint8_t)(v>>8); p[2]=(uint8_t)(v>>16); p[3]=(uint8_t)(v>>24);
}
static uint32_t get_u32(const uint8_t *p) {
    return (uint32_t)p[0]|((uint32_t)p[1]<<8)|((uint32_t)p[2]<<16)|((uint32_t)p[3]<<24);
}

/* ── base64 ────────────────────────────────────────────────────── */
static const char B64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
static void b64_encode(const uint8_t *src, size_t len, char *dst) {
    size_t o=0;
    for (size_t i=0;i<len;i+=3){
        uint32_t v=(uint32_t)src[i]<<16;
        if(i+1<len) v|=(uint32_t)src[i+1]<<8;
        if(i+2<len) v|=(uint32_t)src[i+2];
        dst[o++]=B64[(v>>18)&63];
        dst[o++]=B64[(v>>12)&63];
        dst[o++]= (i+1<len)?B64[(v>>6)&63]:'=';
        dst[o++]= (i+2<len)?B64[v&63]:'=';
    }
    dst[o]='\0';
}

/* ── 机器码（委托共享实现 license_machine.c，与 lib/license.c 单一来源） ── */
static void print_fp_report(const lic_fp_info *info, int ok)
{
    fprintf(stderr, "fingerprint sources (priority dt-serial → otp → mac):\n");
    fprintf(stderr, "  dt-serial : %s\n",
            info->note_dt[0] ? info->note_dt : "(not probed)");
    fprintf(stderr, "  otp       : %s\n",
            info->note_otp[0] ? info->note_otp : "(not probed)");
    fprintf(stderr, "  mac       : %s\n",
            info->note_mac[0] ? info->note_mac : "(not probed)");
    if (ok) {
        fprintf(stderr, "selected   : %s\n", info->tag);
        fprintf(stderr, "path       : %s\n", info->path);
        fprintf(stderr, "raw        : %s\n", info->raw);
        fprintf(stderr, "machine_id : %s\n", info->machine_id);
    } else {
        fprintf(stderr, "error: cannot collect hardware fingerprint\n");
    }
}

static int machine_id(char *out, size_t sz)
{
    lic_fp_info info;
    if (lic_machine_id_collect(&info) != 0) {
        print_fp_report(&info, 0);
        return -1;
    }
    if (sz < LIC_MACHINE_ID_HEX_LEN)
        return -1;
    memcpy(out, info.machine_id, LIC_MACHINE_ID_HEX_LEN);
    return 0;
}

/* ── hex 工具 ──────────────────────────────────────────────────── */
static int hex2bytes(const char*hex,uint8_t*out,size_t n){
    if(strlen(hex)!=n*2)return -1;
    for(size_t i=0;i<n;i++){char b[3]={hex[i*2],hex[i*2+1],0};char*e=NULL;
        long v=strtol(b,&e,16);if(*e)return -1;out[i]=(uint8_t)v;}
    return 0;
}

/* ── 密钥文件读写（原始二进制） ────────────────────────────────── */

/* 读取密钥文件到缓冲，返回读取字节数；失败返回 -1。 */
static int read_keyfile(const char *path, uint8_t *buf, size_t cap)
{
    FILE *f = fopen(path, "rb");
    if (!f) { perror(path); return -1; }
    size_t n = fread(buf, 1, cap, f);
    int err = ferror(f);
    fclose(f);
    if (err) { fprintf(stderr, "error: read %s failed\n", path); return -1; }
    return (int)n;
}

/* 用原始私钥（64 字节）对 data 做 detached 签名。 */
static int sign_with_secret_key(const char *keyfile, const uint8_t *data,
                                size_t dlen, uint8_t *sig)
{
    uint8_t sk[crypto_sign_SECRETKEYBYTES];
    int n = read_keyfile(keyfile, sk, sizeof(sk));
    if (n < 0 || (size_t)n != crypto_sign_SECRETKEYBYTES) {
        fprintf(stderr, "error: cannot read %u-byte secret key from %s\n",
                (unsigned)crypto_sign_SECRETKEYBYTES, keyfile);
        return -1;
    }
    unsigned long long siglen = 0;
    if (crypto_sign_detached(sig, &siglen, data,
                             (unsigned long long)dlen, sk) != 0)
        return -1;
    return 0;
}

/* 加载公钥：接受 32 字节公钥文件或 64 字节私钥文件（派生公钥）。 */
static int load_pubkey(const char *keyfile, uint8_t pub[crypto_sign_PUBLICKEYBYTES])
{
    uint8_t buf[crypto_sign_SECRETKEYBYTES];
    int n = read_keyfile(keyfile, buf, sizeof(buf));
    if (n < 0)
        return -1;
    if ((size_t)n == crypto_sign_PUBLICKEYBYTES) {
        memcpy(pub, buf, crypto_sign_PUBLICKEYBYTES);
        return 0;
    }
    if ((size_t)n == crypto_sign_SECRETKEYBYTES) {
        if (crypto_sign_ed25519_sk_to_pk(pub, buf) != 0)
            return -1;
        return 0;
    }
    fprintf(stderr, "error: key file %s is %d bytes (expected 32 or 64)\n",
            keyfile, n);
    return -1;
}

/* ── blob 组装 ─────────────────────────────────────────────────── */
static void build_signed(uint8_t*out,uint32_t product,const uint8_t*mid32){
    put_u32(out,MAGIC);put_u32(out+4,product);
    memcpy(out+8,mid32,32);
}

/* ── 子命令 ────────────────────────────────────────────────────── */
#ifndef RKVC_LIC_MACHINE_ONLY
static int cmd_genkey(const char*outdir){
    if (sodium_init() < 0) { fprintf(stderr,"error: sodium_init failed\n"); return 1; }

    unsigned char pk[crypto_sign_PUBLICKEYBYTES];
    unsigned char sk[crypto_sign_SECRETKEYBYTES];
    if (crypto_sign_keypair(pk, sk) != 0) {
        fprintf(stderr, "error: keygen failed\n"); return 1; }

    char path[512]; FILE *f;

    /* secret.key（64 字节原始私钥，权限 0600） */
    snprintf(path,sizeof(path),"%s/secret.key",outdir);
    f=fopen(path,"wb");
    if(!f){perror(outdir);return 1;}
    if(fwrite(sk,1,sizeof(sk),f)!=sizeof(sk)){fprintf(stderr,"error: write failed\n");fclose(f);return 1;}
    fclose(f);
    chmod(path,0600);

    /* public.key（32 字节原始公钥） */
    snprintf(path,sizeof(path),"%s/public.key",outdir);
    f=fopen(path,"wb");
    if(!f){perror(outdir);return 1;}
    if(fwrite(pk,1,sizeof(pk),f)!=sizeof(pk)){fprintf(stderr,"error: write failed\n");fclose(f);return 1;}
    fclose(f);

    printf("Generated:\n  %s/secret.key (%u bytes)\n  %s/public.key (%u bytes)\n",
           outdir,(unsigned)crypto_sign_SECRETKEYBYTES,
           outdir,(unsigned)crypto_sign_PUBLICKEYBYTES);
    printf("Public key (hex): ");
    for(int i=0;i<32;i++)printf("%02x",pk[i]);
    printf("\n\n⚠️  妥善保管 secret.key，切勿提交到仓库。\n");
    return 0;
}
#endif

static int cmd_machine_id(void)
{
    lic_fp_info info;
    if (lic_machine_id_collect(&info) != 0) {
        print_fp_report(&info, 0);
        return 1;
    }
    /* 诊断走 stderr；stdout 仅机器码，兼容 `mid=$(rkvc_lic machine-id)` */
    print_fp_report(&info, 1);
    printf("%s\n", info.machine_id);
    return 0;
}

static void print_blob(const uint8_t*blob){
    printf("magic      : 0x%08x\n",get_u32(blob));
    printf("product_id : %u\n",get_u32(blob+4));
    printf("machine_id : ");for(int i=0;i<32;i++)printf("%02x",blob[8+i]);printf("\n");
}

#ifndef RKVC_LIC_MACHINE_ONLY
static int cmd_issue(const char*mid_hex,const char*secret_key,
                     uint32_t product,const char*outfile){
    uint8_t mid32[32];
    if(hex2bytes(mid_hex,mid32,32)){fprintf(stderr,"error: bad machine-id (need 64 hex chars)\n");return 1;}
    uint8_t signed_region[SIGNED_LEN];
    build_signed(signed_region,product,mid32);
    uint8_t sig[SIG_LEN];
    if(sign_with_secret_key(secret_key,signed_region,SIGNED_LEN,sig)){
        fprintf(stderr,"error: signing failed\n");return 1;}
    uint8_t blob[BLOB_SIZE];
    memcpy(blob,signed_region,SIGNED_LEN);
    memcpy(blob+SIGNED_LEN,sig,SIG_LEN);
    char b64[256];b64_encode(blob,BLOB_SIZE,b64);
    FILE*f=outfile? fopen(outfile,"wb"):stdout;
    if(!f){perror(outfile);return 1;}
    fprintf(f,"%s\n",b64);
    if(outfile)fclose(f);
    if(outfile)printf("License written to %s\n",outfile);
    return 0;
}

static int cmd_inspect(const char*file){
    FILE*f=fopen(file,"rb");if(!f){perror(file);return 1;}
    char text[512];size_t n=fread(text,1,sizeof(text)-1,f);fclose(f);text[n]='\0';
    uint8_t blob[BLOB_SIZE];size_t bl=0;
    if(lic_b64_decode(text,n,blob,sizeof(blob),&bl)||bl!=BLOB_SIZE){
        fprintf(stderr,"error: cannot decode license\n");return 1;}
    print_blob(blob);return 0;
}
#endif

static int cmd_verify(const char*file,const char*keyfile){
    FILE*f=fopen(file,"rb");if(!f){perror(file);return 1;}
    char text[512];size_t n=fread(text,1,sizeof(text)-1,f);fclose(f);text[n]='\0';
    uint8_t blob[BLOB_SIZE];size_t bl=0;
    if(lic_b64_decode(text,n,blob,sizeof(blob),&bl)||bl!=BLOB_SIZE){
        fprintf(stderr,"error: cannot decode license\n");return 1;}
    /* 签名（libsodium 原生 Ed25519 detached 验签） */
    uint8_t pub[crypto_sign_PUBLICKEYBYTES];
    if(load_pubkey(keyfile,pub)){
        fprintf(stderr,"error: cannot load key from %s\n",keyfile);return 1;}
    int sigok=(crypto_sign_verify_detached(blob+SIGNED_LEN,blob,SIGNED_LEN,pub)==0)?1:0;
    printf("signature  : %s\n",sigok?"VALID":"INVALID");
    /* 机器码 */
    char local[65];int got=(!machine_id(local,sizeof(local)));
    char lic[65];for(int i=0;i<32;i++)snprintf(lic+i*2,3,"%02x",blob[8+i]);
    printf("local_mch  : %s\n",got?local:"(unavailable)");
    printf("lic_machine: %s\n",lic);
    printf("machine    : %s\n",(!got)?"unknown":(strcmp(local,lic)?"MISMATCH":"MATCH"));
    print_blob(blob);
    return (sigok&&got&&!strcmp(local,lic))?0:1;
}

/* ── main ──────────────────────────────────────────────────────── */
static void usage(void){
    fprintf(stderr,
"rkvc_lic — rkvc 1机1码 工具\n\n"
#ifdef RKVC_LIC_MACHINE_ONLY
"  rkvc_lic machine-id                              打印本机机器码（含指纹来源诊断）\n"
"  rkvc_lic verify    -f <license.lic> -k <key>     校验签名+机器码\n"
"\n"
"  此发行版仅包含机器码采集与校验功能，不含密钥生成/签发能力。\n"
#else
"  rkvc_lic genkey    -o <dir>                      生成 Ed25519 密钥对\n"
"  rkvc_lic machine-id                              打印本机机器码（含指纹来源诊断）\n"
"  rkvc_lic issue     -m <hex> -k <secret.key>\n"
"                    [-p N] -o <file>              签发注册码\n"
"  rkvc_lic inspect   -f <license.lic>              解析注册码字段\n"
"  rkvc_lic verify    -f <license.lic> -k <key>     校验签名+机器码\n"
"\n"
"  <key> 可为 public.key（32 字节）或 secret.key（64 字节，自动派生公钥）。\n"
#endif
);
}

int main(int argc,char**argv){
    if (sodium_init() < 0) {
        fprintf(stderr, "error: sodium_init failed\n"); return 2; }

    if(argc<2){usage();return 2;}
    const char*cmd=argv[1];

    if(!strcmp(cmd,"machine-id"))return cmd_machine_id();

#ifndef RKVC_LIC_MACHINE_ONLY
    if(!strcmp(cmd,"genkey")){
        const char*outdir=NULL;
        for(int i=2;i<argc;i++){if(i+1<argc&&!strcmp(argv[i],"-o"))outdir=argv[++i];}
        if(!outdir){usage();return 2;}
        return cmd_genkey(outdir);
    }
    if(!strcmp(cmd,"issue")){
        const char*mid=NULL,*key=NULL,*outfile=NULL;uint32_t prod=PRODUCT;
        for(int i=2;i<argc;i++){
            if(i+1<argc&&!strcmp(argv[i],"-m"))mid=argv[++i];
            else if(i+1<argc&&!strcmp(argv[i],"-k"))key=argv[++i];
            else if(i+1<argc&&!strcmp(argv[i],"-p")){
                char*end=NULL;unsigned long v=strtoul(argv[++i],&end,10);
                if(end==argv[i]||*end){fprintf(stderr,"error: invalid product id '%s'\n",argv[i]);return 2;}
                prod=(uint32_t)v;
            }
            else if(i+1<argc&&!strcmp(argv[i],"-o"))outfile=argv[++i];
        }
        if(!mid||!key){usage();return 2;}
        return cmd_issue(mid,key,prod,outfile);
    }
    if(!strcmp(cmd,"inspect")){
        const char*file=NULL;
        for(int i=2;i<argc;i++){if(i+1<argc&&!strcmp(argv[i],"-f"))file=argv[++i];}
        if(!file){usage();return 2;}
        return cmd_inspect(file);
    }
#endif
    if(!strcmp(cmd,"verify")){
        const char*file=NULL,*key=NULL;
        for(int i=2;i<argc;i++){
            if(i+1<argc&&!strcmp(argv[i],"-f"))file=argv[++i];
            else if(i+1<argc&&!strcmp(argv[i],"-k"))key=argv[++i];
        }
        if(!file||!key){usage();return 2;}
        return cmd_verify(file,key);
    }
    usage();return 2;
}
