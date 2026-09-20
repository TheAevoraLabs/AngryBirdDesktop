#ifndef ANGRYBIRDS_ASSET_MANAGER_H
#define ANGRYBIRDS_ASSET_MANAGER_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct AAssetManager;
typedef struct AAssetManager AAssetManager;

struct AAsset;
typedef struct AAsset AAsset;

struct AAssetDir;
typedef struct AAssetDir AAssetDir;

enum {
    AASSET_MODE_UNKNOWN   = 0,
    AASSET_MODE_RANDOM    = 1,
    AASSET_MODE_STREAMING = 2,
    AASSET_MODE_BUFFER    = 3
};

void AAssetManager_setBasePath(const char* path);
AAssetManager* AAssetManager_fromJava(void* env, void* assetManager);

AAsset* AAssetManager_open(AAssetManager* mgr, const char* filename, int mode);
void AAsset_close(AAsset* asset);

int AAsset_read(AAsset* asset, void* buf, size_t count);
int64_t AAsset_seek(AAsset* asset, int64_t offset, int whence);
int64_t AAsset_seek64(AAsset* asset, int64_t offset, int whence);
int64_t AAsset_getLength(AAsset* asset);
int64_t AAsset_getLength64(AAsset* asset);
int64_t AAsset_getRemainingLength(AAsset* asset);
int64_t AAsset_getRemainingLength64(AAsset* asset);
const void* AAsset_getBuffer(AAsset* asset);
int AAsset_isAllocated(AAsset* asset);
int AAsset_openFileDescriptor(AAsset* asset, int64_t* outStart, int64_t* outLength);
int AAsset_openFileDescriptor64(AAsset* asset, int64_t* outStart, int64_t* outLength);

AAssetDir* AAssetManager_openDir(AAssetManager* mgr, const char* dirName);
const char* AAssetDir_getNextFileName(AAssetDir* assetDir);
void AAssetDir_rewind(AAssetDir* assetDir);
void AAssetDir_close(AAssetDir* assetDir);

#ifdef __cplusplus
}
#endif

#endif // ANGRYBIRDS_ASSET_MANAGER_H
