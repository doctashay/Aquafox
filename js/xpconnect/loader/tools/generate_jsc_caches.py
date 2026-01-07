#!/usr/bin/env python
# -*- coding: utf-8 -*-
"""
generate_jsc_caches.py - Build-time bytecode cache generation

This script generates .jsc bytecode cache files for JavaScript files
in the browser chrome and toolkit directories during the build/packaging
process.

Usage:
    python generate_jsc_caches.py --objdir <objdir> --srcdir <srcdir> [options]

Options:
    --objdir DIR       Object directory (where built files are)
    --srcdir DIR       Source directory  
    --output DIR       Output directory for .jsc files (default: alongside sources)
    --verbose          Verbose output
    --dry-run          Show what would be done without doing it
    --manifest FILE    Write manifest of generated files
    --tool PATH        Path to jsc_compile tool (auto-detected if not specified)

The script uses the jsc_compile tool (preferred) or JS shell to compile 
scripts and generate XDR-encoded bytecode in the .jsc format.
"""

from __future__ import print_function
import argparse
import hashlib
import os
import struct
import subprocess
import sys
import json

# Constants matching mozJSBytecodeCache.h
MOZ_JSC_MAGIC = b"JSC\x01"
MOZ_JSC_MAGIC_SIZE = 4
MOZ_JSC_HASH_SIZE = 32

# Directories containing chrome/content JS to cache
CHROME_JS_DIRS = [
    'browser/components',
    'browser/base/content',
    'browser/modules',
    'toolkit/components',
    'toolkit/content',
    'toolkit/modules',
    'services',
]

# File patterns to include
JS_EXTENSIONS = ('.js', '.jsm')

# Files/patterns to exclude
EXCLUDE_PATTERNS = [
    '/test/',
    '/tests/', 
    '/Test/',
    '.eslintrc',
    'moz.build',
    '/test_',
    '_test.js',
]


def compute_sha256(data):
    """Compute SHA-256 hash of data."""
    return hashlib.sha256(data).digest()


def find_jsc_compile_tool(objdir):
    """Find the jsc_compile tool in the object directory."""
    possible_paths = [
        os.path.join(objdir, 'dist', 'host', 'bin', 'jsc_compile'),
        os.path.join(objdir, 'dist', 'host', 'bin', 'jsc_compile.exe'),
        os.path.join(objdir, 'js', 'xpconnect', 'loader', 'tools', 'jsc_compile'),
        os.path.join(objdir, 'js', 'xpconnect', 'loader', 'tools', 'jsc_compile.exe'),
    ]
    
    for path in possible_paths:
        if os.path.exists(path):
            return path
    
    return None


def find_js_shell(objdir):
    """Find the JS shell in the object directory."""
    possible_paths = [
        os.path.join(objdir, 'dist', 'bin', 'js'),
        os.path.join(objdir, 'dist', 'bin', 'js.exe'),
        os.path.join(objdir, 'js', 'src', 'shell', 'js'),
        os.path.join(objdir, 'js', 'src', 'shell', 'js.exe'),
    ]
    
    for path in possible_paths:
        if os.path.exists(path):
            return path
    
    return None


def should_exclude(path):
    """Check if a path should be excluded from caching."""
    for pattern in EXCLUDE_PATTERNS:
        if pattern in path:
            return True
    return False


def find_js_files(srcdir, chrome_dirs):
    """Find all JS files to cache."""
    js_files = []
    
    for chrome_dir in chrome_dirs:
        full_dir = os.path.join(srcdir, chrome_dir)
        if not os.path.exists(full_dir):
            continue
        
        for root, dirs, files in os.walk(full_dir):
            # Skip excluded directories
            dirs[:] = [d for d in dirs if not should_exclude(os.path.join(root, d))]
            
            for filename in files:
                if not filename.endswith(JS_EXTENSIONS):
                    continue
                
                filepath = os.path.join(root, filename)
                
                if should_exclude(filepath):
                    continue
                
                js_files.append(filepath)
    
    return js_files


def compile_with_tool(tool_path, source_path, output_path):
    """
    Compile a JS file to bytecode using the jsc_compile tool.
    Returns (success, error_message) tuple.
    """
    try:
        result = subprocess.run(
            [tool_path, source_path, output_path],
            capture_output=True,
            text=True,
            timeout=60
        )
        
        if result.returncode != 0:
            return False, result.stderr.strip() or "Unknown error"
        
        return True, None
        
    except subprocess.TimeoutExpired:
        return False, "Compilation timed out"
    except Exception as e:
        return False, str(e)


