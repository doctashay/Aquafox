#!/usr/bin/env python
# -*- coding: utf-8 -*-
"""
jsc_generator.py - Generate .jsc bytecode cache files from .js source files

This tool compiles JavaScript source files into pre-compiled bytecode cache
files (.jsc) that can be loaded directly by the browser without parsing.

Usage:
    python jsc_generator.py <js-shell-path> <input.js> [output.jsc]

The tool uses the SpiderMonkey JS shell to compile the source and generate
XDR-encoded bytecode, then wraps it in our .jsc cache format with:
- Magic number and version
- SHA-256 hash of source for validation
- XDR bytecode data

Requirements:
    - SpiderMonkey JS shell (js or js.exe)
    - Python 3.6+
"""

import argparse
import hashlib
import os
import struct
import subprocess
import sys
import tempfile

# Constants matching mozJSBytecodeCache.h
MOZ_JSC_MAGIC = b"JSC\x01"
MOZ_JSC_MAGIC_SIZE = 4
MOZ_JSC_HASH_SIZE = 32

def compute_sha256(data):
    """Compute SHA-256 hash of data."""
    return hashlib.sha256(data).digest()

def get_xdr_version(js_shell):
    """
    Get the XDR_BYTECODE_VERSION from the JS shell.
    This runs a small script that outputs the version.
    """
    # The XDR version is compiled into the shell, we need to extract it
    # For now, we'll use a placeholder that matches the engine
    # In practice, this should be obtained from the build
    script = """
// Print XDR version - this is a compile-time constant
// We can't easily get it at runtime, so we use a sentinel
print("XDR_VERSION_QUERY");
"""
    # For now, return the version from Xdr.h
    # XDR_BYTECODE_VERSION = 0xb973c0de - 339
    return 0xb973c0de - 339

def compile_to_xdr(js_shell, source_path, output_path):
    """
    Use the JS shell to compile source to XDR bytecode.
    
    This creates a temporary script that compiles the source file
    and outputs the XDR-encoded bytecode.
    """
    # Read source file
    with open(source_path, 'rb') as f:
        source_data = f.read()
    
    source_text = source_data.decode('utf-8', errors='replace')
    source_length = len(source_data)
    source_hash = compute_sha256(source_data)
    
    # Create a helper script for the JS shell
    # The shell needs to compile and encode the script
    helper_script = f'''
// Helper script to compile and encode bytecode
const source = read("{source_path.replace(os.sep, '/')}");
const options = {{
    fileName: "{os.path.basename(source_path)}",
    lineNumber: 1,
    noScriptRval: true
}};

try {{
    const script = compile(source, options);
    const encoded = encode(script);
    
    // Output as hex for easy parsing
    let hex = "";
    for (let i = 0; i < encoded.length; i++) {{
        hex += encoded[i].toString(16).padStart(2, '0');
    }}
    print("BYTECODE_START");
    print(hex);
    print("BYTECODE_END");
}} catch (e) {{
    print("ERROR: " + e);
    quit(1);
}}
'''
    
    # Write helper script to temp file
    with tempfile.NamedTemporaryFile(mode='w', suffix='.js', delete=False) as f:
        f.write(helper_script)
        helper_path = f.name
    
    try:
        # Run JS shell with helper script
        result = subprocess.run(
            [js_shell, helper_path],
            capture_output=True,
            text=True
        )
        
        if result.returncode != 0:
            print(f"JS shell error: {result.stderr}", file=sys.stderr)
            return False
        
        # Parse output to extract bytecode
        output = result.stdout
        if "BYTECODE_START" not in output or "BYTECODE_END" not in output:
            print(f"Failed to extract bytecode from shell output", file=sys.stderr)
            print(f"Output: {output}", file=sys.stderr)
            return False
        
        start = output.index("BYTECODE_START") + len("BYTECODE_START")
        end = output.index("BYTECODE_END")
        hex_data = output[start:end].strip()
        
        bytecode = bytes.fromhex(hex_data)
        
    finally:
        os.unlink(helper_path)
    
    # Get XDR version
    xdr_version = get_xdr_version(js_shell)
    
    # Build .jsc file
    # Header: magic(4) + xdrVersion(4) + sourceLength(4) + bytecodeLength(4) + hash(32)
    header = struct.pack(
        '<4sIII32s',
        MOZ_JSC_MAGIC,
        xdr_version,
        source_length,
        len(bytecode),
        source_hash
    )
    
    # Write output file
    with open(output_path, 'wb') as f:
        f.write(header)
        f.write(bytecode)
    
    return True

def main():
    parser = argparse.ArgumentParser(
        description='Generate .jsc bytecode cache files from .js source files'
    )
    parser.add_argument('js_shell', help='Path to SpiderMonkey JS shell')
    parser.add_argument('input', help='Input .js source file')
    parser.add_argument('output', nargs='?', help='Output .jsc file (default: input with .jsc extension)')
    parser.add_argument('-v', '--verbose', action='store_true', help='Verbose output')
    
    args = parser.parse_args()
    
    # Validate input
    if not os.path.exists(args.js_shell):
        print(f"Error: JS shell not found: {args.js_shell}", file=sys.stderr)
        return 1
    
    if not os.path.exists(args.input):
        print(f"Error: Input file not found: {args.input}", file=sys.stderr)
        return 1
    
    # Determine output path
    output_path = args.output
    if not output_path:
        base, _ = os.path.splitext(args.input)
        output_path = base + '.jsc'
    
    if args.verbose:
        print(f"Compiling: {args.input}")
        print(f"Output: {output_path}")
    
    # Generate bytecode cache
    success = compile_to_xdr(args.js_shell, args.input, output_path)
    
    if success:
        if args.verbose:
            size = os.path.getsize(output_path)
            print(f"Generated {output_path} ({size} bytes)")
        return 0
    else:
        print(f"Error: Failed to generate bytecode cache", file=sys.stderr)
        return 1

if __name__ == '__main__':
    sys.exit(main())
