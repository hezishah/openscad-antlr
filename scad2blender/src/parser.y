/**
 * @file parser.y
 * @brief Bison parser for OpenSCAD
 *
 * Based on OpenSCAD's parser, simplified for scad2blender.
 */

%{
#include <iostream>
#include <string>
#include <vector>
#include <memory>
#include <map>
#include <set>
#include <stack>
#include <cmath>
#include <algorithm>
#include <fstream>
#include <sstream>
#include <libgen.h>
#include "ast.h"
#include "value.h"

using namespace scad2blender;

extern int yylex();
extern int yylineno;
extern char* yytext;
void yyerror(const char* s);

// Global root node
ASTNodePtr g_root;

// Temporary storage for module parameter defaults (saved before body parsing)
static std::map<std::string, Value> g_module_param_defaults;
// Track which parameters had explicit `= expr` in their definition
static std::set<std::string> g_params_with_explicit_default;

// Symbol table for variable lookup during parsing
static std::stack<std::map<std::string, Value>> g_symbol_stack;
static std::map<std::string, Value>& current_scope() {
    if (g_symbol_stack.empty()) {
        g_symbol_stack.push(std::map<std::string, Value>());
    }
    return g_symbol_stack.top();
}

static void push_scope() {
    if (g_symbol_stack.empty()) {
        g_symbol_stack.push(std::map<std::string, Value>());
    } else {
        // Copy parent scope
        g_symbol_stack.push(g_symbol_stack.top());
    }
}

static void pop_scope() {
    if (!g_symbol_stack.empty()) {
        g_symbol_stack.pop();
    }
}

static Value lookup_variable(const std::string& name) {
    if (g_symbol_stack.empty()) {
        return Value();  // undefined
    }
    auto& scope = g_symbol_stack.top();
    auto it = scope.find(name);
    if (it != scope.end()) {
        return it->second;
    }
    return Value();  // undefined
}

static void set_variable(const std::string& name, const Value& val) {
    current_scope()[name] = val;
}

// ---- Function table and evaluation engine ----

struct FunctionDef {
    std::vector<std::string> params;
    ExprNodePtr body;
    std::map<std::string, ExprNodePtr> defaults;  // param name → default expr tree
};
static std::map<std::string, FunctionDef> g_function_table;

// Recursion depth guard
static int g_eval_depth = 0;
static const int MAX_EVAL_DEPTH = 100;

// Total evaluation step counter to prevent runaway eager evaluation
static int g_eval_steps = 0;
static const int MAX_EVAL_STEPS = 200;

// Flag: when true, parameter_list stores VarRef expressions for params
// so that function bodies build expression trees instead of evaluating
static bool g_in_function_def = false;
static std::vector<bool> g_in_function_def_stack;  // save/restore stack for nesting
static bool g_in_module_def = false;  // true inside module body — prevents eager for-loop eval
static std::vector<bool> g_in_module_def_stack;

// Track module-local variables assigned from bare literals (e.g. height = 20).
// When referenced later in the same module body, these produce VarRef expression
// trees so that the code generator can link them to group_input sockets.
static std::set<std::string> g_module_literal_vars;
static std::vector<std::set<std::string>> g_module_literal_vars_stack;

// Track top-level (global) variables. When referenced inside module bodies,
// these produce VarRef expression trees so the code generator can link them
// to group_input sockets instead of hardcoding values.
static std::set<std::string> g_top_level_vars;

// Ordered let-binding capture: argument_list appends entries here
// so that the let() rule can reconstruct insertion order
static std::vector<std::pair<std::string, Value>> g_ordered_args;
static std::vector<std::vector<std::pair<std::string, Value>>> g_ordered_args_stack;

// Forward declarations
static Value evaluate_expr_tree(const ExprNodePtr& tree, const std::map<std::string, Value>& bindings);
static Value evaluate_function_call(const std::string& name, const std::vector<Value>& arg_vals,
                                     const std::map<std::string, Value>& bindings);

// Helper: resolve Expression values to concrete types by evaluating their expression trees.
// Returns the original value if no resolution possible.
static Value resolve_expression_value(const Value& val, const std::map<std::string, Value>& bindings) {
    if (val.isExpression() && val.exprTree() && !val.exprTree()->hasVariableRefs()) {
        std::map<std::string, Value> empty;
        Value resolved = evaluate_expr_tree(val.exprTree(), empty);
        if (!resolved.isUndefined() && !resolved.isExpression()) return resolved;
    }
    if (val.isExpression() && val.exprTree()) {
        Value resolved = evaluate_expr_tree(val.exprTree(), bindings);
        if (!resolved.isUndefined() && !resolved.isExpression()) return resolved;
    }
    if (val.isVector()) {
        bool has_expr = false;
        for (size_t i = 0; i < val.size(); i++) {
            if (val[i].isExpression()) { has_expr = true; break; }
        }
        if (has_expr) {
            Vector resolved_vec;
            for (size_t i = 0; i < val.size(); i++) {
                resolved_vec.push_back(resolve_expression_value(val[i], bindings));
            }
            return Value(resolved_vec);
        }
    }
    return val;
}

static Value evaluate_expr_tree(const ExprNodePtr& tree, const std::map<std::string, Value>& bindings) {
    if (!tree) return Value();
    if (++g_eval_depth > MAX_EVAL_DEPTH) { --g_eval_depth; return Value(); }
    if (++g_eval_steps > MAX_EVAL_STEPS) { --g_eval_depth; return Value(); }

    Value result;

    switch (tree->kind) {
        case ExprNode::Kind::Literal:
            result = Value(tree->literal_value);
            break;

        case ExprNode::Kind::VarRef: {
            // Check bindings first, then symbol table, then built-in constants
            auto it = bindings.find(tree->var_name);
            if (it != bindings.end()) { result = it->second; break; }
            Value sv = lookup_variable(tree->var_name);
            if (!sv.isUndefined()) { result = sv; break; }
            // Built-in constants
            if (tree->var_name == "PI") { result = Value(M_PI); break; }
            if (tree->var_name == "true") { result = Value(true); break; }
            if (tree->var_name == "false") { result = Value(false); break; }
            // Handle var.x / var.y / var.z member access
            {
                const std::string& vn = tree->var_name;
                if (vn.size() > 2 && vn[vn.size()-2] == '.') {
                    char member = vn.back();
                    int idx = -1;
                    if (member == 'x') idx = 0;
                    else if (member == 'y') idx = 1;
                    else if (member == 'z') idx = 2;
                    if (idx >= 0) {
                        std::string baseName = vn.substr(0, vn.size() - 2);
                        auto bit = bindings.find(baseName);
                        if (bit == bindings.end()) {
                            Value bsv = lookup_variable(baseName);
                            if (!bsv.isUndefined()) {
                                if (bsv.isVector() && static_cast<size_t>(idx) < bsv.size()) {
                                    result = bsv[idx]; break;
                                } else if (bsv.isNumber() && idx == 0) {
                                    result = bsv; break;
                                }
                            }
                        } else {
                            const Value& bv = bit->second;
                            if (bv.isVector() && static_cast<size_t>(idx) < bv.size()) {
                                result = bv[idx]; break;
                            } else if (bv.isNumber() && idx == 0) {
                                result = bv; break;
                            }
                        }
                    }
                }
            }
            // If it's still an expression with a tree, try to evaluate that
            result = Value();
            break;
        }

        case ExprNode::Kind::UnaryOp: {
            Value operand = evaluate_expr_tree(tree->left, bindings);
            if (tree->op == ExprNode::Op::NEGATE) {
                if (operand.isNumber()) { result = Value(-operand.toNumber()); break; }
                if (operand.isVector()) {
                    Vector v;
                    for (size_t i = 0; i < operand.size(); i++) {
                        if (operand[i].isNumber())
                            v.push_back(Value(-operand[i].toNumber()));
                        else
                            v.push_back(operand[i]);
                    }
                    result = Value(v);
                    break;
                }
            }
            if (tree->op == ExprNode::Op::NOT) {
                if (operand.isBool()) { result = Value(!operand.toBool()); break; }
                if (operand.isNumber()) { result = Value(operand.toNumber() == 0.0); break; }
                result = Value(true); // !undef = true
                break;
            }
            result = operand;
            break;
        }

        case ExprNode::Kind::BinaryOp: {
            Value left = evaluate_expr_tree(tree->left, bindings);
            Value right = evaluate_expr_tree(tree->right, bindings);

            // Comparison/logic operators
            switch (tree->op) {
                case ExprNode::Op::LESS:
                    if (left.isNumber() && right.isNumber())
                        result = Value(left.toNumber() < right.toNumber());
                    else result = Value();
                    break;
                case ExprNode::Op::GREATER:
                    if (left.isNumber() && right.isNumber())
                        result = Value(left.toNumber() > right.toNumber());
                    else result = Value();
                    break;
                case ExprNode::Op::LESS_EQ:
                    if (left.isNumber() && right.isNumber())
                        result = Value(left.toNumber() <= right.toNumber());
                    else result = Value();
                    break;
                case ExprNode::Op::GREATER_EQ:
                    if (left.isNumber() && right.isNumber())
                        result = Value(left.toNumber() >= right.toNumber());
                    else result = Value();
                    break;
                case ExprNode::Op::EQUAL:
                    if (left.isNumber() && right.isNumber())
                        result = Value(left.toNumber() == right.toNumber());
                    else if (left.isBool() && right.isBool())
                        result = Value(left.toBool() == right.toBool());
                    else if (left.isString() && right.isString())
                        result = Value(left.toString() == right.toString());
                    else if (left.isUndefined() && right.isUndefined())
                        result = Value(true);
                    else
                        result = Value(false);
                    break;
                case ExprNode::Op::NOT_EQUAL:
                    if (left.isNumber() && right.isNumber())
                        result = Value(left.toNumber() != right.toNumber());
                    else if (left.isBool() && right.isBool())
                        result = Value(left.toBool() != right.toBool());
                    else if (left.isString() && right.isString())
                        result = Value(left.toString() != right.toString());
                    else if (left.isUndefined() && right.isUndefined())
                        result = Value(false);
                    else
                        result = Value(true);
                    break;
                case ExprNode::Op::AND: {
                    bool lb = left.isBool() ? left.toBool() : (left.isNumber() ? left.toNumber() != 0 : false);
                    bool rb = right.isBool() ? right.toBool() : (right.isNumber() ? right.toNumber() != 0 : false);
                    result = Value(lb && rb);
                    break;
                }
                case ExprNode::Op::OR: {
                    bool lb = left.isBool() ? left.toBool() : (left.isNumber() ? left.toNumber() != 0 : false);
                    bool rb = right.isBool() ? right.toBool() : (right.isNumber() ? right.toNumber() != 0 : false);
                    result = Value(lb || rb);
                    break;
                }
                default:
                    break;
            }
            if (!result.isUndefined() || tree->op == ExprNode::Op::LESS ||
                tree->op == ExprNode::Op::GREATER || tree->op == ExprNode::Op::LESS_EQ ||
                tree->op == ExprNode::Op::GREATER_EQ || tree->op == ExprNode::Op::EQUAL ||
                tree->op == ExprNode::Op::NOT_EQUAL || tree->op == ExprNode::Op::AND ||
                tree->op == ExprNode::Op::OR) {
                break;
            }

            // Number x Number
            if (left.isNumber() && right.isNumber()) {
                double l = left.toNumber(), r = right.toNumber();
                switch (tree->op) {
                    case ExprNode::Op::ADD: result = Value(l + r); break;
                    case ExprNode::Op::SUBTRACT: result = Value(l - r); break;
                    case ExprNode::Op::MULTIPLY: result = Value(l * r); break;
                    case ExprNode::Op::DIVIDE: result = r != 0 ? Value(l / r) : Value(); break;
                    case ExprNode::Op::MODULO: result = r != 0 ? Value(std::fmod(l, r)) : Value(); break;
                    case ExprNode::Op::POWER: result = Value(std::pow(l, r)); break;
                    default: result = Value(); break;
                }
                break;
            }
            // Scalar * Vector or Vector * Scalar
            if (tree->op == ExprNode::Op::MULTIPLY) {
                Value scalar, vec;
                ExprNodePtr scalarTree;
                if (left.isNumber() && right.isVector()) {
                    scalar = left; vec = right;
                    scalarTree = tree->left;
                } else if (left.isVector() && right.isNumber()) {
                    scalar = right; vec = left;
                    scalarTree = tree->right;
                }
                if (vec.isVector() && scalar.isNumber()) {
                    double s = scalar.toNumber();
                    Vector vr;
                    for (size_t i = 0; i < vec.size(); i++) {
                        if (vec[i].isNumber()) {
                            Value elem(vec[i].toNumber() * s);
                            // Preserve expression tree: scalar * element
                            ExprNodePtr elemTree = vec[i].exprTree();
                            if (elemTree && elemTree->hasVariableRefs()) {
                                ExprNodePtr sTree = scalarTree ? scalarTree : ExprNode::makeLiteral(s);
                                auto prodTree = ExprNode::makeBinary(ExprNode::Op::MULTIPLY, sTree, elemTree);
                                elem.setExprTree(prodTree);
                            } else if (scalarTree && scalarTree->hasVariableRefs()) {
                                ExprNodePtr eTree = elemTree ? elemTree : ExprNode::makeLiteral(vec[i].toNumber());
                                auto prodTree = ExprNode::makeBinary(ExprNode::Op::MULTIPLY, scalarTree, eTree);
                                elem.setExprTree(prodTree);
                            }
                            vr.push_back(elem);
                        } else {
                            vr.push_back(vec[i]);
                        }
                    }
                    result = Value(vr);
                    break;
                }
            }
            // Vector +/- Vector
            if ((tree->op == ExprNode::Op::ADD || tree->op == ExprNode::Op::SUBTRACT) &&
                left.isVector() && right.isVector()) {
                Vector vr;
                size_t len = std::min(left.size(), right.size());
                for (size_t i = 0; i < len; i++) {
                    if (left[i].isNumber() && right[i].isNumber()) {
                        double v = (tree->op == ExprNode::Op::ADD)
                            ? left[i].toNumber() + right[i].toNumber()
                            : left[i].toNumber() - right[i].toNumber();
                        vr.push_back(Value(v));
                    } else {
                        vr.push_back(Value());
                    }
                }
                result = Value(vr);
                break;
            }
            result = Value();
            break;
        }

        case ExprNode::Kind::FunctionCall: {
            // Build arg values, handling named arguments via arg_names
            std::vector<Value> arg_vals;
            std::vector<std::string> names;
            for (size_t _ai = 0; _ai < tree->func_args.size(); _ai++) {
                arg_vals.push_back(evaluate_expr_tree(tree->func_args[_ai], bindings));
            }
            if (!tree->arg_names.empty()) {
                names = tree->arg_names;
            }
            if (!names.empty()) {
                // Named args: look up function def, map to positional
                auto fit = g_function_table.find(tree->func_name);
                if (fit != g_function_table.end()) {
                    const FunctionDef& fdef = fit->second;
                    std::vector<Value> positioned(fdef.params.size());
                    // First fill from defaults
                    for (size_t i = 0; i < fdef.params.size(); i++) {
                        auto dit = fdef.defaults.find(fdef.params[i]);
                        if (dit != fdef.defaults.end()) {
                            positioned[i] = evaluate_expr_tree(dit->second, bindings);
                        }
                    }
                    // Then apply provided args
                    for (size_t i = 0; i < arg_vals.size() && i < names.size(); i++) {
                        if (names[i].empty()) {
                            // Positional
                            if (i < positioned.size()) positioned[i] = arg_vals[i];
                        } else {
                            // Named — find position
                            for (size_t j = 0; j < fdef.params.size(); j++) {
                                if (fdef.params[j] == names[i]) {
                                    positioned[j] = arg_vals[i];
                                    break;
                                }
                            }
                        }
                    }
                    result = evaluate_function_call(tree->func_name, positioned, bindings);
                    break;
                }
            }
            result = evaluate_function_call(tree->func_name, arg_vals, bindings);
            break;
        }

        case ExprNode::Kind::VectorLiteral: {
            Vector v;
            for (const auto& elem : tree->vec_elements) {
                Value ev = evaluate_expr_tree(elem, bindings);
                // If element evaluates to a vector and was a ForLoop or each, flatten it
                if (elem && (elem->kind == ExprNode::Kind::ForLoop ||
                             elem->kind == ExprNode::Kind::Conditional)) {
                    if (ev.isVector()) {
                        for (size_t i = 0; i < ev.size(); i++) {
                            v.push_back(ev[i]);
                        }
                        continue;
                    }
                }
                v.push_back(ev);
            }
            result = Value(v);
            break;
        }

        case ExprNode::Kind::Conditional: {
            Value cond = evaluate_expr_tree(tree->left, bindings);
            bool cond_true = false;
            if (cond.isBool()) cond_true = cond.toBool();
            else if (cond.isNumber()) cond_true = cond.toNumber() != 0.0;
            if (cond_true) {
                result = evaluate_expr_tree(tree->right, bindings);
            } else if (tree->else_branch) {
                result = evaluate_expr_tree(tree->else_branch, bindings);
            } else {
                result = Value(); // no else → undef
            }
            break;
        }

        case ExprNode::Kind::ForLoop: {
            // Evaluate range
            Value range_val = evaluate_expr_tree(tree->left, bindings);
            Vector results;
            if (range_val.isRange()) {
                double start = range_val.rangeStart();
                double end = range_val.rangeEnd();
                double step = range_val.rangeStep();
                if (step == 0) step = 1;
                if ((step > 0 && start <= end) || (step < 0 && start >= end)) {
                    for (double i = start; (step > 0) ? (i <= end + 1e-10) : (i >= end - 1e-10); i += step) {
                        std::map<std::string, Value> new_bindings = bindings;
                        new_bindings[tree->var_name] = Value(i);
                        Value body_val = evaluate_expr_tree(tree->right, new_bindings);
                        if (!body_val.isUndefined()) {
                            results.push_back(body_val);
                        }
                    }
                }
            } else if (range_val.isVector()) {
                for (size_t i = 0; i < range_val.size(); i++) {
                    std::map<std::string, Value> new_bindings = bindings;
                    new_bindings[tree->var_name] = range_val[i];
                    Value body_val = evaluate_expr_tree(tree->right, new_bindings);
                    if (!body_val.isUndefined()) {
                        results.push_back(body_val);
                    }
                }
            }
            result = Value(results);
            break;
        }

        case ExprNode::Kind::LetBinding: {
            std::map<std::string, Value> new_bindings = bindings;
            for (const auto& binding : tree->let_bindings) {
                Value bval = evaluate_expr_tree(binding.second, new_bindings);
                // Resolve Expression values to concrete types
                bval = resolve_expression_value(bval, new_bindings);
                new_bindings[binding.first] = bval;
            }
            result = evaluate_expr_tree(tree->right, new_bindings);
            break;
        }
    }
    --g_eval_depth;
    return result;
}

static Value evaluate_function_call(const std::string& name, const std::vector<Value>& arg_vals,
                                     const std::map<std::string, Value>& bindings) {
    if (g_eval_depth > MAX_EVAL_DEPTH) return Value();

    // Pseudo-function: __index__(vector, index) — array access
    if (name == "__index__" && arg_vals.size() == 2) {
        if (arg_vals[0].isVector() && arg_vals[1].isNumber()) {
            size_t idx = static_cast<size_t>(arg_vals[1].toNumber());
            if (idx < arg_vals[0].size()) {
                return arg_vals[0][idx];
            }
        }
        return Value();
    }

    // Pseudo-function: __range__(start, end, step) — build Range value
    if (name == "__range__" && arg_vals.size() == 3) {
        if (arg_vals[0].isNumber() && arg_vals[1].isNumber() && arg_vals[2].isNumber()) {
            return Value::range(arg_vals[0].toNumber(), arg_vals[1].toNumber(), arg_vals[2].toNumber());
        }
        return Value();
    }

    // Special case: norm(vector)
    if (name == "norm" && arg_vals.size() == 1 && arg_vals[0].isVector()) {
        double sum = 0;
        for (size_t i = 0; i < arg_vals[0].size(); i++) {
            if (arg_vals[0][i].isNumber()) {
                double v = arg_vals[0][i].toNumber();
                sum += v * v;
            }
        }
        return Value(std::sqrt(sum));
    }
    // Special case: len(vector) or len(string)
    if (name == "len" && arg_vals.size() == 1) {
        if (arg_vals[0].isVector()) return Value(static_cast<double>(arg_vals[0].size()));
        if (arg_vals[0].isString()) return Value(static_cast<double>(arg_vals[0].toString().size()));
    }
    // Special case: concat(...)
    if (name == "concat") {
        Vector result;
        for (const auto& a : arg_vals) {
            if (a.isVector()) {
                for (size_t i = 0; i < a.size(); i++) result.push_back(a[i]);
            } else {
                result.push_back(a);
            }
        }
        return Value(result);
    }
    // Special case: rands(min_val, max_val, num_results [, seed])
    if (name == "rands" && arg_vals.size() >= 3) {
        double min_val = arg_vals[0].toNumber();
        double max_val = arg_vals[1].toNumber();
        int num = static_cast<int>(arg_vals[2].toNumber());
        if (num <= 0) return Value(Vector());
        // Use seed if provided, otherwise use a fixed seed for reproducibility
        unsigned int seed = 42;
        if (arg_vals.size() >= 4 && arg_vals[3].isNumber()) {
            seed = static_cast<unsigned int>(arg_vals[3].toNumber());
        }
        // Simple seeded PRNG (linear congruential)
        auto lcg = [](unsigned int& s) -> double {
            s = s * 1103515245u + 12345u;
            return static_cast<double>(s & 0x7fffffff) / static_cast<double>(0x7fffffff);
        };
        Vector result;
        unsigned int state = seed;
        for (int i = 0; i < num; i++) {
            double r = lcg(state);
            result.push_back(Value(min_val + r * (max_val - min_val)));
        }
        return Value(result);
    }
    // Special case: cross(v1, v2) - 3D cross product
    if (name == "cross" && arg_vals.size() == 2 &&
        arg_vals[0].isVector() && arg_vals[1].isVector() &&
        arg_vals[0].size() >= 3 && arg_vals[1].size() >= 3) {
        double ax = arg_vals[0][0].toNumber(), ay = arg_vals[0][1].toNumber(), az = arg_vals[0][2].toNumber();
        double bx = arg_vals[1][0].toNumber(), by = arg_vals[1][1].toNumber(), bz = arg_vals[1][2].toNumber();
        return Value(Vector{Value(ay*bz - az*by), Value(az*bx - ax*bz), Value(ax*by - ay*bx)});
    }

    // Type-checking functions
    if (name == "is_num" && arg_vals.size() == 1)
        return Value(arg_vals[0].isNumber());
    if (name == "is_list" && arg_vals.size() == 1)
        return Value(arg_vals[0].isVector());
    if (name == "is_string" && arg_vals.size() == 1)
        return Value(arg_vals[0].isString());
    if (name == "is_bool" && arg_vals.size() == 1)
        return Value(arg_vals[0].isBool());
    if (name == "is_undef" && arg_vals.size() == 1)
        return Value(arg_vals[0].isUndefined());

    // str() function
    if (name == "str") {
        std::string result;
        for (const auto& a : arg_vals) {
            if (a.isString()) {
                result += a.toString();
            } else {
                result += a.repr();
            }
        }
        return Value(result);
    }

    // Built-in math functions
    if (isBuiltinFunction(name)) {
        std::vector<double> nums;
        bool allNumbers = true;
        for (const auto& a : arg_vals) {
            if (a.isNumber()) nums.push_back(a.toNumber());
            else { allNumbers = false; break; }
        }
        if (allNumbers && !nums.empty()) {
            return Value(evaluateBuiltinMath(name, nums));
        }
    }

    // User-defined function
    auto fit = g_function_table.find(name);
    if (fit != g_function_table.end()) {
        const FunctionDef& fdef = fit->second;
        std::map<std::string, Value> new_bindings = bindings;
        for (size_t i = 0; i < fdef.params.size(); i++) {
            if (i < arg_vals.size() && !arg_vals[i].isUndefined()) {
                new_bindings[fdef.params[i]] = resolve_expression_value(arg_vals[i], bindings);
            } else {
                // Try default value
                auto dit = fdef.defaults.find(fdef.params[i]);
                if (dit != fdef.defaults.end()) {
                    Value dval = evaluate_expr_tree(dit->second, new_bindings);
                    new_bindings[fdef.params[i]] = resolve_expression_value(dval, new_bindings);
                }
            }
        }
        Value result = evaluate_expr_tree(fdef.body, new_bindings);
        return result;
    }

    return Value();
}

// Extract positional arguments in numeric order (NOT lexicographic)
static std::vector<ExprNodePtr> args_to_expr_trees(const Arguments& args) {
    std::vector<ExprNodePtr> result;
    for (size_t i = 0; ; i++) {
        auto it = args.find("_" + std::to_string(i));
        if (it == args.end()) break;
        const Value& v = it->second;
        ExprNodePtr tree = v.exprTree();
        if (!tree) {
            if (v.isNumber()) tree = ExprNode::makeLiteral(v.toNumber());
            else if (v.isVector()) {
                // Build a VectorLiteral tree from vector elements
                std::vector<ExprNodePtr> elems;
                for (size_t j = 0; j < v.size(); j++) {
                    ExprNodePtr et = v[j].exprTree();
                    if (!et && v[j].isNumber()) et = ExprNode::makeLiteral(v[j].toNumber());
                    elems.push_back(et ? et : ExprNode::makeLiteral(0));
                }
                tree = ExprNode::makeVectorLiteral(elems);
            }
        }
        result.push_back(tree ? tree : ExprNode::makeLiteral(0));
    }
    return result;
}

// ─── DXF dimension/cross-point extraction ───────────────────────────────────
// Implements OpenSCAD's dxf_dim() and dxf_cross() built-in functions.

// Forward declarations (defined later in this file)
extern const std::vector<std::string>& get_include_paths();
extern const std::string& get_current_file_dir();
extern bool is_in_use_context();
static std::string resolve_include_file(const std::string& filename, const std::string& from_dir);

struct DxfDimension {
    std::string name;
    double x1, y1, x2, y2;  // Definition points (group codes 13/23 and 14/24)
};

struct DxfLine {
    std::string layer;
    double x1, y1, x2, y2;  // Start/end points
};

// Parse a DXF file and extract DIMENSION entities and LINE entities
static void parse_dxf_entities(const std::string& filepath,
                               std::vector<DxfDimension>& dims,
                               std::vector<DxfLine>& dxf_lines) {
    std::ifstream file(filepath);
    if (!file.is_open()) return;

    std::vector<std::pair<std::string,std::string>> pairs;
    std::string code_line, val_line;
    while (std::getline(file, code_line) && std::getline(file, val_line)) {
        // Trim whitespace
        auto trim = [](std::string& s) {
            size_t start = s.find_first_not_of(" \t\r\n");
            size_t end = s.find_last_not_of(" \t\r\n");
            s = (start == std::string::npos) ? "" : s.substr(start, end - start + 1);
        };
        trim(code_line);
        trim(val_line);
        pairs.push_back({code_line, val_line});
    }

    for (size_t i = 0; i < pairs.size(); i++) {
        if (pairs[i].first == "0" && pairs[i].second == "DIMENSION") {
            DxfDimension dim = {};
            for (size_t j = i + 1; j < pairs.size() && !(pairs[j].first == "0"); j++) {
                const auto& code = pairs[j].first;
                const auto& val = pairs[j].second;
                if (code == "1") dim.name = val;
                else if (code == "13") dim.x1 = std::stod(val);
                else if (code == "23") dim.y1 = std::stod(val);
                else if (code == "14") dim.x2 = std::stod(val);
                else if (code == "24") dim.y2 = std::stod(val);
            }
            dims.push_back(dim);
        }
        else if (pairs[i].first == "0" && pairs[i].second == "LINE") {
            DxfLine ln = {};
            for (size_t j = i + 1; j < pairs.size() && !(pairs[j].first == "0"); j++) {
                const auto& code = pairs[j].first;
                const auto& val = pairs[j].second;
                if (code == "8") ln.layer = val;
                else if (code == "10") ln.x1 = std::stod(val);
                else if (code == "20") ln.y1 = std::stod(val);
                else if (code == "11") ln.x2 = std::stod(val);
                else if (code == "21") ln.y2 = std::stod(val);
            }
            dxf_lines.push_back(ln);
        }
    }
}

// dxf_dim: returns the distance between the two definition points of a named DIMENSION entity
static Value eval_dxf_dim(const Arguments& args) {
    std::string filename, dim_name;
    for (const auto& kv : args) {
        if (kv.first == "file") filename = kv.second.toString();
        else if (kv.first == "name") dim_name = kv.second.toString();
    }
    if (filename.empty() || dim_name.empty()) return Value();

    std::string resolved = resolve_include_file(filename, get_current_file_dir());
    if (resolved.empty()) return Value();

    std::vector<DxfDimension> dims;
    std::vector<DxfLine> lines;
    parse_dxf_entities(resolved, dims, lines);

    for (const auto& d : dims) {
        if (d.name == dim_name) {
            double dist = std::sqrt((d.x2 - d.x1) * (d.x2 - d.x1) + (d.y2 - d.y1) * (d.y2 - d.y1));
            return Value(dist);
        }
    }
    return Value();
}

// dxf_cross: returns the intersection point of two LINE entities on the given layer
static Value eval_dxf_cross(const Arguments& args) {
    std::string filename, layer;
    for (const auto& kv : args) {
        if (kv.first == "file") filename = kv.second.toString();
        else if (kv.first == "layer") layer = kv.second.toString();
    }
    if (filename.empty() || layer.empty()) return Value();

    std::string resolved = resolve_include_file(filename, get_current_file_dir());
    if (resolved.empty()) return Value();

    std::vector<DxfDimension> dims;
    std::vector<DxfLine> lines;
    parse_dxf_entities(resolved, dims, lines);

    // Collect LINE entities on the given layer
    std::vector<DxfLine> layer_lines;
    for (const auto& ln : lines) {
        if (ln.layer == layer) layer_lines.push_back(ln);
    }
    if (layer_lines.size() < 2) return Value();

    // Find intersection of first two lines
    const auto& l1 = layer_lines[0];
    const auto& l2 = layer_lines[1];
    double dx1 = l1.x2 - l1.x1, dy1 = l1.y2 - l1.y1;
    double dx2 = l2.x2 - l2.x1, dy2 = l2.y2 - l2.y1;
    double denom = dx1 * dy2 - dy1 * dx2;
    if (std::abs(denom) < 1e-12) return Value();  // Parallel lines

    double t = ((l2.x1 - l1.x1) * dy2 - (l2.y1 - l1.y1) * dx2) / denom;
    double ix = l1.x1 + t * dx1;
    double iy = l1.y1 + t * dy1;

    return Value(Vector{Value(ix), Value(iy)});
}

// Entry point from grammar: try to fully evaluate a function call
static Value try_evaluate_function(const std::string& name, const Arguments& args) {
    // Reset step counter for each top-level evaluation
    g_eval_steps = 0;

    // Handle DXF built-in functions early (they use named args, not user-defined)
    if (name == "dxf_dim") {
        Value result = eval_dxf_dim(args);
        if (result.isNumber()) {
            result.setExprTree(ExprNode::makeLiteral(result.toNumber()));
            return result;
        }
        return Value();  // Return undef if DXF parsing fails
    }
    if (name == "dxf_cross") {
        Value result = eval_dxf_cross(args);
        if (result.isVector()) return result;
        return Value();
    }

    // Extract ExprNode trees from positional args in numeric order
    auto arg_trees = args_to_expr_trees(args);

    // During function definition, parameters are symbolic — don't eagerly evaluate.
    // Build a FunctionCall ExprNode tree for deferred evaluation at call time.
    if (g_in_function_def) {
        // Check if there are named args
        bool has_named = false;
        for (const auto& kv : args) {
            if (kv.first.size() > 0 && kv.first[0] != '_') {
                has_named = true;
                break;
            }
        }
        if (has_named) {
            std::vector<std::string> arg_names_list;
            std::vector<ExprNodePtr> all_trees;
            // First positional args
            for (size_t i = 0; ; i++) {
                auto it = args.find("_" + std::to_string(i));
                if (it == args.end()) break;
                const Value& v = it->second;
                ExprNodePtr tree = v.exprTree();
                if (!tree) {
                    if (v.isNumber()) tree = ExprNode::makeLiteral(v.toNumber());
                    else if (v.isVector()) {
                        std::vector<ExprNodePtr> elems;
                        for (size_t j = 0; j < v.size(); j++) {
                            ExprNodePtr et = v[j].exprTree();
                            if (!et && v[j].isNumber()) et = ExprNode::makeLiteral(v[j].toNumber());
                            elems.push_back(et ? et : ExprNode::makeLiteral(0));
                        }
                        tree = ExprNode::makeVectorLiteral(elems);
                    }
                }
                all_trees.push_back(tree ? tree : ExprNode::makeLiteral(0));
                arg_names_list.push_back("");
            }
            // Then named args
            for (const auto& kv : args) {
                if (kv.first.size() > 0 && kv.first[0] != '_') {
                    const Value& v = kv.second;
                    ExprNodePtr tree = v.exprTree();
                    if (!tree) {
                        if (v.isNumber()) tree = ExprNode::makeLiteral(v.toNumber());
                        else if (v.isVector()) {
                            std::vector<ExprNodePtr> elems;
                            for (size_t j = 0; j < v.size(); j++) {
                                ExprNodePtr et = v[j].exprTree();
                                if (!et && v[j].isNumber()) et = ExprNode::makeLiteral(v[j].toNumber());
                                elems.push_back(et ? et : ExprNode::makeLiteral(0));
                            }
                            tree = ExprNode::makeVectorLiteral(elems);
                        }
                    }
                    all_trees.push_back(tree ? tree : ExprNode::makeLiteral(0));
                    arg_names_list.push_back(kv.first);
                }
            }
            auto fc_tree = ExprNode::makeFunctionCall(name, all_trees);
            fc_tree->arg_names = arg_names_list;
            return Value::expressionWithTree(name + "(...)", fc_tree);
        }
        // Positional-only: build FunctionCall tree
        auto fc_tree = ExprNode::makeFunctionCall(name, arg_trees);
        return Value::expressionWithTree(name + "(...)", fc_tree);
    }

    // Check if there are named args (keys that don't start with "_")
    std::vector<std::string> arg_names;
    std::vector<ExprNodePtr> all_arg_trees;
    bool has_named = false;
    for (const auto& kv : args) {
        if (kv.first.size() > 0 && kv.first[0] != '_') {
            has_named = true;
            break;
        }
    }

    if (has_named) {
        // Rebuild arg list preserving names, in order
        // First positional args (in numeric order)
        for (size_t i = 0; ; i++) {
            auto it = args.find("_" + std::to_string(i));
            if (it == args.end()) break;
            const Value& v = it->second;
            ExprNodePtr tree = v.exprTree();
            if (!tree) {
                if (v.isNumber()) tree = ExprNode::makeLiteral(v.toNumber());
                else if (v.isVector()) {
                    std::vector<ExprNodePtr> elems;
                    for (size_t j = 0; j < v.size(); j++) {
                        ExprNodePtr et = v[j].exprTree();
                        if (!et && v[j].isNumber()) et = ExprNode::makeLiteral(v[j].toNumber());
                        elems.push_back(et ? et : ExprNode::makeLiteral(0));
                    }
                    tree = ExprNode::makeVectorLiteral(elems);
                }
            }
            all_arg_trees.push_back(tree ? tree : ExprNode::makeLiteral(0));
            arg_names.push_back(""); // positional
        }
        // Then named args
        for (const auto& kv : args) {
            if (kv.first.size() > 0 && kv.first[0] != '_') {
                const Value& v = kv.second;
                ExprNodePtr tree = v.exprTree();
                if (!tree) {
                    if (v.isNumber()) tree = ExprNode::makeLiteral(v.toNumber());
                    else if (v.isVector()) {
                        std::vector<ExprNodePtr> elems;
                        for (size_t j = 0; j < v.size(); j++) {
                            ExprNodePtr et = v[j].exprTree();
                            if (!et && v[j].isNumber()) et = ExprNode::makeLiteral(v[j].toNumber());
                            elems.push_back(et ? et : ExprNode::makeLiteral(0));
                        }
                        tree = ExprNode::makeVectorLiteral(elems);
                    }
                }
                all_arg_trees.push_back(tree ? tree : ExprNode::makeLiteral(0));
                arg_names.push_back(kv.first);
            }
        }

        // Try to evaluate with named arg resolution
        auto fit = g_function_table.find(name);
        if (fit != g_function_table.end()) {
            const FunctionDef& fdef = fit->second;
            // Use current scope for resolving variable references in arguments
            std::map<std::string, Value> scope_bindings(current_scope().begin(), current_scope().end());
            std::vector<Value> positioned(fdef.params.size());

            // Fill defaults first
            for (size_t i = 0; i < fdef.params.size(); i++) {
                auto dit = fdef.defaults.find(fdef.params[i]);
                if (dit != fdef.defaults.end()) {
                    positioned[i] = evaluate_expr_tree(dit->second, scope_bindings);
                }
            }

            // Apply provided args
            size_t pos_idx = 0;
            for (size_t i = 0; i < all_arg_trees.size(); i++) {
                Value val = evaluate_expr_tree(all_arg_trees[i], scope_bindings);
                if (i < arg_names.size() && !arg_names[i].empty()) {
                    // Named arg — find position
                    for (size_t j = 0; j < fdef.params.size(); j++) {
                        if (fdef.params[j] == arg_names[i]) {
                            positioned[j] = val;
                            break;
                        }
                    }
                } else {
                    // Positional
                    if (pos_idx < positioned.size()) positioned[pos_idx] = val;
                    pos_idx++;
                }
            }

            Value result = evaluate_function_call(name, positioned, scope_bindings);
            if (result.isNumber()) {
                result.setExprTree(ExprNode::makeLiteral(result.toNumber()));
                return result;
            }
            if (result.isVector()) return result;
        }

        // Fall back to FunctionCall ExprNode with named args
        auto fc_tree = ExprNode::makeFunctionCall(name, all_arg_trees);
        fc_tree->arg_names = arg_names;
        return Value::expressionWithTree(name + "(...)", fc_tree);
    }

    // Original positional-only path
    // Build a list of arg values by evaluating trees using current scope
    std::map<std::string, Value> scope_bindings2(current_scope().begin(), current_scope().end());
    std::vector<Value> arg_vals;
    for (const auto& tree : arg_trees) {
        arg_vals.push_back(evaluate_expr_tree(tree, scope_bindings2));
    }

    Value result = evaluate_function_call(name, arg_vals, scope_bindings2);

    // If we got a concrete result, return it — but preserve the function call
    // expression tree if any argument had variable references (for module params, loop vars)
    if (result.isNumber()) {
        // Check if any arg tree has variable refs
        bool hasVarRefs = false;
        for (const auto& tree : arg_trees) {
            if (tree && tree->hasVariableRefs()) { hasVarRefs = true; break; }
        }
        if (hasVarRefs) {
            auto fc_tree = ExprNode::makeFunctionCall(name, arg_trees);
            result.setExprTree(fc_tree);
        } else {
            result.setExprTree(ExprNode::makeLiteral(result.toNumber()));
        }
        return result;
    }
    if (result.isVector()) {
        // Preserve function call tree if any arg has variable refs
        bool hasVarRefs = false;
        for (const auto& tree : arg_trees) {
            if (tree && tree->hasVariableRefs()) { hasVarRefs = true; break; }
        }
        if (hasVarRefs) {
            auto fc_tree = ExprNode::makeFunctionCall(name, arg_trees);
            result.setExprTree(fc_tree);
        }
        return result;
    }

    // Fall back to storing a FunctionCall ExprNode tree
    auto fc_tree = ExprNode::makeFunctionCall(name, arg_trees);
    return Value::expressionWithTree(name + "(...)", fc_tree);
}

// Include path resolution (shared with lexer)
extern const std::vector<std::string>& get_include_paths();
extern const std::string& get_current_file_dir();

static std::string resolve_include_file(const std::string& filename, const std::string& from_dir) {
    // 1. Try each include path first (allows filtered library overrides)
    for (const auto& p : get_include_paths()) {
        std::string path = p + "/" + filename;
        FILE* test = fopen(path.c_str(), "r");
        if (test) { fclose(test); return path; }
    }
    // 2. Try relative to the referring file's directory
    if (!from_dir.empty()) {
        std::string path = from_dir + "/" + filename;
        FILE* test = fopen(path.c_str(), "r");
        if (test) { fclose(test); return path; }
    }
    // 3. Try as absolute path
    {
        FILE* test = fopen(filename.c_str(), "r");
        if (test) { fclose(test); return filename; }
    }
    return "";
}

// Forward declaration for recursive prescan
static void prescan_variables_internal(FILE* f, const std::string& file_dir);

// Pre-scan file for simple top-level assignments (id = true/false/number ;)
// This implements OpenSCAD's "hoisted" variable semantics so that ternaries
// evaluated before the assignment line still see the correct value.
void prescan_variables(FILE* f) {
    prescan_variables_internal(f, get_current_file_dir());
    // Set defaults for OpenSCAD runtime special variables that have no
    // static equivalent.  Safe fallback values prevent undefined-name
    // errors when these leak into the generated Python.
    if (lookup_variable("$parent_modules").isUndefined())
        set_variable("$parent_modules", Value(0.0));
    if (lookup_variable("$children").isUndefined())
        set_variable("$children", Value(0.0));
    // OpenSCAD built-in constants
    set_variable("PI", Value(M_PI));

    // Record all variables set so far as top-level globals.
    // Inside module bodies, references to these will produce VarRef expression
    // trees so the code generator can link them to group_input sockets.
    auto& scope = current_scope();
    for (const auto& kv : scope) {
        if (kv.first[0] != '$' && kv.first != "PI") {  // skip special vars and constants
            g_top_level_vars.insert(kv.first);
        }
    }
}

static void prescan_variables_internal(FILE* f, const std::string& file_dir) {
    // Save current position
    long saved = ftell(f);
    rewind(f);

    // Simple state machine to find patterns like: ID = VALUE ;
    // at the top level (not inside braces)
    int brace_depth = 0;
    int c;
    std::string token;
    enum { IDLE, GOT_ID, GOT_EQ, GOT_VAL } state = IDLE;
    std::string current_id;
    std::string current_val;
    bool in_line_comment = false;
    bool in_block_comment = false;
    int prev_c = 0;

    while ((c = fgetc(f)) != EOF) {
        // Handle comments
        if (in_line_comment) {
            if (c == '\n') in_line_comment = false;
            prev_c = c;
            continue;
        }
        if (in_block_comment) {
            if (prev_c == '*' && c == '/') in_block_comment = false;
            prev_c = c;
            continue;
        }
        if (prev_c == '/' && c == '/') { in_line_comment = true; prev_c = c; continue; }
        if (prev_c == '/' && c == '*') { in_block_comment = true; prev_c = c; continue; }

        if (c == '{') { brace_depth++; state = IDLE; prev_c = c; continue; }
        if (c == '}') { brace_depth--; state = IDLE; prev_c = c; continue; }

        // Only process top-level assignments
        if (brace_depth > 0) { prev_c = c; continue; }

        if (state == IDLE) {
            if (isalpha(c) || c == '_' || c == '$') {
                token.clear();
                token += (char)c;
                state = GOT_ID;
            }
        } else if (state == GOT_ID) {
            if (isalnum(c) || c == '_') {
                token += (char)c;
            } else if (c == '=' && !token.empty()) {
                // Check if this is "include" or "use" — not an assignment
                // (These keywords won't have '=' after them in valid syntax,
                //  but handle them in IDLE state below instead)
                current_id = token;
                current_val.clear();
                state = GOT_EQ;
            } else if (c == '(' || c == '{') {
                // This is a function/module call, not assignment
                if (c == '{') brace_depth++;
                state = IDLE;
            } else if (!isspace(c)) {
                // Check for include/use directive
                if ((token == "include" || token == "use") && (c == '<' || c == '"')) {
                    char end_delim = (c == '<') ? '>' : '"';
                    std::string inc_file;
                    while ((c = fgetc(f)) != EOF && c != end_delim) {
                        inc_file += (char)c;
                    }
                    // Resolve and recursively prescan
                    std::string resolved = resolve_include_file(inc_file, file_dir);
                    if (!resolved.empty()) {
                        FILE* inc_f = fopen(resolved.c_str(), "r");
                        if (inc_f) {
                            // Get directory of included file for nested includes
                            char* rc = strdup(resolved.c_str());
                            std::string inc_dir = dirname(rc);
                            free(rc);
                            prescan_variables_internal(inc_f, inc_dir);
                            fclose(inc_f);
                        }
                    }
                }
                state = IDLE;
            }
        } else if (state == GOT_EQ) {
            if (isspace(c)) {
                if (current_val.empty()) { prev_c = c; continue; }
                // whitespace after value - stay in GOT_VAL
                state = GOT_VAL;
            } else if (c == ';') {
                // End of assignment
                std::string val = current_val.empty() ? "" : current_val;
                // Trim
                while (!val.empty() && isspace(val.back())) val.pop_back();
                if (val == "true") {
                    set_variable(current_id, Value(true));
                } else if (val == "false") {
                    set_variable(current_id, Value(false));
                } else {
                    // Try to parse as number
                    try {
                        size_t pos;
                        double d = std::stod(val, &pos);
                        if (pos == val.size()) {
                            set_variable(current_id, Value(d));
                        }
                    } catch (...) {}
                }
                state = IDLE;
            } else {
                current_val += (char)c;
                state = GOT_EQ; // stay in GOT_EQ while collecting value chars
            }
        } else if (state == GOT_VAL) {
            if (c == ';') {
                std::string val = current_val;
                while (!val.empty() && isspace(val.back())) val.pop_back();
                if (val == "true") {
                    set_variable(current_id, Value(true));
                } else if (val == "false") {
                    set_variable(current_id, Value(false));
                } else {
                    try {
                        size_t pos;
                        double d = std::stod(val, &pos);
                        if (pos == val.size()) {
                            set_variable(current_id, Value(d));
                        }
                    } catch (...) {}
                }
                state = IDLE;
            } else if (!isspace(c)) {
                // More value chars after whitespace - complex expression, abort
                state = IDLE;
            }
        }
        prev_c = c;
    }

    // Restore file position
    fseek(f, saved, SEEK_SET);
}
%}

