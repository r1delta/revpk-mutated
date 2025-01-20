/**
 * revpk.cpp
 *
 * Main driver for ReVPK tool, now fully implemented with external libraries
 * for hashing, KeyValues, etc.
 */
#include <cstring>
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
        << "  revpk pack <locale> <context> <levelName> [workspacePath] [buildPath] [numThreads] [compressLevel]\n"
        << "  revpk unpack <vpkFile> [outPath] [sanitize]\n"
        << "  revpk packmulti <context> <levelName> [workspacePath] [buildPath] [numThreads] [compressLevel]\n"
        << "  revpk unpackmulti <someDirFile> [outPath] [sanitize]\n\n"
        << "Examples:\n"
        << "  revpk pack english client mp_rr_box\n"
        << "  revpk packmulti client mp_rr_box\n"
        << "  revpk unpack englishclient_mp_rr_box.bsp.pak000_dir.vpk ship/ 1\n"
        << "  revpk unpackmulti englishclient_mp_rr_box.bsp.pak000_dir.vpk ship/ 1\n\n";
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
    std::cout << "[ReVPK] Packing took " << elapsedSec
     << " seconds.\n";
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

// Helper to guess language from the front of the filename.
// For example: "englishclient_mp_rr_box.bsp.pak000_dir.vpk" => "english"
// If not recognized, return "english" as default.
static std::string DetectLanguagePrefix(const std::string& filename)
{
    // You can define a list of known languages or do something smarter:
    static const std::vector<std::string> knownLangs = {
        "english", "french", "german", "italian", "spanish", "russian",
        "polish", "japanese", "korean", "tchinese", "portuguese" };

    for (auto& lang : knownLangs)
    {
        if (filename.rfind(lang, 0) == 0) // if file starts with <lang>
            return lang;
    }
    return "english"; // fallback
}

static bool LoadMultiLangManifest(const std::string& manifestFile,
    std::map<std::string, std::vector<VPKKeyValues_t>>& outLangMap)
{
    // 1) Parse the top-level VDF
    std::ifstream ifs(manifestFile);
    if (!ifs.good())
        return false;

    tyti::vdf::object root = tyti::vdf::read(ifs);

    // 2) For each child that is a language name:
    for (auto& langPair : root.childs)
    {
        const std::string& language = langPair.first; // "english", etc.
        tyti::vdf::object* pLangObj = langPair.second.get();

        // 2a) For each file path in that language
        for (auto& filePair : pLangObj->childs)
        {
            const std::string& filePath = filePair.first; // e.g. "root/gameinfo.txt"
            tyti::vdf::object* pFileObj = filePair.second.get();

            // read fields
            VPKKeyValues_t kv;
            kv.m_EntryPath = filePath;
            kv.m_iPreloadSize = (uint16_t)std::stoi(pFileObj->attribs["preloadSize"]);
            kv.m_nLoadFlags = (uint32_t)std::stoul(pFileObj->attribs["loadFlags"]);
            kv.m_nTextureFlags = (uint16_t)std::stoi(pFileObj->attribs["textureFlags"]);
            kv.m_bUseCompression = (pFileObj->attribs["useCompression"] == "1");
            kv.m_bDeduplicate = (pFileObj->attribs["deDuplicate"] == "1");
//if (strncmp(kv.m_EntryPath.c_str(), "sound/", 6) == 0) {
//    kv.m_bUseCompression = false;
//}
            outLangMap[language].push_back(kv);
        }
    }

    return true;
}

