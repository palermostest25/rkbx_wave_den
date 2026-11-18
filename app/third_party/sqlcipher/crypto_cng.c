/* SQLCipher's crypto through Windows' own CNG (bcrypt.dll), so Wave needs no OpenSSL on Windows.
   Rekordbox Wave's: AES-256-CBC without padding, HMAC-SHA1/256/512, PBKDF2-HMAC and the system RNG,
   exactly what SQLCipher's OpenSSL provider does. Built with -DSQLCIPHER_CRYPTO_CUSTOM. */
#if defined(_WIN32) && defined(SQLCIPHER_CRYPTO_CUSTOM)

#define SQLITE_HAS_CODEC 1
#include "sqlite3.h"

#include <windows.h>
#include <bcrypt.h>

#pragma comment(lib, "bcrypt.lib")

/* As sqlite3.c declares them (crypto.h / sqlcipher.h). */
#define SQLCIPHER_HMAC_SHA1 0
#define SQLCIPHER_HMAC_SHA256 1
#define SQLCIPHER_HMAC_SHA512 2
#define CIPHER_ENCRYPT 1

typedef struct {
  int (*activate)(void *ctx);
  int (*deactivate)(void *ctx);
  const char *(*get_provider_name)(void *ctx);
  int (*add_random)(void *ctx, void *buffer, int length);
  int (*random)(void *ctx, void *buffer, int length);
  int (*hmac)(void *ctx, int algorithm, unsigned char *hmac_key, int key_sz, unsigned char *in, int in_sz,
              unsigned char *in2, int in2_sz, unsigned char *out);
  int (*kdf)(void *ctx, int algorithm, const unsigned char *pass, int pass_sz, unsigned char *salt, int salt_sz,
             int workfactor, int key_sz, unsigned char *key);
  int (*cipher)(void *ctx, int mode, unsigned char *key, int key_sz, unsigned char *iv, unsigned char *in, int in_sz,
                unsigned char *out);
  const char *(*get_cipher)(void *ctx);
  int (*get_key_sz)(void *ctx);
  int (*get_iv_sz)(void *ctx);
  int (*get_block_sz)(void *ctx);
  int (*get_hmac_sz)(void *ctx, int algorithm);
  int (*ctx_init)(void **ctx);
  int (*ctx_free)(void **ctx);
  int (*fips_status)(void *ctx);
  const char *(*get_provider_version)(void *ctx);
} sqlcipher_provider;

static LPCWSTR hash_name(int algorithm) {
  switch (algorithm) {
    case SQLCIPHER_HMAC_SHA1: return BCRYPT_SHA1_ALGORITHM;
    case SQLCIPHER_HMAC_SHA256: return BCRYPT_SHA256_ALGORITHM;
    case SQLCIPHER_HMAC_SHA512: return BCRYPT_SHA512_ALGORITHM;
    default: return NULL;
  }
}

static int cng_add_random(void *ctx, void *buffer, int length) {
  (void)ctx; (void)buffer; (void)length;
  return SQLITE_OK;
}

static int cng_random(void *ctx, void *buffer, int length) {
  (void)ctx;
  return BCRYPT_SUCCESS(BCryptGenRandom(NULL, (PUCHAR)buffer, (ULONG)length, BCRYPT_USE_SYSTEM_PREFERRED_RNG))
             ? SQLITE_OK : SQLITE_ERROR;
}

static const char *cng_name(void *ctx) { (void)ctx; return "cng"; }
static const char *cng_version(void *ctx) { (void)ctx; return "windows"; }

static int cng_hmac(void *ctx, int algorithm, unsigned char *hmac_key, int key_sz, unsigned char *in, int in_sz,
                    unsigned char *in2, int in2_sz, unsigned char *out) {
  BCRYPT_ALG_HANDLE alg = NULL;
  BCRYPT_HASH_HANDLE hash = NULL;
  DWORD size = 0, got = 0;
  int rc = SQLITE_ERROR;
  LPCWSTR name = hash_name(algorithm);
  (void)ctx;
  if (!name || !in) return SQLITE_ERROR;
  if (!BCRYPT_SUCCESS(BCryptOpenAlgorithmProvider(&alg, name, NULL, BCRYPT_ALG_HANDLE_HMAC_FLAG))) return SQLITE_ERROR;
  if (BCRYPT_SUCCESS(BCryptGetProperty(alg, BCRYPT_HASH_LENGTH, (PUCHAR)&size, sizeof size, &got, 0)) &&
      BCRYPT_SUCCESS(BCryptCreateHash(alg, &hash, NULL, 0, hmac_key, (ULONG)key_sz, 0)) &&
      BCRYPT_SUCCESS(BCryptHashData(hash, in, (ULONG)in_sz, 0)) &&
      (!in2 || BCRYPT_SUCCESS(BCryptHashData(hash, in2, (ULONG)in2_sz, 0))) &&
      BCRYPT_SUCCESS(BCryptFinishHash(hash, out, size, 0)))
    rc = SQLITE_OK;
  if (hash) BCryptDestroyHash(hash);
  BCryptCloseAlgorithmProvider(alg, 0);
  return rc;
}

