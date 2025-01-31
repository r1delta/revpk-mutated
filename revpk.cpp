/**
 * revpk.cpp
 *
 * Main driver for ReVPK tool, now with multithreading support for packmulti and unpackmulti.
 */
#include <cstring>
#include <iostream>
#include <string>
#include <vector>
#include <filesystem>
#include <cstdlib>
#include <chrono>
// Added for multithreading:
#include <thread>
#include <future>
#include <mutex>
#include <set>
#include <atomic>

#include "packedstore.h"
#include "keyvalues.h"  // Our Tyti-based VDF KeyValues interface

// For convenience
static const std::string PACK_COMMAND       = "pack";
static const std::string UNPACK_COMMAND     = "unpack";

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
            outLangMap[language].push_back(kv);
        }
    }

    return true;
}

/**
 * DoPackMulti() – Multi-threaded version
 *
 * Instead of processing each language sequentially, we launch an asynchronous task for every file.
 * Each task reads its input file (first from workspace/content/<language> then falling back to English),
 * creates a VPKEntryBlock_t, compresses its chunks (if enabled), and writes deduplicated chunk data
 * into the shared master data file. A mutex protects access to the global deduplication map and the shared file.
 */
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

    // Determine thread count: default to (CPU cores - 1) if no thread count is provided
    unsigned int defaultThreads = std::thread::hardware_concurrency();
    if (defaultThreads > 0)
        defaultThreads = (defaultThreads > 1) ? defaultThreads - 1 : 1;
    else
        defaultThreads = 1;
    if (numThreads <= 0)
        numThreads = defaultThreads;

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

    // 2) Build a single master data file (e.g. client_mp_rr_box.bsp.pak000_000.vpk)
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

    // 3) Initialize the CPackedStoreBuilder (this instance will supply the shared deduplication map)
    CPackedStoreBuilder builder;
    builder.InitLzEncoder(numThreads, compressLevel.c_str());

    // Shared counters for statistics
    std::atomic<size_t> sharedBytes{0};
    std::atomic<size_t> sharedChunks{0};

    // Mutex to protect access to the global dedup map and ofsData
    std::mutex dedupMutex;

    // 4) For each language and each file therein, launch a task to process the file.
    // Each task returns a pair: <language, VPKEntryBlock_t>
    std::vector< std::future< std::pair<std::string, VPKEntryBlock_t> > > futures;
    // Reserve an estimated number of tasks (optional)
    size_t totalFiles = 0;
    for (auto& kv : langFileMap)
        totalFiles += kv.second.size();
    futures.reserve(totalFiles);

    // For each language:
    for (auto& langPair : langFileMap)
    {
        const std::string& language = langPair.first;
        const std::vector<VPKKeyValues_t>& files = langPair.second;
        for (const auto& fileKV : files)
        {
            // Launch an async task for each file
            futures.push_back(
                std::async(std::launch::async, [&, language, fileKV]() -> std::pair<std::string, VPKEntryBlock_t> {
                    // Each thread gets its own buffers
                    std::unique_ptr<uint8_t[]> localChunkBuf(new uint8_t[VPK_ENTRY_MAX_LEN]);
                    std::unique_ptr<uint8_t[]> localCompBuf(new uint8_t[VPK_ENTRY_MAX_LEN]);

                    // Attempt to open file from workspace/content/<language> first
                    std::string filePath = workspace + "content/" + language + "/" + fileKV.m_EntryPath;
                    std::ifstream ifFile(filePath, std::ios::binary);
                    if (!ifFile.good())
                    {
                        // fallback to English
                        filePath = workspace + "content/english/" + fileKV.m_EntryPath;
                        ifFile.open(filePath, std::ios::binary);
                        if (!ifFile.good())
                        {
                            std::cerr << "[ReVPK] WARNING: Could not open file " << filePath << "\n";
                            return std::make_pair(language, VPKEntryBlock_t());
                        }
                    }

                    ifFile.seekg(0, std::ios::end);
                    std::streamoff len = ifFile.tellg();
                    ifFile.seekg(0, std::ios::beg);
                    if (len <= 0)
                    {
                        std::cerr << "[ReVPK] WARNING: " << fileKV.m_EntryPath << " is empty.\n";
                        return std::make_pair(language, VPKEntryBlock_t());
                    }
                    std::vector<uint8_t> fileData((size_t)(len));
                    ifFile.read(reinterpret_cast<char*>(fileData.data()), len);
                    ifFile.close();

                    // Create entry block (using single pack index 0)
                    VPKEntryBlock_t block(fileData.data(), size_t(len), 0,
                                            fileKV.m_iPreloadSize, 0,
                                            fileKV.m_nLoadFlags, fileKV.m_nTextureFlags,
                                            fileKV.m_EntryPath.c_str());

                    size_t memoryOffset = 0;
                    for (auto& frag : block.m_Fragments)
                    {
                        // Copy the fragment’s uncompressed data to localChunkBuf
                        std::memcpy(localChunkBuf.get(),
                                    fileData.data() + memoryOffset,
                                    frag.m_nUncompressedSize);

                        // --- Attempt compression if enabled ---
                        bool compressedOk = false;
                        size_t compSize = frag.m_nUncompressedSize;
                        if (fileKV.m_bUseCompression && builder.IsUsingZSTD())
                        {
                            constexpr size_t markerSize = sizeof(R1D_marker);
                            std::memcpy(localCompBuf.get(), &R1D_marker, markerSize);
                            size_t zstdBound = ZSTD_compressBound(frag.m_nUncompressedSize);
                            if (zstdBound + markerSize > VPK_ENTRY_MAX_LEN)
                                zstdBound = VPK_ENTRY_MAX_LEN - markerSize;
                            size_t zstdResult = ZSTD_compress(
                                localCompBuf.get() + markerSize,
                                zstdBound,
                                localChunkBuf.get(),
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
                            }
                        }
                        // Decide which buffer to use for this fragment’s final data.
                        const uint8_t* finalPtr = compressedOk ? localCompBuf.get() : localChunkBuf.get();
                        size_t finalSize = compressedOk ? compSize : frag.m_nUncompressedSize;

                        // Compute SHA1 hash for deduplication.
                        std::string chunkHash = compute_sha1_hex(finalPtr, finalSize);
                        {
                            std::lock_guard<std::mutex> lock(dedupMutex);
                            auto it = builder.m_ChunkHashMap.find(chunkHash);
                            if (it != builder.m_ChunkHashMap.end())
                            {
                                // Found an existing chunk: reuse its descriptor.
                                frag = it->second;
                                sharedBytes += frag.m_nUncompressedSize;
                                sharedChunks++;
                            }
                            else
                            {
                                // New chunk: determine file offset and write the chunk.
                                uint64_t writePos = static_cast<uint64_t>(ofsData.tellp());
                                frag.m_nPackFileOffset = writePos;
                                frag.m_nCompressedSize = finalSize;
                                ofsData.write(reinterpret_cast<const char*>(finalPtr), finalSize);
                                builder.m_ChunkHashMap[chunkHash] = frag;
                            }
                        }
                        memoryOffset += frag.m_nUncompressedSize;
                    } // for each fragment

                    return std::make_pair(language, block);
                })
            );
        } // for each file in this language
    } // for each language

    // 5) Gather the results as they complete.
    std::map<std::string, std::vector<VPKEntryBlock_t>> languageEntries;
    for (auto& fut : futures)
    {
        auto result = fut.get();
        // Only add valid blocks (non-empty entry paths) to the final result.
        if (!result.second.m_EntryPath.empty())
            languageEntries[result.first].push_back(result.second);
    }

    ofsData.flush();
    ofsData.close();

    std::cout << "[ReVPK] Done writing master data file. Shared " << sharedBytes.load() << " bytes across "
              << sharedChunks.load() << " chunks.\n";

    // 6) Build a .vpk directory file for each language.
    for (auto& pair : languageEntries)
    {
        VPKPair_t langPair(pair.first.c_str(), context.c_str(), level.c_str(), 0);
        fs::path dirPath = fs::path(buildPath) / langPair.m_DirName;
        VPKDir_t dir;
        dir.BuildDirectoryFile(dirPath.string(), pair.second);
    }
}

