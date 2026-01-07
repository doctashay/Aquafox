/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*- */
/* vim: set ts=8 sts=4 et sw=4 tw=99: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "mozJSBytecodeCache.h"

#include "jsapi.h"
#include "jsfriendapi.h"
#include "mozilla/Endian.h"
#include "mozilla/Logging.h"
#include "mozilla/Preferences.h"
#include "nsAutoPtr.h"
#include "nsCOMPtr.h"
#include "nsIFile.h"
#include "nsIFileURL.h"
#include "nsICryptoHash.h"
#include "nsComponentManagerUtils.h"
#include "nsNetUtil.h"
#include "nsDirectoryServiceUtils.h"
#include "prprf.h"
#include "prio.h"
#include "prenv.h"

// XDR version from SpiderMonkey - we need this for validation
#include "vm/Xdr.h"

using namespace mozilla;

static PRLogModuleInfo* gJSCacheLog;

#define LOG(args) MOZ_LOG(gJSCacheLog, mozilla::LogLevel::Debug, args)

// Preference key for enabling/disabling bytecode cache
static const char kBytecodeCachePref[] = "javascript.options.bytecode_cache.enabled";

/**
 * Initialize logging if not already done
 */
static void
EnsureLogging()
{
    if (!gJSCacheLog) {
        gJSCacheLog = PR_NewLogModule("JSBytecodeCache");
    }
}

bool
IsBytecodeCacheEnabled()
{
    // Default to enabled for performance
    return Preferences::GetBool(kBytecodeCachePref, true);
}

const char*
JSCCacheResultToString(JSCCacheResult aResult)
{
    switch (aResult) {
        case JSCCacheResult::Success:        return "Success";
        case JSCCacheResult::NotFound:       return "NotFound";
        case JSCCacheResult::InvalidMagic:   return "InvalidMagic";
        case JSCCacheResult::VersionMismatch: return "VersionMismatch";
        case JSCCacheResult::HashMismatch:   return "HashMismatch";
        case JSCCacheResult::LengthMismatch: return "LengthMismatch";
        case JSCCacheResult::ReadError:      return "ReadError";
        case JSCCacheResult::WriteError:     return "WriteError";
        case JSCCacheResult::EncodeError:    return "EncodeError";
        case JSCCacheResult::DecodeError:    return "DecodeError";
        default:                             return "Unknown";
    }
}

nsresult
ComputeSourceHash(const char* aData, uint32_t aLength, uint8_t* aHashOut)
{
    nsresult rv;

    // Create SHA-256 hash instance
    nsCOMPtr<nsICryptoHash> hash =
        do_CreateInstance("@mozilla.org/security/hash;1", &rv);
    NS_ENSURE_SUCCESS(rv, rv);

    rv = hash->Init(nsICryptoHash::SHA256);
    NS_ENSURE_SUCCESS(rv, rv);

    rv = hash->Update(reinterpret_cast<const uint8_t*>(aData), aLength);
    NS_ENSURE_SUCCESS(rv, rv);

    nsAutoCString hashString;
    rv = hash->Finish(false, hashString);
    NS_ENSURE_SUCCESS(rv, rv);

    // Copy the raw hash bytes
    MOZ_ASSERT(hashString.Length() == MOZ_JSC_HASH_SIZE);
    memcpy(aHashOut, hashString.get(), MOZ_JSC_HASH_SIZE);

    return NS_OK;
}

nsresult
GetBytecodeCachePath(nsIFile* aJSFile, nsCOMPtr<nsIFile>& aJSCFile)
{
    nsresult rv;

    // Clone the file
    rv = aJSFile->Clone(getter_AddRefs(aJSCFile));
    NS_ENSURE_SUCCESS(rv, rv);

    // Get the current leaf name
    nsAutoString leafName;
    rv = aJSCFile->GetLeafName(leafName);
    NS_ENSURE_SUCCESS(rv, rv);

    // Replace .js with .jsc (or append .jsc if no extension)
    int32_t dotPos = leafName.RFindChar('.');
    if (dotPos != kNotFound) {
        leafName.Truncate(dotPos);
    }
    leafName.AppendLiteral(".jsc");

    rv = aJSCFile->SetLeafName(leafName);
    NS_ENSURE_SUCCESS(rv, rv);

    return NS_OK;
}

/**
 * Compute MD5 hash of a string for use as filename.
 * Returns hex string suitable for filesystem use.
 */