%union {
    double number;
    bool boolean;
    std::string* str;
    scad2blender::Value* value;
    scad2blender::ASTNode* node;
    scad2blender::Arguments* args;
    std::vector<scad2blender::ASTNodePtr>* node_list;
    std::vector<std::string>* str_list;
}

/* Tokens */
%token TOK_MODULE TOK_FUNCTION TOK_IF TOK_ELSE TOK_FOR TOK_LET TOK_EACH
%token TOK_ASSERT TOK_ECHO TOK_UNDEF

/* Primitives */
%token TOK_CUBE TOK_SPHERE TOK_CYLINDER TOK_POLYHEDRON
%token TOK_CIRCLE TOK_SQUARE TOK_POLYGON TOK_TEXT

/* Transforms */
%token TOK_TRANSLATE TOK_ROTATE TOK_SCALE TOK_MIRROR
%token TOK_COLOR TOK_OFFSET TOK_RESIZE TOK_MULTMATRIX

/* Boolean operations */
%token TOK_UNION TOK_DIFFERENCE TOK_INTERSECTION TOK_INTERSECTION_FOR

/* Extrusions */
%token TOK_LINEAR_EXTRUDE TOK_ROTATE_EXTRUDE

/* Other modules */
%token TOK_HULL TOK_MINKOWSKI TOK_ROOF TOK_PROJECTION TOK_IMPORT TOK_SURFACE TOK_CHILDREN

