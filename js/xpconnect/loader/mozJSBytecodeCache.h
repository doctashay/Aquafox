/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*- */
/* vim: set ts=8 sts=4 et sw=4 tw=99: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/**
 * mozJSBytecodeCache - Pre-compiled bytecode file cache for JS scripts
 *
 * This module provides support for loading pre-compiled .jsc bytecode cache
 * files that ship alongside .js source files. The cache format is portable
 * and architecture-independent, using SpiderMonkey's XDR serialization.
 *
 * Cache files are validated by:
 * 1. Magic number verification
 * 2. XDR bytecode version check
 * 3. Source file hash validation (SHA-256)
 * 4. Source length verification
 *
 * If validation fails, the cache is skipped and source is compiled normally.
 */

#ifndef mozJSBytecodeCache_h
#define mozJSBytecodeCache_h

#include "nsString.h"
#include "nsIFile.h"
#include "jsapi.h"

/**
 * JSC file format version. Increment when the cache format changes
 * (independent of XDR bytecode version which is checked separately).
 */
#define MOZ_JSC_FORMAT_VERSION 1

/**
 * Magic bytes at the start of every .jsc file: "JSC\x01" (version 1)
 */
#define MOZ_JSC_MAGIC "JSC\x01"
#define MOZ_JSC_MAGIC_SIZE 4

/**
 * SHA-256 hash size in bytes
 */
#define MOZ_JSC_HASH_SIZE 32

/**
 * Header structure for .jsc cache files.
 * All multi-byte values are stored in little-endian format for portability.
 */
struct MOZ_JSCHeader {
    char magic[MOZ_JSC_MAGIC_SIZE];     // "JSC\x01"
    uint32_t xdrVersion;                 // XDR_BYTECODE_VERSION from SpiderMonkey
    uint32_t sourceLength;               // Original source file length
    uint32_t bytecodeLength;             // Length of XDR bytecode data
    uint8_t sourceHash[MOZ_JSC_HASH_SIZE]; // SHA-256 of source file
};

/**
 * Result codes for bytecode cache operations
 */
enum class JSCCacheResult {
    Success,           // Cache loaded/written successfully
    NotFound,          // Cache file doesn't exist
    InvalidMagic,      // Magic number mismatch
    VersionMismatch,   // XDR version mismatch
    HashMismatch,      // Source hash doesn't match
    LengthMismatch,    // Source length doesn't match
    ReadError,         // I/O error reading cache
    WriteError,        // I/O error writing cache
    EncodeError,       // XDR encode failed
    DecodeError        // XDR decode failed
};

/**
 * Attempt to load a script from a pre-compiled .jsc bytecode cache file.
 *
 * @param aJSFile       The original .js source file
 * @param aCx           JSContext for script creation
 * @param aScriptp      Output parameter for the loaded script
 * @param aResult       Output parameter for detailed result code
 *
 * @return NS_OK if cache was loaded successfully, error code otherwise
 *
 * The function looks for a .jsc file alongside the .js file (same path
 * with .jsc extension). If found and valid, the bytecode is decoded
 * and returned. If not found or invalid, returns an error and the
 * caller should fall back to source compilation.
 */
nsresult
ReadBytecodeCache(nsIFile* aJSFile,
                  JSContext* aCx,
                  JS::MutableHandleScript aScriptp,
                  JSCCacheResult* aResult);

/**
 * Attempt to load a script from a pre-compiled .jsc bytecode cache file,
 * using a URI to locate the cache.
 *
 * @param aURI          The URI of the original .js source
 * @param aSourceData   The source code data (for hash validation)
 * @param aSourceLength Length of the source code
 * @param aCx           JSContext for script creation
 * @param aScriptp      Output parameter for the loaded script
 * @param aResult       Output parameter for detailed result code
 *
 * @return NS_OK if cache was loaded successfully, error code otherwise
 */
nsresult
ReadBytecodeCacheFromURI(nsIURI* aURI,
                         const char* aSourceData,
                         uint32_t aSourceLength,
                         JSContext* aCx,
                         JS::MutableHandleScript aScriptp,
                         JSCCacheResult* aResult);

/**
 * Compute the SHA-256 hash of source data.
 *
 * @param aData         Source data to hash
 * @param aLength       Length of source data
 * @param aHashOut      Output buffer for 32-byte hash (must be MOZ_JSC_HASH_SIZE)
 *
 * @return NS_OK on success
 */
nsresult
ComputeSourceHash(const char* aData, uint32_t aLength, uint8_t* aHashOut);

/**
 * Get the path for the .jsc cache file corresponding to a .js file.
 * This returns the path alongside the source file (for pre-shipped caches).
 *
 * @param aJSFile       The original .js file
 * @param aJSCFile      Output: the corresponding .jsc file
 *
 * @return NS_OK on success
 */
