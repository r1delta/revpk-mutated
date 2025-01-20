/**
 * packedstore.h
 *
 * Core header for ReVPK, storing definitions for:
 *  - VPK chunk/entry structures
 *  - The CPackedStoreBuilder class
 *  - The VPKDir_t structure
 *
 * We now rely on real hashing from external libraries (OpenSSL).
 */

#ifndef PACKEDSTORE_H
#define PACKEDSTORE_H

#include <cstdint>
#include <string>
#include <vector>
#include <set>
#include <map>
#include <list>
#include <regex>
#include <unordered_map>
#include "lzham.h"

// --- ZSTD support ---
#include <zstd.h>
// --------------------

/** Maximum # of helper threads, if not provided by LZHAM */
#ifndef LZHAM_MAX_HELPER_THREADS
#define LZHAM_MAX_HELPER_THREADS 128
#endif

// Constants
static constexpr uint32_t VPK_HEADER_MARKER = 0x55AA1234;
static constexpr uint16_t VPK_MAJOR_VERSION = 2;
static constexpr uint16_t VPK_MINOR_VERSION = 3;
static constexpr uint32_t VPK_DICT_SIZE     = 20;          // LZHAM dictionary size (log2)
static constexpr size_t   VPK_ENTRY_MAX_LEN = 1024 * 1024; // 1 MiB
static constexpr uint16_t PACKFILEINDEX_SEP = 0x0000;
static constexpr uint16_t PACKFILEINDEX_END = 0xffff;

// Regex for directory + pack file parsing
static const std::regex g_VpkDirFileRegex (R"((?:.*\/)?([^_]*)(?:_)(.*)(\.bsp\.pak000_dir).*)");
static const std::regex g_VpkPackFileRegex(R"(pak000_([0-9]{3}))");

/** Flags placeholders */
namespace EPackedLoadFlags
{
    static constexpr uint32_t LOAD_VISIBLE = 1 << 0;
    static constexpr uint32_t LOAD_CACHE   = 1 << 1;
}
namespace EPackedTextureFlags
{
    static constexpr uint16_t TEXTURE_DEFAULT = 0;
}

/** KeyValue-like struct used for building the VPK.
 *  Typically parsed from a KeyValues or .vdf manifest.
 */
struct VPKKeyValues_t
{
    static constexpr uint32_t LOAD_FLAGS_DEFAULT =
        (EPackedLoadFlags::LOAD_VISIBLE | EPackedLoadFlags::LOAD_CACHE);
    static constexpr uint16_t TEXTURE_FLAGS_DEFAULT =
        EPackedTextureFlags::TEXTURE_DEFAULT;

    std::string m_EntryPath;      // actual local filesystem path
    uint16_t    m_iPreloadSize;
    uint32_t    m_nLoadFlags;
    uint16_t    m_nTextureFlags;
    bool        m_bUseCompression;
    bool        m_bDeduplicate;

    VPKKeyValues_t(const std::string& entryPath = "",
                   uint16_t preloadSize         = 0,
                   uint32_t loadFlags           = LOAD_FLAGS_DEFAULT,
                   uint16_t textureFlags        = TEXTURE_FLAGS_DEFAULT,
                   bool useCompression          = true,
                   bool deduplicate            = true)
    : m_EntryPath(entryPath), m_iPreloadSize(preloadSize),
    m_nLoadFlags(loadFlags), m_nTextureFlags(textureFlags),
    m_bUseCompression(useCompression), m_bDeduplicate(deduplicate)
    {}
};

/** A single chunk descriptor: offset in the .vpk, compressed/uncompressed size, etc. */
struct VPKChunkDescriptor_t
{
    uint32_t m_nLoadFlags;
    uint16_t m_nTextureFlags;
    uint64_t m_nPackFileOffset;
    uint64_t m_nCompressedSize;
    uint64_t m_nUncompressedSize;

    VPKChunkDescriptor_t()
    : m_nLoadFlags(0), m_nTextureFlags(0),
    m_nPackFileOffset(0), m_nCompressedSize(0),
    m_nUncompressedSize(0)
    {}

    VPKChunkDescriptor_t(uint32_t loadFlags, uint16_t textureFlags,
                         uint64_t packOffset, uint64_t compSize, uint64_t uncompSize)
    : m_nLoadFlags(loadFlags), m_nTextureFlags(textureFlags),
    m_nPackFileOffset(packOffset),
    m_nCompressedSize(compSize), m_nUncompressedSize(uncompSize)
    {}
};

/** Represents one file in the VPK. Big files get split into multiple 1 MiB fragments. */
struct VPKEntryBlock_t
{
    uint32_t                        m_nFileCRC;       // CRC32
    uint16_t                        m_iPreloadSize;
    uint16_t                        m_iPackFileIndex; // which .vpk chunk?
    std::vector<VPKChunkDescriptor_t> m_Fragments;
    std::string                     m_EntryPath;

    // Memory-based constructor (used when packing)
    VPKEntryBlock_t(const uint8_t* pData, size_t nLen, uint64_t nOffset,
                    uint16_t iPreloadSize, uint16_t iPackFileIndex,
                    uint32_t nLoadFlags, uint16_t nTextureFlags,
                    const char* pEntryPath);

    // Copy constructor
    VPKEntryBlock_t(const VPKEntryBlock_t& other) = default;
    // Default
    VPKEntryBlock_t()
    : m_nFileCRC(0), m_iPreloadSize(0), m_iPackFileIndex(0)
    {}
};