/* Operators */
%token TOK_AND TOK_OR TOK_EQ TOK_NE TOK_LE TOK_GE

/* Literals */
%token <number> TOK_NUMBER
%token <str> TOK_STRING TOK_ID TOK_SPECIAL_VAR
%token <boolean> TOK_TRUE TOK_FALSE

/* Types */
%type <node> program statement module_stmt function_stmt module_instantiation
%type <node> single_module_instantiation child_statement
%type <node> primitive_call transform_call boolean_call extrude_call other_call
%type <node> if_statement
%type <node> for_statement
%type <node_list> statements child_statements
%type <value> expr vector_expr
%type <args> arguments argument_list
%type <str_list> parameter_list
%type <str> func_name param_name keyword_id module_name

/* Operator precedence */
%right '?' ':'
%left TOK_OR
%left TOK_AND
%nonassoc TOK_EQ TOK_NE
%nonassoc '<' '>' TOK_LE TOK_GE
%left '+' '-'
%left '*' '/' '%'
%right '^'
%right UNARY

%%

program:
    statements {
        auto root = std::make_shared<RootNode>();
        if ($1) {
            for (auto& stmt : *$1) {
                root->addChild(stmt);
            }
            delete $1;
        }
        g_root = root;
    }
    ;

statements:
    /* empty */ {
        $$ = new std::vector<ASTNodePtr>();
    }
    | statements statement {
        $$ = $1;
        if ($2) {
            // In 'use' context, only keep module defs, function defs, and assignments
            // Skip top-level module instantiations (OpenSCAD 'use' semantics)
            if (is_in_use_context()) {
                auto t = $2->type();
                if (t != ASTNode::Type::Module &&
                    t != ASTNode::Type::FunctionDef &&
                    t != ASTNode::Type::Assignment) {
                    // Skip this statement — it's a top-level instantiation from a 'use' file
                    $$ = $1;
                } else {
                    $$->push_back(ASTNodePtr($2));
                }
            } else {
                $$->push_back(ASTNodePtr($2));
            }
        }
    }
    ;

statement:
    ';' { $$ = nullptr; }
    | '{' child_statements '}' {
        /* Standalone block — treat as implicit union container */
        if ($2 && !$2->empty()) {
            auto container = new BooleanNode(ASTNode::Type::Union);
            container->setDebug(true);
            for (auto& child : *$2) {
                container->addChild(child);
            }
            $$ = container;
        } else {
            $$ = nullptr;
        }
        delete $2;
    }
    | module_stmt { $$ = $1; }
    | function_stmt { $$ = $1; }
    | module_instantiation child_statement {
        if ($1 && $2) {
            $1->addChild(ASTNodePtr($2));
        }
        $$ = $1;
    }
    | module_instantiation ';' { $$ = $1; }
    | '!' module_instantiation ';' { $$ = $2; if ($$) $$->setRoot(true); }
    | '#' module_instantiation ';' { $$ = $2; if ($$) $$->setDebug(true); }
    | '%' module_instantiation ';' { $$ = $2; if ($$) $$->setBackground(true); }
    | '*' module_instantiation ';' { $$ = $2; if ($$) $$->setDisabled(true); }
    | '!' if_statement { $$ = $2; }
    | '#' if_statement { $$ = $2; }
    | '%' if_statement { $$ = $2; }
    | '*' if_statement { $$ = $2; }
    | TOK_ID '=' expr ';' {
        // Try to evaluate expression to a concrete value before storing
        Value storeVal = *$3;
        if ($3->isExpression()) {
            ExprNodePtr tree = $3->exprTree();
            if (tree && tree->hasVariableRefs()) {
                // Expression has unresolved variable refs (module params, etc.)
                // Preserve the expression tree for runtime evaluation
                storeVal = *$3;
            } else if (tree) {
                // Use current scope for resolving variable references
                std::map<std::string, Value> bindings(current_scope().begin(), current_scope().end());
                Value resolved = evaluate_expr_tree(tree, bindings);
                if (resolved.isNumber()) {
                    storeVal = resolved;
                    storeVal.setExprTree(ExprNode::makeLiteral(resolved.toNumber()));
                } else if (resolved.isVector()) {
                    storeVal = resolved;
                    if (tree->hasVariableRefs()) {
                        storeVal.setExprTree(tree);
                    }
                }
            }
        } else if ($3->isVector() && $3->exprTree() && $3->exprTree()->hasVariableRefs()) {
            // Vector with expression tree (e.g. from rands() with param-dependent args)
            // Already a vector but keep the expression tree for runtime evaluation
            storeVal = *$3;
        } else if ($3->isNumber() && $3->exprTree() && $3->exprTree()->hasVariableRefs()) {
            // Number with expression tree containing VarRefs (e.g. min(y+1,r-y) resolved but tree preserved)
            // Convert to Expression type so code generator handles it at runtime
            storeVal = Value::expressionWithTree(std::to_string($3->toNumber()), $3->exprTree());
        }
        set_variable(*$1, storeVal);
        // Track bare-literal assignments inside module bodies so that later
        // references produce VarRef expression trees for parametric linking
        if (g_in_function_def && !storeVal.isExpression() &&
            (storeVal.isNumber() || storeVal.isBool()) &&
            (!storeVal.exprTree() || !storeVal.exprTree()->hasVariableRefs())) {
            bool isBareLiteral = true;
            if ($3->exprTree() && $3->exprTree()->hasVariableRefs()) {
                isBareLiteral = false;
            }
            if ($3->isExpression()) {
                isBareLiteral = false;
            }
            if (isBareLiteral) {
                g_module_literal_vars.insert(*$1);
            }
        }
        $$ = new AssignmentNode(*$1, storeVal);
        delete $1;
        delete $3;
    }
    | TOK_SPECIAL_VAR '=' expr ';' {
        Value storeVal = *$3;
        if ($3->isExpression()) {
            // Use current scope for resolving variable references
            std::map<std::string, Value> bindings(current_scope().begin(), current_scope().end());
            ExprNodePtr tree = $3->exprTree();
            if (tree) {
                Value resolved = evaluate_expr_tree(tree, bindings);
                if (resolved.isNumber()) {
                    storeVal = resolved;
                    storeVal.setExprTree(ExprNode::makeLiteral(resolved.toNumber()));
                } else if (resolved.isVector()) {
                    storeVal = resolved;
                }
            }
        }
        set_variable(*$1, storeVal);
        $$ = new AssignmentNode(*$1, storeVal);
        delete $1;
        delete $3;
    }
    | keyword_id '=' expr ';' {
        // Assignment to a keyword-named variable
        Value storeVal = *$3;
        if ($3->isExpression()) {
            std::map<std::string, Value> empty;
            ExprNodePtr tree = $3->exprTree();
            if (tree) {
                Value resolved = evaluate_expr_tree(tree, empty);
                if (resolved.isNumber()) {
                    storeVal = resolved;
                    storeVal.setExprTree(ExprNode::makeLiteral(resolved.toNumber()));
                } else if (resolved.isVector()) {
                    storeVal = resolved;
                }
            }
        }
        set_variable(*$1, storeVal);
        $$ = new AssignmentNode(*$1, storeVal);
        delete $1;
        delete $3;
    }
    | if_statement { $$ = $1; }
    | for_statement { $$ = $1; }
    | '!' for_statement { $$ = $2; if ($$) $$->setRoot(true); }
    | '#' for_statement { $$ = $2; if ($$) $$->setDebug(true); }
    | '%' for_statement { $$ = $2; if ($$) $$->setBackground(true); }
    | '*' for_statement { $$ = $2; if ($$) $$->setDisabled(true); }
    | TOK_INTERSECTION_FOR '(' TOK_ID '=' expr ')' {
        push_scope();
        auto tree = ExprNode::makeVarRef(*$3);
        set_variable(*$3, Value::expressionWithTree(*$3, tree));
    } child_statement {
        pop_scope();
        auto node = new ForLoopNode(*$3, *$5);
        node->setIntersect(true);
        if ($8) node->addChild(ASTNodePtr($8));
        $$ = node;
        delete $3;
        delete $5;
    }
    | TOK_ECHO '(' arguments ')' ';' {
        // echo() is ignored - just parse and discard
        delete $3;
        $$ = nullptr;
    }
    | TOK_ASSERT '(' arguments ')' ';' {
        // assert() is ignored - just parse and discard
        delete $3;
        $$ = nullptr;
    }
    | TOK_LET '(' arguments ')' child_statement {
        // let() as statement modifier — discard bindings, pass through body
        delete $3;
        $$ = $5;
    }
    ;

module_stmt:
    TOK_MODULE module_name '(' parameter_list ')' {
        // Mid-rule action: save parameter defaults BEFORE the body is parsed.
        // The module body may reassign parameter names (e.g. grad=[grad,grad]),
        // which would overwrite the symbol table entries.
        g_module_param_defaults.clear();
        for (const auto& param : *$4) {
            // Only store defaults for parameters that had explicit `= expr`
            if (g_params_with_explicit_default.count(param)) {
                Value v = lookup_variable(param);
                if (!v.isUndefined()) {
                    g_module_param_defaults[param] = v;
                }
            }
        }
        g_params_with_explicit_default.clear();
        push_scope();  // isolate body scope from parameter defaults
        // Set module parameters as VarRef expressions so that expressions
        // inside the body are deferred instead of eagerly evaluated
        g_in_function_def_stack.push_back(g_in_function_def);
        g_in_function_def = true;
        g_module_literal_vars_stack.push_back(g_module_literal_vars);
        g_module_literal_vars.clear();
        for (const auto& param : *$4) {
            set_variable(param, Value::expressionWithTree(param, ExprNode::makeVarRef(param)));
        }
    } child_statement {
        g_in_function_def = g_in_function_def_stack.back();
        g_in_function_def_stack.pop_back();
        g_module_literal_vars = g_module_literal_vars_stack.back();
        g_module_literal_vars_stack.pop_back();
        pop_scope();  // restore scope
        auto node = new ModuleNode(*$2, *$4);
        // Use the saved defaults (from before body parsing)
        for (const auto& kv : g_module_param_defaults) {
            node->setParameterDefault(kv.first, kv.second);
        }
        if ($7) node->addChild(ASTNodePtr($7));
        $$ = node;
        delete $2;
        delete $4;
    }
    | TOK_MODULE module_name '(' parameter_list ')' {
        // Module with no body — same mid-rule to be consistent
        auto node = new ModuleNode(*$2, *$4);
        for (const auto& param : *$4) {
            if (g_params_with_explicit_default.count(param)) {
                Value v = lookup_variable(param);
                if (!v.isUndefined()) {
                    node->setParameterDefault(param, v);
                }
            }
        }
        g_params_with_explicit_default.clear();
        $$ = node;
        delete $2;
        delete $4;
    }
    | TOK_MODULE error '}' {
        // Error recovery: skip module definitions with invalid names (e.g., starting with digits)
        $$ = nullptr;
        yyerrok;
    }
    ;

