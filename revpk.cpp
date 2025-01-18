/**
 * revpk.cpp
 *
 * Main driver for ReVPK tool, now fully implemented with external libraries
 * for hashing, KeyValues, etc.
 */

#include <iostream>
#include <string>
#include <vector>
#include <filesystem>
#include <cstdlib>
#include <chrono>
#include "packedstore.h"
#include "keyvalues.h"  // Our Tyti-based VDF KeyValues interface

// For convenience
static const std::string PACK_COMMAND   = "pack";
static const std::string UNPACK_COMMAND = "unpack";

static void PrintUsage()
{
    std::cout << "Usage:\n\n"
    << "  revpk pack <locale> <context> <levelName> [workspacePath] [buildPath] [numThreads] [compressLevel]\n\n"
    << "  revpk unpack <vpkFile> [outPath] [sanitize]\n\n"
    << "Examples:\n"
    << "  revpk pack english client mp_rr_box\n"
    << "  revpk unpack englishclient_mp_rr_box.bsp.pak000_dir.vpk ship/ 1\n\n";
}

static void DoPack(const std::vector<std::string>& args)
{
    if (args.size() < 5)
    {
        PrintUsage();
        return;
    }

    // gather arguments
    std::string locale = args[2];
    std::string context = args[3];
    std::string level   = args[4];

    std::string workspace = (args.size() > 5) ? args[5] : "ship";
    std::string buildPath = (args.size() > 6) ? args[6] : "vpk";

    if (!workspace.empty() && workspace.back() != '/' && workspace.back() != '\\')
        workspace.push_back('/');
    if (!buildPath.empty() && buildPath.back() != '/' && buildPath.back() != '\\')
        buildPath.push_back('/');

    int numThreads = -1;
    if (args.size() > 7)
        numThreads = std::atoi(args[7].c_str());
    std::string compressLevel = (args.size() > 8) ? args[8] : "uber";
    std::cout << " locale: " << locale << " context: " << context << " level: " << level << " workspace: " << workspace << " buildPath: " << buildPath << " numThreads: " << numThreads << " compressLevel: " << compressLevel << std::endl;

    // For timing
    auto start = std::chrono::steady_clock::now();

    // create a builder
    CPackedStoreBuilder builder;
    builder.InitLzEncoder(numThreads, compressLevel.c_str());

    // Construct VPKPair
    VPKPair_t pair(locale.c_str(), context.c_str(), level.c_str(), 0);

    std::cout << "[ReVPK] PACK: " << pair.m_DirName << "\n";

    // Actually run pack
    builder.PackStore(pair, workspace.c_str(), buildPath.c_str());

    auto end = std::chrono::steady_clock::now();
    double elapsedSec = std::chrono::duration<double>(end - start).count();
    std::cout << "[ReVPK] Packing took " << elapsedSec << " seconds.\n";
}

static void DoUnpack(const std::vector<std::string>& args)
{
    if (args.size() < 3)
    {
        PrintUsage();
        return;
    }

    std::string fileName = args[2];
    std::string outPath  = (args.size() > 3) ? args[3] : "ship";
    bool sanitize        = false;
    if (args.size() > 4)
        sanitize = (std::atoi(args[4].c_str()) != 0);

    if (!outPath.empty() && outPath.back() != '/' && outPath.back() != '\\')
        outPath.push_back('/');

    auto start = std::chrono::steady_clock::now();

    // parse directory
    VPKDir_t vpkDir(fileName, sanitize);
    if (vpkDir.Failed())
    {
        std::cerr << "[ReVPK] ERROR: Could not parse VPK directory: " << fileName << "\n";
        return;
    }

    // create a builder
    CPackedStoreBuilder builder;
    builder.InitLzDecoder();

    std::cout << "[ReVPK] UNPACK: " << fileName << "\n";
    builder.UnpackStore(vpkDir, outPath.c_str());

    auto end = std::chrono::steady_clock::now();
    double elapsedSec = std::chrono::duration<double>(end - start).count();
    std::cout << "[ReVPK] Unpacking took " << elapsedSec << " seconds.\n";
}

int main(int argc, char* argv[])
{
    if (argc < 2)
    {
        PrintUsage();
        return 0;
    }
    std::vector<std::string> args;
    args.reserve(argc);
    for (int i = 0; i < argc; ++i)
        args.push_back(argv[i]);

    const std::string& cmd = args[1];
    if      (cmd == PACK_COMMAND)   DoPack(args);
    else if (cmd == UNPACK_COMMAND) DoUnpack(args);
    else                            PrintUsage();

    return 0;
}