static void DoPackMulti(const std::vector<std::string>& args)
{
    // Expected usage:
    // revpk packmulti <context> <levelName> [workspace] [buildPath] [numThreads] [compressionLevel]
    // e.g. revpk packmulti client mp_rr_box ship vpk 4 uber

    if (args.size() < 4)
    {
        PrintUsage();
        return;
    }

    std::string context = args[2];
    std::string level = args[3];
    std::string workspace = (args.size() > 4) ? args[4] : "ship";
    std::string buildPath = (args.size() > 5) ? args[5] : "vpk";

    if (!workspace.empty() && workspace.back() != '/' && workspace.back() != '\\')
        workspace.push_back('/');
    if (!buildPath.empty() && buildPath.back() != '/' && buildPath.back() != '\\')
        buildPath.push_back('/');

    int numThreads = -1;
    if (args.size() > 6)
        numThreads = std::atoi(args[6].c_str());
    std::string compressLevel = (args.size() > 7) ? args[7] : "uber";

    std::cout << "[ReVPK] packmulti: context=" << context
        << " level=" << level
        << " workspace=" << workspace
        << " buildPath=" << buildPath
        << " numThreads=" << numThreads
        << " compressLevel=" << compressLevel << "\n";

    // 1) Load the multi-language manifest:
    // e.g. workspace/manifest/multiLangManifest.vdf
    namespace fs = std::filesystem;
    fs::path manifestPath = fs::path(workspace) / "manifest" / "multiLangManifest.vdf";
    std::map<std::string, std::vector<VPKKeyValues_t>> langFileMap;
    if (!LoadMultiLangManifest(manifestPath.string(), langFileMap))
    {
        std::cerr << "[ReVPK] ERROR: Could not load multiLangManifest: "
            << manifestPath << "\n";
        return;
    }

    // 2) Build a single .vpk data file
    // We'll create something like: client_mp_rr_box.bsp.pak000_000.vpk
    // using an empty locale in VPKPair_t
    VPKPair_t masterPair("", context.c_str(), level.c_str(), 0);
    fs::path masterDataFile = fs::path(buildPath) / masterPair.m_PackName;
    std::cout << "[ReVPK] master data file => " << masterDataFile << "\n";

    try
    {
        fs::create_directories(masterDataFile.parent_path());
    }
    catch (...)
    {
        std::cerr << "[ReVPK] ERROR: cannot create dir for "
            << masterDataFile.parent_path() << "\n";
        return;
    }

    std::ofstream ofsData(masterDataFile, std::ios::binary);
    if (!ofsData.is_open())
    {
        std::cerr << "[ReVPK] ERROR: cannot open " << masterDataFile
            << " for writing\n";
        return;
    }

    // 3) Initialize the CPackedStoreBuilder
    CPackedStoreBuilder builder;
    builder.InitLzEncoder(numThreads, compressLevel.c_str());

    // 4) We will gather "entry blocks" for each language separately.
    std::map<std::string, std::vector<VPKEntryBlock_t>> languageEntries;

    // For dedup, we use the builder's chunk-hash map across *all* languages
    // so that identical chunk data is only stored once.
    // We’ll keep writing to 'ofsData'.

    const uint16_t singlePackIndex = 0;  // we have only one data file
    std::unique_ptr<uint8_t[]> chunkBuf(new uint8_t[VPK_ENTRY_MAX_LEN]);
    std::unique_ptr<uint8_t[]> compBuf(new uint8_t[VPK_ENTRY_MAX_LEN]);

    size_t sharedBytes = 0;
    size_t sharedChunks = 0;

    // Iterate over each language in our loaded manifest
    for (auto& kvLang : langFileMap)
    {
        const std::string& language = kvLang.first;
        const std::vector<VPKKeyValues_t>& files = kvLang.second;

        // We'll accumulate entries for this language:
        std::vector<VPKEntryBlock_t> blocks;

        // For each file in this language:
        for (auto& fileKV : files)
        {
            // --- CHANGE #2 ---
            // Attempt to open from the language’s folder first
            std::string langPath = workspace + "content/" + language + "/" + fileKV.m_EntryPath;
            std::ifstream ifFile(langPath, std::ios::binary);

            if (!ifFile.good())
            {
                // fallback to English
                std::string engPath = workspace + "content/english/" + fileKV.m_EntryPath;
                ifFile.open(engPath, std::ios::binary);
                if (!ifFile.good())
                {
                    std::cerr << "[ReVPK] WARNING: Could not open either "
                              << langPath << " or " << engPath << "\n";
                    continue; // no file at all, skip
                }
            }

            // now ifFile is valid, proceed to read
            ifFile.seekg(0, std::ios::end);
            std::streamoff len = ifFile.tellg();
            ifFile.seekg(0, std::ios::beg);

            if (len <= 0)
            {
                std::cerr << "[ReVPK] WARNING: " << fileKV.m_EntryPath << " is empty.\n";
                continue;
            }

            std::unique_ptr<uint8_t[]> fileData(new uint8_t[size_t(len)]);
            ifFile.read(reinterpret_cast<char*>(fileData.get()), len);
            ifFile.close();
            // --- END CHANGE #2 ---

            // create a VPKEntryBlock
            VPKEntryBlock_t block(fileData.get(), size_t(len), 0,
                fileKV.m_iPreloadSize, singlePackIndex,
                fileKV.m_nLoadFlags, fileKV.m_nTextureFlags,
                fileKV.m_EntryPath.c_str());

            // For each chunk in this block:
            size_t memoryOffset = 0;
            for (auto& frag : block.m_Fragments)
            {
                // copy chunk to chunkBuf
                std::memcpy(chunkBuf.get(), fileData.get() + memoryOffset, frag.m_nUncompressedSize);

                // Compress if useCompression = true
                bool compressedOk = false;
                size_t compSize = frag.m_nUncompressedSize;
if (fileKV.m_bUseCompression)
{
    if (builder.IsUsingZSTD())
    {
        constexpr size_t markerSize = sizeof(R1D_marker);
        std::memcpy(compBuf.get(), &R1D_marker, markerSize);

        size_t zstdBound = ZSTD_compressBound(frag.m_nUncompressedSize);
        if (zstdBound + markerSize > VPK_ENTRY_MAX_LEN)
            zstdBound = VPK_ENTRY_MAX_LEN - markerSize;

        size_t zstdResult = ZSTD_compress(
            compBuf.get() + markerSize,
            zstdBound,
            chunkBuf.get(),
            frag.m_nUncompressedSize,
            6 // example compression level
        );

        if (!ZSTD_isError(zstdResult))
        {
            size_t totalZstdSize = zstdResult + markerSize;
            if (totalZstdSize < frag.m_nUncompressedSize)
            {
                compressedOk = true;
                compSize = totalZstdSize;
            }
            else
            {
                std::cerr << "[ReVPK] ZSTD compression resulted in larger size, using uncompressed data\n";
            }
        }
        else
        {
            std::cerr << "[ReVPK] ZSTD compression failed: " << ZSTD_getErrorName(zstdResult) << "\n";
        }
    }
}

                const uint8_t* finalPtr = compressedOk ? compBuf.get() : chunkBuf.get();
                size_t finalSize = compressedOk ? compSize : frag.m_nUncompressedSize;

                // --- Deduplication Logic ---
                std::string chunkHash = compute_sha1_hex(finalPtr, finalSize);
                auto it = builder.m_ChunkHashMap.find(chunkHash);
                if (it != builder.m_ChunkHashMap.end())
                {
                    // Existing chunk:
                    frag = it->second;
                    sharedBytes += frag.m_nUncompressedSize;
                    sharedChunks++;
                }
                else
                {
                    // New chunk:
                    // Physically write chunk
                    uint64_t writePos = static_cast<uint64_t>(ofsData.tellp());
                    frag.m_nPackFileOffset = writePos;
                    frag.m_nCompressedSize = finalSize;
                    ofsData.write(reinterpret_cast<const char*>(finalPtr), finalSize);

                    // Store in map
                    builder.m_ChunkHashMap[chunkHash] = frag;
                }

                memoryOffset += frag.m_nUncompressedSize;
            } // each chunk

            // Add final block to this language’s vector
            blocks.push_back(block);
        } // each file

        // Store the result
        languageEntries[language] = blocks;
    }

    // Done writing the data file
    ofsData.flush();
    ofsData.close();

    // 5) Build a .vpk directory for each language
    for (auto& pair : languageEntries)
    {
        const std::string& lang = pair.first;
        const auto& blocks = pair.second;

        // We define a VPKPair_t for the language:
        VPKPair_t langPair(lang.c_str(), context.c_str(), level.c_str(), 0);
        fs::path dirPath = fs::path(buildPath) / langPair.m_DirName;

        VPKDir_t dir;
        dir.BuildDirectoryFile(dirPath.string(), blocks);
    }

    std::cout << "[ReVPK] Done. Shared " << sharedBytes << " bytes across "
        << sharedChunks << " chunks.\n";
}