function_stmt:
    TOK_FUNCTION func_name '(' { g_in_function_def_stack.push_back(g_in_function_def); g_in_function_def = true; } parameter_list ')' '=' expr ';' {
        g_in_function_def = g_in_function_def_stack.back(); g_in_function_def_stack.pop_back();
        FunctionDef fdef;
        fdef.params = *$5;
        fdef.body = $8->exprTree();
        if (!fdef.body && $8->isNumber())
            fdef.body = ExprNode::makeLiteral($8->toNumber());
        if (!fdef.body) {
            // Handle vector-valued function bodies
            if ($8->isVector()) {
                std::vector<ExprNodePtr> elems;
                for (size_t i = 0; i < $8->size(); i++) {
                    ExprNodePtr et = (*$8)[i].exprTree();
                    if (!et && (*$8)[i].isNumber()) et = ExprNode::makeLiteral((*$8)[i].toNumber());
                    elems.push_back(et ? et : ExprNode::makeLiteral(0));
                }
                fdef.body = ExprNode::makeVectorLiteral(elems);
            }
        }
        // Store default values for parameters
        for (const auto& param : *$5) {
            // Check for stored default under mangled key
            std::string default_key = "__fndef_default_" + param;
            Value dv = lookup_variable(default_key);
            if (!dv.isUndefined()) {
                if (dv.exprTree()) {
                    fdef.defaults[param] = dv.exprTree();
                } else if (dv.isNumber()) {
                    fdef.defaults[param] = ExprNode::makeLiteral(dv.toNumber());
                } else if (dv.isBool()) {
                    fdef.defaults[param] = ExprNode::makeLiteral(dv.toBool() ? 1.0 : 0.0);
                }
            }
        }
        g_function_table[*$2] = fdef;
        auto fn = new FunctionNode(*$2, *$5);
        fn->setBody(fdef.body);
        for (const auto& kv : fdef.defaults) {
            fn->setParamDefault(kv.first, kv.second);
        }
        $$ = fn;
        delete $2; delete $5; delete $8;
    }
    ;

func_name:
    TOK_ID { $$ = $1; }
    | TOK_SCALE { $$ = new std::string("scale"); }
    | TOK_TRANSLATE { $$ = new std::string("translate"); }
    | TOK_ROTATE { $$ = new std::string("rotate"); }
    | TOK_MIRROR { $$ = new std::string("mirror"); }
    | TOK_COLOR { $$ = new std::string("color"); }
    | TOK_OFFSET { $$ = new std::string("offset"); }
    | TOK_RESIZE { $$ = new std::string("resize"); }
    | TOK_HULL { $$ = new std::string("hull"); }
    | TOK_IMPORT { $$ = new std::string("import"); }
    | TOK_LET { $$ = new std::string("let"); }
    ;

param_name:
    TOK_ID { $$ = $1; }
    | TOK_SCALE { $$ = new std::string("scale"); }
    | TOK_TRANSLATE { $$ = new std::string("translate"); }
    | TOK_ROTATE { $$ = new std::string("rotate"); }
    | TOK_MIRROR { $$ = new std::string("mirror"); }
    | TOK_COLOR { $$ = new std::string("color"); }
    | TOK_OFFSET { $$ = new std::string("offset"); }
    | TOK_RESIZE { $$ = new std::string("resize"); }
    | TOK_HULL { $$ = new std::string("hull"); }
    | TOK_IMPORT { $$ = new std::string("import"); }
    | TOK_CHILDREN { $$ = new std::string("children"); }
    | TOK_TEXT { $$ = new std::string("text"); }
    | TOK_SPHERE { $$ = new std::string("sphere"); }
    ;

keyword_id:
    TOK_SCALE { $$ = new std::string("scale"); }
    | TOK_TRANSLATE { $$ = new std::string("translate"); }
    | TOK_ROTATE { $$ = new std::string("rotate"); }
    | TOK_MIRROR { $$ = new std::string("mirror"); }
    | TOK_COLOR { $$ = new std::string("color"); }
    | TOK_OFFSET { $$ = new std::string("offset"); }
    | TOK_RESIZE { $$ = new std::string("resize"); }
    | TOK_HULL { $$ = new std::string("hull"); }
    | TOK_IMPORT { $$ = new std::string("import"); }
    | TOK_CHILDREN { $$ = new std::string("children"); }
    | TOK_TEXT { $$ = new std::string("text"); }
    | TOK_SPHERE { $$ = new std::string("sphere"); }
    ;

module_name:
    TOK_ID { $$ = $1; }
    | TOK_CIRCLE { $$ = new std::string("circle"); }
    | TOK_SQUARE { $$ = new std::string("square"); }
    | TOK_POLYGON { $$ = new std::string("polygon"); }
    | TOK_CUBE { $$ = new std::string("cube"); }
    | TOK_CYLINDER { $$ = new std::string("cylinder"); }
    | TOK_POLYHEDRON { $$ = new std::string("polyhedron"); }
    | keyword_id { $$ = $1; }
    ;

parameter_list:
    /* empty */ { $$ = new std::vector<std::string>(); }
    | param_name {
        $$ = new std::vector<std::string>();
        $$->push_back(*$1);
        if (g_in_function_def) {
            // Store as VarRef expression so function body preserves references
            set_variable(*$1, Value::expressionWithTree(*$1, ExprNode::makeVarRef(*$1)));
        }
        delete $1;
    }
    | param_name '=' expr {
        $$ = new std::vector<std::string>();
        $$->push_back(*$1);
        g_params_with_explicit_default.insert(*$1);
        if (g_in_function_def) {
            // For function params: store as VarRef for body parsing,
            // the default value will be extracted separately in function_stmt
            // But first save the actual default so we can retrieve it later
            // Store the default under a mangled name, and the VarRef under the param name
            std::string default_key = "__fndef_default_" + *$1;
            if ($3->isNumber() || $3->isVector() || $3->isBool() || $3->isString()) {
                set_variable(default_key, *$3);
            } else if ($3->isExpression()) {
                std::map<std::string, Value> empty;
                ExprNodePtr tree = $3->exprTree();
                if (tree) {
                    Value resolved = evaluate_expr_tree(tree, empty);
                    if (!resolved.isUndefined()) set_variable(default_key, resolved);
                    else set_variable(default_key, *$3);
                }
            }
            set_variable(*$1, Value::expressionWithTree(*$1, ExprNode::makeVarRef(*$1)));
        } else {
            // Store default value in symbol table so module body can reference it
            if ($3->isNumber() || $3->isVector() || $3->isBool() || $3->isString()) {
                set_variable(*$1, *$3);
            } else if ($3->isExpression()) {
                std::map<std::string, Value> empty;
                ExprNodePtr tree = $3->exprTree();
                if (tree) {
                    Value resolved = evaluate_expr_tree(tree, empty);
                    if (!resolved.isUndefined()) set_variable(*$1, resolved);
                }
            }
        }
        delete $1;
        delete $3;
    }
    | parameter_list ',' param_name {
        $$ = $1;
        $$->push_back(*$3);
        if (g_in_function_def) {
            set_variable(*$3, Value::expressionWithTree(*$3, ExprNode::makeVarRef(*$3)));
        }
        delete $3;
    }
    | parameter_list ',' param_name '=' expr {
        $$ = $1;
        $$->push_back(*$3);
        g_params_with_explicit_default.insert(*$3);
        if (g_in_function_def) {
            std::string default_key = "__fndef_default_" + *$3;
            if ($5->isNumber() || $5->isVector() || $5->isBool() || $5->isString()) {
                set_variable(default_key, *$5);
            } else if ($5->isExpression()) {
                std::map<std::string, Value> empty;
                ExprNodePtr tree = $5->exprTree();
                if (tree) {
                    Value resolved = evaluate_expr_tree(tree, empty);
                    if (!resolved.isUndefined()) set_variable(default_key, resolved);
                    else set_variable(default_key, *$5);
                }
            }
            set_variable(*$3, Value::expressionWithTree(*$3, ExprNode::makeVarRef(*$3)));
        } else {
            // Store default value in symbol table so module body can reference it
            if ($5->isNumber() || $5->isVector() || $5->isBool() || $5->isString()) {
                set_variable(*$3, *$5);
            } else if ($5->isExpression()) {
                std::map<std::string, Value> empty;
                ExprNodePtr tree = $5->exprTree();
                if (tree) {
                    Value resolved = evaluate_expr_tree(tree, empty);
                    if (!resolved.isUndefined()) set_variable(*$3, resolved);
                }
            }
        }
        delete $3;
        delete $5;
    }
    | TOK_SPECIAL_VAR {
        $$ = new std::vector<std::string>();
        $$->push_back(*$1);
        if (g_in_function_def) {
            set_variable(*$1, Value::expressionWithTree(*$1, ExprNode::makeVarRef(*$1)));
        }
        delete $1;
    }
    | TOK_SPECIAL_VAR '=' expr {
        $$ = new std::vector<std::string>();
        $$->push_back(*$1);
        g_params_with_explicit_default.insert(*$1);
        if (g_in_function_def) {
            std::string default_key = "__fndef_default_" + *$1;
            if ($3->isNumber() || $3->isVector() || $3->isBool() || $3->isString()) {
                set_variable(default_key, *$3);
            }
            set_variable(*$1, Value::expressionWithTree(*$1, ExprNode::makeVarRef(*$1)));
        } else {
            if ($3->isNumber() || $3->isVector() || $3->isBool() || $3->isString()) {
                set_variable(*$1, *$3);
            }
        }
        delete $1;
        delete $3;
    }
    | parameter_list ',' TOK_SPECIAL_VAR {
        $$ = $1;
        $$->push_back(*$3);
        if (g_in_function_def) {
            set_variable(*$3, Value::expressionWithTree(*$3, ExprNode::makeVarRef(*$3)));
        }
        delete $3;
    }
    | parameter_list ',' TOK_SPECIAL_VAR '=' expr {
        $$ = $1;
        $$->push_back(*$3);
        g_params_with_explicit_default.insert(*$3);
        if (g_in_function_def) {
            std::string default_key = "__fndef_default_" + *$3;
            if ($5->isNumber() || $5->isVector() || $5->isBool() || $5->isString()) {
                set_variable(default_key, *$5);
            }
            set_variable(*$3, Value::expressionWithTree(*$3, ExprNode::makeVarRef(*$3)));
        } else {
            if ($5->isNumber() || $5->isVector() || $5->isBool() || $5->isString()) {
                set_variable(*$3, *$5);
            }
        }
        delete $3;
        delete $5;
    }
    ;

module_instantiation:
    single_module_instantiation { $$ = $1; }
    | '!' single_module_instantiation {
        $$ = $2;
        if ($$) $$->setRoot(true);
    }
    | '#' single_module_instantiation {
        $$ = $2;
        if ($$) $$->setDebug(true);
    }
    | '%' single_module_instantiation {
        $$ = $2;
        if ($$) $$->setBackground(true);
    }
    | '*' single_module_instantiation {
        $$ = $2;
        if ($$) $$->setDisabled(true);
    }
    ;

single_module_instantiation:
    primitive_call { $$ = $1; }
    | transform_call { $$ = $1; }
    | boolean_call { $$ = $1; }
    | extrude_call { $$ = $1; }
    | other_call { $$ = $1; }
    | TOK_ID '(' arguments ')' {
        $$ = new ModuleCallNode(*$1, *$3);
        delete $1;
        delete $3;
    }
    ;

primitive_call:
    TOK_CUBE '(' arguments ')' {
        $$ = new PrimitiveNode(ASTNode::Type::Cube, *$3);
        delete $3;
    }
    | TOK_SPHERE '(' arguments ')' {
        $$ = new PrimitiveNode(ASTNode::Type::Sphere, *$3);
        delete $3;
    }
    | TOK_CYLINDER '(' arguments ')' {
        $$ = new PrimitiveNode(ASTNode::Type::Cylinder, *$3);
        delete $3;
    }
    | TOK_POLYHEDRON '(' arguments ')' {
        $$ = new PrimitiveNode(ASTNode::Type::Polyhedron, *$3);
        delete $3;
    }
    | TOK_CIRCLE '(' arguments ')' {
        $$ = new PrimitiveNode(ASTNode::Type::Circle, *$3);
        delete $3;
    }
    | TOK_SQUARE '(' arguments ')' {
        $$ = new PrimitiveNode(ASTNode::Type::Square, *$3);
        delete $3;
    }
    | TOK_POLYGON '(' arguments ')' {
        $$ = new PrimitiveNode(ASTNode::Type::Polygon, *$3);
        delete $3;
    }
    | TOK_TEXT '(' arguments ')' {
        $$ = new PrimitiveNode(ASTNode::Type::Text, *$3);
        delete $3;
    }
    ;

transform_call:
    TOK_TRANSLATE '(' arguments ')' {
        $$ = new TransformNode(ASTNode::Type::Translate, *$3);
        delete $3;
    }
    | TOK_ROTATE '(' arguments ')' {
        $$ = new TransformNode(ASTNode::Type::Rotate, *$3);
        delete $3;
    }
    | TOK_SCALE '(' arguments ')' {
        $$ = new TransformNode(ASTNode::Type::Scale, *$3);
        delete $3;
    }
    | TOK_MIRROR '(' arguments ')' {
        $$ = new TransformNode(ASTNode::Type::Mirror, *$3);
        delete $3;
    }
    | TOK_COLOR '(' arguments ')' {
        $$ = new TransformNode(ASTNode::Type::Color, *$3);
        delete $3;
    }
    | TOK_OFFSET '(' arguments ')' {
        $$ = new TransformNode(ASTNode::Type::Offset, *$3);
        delete $3;
    }
    | TOK_RESIZE '(' arguments ')' {
        $$ = new TransformNode(ASTNode::Type::Resize, *$3);
        delete $3;
    }
    | TOK_MULTMATRIX '(' arguments ')' {
        $$ = new TransformNode(ASTNode::Type::Multmatrix, *$3);
        delete $3;
    }
    ;

boolean_call:
    TOK_UNION '(' ')' {
        $$ = new BooleanNode(ASTNode::Type::Union);
    }
    | TOK_DIFFERENCE '(' ')' {
        $$ = new BooleanNode(ASTNode::Type::Difference);
    }
    | TOK_INTERSECTION '(' ')' {
        $$ = new BooleanNode(ASTNode::Type::Intersection);
    }
    ;

extrude_call:
    TOK_LINEAR_EXTRUDE '(' arguments ')' {
        $$ = new ExtrudeNode(ASTNode::Type::LinearExtrude, *$3);
        delete $3;
    }
    | TOK_ROTATE_EXTRUDE '(' arguments ')' {
        $$ = new ExtrudeNode(ASTNode::Type::RotateExtrude, *$3);
        delete $3;
    }
    ;

other_call:
    TOK_HULL '(' ')' {
        $$ = new TransformNode(ASTNode::Type::Hull, Arguments());
    }
    | TOK_HULL '(' arguments ')' {
        $$ = new TransformNode(ASTNode::Type::Hull, *$3);
        delete $3;
    }
    | TOK_MINKOWSKI '(' ')' {
        $$ = new TransformNode(ASTNode::Type::Minkowski, Arguments());
    }
    | TOK_MINKOWSKI '(' arguments ')' {
        $$ = new TransformNode(ASTNode::Type::Minkowski, *$3);
        delete $3;
    }
    | TOK_ROOF '(' ')' {
        $$ = new TransformNode(ASTNode::Type::Roof, Arguments());
    }
    | TOK_ROOF '(' arguments ')' {
        $$ = new TransformNode(ASTNode::Type::Roof, *$3);
        delete $3;
    }
    | TOK_PROJECTION '(' arguments ')' {
        $$ = new PrimitiveNode(ASTNode::Type::Projection, *$3);
        delete $3;
    }
    | TOK_IMPORT '(' arguments ')' {
        $$ = new PrimitiveNode(ASTNode::Type::Import, *$3);
        delete $3;
    }
    | TOK_SURFACE '(' arguments ')' {
        $$ = new PrimitiveNode(ASTNode::Type::Surface, *$3);
        delete $3;
    }
    | TOK_CHILDREN '(' ')' {
        $$ = new ChildrenNode();
    }
    | TOK_CHILDREN '(' arguments ')' {
        $$ = new ChildrenNode();
        delete $3;
    }
    ;

child_statement:
    ';' { $$ = nullptr; }
    | '{' child_statements '}' {
        if ($2 && !$2->empty()) {
            if ($2->size() == 1) {
                // Take ownership by creating new raw pointer copy
                // The shared_ptr will manage the object, we wrap in new ASTNodePtr later
                ASTNode* ptr = $2->at(0).get();
                // Clear the shared_ptr but keep the object alive by manually incrementing
                // Actually, we need to extract and keep ownership
                // Just wrap single child in union container too for consistency
                auto container = new BooleanNode(ASTNode::Type::Union);
                container->setDebug(true);  // Mark as container
                container->addChild($2->at(0));
                $$ = container;
            } else {
                // Multiple children - create a group node to hold them
                // The parent will unpack them appropriately
                auto container = new BooleanNode(ASTNode::Type::Union);
                container->setDebug(true);  // Mark as container, not actual union
                for (auto& child : *$2) {
                    container->addChild(child);
                }
                $$ = container;
            }
        } else {
            $$ = nullptr;
        }
        delete $2;
    }
    | module_instantiation child_statement {
        if ($1 && $2) {
            $1->addChild(ASTNodePtr($2));
        }
        $$ = $1;
    }
    | TOK_ECHO '(' arguments ')' ';' {
        // echo() as child_statement — ignored
        delete $3;
        $$ = nullptr;
    }
    | TOK_ASSERT '(' arguments ')' ';' {
        // assert() as child_statement — ignored
        delete $3;
        $$ = nullptr;
    }
    | if_statement { $$ = $1; }
    | for_statement { $$ = $1; }
    | '!' for_statement { $$ = $2; if ($$) $$->setRoot(true); }
    | '#' for_statement { $$ = $2; if ($$) $$->setDebug(true); }
    | '%' for_statement { $$ = $2; if ($$) $$->setBackground(true); }
    | '*' for_statement { $$ = $2; if ($$) $$->setDisabled(true); }
    | TOK_INTERSECTION_FOR '(' TOK_ID '=' expr ')' child_statement {
        auto node = new ForLoopNode(*$3, *$5);
        node->setIntersect(true);
        if ($7) node->addChild(ASTNodePtr($7));
        $$ = node;
        delete $3;
        delete $5;
    }
    | TOK_ID '=' expr ';' {
        // Assignment as child_statement
        set_variable(*$1, *$3);
        $$ = new AssignmentNode(*$1, *$3);
        delete $1;
        delete $3;
    }
    | keyword_id '=' expr ';' {
        // Keyword-named assignment as child_statement
        set_variable(*$1, *$3);
        $$ = new AssignmentNode(*$1, *$3);
        delete $1;
        delete $3;
    }
    | TOK_SPECIAL_VAR '=' expr ';' {
        // Special var assignment as child_statement
        set_variable(*$1, *$3);
        $$ = new AssignmentNode(*$1, *$3);
        delete $1;
        delete $3;
    }
    | TOK_LET '(' arguments ')' child_statement {
        // let() as child_statement modifier — discard bindings, pass through body
        delete $3;
        $$ = $5;
    }
    ;

child_statements:
    /* empty */ {
        $$ = new std::vector<ASTNodePtr>();
    }
    | child_statements statement {
        $$ = $1;
        if ($2) {
            $$->push_back(ASTNodePtr($2));
        }
    }
    ;

