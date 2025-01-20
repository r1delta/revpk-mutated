/**
 * packedstore.cpp
 *
 * Implements the packing/unpacking logic with real hashing (CRC32 via Zlib + SHA1 via OpenSSL).
 */
#include "keyvalues.h"
#include "packedstore.h"
#include <filesystem>
#include <fstream>
#include <iostream>
#include <cstring>
#include <cassert>

// OpenSSL for SHA
#include <openssl/sha.h>
// Zlib for CRC32
#include "/usr/include/zlib.h"

// --------------------

/** Helper: do real CRC32 using Zlib. */
static uint32_t compute_crc32(const uint8_t* data, size_t len)
{
    return crc32_z(0, data, len);
}

/** Helper: do real SHA1 using OpenSSL. Returns hex string. */
std::string compute_sha1_hex(const uint8_t* data, size_t len)
{
    unsigned char hash[SHA_DIGEST_LENGTH];
    SHA1(data, len, hash);

    // Convert to hex string
    char hexbuf[SHA_DIGEST_LENGTH * 2 + 1];
    for (int i = 0; i < SHA_DIGEST_LENGTH; i++)
        std::sprintf(hexbuf + (i * 2), "%02x", hash[i]);
    hexbuf[SHA_DIGEST_LENGTH * 2] = '\0';
    return std::string(hexbuf);
}

/**
 * Determine LZHAM compression level from string
 */
// --- ZSTD support ---
static lzham_compress_level parseCompressionLevel(const char* levelStr, CPackedStoreBuilder* pBuilder)
{
    if (!levelStr)
        return LZHAM_COMP_LEVEL_DEFAULT;

    std::string s(levelStr);
    if (s == "zstd")
    {
        // We'll tell the builder we want ZSTD:
        pBuilder->m_eCompressionMethod = kCompressionZSTD;
        // Just return a dummy LZHAM level to keep old code happy:
        return LZHAM_COMP_LEVEL_DEFAULT;
    }
    // Otherwise, handle LZHAM:
    if      (s == "fastest") return LZHAM_COMP_LEVEL_FASTEST;
    else if (s == "faster")  return LZHAM_COMP_LEVEL_FASTER;
    else if (s == "better")  return LZHAM_COMP_LEVEL_BETTER;
    else if (s == "uber")    return LZHAM_COMP_LEVEL_UBER;

    return LZHAM_COMP_LEVEL_DEFAULT;
}
// --------------------

// ------------------------------------------------------------------------
//  VPKEntryBlock_t constructor (packing scenario)
// ------------------------------------------------------------------------
VPKEntryBlock_t::VPKEntryBlock_t(const uint8_t* pData, size_t nLen, uint64_t /*nOffset*/,
                                 uint16_t iPreloadSize, uint16_t iPackFileIndex,
                                 uint32_t nLoadFlags, uint16_t nTextureFlags,
                                 const char* pEntryPath)
{
    m_iPreloadSize = iPreloadSize;
    m_iPackFileIndex = iPackFileIndex;
    m_EntryPath = (pEntryPath ? pEntryPath : "");

    // compute CRC
    m_nFileCRC = compute_crc32(pData, nLen);

    // break file into 1 MiB chunks
    size_t totalLeft = nLen;
    size_t currentMemOffset = 0;  // Track memory offset instead of pack offset
    const size_t chunkSz = VPK_ENTRY_MAX_LEN;

    while (totalLeft > 0)
    {
        size_t csize = (totalLeft >= chunkSz) ? chunkSz : totalLeft;

        VPKChunkDescriptor_t desc(nLoadFlags, nTextureFlags,
                                  0,           // Pack offset will be set later
                                  csize,       // initially same as uncompressed
                                  csize);

        m_Fragments.push_back(desc);

        totalLeft -= csize;
        currentMemOffset += csize;
    }
}

// ------------------------------------------------------------------------
//  CPackedStoreBuilder: init LZHAM
// ------------------------------------------------------------------------
void CPackedStoreBuilder::InitLzEncoder(int maxHelperThreads, const char* compressionLevel)
{
    // --- ZSTD support ---
    // First, zero out the LZHAM encoder config
    std::memset(&m_Encoder, 0, sizeof(m_Encoder));
    m_Encoder.m_struct_size        = sizeof(m_Encoder);
    m_Encoder.m_dict_size_log2     = VPK_DICT_SIZE;
    // Temporarily parse level
    m_Encoder.m_level = parseCompressionLevel(compressionLevel, this);
    // If we remain LZHAM, set up the rest
    if (!IsUsingZSTD())
    {
        m_Encoder.m_max_helper_threads = (maxHelperThreads < 0) ? -1 : maxHelperThreads;
        m_Encoder.m_compress_flags     = LZHAM_COMP_FLAG_DETERMINISTIC_PARSING;
    }
    // --------------------
}

void CPackedStoreBuilder::InitLzDecoder()
{
    std::memset(&m_Decoder, 0, sizeof(m_Decoder));
    m_Decoder.m_struct_size     = sizeof(m_Decoder);
    m_Decoder.m_dict_size_log2  = VPK_DICT_SIZE;
    m_Decoder.m_decompress_flags= LZHAM_DECOMP_FLAG_OUTPUT_UNBUFFERED;
}

// ------------------------------------------------------------------------
//  Deduplicate chunk
// ------------------------------------------------------------------------
bool CPackedStoreBuilder::Deduplicate(const uint8_t* pEntryBuffer, VPKChunkDescriptor_t& descriptor, size_t finalSize)
{
    const std::string chunkHash = compute_sha1_hex(pEntryBuffer, finalSize);

    auto it = m_ChunkHashMap.find(chunkHash);
    if (it != m_ChunkHashMap.end())
    {
        descriptor = it->second;
        return true;
    }

    m_ChunkHashMap.insert({ chunkHash, descriptor });
    return false;
}

