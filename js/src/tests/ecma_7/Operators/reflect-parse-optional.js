// |reftest| skip-if(!xulRuntime.shell)
/*
 * Any copyright is dedicated to the Public Domain.
 * http://creativecommons.org/licenses/publicdomain/
 */

var BUGNUMBER = 0;
var summary = "Reflect.parse support for optional chaining and nullish coalescing";

print(BUGNUMBER + ": " + summary);

// Test nullish coalescing in Reflect.parse
var parseTree = Reflect.parse("a ?? b");
assertEq(parseTree.body[0].type, "ExpressionStatement");
assertEq(parseTree.body[0].expression.type, "LogicalExpression");
assertEq(parseTree.body[0].expression.operator, "??");
assertEq(parseTree.body[0].expression.left.name, "a");
assertEq(parseTree.body[0].expression.right.name, "b");

// Test chained nullish coalescing
parseTree = Reflect.parse("a ?? b ?? c");
assertEq(parseTree.body[0].expression.type, "LogicalExpression");
assertEq(parseTree.body[0].expression.operator, "??");

// Test optional property access in Reflect.parse
parseTree = Reflect.parse("a?.b");
assertEq(parseTree.body[0].type, "ExpressionStatement");
assertEq(parseTree.body[0].expression.type, "MemberExpression");
assertEq(parseTree.body[0].expression.optional, true);
assertEq(parseTree.body[0].expression.computed, false);
assertEq(parseTree.body[0].expression.object.name, "a");
assertEq(parseTree.body[0].expression.property.name, "b");

// Test optional element access
parseTree = Reflect.parse("a?.[b]");
assertEq(parseTree.body[0].expression.type, "MemberExpression");
assertEq(parseTree.body[0].expression.optional, true);
assertEq(parseTree.body[0].expression.computed, true);
assertEq(parseTree.body[0].expression.object.name, "a");
assertEq(parseTree.body[0].expression.property.name, "b");

// Test optional call
parseTree = Reflect.parse("a?.()");
assertEq(parseTree.body[0].expression.type, "CallExpression");
assertEq(parseTree.body[0].expression.optional, true);
assertEq(parseTree.body[0].expression.callee.name, "a");

// Test chained optional access
parseTree = Reflect.parse("a?.b?.c");
assertEq(parseTree.body[0].expression.type, "MemberExpression");
assertEq(parseTree.body[0].expression.optional, true);
assertEq(parseTree.body[0].expression.object.type, "MemberExpression");
assertEq(parseTree.body[0].expression.object.optional, true);

if (typeof reportCompare === "function")
    reportCompare(true, true);
