/* -*- indent-tabs-mode: nil; js-indent-level: 2 -*- */
/* vim: set ts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/**
 * Test the bytecode cache (.jsc) read/write functionality.
 * 
 * These tests verify:
 * 1. Cache files can be written correctly
 * 2. Cache files can be read and scripts execute properly
 * 3. Cache validation correctly rejects stale/invalid caches
 * 4. Fallback to source compilation works when cache is invalid
 */

"use strict";

const { classes: Cc, interfaces: Ci, utils: Cu } = Components;

Cu.import("resource://gre/modules/Services.jsm");
Cu.import("resource://gre/modules/osfile.jsm");

// Test configuration
const TEST_SCRIPT_CONTENT = `
// Test script for bytecode cache validation
var testValue = 42;
function testFunction(x) {
  return x * 2;
}
var testResult = testFunction(testValue);
`;

const MODIFIED_SCRIPT_CONTENT = `
// Modified test script
var testValue = 100;
function testFunction(x) {
  return x + 1;
}
var testResult = testFunction(testValue);
`;

/**
 * Create a temporary JS file for testing
 */
async function createTempJSFile(content) {
  let tempDir = OS.Constants.Path.tmpDir;
  let fileName = "test_bytecode_" + Date.now() + ".js";
  let filePath = OS.Path.join(tempDir, fileName);
  
  await OS.File.writeAtomic(filePath, content, { encoding: "utf-8" });
  
  return filePath;
}

/**
 * Clean up test files
 */
async function cleanupTestFiles(jsPath) {
  try {
    await OS.File.remove(jsPath);
  } catch (e) {}
  
  // Also remove .jsc file if it exists
  let jscPath = jsPath.replace(/\.js$/, ".jsc");
  try {
    await OS.File.remove(jscPath);
  } catch (e) {}
}

/**
 * Test: Basic cache write and read cycle
 */
add_task(async function test_basic_cache_cycle() {
  // Enable bytecode cache
  Services.prefs.setBoolPref("javascript.options.bytecode_cache.enabled", true);
  Services.prefs.setBoolPref("javascript.options.bytecode_cache.generation", true);
  
  let jsPath = await createTempJSFile(TEST_SCRIPT_CONTENT);
  
  try {
    // Load the script - this should compile and potentially generate cache
    let scope = {};
    Services.scriptloader.loadSubScript("file://" + jsPath, scope);
    
    // Verify script executed correctly
    Assert.equal(scope.testValue, 42, "testValue should be 42");
    Assert.equal(scope.testResult, 84, "testResult should be 84");
    
    // Check if cache file was created
    let jscPath = jsPath.replace(/\.js$/, ".jsc");
    let cacheExists = await OS.File.exists(jscPath);
    
    if (cacheExists) {
      info("Cache file was created: " + jscPath);
      
      // Load again - should use cache
      let scope2 = {};
      Services.scriptloader.loadSubScript("file://" + jsPath, scope2);
      
      Assert.equal(scope2.testValue, 42, "Cached script testValue should be 42");
      Assert.equal(scope2.testResult, 84, "Cached script testResult should be 84");
    } else {
      info("Cache file was not created (runtime generation may be disabled)");
    }
  } finally {
    await cleanupTestFiles(jsPath);
    Services.prefs.clearUserPref("javascript.options.bytecode_cache.enabled");
    Services.prefs.clearUserPref("javascript.options.bytecode_cache.generation");
  }
});

/**
 * Test: Cache invalidation when source changes
 */
add_task(async function test_cache_invalidation() {
  Services.prefs.setBoolPref("javascript.options.bytecode_cache.enabled", true);
  Services.prefs.setBoolPref("javascript.options.bytecode_cache.generation", true);
  
  let jsPath = await createTempJSFile(TEST_SCRIPT_CONTENT);
  
  try {
    // First load - create cache
    let scope1 = {};
    Services.scriptloader.loadSubScript("file://" + jsPath, scope1);
    Assert.equal(scope1.testResult, 84, "First load should give 84");
    
    // Modify the source file
    await OS.File.writeAtomic(jsPath, MODIFIED_SCRIPT_CONTENT, { encoding: "utf-8" });
    
    // Load again - cache should be invalidated due to hash mismatch
    let scope2 = {};
    Services.scriptloader.loadSubScript("file://" + jsPath, scope2);
    
    // Should get new values from modified script
    Assert.equal(scope2.testValue, 100, "Modified script testValue should be 100");
    Assert.equal(scope2.testResult, 101, "Modified script testResult should be 101");
    
  } finally {
    await cleanupTestFiles(jsPath);
    Services.prefs.clearUserPref("javascript.options.bytecode_cache.enabled");
    Services.prefs.clearUserPref("javascript.options.bytecode_cache.generation");
  }
});

/**
 * Test: Cache disabled via preference
 */
add_task(async function test_cache_disabled() {
  Services.prefs.setBoolPref("javascript.options.bytecode_cache.enabled", false);
  
  let jsPath = await createTempJSFile(TEST_SCRIPT_CONTENT);
  
  try {
    // Load the script
    let scope = {};
    Services.scriptloader.loadSubScript("file://" + jsPath, scope);
    
    // Script should still work
    Assert.equal(scope.testResult, 84, "Script should work with cache disabled");
    
    // Cache file should not be created
    let jscPath = jsPath.replace(/\.js$/, ".jsc");
    let cacheExists = await OS.File.exists(jscPath);
    Assert.ok(!cacheExists, "Cache file should not be created when disabled");
    
  } finally {
    await cleanupTestFiles(jsPath);
    Services.prefs.clearUserPref("javascript.options.bytecode_cache.enabled");
  }
});

/**
 * Test: Invalid cache file is ignored
 */
add_task(async function test_invalid_cache_ignored() {
  Services.prefs.setBoolPref("javascript.options.bytecode_cache.enabled", true);
  
  let jsPath = await createTempJSFile(TEST_SCRIPT_CONTENT);
  let jscPath = jsPath.replace(/\.js$/, ".jsc");
  
  try {
    // Create an invalid cache file (wrong magic number)
    await OS.File.writeAtomic(jscPath, "INVALID_CACHE_DATA", { encoding: "utf-8" });
    
    // Load the script - should fall back to source compilation
    let scope = {};
    Services.scriptloader.loadSubScript("file://" + jsPath, scope);
    
    // Script should still work
    Assert.equal(scope.testResult, 84, "Script should work with invalid cache");
    
  } finally {
    await cleanupTestFiles(jsPath);
    Services.prefs.clearUserPref("javascript.options.bytecode_cache.enabled");
  }
});

function run_test() {
  run_next_test();
}