for_statement:
    TOK_FOR '(' TOK_ID '=' expr ')' {
        // Mid-rule: add loop variable as expression VarRef so body references
        // produce expression trees instead of baking the start value
        push_scope();
        auto tree = ExprNode::makeVarRef(*$3);
        set_variable(*$3, Value::expressionWithTree(*$3, tree));
    } child_statement {
        pop_scope();
        auto node = new ForLoopNode(*$3, *$5);
        if ($8) node->addChild(ASTNodePtr($8));
        $$ = node;
        delete $3;
        delete $5;
    }
    | TOK_FOR '(' argument_list ')' {
        // Mid-rule: add all for-loop variables as expression VarRefs
        push_scope();
        for (auto& kv : *$3) {
            if (kv.first.find("_") != 0) {
                auto tree = ExprNode::makeVarRef(kv.first);
                set_variable(kv.first, Value::expressionWithTree(kv.first, tree));
            }
        }
    } child_statement {
        pop_scope();
        // Multi-variable for loop — just use first binding
        std::string var = "i";
        Value range;
        for (auto& kv : *$3) {
            if (kv.first.find("_") != 0) {
                var = kv.first;
                range = kv.second;
                break;
            }
        }
        auto node = new ForLoopNode(var, range);
        if ($6) node->addChild(ASTNodePtr($6));
        $$ = node;
        delete $3;
    }
    ;

if_statement:
    TOK_IF '(' expr ')' child_statement {
        auto node = new IfElseNode(*$3);
        if ($5) node->addChild(ASTNodePtr($5));
        $$ = node;
        delete $3;
    }
    | TOK_IF '(' expr ')' child_statement TOK_ELSE child_statement {
        auto node = new IfElseNode(*$3);
        if ($5) node->addChild(ASTNodePtr($5));
        if ($7) node->setElseBranch(ASTNodePtr($7));
        $$ = node;
        delete $3;
    }
    ;

arguments:
    /* empty */ { $$ = new Arguments(); }
    | argument_list { $$ = $1; }
    | argument_list ',' { $$ = $1; }  /* Allow trailing comma */
    ;

argument_list:
    expr {
        $$ = new Arguments();
        (*$$)["_0"] = *$1;
        g_ordered_args.push_back({"_0", *$1});
        delete $1;
    }
    | TOK_ID '=' expr {
        $$ = new Arguments();
        (*$$)[*$1] = *$3;
        g_ordered_args.push_back({*$1, *$3});
        delete $1;
        delete $3;
    }
    | TOK_SPECIAL_VAR '=' expr {
        $$ = new Arguments();
        (*$$)[*$1] = *$3;
        g_ordered_args.push_back({*$1, *$3});
        delete $1;
        delete $3;
    }
    | TOK_SCALE '=' expr {
        $$ = new Arguments();
        (*$$)["scale"] = *$3;
        g_ordered_args.push_back({"scale", *$3});
        delete $3;
    }
    | TOK_COLOR '=' expr {
        $$ = new Arguments();
        (*$$)["color"] = *$3;
        g_ordered_args.push_back({"color", *$3});
        delete $3;
    }
    | TOK_OFFSET '=' expr {
        $$ = new Arguments();
        (*$$)["offset"] = *$3;
        g_ordered_args.push_back({"offset", *$3});
        delete $3;
    }
    | TOK_TEXT '=' expr {
        $$ = new Arguments();
        (*$$)["text"] = *$3;
        g_ordered_args.push_back({"text", *$3});
        delete $3;
    }
    | TOK_CHILDREN '=' expr {
        $$ = new Arguments();
        (*$$)["children"] = *$3;
        g_ordered_args.push_back({"children", *$3});
        delete $3;
    }
    | TOK_MIRROR '=' expr {
        $$ = new Arguments();
        (*$$)["mirror"] = *$3;
        g_ordered_args.push_back({"mirror", *$3});
        delete $3;
    }
    | TOK_TRANSLATE '=' expr {
        $$ = new Arguments();
        (*$$)["translate"] = *$3;
        g_ordered_args.push_back({"translate", *$3});
        delete $3;
    }
    | TOK_ROTATE '=' expr {
        $$ = new Arguments();
        (*$$)["rotate"] = *$3;
        g_ordered_args.push_back({"rotate", *$3});
        delete $3;
    }
    | TOK_RESIZE '=' expr {
        $$ = new Arguments();
        (*$$)["resize"] = *$3;
        g_ordered_args.push_back({"resize", *$3});
        delete $3;
    }
    | TOK_HULL '=' expr {
        $$ = new Arguments();
        (*$$)["hull"] = *$3;
        g_ordered_args.push_back({"hull", *$3});
        delete $3;
    }
    | TOK_IMPORT '=' expr {
        $$ = new Arguments();
        (*$$)["import"] = *$3;
        g_ordered_args.push_back({"import", *$3});
        delete $3;
    }
    | TOK_SPHERE '=' expr {
        $$ = new Arguments();
        (*$$)["sphere"] = *$3;
        g_ordered_args.push_back({"sphere", *$3});
        delete $3;
    }
    | argument_list ',' expr {
        $$ = $1;
        // Find next available positional index (count only positional keys, not named args)
        size_t idx = 0;
        while ($$->find("_" + std::to_string(idx)) != $$->end()) idx++;
        std::string key = "_" + std::to_string(idx);
        (*$$)[key] = *$3;
        g_ordered_args.push_back({key, *$3});
        delete $3;
    }
    | argument_list ',' TOK_ID '=' expr {
        $$ = $1;
        (*$$)[*$3] = *$5;
        g_ordered_args.push_back({*$3, *$5});
        delete $3;
        delete $5;
    }
    | argument_list ',' TOK_SPECIAL_VAR '=' expr {
        $$ = $1;
        (*$$)[*$3] = *$5;
        g_ordered_args.push_back({*$3, *$5});
        delete $3;
        delete $5;
    }
    | argument_list ',' TOK_SCALE '=' expr {
        $$ = $1;
        (*$$)["scale"] = *$5;
        g_ordered_args.push_back({"scale", *$5});
        delete $5;
    }
    | argument_list ',' TOK_COLOR '=' expr {
        $$ = $1;
        (*$$)["color"] = *$5;
        g_ordered_args.push_back({"color", *$5});
        delete $5;
    }
    | argument_list ',' TOK_OFFSET '=' expr {
        $$ = $1;
        (*$$)["offset"] = *$5;
        g_ordered_args.push_back({"offset", *$5});
        delete $5;
    }
    | argument_list ',' TOK_TEXT '=' expr {
        $$ = $1;
        (*$$)["text"] = *$5;
        g_ordered_args.push_back({"text", *$5});
        delete $5;
    }
    | argument_list ',' TOK_CHILDREN '=' expr {
        $$ = $1;
        (*$$)["children"] = *$5;
        g_ordered_args.push_back({"children", *$5});
        delete $5;
    }
    | argument_list ',' TOK_MIRROR '=' expr {
        $$ = $1;
        (*$$)["mirror"] = *$5;
        g_ordered_args.push_back({"mirror", *$5});
        delete $5;
    }
    | argument_list ',' TOK_TRANSLATE '=' expr {
        $$ = $1;
        (*$$)["translate"] = *$5;
        g_ordered_args.push_back({"translate", *$5});
        delete $5;
    }
    | argument_list ',' TOK_ROTATE '=' expr {
        $$ = $1;
        (*$$)["rotate"] = *$5;
        g_ordered_args.push_back({"rotate", *$5});
        delete $5;
    }
    | argument_list ',' TOK_RESIZE '=' expr {
        $$ = $1;
        (*$$)["resize"] = *$5;
        g_ordered_args.push_back({"resize", *$5});
        delete $5;
    }
    | argument_list ',' TOK_HULL '=' expr {
        $$ = $1;
        (*$$)["hull"] = *$5;
        g_ordered_args.push_back({"hull", *$5});
        delete $5;
    }
    | argument_list ',' TOK_IMPORT '=' expr {
        $$ = $1;
        (*$$)["import"] = *$5;
        g_ordered_args.push_back({"import", *$5});
        delete $5;
    }
    | argument_list ',' TOK_SPHERE '=' expr {
        $$ = $1;
        (*$$)["sphere"] = *$5;
        g_ordered_args.push_back({"sphere", *$5});
        delete $5;
    }
    ;

