/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*- */
/* vim: set ts=8 sts=4 et sw=4 tw=99: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/**
 * jsc_compile - Standalone tool to generate .jsc bytecode cache files
 *
 * This tool compiles JavaScript source files into pre-compiled bytecode
 * cache files (.jsc) that can be loaded directly by the browser.
 *
 * Usage: jsc_compile <input.js> [output.jsc]
 *
 * If output is not specified, it defaults to input with .jsc extension.
 *
 * Build: This tool should be built as part of the browser build system.
 *        It links against js_static and uses NSS for SHA-256.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include "jsapi.h"
#include "jsfriendapi.h"

// XDR version from SpiderMonkey
#include "vm/Xdr.h"

// For SHA-256 - use a simple implementation to avoid NSS dependency
// This is the same algorithm used in mozJSBytecodeCache.cpp at runtime

// Cache format constants (must match mozJSBytecodeCache.h)
#define MOZ_JSC_MAGIC "JSC\x01"
#define MOZ_JSC_MAGIC_SIZE 4
#define MOZ_JSC_HASH_SIZE 32

#pragma pack(push, 1)
struct JSCHeader {
    char magic[MOZ_JSC_MAGIC_SIZE];
    uint32_t xdrVersion;
    uint32_t sourceLength;
    uint32_t bytecodeLength;
    uint8_t sourceHash[MOZ_JSC_HASH_SIZE];
};
#pragma pack(pop)

// Simple endian conversion for portability (PPC is big-endian)
static inline uint32_t
ToLittleEndian32(uint32_t value)
{
#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
    return ((value & 0xFF000000) >> 24) |
           ((value & 0x00FF0000) >> 8) |
           ((value & 0x0000FF00) << 8) |
           ((value & 0x000000FF) << 24);
#else
    return value;
#endif
}

// Simple SHA-256 implementation (standalone, no external dependencies)
// Based on public domain implementation

static const uint32_t sha256_k[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5,
    0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3,
    0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc,
    0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7,
    0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13,
    0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3,
    0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5,
    0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208,
    0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2
};

#define ROTR(x, n) (((x) >> (n)) | ((x) << (32 - (n))))
#define CH(x, y, z) (((x) & (y)) ^ (~(x) & (z)))
#define MAJ(x, y, z) (((x) & (y)) ^ ((x) & (z)) ^ ((y) & (z)))
#define EP0(x) (ROTR(x, 2) ^ ROTR(x, 13) ^ ROTR(x, 22))
#define EP1(x) (ROTR(x, 6) ^ ROTR(x, 11) ^ ROTR(x, 25))
#define SIG0(x) (ROTR(x, 7) ^ ROTR(x, 18) ^ ((x) >> 3))
#define SIG1(x) (ROTR(x, 17) ^ ROTR(x, 19) ^ ((x) >> 10))

static void
sha256_transform(uint32_t state[8], const uint8_t data[64])
{
    uint32_t a, b, c, d, e, f, g, h, t1, t2, m[64];
    int i;

    for (i = 0; i < 16; i++) {
        m[i] = ((uint32_t)data[i * 4] << 24) |
               ((uint32_t)data[i * 4 + 1] << 16) |
               ((uint32_t)data[i * 4 + 2] << 8) |
               ((uint32_t)data[i * 4 + 3]);
    }
    for (; i < 64; i++) {
        m[i] = SIG1(m[i - 2]) + m[i - 7] + SIG0(m[i - 15]) + m[i - 16];
    }

    a = state[0]; b = state[1]; c = state[2]; d = state[3];
    e = state[4]; f = state[5]; g = state[6]; h = state[7];

    for (i = 0; i < 64; i++) {
        t1 = h + EP1(e) + CH(e, f, g) + sha256_k[i] + m[i];
        t2 = EP0(a) + MAJ(a, b, c);
        h = g; g = f; f = e; e = d + t1;
        d = c; c = b; b = a; a = t1 + t2;
    }

    state[0] += a; state[1] += b; state[2] += c; state[3] += d;
    state[4] += e; state[5] += f; state[6] += g; state[7] += h;
}

static void
ComputeSHA256(const char* data, uint32_t length, uint8_t hashOut[32])
{
    uint32_t state[8] = {
        0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
        0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19
    };
    
    uint8_t block[64];
    uint32_t i, blockIdx = 0;
    uint64_t bitLen = (uint64_t)length * 8;

    // Process full blocks
    for (i = 0; i < length; i++) {
        block[blockIdx++] = data[i];
        if (blockIdx == 64) {
            sha256_transform(state, block);
            blockIdx = 0;
        }
    }

    // Pad message
    block[blockIdx++] = 0x80;
    if (blockIdx > 56) {
        while (blockIdx < 64) block[blockIdx++] = 0;
        sha256_transform(state, block);
        blockIdx = 0;
    }
    while (blockIdx < 56) block[blockIdx++] = 0;

    // Append length in bits (big-endian)
    for (i = 0; i < 8; i++) {
        block[56 + i] = (bitLen >> (56 - i * 8)) & 0xFF;
    }
    sha256_transform(state, block);

    // Output hash (big-endian)
    for (i = 0; i < 8; i++) {
        hashOut[i * 4] = (state[i] >> 24) & 0xFF;
        hashOut[i * 4 + 1] = (state[i] >> 16) & 0xFF;
        hashOut[i * 4 + 2] = (state[i] >> 8) & 0xFF;
        hashOut[i * 4 + 3] = state[i] & 0xFF;
    }
}

static void
ReportError(JSContext* cx, const char* msg, JSErrorReport* report)
{
    fprintf(stderr, "%s:%u: %s\n",
            report->filename ? report->filename : "[no filename]",
            (unsigned)report->lineno,
            msg);
}

static char*
ReadFile(const char* filename, uint32_t* length)
{
    FILE* f = fopen(filename, "rb");
    if (!f) {
        fprintf(stderr, "Error: Cannot open file %s\n", filename);
        return nullptr;
    }

    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);

    if (size < 0 || size > UINT32_MAX) {
        fprintf(stderr, "Error: File too large %s\n", filename);
        fclose(f);
        return nullptr;
    }

    char* buffer = static_cast<char*>(malloc(size + 1));
    if (!buffer) {
        fprintf(stderr, "Error: Out of memory\n");
        fclose(f);
        return nullptr;
    }

    size_t bytesRead = fread(buffer, 1, size, f);
    fclose(f);

    if (bytesRead != (size_t)size) {
        fprintf(stderr, "Error: Failed to read file %s\n", filename);
        free(buffer);
        return nullptr;
    }

    buffer[size] = '\0';
    *length = static_cast<uint32_t>(size);
    return buffer;
}

static bool
WriteJSCFile(const char* filename,
             const char* source, uint32_t sourceLength,
             const void* bytecode, uint32_t bytecodeLength)
{
    FILE* f = fopen(filename, "wb");
    if (!f) {
        fprintf(stderr, "Error: Cannot create file %s\n", filename);
        return false;
    }

    // Build header with little-endian values for portability
    JSCHeader header;
    memcpy(header.magic, MOZ_JSC_MAGIC, MOZ_JSC_MAGIC_SIZE);
    header.xdrVersion = ToLittleEndian32(js::XDR_BYTECODE_VERSION);
    header.sourceLength = ToLittleEndian32(sourceLength);
    header.bytecodeLength = ToLittleEndian32(bytecodeLength);

    // Compute source hash
    ComputeSHA256(source, sourceLength, header.sourceHash);

    // Write header
    if (fwrite(&header, sizeof(header), 1, f) != 1) {
        fprintf(stderr, "Error: Failed to write header\n");
        fclose(f);
        return false;
    }

    // Write bytecode
    if (fwrite(bytecode, bytecodeLength, 1, f) != 1) {
        fprintf(stderr, "Error: Failed to write bytecode\n");
        fclose(f);
        return false;
    }

    fclose(f);
    return true;
}

int main(int argc, char* argv[])
{
    if (argc < 2) {
        fprintf(stderr, "jsc_compile - Generate .jsc bytecode cache files\n");
        fprintf(stderr, "Usage: %s <input.js> [output.jsc]\n", argv[0]);
        fprintf(stderr, "\nIf output is not specified, uses input path with .jsc extension.\n");
        return 1;
    }

    const char* inputFile = argv[1];
    char outputFile[1024];

    if (argc >= 3) {
        strncpy(outputFile, argv[2], sizeof(outputFile) - 1);
        outputFile[sizeof(outputFile) - 1] = '\0';
    } else {
        // Generate output filename by replacing .js/.jsm with .jsc
        strncpy(outputFile, inputFile, sizeof(outputFile) - 5);
        outputFile[sizeof(outputFile) - 5] = '\0';
        
        char* dot = strrchr(outputFile, '.');
        if (dot && (strcmp(dot, ".js") == 0 || strcmp(dot, ".jsm") == 0)) {
            strcpy(dot, ".jsc");
        } else if (dot) {
            strcpy(dot, ".jsc");
        } else {
            strcat(outputFile, ".jsc");
        }
    }

    // Read source file
    uint32_t sourceLength;
    char* source = ReadFile(inputFile, &sourceLength);
    if (!source) {
        return 1;
    }

    // Initialize SpiderMonkey
    if (!JS_Init()) {
        fprintf(stderr, "Error: Failed to initialize JS engine\n");
        free(source);
        return 1;
    }

    int exitCode = 0;

    {
        JSRuntime* rt = JS_NewRuntime(32L * 1024 * 1024);
        if (!rt) {
            fprintf(stderr, "Error: Failed to create JS runtime\n");
            free(source);
            JS_ShutDown();
            return 1;
        }

        JSContext* cx = JS_NewContext(rt, 8192);
        if (!cx) {
            fprintf(stderr, "Error: Failed to create JS context\n");
            JS_DestroyRuntime(rt);
            free(source);
            JS_ShutDown();
            return 1;
        }

        JS_SetErrorReporter(rt, ReportError);

        {
            JSAutoRequest ar(cx);

            // Create global object
            static const JSClass globalClass = {
                "global", JSCLASS_GLOBAL_FLAGS,
                nullptr, nullptr, nullptr, nullptr,
                nullptr, nullptr, nullptr, nullptr,
                nullptr, nullptr, nullptr,
                JS_GlobalObjectTraceHook
            };

            JS::CompartmentOptions options;
            options.setVersion(JSVERSION_LATEST);
            
            JS::RootedObject global(cx, JS_NewGlobalObject(cx, &globalClass, nullptr,
                                                           JS::FireOnNewGlobalHook, options));
            if (!global) {
                fprintf(stderr, "Error: Failed to create global object\n");
                exitCode = 1;
                goto cleanup;
            }

            JSAutoCompartment ac(cx, global);

            if (!JS_InitStandardClasses(cx, global)) {
                fprintf(stderr, "Error: Failed to init standard classes\n");
                exitCode = 1;
                goto cleanup;
            }

            // Compile the script
            JS::CompileOptions compileOpts(cx);
            compileOpts.setFileAndLine(inputFile, 1)
                       .setNoScriptRval(true)
                       .setVersion(JSVERSION_LATEST);

            JS::RootedScript script(cx);
            if (!JS::Compile(cx, compileOpts, source, sourceLength, &script)) {
                fprintf(stderr, "Error: Failed to compile script: %s\n", inputFile);
                exitCode = 1;
                goto cleanup;
            }

            // Encode to XDR bytecode
            uint32_t bytecodeLength;
            void* bytecode = JS_EncodeScript(cx, script, &bytecodeLength);
            if (!bytecode) {
                fprintf(stderr, "Error: Failed to encode script to XDR\n");
                exitCode = 1;
                goto cleanup;
            }

            // Write .jsc file
            if (!WriteJSCFile(outputFile, source, sourceLength, bytecode, bytecodeLength)) {
                exitCode = 1;
            } else {
                printf("Generated: %s (%u bytes bytecode from %u bytes source)\n",
                       outputFile, bytecodeLength, sourceLength);
            }

            js_free(bytecode);
        }

cleanup:
        JS_DestroyContext(cx);
        JS_DestroyRuntime(rt);
    }

    JS_ShutDown();
    free(source);

    return exitCode;
}
