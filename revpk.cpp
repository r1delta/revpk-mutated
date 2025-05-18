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
#include <fcntl.h>      // open()

#include <unistd.h>     // pwrite(), close()

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
    std::cout << " locale: " << locale << " context: " << context << " level: " << level 
              << " workspace: " << workspace << " buildPath: " << buildPath 
              << " numThreads: " << numThreads << " compressLevel: " << compressLevel << std::endl;

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
            kv.m_EntryPath       = filePath;
            kv.m_iPreloadSize    = (uint16_t)std::stoi(pFileObj->attribs["preloadSize"]);
            kv.m_nLoadFlags      = (uint32_t)std::stoul(pFileObj->attribs["loadFlags"]);
            kv.m_nTextureFlags   = (uint16_t)std::stoi(pFileObj->attribs["textureFlags"]);
            kv.m_bUseCompression = (pFileObj->attribs["useCompression"] == "1");
            kv.m_bDeduplicate    = (pFileObj->attribs["deDuplicate"] == "1");
            outLangMap[language].push_back(kv);
        }
    }

    return true;
}

/**
 * DoPackMulti() – Multi-threaded version
 *
 * Instead of processing each language sequentially, we launch an asynchronous task for every file.
 */
// For demonstration, a global or static mutex.
static std::mutex fileIOMutex;

