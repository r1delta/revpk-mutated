/**
 * keyvalues.cpp
 *
 * Implementation of the LoadKeyValuesManifest function using Tyti's VDF parser.
 */

#include "keyvalues.h"
#include <fstream>
#include <iostream>
#include <filesystem>

bool LoadKeyValuesManifest(const std::string& vdfPath, std::vector<VPKKeyValues_t>& outList)
{
    namespace fs = std::filesystem;
    if (!fs::exists(vdfPath))
    {
        std::cerr << "[ReVPK] WARNING: Manifest file doesn't exist: " << vdfPath << "\n";
        return false;
    }

    std::ifstream ifs(vdfPath);
    if (!ifs.good())
    {
        std::cerr << "[ReVPK] WARNING: Cannot open manifest file: " << vdfPath << "\n";
        return false;
    }

    // parse top-level using Tyti
    tyti::vdf::Options opt;
    // We want a single object
    tyti::vdf::object doc = tyti::vdf::read<tyti::vdf::object>(ifs, opt);

    // We expect doc.name == "BuildManifest" if user followed the structure
    // The actual children are the filenames
    if (doc.name != "BuildManifest")
    {
        std::cerr << "[ReVPK] INFO: The top-level VDF object is named '" << doc.name 
                  << "' but we expect 'BuildManifest'. Proceeding anyway.\n";
    }

    // For each child => that's presumably our file
    // Each child is named like "path/to/file"
    for (auto& childPair : doc.childs)
    {
        auto& child = *childPair.second; // the actual node
        VPKKeyValues_t val;
        val.m_EntryPath = child.name; // the "key"

        // parse sub-attributes
        // e.g. child.attribs["preloadSize"], etc.
        auto itPreload = child.attribs.find("preloadSize");
        if (itPreload != child.attribs.end())
            val.m_iPreloadSize = static_cast<uint16_t>(std::stoi(itPreload->second));

        auto itLoadFlags = child.attribs.find("loadFlags");
        if (itLoadFlags != child.attribs.end())
            val.m_nLoadFlags = static_cast<uint32_t>(std::stoul(itLoadFlags->second));

        auto itTexFlags = child.attribs.find("textureFlags");
        if (itTexFlags != child.attribs.end())
            val.m_nTextureFlags = static_cast<uint16_t>(std::stoul(itTexFlags->second));

        auto itCompression = child.attribs.find("useCompression");
        if (itCompression != child.attribs.end())
            val.m_bUseCompression = (std::stoi(itCompression->second) != 0);

        auto itDeduplicate = child.attribs.find("deDuplicate");
        if (itDeduplicate != child.attribs.end())
            val.m_bDeduplicate = (std::stoi(itDeduplicate->second) != 0);

        outList.push_back(val);
    }

    return true;
}