static void DoUnpackMulti(const std::vector<std::string>& args)
{
    // usage: revpk unpackmulti <someDirFile> [outPath] [sanitize? 0/1]
    if (args.size() < 3)
    {
        PrintUsage();
        return;
    }

    std::string fileName = args[2]; // e.g. "englishclient_mp_rr_box.bsp.pak000_dir.vpk"
    std::string outPath = (args.size() > 3) ? args[3] : "ship";
    bool sanitize = false;
    if (args.size() > 4)
        sanitize = (std::atoi(args[4].c_str()) != 0);

    if (!outPath.empty() && outPath.back() != '/' && outPath.back() != '\\')
        outPath.push_back('/');

    namespace fs = std::filesystem;
    fs::path dirPath = fs::path(fileName).parent_path();
    if (dirPath.empty()) dirPath = ".";

    // Step 1: Identify the "base name" ignoring the language prefix if present.
    // E.g. "englishclient_mp_rr_box.bsp.pak000_dir.vpk" => "client_mp_rr_box.bsp.pak000_dir.vpk"
    std::string baseFilename = fs::path(fileName).filename().string();
    std::string englishPrefix = "english";
    // We'll define a function or inline logic:
    static const std::vector<std::string> knownLangs = {
        "english", "french", "german", "italian", "spanish", "russian",
        "polish", "japanese", "korean", "tchinese", "portuguese"
    };
    for (auto& lang : knownLangs)
    {
        // If baseFilename starts with <lang>, remove it
        if (baseFilename.rfind(lang, 0) == 0)
        {
            baseFilename.erase(0, lang.size());
            break;
        }
    }

    // Step 2: Collect all matching _dir VPKs that contain baseFilename
    std::map<std::string, VPKDir_t> languageDirs;
    for (auto& entry : fs::directory_iterator(dirPath))
    {
        if (!entry.is_regular_file())
            continue;
        std::string fname = entry.path().filename().string();
        // a naive pattern check
        if (fname.find(baseFilename) != std::string::npos
            && fname.find("_dir.vpk") != std::string::npos)
        {
            // detect language prefix
            std::string detectedLang = "english"; // fallback
            for (auto& lang : knownLangs)
            {
                if (fname.rfind(lang, 0) == 0)
                {
                    detectedLang = lang;
                    break;
                }
            }
            // parse the dir
            VPKDir_t dirVpk(entry.path().string(), sanitize);
            if (!dirVpk.Failed())
            {
                languageDirs[detectedLang] = dirVpk;
            }
        }
    }

    if (languageDirs.empty())
    {
        std::cerr << "[ReVPK] ERROR: Found no matching language VPKs for "
            << fileName << "\n";
        return;
    }

    // Step 3: designate an English fallback. If none found, pick the first
    VPKDir_t* pEnglishDir = nullptr;
    {
        auto it = languageDirs.find("english");
        if (it != languageDirs.end())
            pEnglishDir = &it->second;
        else
            pEnglishDir = &languageDirs.begin()->second;
    }

    // Step 4: Create a CPackedStoreBuilder to decode
    CPackedStoreBuilder builder;
    builder.InitLzDecoder();

    // Step 5: Unpack "english" fully into outPath/content/english/
    std::string engOut = outPath + "content/english/";
    fs::create_directories(engOut);
    builder.UnpackStore(*pEnglishDir, engOut.c_str());

    // Step 6: For each other language, only unpack differences
    for (auto& kvLang : languageDirs)
    {
        if (&kvLang.second == pEnglishDir)
            continue; // skip english

        const std::string& lang = kvLang.first;
        VPKDir_t& dirVpk = (VPKDir_t&)kvLang.second;

        std::cout << "[ReVPK] Unpacking " << lang << " differences...\n";
        std::string langOutPath = outPath + "content/" + lang + "/";
        fs::create_directories(langOutPath);

        // We'll implement a function that does chunk-level or file-level compare
        builder.UnpackStoreDifferences(*pEnglishDir, dirVpk, engOut, langOutPath);
    }

    // Step 7: Potentially rebuild or create a multi-language manifest that captures
    // which files differ from English. This is optional but typically desirable.
    {
        namespace fs = std::filesystem;
        // Make sure to create the "manifest" folder under outPath
        fs::path manifestDir = fs::path(outPath) / "manifest";
        fs::create_directories(manifestDir);

        std::string multiLangPath = (manifestDir / "multiLangManifest.vdf").string();
        bool success = builder.BuildMultiLangManifest(languageDirs, multiLangPath);
        if (!success)
        {
            std::cerr << "[ReVPK] WARNING: Could not write multiLangUnpacked.vdf\n";
        }
        else
        {
            std::cout << "[ReVPK] Wrote multiLangUnpacked.vdf at " << multiLangPath << "\n";
        }
    }

    std::cout << "[ReVPK] UnpackMulti completed.\n";
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

    if (cmd == PACK_COMMAND)        DoPack(args);
    else if (cmd == UNPACK_COMMAND)      DoUnpack(args);
    else if (cmd == "packmulti")         DoPackMulti(args);
    else if (cmd == "unpackmulti")       DoUnpackMulti(args);
    else                                 PrintUsage();

    return 0;
}