def validate_script_with_shell(js_shell, source_path):
    """
    Validate that a script compiles using the JS shell.
    Returns (valid, error_message) tuple.
    """
    try:
        # Create a simple validation script
        escaped_path = source_path.replace('\\', '/').replace('"', '\\"')
        helper = 'try { compile(read("%s")); print("OK"); } catch(e) { print("ERROR:" + e); quit(1); }' % escaped_path
        
        result = subprocess.run(
            [js_shell, '-e', helper],
            capture_output=True,
            text=True,
            timeout=30
        )
        
        if result.returncode != 0 or "ERROR" in result.stdout:
            error = result.stdout.replace("ERROR:", "").strip() or result.stderr.strip()
            return False, error
        
        return True, None
        
    except subprocess.TimeoutExpired:
        return False, "Validation timed out"
    except Exception as e:
        return False, str(e)


def main():
    parser = argparse.ArgumentParser(
        description='Generate .jsc bytecode caches for chrome JS files'
    )
    parser.add_argument('--objdir', required=True, help='Object directory')
    parser.add_argument('--srcdir', required=True, help='Source directory')
    parser.add_argument('--output', help='Output directory (default: alongside sources)')
    parser.add_argument('--verbose', '-v', action='store_true', help='Verbose output')
    parser.add_argument('--dry-run', '-n', action='store_true', help='Dry run')
    parser.add_argument('--manifest', help='Write manifest file')
    parser.add_argument('--tool', help='Path to jsc_compile tool')
    parser.add_argument('--validate-only', action='store_true', 
                        help='Only validate scripts compile (no cache generation)')
    parser.add_argument('--dirs', nargs='+', help='Override default chrome directories')
    
    args = parser.parse_args()
    
    # Find compilation tool
    jsc_tool = args.tool
    if not jsc_tool:
        jsc_tool = find_jsc_compile_tool(args.objdir)
    
    # Find JS shell as fallback for validation
    js_shell = find_js_shell(args.objdir)
    
    if not jsc_tool and not js_shell:
        print("Error: Cannot find jsc_compile tool or JS shell", file=sys.stderr)
        print("Build the browser first, or specify --tool path", file=sys.stderr)
        return 1
    
    if args.verbose:
        if jsc_tool:
            print(f"Using jsc_compile tool: {jsc_tool}")
        if js_shell:
            print(f"Using JS shell: {js_shell}")
    
    # Determine directories to process
    chrome_dirs = args.dirs if args.dirs else CHROME_JS_DIRS
    
    # Find JS files
    js_files = find_js_files(args.srcdir, chrome_dirs)
    if args.verbose:
        print(f"Found {len(js_files)} JS files to process")
    
    # Process files
    processed = 0
    failed = 0
    skipped = 0
    manifest = []
    
    for js_file in js_files:
        rel_path = os.path.relpath(js_file, args.srcdir)
        
        # Determine output path
        if args.output:
            jsc_path = os.path.join(args.output, rel_path)
            jsc_path = os.path.splitext(jsc_path)[0] + '.jsc'
        else:
            jsc_path = os.path.splitext(js_file)[0] + '.jsc'
        
        if args.dry_run:
            print(f"Would generate: {jsc_path}")
            continue
        
        if args.validate_only:
            # Just check if it compiles
            if js_shell:
                valid, error = validate_script_with_shell(js_shell, js_file)
                if not valid:
                    print(f"FAIL: {rel_path}: {error}")
                    failed += 1
                else:
                    if args.verbose:
                        print(f"OK: {rel_path}")
                    processed += 1
            else:
                print(f"SKIP: {rel_path} (no shell for validation)")
                skipped += 1
            continue
        
        # Generate cache file
        if not jsc_tool:
            if args.verbose:
                print(f"SKIP: {rel_path} (no jsc_compile tool)")
            skipped += 1
            continue
        
        # Create output directory
        output_dir = os.path.dirname(jsc_path)
        if not os.path.exists(output_dir):
            try:
                os.makedirs(output_dir, exist_ok=True)
            except Exception as e:
                print(f"Error creating directory {output_dir}: {e}", file=sys.stderr)
                failed += 1
                continue
        
        # Compile to bytecode
        success, error = compile_with_tool(jsc_tool, js_file, jsc_path)
        
        if success:
            processed += 1
            try:
                size = os.path.getsize(jsc_path)
            except:
                size = 0
            manifest.append({
                'source': rel_path,
                'cache': os.path.relpath(jsc_path, args.output or args.srcdir),
                'size': size
            })
            if args.verbose:
                print(f"Generated: {jsc_path} ({size} bytes)")
        else:
            print(f"FAIL: {rel_path}: {error}", file=sys.stderr)
            failed += 1
    
    # Write manifest
    if args.manifest and manifest:
        try:
            with open(args.manifest, 'w') as f:
                json.dump({
                    'files': manifest,
                    'total': len(manifest),
                    'failed': failed
                }, f, indent=2)
            if args.verbose:
                print(f"Wrote manifest: {args.manifest}")
        except Exception as e:
            print(f"Error writing manifest: {e}", file=sys.stderr)
    
    # Summary
    print(f"\nSummary: {processed} generated, {skipped} skipped, {failed} failed")
    
    return 0 if failed == 0 else 1


if __name__ == '__main__':
    sys.exit(main())