expr:
    TOK_NUMBER {
        $$ = new Value($1);
        $$->setExprTree(ExprNode::makeLiteral($1));
    }
    | TOK_TRUE { $$ = new Value(true); }
    | TOK_FALSE { $$ = new Value(false); }
    | TOK_UNDEF { $$ = new Value(); }
    | TOK_STRING { $$ = new Value(*$1); delete $1; }
    | TOK_ID {
        // When inside a module/function body (g_in_function_def), look up the variable.
        // If it has a concrete value (not a bare VarRef from module params), use that
        // concrete value so that Vector+Vector etc. work correctly at parse time.
        // Only create Expression(VarRef) for truly symbolic variables (module params,
        // loop vars) or when the variable is not in scope.
        if (g_in_function_def) {
            Value v = lookup_variable(*$1);
            if (!v.isUndefined()) {
                // Check if this is a bare VarRef (module parameter placeholder)
                bool isBareVarRef = v.isExpression() && v.exprTree() &&
                    v.exprTree()->kind == ExprNode::Kind::VarRef &&
                    !v.exprTree()->left && !v.exprTree()->right;
                if (!isBareVarRef) {
                    // Check if this is a module-local literal variable or a top-level
                    // global variable that should be treated as symbolic for parametric
                    // group_input linking
                    if (g_module_literal_vars.count(*$1) || g_top_level_vars.count(*$1)) {
                        // Return as VarRef expression (like a module parameter)
                        auto tree = ExprNode::makeVarRef(*$1);
                        $$ = new Value(Value::expressionWithTree(*$1, tree));
                        delete $1;
                    } else if (v.exprTree() && v.exprTree()->hasVariableRefs()) {
                        // Expression depends on runtime variables (module params, loop vars,
                        // or group_input vars).  Inline the expression tree so that nested
                        // modules (where variables_ is saved/restored) still have the full
                        // expression rather than a bare VarRef that can't be resolved.
                        $$ = new Value(v);
                        delete $1;
                    } else {
                    // Concrete value — return it with its expression tree
                    $$ = new Value(v);
                    // Ensure it has an expression tree for code generation
                    if (!$$->exprTree()) {
                        if (v.isNumber()) {
                            $$->setExprTree(ExprNode::makeLiteral(v.toNumber()));
                        } else if (v.isVector()) {
                            // Build VectorLiteral tree from elements
                            std::vector<ExprNodePtr> elems;
                            for (size_t i = 0; i < v.size(); i++) {
                                ExprNodePtr et = v[i].exprTree();
                                if (!et && v[i].isNumber())
                                    et = ExprNode::makeLiteral(v[i].toNumber());
                                elems.push_back(et ? et : ExprNode::makeLiteral(0));
                            }
                            auto vecTree = std::make_shared<ExprNode>();
                            vecTree->kind = ExprNode::Kind::VectorLiteral;
                            vecTree->func_args = elems;
                            $$->setExprTree(vecTree);
                        }
                    }
                    delete $1;
                    }
                } else {
                    // Module parameter — keep as VarRef expression
                    auto tree = ExprNode::makeVarRef(*$1);
                    $$ = new Value(Value::expressionWithTree(*$1, tree));
                    delete $1;
                }
            } else {
                // Not in scope — create VarRef expression
                auto tree = ExprNode::makeVarRef(*$1);
                $$ = new Value(Value::expressionWithTree(*$1, tree));
                delete $1;
            }
        } else {
            // Outside module/function body — always create VarRef for group_input linking
            auto tree = ExprNode::makeVarRef(*$1);
            $$ = new Value(Value::expressionWithTree(*$1, tree));
            delete $1;
        }
    }
    | TOK_SPECIAL_VAR {
        // Always store as expression to support linking to group inputs
        auto tree = ExprNode::makeVarRef(*$1);
        $$ = new Value(Value::expressionWithTree(*$1, tree));
        delete $1;
    }
    | TOK_SCALE {
        auto tree = ExprNode::makeVarRef("scale");
        $$ = new Value(Value::expressionWithTree("scale", tree));
    }
    | TOK_TRANSLATE {
        auto tree = ExprNode::makeVarRef("translate");
        $$ = new Value(Value::expressionWithTree("translate", tree));
    }
    | TOK_ROTATE {
        auto tree = ExprNode::makeVarRef("rotate");
        $$ = new Value(Value::expressionWithTree("rotate", tree));
    }
    | TOK_MIRROR {
        auto tree = ExprNode::makeVarRef("mirror");
        $$ = new Value(Value::expressionWithTree("mirror", tree));
    }
    | TOK_COLOR {
        auto tree = ExprNode::makeVarRef("color");
        $$ = new Value(Value::expressionWithTree("color", tree));
    }
    | TOK_OFFSET {
        auto tree = ExprNode::makeVarRef("offset");
        $$ = new Value(Value::expressionWithTree("offset", tree));
    }
    | TOK_HULL {
        auto tree = ExprNode::makeVarRef("hull");
        $$ = new Value(Value::expressionWithTree("hull", tree));
    }
    | TOK_CHILDREN {
        auto tree = ExprNode::makeVarRef("children");
        $$ = new Value(Value::expressionWithTree("children", tree));
    }
    | TOK_TEXT {
        auto tree = ExprNode::makeVarRef("text");
        $$ = new Value(Value::expressionWithTree("text", tree));
    }
    | TOK_RESIZE {
        auto tree = ExprNode::makeVarRef("resize");
        $$ = new Value(Value::expressionWithTree("resize", tree));
    }
    | TOK_IMPORT {
        auto tree = ExprNode::makeVarRef("import");
        $$ = new Value(Value::expressionWithTree("import", tree));
    }
    | TOK_SPHERE {
        auto tree = ExprNode::makeVarRef("sphere");
        $$ = new Value(Value::expressionWithTree("sphere", tree));
    }
    | vector_expr { $$ = $1; }
    | '(' expr ')' {
        if ($2->isExpression()) {
            auto tree = $2->exprTree();
            $$ = new Value(Value::expressionWithTree("(" + $2->toPython() + ")", tree));
            delete $2;
        } else {
            $$ = $2;
        }
    }
    | expr '+' expr {
        if ($1->isExpression() || $3->isExpression()) {
            auto ltree = $1->exprTree();
            if (!ltree && $1->isVector()) {
                std::vector<ExprNodePtr> elems;
                for (size_t i = 0; i < $1->size(); i++) {
                    auto et = (*$1)[i].exprTree();
                    elems.push_back(et ? et : ExprNode::makeLiteral((*$1)[i].isNumber() ? (*$1)[i].toNumber() : 0));
                }
                ltree = ExprNode::makeVectorLiteral(elems);
            }
            if (!ltree) ltree = ExprNode::makeLiteral($1->toNumber());
            auto rtree = $3->exprTree();
            if (!rtree && $3->isVector()) {
                std::vector<ExprNodePtr> elems;
                for (size_t i = 0; i < $3->size(); i++) {
                    auto et = (*$3)[i].exprTree();
                    elems.push_back(et ? et : ExprNode::makeLiteral((*$3)[i].isNumber() ? (*$3)[i].toNumber() : 0));
                }
                rtree = ExprNode::makeVectorLiteral(elems);
            }
            if (!rtree) rtree = ExprNode::makeLiteral($3->toNumber());
            auto tree = ExprNode::makeBinary(ExprNode::Op::ADD, ltree, rtree);
            $$ = new Value(Value::expressionWithTree("(" + $1->toPython() + " + " + $3->toPython() + ")", tree));
        } else if ($1->isNumber() && $3->isNumber()) {
            $$ = new Value($1->toNumber() + $3->toNumber());
            $$->setExprTree(ExprNode::makeLiteral($1->toNumber() + $3->toNumber()));
        } else if ($1->isVector() && $3->isVector()) {
            Vector v;
            bool hasExprTrees = false;
            size_t len = std::min($1->size(), $3->size());
            for (size_t i = 0; i < len; i++) {
                const Value& lv = (*$1)[i];
                const Value& rv = (*$3)[i];
                if (lv.isNumber() && rv.isNumber() && !lv.exprTree() && !rv.exprTree()) {
                    v.push_back(Value(lv.toNumber() + rv.toNumber()));
                } else if (lv.isExpression() || rv.isExpression() ||
                           (lv.exprTree() && lv.exprTree()->hasVariableRefs()) ||
                           (rv.exprTree() && rv.exprTree()->hasVariableRefs())) {
                    // Preserve expression trees for components with variable refs
                    auto lt = lv.exprTree() ? lv.exprTree() : ExprNode::makeLiteral(lv.toNumber());
                    auto rt = rv.exprTree() ? rv.exprTree() : ExprNode::makeLiteral(rv.toNumber());
                    auto tree = ExprNode::makeBinary(ExprNode::Op::ADD, lt, rt);
                    Value comp = Value::expressionWithTree("", tree);
                    v.push_back(comp);
                    hasExprTrees = true;
                } else if (lv.isNumber() || rv.isNumber()) {
                    v.push_back(Value(lv.toNumber() + rv.toNumber()));
                } else {
                    v.push_back(Value());
                }
            }
            $$ = new Value(v);
            if (hasExprTrees) {
                std::vector<ExprNodePtr> elems;
                for (size_t i = 0; i < v.size(); i++) {
                    ExprNodePtr et = v[i].exprTree();
                    if (!et && v[i].isNumber()) et = ExprNode::makeLiteral(v[i].toNumber());
                    elems.push_back(et ? et : ExprNode::makeLiteral(0));
                }
                $$->setExprTree(ExprNode::makeVectorLiteral(elems));
            }
        } else {
            $$ = new Value();
        }
        delete $1; delete $3;
    }
    | expr '-' expr {
        if ($1->isExpression() || $3->isExpression()) {
            auto ltree = $1->exprTree();
            if (!ltree && $1->isVector()) {
                std::vector<ExprNodePtr> elems;
                for (size_t i = 0; i < $1->size(); i++) {
                    auto et = (*$1)[i].exprTree();
                    elems.push_back(et ? et : ExprNode::makeLiteral((*$1)[i].isNumber() ? (*$1)[i].toNumber() : 0));
                }
                ltree = ExprNode::makeVectorLiteral(elems);
            }
            if (!ltree) ltree = ExprNode::makeLiteral($1->toNumber());
            auto rtree = $3->exprTree();
            if (!rtree && $3->isVector()) {
                std::vector<ExprNodePtr> elems;
                for (size_t i = 0; i < $3->size(); i++) {
                    auto et = (*$3)[i].exprTree();
                    elems.push_back(et ? et : ExprNode::makeLiteral((*$3)[i].isNumber() ? (*$3)[i].toNumber() : 0));
                }
                rtree = ExprNode::makeVectorLiteral(elems);
            }
            if (!rtree) rtree = ExprNode::makeLiteral($3->toNumber());
            auto tree = ExprNode::makeBinary(ExprNode::Op::SUBTRACT, ltree, rtree);
            $$ = new Value(Value::expressionWithTree("(" + $1->toPython() + " - " + $3->toPython() + ")", tree));
        } else if ($1->isNumber() && $3->isNumber()) {
            $$ = new Value($1->toNumber() - $3->toNumber());
            $$->setExprTree(ExprNode::makeLiteral($1->toNumber() - $3->toNumber()));
        } else if ($1->isVector() && $3->isVector()) {
            Vector v;
            bool hasExprTrees = false;
            size_t len = std::min($1->size(), $3->size());
            for (size_t i = 0; i < len; i++) {
                const Value& lv = (*$1)[i];
                const Value& rv = (*$3)[i];
                if (lv.isNumber() && rv.isNumber() && !lv.exprTree() && !rv.exprTree()) {
                    v.push_back(Value(lv.toNumber() - rv.toNumber()));
                } else if (lv.isExpression() || rv.isExpression() ||
                           (lv.exprTree() && lv.exprTree()->hasVariableRefs()) ||
                           (rv.exprTree() && rv.exprTree()->hasVariableRefs())) {
                    auto lt = lv.exprTree() ? lv.exprTree() : ExprNode::makeLiteral(lv.toNumber());
                    auto rt = rv.exprTree() ? rv.exprTree() : ExprNode::makeLiteral(rv.toNumber());
                    auto tree = ExprNode::makeBinary(ExprNode::Op::SUBTRACT, lt, rt);
                    Value comp = Value::expressionWithTree("", tree);
                    v.push_back(comp);
                    hasExprTrees = true;
                } else if (lv.isNumber() || rv.isNumber()) {
                    v.push_back(Value(lv.toNumber() - rv.toNumber()));
                } else {
                    v.push_back(Value());
                }
            }
            $$ = new Value(v);
            if (hasExprTrees) {
                std::vector<ExprNodePtr> elems;
                for (size_t i = 0; i < v.size(); i++) {
                    ExprNodePtr et = v[i].exprTree();
                    if (!et && v[i].isNumber()) et = ExprNode::makeLiteral(v[i].toNumber());
                    elems.push_back(et ? et : ExprNode::makeLiteral(0));
                }
                $$->setExprTree(ExprNode::makeVectorLiteral(elems));
            }
        } else {
            $$ = new Value();
        }
        delete $1; delete $3;
    }
    | expr '*' expr {
        if ($1->isExpression() || $3->isExpression()) {
            // Check if either operand has VarRef trees that should be preserved
            bool hasVarRefs = ($1->exprTree() && $1->exprTree()->hasVariableRefs()) ||
                              ($3->exprTree() && $3->exprTree()->hasVariableRefs());

            if (hasVarRefs && !$1->isVector() && !$3->isVector()) {
                // Scalar * scalar with VarRefs: preserve expression tree (like + and - rules)
                auto ltree = $1->exprTree() ? $1->exprTree() : ExprNode::makeLiteral($1->toNumber());
                auto rtree = $3->exprTree() ? $3->exprTree() : ExprNode::makeLiteral($3->toNumber());
                auto tree = ExprNode::makeBinary(ExprNode::Op::MULTIPLY, ltree, rtree);
                $$ = new Value(Value::expressionWithTree("(" + $1->toPython() + " * " + $3->toPython() + ")", tree));
            } else if (hasVarRefs && ($1->isVector() || $3->isVector())) {
                // Vector * scalar (or scalar * vector) with VarRefs: build expression tree
                // so exprTreeToPython emits _vmul(vec, scalar) at runtime
                auto ltree = $1->exprTree();
                if (!ltree && $1->isVector()) {
                    std::vector<ExprNodePtr> elems;
                    for (size_t i = 0; i < $1->size(); i++) {
                        auto et = (*$1)[i].exprTree();
                        elems.push_back(et ? et : ExprNode::makeLiteral((*$1)[i].isNumber() ? (*$1)[i].toNumber() : 0));
                    }
                    ltree = ExprNode::makeVectorLiteral(elems);
                }
                if (!ltree) ltree = ExprNode::makeLiteral($1->toNumber());
                auto rtree = $3->exprTree();
                if (!rtree && $3->isVector()) {
                    std::vector<ExprNodePtr> elems;
                    for (size_t i = 0; i < $3->size(); i++) {
                        auto et = (*$3)[i].exprTree();
                        elems.push_back(et ? et : ExprNode::makeLiteral((*$3)[i].isNumber() ? (*$3)[i].toNumber() : 0));
                    }
                    rtree = ExprNode::makeVectorLiteral(elems);
                }
                if (!rtree) rtree = ExprNode::makeLiteral($3->toNumber());
                auto tree = ExprNode::makeBinary(ExprNode::Op::MULTIPLY, ltree, rtree);
                $$ = new Value(Value::expressionWithTree("(" + $1->toPython() + " * " + $3->toPython() + ")", tree));
            } else {
                // Try to resolve Expression operands to concrete values
                // so that Vector * Expression (scalar) works correctly
                Value resolvedL = *$1, resolvedR = *$3;
                if ($1->isExpression() && $1->exprTree()) {
                    std::map<std::string, Value> _bindings(current_scope().begin(), current_scope().end());
                    Value r = evaluate_expr_tree($1->exprTree(), _bindings);
                    if (r.isNumber() || r.isVector()) resolvedL = r;
                }
                if ($3->isExpression() && $3->exprTree()) {
                    std::map<std::string, Value> _bindings(current_scope().begin(), current_scope().end());
                    Value r = evaluate_expr_tree($3->exprTree(), _bindings);
                    if (r.isNumber() || r.isVector()) resolvedR = r;
                }
                // If both resolved to concrete types, compute directly
                if (resolvedL.isNumber() && resolvedR.isNumber()) {
                    $$ = new Value(resolvedL.toNumber() * resolvedR.toNumber());
                    $$->setExprTree(ExprNode::makeLiteral(resolvedL.toNumber() * resolvedR.toNumber()));
                } else if (resolvedL.isVector() && resolvedR.isNumber()) {
                    double s = resolvedR.toNumber();
                    ExprNodePtr sTree = $3->exprTree();
                    Vector v;
                    for (size_t i = 0; i < resolvedL.size(); i++) {
                        if (resolvedL[i].isNumber()) {
                            Value elem(resolvedL[i].toNumber() * s);
                            ExprNodePtr elemTree = resolvedL[i].exprTree();
                            if (sTree && sTree->hasVariableRefs()) {
                                ExprNodePtr et = elemTree ? elemTree : ExprNode::makeLiteral(resolvedL[i].toNumber());
                                elem.setExprTree(ExprNode::makeBinary(ExprNode::Op::MULTIPLY, et, sTree));
                            }
                            v.push_back(elem);
                        }
                        else v.push_back(resolvedL[i]);
                    }
                    $$ = new Value(v);
                } else if (resolvedL.isNumber() && resolvedR.isVector()) {
                    double s = resolvedL.toNumber();
                    ExprNodePtr sTree = $1->exprTree();
                    Vector v;
                    for (size_t i = 0; i < resolvedR.size(); i++) {
                        if (resolvedR[i].isNumber()) {
                            Value elem(resolvedR[i].toNumber() * s);
                            ExprNodePtr elemTree = resolvedR[i].exprTree();
                            if (sTree && sTree->hasVariableRefs()) {
                                ExprNodePtr et = elemTree ? elemTree : ExprNode::makeLiteral(resolvedR[i].toNumber());
                                elem.setExprTree(ExprNode::makeBinary(ExprNode::Op::MULTIPLY, sTree, et));
                            }
                            v.push_back(elem);
                        }
                        else v.push_back(resolvedR[i]);
                    }
                    $$ = new Value(v);
                } else {
                    // Can't fully resolve — build expression tree
                    auto ltree = $1->exprTree() ? $1->exprTree() : ExprNode::makeLiteral($1->toNumber());
                    auto rtree = $3->exprTree() ? $3->exprTree() : ExprNode::makeLiteral($3->toNumber());
                    auto tree = ExprNode::makeBinary(ExprNode::Op::MULTIPLY, ltree, rtree);
                    $$ = new Value(Value::expressionWithTree("(" + $1->toPython() + " * " + $3->toPython() + ")", tree));
                }
            }
        } else if ($1->isNumber() && $3->isNumber()) {
            $$ = new Value($1->toNumber() * $3->toNumber());
            $$->setExprTree(ExprNode::makeLiteral($1->toNumber() * $3->toNumber()));
        } else if ($1->isNumber() && $3->isVector()) {
            double s = $1->toNumber();
            ExprNodePtr sTree = $1->exprTree();
            Vector v;
            for (size_t i = 0; i < $3->size(); i++) {
                if ((*$3)[i].isNumber()) {
                    Value elem((*$3)[i].toNumber() * s);
                    ExprNodePtr elemTree = (*$3)[i].exprTree();
                    if (elemTree && elemTree->hasVariableRefs()) {
                        ExprNodePtr st = sTree ? sTree : ExprNode::makeLiteral(s);
                        elem.setExprTree(ExprNode::makeBinary(ExprNode::Op::MULTIPLY, st, elemTree));
                    } else if (sTree && sTree->hasVariableRefs()) {
                        ExprNodePtr et = elemTree ? elemTree : ExprNode::makeLiteral((*$3)[i].toNumber());
                        elem.setExprTree(ExprNode::makeBinary(ExprNode::Op::MULTIPLY, sTree, et));
                    }
                    v.push_back(elem);
                } else {
                    v.push_back((*$3)[i]);
                }
            }
            $$ = new Value(v);
        } else if ($1->isVector() && $3->isNumber()) {
            double s = $3->toNumber();
            ExprNodePtr sTree = $3->exprTree();
            Vector v;
            for (size_t i = 0; i < $1->size(); i++) {
                if ((*$1)[i].isNumber()) {
                    Value elem((*$1)[i].toNumber() * s);
                    ExprNodePtr elemTree = (*$1)[i].exprTree();
                    if (elemTree && elemTree->hasVariableRefs()) {
                        ExprNodePtr st = sTree ? sTree : ExprNode::makeLiteral(s);
                        elem.setExprTree(ExprNode::makeBinary(ExprNode::Op::MULTIPLY, elemTree, st));
                    } else if (sTree && sTree->hasVariableRefs()) {
                        ExprNodePtr et = elemTree ? elemTree : ExprNode::makeLiteral((*$1)[i].toNumber());
                        elem.setExprTree(ExprNode::makeBinary(ExprNode::Op::MULTIPLY, et, sTree));
                    }
                    v.push_back(elem);
                } else {
                    v.push_back((*$1)[i]);
                }
            }
            $$ = new Value(v);
        } else {
            $$ = new Value();
        }
        delete $1; delete $3;
    }
    | expr '/' expr {
        if ($1->isExpression() || $3->isExpression()) {
            auto ltree = $1->exprTree();
            if (!ltree && $1->isVector()) {
                std::vector<ExprNodePtr> elems;
                for (size_t i = 0; i < $1->size(); i++) {
                    auto et = (*$1)[i].exprTree();
                    elems.push_back(et ? et : ExprNode::makeLiteral((*$1)[i].isNumber() ? (*$1)[i].toNumber() : 0));
                }
                ltree = ExprNode::makeVectorLiteral(elems);
            }
            if (!ltree) ltree = ExprNode::makeLiteral($1->toNumber());
            auto rtree = $3->exprTree();
            if (!rtree && $3->isVector()) {
                std::vector<ExprNodePtr> elems;
                for (size_t i = 0; i < $3->size(); i++) {
                    auto et = (*$3)[i].exprTree();
                    elems.push_back(et ? et : ExprNode::makeLiteral((*$3)[i].isNumber() ? (*$3)[i].toNumber() : 0));
                }
                rtree = ExprNode::makeVectorLiteral(elems);
            }
            if (!rtree) rtree = ExprNode::makeLiteral($3->toNumber());
            auto tree = ExprNode::makeBinary(ExprNode::Op::DIVIDE, ltree, rtree);
            $$ = new Value(Value::expressionWithTree("(" + $1->toPython() + " / " + $3->toPython() + ")", tree));
        } else if ($1->isVector() && ($3->isNumber() || ($3->exprTree() && $3->exprTree()->hasVariableRefs()))) {
            // Vector / scalar: build expression tree for runtime _vdiv
            std::vector<ExprNodePtr> elems;
            for (size_t i = 0; i < $1->size(); i++) {
                auto et = (*$1)[i].exprTree();
                elems.push_back(et ? et : ExprNode::makeLiteral((*$1)[i].isNumber() ? (*$1)[i].toNumber() : 0));
            }
            auto ltree = ExprNode::makeVectorLiteral(elems);
            auto rtree = $3->exprTree() ? $3->exprTree() : ExprNode::makeLiteral($3->toNumber());
            auto tree = ExprNode::makeBinary(ExprNode::Op::DIVIDE, ltree, rtree);
            $$ = new Value(Value::expressionWithTree("(" + $1->toPython() + " / " + $3->toPython() + ")", tree));
        } else if ($1->isNumber() && $3->isNumber() && $3->toNumber() != 0) {
            $$ = new Value($1->toNumber() / $3->toNumber());
            $$->setExprTree(ExprNode::makeLiteral($1->toNumber() / $3->toNumber()));
        } else {
            $$ = new Value();
        }
        delete $1; delete $3;
    }
    | expr '%' expr {
        if ($1->isExpression() || $3->isExpression()) {
            auto ltree = $1->exprTree() ? $1->exprTree() : ExprNode::makeLiteral($1->toNumber());
            auto rtree = $3->exprTree() ? $3->exprTree() : ExprNode::makeLiteral($3->toNumber());
            auto tree = ExprNode::makeBinary(ExprNode::Op::MODULO, ltree, rtree);
            $$ = new Value(Value::expressionWithTree("(" + $1->toPython() + " % " + $3->toPython() + ")", tree));
        } else if ($1->isNumber() && $3->isNumber()) {
            $$ = new Value(std::fmod($1->toNumber(), $3->toNumber()));
            $$->setExprTree(ExprNode::makeLiteral(std::fmod($1->toNumber(), $3->toNumber())));
        } else {
            $$ = new Value();
        }
        delete $1; delete $3;
    }
    | expr '^' expr {
        if ($1->isExpression() || $3->isExpression()) {
            auto ltree = $1->exprTree() ? $1->exprTree() : ExprNode::makeLiteral($1->toNumber());
            auto rtree = $3->exprTree() ? $3->exprTree() : ExprNode::makeLiteral($3->toNumber());
            auto tree = ExprNode::makeBinary(ExprNode::Op::POWER, ltree, rtree);
            $$ = new Value(Value::expressionWithTree("pow(" + $1->toPython() + ", " + $3->toPython() + ")", tree));
        } else if ($1->isNumber() && $3->isNumber()) {
            $$ = new Value(std::pow($1->toNumber(), $3->toNumber()));
            $$->setExprTree(ExprNode::makeLiteral(std::pow($1->toNumber(), $3->toNumber())));
        } else {
            $$ = new Value();
        }
        delete $1; delete $3;
    }
    | '-' expr %prec UNARY {
        bool hasVarTree = ($2->isNumber() && $2->exprTree() && $2->exprTree()->hasVariableRefs());
        if ($2->isExpression() || hasVarTree) {
            auto operand = $2->exprTree() ? $2->exprTree() : ExprNode::makeLiteral($2->toNumber());
            auto tree = ExprNode::makeUnary(ExprNode::Op::NEGATE, operand);
            $$ = new Value(Value::expressionWithTree("(-" + $2->toPython() + ")", tree));
        } else if ($2->isNumber()) {
            $$ = new Value(-$2->toNumber());
            $$->setExprTree(ExprNode::makeLiteral(-$2->toNumber()));
        } else if ($2->isVector()) {
            Vector v;
            for (size_t i = 0; i < $2->size(); i++) {
                if ((*$2)[i].isNumber()) v.push_back(Value(-(*$2)[i].toNumber()));
                else v.push_back((*$2)[i]);
            }
            $$ = new Value(v);
        } else {
            $$ = new Value();
        }
        delete $2;
    }
    | '+' expr %prec UNARY { $$ = $2; }
    | '!' expr %prec UNARY {
        if ($2->isExpression()) {
            auto operand = $2->exprTree() ? $2->exprTree() : ExprNode::makeLiteral(0);
            auto tree = ExprNode::makeUnary(ExprNode::Op::NOT, operand);
            $$ = new Value(Value::expressionWithTree("(not " + $2->toPython() + ")", tree));
        } else if ($2->isBool()) {
            $$ = new Value(!$2->toBool());
        } else if ($2->isNumber()) {
            $$ = new Value($2->toNumber() == 0.0);
        } else {
            $$ = new Value(true); // !undef = true in OpenSCAD
        }
        delete $2;
    }
    | expr '<' expr {
        double lv = 0, rv = 0;
        bool lok = false, rok = false;
        if ($1->isNumber()) { lv = $1->toNumber(); lok = true; }
        else if ($1->isExpression() && $1->exprTree()) {
            std::map<std::string, Value> empty;
            Value r = evaluate_expr_tree($1->exprTree(), empty);
            if (r.isNumber()) { lv = r.toNumber(); lok = true; }
        }
        if ($3->isNumber()) { rv = $3->toNumber(); rok = true; }
        else if ($3->isExpression() && $3->exprTree()) {
            std::map<std::string, Value> empty;
            Value r = evaluate_expr_tree($3->exprTree(), empty);
            if (r.isNumber()) { rv = r.toNumber(); rok = true; }
        }
        if (lok && rok) {
            $$ = new Value(lv < rv);
        } else {
            auto ltree = $1->exprTree() ? $1->exprTree() : ExprNode::makeLiteral($1->toNumber());
            auto rtree = $3->exprTree() ? $3->exprTree() : ExprNode::makeLiteral($3->toNumber());
            auto tree = ExprNode::makeBinary(ExprNode::Op::LESS, ltree, rtree);
            $$ = new Value(Value::expressionWithTree("(" + $1->toPython() + " < " + $3->toPython() + ")", tree));
        }
        delete $1; delete $3;
    }
    | expr '>' expr {
        double lv = 0, rv = 0;
        bool lok = false, rok = false;
        if ($1->isNumber()) { lv = $1->toNumber(); lok = true; }
        else if ($1->isExpression() && $1->exprTree()) {
            std::map<std::string, Value> empty;
            Value r = evaluate_expr_tree($1->exprTree(), empty);
            if (r.isNumber()) { lv = r.toNumber(); lok = true; }
        }
        if ($3->isNumber()) { rv = $3->toNumber(); rok = true; }
        else if ($3->isExpression() && $3->exprTree()) {
            std::map<std::string, Value> empty;
            Value r = evaluate_expr_tree($3->exprTree(), empty);
            if (r.isNumber()) { rv = r.toNumber(); rok = true; }
        }
        if (lok && rok) {
            $$ = new Value(lv > rv);
        } else {
            auto ltree = $1->exprTree() ? $1->exprTree() : ExprNode::makeLiteral($1->toNumber());
            auto rtree = $3->exprTree() ? $3->exprTree() : ExprNode::makeLiteral($3->toNumber());
            auto tree = ExprNode::makeBinary(ExprNode::Op::GREATER, ltree, rtree);
            $$ = new Value(Value::expressionWithTree("(" + $1->toPython() + " > " + $3->toPython() + ")", tree));
        }
        delete $1; delete $3;
    }
    | expr TOK_LE expr {
        double lv = 0, rv = 0;
        bool lok = false, rok = false;
        if ($1->isNumber()) { lv = $1->toNumber(); lok = true; }
        else if ($1->isExpression() && $1->exprTree()) {
            std::map<std::string, Value> empty;
            Value r = evaluate_expr_tree($1->exprTree(), empty);
            if (r.isNumber()) { lv = r.toNumber(); lok = true; }
        }
        if ($3->isNumber()) { rv = $3->toNumber(); rok = true; }
        else if ($3->isExpression() && $3->exprTree()) {
            std::map<std::string, Value> empty;
            Value r = evaluate_expr_tree($3->exprTree(), empty);
            if (r.isNumber()) { rv = r.toNumber(); rok = true; }
        }
        if (lok && rok) {
            $$ = new Value(lv <= rv);
        } else {
            auto ltree = $1->exprTree() ? $1->exprTree() : ExprNode::makeLiteral($1->toNumber());
            auto rtree = $3->exprTree() ? $3->exprTree() : ExprNode::makeLiteral($3->toNumber());
            auto tree = ExprNode::makeBinary(ExprNode::Op::LESS_EQ, ltree, rtree);
            $$ = new Value(Value::expressionWithTree("(" + $1->toPython() + " <= " + $3->toPython() + ")", tree));
        }
        delete $1; delete $3;
    }
    | expr TOK_GE expr {
        double lv = 0, rv = 0;
        bool lok = false, rok = false;
        if ($1->isNumber()) { lv = $1->toNumber(); lok = true; }
        else if ($1->isExpression() && $1->exprTree()) {
            std::map<std::string, Value> empty;
            Value r = evaluate_expr_tree($1->exprTree(), empty);
            if (r.isNumber()) { lv = r.toNumber(); lok = true; }
        }
        if ($3->isNumber()) { rv = $3->toNumber(); rok = true; }
        else if ($3->isExpression() && $3->exprTree()) {
            std::map<std::string, Value> empty;
            Value r = evaluate_expr_tree($3->exprTree(), empty);
            if (r.isNumber()) { rv = r.toNumber(); rok = true; }
        }
        if (lok && rok) {
            $$ = new Value(lv >= rv);
        } else {
            auto ltree = $1->exprTree() ? $1->exprTree() : ExprNode::makeLiteral($1->toNumber());
            auto rtree = $3->exprTree() ? $3->exprTree() : ExprNode::makeLiteral($3->toNumber());
            auto tree = ExprNode::makeBinary(ExprNode::Op::GREATER_EQ, ltree, rtree);
            $$ = new Value(Value::expressionWithTree("(" + $1->toPython() + " >= " + $3->toPython() + ")", tree));
        }
        delete $1; delete $3;
    }
    | expr TOK_EQ expr {
        // Try to evaluate both sides
        Value lv = *$1, rv = *$3;
        if ($1->isExpression() && $1->exprTree()) {
            std::map<std::string, Value> empty;
            Value r = evaluate_expr_tree($1->exprTree(), empty);
            if (!r.isUndefined()) lv = r;
        }
        if ($3->isExpression() && $3->exprTree()) {
            std::map<std::string, Value> empty;
            Value r = evaluate_expr_tree($3->exprTree(), empty);
            if (!r.isUndefined()) rv = r;
        }
        if (lv.isNumber() && rv.isNumber()) {
            $$ = new Value(lv.toNumber() == rv.toNumber());
        } else if (lv.isBool() && rv.isBool()) {
            $$ = new Value(lv.toBool() == rv.toBool());
        } else {
            auto ltree = $1->exprTree() ? $1->exprTree() : ExprNode::makeLiteral($1->toNumber());
            auto rtree = $3->exprTree() ? $3->exprTree() : ExprNode::makeLiteral($3->toNumber());
            auto tree = ExprNode::makeBinary(ExprNode::Op::EQUAL, ltree, rtree);
            $$ = new Value(Value::expressionWithTree("(" + $1->toPython() + " == " + $3->toPython() + ")", tree));
        }
        delete $1; delete $3;
    }
    | expr TOK_NE expr {
        Value lv = *$1, rv = *$3;
        if ($1->isExpression() && $1->exprTree()) {
            std::map<std::string, Value> empty;
            Value r = evaluate_expr_tree($1->exprTree(), empty);
            if (!r.isUndefined()) lv = r;
        }
        if ($3->isExpression() && $3->exprTree()) {
            std::map<std::string, Value> empty;
            Value r = evaluate_expr_tree($3->exprTree(), empty);
            if (!r.isUndefined()) rv = r;
        }
        if (lv.isNumber() && rv.isNumber()) {
            $$ = new Value(lv.toNumber() != rv.toNumber());
        } else if (lv.isBool() && rv.isBool()) {
            $$ = new Value(lv.toBool() != rv.toBool());
        } else {
            auto ltree = $1->exprTree() ? $1->exprTree() : ExprNode::makeLiteral($1->toNumber());
            auto rtree = $3->exprTree() ? $3->exprTree() : ExprNode::makeLiteral($3->toNumber());
            auto tree = ExprNode::makeBinary(ExprNode::Op::NOT_EQUAL, ltree, rtree);
            $$ = new Value(Value::expressionWithTree("(" + $1->toPython() + " != " + $3->toPython() + ")", tree));
        }
        delete $1; delete $3;
    }
    | expr TOK_AND expr {
        if ($1->isExpression() || $3->isExpression()) {
            auto ltree = $1->exprTree() ? $1->exprTree() : ExprNode::makeLiteral($1->toBool() ? 1.0 : 0.0);
            auto rtree = $3->exprTree() ? $3->exprTree() : ExprNode::makeLiteral($3->toBool() ? 1.0 : 0.0);
            auto tree = ExprNode::makeBinary(ExprNode::Op::AND, ltree, rtree);
            $$ = new Value(Value::expressionWithTree("(" + $1->toPython() + " and " + $3->toPython() + ")", tree));
        } else {
            $$ = new Value($1->toBool() && $3->toBool());
        }
        delete $1; delete $3;
    }
    | expr TOK_OR expr {
        if ($1->isExpression() || $3->isExpression()) {
            auto ltree = $1->exprTree() ? $1->exprTree() : ExprNode::makeLiteral($1->toBool() ? 1.0 : 0.0);
            auto rtree = $3->exprTree() ? $3->exprTree() : ExprNode::makeLiteral($3->toBool() ? 1.0 : 0.0);
            auto tree = ExprNode::makeBinary(ExprNode::Op::OR, ltree, rtree);
            $$ = new Value(Value::expressionWithTree("(" + $1->toPython() + " or " + $3->toPython() + ")", tree));
        } else {
            $$ = new Value($1->toBool() || $3->toBool());
        }
        delete $1; delete $3;
    }
    | expr '?' expr ':' expr {
        // Ternary conditional — try to resolve, or build Conditional ExprNode
        // During function definition, parameters are symbolic — never eagerly resolve
        // ternary conditions, as they'd incorrectly evaluate to false/0.
        Value condVal = *$1;
        bool resolved = false;
        if (!g_in_function_def) {
            if ($1->isBool()) { resolved = true; }
            else if ($1->isNumber()) { resolved = true; condVal = Value($1->toNumber() != 0.0); }
            else if ($1->isExpression()) {
                // Try variable lookup
                Value looked = lookup_variable($1->toString());
                if (!looked.isUndefined()) {
                    condVal = looked;
                    resolved = true;
                } else if ($1->exprTree()) {
                    // Try evaluating the expression tree with current scope
                    std::map<std::string, Value> scope_bindings(current_scope().begin(), current_scope().end());
                    Value eval_result = evaluate_expr_tree($1->exprTree(), scope_bindings);
                    if (eval_result.isBool()) { condVal = eval_result; resolved = true; }
                    else if (eval_result.isNumber()) { condVal = Value(eval_result.toNumber() != 0.0); resolved = true; }
                }
            }
        }
        if (resolved) {
            bool cond = condVal.toBool();
            delete $1;
            if (cond) { $$ = $3; delete $5; }
            else      { $$ = $5; delete $3; }
        } else {
            // Can't resolve — build Conditional ExprNode preserving both branches
            ExprNodePtr cond_tree = $1->exprTree();
            if (!cond_tree) cond_tree = ExprNode::makeLiteral(0);
            ExprNodePtr then_tree = $3->exprTree();
            if (!then_tree) {
                if ($3->isNumber()) then_tree = ExprNode::makeLiteral($3->toNumber());
                else if ($3->isVector()) {
                    std::vector<ExprNodePtr> elems;
                    for (size_t i = 0; i < $3->size(); i++) {
                        ExprNodePtr et = (*$3)[i].exprTree();
                        if (!et && (*$3)[i].isNumber()) et = ExprNode::makeLiteral((*$3)[i].toNumber());
                        elems.push_back(et ? et : ExprNode::makeLiteral(0));
                    }
                    then_tree = ExprNode::makeVectorLiteral(elems);
                } else {
                    then_tree = ExprNode::makeLiteral(0);
                }
            }
            ExprNodePtr else_tree = $5->exprTree();
            if (!else_tree) {
                if ($5->isNumber()) else_tree = ExprNode::makeLiteral($5->toNumber());
                else if ($5->isVector()) {
                    std::vector<ExprNodePtr> elems;
                    for (size_t i = 0; i < $5->size(); i++) {
                        ExprNodePtr et = (*$5)[i].exprTree();
                        if (!et && (*$5)[i].isNumber()) et = ExprNode::makeLiteral((*$5)[i].toNumber());
                        elems.push_back(et ? et : ExprNode::makeLiteral(0));
                    }
                    else_tree = ExprNode::makeVectorLiteral(elems);
                } else {
                    else_tree = ExprNode::makeLiteral(0);
                }
            }
            auto ternary_tree = ExprNode::makeConditional(cond_tree, then_tree, else_tree);
            $$ = new Value(Value::expressionWithTree(
                "(" + $3->toPython() + " if " + $1->toPython() + " else " + $5->toPython() + ")",
                ternary_tree));
            delete $1; delete $3; delete $5;
        }
    }
    | expr '[' expr ']' {
        if ($1->isVector() && $3->isNumber()) {
            size_t idx = static_cast<size_t>($3->toNumber());
            if (idx < $1->size()) {
                $$ = new Value($1->toVector()[idx]);
            } else {
                $$ = new Value();
            }
        } else if ($1->isExpression() && $1->exprTree() && $3->isNumber()) {
            // Try evaluating the expression tree to get a vector
            std::map<std::string, Value> bindings(current_scope().begin(), current_scope().end());
            Value resolved = evaluate_expr_tree($1->exprTree(), bindings);
            if (resolved.isVector()) {
                size_t idx = static_cast<size_t>($3->toNumber());
                if (idx < resolved.size()) {
                    $$ = new Value(resolved.toVector()[idx]);
                } else {
                    $$ = new Value();
                }
            } else {
                // Can't resolve at parse time — build deferred __index__ ExprNode
                ExprNodePtr vec_tree = $1->exprTree();
                ExprNodePtr idx_tree = $3->exprTree();
                if (!idx_tree) idx_tree = ExprNode::makeLiteral($3->toNumber());
                auto fc = ExprNode::makeFunctionCall("__index__", {vec_tree, idx_tree});
                $$ = new Value(Value::expressionWithTree("0", fc));
            }
        } else if ($1->exprTree() && $3->isNumber()) {
            // Expression without exprTree check — still try to defer
            ExprNodePtr vec_tree = $1->exprTree();
            ExprNodePtr idx_tree = ExprNode::makeLiteral($3->toNumber());
            auto fc = ExprNode::makeFunctionCall("__index__", {vec_tree, idx_tree});
            $$ = new Value(Value::expressionWithTree("0", fc));
        } else if ($1->exprTree() && $3->exprTree()) {
            // Both vector and index are expressions — defer with __index__
            ExprNodePtr vec_tree = $1->exprTree();
            ExprNodePtr idx_tree = $3->exprTree();
            auto fc = ExprNode::makeFunctionCall("__index__", {vec_tree, idx_tree});
            $$ = new Value(Value::expressionWithTree("0", fc));
        } else if ($3->exprTree()) {
            // Index is expression, vector may or may not have tree
            ExprNodePtr vec_tree = $1->exprTree();
            if (!vec_tree) {
                // Build a vector literal tree from $1 if it's a vector
                if ($1->isVector()) {
                    std::vector<ExprNodePtr> elems;
                    for (size_t i = 0; i < $1->size(); i++) {
                        const Value& elem = $1->toVector()[i];
                        if (elem.exprTree()) elems.push_back(elem.exprTree());
                        else elems.push_back(ExprNode::makeLiteral(elem.toNumber()));
                    }
                    vec_tree = ExprNode::makeVectorLiteral(elems);
                } else {
                    vec_tree = ExprNode::makeLiteral($1->toNumber());
                }
            }
            ExprNodePtr idx_tree = $3->exprTree();
            auto fc = ExprNode::makeFunctionCall("__index__", {vec_tree, idx_tree});
            $$ = new Value(Value::expressionWithTree("0", fc));
        } else {
            $$ = new Value();
        }
        delete $1; delete $3;
    }
    | expr '.' TOK_ID {
        // Dot-member access: v.x = v[0], v.y = v[1], v.z = v[2]
        int idx = -1;
        if (*$3 == "x") idx = 0;
        else if (*$3 == "y") idx = 1;
        else if (*$3 == "z") idx = 2;
        if (idx >= 0 && $1->isVector() && (size_t)idx < $1->size()) {
            $$ = new Value($1->toVector()[idx]);
        } else if (idx >= 0 && $1->isExpression() && $1->exprTree()) {
            // Try evaluating the expression tree to get a vector
            // Use current scope bindings so variable references like 'cut' can resolve
            std::map<std::string, Value> bindings(current_scope().begin(), current_scope().end());
            Value resolved = evaluate_expr_tree($1->exprTree(), bindings);
            if (resolved.isVector() && (size_t)idx < resolved.size()) {
                $$ = new Value(resolved.toVector()[idx]);
            } else {
                // Can't resolve — use repr() (not toPython()) to preserve the raw name
                std::string rawName = $1->repr();
                // Strip surrounding <expr: > if present
                if (rawName.size() > 7 && rawName.substr(0, 7) == "<expr: ") {
                    rawName = rawName.substr(7, rawName.size() - 8);
                }
                auto tree = ExprNode::makeVarRef(rawName + "." + *$3);
                $$ = new Value(Value::expressionWithTree(rawName + "." + *$3, tree));
            }
        } else {
            $$ = new Value();
        }
        delete $1; delete $3;
    }
    | '[' expr ':' expr ']' {
        // Range [start:end]
        $$ = new Value();
        double start_val = 0, end_val = 0;
        bool start_ok = false, end_ok = false;
        ExprNodePtr startExpr, endExpr;
        if ($2->isNumber()) { start_val = $2->toNumber(); start_ok = true; }
        else if ($2->isExpression() && $2->exprTree()) {
            startExpr = $2->exprTree();
            std::map<std::string, Value> bindings(current_scope().begin(), current_scope().end());
            Value r = evaluate_expr_tree($2->exprTree(), bindings);
            if (r.isNumber()) { start_val = r.toNumber(); start_ok = true; }
        }
        if ($4->isNumber()) { end_val = $4->toNumber(); end_ok = true; }
        else if ($4->isExpression() && $4->exprTree()) {
            endExpr = $4->exprTree();
            std::map<std::string, Value> bindings(current_scope().begin(), current_scope().end());
            Value r = evaluate_expr_tree($4->exprTree(), bindings);
            if (r.isNumber()) { end_val = r.toNumber(); end_ok = true; }
        }
        if (start_ok && end_ok) {
            if (startExpr || endExpr) {
                if (!startExpr) startExpr = ExprNode::makeLiteral(start_val);
                if (!endExpr) endExpr = ExprNode::makeLiteral(end_val);
                *$$ = Value::rangeWithExprs(start_val, end_val, 1.0, startExpr, endExpr);
            } else {
                *$$ = Value::range(start_val, end_val);
            }
        } else if (startExpr || endExpr) {
            // Can't resolve values but have expression trees — store for deferred evaluation
            if (!startExpr) startExpr = ExprNode::makeLiteral(start_val);
            if (!endExpr && $4->exprTree()) endExpr = $4->exprTree();
            else if (!endExpr) endExpr = ExprNode::makeLiteral(end_val);
            *$$ = Value::rangeWithExprs(start_val, end_val, 1.0, startExpr, endExpr);
        }
        delete $2; delete $4;
    }
    | '[' expr ':' expr ':' expr ']' {
        // Range [start:step:end]
        $$ = new Value();
        double start_val = 0, step_val = 0, end_val = 0;
        bool start_ok = false, step_ok = false, end_ok = false;
        ExprNodePtr startExpr, stepExpr, endExpr;
        if ($2->isNumber()) { start_val = $2->toNumber(); start_ok = true; }
        else if ($2->isExpression() && $2->exprTree()) {
            startExpr = $2->exprTree();
            std::map<std::string, Value> empty;
            Value r = evaluate_expr_tree($2->exprTree(), empty);
            if (r.isNumber()) { start_val = r.toNumber(); start_ok = true; }
        }
        if ($4->isNumber()) { step_val = $4->toNumber(); step_ok = true; }
        else if ($4->isExpression() && $4->exprTree()) {
            stepExpr = $4->exprTree();
            std::map<std::string, Value> empty;
            Value r = evaluate_expr_tree($4->exprTree(), empty);
            if (r.isNumber()) { step_val = r.toNumber(); step_ok = true; }
        }
        if ($6->isNumber()) { end_val = $6->toNumber(); end_ok = true; }
        else if ($6->isExpression() && $6->exprTree()) {
            endExpr = $6->exprTree();
            std::map<std::string, Value> empty;
            Value r = evaluate_expr_tree($6->exprTree(), empty);
            if (r.isNumber()) { end_val = r.toNumber(); end_ok = true; }
        }
        if (start_ok && step_ok && end_ok) {
            if (startExpr || stepExpr || endExpr) {
                if (!startExpr) startExpr = ExprNode::makeLiteral(start_val);
                if (!endExpr) endExpr = ExprNode::makeLiteral(end_val);
                if (!stepExpr) stepExpr = ExprNode::makeLiteral(step_val);
                *$$ = Value::rangeWithExprs(start_val, end_val, step_val, startExpr, endExpr, stepExpr);
            } else {
                *$$ = Value::range(start_val, end_val, step_val);
            }
        } else if (startExpr || stepExpr || endExpr) {
            // Can't resolve all values but have expression trees — store for deferred evaluation
            if (!startExpr) startExpr = ExprNode::makeLiteral(start_val);
            if (!stepExpr && $4->exprTree()) stepExpr = $4->exprTree();
            else if (!stepExpr) stepExpr = ExprNode::makeLiteral(step_val);
            if (!endExpr && $6->exprTree()) endExpr = $6->exprTree();
            else if (!endExpr) endExpr = ExprNode::makeLiteral(end_val);
            *$$ = Value::rangeWithExprs(start_val, end_val, step_val, startExpr, endExpr, stepExpr);
        }
        delete $2; delete $4; delete $6;
    }
    | TOK_ID '(' arguments ')' {
        Value result = try_evaluate_function(*$1, *$3);
        $$ = new Value(result);
        delete $1; delete $3;
    }
    | TOK_TRANSLATE '(' arguments ')' {
        Value result = try_evaluate_function("translate", *$3);
        $$ = new Value(result);
        delete $3;
    }
    | TOK_ROTATE '(' arguments ')' {
        Value result = try_evaluate_function("rotate", *$3);
        $$ = new Value(result);
        delete $3;
    }
    | TOK_MIRROR '(' arguments ')' {
        Value result = try_evaluate_function("mirror", *$3);
        $$ = new Value(result);
        delete $3;
    }
    | TOK_SCALE '(' arguments ')' {
        Value result = try_evaluate_function("scale", *$3);
        $$ = new Value(result);
        delete $3;
    }
    | TOK_LET '(' { g_ordered_args_stack.push_back(g_ordered_args); g_ordered_args.clear(); } arguments ')' expr {
        // let() expression — evaluate bindings and apply to body
        // Use g_ordered_args for insertion-order bindings (critical for let-bindings)
        std::vector<std::pair<std::string, ExprNodePtr>> let_pairs;
        std::map<std::string, Value> let_bindings;

        // Build let_pairs in insertion order from g_ordered_args
        // Only include entries whose key exists in $4 (the Arguments map) — this filters
        // out entries from inner function call argument lists that polluted g_ordered_args
        std::set<std::string> seen_keys;
        for (auto& kv : g_ordered_args) {
            if (kv.first.size() > 0 && kv.first[0] != '_' && $4->count(kv.first) && !seen_keys.count(kv.first)) {
                seen_keys.insert(kv.first);
                ExprNodePtr tree = kv.second.exprTree();
                if (!tree) {
                    if (kv.second.isNumber()) tree = ExprNode::makeLiteral(kv.second.toNumber());
                    else if (kv.second.isBool()) tree = ExprNode::makeLiteral(kv.second.toBool() ? 1.0 : 0.0);
                    else if (kv.second.isVector()) {
                        std::vector<ExprNodePtr> elems;
                        for (size_t i = 0; i < kv.second.size(); i++) {
                            ExprNodePtr et = kv.second[i].exprTree();
                            if (!et && kv.second[i].isNumber()) et = ExprNode::makeLiteral(kv.second[i].toNumber());
                            else if (!et && kv.second[i].isBool()) et = ExprNode::makeLiteral(kv.second[i].toBool() ? 1.0 : 0.0);
                            else if (!et && kv.second[i].isExpression()) et = ExprNode::makeVarRef(kv.second[i].toString());
                            elems.push_back(et ? et : ExprNode::makeLiteral(0));
                        }
                        tree = ExprNode::makeVectorLiteral(elems);
                    } else if (kv.second.isExpression()) {
                        tree = ExprNode::makeVarRef(kv.second.toString());
                    } else if (kv.second.isUndefined()) {
                        tree = ExprNode::makeLiteral(std::numeric_limits<double>::quiet_NaN());
                    }
                }
                if (tree) let_pairs.push_back({kv.first, tree});
                // Evaluate for immediate use (non-function-def path)
                Value val = kv.second;
                if (val.isExpression() && val.exprTree()) {
                    Value resolved = evaluate_expr_tree(val.exprTree(), let_bindings);
                    if (!resolved.isUndefined()) val = resolved;
                }
                let_bindings[kv.first] = val;
            }
        }
        // Restore saved ordered args
        g_ordered_args = g_ordered_args_stack.back();
        g_ordered_args_stack.pop_back();
        ExprNodePtr body_tree = $6->exprTree();
        if (!body_tree) {
            if ($6->isNumber()) body_tree = ExprNode::makeLiteral($6->toNumber());
            else if ($6->isVector()) {
                std::vector<ExprNodePtr> elems;
                for (size_t i = 0; i < $6->size(); i++) {
                    ExprNodePtr et = (*$6)[i].exprTree();
                    if (!et && (*$6)[i].isNumber()) et = ExprNode::makeLiteral((*$6)[i].toNumber());
                    elems.push_back(et ? et : ExprNode::makeLiteral(0));
                }
                body_tree = ExprNode::makeVectorLiteral(elems);
            }
        }
        // Try immediate evaluation
        if (body_tree) {
            if (g_in_function_def) {
                // During function definition, parameters are symbolic — don't eagerly evaluate.
                // Build a LetBinding ExprNode tree for deferred evaluation at call time.
                auto let_tree = ExprNode::makeLetBinding(let_pairs, body_tree);
                $$ = new Value(Value::expressionWithTree("0", let_tree));
            } else {
                Value result = evaluate_expr_tree(body_tree, let_bindings);
                if (result.isNumber()) {
                    $$ = new Value(result);
                    $$->setExprTree(ExprNode::makeLiteral(result.toNumber()));
                } else if (result.isVector()) {
                    $$ = new Value(result);
                } else if (result.isBool()) {
                    $$ = new Value(result);
                } else {
                    // Build LetBinding ExprNode
                    auto let_tree = ExprNode::makeLetBinding(let_pairs, body_tree);
                    $$ = new Value(Value::expressionWithTree("0", let_tree));
                }
            }
        } else {
            $$ = $6;
        }
        delete $4;
        if ($6 != $$) delete $6;
    }
    | TOK_EACH expr %prec UNARY {
        // each flattens — just pass through the expression
        $$ = $2;
    }
    | TOK_ASSERT '(' arguments ')' expr %prec UNARY {
        // assert() as expression — discard assertion, return trailing expr
        delete $3;
        $$ = $5;
    }
    | TOK_ECHO '(' arguments ')' expr %prec UNARY {
        // echo() as expression — discard echo, return trailing expr
        delete $3;
        $$ = $5;
    }
    | TOK_FUNCTION '(' parameter_list ')' expr {
        // Function literal (anonymous function) — discard, return body expr
        delete $3;
        $$ = $5;
    }
    | TOK_IF '(' expr ')' expr %prec TOK_ELSE {
        // Conditional expression (used in list comprehensions)
        ExprNodePtr cond_tree = $3->exprTree();
        if (!cond_tree) {
            if ($3->isNumber()) cond_tree = ExprNode::makeLiteral($3->toNumber());
            else if ($3->isBool()) cond_tree = ExprNode::makeLiteral($3->toBool() ? 1.0 : 0.0);
        }
        ExprNodePtr then_tree = $5->exprTree();
        if (!then_tree) {
            if ($5->isNumber()) then_tree = ExprNode::makeLiteral($5->toNumber());
            else if ($5->isVector()) {
                std::vector<ExprNodePtr> elems;
                for (size_t i = 0; i < $5->size(); i++) {
                    ExprNodePtr et = (*$5)[i].exprTree();
                    if (!et && (*$5)[i].isNumber()) et = ExprNode::makeLiteral((*$5)[i].toNumber());
                    elems.push_back(et ? et : ExprNode::makeLiteral(0));
                }
                then_tree = ExprNode::makeVectorLiteral(elems);
            }
        }
        auto cond_node = ExprNode::makeConditional(cond_tree, then_tree, nullptr);
        // Inside function/module bodies, conditionals with variable references
        // can't be reliably evaluated at parse time (the empty variable map
        // would resolve VarRefs to 0, giving wrong results for let-bound vars
        // like if(r&&grad) inside kreis()). Only defer when VarRefs are present.
        bool has_var_refs = cond_tree && cond_tree->hasVariableRefs();
        if (g_in_function_def && has_var_refs) {
            $$ = new Value(Value::expressionWithTree("0", cond_node));
        } else {
            // Try to evaluate
            std::map<std::string, Value> empty;
            Value cond_val = evaluate_expr_tree(cond_tree, empty);
            bool resolved = false;
            if (cond_val.isBool()) { resolved = true; }
            else if (cond_val.isNumber()) { resolved = true; }
            if (resolved) {
                bool is_true = cond_val.isBool() ? cond_val.toBool() : (cond_val.toNumber() != 0.0);
                if (is_true) {
                    Value result = evaluate_expr_tree(then_tree, empty);
                    if (!result.isUndefined()) {
                        $$ = new Value(result);
                    } else {
                        $$ = new Value(*$5);
                    }
                } else {
                    $$ = new Value(); // condition false, no else → undef (filtered in for-loops)
                }
            } else {
                $$ = new Value(Value::expressionWithTree("0", cond_node));
            }
        }
        delete $3; delete $5;
    }
    | TOK_IF '(' expr ')' expr TOK_ELSE expr %prec TOK_ELSE {
        // Conditional expression with else
        ExprNodePtr cond_tree = $3->exprTree();
        if (!cond_tree) {
            if ($3->isNumber()) cond_tree = ExprNode::makeLiteral($3->toNumber());
            else if ($3->isBool()) cond_tree = ExprNode::makeLiteral($3->toBool() ? 1.0 : 0.0);
        }
        ExprNodePtr then_tree = $5->exprTree();
        if (!then_tree) {
            if ($5->isNumber()) then_tree = ExprNode::makeLiteral($5->toNumber());
            else if ($5->isVector()) {
                std::vector<ExprNodePtr> elems;
                for (size_t i = 0; i < $5->size(); i++) {
                    ExprNodePtr et = (*$5)[i].exprTree();
                    if (!et && (*$5)[i].isNumber()) et = ExprNode::makeLiteral((*$5)[i].toNumber());
                    elems.push_back(et ? et : ExprNode::makeLiteral(0));
                }
                then_tree = ExprNode::makeVectorLiteral(elems);
            }
        }
        ExprNodePtr else_tree = $7->exprTree();
        if (!else_tree) {
            if ($7->isNumber()) else_tree = ExprNode::makeLiteral($7->toNumber());
            else if ($7->isVector()) {
                std::vector<ExprNodePtr> elems;
                for (size_t i = 0; i < $7->size(); i++) {
                    ExprNodePtr et = (*$7)[i].exprTree();
                    if (!et && (*$7)[i].isNumber()) et = ExprNode::makeLiteral((*$7)[i].toNumber());
                    elems.push_back(et ? et : ExprNode::makeLiteral(0));
                }
                else_tree = ExprNode::makeVectorLiteral(elems);
            }
        }
        auto cond_node = ExprNode::makeConditional(cond_tree, then_tree, else_tree);
        // Try to evaluate
        std::map<std::string, Value> empty;
        Value cond_val = evaluate_expr_tree(cond_tree, empty);
        bool resolved = false;
        if (cond_val.isBool()) { resolved = true; }
        else if (cond_val.isNumber()) { resolved = true; }
        if (resolved) {
            bool is_true = cond_val.isBool() ? cond_val.toBool() : (cond_val.toNumber() != 0.0);
            if (is_true) {
                Value result = evaluate_expr_tree(then_tree, empty);
                if (!result.isUndefined()) { $$ = new Value(result); }
                else { $$ = new Value(*$5); }
            } else {
                Value result = evaluate_expr_tree(else_tree, empty);
                if (!result.isUndefined()) { $$ = new Value(result); }
                else { $$ = new Value(*$7); }
            }
        } else {
            $$ = new Value(Value::expressionWithTree("0", cond_node));
        }
        delete $3; delete $5; delete $7;
    }
    | TOK_FOR '(' TOK_ID '=' expr ')' expr {
        // For-loop comprehension: [for(i=[0:n]) expr]
        // Build range ExprNode from the range expression
        ExprNodePtr range_tree = $5->exprTree();
        if (!range_tree) {
            if ($5->isRange()) {
                // Build range ExprNode from range bound expressions
                ExprNodePtr startExpr = $5->rangeStartExpr();
                ExprNodePtr endExpr = $5->rangeEndExpr();
                ExprNodePtr stepExpr = $5->rangeStepExpr();
                if (!startExpr) startExpr = ExprNode::makeLiteral($5->rangeStart());
                if (!endExpr) endExpr = ExprNode::makeLiteral($5->rangeEnd());
                if (!stepExpr) stepExpr = ExprNode::makeLiteral($5->rangeStep());
                // Use __range__(start, end, step) pseudo-function
                range_tree = ExprNode::makeFunctionCall("__range__", {startExpr, endExpr, stepExpr});
            } else if ($5->isNumber()) {
                range_tree = ExprNode::makeLiteral($5->toNumber());
            }
        }
        ExprNodePtr body_tree = $7->exprTree();
        if (!body_tree) {
            if ($7->isNumber()) body_tree = ExprNode::makeLiteral($7->toNumber());
            else if ($7->isVector()) {
                std::vector<ExprNodePtr> elems;
                for (size_t i = 0; i < $7->size(); i++) {
                    ExprNodePtr et = (*$7)[i].exprTree();
                    if (!et && (*$7)[i].isNumber()) et = ExprNode::makeLiteral((*$7)[i].toNumber());
                    elems.push_back(et ? et : ExprNode::makeLiteral(0));
                }
                body_tree = ExprNode::makeVectorLiteral(elems);
            }
        }
        auto for_tree = ExprNode::makeForLoop(*$3, range_tree, body_tree);
        // Try to evaluate immediately (but NOT if range has unresolved expression trees)
        Value range_val = *$5;
        bool has_unresolved_range = (range_val.isRange() &&
            (range_val.rangeStartExpr() || range_val.rangeEndExpr() || range_val.rangeStepExpr()) &&
            (!range_val.rangeStartExpr() || !range_val.rangeEndExpr()));  // partial resolution
        // Skip eager evaluation during function definition — defer to call-time evaluation
        if (g_in_function_def) {
            has_unresolved_range = true;
        }
        if (!has_unresolved_range && (range_val.isRange() || range_val.isVector())) {
            // Create a temporary binding-like approach: put range directly
            // We need a way to pass the range. Re-create the for_tree with a special literal.
            // Actually, let's just evaluate directly here.
            Vector results;
            bool can_eval = true;
            if (range_val.isRange()) {
                double start = range_val.rangeStart();
                double end = range_val.rangeEnd();
                double step = range_val.rangeStep();
                if (step == 0) step = 1;
                if ((step > 0 && start <= end) || (step < 0 && start >= end)) {
                    for (double i = start; (step > 0) ? (i <= end + 1e-10) : (i >= end - 1e-10); i += step) {
                        std::map<std::string, Value> bindings;
                        bindings[*$3] = Value(i);
                        Value body_val = evaluate_expr_tree(body_tree, bindings);
                        if (!body_val.isUndefined()) results.push_back(body_val);
                        else can_eval = false;
                    }
                }
            } else if (range_val.isVector()) {
                for (size_t i = 0; i < range_val.size(); i++) {
                    std::map<std::string, Value> bindings;
                    bindings[*$3] = range_val[i];
                    Value body_val = evaluate_expr_tree(body_tree, bindings);
                    if (!body_val.isUndefined()) results.push_back(body_val);
                    else can_eval = false;
                }
            }
            if (can_eval && !results.empty()) {
                $$ = new Value(results);
            } else {
                $$ = new Value(Value::expressionWithTree("0", for_tree));
            }
        } else {
            $$ = new Value(Value::expressionWithTree("0", for_tree));
        }
        delete $3; delete $5; delete $7;
    }
    | TOK_FOR '(' argument_list ')' expr {
        // Multi-variable for loop comprehension: for(i=R1, w=R2, ...) body
        // Collect iterators in insertion order from g_ordered_args
        struct ForIter { std::string var; Value range; };
        std::vector<ForIter> iters;
        // Use g_ordered_args for insertion order, filter to named keys in *$3
        for (auto& oa : g_ordered_args) {
            if (oa.first.size() > 0 && oa.first[0] != '_' && $3->count(oa.first)) {
                iters.push_back({oa.first, oa.second});
            }
        }
        // Fallback: if g_ordered_args didn't capture them, use map order
        if (iters.empty()) {
            for (auto& kv : *$3) {
                if (kv.first.size() > 0 && kv.first[0] != '_') {
                    iters.push_back({kv.first, kv.second});
                }
            }
        }
        // Build body expression tree
        ExprNodePtr body_tree = $5->exprTree();
        if (!body_tree) {
            if ($5->isNumber()) body_tree = ExprNode::makeLiteral($5->toNumber());
            else if ($5->isVector()) {
                std::vector<ExprNodePtr> elems;
                for (size_t i = 0; i < $5->size(); i++) {
                    ExprNodePtr et = (*$5)[i].exprTree();
                    if (!et && (*$5)[i].isNumber()) et = ExprNode::makeLiteral((*$5)[i].toNumber());
                    elems.push_back(et ? et : ExprNode::makeLiteral(0));
                }
                body_tree = ExprNode::makeVectorLiteral(elems);
            }
        }
        // Build nested ForLoop tree: for(a=R1) for(b=R2) for(c=R3) body
        // Start from innermost and wrap outward
        ExprNodePtr current = body_tree;
        for (int idx = (int)iters.size() - 1; idx >= 0; idx--) {
            ExprNodePtr range_tree = iters[idx].range.exprTree();
            if (!range_tree) {
                if (iters[idx].range.isRange()) {
                    ExprNodePtr startExpr = iters[idx].range.rangeStartExpr();
                    ExprNodePtr endExpr = iters[idx].range.rangeEndExpr();
                    ExprNodePtr stepExpr = iters[idx].range.rangeStepExpr();
                    if (!startExpr) startExpr = ExprNode::makeLiteral(iters[idx].range.rangeStart());
                    if (!endExpr) endExpr = ExprNode::makeLiteral(iters[idx].range.rangeEnd());
                    if (!stepExpr) stepExpr = ExprNode::makeLiteral(iters[idx].range.rangeStep());
                    range_tree = ExprNode::makeFunctionCall("__range__", {startExpr, endExpr, stepExpr});
                } else if (iters[idx].range.isNumber()) {
                    range_tree = ExprNode::makeLiteral(iters[idx].range.toNumber());
                }
            }
            current = ExprNode::makeForLoop(iters[idx].var, range_tree, current);
        }
        // Use first iterator for eager evaluation attempt
        std::string var = iters.empty() ? "i" : iters[0].var;
        Value range_val = iters.empty() ? Value(0.0) : iters[0].range;
        // Skip eager evaluation during function definition
        if (g_in_function_def || iters.size() > 1) {
            // Multi-iterator or function def: always defer to runtime
            $$ = new Value(Value::expressionWithTree("0", current));
        } else if (range_val.isRange() || range_val.isVector()) {
            // Single iterator: try to evaluate immediately
            Vector results;
            bool can_eval = true;
            if (range_val.isRange()) {
                double start = range_val.rangeStart();
                double end = range_val.rangeEnd();
                double step = range_val.rangeStep();
                if (step == 0) step = 1;
                if ((step > 0 && start <= end) || (step < 0 && start >= end)) {
                    for (double i = start; (step > 0) ? (i <= end + 1e-10) : (i >= end - 1e-10); i += step) {
                        std::map<std::string, Value> bindings;
                        bindings[var] = Value(i);
                        Value body_val = evaluate_expr_tree(body_tree, bindings);
                        if (!body_val.isUndefined()) results.push_back(body_val);
                        else can_eval = false;
                    }
                }
            } else if (range_val.isVector()) {
                for (size_t i = 0; i < range_val.size(); i++) {
                    std::map<std::string, Value> bindings;
                    bindings[var] = range_val[i];
                    Value body_val = evaluate_expr_tree(body_tree, bindings);
                    if (!body_val.isUndefined()) results.push_back(body_val);
                    else can_eval = false;
                }
            }
            if (can_eval && !results.empty()) {
                $$ = new Value(results);
            } else {
                $$ = new Value(Value::expressionWithTree("0", current));
            }
        } else {
            $$ = new Value(Value::expressionWithTree("0", current));
        }
        delete $3; delete $5;
    }
    ;