static nsresult
ComputePathHash(const nsACString& aPath, nsACString& aHashOut)
{
    nsresult rv;
    
    nsCOMPtr<nsICryptoHash> hash =
        do_CreateInstance("@mozilla.org/security/hash;1", &rv);
    NS_ENSURE_SUCCESS(rv, rv);

    // Use MD5 for shorter filenames (32 hex chars vs 64 for SHA-256)
    rv = hash->Init(nsICryptoHash::MD5);
    NS_ENSURE_SUCCESS(rv, rv);

    rv = hash->Update(reinterpret_cast<const uint8_t*>(aPath.BeginReading()),
                      aPath.Length());
    NS_ENSURE_SUCCESS(rv, rv);

    // Get hex-encoded hash
    rv = hash->Finish(true, aHashOut);
    NS_ENSURE_SUCCESS(rv, rv);

    // Replace any non-filesystem-safe characters (base64 uses / and +)
    aHashOut.ReplaceChar('/', '_');
    aHashOut.ReplaceChar('+', '-');
    aHashOut.ReplaceChar('=', '\0'); // Truncate at padding

    return NS_OK;
}

nsresult
GetProfileBytecodeCachePath(nsIFile* aJSFile, nsCOMPtr<nsIFile>& aJSCFile)
{
    nsresult rv;

    // Get profile directory
    nsCOMPtr<nsIFile> profDir;
    rv = NS_GetSpecialDirectory("ProfD", getter_AddRefs(profDir));
    NS_ENSURE_SUCCESS(rv, rv);

    // Create bytecode_cache subdirectory path
    rv = profDir->Append(NS_LITERAL_STRING("bytecode_cache"));
    NS_ENSURE_SUCCESS(rv, rv);

    // Create directory if it doesn't exist
    bool exists;
    rv = profDir->Exists(&exists);
    NS_ENSURE_SUCCESS(rv, rv);
    
    if (!exists) {
        rv = profDir->Create(nsIFile::DIRECTORY_TYPE, 0755);
        if (NS_FAILED(rv) && rv != NS_ERROR_FILE_ALREADY_EXISTS) {
            return rv;
        }
    }

    // Generate unique filename from source path hash
    nsAutoCString sourcePath;
    rv = aJSFile->GetNativePath(sourcePath);
    NS_ENSURE_SUCCESS(rv, rv);

    nsAutoCString pathHash;
    rv = ComputePathHash(sourcePath, pathHash);
    NS_ENSURE_SUCCESS(rv, rv);

    // Build cache filename: hash.jsc
    nsAutoCString cacheFileName(pathHash);
    cacheFileName.AppendLiteral(".jsc");

    rv = profDir->AppendNative(cacheFileName);
    NS_ENSURE_SUCCESS(rv, rv);

    aJSCFile = profDir;
    return NS_OK;
}

nsresult
FindBytecodeCachePath(nsIFile* aJSFile, nsCOMPtr<nsIFile>& aJSCFile, bool* aFoundInProfile)
{
    nsresult rv;
    *aFoundInProfile = false;

    // First check alongside source file (for pre-shipped caches)
    nsCOMPtr<nsIFile> sourceSideCache;
    rv = GetBytecodeCachePath(aJSFile, sourceSideCache);
    if (NS_SUCCEEDED(rv)) {
        bool exists;
        rv = sourceSideCache->Exists(&exists);
        if (NS_SUCCEEDED(rv) && exists) {
            aJSCFile = sourceSideCache;
            return NS_OK;
        }
    }

    // Then check profile directory (for runtime-generated caches)
    nsCOMPtr<nsIFile> profileCache;
    rv = GetProfileBytecodeCachePath(aJSFile, profileCache);
    if (NS_SUCCEEDED(rv)) {
        bool exists;
        rv = profileCache->Exists(&exists);
        if (NS_SUCCEEDED(rv) && exists) {
            aJSCFile = profileCache;
            *aFoundInProfile = true;
            return NS_OK;
        }
    }

    // No cache found - return source-side path for potential creation
    aJSCFile = sourceSideCache;
    return NS_OK;
}

bool
IsDirectoryWritable(nsIFile* aDir)
{
    if (!aDir) {
        return false;
    }

    bool isWritable = false;
    nsresult rv = aDir->IsWritable(&isWritable);
    
    return NS_SUCCEEDED(rv) && isWritable;
}

