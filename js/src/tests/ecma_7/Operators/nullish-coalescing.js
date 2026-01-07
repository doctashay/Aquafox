// |reftest| skip-if(!xulRuntime.shell)
/*
 * Any copyright is dedicated to the Public Domain.
 * http://creativecommons.org/licenses/publicdomain/
 */

var BUGNUMBER = 0;
var summary = "Implement nullish coalescing operator (??)";

print(BUGNUMBER + ": " + summary);

// Basic nullish coalescing tests
assertEq(null ?? 1, 1);
assertEq(undefined ?? 2, 2);
assertEq(0 ?? 3, 0);
assertEq("" ?? 4, "");
assertEq(false ?? 5, false);
assertEq(NaN ?? 6, NaN);

// Right-hand side should not be evaluated if left is not nullish
var evaluated = false;
var result = 1 ?? (evaluated = true, 2);
assertEq(result, 1);
assertEq(evaluated, false);

// Right-hand side should be evaluated if left is null
evaluated = false;
result = null ?? (evaluated = true, 2);
assertEq(result, 2);
assertEq(evaluated, true);

// Right-hand side should be evaluated if left is undefined
evaluated = false;
result = undefined ?? (evaluated = true, 3);
assertEq(result, 3);
assertEq(evaluated, true);

// Chaining nullish coalescing
assertEq(null ?? undefined ?? 1, 1);
assertEq(null ?? 2 ?? 3, 2);
assertEq(1 ?? null ?? 3, 1);

// Cannot mix ?? with || without parentheses
assertThrowsInstanceOf(() => eval("1 || 2 ?? 3"), SyntaxError);
assertThrowsInstanceOf(() => eval("1 ?? 2 || 3"), SyntaxError);
assertThrowsInstanceOf(() => eval("1 && 2 ?? 3"), SyntaxError);
assertThrowsInstanceOf(() => eval("1 ?? 2 && 3"), SyntaxError);

// But can mix with parentheses
assertEq((1 || 2) ?? 3, 1);
assertEq(1 ?? (2 || 3), 1);
assertEq((null || undefined) ?? 1, 1);
assertEq(null ?? (0 || 1), 1);
assertEq((1 && 2) ?? 3, 2);
assertEq(1 ?? (2 && 3), 1);

// Precedence with other operators
assertEq(1 + 2 ?? 3, 3);
assertEq(null ?? 1 + 2, 3);

// Loop to test baseline and ION
for (var i = 0; i < 10000; i++) {
    assertEq(null ?? 1, 1);
    assertEq(undefined ?? 2, 2);
    assertEq(0 ?? 3, 0);
    assertEq("" ?? 4, "");
}

if (typeof reportCompare === "function")
    reportCompare(true, true);
