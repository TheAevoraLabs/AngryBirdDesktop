/* store_patch.c -- see store_patch.h.
 *
 * The JSON is machine-generated and flat, so instead of pulling in a JSON
 * parser we walk it and rewrite only the numeric literal that follows a key we
 * care about. Everything else -- key order, whitespace, the checksum string --
 * is copied through byte for byte, which keeps the blob in exactly the shape the
 * engine wrote it in.
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>
#include <dirent.h>
#include <time.h>
#include <unistd.h>
#include <sys/stat.h>

#include "common/game_config.h"
#include "common/util.h"
#include "aes.h"
#include "store_patch.h"

#define PATHMAX 512
#define WALLET_MAX 9999999
#define ITEM_MAX   9999

/* Rovio's save-file key (settings.lua / highscores.lua / RCS cache), AES-256-CBC,
 * zero IV -- same key the power-up patch uses. */
static const uint8_t SAVE_KEY[32] = {
  '4','4','i','U','Y','5','a','T','r','l','a','Y','o','e','t','9',
  'l','a','p','R','l','a','K','1','E','h','l','e','c','5','i','0'
};

/* ------------------------------------------------------------------ target */

typedef struct {
    long wallet;   /* coins / gems / tickets */
    long item;     /* power-ups and consumables */
    int  enabled;
} Targets;

static int is_wallet_key(const char *k) {
    return !strcmp(k, "coins") || !strcmp(k, "gems") || !strcmp(k, "tickets");
}

static int is_item_key(const char *k) {
    static const char *const items[] = {
        "shockwave", "slingscope", "birdquake", "powerpotion", "kingsling",
        "tntshield", "leagueticketdoubler", "extrastars",
    };
    for (unsigned i = 0; i < sizeof items / sizeof *items; i++)
        if (!strcmp(k, items[i])) return 1;
    return 0;
}

static Targets targets_from_config(void) {
    Targets t = { 0, 0, 0 };
    const char *v = config.money;
    if (!v || !v[0]) return t;
    if (!strcasecmp(v, "off") || !strcasecmp(v, "no") || !strcasecmp(v, "false") ||
        !strcmp(v, "0"))
        return t;

    if (!strcasecmp(v, "random")) {
        /* "some random rupees": a different-looking wallet each launch, with
         * everything else maxed. */
        srand((unsigned)time(NULL) ^ (unsigned)getpid());
        t.wallet = 250000 + rand() % 750000;
        t.item = ITEM_MAX;
        t.enabled = 1;
        return t;
    }
    if (!strcasecmp(v, "max") || !strcasecmp(v, "on") || !strcasecmp(v, "yes") ||
        !strcasecmp(v, "true")) {
        t.wallet = WALLET_MAX;
        t.item = ITEM_MAX;
        t.enabled = 1;
        return t;
    }
    char *end = NULL;
    long n = strtol(v, &end, 10);
    if (*end || n <= 0) {
        debugPrintf("[Store] money: unrecognised value '%s' (use off, max, random or 1-%d); "
                    "treating as off\n", v, WALLET_MAX);
        return t;
    }
    if (n > WALLET_MAX) n = WALLET_MAX;
    t.wallet = n;
    t.item = ITEM_MAX;
    t.enabled = 1;
    return t;
}

/* --------------------------------------------------------------- rewriting */

/* Replace the digits after every known `"key":` with max(current, target).
 * Returns a malloc'd NUL-terminated string, or NULL if nothing changed. */