// ------------------------------------------------------------------------
//  CPackedStoreBuilder::PackStore
// ------------------------------------------------------------------------
void CPackedStoreBuilder::PackStore(const VPKPair_t& vpkPair, const char* workspaceName, const char* buildPath)
{
    namespace fs = std::filesystem;

    // 1) Read the KeyValues manifest
    std::string baseName = PackedStore_GetDirBaseName(vpkPair.m_DirName);
    fs::path manifestFile = fs::path(workspaceName) / "manifest" / (baseName + ".vdf");

    std::vector<VPKKeyValues_t> buildList;
    if (!LoadKeyValuesManifest(manifestFile.string(), buildList))
    {
        std::cerr << "[ReVPK] ERROR: Could not load manifest: " << manifestFile << "\n";
        return;
    }

    // 2) Create the pack file
    fs::path packPath = fs::path(buildPath) / vpkPair.m_PackName;
    fs::path dirPath = fs::path(buildPath) / vpkPair.m_DirName;

    try
    {
        fs::create_directories(packPath.parent_path());
    }
    catch (...)
    {
        std::cerr << "[ReVPK] ERROR: Cannot create directory: " << packPath.parent_path() << "\n";
        return;
    }

    std::ofstream ofsPack(packPath, std::ios::binary);
    if (!ofsPack.is_open())
    {
        std::cerr << "[ReVPK] ERROR: Cannot open pack file for writing: " << packPath << "\n";
        return;
    }

    std::vector<VPKEntryBlock_t> entryBlocks;
    entryBlocks.reserve(buildList.size());

    // Buffers for chunk operations
    std::unique_ptr<uint8_t[]> chunkBuf(new uint8_t[VPK_ENTRY_MAX_LEN]);
    std::unique_ptr<uint8_t[]> compBuf(new uint8_t[VPK_ENTRY_MAX_LEN]);

    size_t sharedBytes = 0;
    size_t sharedChunks = 0;
    uint16_t packFileIndex = 0; // single .vpk scenario

    // 3) Process each file from manifest
    for (const auto& kv : buildList)
    {
        // Open and read input file
        std::ifstream ifFile(kv.m_EntryPath, std::ios::binary);
        if (!ifFile.good())
        {
            std::cerr << "[ReVPK] WARNING: Could not open " << kv.m_EntryPath << "\n";
            continue;
        }

        ifFile.seekg(0, std::ios::end);
        std::streamoff len = ifFile.tellg();
        ifFile.seekg(0, std::ios::beg);

        if (len <= 0)
        {
            std::cerr << "[ReVPK] WARNING: " << kv.m_EntryPath << " is empty.\n";
            continue;
        }

        // Read entire file
        std::unique_ptr<uint8_t[]> fileData(new uint8_t[len]);
        ifFile.read(reinterpret_cast<char*>(fileData.get()), len);
        ifFile.close();

        // Create entry block
        VPKEntryBlock_t blk(fileData.get(), size_t(len), 0,
                            kv.m_iPreloadSize, packFileIndex,
                            kv.m_nLoadFlags, kv.m_nTextureFlags,
                            kv.m_EntryPath.c_str());
        entryBlocks.push_back(blk);

        // Process each chunk
        size_t memoryOffset = 0;
        for (auto& frag : entryBlocks.back().m_Fragments)
        {
            // Copy chunk to working buffer
            std::memcpy(chunkBuf.get(),
                        fileData.get() + memoryOffset,
                        frag.m_nUncompressedSize);

            // --- ZSTD support ---
            // 1) Attempt compression if enabled
            bool compressedOk = false;
            size_t compSize = frag.m_nUncompressedSize;

            if (kv.m_bUseCompression)
            {
                if (IsUsingZSTD())
                {
                    // ------------------------------
                    // (A) ZSTD path
                    // ------------------------------
                    // We'll put the marker at the beginning of compBuf,
                    // and compress into compBuf + markerSize.
                    constexpr size_t markerSize = sizeof(R1D_marker);
                    // For safety, ensure we have enough space in compBuf
                    // compBuf is VPK_ENTRY_MAX_LEN in size, so it's presumably safe.

                    // Store marker in compBuf
                    std::memcpy(compBuf.get(), &R1D_marker, markerSize);

                    // Then do ZSTD compression after the marker
                    size_t zstdBound = ZSTD_compressBound(frag.m_nUncompressedSize);
                    // We must not exceed VPK_ENTRY_MAX_LEN - markerSize
                    if (zstdBound + markerSize > VPK_ENTRY_MAX_LEN)
                        zstdBound = VPK_ENTRY_MAX_LEN - markerSize;

                    size_t zstdResult = ZSTD_compress(
                        compBuf.get() + markerSize,   // dest
                        zstdBound,                    // dest capacity
                        chunkBuf.get(),               // src
                        frag.m_nUncompressedSize,     // src size
                        6 /* or some default ZSTD level */
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
                else
                {
                    // ------------------------------
                    // (B) LZHAM path
                    // ------------------------------
                    lzham_compress_status_t st = lzham_compress_memory(
                        &m_Encoder,
                        compBuf.get(), &compSize,
                        chunkBuf.get(), frag.m_nUncompressedSize,
                        nullptr
                    );
                    if (st == LZHAM_COMP_STATUS_SUCCESS && compSize < frag.m_nUncompressedSize)
                    {
                        compressedOk = true;
                    }
                    else
                    {
                        compSize = frag.m_nUncompressedSize; // revert
                    }
                }
            }

            // 2) Decide final data to write
            const uint8_t* finalDataPtr = compressedOk ? compBuf.get() : chunkBuf.get();
            size_t finalDataSize        = compressedOk ? compSize : frag.m_nUncompressedSize;

            // --- Deduplication Logic ---
            std::string chunkHash = compute_sha1_hex(finalDataPtr, finalDataSize);
            auto it = m_ChunkHashMap.find(chunkHash);
            if (it != m_ChunkHashMap.end())
            {
                // Existing chunk:
                frag = it->second;
                sharedBytes += frag.m_nUncompressedSize;
                sharedChunks++;
            }
            else
            {
                // New chunk:
                // 3) Prepare descriptor with final values
                uint64_t writePos = static_cast<uint64_t>(ofsPack.tellp());
                frag.m_nPackFileOffset = writePos;
                frag.m_nCompressedSize = finalDataSize;

                // 4) Write
                ofsPack.write(reinterpret_cast<const char*>(finalDataPtr), finalDataSize);

                // 5) Store in map
                m_ChunkHashMap[chunkHash] = frag;
            }

            memoryOffset += frag.m_nUncompressedSize;
        }
    }

    ofsPack.flush();
    ofsPack.close();

    // Log statistics
    auto finalSize = fs::file_size(packPath);
    std::cout << "[ReVPK] Packed " << buildList.size()
        << " files into " << packPath.filename().string()
        << " (" << finalSize << " bytes total, "
        << sharedBytes << " bytes deduplicated in "
        << sharedChunks << " shared chunks)\n";

    // Build directory file
    VPKDir_t dir;
    dir.BuildDirectoryFile(dirPath.string(), entryBlocks);

    m_ChunkHashMap.clear();
}

// ------------------------------------------------------------------------
//  CPackedStoreBuilder::UnpackStore
// ------------------------------------------------------------------------
void CPackedStoreBuilder::UnpackStore(const VPKDir_t& vpkDir, const char* workspaceName)
{
    namespace fs = std::filesystem;
    fs::path outPath = (workspaceName ? workspaceName : "");
    if (!outPath.empty())
    {
        fs::create_directories(outPath);
    }

    // Also rebuild a manifest from the directory
    std::string baseName = PackedStore_GetDirBaseName(vpkDir.m_DirFilePath);
    fs::path manifestDir = outPath / "manifest";
    fs::create_directories(manifestDir);
    fs::path manifestPath = manifestDir / (baseName + ".vdf");

    // Build manifest lines
    std::ostringstream oss;
    for (auto& blk : vpkDir.m_EntryBlocks)
    {
        bool compressed = false;
        for (auto& f : blk.m_Fragments)
        {
            if (f.m_nCompressedSize < f.m_nUncompressedSize)
            {
                compressed = true;
                break;
            }
        }
        oss << "\"" << blk.m_EntryPath << "\"\n"
        << "{\n"
        << "  \"preloadSize\" \"" << blk.m_iPreloadSize << "\"\n"
        << "  \"loadFlags\" \""   << (blk.m_Fragments.empty()? 3 : blk.m_Fragments[0].m_nLoadFlags) << "\"\n"
        << "  \"textureFlags\" \""<< (blk.m_Fragments.empty()? 0 : blk.m_Fragments[0].m_nTextureFlags) << "\"\n"
        << "  \"useCompression\" \"" << (compressed ? "1" : "0") << "\"\n"
        << "  \"deDuplicate\" \"1\"\n"
        << "}\n\n";
    }
    {
        std::ofstream ofs(manifestPath);
        if (ofs.is_open())
        {
            ofs << "\"BuildManifest\"\n{\n";
            ofs << oss.str();
            ofs << "}\n";
        }
        else
        {
            std::cerr << "[ReVPK] WARNING: Could not write manifest: " << manifestPath << "\n";
        }
    }

    // 2) Extract
    std::unique_ptr<uint8_t[]> srcBuf(new uint8_t[VPK_ENTRY_MAX_LEN]);
    std::unique_ptr<uint8_t[]> dstBuf(new uint8_t[VPK_ENTRY_MAX_LEN]);

    fs::path baseDir = fs::path(vpkDir.m_DirFilePath).parent_path();
    for (auto idx : vpkDir.m_PakFileIndices)
    {
        std::string chunkName = vpkDir.GetPackFileNameForIndex(idx);
        fs::path chunkPath = baseDir / chunkName;

        std::ifstream ifs(chunkPath, std::ios::binary);
        if (!ifs.good())
        {
            std::cerr << "[ReVPK] ERROR: Could not open chunk file: " << chunkPath << "\n";
            continue;
        }

        for (auto& block : vpkDir.m_EntryBlocks)
        {
            if (block.m_iPackFileIndex != idx)
                continue;

            // create subdirs
            fs::path outFile = outPath / block.m_EntryPath;
            fs::create_directories(outFile.parent_path());
            std::ofstream ofs(outFile, std::ios::binary);
            if (!ofs.is_open())
            {
                std::cerr << "[ReVPK] ERROR: Could not open output file for writing: " << outFile << "\n";
                continue;
            }

            // read chunk by chunk
            for (auto& frag : block.m_Fragments)
            {
                if (frag.m_nPackFileOffset == 0 && frag.m_nCompressedSize == 0)
                    continue; // deduplicated chunk might refer to offset in another pack file
                ifs.seekg(frag.m_nPackFileOffset, std::ios::beg);
                if (!ifs.good()) break;

                ifs.read(reinterpret_cast<char*>(srcBuf.get()), frag.m_nCompressedSize);

                // --- ZSTD support ---
                if (frag.m_nCompressedSize == frag.m_nUncompressedSize)
                {
                    // Uncompressed
                    ofs.write(reinterpret_cast<const char*>(srcBuf.get()), frag.m_nUncompressedSize);
                }
                else
                {
                    // Potentially compressed. Let's see if it’s ZSTD:
                    if (frag.m_nCompressedSize >= sizeof(R1D_marker))
                    {
                        uint64_t possibleMarker = 0;
                        std::memcpy(&possibleMarker, srcBuf.get(), sizeof(R1D_marker));

                        if (possibleMarker == R1D_marker)
                        {
                            // -------------------------
                            // ZSTD chunk
                            // -------------------------
                            // Data after the marker:
                            const uint8_t* zstdData = srcBuf.get() + sizeof(R1D_marker);
                            size_t zstdSize        = frag.m_nCompressedSize - sizeof(R1D_marker);

                            // Decompress into dstBuf
                            size_t dstLen = VPK_ENTRY_MAX_LEN;  // max chunk size
                            size_t dResult = ZSTD_decompress(
                                dstBuf.get(),
                                                             dstLen,
                                                             zstdData,
                                                             zstdSize
                            );
                            if (!ZSTD_isError(dResult))
                            {
                                ofs.write(reinterpret_cast<const char*>(dstBuf.get()), dResult);
                            }
                            else
                            {
                                std::cerr << "[ReVPK] ERROR decompressing ZSTD chunk.\n";
                            }
                            continue;
                        }
                    }

                    // -------------------------
                    // If we reach here, assume LZHAM
                    // -------------------------
                    size_t dstLen = VPK_ENTRY_MAX_LEN;
                    lzham_decompress_status_t st = lzham_decompress_memory(
                        &m_Decoder,
                        dstBuf.get(), &dstLen,
                                                                           srcBuf.get(), static_cast<size_t>(frag.m_nCompressedSize),
                                                                           nullptr
                    );

                    if (st != LZHAM_DECOMP_STATUS_SUCCESS)
                    {
                        std::cerr << "[ReVPK] ERROR decompressing LZHAM chunk.\n";
                    }
                    else
                    {
                        ofs.write(reinterpret_cast<const char*>(dstBuf.get()), dstLen);
                    }
                }
                // --------------------
            }
        }
    }
}

// ------------------------------------------------------------------------
//  VPKDir_t
// ------------------------------------------------------------------------
VPKDir_t::VPKDir_t()
: m_bInitFailed(false)
{
}

VPKDir_t::VPKDir_t(const std::string& dirFilePath, bool bSanitize)
: m_bInitFailed(false)
{
    if (!bSanitize)
    {
        Init(dirFilePath);
    }
    else
    {
        // Attempt to guess the directory file from a chunk file
        std::smatch match;
        if (std::regex_search(dirFilePath, match, g_VpkPackFileRegex))
        {
            // replace "pak000_XXX" with "pak000_dir"
            std::string replaced = std::regex_replace(dirFilePath, g_VpkPackFileRegex, "pak000_dir");
            if (std::filesystem::exists(replaced))
            {
                Init(replaced);
            }
            else
            {
                std::cerr << "[ReVPK] ERROR: No corresponding _dir VPK found for " << dirFilePath << "\n";
                m_bInitFailed = true;
            }
        }
        else
        {
            // fallback
            Init(dirFilePath);
        }
    }
}

void VPKDir_t::Init(const std::string& dirFilePath)
{
    m_DirFilePath = dirFilePath;

    std::ifstream ifs(dirFilePath, std::ios::binary);
    if (!ifs.is_open())
    {
        std::cerr << "[ReVPK] ERROR: Unable to open VPK dir file: " << dirFilePath << "\n";
        m_bInitFailed = true;
        return;
    }

    // Read the VPKDirHeader_t:
    ifs.read(reinterpret_cast<char*>(&m_Header.m_nHeaderMarker),   sizeof(m_Header.m_nHeaderMarker));
    ifs.read(reinterpret_cast<char*>(&m_Header.m_nMajorVersion),   sizeof(m_Header.m_nMajorVersion));
    ifs.read(reinterpret_cast<char*>(&m_Header.m_nMinorVersion),   sizeof(m_Header.m_nMinorVersion));
    ifs.read(reinterpret_cast<char*>(&m_Header.m_nDirectorySize),  sizeof(m_Header.m_nDirectorySize));
    ifs.read(reinterpret_cast<char*>(&m_Header.m_nSignatureSize),  sizeof(m_Header.m_nSignatureSize));

    // Validate header:
    if (m_Header.m_nHeaderMarker != VPK_HEADER_MARKER ||
        m_Header.m_nMajorVersion != VPK_MAJOR_VERSION ||
        m_Header.m_nMinorVersion != VPK_MINOR_VERSION)
    {
        std::cerr << "[ReVPK] ERROR: Invalid VPK header in " << dirFilePath << "\n";
        m_bInitFailed = true;
        return;
    }

    // Helper lambda to read a null-terminated string (1-byte delimiter).
    auto readNullTerminatedString = [&](std::ifstream& in) -> std::string
    {
        std::string s;
        while (true)
        {
            char c;
            if (!in.get(c))
                break; // EOF or error
                if (c == '\0')
                    break; // finished
                    s.push_back(c);
        }
        return s;
    };

    // Outer loop: read extension until empty
    while (true)
    {
        std::string ext = readNullTerminatedString(ifs);
        if (ext.empty())
        {
            // If the extension is empty, that means we reached the end of all data.
            break;
        }

        // Next loop: read path until empty
        while (true)
        {
            std::string path = readNullTerminatedString(ifs);
            if (path.empty())
            {
                // No more paths for this extension => break to read next extension
                break;
            }

            // Next loop: read filename until empty
            while (true)
            {
                std::string filename = readNullTerminatedString(ifs);
                if (filename.empty())
                {
                    // No more filenames for this path => break to read next path
                    break;
                }

                VPKEntryBlock_t block;
                block.m_EntryPath = path;
                if (block.m_EntryPath == " ")
                {
                    // Valve uses " " (space) to indicate root path. If so, clear it.
                    block.m_EntryPath.clear();
                }

                if (!block.m_EntryPath.empty())
                {
                    // Add slash if needed
                    if (block.m_EntryPath.back() != '/')
                        block.m_EntryPath.push_back('/');
                }
                // full file path = path + filename + '.' + extension
                block.m_EntryPath += filename;
                if (!ext.empty())
                {
                    block.m_EntryPath.push_back('.');
                    block.m_EntryPath += ext;
                }

                // Read the file CRC, preload size, pack file index
                ifs.read(reinterpret_cast<char*>(&block.m_nFileCRC),       sizeof(block.m_nFileCRC));
                ifs.read(reinterpret_cast<char*>(&block.m_iPreloadSize),   sizeof(block.m_iPreloadSize));
                ifs.read(reinterpret_cast<char*>(&block.m_iPackFileIndex), sizeof(block.m_iPackFileIndex));

                // Now read chunk descriptors until we hit PACKFILEINDEX_END
                while (true)
                {
                    // Each chunk descriptor is 30 bytes total (4+2+8+8+8).
                    VPKChunkDescriptor_t desc;
                    ifs.read(reinterpret_cast<char*>(&desc.m_nLoadFlags),        sizeof(desc.m_nLoadFlags));
                    ifs.read(reinterpret_cast<char*>(&desc.m_nTextureFlags),     sizeof(desc.m_nTextureFlags));
                    ifs.read(reinterpret_cast<char*>(&desc.m_nPackFileOffset),   sizeof(desc.m_nPackFileOffset));
                    ifs.read(reinterpret_cast<char*>(&desc.m_nCompressedSize),   sizeof(desc.m_nCompressedSize));
                    ifs.read(reinterpret_cast<char*>(&desc.m_nUncompressedSize), sizeof(desc.m_nUncompressedSize));

                    // Then read the 2-byte marker after each chunk
                    uint16_t marker = 0;
                    ifs.read(reinterpret_cast<char*>(&marker), sizeof(marker));

                    // Add chunk descriptor
                    block.m_Fragments.push_back(desc);

                    // If marker == PACKFILEINDEX_END (0xFFFF), we're done with this file
                    if (marker == PACKFILEINDEX_END)
                        break;
                    // Otherwise (marker == PACKFILEINDEX_SEP or something else),
                    // keep reading another chunk descriptor
                }

                // Finished reading all chunks for this file => add block to entries
                m_EntryBlocks.push_back(block);
                m_PakFileIndices.insert(block.m_iPackFileIndex);

                // After the chunk loop, Valve code writes a single 0‑byte as a
                // separator for the next filename. But we do NOT forcibly read
                // it here, because readNullTerminatedString(...) for the next
                // filename will consume it automatically when that filename
                // turns out to be "" => break. So our next iteration
                // readNullTerminatedString() covers that.
            } // filename loop
        } // path loop
    } // extension loop

    ifs.close();
    m_bInitFailed = false;
}

void VPKDir_t::BuildDirectoryFile(const std::string& directoryPath,
                                  const std::vector<VPKEntryBlock_t>& entryBlocks)
{
    std::ofstream ofs(directoryPath, std::ios::binary);
    if (!ofs.is_open())
    {
        std::cerr << "[ReVPK] ERROR: Could not write directory file: " << directoryPath << "\n";
        return;
    }

    VPKDirHeader_t header;
    header.m_nHeaderMarker = VPK_HEADER_MARKER;
    header.m_nMajorVersion = VPK_MAJOR_VERSION;
    header.m_nMinorVersion = VPK_MINOR_VERSION;
    header.m_nDirectorySize= 0;
    header.m_nSignatureSize= 0;

    ofs.write(reinterpret_cast<const char*>(&header.m_nHeaderMarker),   sizeof(header.m_nHeaderMarker));
    ofs.write(reinterpret_cast<const char*>(&header.m_nMajorVersion),   sizeof(header.m_nMajorVersion));
    ofs.write(reinterpret_cast<const char*>(&header.m_nMinorVersion),   sizeof(header.m_nMinorVersion));
    ofs.write(reinterpret_cast<const char*>(&header.m_nDirectorySize),  sizeof(header.m_nDirectorySize));
    ofs.write(reinterpret_cast<const char*>(&header.m_nSignatureSize),  sizeof(header.m_nSignatureSize));

    // Build tree
    CTreeBuilder builder;
    builder.BuildTree(entryBlocks);
    int nDescriptors = builder.WriteTree(ofs);

    // trailing 0
    uint8_t z = 0;
    ofs.write(reinterpret_cast<const char*>(&z), 1);

    auto endPos = ofs.tellp();
    uint32_t dirSize = static_cast<uint32_t>(std::streamoff(endPos) - sizeof(VPKDirHeader_t));
    ofs.seekp(0, std::ios::beg);

    header.m_nDirectorySize = dirSize;
    ofs.write(reinterpret_cast<const char*>(&header.m_nHeaderMarker),   sizeof(header.m_nHeaderMarker));
    ofs.write(reinterpret_cast<const char*>(&header.m_nMajorVersion),   sizeof(header.m_nMajorVersion));
    ofs.write(reinterpret_cast<const char*>(&header.m_nMinorVersion),   sizeof(header.m_nMinorVersion));
    ofs.write(reinterpret_cast<const char*>(&header.m_nDirectorySize),  sizeof(header.m_nDirectorySize));
    ofs.write(reinterpret_cast<const char*>(&header.m_nSignatureSize),  sizeof(header.m_nSignatureSize));

    ofs.close();

    std::cout << "[ReVPK] Directory built at " << directoryPath
    << " with " << entryBlocks.size() << " entries and "
    << nDescriptors << " descriptors.\n";
}

std::string VPKDir_t::GetPackFileNameForIndex(uint16_t iPackFileIndex) const
{
    std::string stripped = StripLocalePrefix(m_DirFilePath);
    std::string from = "pak000_dir";
    std::string to   = "pak000_";
    char patchBuf[16];
    std::snprintf(patchBuf, sizeof(patchBuf), "%03d", iPackFileIndex);
    to += patchBuf;

    return std::regex_replace(stripped, std::regex(from), to);
}

std::string VPKDir_t::StripLocalePrefix(const std::string& directoryPath) const
{
    // e.g. "englishserver_mp_rr_box.bsp.pak000_dir.vpk" -> "server_mp_rr_box.bsp.pak000_dir.vpk"
    static const char* knownLocales[] = {
        "english", "french", "german", "italian", "spanish", "russian",
        "polish", "japanese", "korean", "tchinese", "portuguese"
    };
    auto fname = std::filesystem::path(directoryPath).filename().string();
    for (auto& loc : knownLocales)
    {
        size_t locLen = std::strlen(loc);
        if (fname.compare(0, locLen, loc) == 0)
        {
            fname.erase(0, locLen);
            break;
        }
    }
    return fname;
}
void VPKDir_t::WriteHeader(std::ofstream& /*ofs*/)
{
    // Not used in this example.
}

void VPKDir_t::CTreeBuilder::BuildTree(const std::vector<VPKEntryBlock_t>& entryBlocks)
{
    for (auto& blk : entryBlocks)
    {
        auto pos = blk.m_EntryPath.rfind('.');
        std::string ext;
        std::string path;
        std::string fileNoExt;

        if (pos != std::string::npos)
            ext = blk.m_EntryPath.substr(pos+1);

        auto slashPos = blk.m_EntryPath.rfind('/');
        if (slashPos != std::string::npos)
        {
            path = blk.m_EntryPath.substr(0, slashPos);
            fileNoExt = (pos != std::string::npos)
            ? blk.m_EntryPath.substr(slashPos+1, pos - (slashPos+1))
            : blk.m_EntryPath.substr(slashPos+1);
        }
        else
        {
            path = "";
            fileNoExt = (pos != std::string::npos)
            ? blk.m_EntryPath.substr(0, pos)
            : blk.m_EntryPath;
        }
        if (path.empty()) path = " ";

        m_FileTree[ext][path].push_back(blk);
    }
}

int VPKDir_t::CTreeBuilder::WriteTree(std::ofstream& ofs) const
{
    int descriptorCount = 0;
    for (auto& extPair : m_FileTree)
    {
        ofs.write(extPair.first.c_str(), extPair.first.size());
        ofs.put('\0');

        for (auto& pathPair : extPair.second)
        {
            ofs.write(pathPair.first.c_str(), pathPair.first.size());
            ofs.put('\0');

            for (auto& block : pathPair.second)
            {
                // compute "filename" by removing path + extension
                std::string filename;
                {
                    auto slashPos = block.m_EntryPath.rfind('/');
                    auto dotPos   = block.m_EntryPath.rfind('.');
                    if (slashPos == std::string::npos)
                        slashPos = 0;
                    else
                        slashPos += 1;
                    if (dotPos == std::string::npos || dotPos < slashPos)
                        dotPos = block.m_EntryPath.size();

                    filename = block.m_EntryPath.substr(slashPos, dotPos - slashPos);
                }

                ofs.write(filename.c_str(), filename.size());
                ofs.put('\0');

                // write CRC, preload, packFileIndex
                ofs.write(reinterpret_cast<const char*>(&block.m_nFileCRC),     sizeof(block.m_nFileCRC));
                ofs.write(reinterpret_cast<const char*>(&block.m_iPreloadSize), sizeof(block.m_iPreloadSize));
                ofs.write(reinterpret_cast<const char*>(&block.m_iPackFileIndex), sizeof(block.m_iPackFileIndex));

                // now chunk descriptors
                for (size_t i = 0; i < block.m_Fragments.size(); i++)
                {
                    auto& d = block.m_Fragments[i];

                    ofs.write(reinterpret_cast<const char*>(&d.m_nLoadFlags),     sizeof(d.m_nLoadFlags));
                    ofs.write(reinterpret_cast<const char*>(&d.m_nTextureFlags),  sizeof(d.m_nTextureFlags));
                    ofs.write(reinterpret_cast<const char*>(&d.m_nPackFileOffset),sizeof(d.m_nPackFileOffset));
                    ofs.write(reinterpret_cast<const char*>(&d.m_nCompressedSize),sizeof(d.m_nCompressedSize));
                    ofs.write(reinterpret_cast<const char*>(&d.m_nUncompressedSize),sizeof(d.m_nUncompressedSize));

                    if (i < block.m_Fragments.size() - 1)
                    {
                        uint16_t sep = PACKFILEINDEX_SEP;
                        ofs.write(reinterpret_cast<const char*>(&sep), sizeof(sep));
                    }
                    else
                    {
                        uint16_t end = PACKFILEINDEX_END;
                        ofs.write(reinterpret_cast<const char*>(&end), sizeof(end));
                    }
                    descriptorCount++;
                }
            }
            uint8_t zero = 0;
            ofs.write(reinterpret_cast<const char*>(&zero), 1);
        }
        uint8_t zero = 0;
        ofs.write(reinterpret_cast<const char*>(&zero), 1);
    }
    return descriptorCount;
}

// ------------------------------------------------------------------------
//  VPKPair_t
// ------------------------------------------------------------------------
VPKPair_t::VPKPair_t(const char* pLocale,
                     const char* pTarget,
                     const char* pLevel,
                     int         nPatch)
{
    std::string loc = (pLocale && *pLocale) ? pLocale : "english";
    std::string tgt = (pTarget && *pTarget) ? pTarget : "server";
    std::string lvl = (pLevel  && *pLevel)  ? pLevel  : "map_unknown";

    char patchBuf[16];
    std::snprintf(patchBuf, sizeof(patchBuf), "%03d", nPatch);

    // <target>_<level>.bsp.pak000_XXX.vpk
    m_PackName = tgt + "_" + lvl + ".bsp.pak000_" + patchBuf + ".vpk";

    // <locale><target>_<level>.bsp.pak000_dir.vpk
    m_DirName  = loc + tgt + "_" + lvl + ".bsp.pak000_dir.vpk";
}

// ------------------------------------------------------------------------
//  Utility: get base name from a directory file path
// ------------------------------------------------------------------------
std::string PackedStore_GetDirBaseName(const std::string& dirFileName)
{
    // e.g. "englishserver_mp_rr_box"
    std::smatch matches;
    if (std::regex_search(dirFileName, matches, g_VpkDirFileRegex))
    {
        if (matches.size() >= 3)
        {
            return matches[1].str() + "_" + matches[2].str();
        }
    }
    return dirFileName; // fallback
}

// ------------------------------------------------------------------------
//  CPackedStoreBuilder::UnpackStoreDifferences()
// ------------------------------------------------------------------------
void CPackedStoreBuilder::UnpackStoreDifferences(
    const VPKDir_t& fallbackDir,     // Typically English
    const VPKDir_t& otherLangDir,    // Current language
    const std::string& fallbackOutputPath, // e.g. outPath/content/english/
    const std::string& langOutputPath      // e.g. outPath/content/spanish/
)
{
    namespace fs = std::filesystem;

    // We'll read from the "otherLangDir" chunk files
    // For each file in 'otherLangDir', check if it is truly different from fallback.
    // If yes, physically extract. If no, skip.

    // Build a quick index: (CRC -> vector<filePaths>) for fallback or something chunk-based.
    // For simplest approach, do file-level compare via CRC.
    // If the entire file's CRC matches fallback's CRC, skip.

    // 1) Make a map from <entryPath> to <CRC> for the fallback
    std::unordered_map<std::string, uint32_t> fallbackCrcMap;
    for (auto& fbBlock : fallbackDir.m_EntryBlocks)
    {
        fallbackCrcMap[fbBlock.m_EntryPath] = fbBlock.m_nFileCRC;
    }

    // 2) Open chunk file(s) for reading
    // Because we typically have only one .vpk data pack, the offset might be the same.
    // But in multi-lingual, you can also have separate data packs.
    // We'll do a naive approach: for each block, open its pack file by index.

    fs::path baseDir = fs::path(otherLangDir.m_DirFilePath).parent_path();
    std::ifstream ifsPak;

    // 3) For each block in 'otherLangDir'
    for (auto& block : otherLangDir.m_EntryBlocks)
    {
        auto itCrc = fallbackCrcMap.find(block.m_EntryPath);
        bool sameAsFallback = (itCrc != fallbackCrcMap.end()
            && itCrc->second == block.m_nFileCRC);

        // if the file is the same as fallback (via CRC), skip extracting
        if (sameAsFallback)
            continue;

        // We physically extract it
        // 3a) open the correct data file once we know block.m_iPackFileIndex
        std::string packFileName = otherLangDir.GetPackFileNameForIndex(block.m_iPackFileIndex);
        fs::path fullPakPath = baseDir / packFileName;
        if (!ifsPak.is_open() || ifsPak.tellg() != std::streampos(0) || /* naive check */ true)
        {
            ifsPak.close();
            ifsPak.open(fullPakPath, std::ios::binary);
            if (!ifsPak.good())
            {
                std::cerr << "[ReVPK] ERROR: cannot open " << fullPakPath
                    << " for reading\n";
                continue;
            }
        }

        // 3b) open output file
        fs::path outFile = fs::path(langOutputPath) / block.m_EntryPath;
        fs::create_directories(outFile.parent_path());
        std::ofstream ofsOut(outFile, std::ios::binary);
        if (!ofsOut.is_open())
        {
            std::cerr << "[ReVPK] ERROR: cannot create " << outFile << "\n";
            continue;
        }

        // 3c) read each chunk
        std::unique_ptr<uint8_t[]> srcBuf(new uint8_t[VPK_ENTRY_MAX_LEN]);
        std::unique_ptr<uint8_t[]> dstBuf(new uint8_t[VPK_ENTRY_MAX_LEN]);

        for (auto& frag : block.m_Fragments)
        {
            // if packFileOffset/CompressedSize=0 => deduplicated chunk from somewhere else
            if (frag.m_nPackFileOffset == 0 && frag.m_nCompressedSize == 0)
                continue;

            ifsPak.seekg(frag.m_nPackFileOffset, std::ios::beg);
            if (!ifsPak.good())
            {
                std::cerr << "[ReVPK] ERROR: Bad seek in " << fullPakPath << "\n";
                break;
            }

            ifsPak.read(reinterpret_cast<char*>(srcBuf.get()), frag.m_nCompressedSize);

            // decompress if needed
            if (frag.m_nCompressedSize == frag.m_nUncompressedSize)
            {
                // uncompressed
                ofsOut.write(reinterpret_cast<const char*>(srcBuf.get()), frag.m_nUncompressedSize);
            }
            else
            {
                // check for ZSTD marker
                if (frag.m_nCompressedSize >= sizeof(R1D_marker))
                {
                    uint64_t possibleMarker = 0;
                    std::memcpy(&possibleMarker, srcBuf.get(), sizeof(R1D_marker));
                    if (possibleMarker == R1D_marker)
                    {
                        // ZSTD chunk
                        size_t zstdSize = frag.m_nCompressedSize - sizeof(R1D_marker);
                        const uint8_t* zstdData = srcBuf.get() + sizeof(R1D_marker);
                        size_t dstLen = VPK_ENTRY_MAX_LEN;
                        size_t dRes = ZSTD_decompress(dstBuf.get(), dstLen, zstdData, zstdSize);
                        if (!ZSTD_isError(dRes))
                        {
                            ofsOut.write(reinterpret_cast<const char*>(dstBuf.get()), dRes);
                        }
                        else
                        {
                            std::cerr << "[ReVPK] ERROR: ZSTD decompress failed.\n";
                        }
                        continue;
                    }
                }
                // LZHAM path
                size_t dstLen = VPK_ENTRY_MAX_LEN;
                lzham_decompress_status_t st = lzham_decompress_memory(
                    &m_Decoder,
                    dstBuf.get(), &dstLen,
                    srcBuf.get(), static_cast<size_t>(frag.m_nCompressedSize),
                    nullptr
                );
                if (st == LZHAM_DECOMP_STATUS_SUCCESS)
                {
                    ofsOut.write(reinterpret_cast<const char*>(dstBuf.get()), dstLen);
                }
                else
                {
                    std::cerr << "[ReVPK] ERROR: LZHAM decompress failed.\n";
                }
            }
        }
    }

    ifsPak.close();
}

// ------------------------------------------------------------------------
//  CPackedStoreBuilder::BuildMultiLangManifest()
// ------------------------------------------------------------------------
bool CPackedStoreBuilder::BuildMultiLangManifest(
    const std::map<std::string, VPKDir_t>& languageDirs,
    const std::string& outFilePath)
{
    std::ofstream ofs(outFilePath);
    if (!ofs.good())
        return false;

    ofs << "\"BuildManifest\"\n{\n";

    // 1) Gather all file paths across all languages
    std::set<std::string> allFilePaths;
    for (const auto& langPair : languageDirs)
    {
        const VPKDir_t& vpkDir = langPair.second;
        for (const auto& blk : vpkDir.m_EntryBlocks)
            allFilePaths.insert(blk.m_EntryPath);
    }

    // 2) Build a map of English CRCs (or blocks)
    std::unordered_map<std::string, const VPKEntryBlock_t*> englishMap;
    auto itEnglish = languageDirs.find("english");
    if (itEnglish != languageDirs.end())
    {
        for (const auto& blk : itEnglish->second.m_EntryBlocks)
        {
            englishMap[blk.m_EntryPath] = &blk;
        }
    }

    // 3) For each language, write sub-block
    for (const auto& langPair : languageDirs)
    {
        const std::string& lang = langPair.first;
        const VPKDir_t&    vpkDir = langPair.second;

        // Build a quick map for this language, path -> block
        std::unordered_map<std::string, const VPKEntryBlock_t*> thisLangMap;
        for (const auto& blk : vpkDir.m_EntryBlocks)
        {
            thisLangMap[blk.m_EntryPath] = &blk;
        }

        ofs << "\t\"" << lang << "\"\n\t{\n";

        // 4) Iterate all possible file paths
        for (const std::string& filePath : allFilePaths)
        {
            // Check if this language has a custom block
            auto itBlock = thisLangMap.find(filePath);
            const VPKEntryBlock_t* blockPtr = nullptr;
            if (itBlock != thisLangMap.end())
            {
                // Language has a unique (or differing) version
                blockPtr = itBlock->second;
            }
            else
            {
                // Fallback to English (if English has it)
                auto itEng = englishMap.find(filePath);
                if (itEng != englishMap.end())
                    blockPtr = itEng->second;
            }

            // If we found a block (either language or fallback), write a manifest entry
            if (blockPtr)
            {
                bool compressed = false;
                for (const auto& frag : blockPtr->m_Fragments)
                {
                    if (frag.m_nCompressedSize < frag.m_nUncompressedSize)
                    {
                        compressed = true;
                        break;
                    }
                }

                ofs << "\t\t\"" << filePath << "\"\n"
                    << "\t\t{\n"
                    << "\t\t\t\"preloadSize\"\t\"" << blockPtr->m_iPreloadSize << "\"\n"
                    << "\t\t\t\"loadFlags\"\t\""
                            << (blockPtr->m_Fragments.empty()
                                ? 3
                                : blockPtr->m_Fragments[0].m_nLoadFlags)
                            << "\"\n"
                    << "\t\t\t\"textureFlags\"\t\""
                            << (blockPtr->m_Fragments.empty()
                                ? 0
                                : blockPtr->m_Fragments[0].m_nTextureFlags)
                            << "\"\n"
                    << "\t\t\t\"useCompression\"\t\"" << (compressed ? "1" : "0") << "\"\n"
                    << "\t\t\t\"deDuplicate\"\t\"1\"\n"
                    << "\t\t}\n";
            }
        }

        ofs << "\t}\n";
    }

    ofs << "}\n";
    return true;
}