/**
 * Read and validate a .jsc cache file header
 */
static nsresult
ReadAndValidateHeader(PRFileDesc* aFile,
                      MOZ_JSCHeader* aHeader,
                      JSCCacheResult* aResult)
{
    // Read the header
    int32_t bytesRead = PR_Read(aFile, aHeader, sizeof(MOZ_JSCHeader));
    if (bytesRead != sizeof(MOZ_JSCHeader)) {
        *aResult = JSCCacheResult::ReadError;
        return NS_ERROR_FAILURE;
    }

    // Validate magic number
    if (memcmp(aHeader->magic, MOZ_JSC_MAGIC, MOZ_JSC_MAGIC_SIZE) != 0) {
        *aResult = JSCCacheResult::InvalidMagic;
        return NS_ERROR_FAILURE;
    }

    // Convert from little-endian
    aHeader->xdrVersion = LittleEndian::readUint32(&aHeader->xdrVersion);
    aHeader->sourceLength = LittleEndian::readUint32(&aHeader->sourceLength);
    aHeader->bytecodeLength = LittleEndian::readUint32(&aHeader->bytecodeLength);

    // Validate XDR version
    if (aHeader->xdrVersion != js::XDR_BYTECODE_VERSION) {
        LOG(("JSC version mismatch: file=%u, engine=%u",
             aHeader->xdrVersion, js::XDR_BYTECODE_VERSION));
        *aResult = JSCCacheResult::VersionMismatch;
        return NS_ERROR_FAILURE;
    }

    return NS_OK;
}

/**
 * Validate source hash matches what's in the header
 */
static nsresult
ValidateSourceHash(const MOZ_JSCHeader* aHeader,
                   const char* aSourceData,
                   uint32_t aSourceLength,
                   JSCCacheResult* aResult)
{
    // Check source length first (quick check)
    if (aHeader->sourceLength != aSourceLength) {
        LOG(("Source length mismatch: header=%u, actual=%u",
             aHeader->sourceLength, aSourceLength));
        *aResult = JSCCacheResult::LengthMismatch;
        return NS_ERROR_FAILURE;
    }

    // Compute hash of actual source
    uint8_t computedHash[MOZ_JSC_HASH_SIZE];
    nsresult rv = ComputeSourceHash(aSourceData, aSourceLength, computedHash);
    NS_ENSURE_SUCCESS(rv, rv);

    // Compare hashes
    if (memcmp(aHeader->sourceHash, computedHash, MOZ_JSC_HASH_SIZE) != 0) {
        LOG(("Source hash mismatch - cache is stale"));
        *aResult = JSCCacheResult::HashMismatch;
        return NS_ERROR_FAILURE;
    }

    return NS_OK;
}