static char *rewrite_numbers(const char *in, size_t n, const Targets *t, size_t *out_len,
                             int *nchanged) {
    char *out = malloc(n + 64);
    if (!out) return NULL;
    size_t o = 0, i = 0;
    int changed = 0;

    while (i < n) {
        /* look for a quoted key */
        if (in[i] != '"') { out[o++] = in[i++]; continue; }

        size_t ks = i + 1, ke = ks;
        while (ke < n && in[ke] != '"' && in[ke] != '\n') ke++;
        if (ke >= n || in[ke] != '"') { out[o++] = in[i++]; continue; }

        size_t j = ke + 1;
        while (j < n && (in[j] == ' ' || in[j] == '\t')) j++;
        if (j >= n || in[j] != ':') {
            memcpy(out + o, in + i, ke + 1 - i);
            o += ke + 1 - i;
            i = ke + 1;
            continue;
        }

        char key[64];
        size_t kl = ke - ks;
        if (kl >= sizeof(key)) kl = sizeof(key) - 1;
        memcpy(key, in + ks, kl);
        key[kl] = 0;

        int is_wallet = is_wallet_key(key);
        int is_item = is_item_key(key);
        if (!is_wallet && !is_item) {                 /* copy the key verbatim */
            memcpy(out + o, in + i, ke + 1 - i);
            o += ke + 1 - i;
            i = ke + 1;
            continue;
        }

        size_t vs = j + 1;
        while (vs < n && (in[vs] == ' ' || in[vs] == '\t')) vs++;
        size_t ve = vs;
        while (ve < n && (isdigit((unsigned char)in[ve]) || in[ve] == '-')) ve++;
        if (ve == vs) {                               /* not a number: leave it */
            memcpy(out + o, in + i, ke + 1 - i);
            o += ke + 1 - i;
            i = ke + 1;
            continue;
        }

        long cur = strtol(in + vs, NULL, 10);
        long want = is_wallet ? t->wallet : t->item;
        if (cur < want) { cur = want; changed++; }

        /* copy `"key"` + `:` + spaces, then the number */
        memcpy(out + o, in + i, vs - i);
        o += vs - i;
        o += (size_t)sprintf(out + o, "%ld", cur);
        i = ve;
    }
    out[o] = 0;
    if (out_len) *out_len = o;
    if (nchanged) *nchanged = changed;
    return changed ? out : (free(out), NULL);
}

/* ------------------------------------------------------------------ file I/O */

static unsigned char *read_all(const char *path, size_t *len) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return NULL; }
    long sz = ftell(f);
    if (fseek(f, 0, SEEK_SET) != 0) { fclose(f); return NULL; }
    if (sz <= 0 || sz > 8 * 1024 * 1024) { fclose(f); return NULL; }
    unsigned char *b = malloc((size_t)sz + 16);
    if (b && fread(b, 1, (size_t)sz, f) != (size_t)sz) { free(b); b = NULL; }
    fclose(f);
    if (b) *len = (size_t)sz;
    return b;
}

static int write_all(const char *path, const void *buf, size_t len) {
    FILE *f = fopen(path, "wb");
    if (!f) return 0;
    size_t w = fwrite(buf, 1, len, f);
    return (fclose(f) == 0) && w == len;
}

static int exists(const char *p) { struct stat st; return stat(p, &st) == 0; }

/* End of the JSON object in a decrypted buffer. The engine writes this file
 * with a pad whose *filler* bytes are random (only the length byte is fixed),
 * so PKCS7 validation always fails -- but the plaintext is a JSON object that
 * starts with '{', and the engine itself parses up to the matching '}', so
 * locate that instead of trusting the trailer. */
static size_t json_end(const unsigned char *p, size_t n) {
    int depth = 0, in_str = 0, esc = 0;
    for (size_t i = 0; i < n; i++) {
        unsigned char c = p[i];
        if (in_str) {
            if (esc) esc = 0;
            else if (c == '\\') esc = 1;
            else if (c == '"') in_str = 0;
            continue;
        }
        if (c == '"') in_str = 1;
        else if (c == '{' || c == '[') depth++;
        else if (c == '}' || c == ']') {
            depth--;
            if (depth == 0) return i + 1;
        }
    }
    return 0;
}

/* <hex digest>GC<major> -- the RCS response cache in the save directory.
 * The digest length is not fixed (observed 41 hex chars in the 8.0.3 build), so
 * find the "GC" marker instead of assuming a byte offset: G and C are not hex
 * digits, so the first occurrence marks the end of the prefix. */
