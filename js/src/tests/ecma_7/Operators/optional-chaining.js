// |reftest| skip-if(!xulRuntime.shell)
/*
 * Any copyright is dedicated to the Public Domain.
 * http://creativecommons.org/licenses/publicdomain/
 */

var BUGNUMBER = 0;
var summary = "Implement optional chaining operator (?.)";

print(BUGNUMBER + ": " + summary);

// Basic optional chaining property access
var obj = { a: 1, b: { c: 2 } };
assertEq(obj?.a, 1);
assertEq(obj?.b?.c, 2);
assertEq(obj?.nonexistent, undefined);

// Optional chaining with null/undefined base
assertEq(null?.a, undefined);
assertEq(undefined?.a, undefined);

// Optional chaining should short-circuit
var evaluated = false;
var result = null?.a?.(evaluated = true);
assertEq(result, undefined);
assertEq(evaluated, false);

// Optional element access
var arr = [1, 2, 3];
assertEq(arr?.[0], 1);
assertEq(arr?.[1], 2);
assertEq(null?.[0], undefined);
assertEq(undefined?.[0], undefined);

// Optional element access with computed property
var key = "a";
assertEq(obj?.[key], 1);
assertEq(null?.[key], undefined);

// Optional call
function fn() { return 42; }
assertEq(fn?.(), 42);

var maybeFunc = null;
assertEq(maybeFunc?.(), undefined);

maybeFunc = undefined;
assertEq(maybeFunc?.(), undefined);

// Optional method call
var objWithMethod = {
    method: function() { return "called"; }
};
assertEq(objWithMethod?.method?.(), "called");
assertEq(null?.method?.(), undefined);

// Chained optional access
var deep = { a: { b: { c: { d: 1 } } } };
assertEq(deep?.a?.b?.c?.d, 1);
assertEq(deep?.a?.x?.c?.d, undefined);

// Optional chaining does not create intermediate objects
var result2 = null?.a?.b?.c;
assertEq(result2, undefined);

// Mixed regular and optional access
assertEq(obj?.b.c, 2);

// Optional chaining with function that returns null
function returnsNull() { return null; }
assertEq(returnsNull()?.a, undefined);

// Disambiguation: ?. followed by digit should be ternary + number
// This is handled at lexer level - ?. followed by digit is ? followed by .digit
assertEq(true ? .5 : 1, 0.5);

// Loop to test baseline and ION
for (var i = 0; i < 10000; i++) {
    assertEq(obj?.a, 1);
    assertEq(null?.a, undefined);
    assertEq(arr?.[0], 1);
    assertEq(null?.[0], undefined);
}

if (typeof reportCompare === "function")
    reportCompare(true, true);