static void DoPackMulti(const std::vector<std::string>& args)
{
    // usage:
    //  revpk packmulti <context> <levelName> [workspace] [buildPath] [numThreads] [compressionLevel]

    if (args.size() < 4)
    {
        PrintUsage();
        return;
    }

    std::string context   = args[2];
    std::string level     = args[3];
    std::string workspace = (args.size() > 4) ? args[4] : "ship";
    std::string buildPath = (args.size() > 5) ? args[5] : "vpk";

    if (!workspace.empty() && (workspace.back() != '/' && workspace.back() != '\\'))
        workspace.push_back('/');
    if (!buildPath.empty() && (buildPath.back() != '/' && buildPath.back() != '\\'))
        buildPath.push_back('/');

    int numThreads = -1;
    if (args.size() > 6)
        numThreads = std::atoi(args[6].c_str());
    std::string compressLevel = (args.size() > 7) ? args[7] : "uber";

    unsigned int defaultThreads = std::thread::hardware_concurrency();
    if (defaultThreads == 0) defaultThreads = 1;
    if (defaultThreads > 1)  defaultThreads--; // leave 1 core free
    if (numThreads <= 0)     numThreads = defaultThreads;

    std::cout << "[ReVPK] packmulti: context=" << context
              << " level=" << level
              << " workspace=" << workspace
              << " buildPath=" << buildPath
              << " numThreads=" << numThreads
              << " compressLevel=" << compressLevel << "\n";

    // 1) Load multi-language manifest
    namespace fs = std::filesystem;
    fs::path manifestPath = fs::path(workspace) / "manifest" / "multiLangManifest.vdf";

    std::map<std::string, std::vector<VPKKeyValues_t>> langFileMap;
    if (!LoadMultiLangManifest(manifestPath.string(), langFileMap))
    {
        std::cerr << "[ReVPK] ERROR: Could not load multiLangManifest: " << manifestPath << "\n";
        return;
    }

    // 2) Create a single “master” data file
    VPKPair_t masterPair("", context.c_str(), level.c_str(), 0);
    fs::path masterDataFile = fs::path(buildPath) / masterPair.m_PackName;

    try
    {
        fs::create_directories(masterDataFile.parent_path());
    }
    catch (...)
    {
        std::cerr << "[ReVPK] ERROR: cannot create dir for " << masterDataFile.parent_path() << "\n";
        return;
    }

    std::ofstream ofsData(masterDataFile, std::ios::binary);
    if (!ofsData.is_open())
    {
        std::cerr << "[ReVPK] ERROR: cannot open " << masterDataFile << " for writing\n";
        return;
    }

    // 3) Prepare the CPackedStoreBuilder (which has dedup map)
    CPackedStoreBuilder builder;
    builder.InitLzEncoder(numThreads, compressLevel.c_str());

    std::atomic<size_t> sharedBytes{0};
    std::atomic<size_t> sharedChunks{0};

    std::mutex dedupMutex;
    std::mutex resultsMutex;
    std::map<std::string, std::vector<VPKEntryBlock_t>> languageEntries;

    // 4) Thread pool for compression tasks
    ThreadPool pool(numThreads);

    // For each language => for each file => compress+dedup
    for (auto& langPair : langFileMap)
    {
        const std::string& language = langPair.first;
        const auto& files           = langPair.second;

        for (auto& fileKV : files)
        {
            pool.enqueue([&, language, fileKV]()
            {
                // Per-task buffers for compression
                std::unique_ptr<uint8_t[]> chunkBuf(new uint8_t[VPK_ENTRY_MAX_LEN]);
                std::unique_ptr<uint8_t[]> compBuf(new uint8_t[VPK_ENTRY_MAX_LEN]);

                // Attempt to read file from workspace/<language>
                std::string path = workspace + "content/" + language + "/" + fileKV.m_EntryPath;
                std::ifstream ifFile(path, std::ios::binary);
                if (!ifFile.good())
                {
                    // fallback to english
                    path = workspace + "content/english/" + fileKV.m_EntryPath;
                    ifFile.open(path, std::ios::binary);
                    if (!ifFile.good())
                    {
                        std::cerr << "[ReVPK] WARNING: Could not open " << path << "\n";
                        return;
                    }
                }
                ifFile.seekg(0, std::ios::end);
                std::streamoff len = ifFile.tellg();
                ifFile.seekg(0, std::ios::beg);

                if (len <= 0)
                {
                    std::cerr << "[ReVPK] WARNING: empty file " << fileKV.m_EntryPath << "\n";
                    return;
                }
                std::vector<uint8_t> fileData(static_cast<size_t>(len));
                ifFile.read(reinterpret_cast<char*>(fileData.data()), len);
                ifFile.close();

                // Build an entry block
                VPKEntryBlock_t block(fileData.data(), fileData.size(), 
                                      0,  // offset assigned later
                                      fileKV.m_iPreloadSize, 0,
                                      fileKV.m_nLoadFlags,
                                      fileKV.m_nTextureFlags,
                                      fileKV.m_EntryPath.c_str());

                // Compress/deduplicate each fragment
                size_t filePos = 0; 
                for (auto& frag : block.m_Fragments)
                {
                    // Copy uncompressed data to chunkBuf
                    const size_t chunkSize = frag.m_nUncompressedSize;
                    std::memcpy(chunkBuf.get(), &fileData[filePos], chunkSize);
                    filePos += chunkSize;

                    // Attempt compression if desired
                    bool compressedOk = false;
                    size_t compSize   = chunkSize;
                    const uint8_t* finalPtr = chunkBuf.get();

                    if (fileKV.m_bUseCompression && builder.IsUsingZSTD())
                    {
                        constexpr size_t markerSize = sizeof(R1D_marker);
                        std::memcpy(compBuf.get(), &R1D_marker, markerSize);

                        size_t zstdBound = ZSTD_compressBound(chunkSize);
                        if (zstdBound + markerSize > VPK_ENTRY_MAX_LEN)
                            zstdBound = VPK_ENTRY_MAX_LEN - markerSize;

                        size_t zstdResult = ZSTD_compress(
                            compBuf.get() + markerSize,
                            zstdBound,
                            chunkBuf.get(),
                            chunkSize,
                            6 // example ZSTD level
                        );
                        if (!ZSTD_isError(zstdResult))
                        {
                            size_t total = zstdResult + markerSize;
                            if (total < chunkSize)
                            {
                                compressedOk = true;
                                compSize     = total;
                                finalPtr     = compBuf.get();
                            }
                        }
                    }
                    else if (fileKV.m_bUseCompression)
                    {
                        // LZHAM path
                        size_t tmpCompSize = compSize;
                        lzham_compress_status_t st = lzham_compress_memory(
                            &builder.m_Encoder,
                            compBuf.get(),
                            &tmpCompSize,
                            chunkBuf.get(),
                            chunkSize,
                            nullptr
                        );
                        if (st == LZHAM_COMP_STATUS_SUCCESS && tmpCompSize < chunkSize)
                        {
                            compressedOk = true;
                            compSize     = tmpCompSize;
                            finalPtr     = compBuf.get();
                        }
                    }

                    // Now deduplicate. Note that we deduplicate by hashing
                    // the *uncompressed* data (to catch identical blocks
                    // even if compressed differently).
                    // If you prefer hashing compressed data, just keep finalPtr/compSize.
                    std::string chunkHash = compute_sha1_hex(chunkBuf.get(), chunkSize);

                    {
                        // Acquire dedupMutex for map access
                        std::lock_guard<std::mutex> dedupLock(dedupMutex);
                        auto it = builder.m_ChunkHashMap.find(chunkHash);
                        if (it != builder.m_ChunkHashMap.end())
                        {
                            // Duplicate found
                            frag = it->second;
                            sharedBytes += frag.m_nUncompressedSize;
                            sharedChunks++;
                            continue; // done for this chunk
                        }

                        // Not in map => must write new chunk
                        {
                            // Acquire fileIOMutex to do offset + write
                            std::lock_guard<std::mutex> fileLock(fileIOMutex);

                            // 1) get offset
                            uint64_t offset = static_cast<uint64_t>(ofsData.tellp());
                            // 2) write data
                            ofsData.write(reinterpret_cast<const char*>(finalPtr), compSize);

                            // 3) finalize descriptor
                            frag.m_nPackFileOffset = offset;
                            frag.m_nCompressedSize = compSize;
                            // Insert into chunk map
                            builder.m_ChunkHashMap[chunkHash] = frag;
                        }
                    }
                } // end for each fragment

                // Store the block in a language-specific vector
                if (!block.m_EntryPath.empty())
                {
                    std::lock_guard<std::mutex> lock2(resultsMutex);
                    languageEntries[language].push_back(block);
                }
            }); // end enqueue
        }
    }

    // 5) Wait for all tasks
    pool.wait();
    ofsData.flush();
    ofsData.close();

    std::cout << "[ReVPK] Master data file complete: " << masterDataFile << "\n"
              << "       Shared " << sharedBytes.load() 
              << " bytes in " << sharedChunks.load() << " deduplicated chunks.\n";

    // 6) Build each language’s .vpk directory
    for (auto& kv : languageEntries)
    {
        const std::string& lang = kv.first;
        const auto& blocks      = kv.second;
        VPKPair_t vp(lang.c_str(), context.c_str(), level.c_str(), 0);
        fs::path dirPath = fs::path(buildPath) / vp.m_DirName;

        VPKDir_t dir;
        dir.BuildDirectoryFile(dirPath.string(), blocks);
    }
}