/** The .vpk directory file header */
struct VPKDirHeader_t
{
    uint32_t m_nHeaderMarker;
    uint16_t m_nMajorVersion;
    uint16_t m_nMinorVersion;
    uint32_t m_nDirectorySize;
    uint32_t m_nSignatureSize;

    VPKDirHeader_t()
    : m_nHeaderMarker(0),
    m_nMajorVersion(0),
    m_nMinorVersion(0),
    m_nDirectorySize(0),
    m_nSignatureSize(0)
    {}
};

/**
 *  The .vpk directory data. Contains references to all files (EntryBlocks).
 */
struct VPKDir_t
{
    VPKDirHeader_t                m_Header;
    std::string                   m_DirFilePath;
    std::vector<VPKEntryBlock_t>  m_EntryBlocks;
    std::set<uint16_t>            m_PakFileIndices;
    bool                          m_bInitFailed;

    VPKDir_t();
    VPKDir_t(const std::string& dirFilePath, bool bSanitize=false);

    bool Failed() const { return m_bInitFailed; }
    void Init(const std::string& dirFilePath);

    // Build the final directory file given a set of EntryBlocks
    void BuildDirectoryFile(const std::string& directoryPath,
                            const std::vector<VPKEntryBlock_t>& entryBlocks);

    // We partially replicate Valve’s naming: if we have "xxx_dir.vpk",
    // we replace "pak000_dir" with "pak000_00x" to get chunk file names, etc.
    std::string GetPackFileNameForIndex(uint16_t iPackFileIndex) const;
    std::string StripLocalePrefix(const std::string& directoryPath) const;

    void WriteHeader(std::ofstream& ofs); // Not strictly needed in this example.

    // Helper sub-structure to store the "directory tree"
    struct CTreeBuilder
    {
        // For each extension => for each path => list of blocks
        using PathContainer_t = std::map<std::string, std::list<VPKEntryBlock_t>>;
        using TypeContainer_t = std::map<std::string, PathContainer_t>;

        TypeContainer_t m_FileTree;

        void BuildTree(const std::vector<VPKEntryBlock_t>& entryBlocks);
        int  WriteTree(std::ofstream& ofs) const;
    };
};

/** A small struct storing the directory name + pack name for building a single-level VPK. */
struct VPKPair_t
{
    std::string m_PackName;
    std::string m_DirName;

    VPKPair_t(const char* pLocale,
              const char* pTarget,
              const char* pLevel,
              int         nPatch);
};

// --- ZSTD support ---
enum ECompressionMethod
{
    kCompressionNone = 0,  // no compression
    kCompressionLZHAM,
    kCompressionZSTD
};
// --------------------

/** The main class that packs/unpacks from a VPK. */
class CPackedStoreBuilder
{
public:
    // --- ZSTD support ---
    CPackedStoreBuilder()
    : m_eCompressionMethod(kCompressionLZHAM) // default to LZHAM
    {}
    // --------------------

    void InitLzEncoder(int maxHelperThreads, const char* compressionLevel);
    void InitLzDecoder();

    // Deduplicate a chunk: if we’ve seen identical data (SHA1) before,
    // point descriptor to existing chunk
    bool Deduplicate(const uint8_t* pEntryBuffer,
                     VPKChunkDescriptor_t& descriptor,
                     size_t finalSize);

    // Pack everything into a single .vpk pack + .vpk directory file
    void PackStore(const VPKPair_t& vpkPair,
                   const char* workspaceName,
                   const char* buildPath);

    // Unpack from an existing directory file
    void UnpackStore(const VPKDir_t& vpkDir, const char* workspaceName = "");

    // Unpack differences between two languages
    void UnpackStoreDifferences(
        const VPKDir_t& fallbackDir,     // Typically English
        const VPKDir_t& otherLangDir,    // Current language
        const std::string& fallbackOutputPath, // e.g. outPath/content/english/
        const std::string& langOutputPath      // e.g. outPath/content/spanish/
    );

    // Build a multi-language manifest
    bool BuildMultiLangManifest(
        const std::map<std::string, VPKDir_t>& languageDirs,
        const std::string& outFilePath
    );

public:
    lzham_compress_params   m_Encoder;
    lzham_decompress_params m_Decoder;

    // Dedup map: from SHA1 hash => descriptor
    // so multiple identical chunks get a single copy
    std::unordered_map<std::string, VPKChunkDescriptor_t> m_ChunkHashMap;

    // --- ZSTD support ---
    ECompressionMethod m_eCompressionMethod;
    inline bool IsUsingZSTD() const { return m_eCompressionMethod == kCompressionZSTD; }
    // --------------------
};

// Utility
std::string PackedStore_GetDirBaseName(const std::string& dirFileName);

// New helper struct for multi-language manifest
struct LangKVPair_t
{
    std::string m_Language;
    VPKKeyValues_t m_Keys;

    LangKVPair_t(const std::string& lang, const VPKKeyValues_t& kv)
        : m_Language(lang), m_Keys(kv) {}
};
// --- ZSTD support ---
// 64-bit marker for easy detection:
static constexpr uint64_t R1D_marker    = 0x5244315F5F4D4150ULL;
// Example only; pick any 8-byte literal you like.
// If you prefer exactly the numeric literal you gave, you can keep that:
// static constexpr uint64_t R1D_marker = 18388560042537298ULL;

// 32-bit marker if needed:
static constexpr uint32_t R1D_marker_32 = 0x52443144; // 'R1D'
std::string compute_sha1_hex(const uint8_t* data, size_t len);

#endif // PACKEDSTORE_H
