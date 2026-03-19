#ifndef DATA_ASSETS_H
#define DATA_ASSETS_H

#include "core/types.h"
#include "embedded_assets.h"

/* Direct access macros — no branch, no out-pointer overhead.
 * Use ASSET_DATA(ASSET_FOO) and ASSET_SIZE(ASSET_FOO) instead of asset_get().
 * For parameterized families: ASSET_DATA(ASSET_FAMILY(n)) / ASSET_SIZE(ASSET_FAMILY(n)).
 */
#define ASSET_DATA(id) ((const uint8_t *)embedded_assets[id].data)
#define ASSET_SIZE(id) ((size_t)*embedded_assets[id].size_ptr)

#endif /* DATA_ASSETS_H */