vector_expr:
    '[' ']' { $$ = new Value(Vector()); }
    | '[' argument_list ']' {
        Vector v;
        bool hasExprTrees = false;
        // Extract in numeric order to avoid lexicographic sorting of _10 etc
        for (size_t i = 0; ; i++) {
            auto it = $2->find("_" + std::to_string(i));
            if (it == $2->end()) break;
            v.push_back(it->second);
            if (it->second.exprTree() && it->second.exprTree()->hasVariableRefs()) hasExprTrees = true;
            else if (it->second.isExpression() && it->second.exprTree()) hasExprTrees = true;
        }
        // If vector contains a single for-comprehension expression, unwrap it
        // since [for(x=...) body] produces a flat list, not a nested vector
        if (v.size() == 1 && v[0].isExpression() && v[0].exprTree() &&
            v[0].exprTree()->kind == ExprNode::Kind::ForLoop) {
            $$ = new Value(v[0]);
        } else {
            $$ = new Value(v);
            if (hasExprTrees) {
                std::vector<ExprNodePtr> elems;
                for (size_t i = 0; i < v.size(); i++) {
                    ExprNodePtr et = v[i].exprTree();
                    if (!et && v[i].isNumber()) et = ExprNode::makeLiteral(v[i].toNumber());
                    elems.push_back(et ? et : ExprNode::makeLiteral(0));
                }
                $$->setExprTree(ExprNode::makeVectorLiteral(elems));
            }
        }
        delete $2;
    }
    | '[' argument_list ',' ']' {
        Vector v;
        bool hasExprTrees = false;
        for (size_t i = 0; ; i++) {
            auto it = $2->find("_" + std::to_string(i));
            if (it == $2->end()) break;
            v.push_back(it->second);
            if (it->second.isExpression() && it->second.exprTree()) hasExprTrees = true;
        }
        $$ = new Value(v);
        if (hasExprTrees) {
            std::vector<ExprNodePtr> elems;
            for (size_t i = 0; i < v.size(); i++) {
                ExprNodePtr et = v[i].exprTree();
                if (!et && v[i].isNumber()) et = ExprNode::makeLiteral(v[i].toNumber());
                elems.push_back(et ? et : ExprNode::makeLiteral(0));
            }
            $$->setExprTree(ExprNode::makeVectorLiteral(elems));
        }
        delete $2;
    }
    ;

%%

static bool g_suppress_parse_errors = false;

void yyerror(const char* s) {
    if (!g_suppress_parse_errors) {
        std::cerr << "Parse error at line " << yylineno << ": " << s << std::endl;
    }
}