/**
 * DoUnpackMulti() – Multi-threaded version
 *
 * First, the English VPK is fully unpacked. Then, for each other language a separate thread is
 * launched (each with its own CPackedStoreBuilder/decoder) to unpack only the differences.
 */
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

    // Step 1: Identify the "base name" ignoring any language prefix.
    std::string baseFilename = fs::path(fileName).filename().string();
    static const std::vector<std::string> knownLangs = {
        "english", "french", "german", "italian", "spanish", "russian",
        "polish", "japanese", "korean", "tchinese", "portuguese"
    };
    for (auto& lang : knownLangs)
    {
        if (baseFilename.rfind(lang, 0) == 0)
        {
            baseFilename.erase(0, lang.size());
            break;
        }
    }

    // Step 2: Collect all matching _dir VPK files from the directory.
    std::map<std::string, VPKDir_t> languageDirs;
    for (auto& entry : fs::directory_iterator(dirPath))
    {
        if (!entry.is_regular_file())
            continue;
        std::string fname = entry.path().filename().string();
        if (fname.find(baseFilename) != std::string::npos
            && fname.find("_dir.vpk") != std::string::npos)
        {
            // Detect language prefix (default to English if not found).
            std::string detectedLang = "english";
            for (auto& lang : knownLangs)
            {
                if (fname.rfind(lang, 0) == 0)
                {
                    detectedLang = lang;
                    break;
                }
            }
            VPKDir_t dirVpk(entry.path().string(), sanitize);
            if (!dirVpk.Failed())
            {
                languageDirs[detectedLang] = dirVpk;
            }
        }
    }

    if (languageDirs.empty())
    {
        std::cerr << "[ReVPK] ERROR: Found no matching language VPKs for " << fileName << "\n";
        return;
    }

    // Step 3: Designate an English fallback. If none is found, pick the first.
    VPKDir_t* pEnglishDir = nullptr;
    {
        auto it = languageDirs.find("english");
        if (it != languageDirs.end())
            pEnglishDir = &it->second;
        else
            pEnglishDir = &languageDirs.begin()->second;
    }

    // Step 4: Create a CPackedStoreBuilder to decode and unpack English fully.
    CPackedStoreBuilder builder;
    builder.InitLzDecoder();

    std::string engOut = outPath + "content/english/";
    fs::create_directories(engOut);
    builder.UnpackStore(*pEnglishDir, engOut.c_str());

    // Step 5: For each other language, concurrently unpack only differences.
    std::vector<std::future<void>> unpackFutures;
    for (auto& kvLang : languageDirs)
    {
        if (kvLang.first == "english")
            continue;
        // Capture language and a copy of the VPKDir_t for safe use in the lambda.
        std::string lang = kvLang.first;
        VPKDir_t langDir = kvLang.second;
        unpackFutures.push_back(
            std::async(std::launch::async, [&, lang, langDir]()
            {
                CPackedStoreBuilder localBuilder;
                localBuilder.InitLzDecoder();
                std::string langOutPath = outPath + "content/" + lang + "/";
                fs::create_directories(langOutPath);
                localBuilder.UnpackStoreDifferences(*pEnglishDir, langDir, engOut, langOutPath);
                std::cout << "[ReVPK] Unpacked differences for " << lang << "\n";
            })
        );
    }
    // Wait for all tasks to finish.
    for (auto& fut : unpackFutures)
        fut.get();

    // Optional: rebuild multi-language manifest
    {
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

    if (cmd == PACK_COMMAND)              DoPack(args);
    else if (cmd == UNPACK_COMMAND)         DoUnpack(args);
    else if (cmd == "packmulti")            DoPackMulti(args);
    else if (cmd == "unpackmulti")          DoUnpackMulti(args);
    else                                  PrintUsage();

    return 0;
}
