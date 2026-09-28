#ifndef STORE_PATCH_H
#define STORE_PATCH_H

/* store_patch.h -- optional "money" cheat from config.txt.
 *
 * Rovio Cloud Services caches the player's wallet in the save directory as an
 * AES-encrypted JSON blob whose file name is <sha1><GCn> (the name is a hash of
 * the cache key, so it is not a constant we can hardcode). The blob looks like:
 *
 *   {"Emblems":null,
 *    "Inventory":[{"coins":21},{"gems":0},{"tickets":0},{"shockwave":0}, ...],
 *    "actionMap":[{"coins":21}, ...],
 *    "checksum":"7FF4..."}
 *
 * With no server to talk to, everything the store sells is unpurchasable, so a
 * desktop player can never obtain coins/gems. `store_patch_init()` walks the
 * save directory, finds that file, and raises the wallet and every inventory
 * entry in place -- only ever upwards, and without touching the surrounding
 * structure or the checksum field.
 *
 * config.txt key: `money` = off | max | random | 1..9999999
 */

#ifdef __cplusplus
extern "C" {
#endif

/* Returns the number of cache files that were patched. */
int store_patch_init(void);

#ifdef __cplusplus
}
#endif

#endif /* STORE_PATCH_H */