nsresult
ReadBytecodeCache(nsIFile* aJSFile,
                  JSContext* aCx,
                  JS::MutableHandleScript aScriptp,
                  JSCCacheResult* aResult)
{
    EnsureLogging();
    
    nsresult rv;
    *aResult = JSCCacheResult::NotFound;

    if (!IsBytecodeCacheEnabled()) {
        return NS_ERROR_NOT_AVAILABLE;
    }

    // Find cache file - check alongside source first, then profile directory
    nsCOMPtr<nsIFile> jscFile;
    bool foundInProfile = false;
    rv = FindBytecodeCachePath(aJSFile, jscFile, &foundInProfile);
    NS_ENSURE_SUCCESS(rv, rv);

    // Check if cache file exists
    bool exists;
    rv = jscFile->Exists(&exists);
    NS_ENSURE_SUCCESS(rv, rv);

    if (!exists) {
        LOG(("Bytecode cache not found"));
        *aResult = JSCCacheResult::NotFound;
        return NS_ERROR_FILE_NOT_FOUND;
    }

    if (foundInProfile) {
        nsAutoCString path;
        jscFile->GetNativePath(path);
        LOG(("Found bytecode cache in profile: %s", path.get()));
    }

    // Open the cache file
    PRFileDesc* fd;
    rv = jscFile->OpenNSPRFileDesc(PR_RDONLY, 0, &fd);
    if (NS_FAILED(rv)) {
        *aResult = JSCCacheResult::ReadError;
        return rv;
    }

    // Ensure file is closed on all paths
    class FileCloser {
    public:
        explicit FileCloser(PRFileDesc* f) : mFile(f) {}
        ~FileCloser() { PR_Close(mFile); }
    private:
        PRFileDesc* mFile;
    } fileCloser(fd);

    // Read and validate header
    MOZ_JSCHeader header;
    rv = ReadAndValidateHeader(fd, &header, aResult);
    if (NS_FAILED(rv)) {
        return rv;
    }

    // We need to read the source file to validate the hash
    // Read source file
    int64_t sourceSize;
    rv = aJSFile->GetFileSize(&sourceSize);
    NS_ENSURE_SUCCESS(rv, rv);

    if (sourceSize > UINT32_MAX) {
        *aResult = JSCCacheResult::ReadError;
        return NS_ERROR_FILE_TOO_BIG;
    }

    PRFileDesc* srcFd;
    rv = aJSFile->OpenNSPRFileDesc(PR_RDONLY, 0, &srcFd);
    if (NS_FAILED(rv)) {
        *aResult = JSCCacheResult::ReadError;
        return rv;
    }

    nsAutoArrayPtr<char> sourceData(new char[sourceSize]);
    int32_t srcBytesRead = PR_Read(srcFd, sourceData.get(), sourceSize);
    PR_Close(srcFd);

    if (srcBytesRead != sourceSize) {
        *aResult = JSCCacheResult::ReadError;
        return NS_ERROR_FAILURE;
    }

    // Validate source hash
    rv = ValidateSourceHash(&header, sourceData.get(), sourceSize, aResult);
    if (NS_FAILED(rv)) {
        return rv;
    }

    // Read bytecode data
    nsAutoArrayPtr<char> bytecodeData(new char[header.bytecodeLength]);
    int32_t bytesRead = PR_Read(fd, bytecodeData.get(), header.bytecodeLength);
    if (bytesRead != (int32_t)header.bytecodeLength) {
        *aResult = JSCCacheResult::ReadError;
        return NS_ERROR_FAILURE;
    }

    // Decode the script
    JSScript* script = JS_DecodeScript(aCx, bytecodeData.get(), header.bytecodeLength);
    if (!script) {
        LOG(("Failed to decode bytecode"));
        *aResult = JSCCacheResult::DecodeError;
        return NS_ERROR_FAILURE;
    }

    aScriptp.set(script);
    *aResult = JSCCacheResult::Success;

    nsAutoCString path;
    aJSFile->GetNativePath(path);
    LOG(("Successfully loaded bytecode cache for %s", path.get()));

    return NS_OK;
}

nsresult
ReadBytecodeCacheFromURI(nsIURI* aURI,
                         const char* aSourceData,
                         uint32_t aSourceLength,
                         JSContext* aCx,
                         JS::MutableHandleScript aScriptp,
                         JSCCacheResult* aResult)
{
    EnsureLogging();
    
    nsresult rv;
    *aResult = JSCCacheResult::NotFound;

    if (!IsBytecodeCacheEnabled()) {
        return NS_ERROR_NOT_AVAILABLE;
    }

    // Try to get the file from the URI
    nsCOMPtr<nsIFileURL> fileURL = do_QueryInterface(aURI);
    if (!fileURL) {
        // Not a file URL, can't use bytecode cache
        return NS_ERROR_NOT_AVAILABLE;
    }

    nsCOMPtr<nsIFile> jsFile;
    rv = fileURL->GetFile(getter_AddRefs(jsFile));
    if (NS_FAILED(rv) || !jsFile) {
        return NS_ERROR_NOT_AVAILABLE;
    }

    // Get the .jsc file path
    nsCOMPtr<nsIFile> jscFile;
    rv = GetBytecodeCachePath(jsFile, jscFile);
    NS_ENSURE_SUCCESS(rv, rv);

    // Check if cache file exists
    bool exists;
    rv = jscFile->Exists(&exists);
    NS_ENSURE_SUCCESS(rv, rv);

    if (!exists) {
        *aResult = JSCCacheResult::NotFound;
        return NS_ERROR_FILE_NOT_FOUND;
    }

    // Open the cache file
    PRFileDesc* fd;
    rv = jscFile->OpenNSPRFileDesc(PR_RDONLY, 0, &fd);
    if (NS_FAILED(rv)) {
        *aResult = JSCCacheResult::ReadError;
        return rv;
    }

    // Ensure file is closed on all paths
    class FileCloser {
    public:
        explicit FileCloser(PRFileDesc* f) : mFile(f) {}
        ~FileCloser() { PR_Close(mFile); }
    private:
        PRFileDesc* mFile;
    } fileCloser(fd);

    // Read and validate header
    MOZ_JSCHeader header;
    rv = ReadAndValidateHeader(fd, &header, aResult);
    if (NS_FAILED(rv)) {
        return rv;
    }

    // Validate source hash using provided source data
    rv = ValidateSourceHash(&header, aSourceData, aSourceLength, aResult);
    if (NS_FAILED(rv)) {
        return rv;
    }

    // Read bytecode data
    nsAutoArrayPtr<char> bytecodeData(new char[header.bytecodeLength]);
    int32_t bytesRead = PR_Read(fd, bytecodeData.get(), header.bytecodeLength);
    if (bytesRead != (int32_t)header.bytecodeLength) {
        *aResult = JSCCacheResult::ReadError;
        return NS_ERROR_FAILURE;
    }

    // Decode the script
    JSScript* script = JS_DecodeScript(aCx, bytecodeData.get(), header.bytecodeLength);
    if (!script) {
        LOG(("Failed to decode bytecode from URI cache"));
        *aResult = JSCCacheResult::DecodeError;
        return NS_ERROR_FAILURE;
    }

    aScriptp.set(script);
    *aResult = JSCCacheResult::Success;

    nsAutoCString spec;
    aURI->GetSpec(spec);
    LOG(("Successfully loaded bytecode cache for %s", spec.get()));

    return NS_OK;
}

