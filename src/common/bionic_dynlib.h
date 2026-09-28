/* bionic_dynlib.h -- the Android/Bionic imports every Fusion title needs.
 *
 * The engine links against Bionic's libc rather than glibc, so a handful of
 * symbols (the ctype tables, __sF, memalign, the log/asset HIPEs) have to be
 * satisfied by the host. The list is identical across titles, which is why it
 * lives here rather than inside one game's main().
 *
 * This software may be modified and distributed under the terms of the MIT
 * license. See the LICENSE file for details.
 */

#ifndef BIONIC_DYNLIBS_H
#define BIONIC_DYNLIBS_H

#include <stddef.h>
#include "loader/so_util.h"

#ifdef __cplusplus
extern "C" {
#endif

/* The Bionic shim table, and its length, for so_resolve(). */
const so_default_dynlib *ab_bionic_dynlib(size_t *count);

#ifdef __cplusplus
}
#endif

#endif /* BIONIC_DYNLIBS_H */