static int cng_kdf(void *ctx, int algorithm, const unsigned char *pass, int pass_sz, unsigned char *salt, int salt_sz,
                   int workfactor, int key_sz, unsigned char *key) {
  BCRYPT_ALG_HANDLE alg = NULL;
  int rc;
  LPCWSTR name = hash_name(algorithm);
  (void)ctx;
  if (!name) return SQLITE_ERROR;
  if (!BCRYPT_SUCCESS(BCryptOpenAlgorithmProvider(&alg, name, NULL, BCRYPT_ALG_HANDLE_HMAC_FLAG))) return SQLITE_ERROR;
  rc = BCRYPT_SUCCESS(BCryptDeriveKeyPBKDF2(alg, (PUCHAR)pass, (ULONG)pass_sz, salt, (ULONG)salt_sz,
                                            (ULONGLONG)workfactor, key, (ULONG)key_sz, 0)) ? SQLITE_OK : SQLITE_ERROR;
  BCryptCloseAlgorithmProvider(alg, 0);
  return rc;
}

static int cng_cipher(void *ctx, int mode, unsigned char *key, int key_sz, unsigned char *iv, unsigned char *in,
                      int in_sz, unsigned char *out) {
  BCRYPT_ALG_HANDLE alg = NULL;
  BCRYPT_KEY_HANDLE handle = NULL;
  unsigned char chain[16];
  ULONG done = 0;
  int rc = SQLITE_ERROR;
  (void)ctx;
  memcpy(chain, iv, sizeof chain);  /* CNG updates the IV in place; SQLCipher's must stay as it is */
  if (!BCRYPT_SUCCESS(BCryptOpenAlgorithmProvider(&alg, BCRYPT_AES_ALGORITHM, NULL, 0))) return SQLITE_ERROR;
  if (BCRYPT_SUCCESS(BCryptSetProperty(alg, BCRYPT_CHAINING_MODE, (PUCHAR)BCRYPT_CHAIN_MODE_CBC,
                                       sizeof(BCRYPT_CHAIN_MODE_CBC), 0)) &&
      BCRYPT_SUCCESS(BCryptGenerateSymmetricKey(alg, &handle, NULL, 0, key, (ULONG)key_sz, 0))) {
    NTSTATUS status = mode == CIPHER_ENCRYPT
        ? BCryptEncrypt(handle, in, (ULONG)in_sz, NULL, chain, sizeof chain, out, (ULONG)in_sz, &done, 0)
        : BCryptDecrypt(handle, in, (ULONG)in_sz, NULL, chain, sizeof chain, out, (ULONG)in_sz, &done, 0);
    if (BCRYPT_SUCCESS(status) && done == (ULONG)in_sz) rc = SQLITE_OK;
  }
  if (handle) BCryptDestroyKey(handle);
  BCryptCloseAlgorithmProvider(alg, 0);
  return rc;
}

static const char *cng_get_cipher(void *ctx) { (void)ctx; return "aes-256-cbc"; }
static int cng_key_sz(void *ctx) { (void)ctx; return 32; }
static int cng_iv_sz(void *ctx) { (void)ctx; return 16; }
static int cng_block_sz(void *ctx) { (void)ctx; return 16; }

static int cng_hmac_sz(void *ctx, int algorithm) {
  (void)ctx;
  switch (algorithm) {
    case SQLCIPHER_HMAC_SHA1: return 20;
    case SQLCIPHER_HMAC_SHA256: return 32;
    case SQLCIPHER_HMAC_SHA512: return 64;
    default: return 0;
  }
}

static int cng_ctx_init(void **ctx) { (void)ctx; return SQLITE_OK; }
static int cng_ctx_free(void **ctx) { (void)ctx; return SQLITE_OK; }
static int cng_fips(void *ctx) { (void)ctx; return 0; }

int sqlcipher_custom_setup(sqlcipher_provider *p) {
  p->random = cng_random;
  p->get_provider_name = cng_name;
  p->hmac = cng_hmac;
  p->kdf = cng_kdf;
  p->cipher = cng_cipher;
  p->get_cipher = cng_get_cipher;
  p->get_key_sz = cng_key_sz;
  p->get_iv_sz = cng_iv_sz;
  p->get_block_sz = cng_block_sz;
  p->get_hmac_sz = cng_hmac_sz;
  p->ctx_init = cng_ctx_init;
  p->ctx_free = cng_ctx_free;
  p->add_random = cng_add_random;
  p->fips_status = cng_fips;
  p->get_provider_version = cng_version;
  return SQLITE_OK;
}

#endif