/*
 * =============================================================================
 * CACHE WRITER IMPLEMENTATION (Phase 2)
 * =============================================================================
 */

// Preference key for enabling/disabling bytecode cache generation
static const char kBytecodeCacheGenPref[] = "javascript.options.bytecode_cache.generation";

// Environment variable for generation mode (build-time/first-run cache generation)
static const char kGenerateCachesEnvVar[] = "MOZ_GENERATE_BYTECODE_CACHES";

/**
 * Check if we're in generation mode (controlled cache generation).
 * Generation mode is enabled by:
 * 1. The MOZ_GENERATE_BYTECODE_CACHES environment variable being set
 * 2. OR the javascript.options.bytecode_cache.generation preference being true
 *
 * Generation mode is intended for:
 * - Build-time cache generation
 * - First-run cache generation
 * - Development/debugging
 *
 * In production, generation should be disabled and pre-built caches used.
 */
static bool
IsInGenerationMode()
{
    // Check environment variable first (for build-time generation)
    const char* envVar = PR_GetEnv(kGenerateCachesEnvVar);
    if (envVar && *envVar) {
        return true;
    }
    
    return false;
}

bool
IsBytecodeCacheGenerationEnabled()
{
    // Check generation mode flag first
    if (IsInGenerationMode()) {
        return true;
    }
    
    // Then check preference (default to disabled)
    return Preferences::GetBool(kBytecodeCacheGenPref, false);
}

/**
 * Write the cache header to a file descriptor
 */
static nsresult
WriteHeader(PRFileDesc* aFile,
            const char* aSourceData,
            uint32_t aSourceLength,
            uint32_t aBytecodeLength,
            JSCCacheResult* aResult)
{
    MOZ_JSCHeader header;
    
    // Set magic number
    memcpy(header.magic, MOZ_JSC_MAGIC, MOZ_JSC_MAGIC_SIZE);
    
    // Set versions and lengths (convert to little-endian for portability)
    uint32_t xdrVersion = js::XDR_BYTECODE_VERSION;
    LittleEndian::writeUint32(&header.xdrVersion, xdrVersion);
    LittleEndian::writeUint32(&header.sourceLength, aSourceLength);
    LittleEndian::writeUint32(&header.bytecodeLength, aBytecodeLength);
    
    // Compute source hash
    nsresult rv = ComputeSourceHash(aSourceData, aSourceLength, header.sourceHash);
    if (NS_FAILED(rv)) {
        *aResult = JSCCacheResult::WriteError;
        return rv;
    }
    
    // Write header
    int32_t bytesWritten = PR_Write(aFile, &header, sizeof(MOZ_JSCHeader));
    if (bytesWritten != sizeof(MOZ_JSCHeader)) {
        LOG(("Failed to write cache header"));
        *aResult = JSCCacheResult::WriteError;
        return NS_ERROR_FAILURE;
    }
    
    return NS_OK;
}