/**
 * DoUnpackMulti() – Multi-threaded version
 */
static void DoUnpackMulti(const std::vector<std::string>& args)
{
    // usage: revpk unpackmulti <someDirFile> [outPath] [sanitize? 0/1]
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

    namespace fs = std::filesystem;
    fs::path dirPath = fs::path(fileName).parent_path();
    if (dirPath.empty()) dirPath = ".";

    // Step 1: Identify base name ignoring any known language prefix
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

    // Step 2: Collect all matching _dir VPK files from the directory
    std::map<std::string, VPKDir_t> languageDirs;
    for (auto& entry : fs::directory_iterator(dirPath))
    {
        if (!entry.is_regular_file())
            continue;
        std::string fname = entry.path().filename().string();
        if (fname.find(baseFilename) != std::string::npos
            && fname.find("_dir.vpk") != std::string::npos)
        {
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

    // Step 4: Unpack the fallback (English) fully
    CPackedStoreBuilder builder;
    builder.InitLzDecoder();

    std::string engOut = outPath + "content/english/";
    fs::create_directories(engOut);
    builder.UnpackStore(*pEnglishDir, engOut.c_str());

    // Step 5: For each other language, unpack differences in parallel
    std::vector<std::future<void>> unpackFutures;
    for (auto& kvLang : languageDirs)
    {
        if (kvLang.first == "english")
            continue;

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
    for (auto& fut : unpackFutures)
        fut.get();

    // Optional: build a multiLangManifest
    {
        fs::path manifestDir = fs::path(outPath) / "manifest";
        fs::create_directories(manifestDir);
        std::string multiLangPath = (manifestDir / "multiLangManifest.vdf").string();
        bool success = builder.BuildMultiLangManifest(languageDirs, multiLangPath);
        if (!success)
            std::cerr << "[ReVPK] WARNING: Could not write multiLangUnpacked.vdf\n";
        else
            std::cout << "[ReVPK] Wrote multiLangUnpacked.vdf at " << multiLangPath << "\n";
    }

    std::cout << "[ReVPK] UnpackMulti completed.\n";
}

static void DoPackDeltaCommon(const std::vector<std::string>& args)
{
    if (args.size() < 3)
    {
        std::cout << "Usage: revpk packdeltacommon <context> [workspacePath] [buildPath] [numThreads] [compressLevel]\n";
        return;
    }

    // Parse arguments.
    std::string context      = args[2];
    std::string workspace    = (args.size() > 3) ? args[3] : "ship";
    std::string buildPath    = (args.size() > 4) ? args[4] : "vpk";

    // Ensure trailing directory separator.
    if (!workspace.empty() && workspace.back() != '/' && workspace.back() != '\\')
        workspace.push_back('/');
    if (!buildPath.empty() && buildPath.back() != '/' && buildPath.back() != '\\')
        buildPath.push_back('/');

    int numThreads = (args.size() > 5) ? std::atoi(args[5].c_str()) : -1;
    if (numThreads <= 0)
    {
        numThreads = std::thread::hardware_concurrency();
        if (numThreads == 0)
            numThreads = 1;
    }
    std::string compressLevel = (args.size() > 6) ? args[6] : "uber";

    // Locate all manifest files.
    namespace fs = std::filesystem;
    std::vector<std::string> manifestFiles;
    try {
        for (const auto& entry : fs::recursive_directory_iterator(workspace))
        {
            if (entry.is_regular_file() &&
                entry.path().filename() == "multiLangManifest.vdf")
            {
                manifestFiles.push_back(entry.path().string());
            }
        }
    }
    catch (const std::exception &e) {
        std::cerr << "[ReVPK] ERROR scanning for manifests: " << e.what() << "\n";
        return;
    }
    if (manifestFiles.empty())
    {
        std::cerr << "[ReVPK] ERROR: No multiLangManifest.vdf files found under " << workspace << "\n";
        return;
    }

    // Define manifest entry structure.
    struct ManifestEntry
    {
        std::string lang;
        std::string mapName;
        std::string filePath;
        VPKKeyValues_t kv;
    };

    // Split tasks into English and non-English.
    std::vector<ManifestEntry> englishTasks, nonEnglishTasks;
    for (const std::string &manifestFile : manifestFiles)
    {
        std::map<std::string, std::vector<VPKKeyValues_t>> langFileMap;
        if (!LoadMultiLangManifest(manifestFile, langFileMap))
        {
            std::cerr << "[ReVPK] WARNING: Failed to load manifest: " << manifestFile << "\n";
            continue;
        }

        fs::path mfPath(manifestFile);
        std::string mapName = mfPath.parent_path().parent_path().filename().string();

        for (auto &langPair : langFileMap)
        {
            const std::string &lang = langPair.first;
            for (const VPKKeyValues_t &kv : langPair.second)
            {
                std::string filePath = workspace + mapName + "/content/" +
                    (lang == "english" ? "english/" : (lang + "/")) + kv.m_EntryPath;

                ManifestEntry entry = { lang, mapName, filePath, kv };
                if (lang == "english")
                    englishTasks.push_back(entry);
                else
                    nonEnglishTasks.push_back(entry);
            }
        }
    }

    if (englishTasks.empty() && nonEnglishTasks.empty())
    {
        std::cerr << "[ReVPK] ERROR: No tasks found from manifests.\n";
        return;
    }

    // Track progress in a separate thread.
    std::atomic<size_t> filesProcessed{0};
    size_t totalFilesCount = englishTasks.size() + nonEnglishTasks.size();
    std::atomic<bool> progressDone{false};
    auto progressFuture = std::async(std::launch::async, [&]()
    {
        auto startTime = std::chrono::steady_clock::now();
        while (!progressDone.load())
        {
            size_t processed = filesProcessed.load();
            double progressFraction = (totalFilesCount > 0) ? double(processed) / totalFilesCount : 1.0;
            double percent = progressFraction * 100.0;
            auto now = std::chrono::steady_clock::now();
            double elapsedSec = std::chrono::duration<double>(now - startTime).count();
            double estimatedTotalSec = (progressFraction > 0.0) ? elapsedSec / progressFraction : 0.0;
            double remainingSec = estimatedTotalSec - elapsedSec;
            if (remainingSec < 0.0)
                remainingSec = 0.0;
            std::cout << "\rProcessed " << processed << "/" << totalFilesCount
                      << " files (" << int(percent) << "%), ETA: " << int(remainingSec) << " sec." << std::flush;
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
        }
        std::cout << "\rProcessed " << totalFilesCount << "/" << totalFilesCount
                  << " files (100%), ETA: 0 sec." << std::endl;
    });

    // Open master VPK files for random–access writing.
    std::string omegaClientPath = buildPath + "client_mp_delta_common.bsp.pak000_000.vpk";
    std::string omegaServerPath = buildPath + "server_mp_delta_common.bsp.pak000_000.vpk";
    int fdClient = open(omegaClientPath.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0666);
    int fdServer = open(omegaServerPath.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (fdClient < 0 || fdServer < 0)
    {
        std::cerr << "[ReVPK] ERROR: Could not open omega output file(s) for writing.\n";
        progressDone = true;
        progressFuture.wait();
        if (fdClient >= 0)
            close(fdClient);
        if (fdServer >= 0)
            close(fdServer);
        return;
    }

    // Atomic offsets for lock-free writes.
    std::atomic<uint64_t> clientOffset{0}, serverOffset{0};

    // Prepare the encoder and shared maps.
    CPackedStoreBuilder builder;
    builder.InitLzEncoder(numThreads, compressLevel.c_str());

    std::unordered_map<std::string, VPKChunkDescriptor_t> serverChunkMap;
    std::mutex clientMapMutex, serverMapMutex, resultsMutex;
    std::condition_variable englishProcessedCV;
    std::atomic<bool> englishProcessingComplete{false};

    // Maps for entries.
    // The fallback maps now use a composite key: "mapName|filePath"
    std::map<std::string, VPKEntryBlock_t> englishClientEntries;
    std::map<std::string, VPKEntryBlock_t> englishServerEntries;
    typedef std::pair<std::string, std::string> LangMapKey;
    std::map<LangMapKey, std::vector<VPKEntryBlock_t>> clientDirEntries;
    std::map<LangMapKey, std::vector<VPKEntryBlock_t>> serverDirEntries;

    // The file processing lambda.
    auto processFile = [&](const ManifestEntry &entry) -> std::pair<VPKEntryBlock_t, VPKEntryBlock_t>
    {
        // Use thread–local buffers to avoid repeated allocation.
        thread_local std::vector<uint8_t> chunkBuf(VPK_ENTRY_MAX_LEN);
        thread_local std::vector<uint8_t> compBuf(VPK_ENTRY_MAX_LEN);

        std::ifstream ifs(entry.filePath, std::ios::binary);
        if (!ifs.good())
            return {};

        ifs.seekg(0, std::ios::end);
        std::streamoff len = ifs.tellg();
        ifs.seekg(0, std::ios::beg);
        if (len < 0)
        {
            std::cerr << "[ReVPK] ERROR: " << entry.kv.m_EntryPath << " has negative length.\n";
            return {};
        }

        if (len == 0)
        {
            std::cerr << "[ReVPK] INFO: " << entry.kv.m_EntryPath 
                      << " is empty (0 bytes). Creating empty entry block.\n";
            
            VPKEntryBlock_t clientEntry;
            clientEntry.m_EntryPath = entry.kv.m_EntryPath;
            clientEntry.m_iPackFileIndex = 0x1337;
            clientEntry.m_iPreloadSize = entry.kv.m_iPreloadSize;

            VPKChunkDescriptor_t emptyChunk;
            emptyChunk.m_nUncompressedSize = 0;
            emptyChunk.m_nCompressedSize = 0;
            emptyChunk.m_nPackFileOffset = 0;
            emptyChunk.m_nLoadFlags = entry.kv.m_nLoadFlags;
            emptyChunk.m_nTextureFlags = entry.kv.m_nTextureFlags;
            clientEntry.m_Fragments.push_back(emptyChunk);

            VPKEntryBlock_t serverEntry = clientEntry;
            return std::make_pair(clientEntry, serverEntry);
        }

        // Read file data.
        std::vector<uint8_t> fileData(static_cast<size_t>(len));
        ifs.read(reinterpret_cast<char*>(fileData.data()), len);

        VPKEntryBlock_t clientEntry(fileData.data(), fileData.size(), 0,
                                    entry.kv.m_iPreloadSize, 0,
                                    entry.kv.m_nLoadFlags, entry.kv.m_nTextureFlags,
                                    entry.kv.m_EntryPath.c_str());
        clientEntry.m_iPackFileIndex = 0x1337;

        VPKEntryBlock_t serverEntry;
        bool includeServer = false;
        {
            auto shouldIncludeForServer = [&](const std::string &path) -> bool {
                std::string lowerPath = path;
                std::transform(lowerPath.begin(), lowerPath.end(), lowerPath.begin(), ::tolower);
                const std::vector<std::string> excludedExts = { ".raw", ".vcs", ".vtf", ".vfont",
                                                                  ".vbf", ".bsp_lump", ".vvd", ".vtx" };
                for (const auto &ext : excludedExts)
                {
                    if (lowerPath.size() >= ext.size() &&
                        lowerPath.compare(lowerPath.size() - ext.size(), ext.size(), ext) == 0)
                        return false;
                }
                const std::vector<std::string> excludedDirs = { "depot/", "media/", "shaders/", "sound/" };
                for (const auto &dir : excludedDirs)
                {
                    if (lowerPath.rfind(dir, 0) == 0)
                        return false;
                }
		if (entry.mapName == "mp_npe")
        		return false;
                return true;
            };
            includeServer = shouldIncludeForServer(entry.kv.m_EntryPath);
            if (includeServer)
                serverEntry = clientEntry;
        }

        size_t memoryOffset = 0;
        for (size_t i = 0; i < clientEntry.m_Fragments.size(); i++)
        {
            VPKChunkDescriptor_t &clientFrag = clientEntry.m_Fragments[i];
            VPKChunkDescriptor_t *pServerFrag = (includeServer ? &serverEntry.m_Fragments[i] : nullptr);

            std::memcpy(chunkBuf.data(), fileData.data() + memoryOffset, clientFrag.m_nUncompressedSize);
            memoryOffset += clientFrag.m_nUncompressedSize;

            size_t compSize = clientFrag.m_nUncompressedSize;
            const uint8_t* finalDataPtr = chunkBuf.data();

            if (entry.kv.m_bUseCompression)
            {
                if (builder.IsUsingZSTD())
                {
                    constexpr size_t markerSize = sizeof(R1D_marker);
                    std::memcpy(compBuf.data(), &R1D_marker, markerSize);
                    size_t zstdBound = ZSTD_compressBound(clientFrag.m_nUncompressedSize);
                    if (zstdBound + markerSize > VPK_ENTRY_MAX_LEN)
                        zstdBound = VPK_ENTRY_MAX_LEN - markerSize;
                    size_t zstdResult = ZSTD_compress(compBuf.data() + markerSize,
                                                      zstdBound,
                                                      chunkBuf.data(),
                                                      clientFrag.m_nUncompressedSize,
                                                      6);
                    if (!ZSTD_isError(zstdResult))
                    {
                        size_t totalSize = zstdResult + markerSize;
                        if (totalSize < clientFrag.m_nUncompressedSize)
                        {
                            compSize = totalSize;
                            finalDataPtr = compBuf.data();
                        }
                    }
                }
                else
                {
                    size_t tmpCompSize = compSize;
                    lzham_compress_status_t st = lzham_compress_memory(
                        &builder.m_Encoder,
                        compBuf.data(), &tmpCompSize,
                        chunkBuf.data(), clientFrag.m_nUncompressedSize,
                        nullptr);
                    if (st == LZHAM_COMP_STATUS_SUCCESS && tmpCompSize < clientFrag.m_nUncompressedSize)
                    {
                        compSize = tmpCompSize;
                        finalDataPtr = compBuf.data();
                    }
                }
            }

            std::string chunkHash = compute_sha1_hex(chunkBuf.data(), clientFrag.m_nUncompressedSize);

            // Write to client file.
{
    std::lock_guard<std::mutex> lock(clientMapMutex);
    auto it = builder.m_ChunkHashMap.find(chunkHash);
    if (it != builder.m_ChunkHashMap.end())
    {
         // Do not override the load/texture flags.
         clientFrag.m_nPackFileOffset = it->second.m_nPackFileOffset;
         clientFrag.m_nCompressedSize = it->second.m_nCompressedSize;
         // (m_nUncompressedSize should already be correct)
    }
    else
    {
         uint64_t writePos = clientOffset.fetch_add(compSize);
         ssize_t written = pwrite(fdClient, finalDataPtr, compSize, writePos);
         if (written != (ssize_t)compSize)
         {
             std::cerr << "[ReVPK] ERROR: Failed to write client chunk for " << entry.kv.m_EntryPath << "\n";
         }
         clientFrag.m_nPackFileOffset = writePos;
         clientFrag.m_nCompressedSize = compSize;
         builder.m_ChunkHashMap[chunkHash] = clientFrag;
    }
}


            // Write to server file if needed.
            if (pServerFrag)
            {
                std::lock_guard<std::mutex> lock(serverMapMutex);
                auto it = serverChunkMap.find(chunkHash);
                if (it != serverChunkMap.end())
                {
    pServerFrag->m_nPackFileOffset = it->second.m_nPackFileOffset;
    pServerFrag->m_nCompressedSize = it->second.m_nCompressedSize;

                }
                else
                {
                    uint64_t writePos = serverOffset.fetch_add(compSize);
                    ssize_t written = pwrite(fdServer, finalDataPtr, compSize, writePos);
                    if (written != (ssize_t)compSize)
                    {
                        std::cerr << "[ReVPK] ERROR: Failed to write server chunk for " << entry.kv.m_EntryPath << "\n";
                    }
                    pServerFrag->m_nPackFileOffset = writePos;
                    pServerFrag->m_nCompressedSize = compSize;
                    serverChunkMap[chunkHash] = *pServerFrag;
                }
            }
        }
        return std::make_pair(clientEntry, serverEntry);
    };

    // Create a thread pool (implementation assumed elsewhere).
    ThreadPool pool(numThreads);

    // Process English files first.
    for (const auto &entry : englishTasks)
    {
        pool.enqueue([&, entry]()
        {
            auto entries = processFile(entry);
            if (!entries.first.m_EntryPath.empty())
            {
                std::string finalMapName = entry.mapName;
                if (entry.kv.m_EntryPath.size() >= 4 &&
                    entry.kv.m_EntryPath.compare(entry.kv.m_EntryPath.size() - 4, 4, ".bsp") == 0)
                {
                    finalMapName = "mp_common";
                }
                LangMapKey key("english", finalMapName);
                std::string englishKey = entry.mapName + "|" + entry.kv.m_EntryPath;
                {
                    std::lock_guard<std::mutex> lock(resultsMutex);
                    englishClientEntries[englishKey] = entries.first;
                    if (!entries.second.m_EntryPath.empty())
                        englishServerEntries[englishKey] = entries.second;

                    clientDirEntries[key].push_back(entries.first);
                    if (!entries.second.m_EntryPath.empty())
                        serverDirEntries[key].push_back(entries.second);
                }
            }
            filesProcessed++;
        });
    }

    // Wait until all English tasks are done.
    pool.wait();
    englishProcessingComplete = true;
    englishProcessedCV.notify_all();

    // Process non-English files.
    for (const auto &entry : nonEnglishTasks)
    {
        pool.enqueue([&, entry]()
        {
            // If the non-English file doesn't exist, fall back to the English entry.
            if (!fs::exists(entry.filePath))
            {
                std::unique_lock<std::mutex> lock(resultsMutex);
                englishProcessedCV.wait(lock, [&]{ return englishProcessingComplete.load(); });
                std::string englishKey = entry.mapName + "|" + entry.kv.m_EntryPath;
                auto it = englishClientEntries.find(englishKey);
                if (it != englishClientEntries.end())
                {
        // Check for .bsp extension and remap to "mp_common" if needed.
        std::string finalMapName = entry.mapName;
        if (entry.kv.m_EntryPath.size() >= 4 &&
            entry.kv.m_EntryPath.compare(entry.kv.m_EntryPath.size() - 4, 4, ".bsp") == 0)
        {
            finalMapName = "mp_common";
        }

                    LangMapKey key(entry.lang, finalMapName);
                    clientDirEntries[key].push_back(it->second);
                    // Also, add the fallback server entry if available.
                    auto itServer = englishServerEntries.find(englishKey);
                    if (itServer != englishServerEntries.end())
                        serverDirEntries[key].push_back(itServer->second);
                }
                filesProcessed++;
                return;
            }

            // Otherwise, process the file normally.
            auto entries = processFile(entry);
            if (!entries.first.m_EntryPath.empty())
            {
                std::string finalMapName = entry.mapName;
                if (entry.kv.m_EntryPath.size() >= 4 &&
                    entry.kv.m_EntryPath.compare(entry.kv.m_EntryPath.size() - 4, 4, ".bsp") == 0)
                {
                    finalMapName = "mp_common";
                }
                LangMapKey key(entry.lang, finalMapName);
                {
                    std::lock_guard<std::mutex> lock(resultsMutex);
                    clientDirEntries[key].push_back(entries.first);
                    if (!entries.second.m_EntryPath.empty())
                        serverDirEntries[key].push_back(entries.second);
                }
            }
            filesProcessed++;
        });
    }

    // Wait for all tasks to finish.
    pool.wait();
    progressDone = true;
    progressFuture.wait();

    // Close master file descriptors.
    close(fdClient);
    close(fdServer);

    // Build directory VPKs.
    for (const auto &entry : clientDirEntries)
    {
        const std::string &lang = entry.first.first;
        const std::string &mapName = entry.first.second;
        std::string dirVpkName = lang + "client_" + mapName + ".bsp.pak000_dir.vpk";
        std::string dirVpkPath = buildPath + dirVpkName;
        VPKDir_t dir;
        dir.BuildDirectoryFile(dirVpkPath, entry.second);
        std::cout << "[ReVPK] Wrote client directory VPK: " << dirVpkPath << "\n";
    }

    for (const auto &entry : serverDirEntries)
    {
        const std::string &lang = entry.first.first;
        const std::string &mapName = entry.first.second;
	if (mapName == "mp_npe")
		continue;
        std::string dirVpkName = lang + "server_" + mapName + ".bsp.pak000_dir.vpk";
        std::string dirVpkPath = buildPath + dirVpkName;
        VPKDir_t dir;
        dir.BuildDirectoryFile(dirVpkPath, entry.second);
        std::cout << "[ReVPK] Wrote server directory VPK: " << dirVpkPath << "\n";
    }

    std::cout << "[ReVPK] Omega data VPKs built:\n"
              << "         Client: " << omegaClientPath << "\n"
              << "         Server: " << omegaServerPath << "\n";
}


static void DoList(const std::vector<std::string>& args)
{
    if (args.size() < 3)
    {
        PrintUsage();
        return;
    }

    std::string fileName = args[2];

    // Parse directory
    VPKDir_t vpkDir(fileName, false);
    if (vpkDir.Failed())
    {
        std::cerr << "[ReVPK] ERROR: Could not parse VPK directory: " << fileName << "\n";
        return;
    }

    // Sort entries by path for consistent output
    std::vector<std::string> sortedPaths;
    for (const auto& entry : vpkDir.m_EntryBlocks)  // Changed from m_Entries
    {
        sortedPaths.push_back(entry.m_EntryPath);
    }
    std::sort(sortedPaths.begin(), sortedPaths.end());

    // Print each entry with its size
    for (const auto& path : sortedPaths)
    {
        // Find the matching entry block
        const auto& entryBlock = std::find_if(
            vpkDir.m_EntryBlocks.begin(),
            vpkDir.m_EntryBlocks.end(),
            [&path](const VPKEntryBlock_t& entry) {
                return entry.m_EntryPath == path;
            });

        if (entryBlock != vpkDir.m_EntryBlocks.end())
        {
            size_t totalSize = 0;
            for (const auto& frag : entryBlock->m_Fragments)
            {
                totalSize += frag.m_nUncompressedSize;
            }
            std::cout << std::setw(12) << totalSize << "  " << path << "\n";
        }
    }

    // Print total number of files and total size
    size_t totalFiles = vpkDir.m_EntryBlocks.size();
    size_t totalBytes = 0;
    for (const auto& entry : vpkDir.m_EntryBlocks)
    {
        for (const auto& frag : entry.m_Fragments)
        {
            totalBytes += frag.m_nUncompressedSize;
        }
    }
    std::cout << "\nTotal: " << totalFiles << " files, " 
              << totalBytes << " bytes (" 
              << (totalBytes / 1024 / 1024) << " MB)\n";
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

    if      (cmd == PACK_COMMAND)      DoPack(args);
    else if (cmd == UNPACK_COMMAND)    DoUnpack(args);
    else if (cmd == "packmulti")       DoPackMulti(args);
    else if (cmd == "unpackmulti")     DoUnpackMulti(args);
    else if (cmd == "packdeltacommon")      DoPackDeltaCommon(args);
    else if (cmd == "ls")      DoList(args);
    else                               PrintUsage();

    return 0;
}