static int looks_like_cache(const char *name) {
    size_t n = strlen(name);
    if (n < 44) return 0;
    const char *gc = strstr(name, "GC");
    if (!gc) return 0;
    size_t prefix = (size_t)(gc - name);
    if (prefix < 40 || prefix > 48) return 0;
    for (size_t i = 0; i < prefix; i++)
        if (!isxdigit((unsigned char)name[i])) return 0;
    return 1;
}

static int patch_one(const char *path, const Targets *t) {
    size_t elen = 0;
    unsigned char *enc = read_all(path, &elen);
    if (!enc) return 0;
    if (elen < 16 || (elen % 16) != 0) { free(enc); return 0; }

    unsigned char *pt = malloc(elen + 1);
    if (!pt) { free(enc); return 0; }
    memcpy(pt, enc, elen);

    struct AES_ctx aes;
    uint8_t iv[16] = { 0 };
    AES_init_ctx_iv(&aes, SAVE_KEY, iv);
    AES_CBC_decrypt_buffer(&aes, pt, elen);

    size_t tlen = pt[0] == '{' ? json_end(pt, elen) : 0;
    if (!tlen) {
        debugPrintf("[Store] %s: not a decryptable RCS cache, left alone\n", path);
        free(pt); free(enc); return 0;
    }
    pt[tlen] = 0;

    size_t nlen = 0; int changed = 0;
    char *txt = rewrite_numbers((const char *)pt, tlen, t, &nlen, &changed);
    free(pt);
    if (!txt) {
        debugPrintf("[Store] %s: wallet already at %ld, nothing to do\n", path, t->wallet);
        free(enc);
        return 0;
    }

    size_t npad = 16 - (nlen % 16), clen = nlen + npad;
    unsigned char *ct = malloc(clen);
    if (!ct) { free(txt); free(enc); return 0; }
    memcpy(ct, txt, nlen);
    memset(ct + nlen, (int)npad, npad);
    free(txt);

    uint8_t iv2[16] = { 0 };
    AES_init_ctx_iv(&aes, SAVE_KEY, iv2);
    AES_CBC_encrypt_buffer(&aes, ct, clen);

    char bak[PATHMAX + 8], tmp[PATHMAX + 8];
    snprintf(bak, sizeof bak, "%s.bak", path);
    snprintf(tmp, sizeof tmp, "%s.tmp", path);
    if (!exists(bak) && !write_all(bak, enc, elen)) {
        debugPrintf("[Store] cannot back up %s, left alone\n", path);
        free(ct); free(enc); return 0;
    }

    int done = 0;
    if (write_all(tmp, ct, clen)) {
        remove(path);
        if (rename(tmp, path) == 0) {
            done = 1;
            debugPrintf("[Store] %s: %d entr%s topped up (wallet %ld, items %ld)\n",
                        path, changed, changed == 1 ? "y" : "ies", t->wallet, t->item);
        } else {
            debugPrintf("[Store] rename %s -> %s failed (backup at %s)\n", tmp, path, bak);
        }
    } else {
        remove(tmp);
        debugPrintf("[Store] cannot write %s, left alone\n", tmp);
    }
    free(ct); free(enc);
    return done;
}

int store_patch_init(void) {
    Targets t = targets_from_config();
    if (!t.enabled) {
        debugPrintf("[Store] money: off\n");
        return 0;
    }

    DIR *d = opendir(DATA_DIR);
    if (!d) {
        debugPrintf("[Store] cannot open %s\n", DATA_DIR);
        return 0;
    }

    int patched = 0, seen = 0;
    struct dirent *e;
    while ((e = readdir(d))) {
        if (!looks_like_cache(e->d_name)) continue;
        seen++;
        char path[PATHMAX];
        snprintf(path, sizeof path, "%s/%s", DATA_DIR, e->d_name);
        patched += patch_one(path, &t);
    }
    closedir(d);

    if (!seen)
        debugPrintf("[Store] no RCS inventory cache in %s yet "
                    "(launch the game once so it can create one)\n", DATA_DIR);
    return patched;
}