nsresult
WriteBytecodeCacheToFile(nsIFile* aJSCFile,
                         const char* aSourceData,
                         uint32_t aSourceLength,
                         JSContext* aCx,
                         JS::HandleScript aScript,
                         JSCCacheResult* aResult)
{
    EnsureLogging();
    
    nsresult rv;
    *aResult = JSCCacheResult::WriteError;
    
    // Encode the script to XDR bytecode
    uint32_t bytecodeLength;
    void* bytecodeData = JS_EncodeScript(aCx, aScript, &bytecodeLength);
    if (!bytecodeData) {
        LOG(("Failed to encode script to bytecode"));
        // JS_EncodeScript may have set a pending exception
        JS_ClearPendingException(aCx);
        *aResult = JSCCacheResult::EncodeError;
        return NS_ERROR_FAILURE;
    }
    
    // Ensure bytecode is freed on all paths
    class BytecodeFree {
    public:
        explicit BytecodeFree(void* data) : mData(data) {}
        ~BytecodeFree() { js_free(mData); }
    private:
        void* mData;
    } bytecodeFree(bytecodeData);
    
    // Create/truncate the output file
    PRFileDesc* fd;
    rv = aJSCFile->OpenNSPRFileDesc(PR_WRONLY | PR_CREATE_FILE | PR_TRUNCATE, 0644, &fd);
    if (NS_FAILED(rv)) {
        LOG(("Failed to create cache file"));
        *aResult = JSCCacheResult::WriteError;
        return rv;
    }
    
    // Ensure file is closed on all paths
    class FileCloser {
    public:
        explicit FileCloser(PRFileDesc* f) : mFile(f) {}
        ~FileCloser() { PR_Close(mFile); }
    private:
        PRFileDesc* mFile;
    } fileCloser(fd);
    
    // Write header
    rv = WriteHeader(fd, aSourceData, aSourceLength, bytecodeLength, aResult);
    if (NS_FAILED(rv)) {
        return rv;
    }
    
    // Write bytecode data
    int32_t bytesWritten = PR_Write(fd, bytecodeData, bytecodeLength);
    if (bytesWritten != (int32_t)bytecodeLength) {
        LOG(("Failed to write bytecode data"));
        *aResult = JSCCacheResult::WriteError;
        return NS_ERROR_FAILURE;
    }
    
    *aResult = JSCCacheResult::Success;
    
    nsAutoCString path;
    aJSCFile->GetNativePath(path);
    LOG(("Successfully wrote bytecode cache: %s (%u bytes)", path.get(), bytecodeLength));
    
    return NS_OK;
}

nsresult
WriteBytecodeCache(nsIFile* aJSFile,
                   const char* aSourceData,
                   uint32_t aSourceLength,
                   JSContext* aCx,
                   JS::HandleScript aScript,
                   JSCCacheResult* aResult)
{
    EnsureLogging();
    
    nsresult rv;
    nsCOMPtr<nsIFile> jscFile;
    
    // First try to write alongside the source file (for development)
    rv = GetBytecodeCachePath(aJSFile, jscFile);
    if (NS_SUCCEEDED(rv)) {
        // Check if the parent directory is writable
        nsCOMPtr<nsIFile> parentDir;
        rv = jscFile->GetParent(getter_AddRefs(parentDir));
        
        if (NS_SUCCEEDED(rv) && IsDirectoryWritable(parentDir)) {
            LOG(("Writing bytecode cache alongside source"));
            return WriteBytecodeCacheToFile(jscFile, aSourceData, aSourceLength,
                                            aCx, aScript, aResult);
        }
    }
    
    // Source directory not writable - use profile directory instead
    LOG(("Source directory not writable, using profile directory for cache"));
    rv = GetProfileBytecodeCachePath(aJSFile, jscFile);
    NS_ENSURE_SUCCESS(rv, rv);
    
    return WriteBytecodeCacheToFile(jscFile, aSourceData, aSourceLength,
                                    aCx, aScript, aResult);
}

