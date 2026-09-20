#include "asset_manager.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <unistd.h>
#include <sys/stat.h>

struct AAssetManager {
    char base_path[512];
};

struct AAsset {
    FILE* fp;
    size_t length;
    size_t offset;
    void* buffer;
};

struct AAssetDir {
    DIR* dir;
    struct dirent* current_entry;
};

static struct AAssetManager g_mgr = { "assets" };
char g_last_opened_asset[512] = {0};

static char* pjoin(char* dst, size_t dsz, const char* a, const char* b) {
    size_t al = strlen(a);
    while (al > 0 && a[al-1] == '/') al--;
    while (*b == '/') b++;
    snprintf(dst, dsz, "%.*s/%s", (int)al, a, b);
    return dst;
}

static FILE* try_open_asset(const char* fullpath) {
    FILE* f = fopen(fullpath, "rb");
    if (f) return f;
    return NULL;
}

void AAssetManager_setBasePath(const char* path) {
    if (!path) return;
    strncpy(g_mgr.base_path, path, sizeof(g_mgr.base_path) - 1);
    g_mgr.base_path[sizeof(g_mgr.base_path) - 1] = '\0';
    printf("[AssetMgr] Base path configured to: %s\n", g_mgr.base_path);
}

AAssetManager* AAssetManager_fromJava(void* env, void* assetManager) {
    (void)env;
    (void)assetManager;
    return &g_mgr;
}

AAsset* AAssetManager_open(AAssetManager* mgr, const char* filename, int mode) {
    (void)mode;
    if (!mgr) mgr = &g_mgr;
    if (!filename || !filename[0]) return NULL;

    strncpy(g_last_opened_asset, filename, sizeof(g_last_opened_asset) - 1);

    char fullpath[1024];
    FILE* fp = NULL;

    // 1. Try directly under base_path (e.g. assets/<filename>)
    pjoin(fullpath, sizeof(fullpath), mgr->base_path, filename);
    fp = try_open_asset(fullpath);

    // 2. Try under assets/data/<filename>
    if (!fp) {
        char subpath[1024];
        snprintf(subpath, sizeof(subpath), "%s/data", mgr->base_path);
        pjoin(fullpath, sizeof(fullpath), subpath, filename);
        fp = try_open_asset(fullpath);
    }

    // 3. Try under raw filename (if already prefixed with assets/)
    if (!fp) {
        fp = try_open_asset(filename);
    }

    if (!fp) {
        printf("[AssetMgr] File not found: %s\n", filename);
        return NULL;
    }

    fseek(fp, 0, SEEK_END);
    long len = ftell(fp);
    fseek(fp, 0, SEEK_SET);

    printf("[AssetMgr] AAssetManager_open: %s (found at %s, size=%ld)\n", filename, fullpath, len);

    AAsset* asset = (AAsset*)calloc(1, sizeof(AAsset));
    if (!asset) {
        fclose(fp);
        return NULL;
    }

    asset->fp = fp;
    asset->length = (size_t)len;
    asset->offset = 0;
    asset->buffer = NULL;

    return asset;
}

void AAsset_close(AAsset* asset) {
    if (!asset) return;
    printf("[AssetMgr] AAsset_close (length=%zu)\n", asset->length);
    if (asset->fp) {
        fclose(asset->fp);
        asset->fp = NULL;
    }
    // Note: Do NOT free asset->buffer here. Callers of AAsset_getBuffer (like Texture/Font loaders)
    // retain and use buffer pointers after AAsset_close() has been called.
    free(asset);
}

int AAsset_read(AAsset* asset, void* buf, size_t count) {
    if (!asset || !asset->fp || !buf) return -1;
    size_t bytes_read = fread(buf, 1, count, asset->fp);
    asset->offset += bytes_read;
    printf("[AssetMgr] AAsset_read: requested=%zu, read=%zu\n", count, bytes_read);
    return (int)bytes_read;
}

int64_t AAsset_seek(AAsset* asset, int64_t offset, int whence) {
    if (!asset || !asset->fp) return -1;
    if (fseek(asset->fp, (long)offset, whence) != 0) return -1;
    asset->offset = (size_t)ftell(asset->fp);
    return (int64_t)asset->offset;
}

int64_t AAsset_seek64(AAsset* asset, int64_t offset, int whence) {
    return AAsset_seek(asset, offset, whence);
}

int64_t AAsset_getLength(AAsset* asset) {
    if (!asset) return -1;
    return (int64_t)asset->length;
}

int64_t AAsset_getLength64(AAsset* asset) {
    return AAsset_getLength(asset);
}

int64_t AAsset_getRemainingLength(AAsset* asset) {
    if (!asset) return -1;
    if (asset->offset >= asset->length) return 0;
    return (int64_t)(asset->length - asset->offset);
}

int64_t AAsset_getRemainingLength64(AAsset* asset) {
    return AAsset_getRemainingLength(asset);
}

const void* AAsset_getBuffer(AAsset* asset) {
    if (!asset || !asset->fp) return NULL;
    if (asset->buffer) return asset->buffer;

    asset->buffer = malloc(asset->length + 1);
    if (!asset->buffer) return NULL;

    long cur = ftell(asset->fp);
    fseek(asset->fp, 0, SEEK_SET);
    size_t read_bytes = fread(asset->buffer, 1, asset->length, asset->fp);
    ((char*)asset->buffer)[read_bytes] = '\0';
    fseek(asset->fp, cur, SEEK_SET);

    printf("[AssetMgr] AAsset_getBuffer: %p (length=%zu)\n", asset->buffer, asset->length);
    return asset->buffer;
}

int AAsset_isAllocated(AAsset* asset) {
    if (!asset) return 0;
    return (asset->buffer != NULL);
}

int AAsset_openFileDescriptor(AAsset* asset, int64_t* outStart, int64_t* outLength) {
    if (!asset || !asset->fp) return -1;
    if (outStart) *outStart = 0;
    if (outLength) *outLength = (int64_t)asset->length;
    return fileno(asset->fp);
}

int AAsset_openFileDescriptor64(AAsset* asset, int64_t* outStart, int64_t* outLength) {
    return AAsset_openFileDescriptor(asset, outStart, outLength);
}

AAssetDir* AAssetManager_openDir(AAssetManager* mgr, const char* dirName) {
    if (!mgr) mgr = &g_mgr;
    char path[1024];
    pjoin(path, sizeof(path), mgr->base_path, dirName ? dirName : "");

    DIR* d = opendir(path);
    if (!d) return NULL;

    AAssetDir* assetDir = (AAssetDir*)calloc(1, sizeof(AAssetDir));
    if (!assetDir) {
        closedir(d);
        return NULL;
    }
    assetDir->dir = d;
    return assetDir;
}

const char* AAssetDir_getNextFileName(AAssetDir* assetDir) {
    if (!assetDir || !assetDir->dir) return NULL;
    while ((assetDir->current_entry = readdir(assetDir->dir)) != NULL) {
        const char* name = assetDir->current_entry->d_name;
        if (strcmp(name, ".") != 0 && strcmp(name, "..") != 0) {
            return name;
        }
    }
    return NULL;
}

void AAssetDir_rewind(AAssetDir* assetDir) {
    if (!assetDir || !assetDir->dir) return;
    rewinddir(assetDir->dir);
}

void AAssetDir_close(AAssetDir* assetDir) {
    if (!assetDir) return;
    if (assetDir->dir) closedir(assetDir->dir);
    free(assetDir);
}
