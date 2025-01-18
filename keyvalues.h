/**
 * keyvalues.h
 *
 * Provides a function to load a "BuildManifest" from a .vdf file
 * using Tyti's VDF parser. We store the results into a list of
 * VPKKeyValues_t. This replicates the original "KeyValues" usage.
 */

#ifndef KEYVALUES_H
#define KEYVALUES_H

#include <string>
#include <vector>
#include "packedstore.h"

// We can include Tyti's VDF parser. E.g. if you have "tyti_vdf_parser.h"
#include "tyti_vdf_parser.h"

// ------------------------------------------------------------------
// LoadKeyValuesManifest:
//  Expects a top-level object "BuildManifest" with multiple children.
//   Each child’s name = local filesystem path
//   Contains fields:
//     "preloadSize"
//     "loadFlags"
//     "textureFlags"
//     "useCompression"
//     "deDuplicate"
// ------------------------------------------------------------------
bool LoadKeyValuesManifest(const std::string& vdfPath, std::vector<VPKKeyValues_t>& outList);

#endif // KEYVALUES_H