nsresult
GenerateBytecodeCacheForDirectory(nsIFile* aSourceDir,
                                  nsIFile* aOutputDir,
                                  bool aRecursive,
                                  JSContext* aCx,
                                  uint32_t* aProcessed,
                                  uint32_t* aFailed)
{
    EnsureLogging();
    
    nsresult rv;
    *aProcessed = 0;
    *aFailed = 0;
    
    // Enumerate files in the directory
    nsCOMPtr<nsISimpleEnumerator> entries;
    rv = aSourceDir->GetDirectoryEntries(getter_AddRefs(entries));
    NS_ENSURE_SUCCESS(rv, rv);
    
    bool hasMore;
    while (NS_SUCCEEDED(entries->HasMoreElements(&hasMore)) && hasMore) {
        nsCOMPtr<nsISupports> item;
        rv = entries->GetNext(getter_AddRefs(item));
        if (NS_FAILED(rv)) continue;
        
        nsCOMPtr<nsIFile> file = do_QueryInterface(item);
        if (!file) continue;
        
        bool isDir;
        rv = file->IsDirectory(&isDir);
        if (NS_FAILED(rv)) continue;
        
        if (isDir) {
            if (aRecursive) {
                // Determine output subdirectory
                nsAutoString leafName;
                file->GetLeafName(leafName);
                
                nsCOMPtr<nsIFile> outputSubDir;
                rv = aOutputDir->Clone(getter_AddRefs(outputSubDir));
                if (NS_SUCCEEDED(rv)) {
                    outputSubDir->Append(leafName);
                    
                    // Create output directory if needed
                    bool exists;
                    outputSubDir->Exists(&exists);
                    if (!exists) {
                        outputSubDir->Create(nsIFile::DIRECTORY_TYPE, 0755);
                    }
                    
                    // Recurse
                    uint32_t subProcessed, subFailed;
                    GenerateBytecodeCacheForDirectory(file, outputSubDir, true,
                                                      aCx, &subProcessed, &subFailed);
                    *aProcessed += subProcessed;
                    *aFailed += subFailed;
                }
            }
            continue;
        }
        
        // Check if this is a .js file
        nsAutoString leafName;
        file->GetLeafName(leafName);
        
        if (!StringEndsWith(leafName, NS_LITERAL_STRING(".js")) &&
            !StringEndsWith(leafName, NS_LITERAL_STRING(".jsm"))) {
            continue;
        }
        
        // Read source file
        int64_t fileSize;
        rv = file->GetFileSize(&fileSize);
        if (NS_FAILED(rv) || fileSize <= 0 || fileSize > UINT32_MAX) {
            (*aFailed)++;
            continue;
        }
        
        PRFileDesc* srcFd;
        rv = file->OpenNSPRFileDesc(PR_RDONLY, 0, &srcFd);
        if (NS_FAILED(rv)) {
            (*aFailed)++;
            continue;
        }
        
        nsAutoArrayPtr<char> sourceData(new char[fileSize + 1]);
        int32_t bytesRead = PR_Read(srcFd, sourceData.get(), fileSize);
        PR_Close(srcFd);
        
        if (bytesRead != fileSize) {
            (*aFailed)++;
            continue;
        }
        sourceData[fileSize] = '\0';
        
        // Compile the script
        nsAutoCString nativePath;
        file->GetNativePath(nativePath);
        
        JS::CompileOptions options(aCx);
        options.setFileAndLine(nativePath.get(), 1);
        options.setNoScriptRval(true);
        options.setVersion(JSVERSION_LATEST);
        
        JS::RootedScript script(aCx);
        if (!JS::Compile(aCx, options, sourceData.get(), fileSize, &script)) {
            LOG(("Failed to compile: %s", nativePath.get()));
            JS_ClearPendingException(aCx);
            (*aFailed)++;
            continue;
        }
        
        // Determine output file path
        nsCOMPtr<nsIFile> outputFile;
        rv = aOutputDir->Clone(getter_AddRefs(outputFile));
        if (NS_FAILED(rv)) {
            (*aFailed)++;
            continue;
        }
        
        // Change extension to .jsc
        nsAutoString jscName(leafName);
        int32_t dotPos = jscName.RFindChar('.');
        if (dotPos != kNotFound) {
            jscName.Truncate(dotPos);
        }
        jscName.AppendLiteral(".jsc");
        outputFile->Append(jscName);
        
        // Write cache file
        JSCCacheResult cacheResult;
        rv = WriteBytecodeCacheToFile(outputFile, sourceData.get(), fileSize,
                                      aCx, script, &cacheResult);
        if (NS_SUCCEEDED(rv)) {
            (*aProcessed)++;
        } else {
            LOG(("Failed to write cache for: %s (%s)", 
                 nativePath.get(), JSCCacheResultToString(cacheResult)));
            (*aFailed)++;
        }
    }
    
    return NS_OK;
}