nsresult
GetBytecodeCachePath(nsIFile* aJSFile, nsCOMPtr<nsIFile>& aJSCFile);

/**
 * Get the path for the .jsc cache file in the user's profile directory.
 * This is used for runtime-generated caches when the source directory
 * is not writable.
 *
 * @param aJSFile       The original .js file (used to generate unique filename)
 * @param aJSCFile      Output: the cache file in profile directory
 *
 * @return NS_OK on success
 */
nsresult
GetProfileBytecodeCachePath(nsIFile* aJSFile, nsCOMPtr<nsIFile>& aJSCFile);

/**
 * Get the path for the .jsc cache file, checking profile directory first
 * then falling back to alongside source.
 *
 * @param aJSFile       The original .js file
 * @param aJSCFile      Output: the cache file (profile or alongside source)
 * @param aFoundInProfile Output: true if cache was found in profile dir
 *
 * @return NS_OK on success (even if file doesn't exist)
 */
nsresult
FindBytecodeCachePath(nsIFile* aJSFile, nsCOMPtr<nsIFile>& aJSCFile, bool* aFoundInProfile);

/**
 * Check if a directory is writable.
 *
 * @param aDir          The directory to check
 *
 * @return true if the directory is writable
 */
bool
IsDirectoryWritable(nsIFile* aDir);

/**
 * Check if bytecode caching is enabled via preferences.
 *
 * @return true if bytecode cache loading is enabled
 */
bool
IsBytecodeCacheEnabled();

/**
 * Convert a JSCCacheResult to a human-readable string for logging.
 */
const char*
JSCCacheResultToString(JSCCacheResult aResult);

/*
 * =============================================================================
 * CACHE WRITER API (Phase 2)
 * =============================================================================
 */

/**
 * Write a compiled script to a .jsc bytecode cache file.
 *
 * @param aJSFile       The original .js source file (used to determine output path)
 * @param aSourceData   The original source code (for hash computation)
 * @param aSourceLength Length of the source code
 * @param aCx           JSContext for script encoding
 * @param aScript       The compiled script to cache
 * @param aResult       Output parameter for detailed result code
 *
 * @return NS_OK if cache was written successfully, error code otherwise
 *
 * The cache file is written alongside the source file with .jsc extension.
 * If a cache file already exists, it will be overwritten.
 */
nsresult
WriteBytecodeCache(nsIFile* aJSFile,
                   const char* aSourceData,
                   uint32_t aSourceLength,
                   JSContext* aCx,
                   JS::HandleScript aScript,
                   JSCCacheResult* aResult);

/**
 * Write a compiled script to a specified .jsc file path.
 *
 * @param aJSCFile      The output .jsc file path
 * @param aSourceData   The original source code (for hash computation)
 * @param aSourceLength Length of the source code
 * @param aCx           JSContext for script encoding
 * @param aScript       The compiled script to cache
 * @param aResult       Output parameter for detailed result code
 *
 * @return NS_OK if cache was written successfully, error code otherwise
 */
nsresult
WriteBytecodeCacheToFile(nsIFile* aJSCFile,
                         const char* aSourceData,
                         uint32_t aSourceLength,
                         JSContext* aCx,
                         JS::HandleScript aScript,
                         JSCCacheResult* aResult);

/**
 * Check if bytecode cache generation is enabled.
 * This controls whether new caches are written at runtime.
 *
 * Generation is enabled if:
 * 1. The MOZ_GENERATE_BYTECODE_CACHES environment variable is set
 * 2. OR the javascript.options.bytecode_cache.generation preference is true
 *
 * @return true if bytecode cache generation is enabled
 */
bool
IsBytecodeCacheGenerationEnabled();

/**
 * Environment variable name for generation mode.
 * When set, enables bytecode cache generation regardless of preference.
 * Intended for build-time cache generation and first-run scenarios.
 */
#define MOZ_GENERATE_BYTECODE_CACHES_ENV "MOZ_GENERATE_BYTECODE_CACHES"

/**
 * Generate bytecode cache files for all JS files in a directory.
 * This is intended for build-time cache generation.
 *
 * @param aSourceDir    Directory containing .js files
 * @param aOutputDir    Directory for output .jsc files (can be same as source)
 * @param aRecursive    Whether to process subdirectories
 * @param aCx           JSContext for compilation
 * @param aProcessed    Output: number of files processed
 * @param aFailed       Output: number of files that failed
 *
 * @return NS_OK if generation completed (even if some files failed)
 */
nsresult
GenerateBytecodeCacheForDirectory(nsIFile* aSourceDir,
                                  nsIFile* aOutputDir,
                                  bool aRecursive,
                                  JSContext* aCx,
                                  uint32_t* aProcessed,
                                  uint32_t* aFailed);

#endif /* mozJSBytecodeCache_h */
