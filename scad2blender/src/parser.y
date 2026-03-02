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
#include <stack>
#include <cmath>
#include <algorithm>
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
};
static std::map<std::string, FunctionDef> g_function_table;

// Forward declarations
static Value evaluate_expr_tree(const ExprNodePtr& tree, const std::map<std::string, Value>& bindings);
static Value evaluate_function_call(const std::string& name, const std::vector<Value>& arg_vals,
                                     const std::map<std::string, Value>& bindings);

static Value evaluate_expr_tree(const ExprNodePtr& tree, const std::map<std::string, Value>& bindings) {
    if (!tree) return Value();

    switch (tree->kind) {
        case ExprNode::Kind::Literal:
            return Value(tree->literal_value);

        case ExprNode::Kind::VarRef: {
            // Check bindings first, then symbol table
            auto it = bindings.find(tree->var_name);
            if (it != bindings.end()) return it->second;
            Value sv = lookup_variable(tree->var_name);
            if (!sv.isUndefined()) return sv;
            // If it's still an expression with a tree, try to evaluate that
            return Value();
        }

        case ExprNode::Kind::UnaryOp: {
            Value operand = evaluate_expr_tree(tree->left, bindings);
            if (tree->op == ExprNode::Op::NEGATE) {
                if (operand.isNumber()) return Value(-operand.toNumber());
                if (operand.isVector()) {
                    Vector v;
                    for (size_t i = 0; i < operand.size(); i++) {
                        if (operand[i].isNumber())
                            v.push_back(Value(-operand[i].toNumber()));
                        else
                            v.push_back(operand[i]);
                    }
                    return Value(v);
                }
            }
            return operand;
        }

        case ExprNode::Kind::BinaryOp: {
            Value left = evaluate_expr_tree(tree->left, bindings);
            Value right = evaluate_expr_tree(tree->right, bindings);

            // Number x Number
            if (left.isNumber() && right.isNumber()) {
                double l = left.toNumber(), r = right.toNumber();
                switch (tree->op) {
                    case ExprNode::Op::ADD: return Value(l + r);
                    case ExprNode::Op::SUBTRACT: return Value(l - r);
                    case ExprNode::Op::MULTIPLY: return Value(l * r);
                    case ExprNode::Op::DIVIDE: return r != 0 ? Value(l / r) : Value();
                    case ExprNode::Op::MODULO: return r != 0 ? Value(std::fmod(l, r)) : Value();
                    case ExprNode::Op::POWER: return Value(std::pow(l, r));
                    default: return Value();
                }
            }
            // Scalar * Vector or Vector * Scalar
            if (tree->op == ExprNode::Op::MULTIPLY) {
                Value scalar, vec;
                if (left.isNumber() && right.isVector()) { scalar = left; vec = right; }
                else if (left.isVector() && right.isNumber()) { scalar = right; vec = left; }
                if (vec.isVector() && scalar.isNumber()) {
                    double s = scalar.toNumber();
                    Vector result;
                    for (size_t i = 0; i < vec.size(); i++) {
                        if (vec[i].isNumber()) result.push_back(Value(vec[i].toNumber() * s));
                        else result.push_back(vec[i]);
                    }
                    return Value(result);
                }
            }
            // Vector +/- Vector
            if ((tree->op == ExprNode::Op::ADD || tree->op == ExprNode::Op::SUBTRACT) &&
                left.isVector() && right.isVector()) {
                Vector result;
                size_t len = std::min(left.size(), right.size());
                for (size_t i = 0; i < len; i++) {
                    if (left[i].isNumber() && right[i].isNumber()) {
                        double v = (tree->op == ExprNode::Op::ADD)
                            ? left[i].toNumber() + right[i].toNumber()
                            : left[i].toNumber() - right[i].toNumber();
                        result.push_back(Value(v));
                    } else {
                        result.push_back(Value());
                    }
                }
                return Value(result);
            }
            return Value();
        }

        case ExprNode::Kind::FunctionCall: {
            std::vector<Value> arg_vals;
            for (const auto& arg : tree->func_args) {
                arg_vals.push_back(evaluate_expr_tree(arg, bindings));
            }
            return evaluate_function_call(tree->func_name, arg_vals, bindings);
        }

        case ExprNode::Kind::VectorLiteral: {
            Vector v;
            for (const auto& elem : tree->vec_elements) {
                v.push_back(evaluate_expr_tree(elem, bindings));
            }
            return Value(v);
        }
    }
    return Value();
}

static Value evaluate_function_call(const std::string& name, const std::vector<Value>& arg_vals,
                                     const std::map<std::string, Value>& bindings) {
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
    // Special case: cross(v1, v2) - 3D cross product
    if (name == "cross" && arg_vals.size() == 2 &&
        arg_vals[0].isVector() && arg_vals[1].isVector() &&
        arg_vals[0].size() >= 3 && arg_vals[1].size() >= 3) {
        double ax = arg_vals[0][0].toNumber(), ay = arg_vals[0][1].toNumber(), az = arg_vals[0][2].toNumber();
        double bx = arg_vals[1][0].toNumber(), by = arg_vals[1][1].toNumber(), bz = arg_vals[1][2].toNumber();
        return Value(Vector{Value(ay*bz - az*by), Value(az*bx - ax*bz), Value(ax*by - ay*bx)});
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
        for (size_t i = 0; i < fdef.params.size() && i < arg_vals.size(); i++) {
            new_bindings[fdef.params[i]] = arg_vals[i];
        }
        return evaluate_expr_tree(fdef.body, new_bindings);
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

// Entry point from grammar: try to fully evaluate a function call
static Value try_evaluate_function(const std::string& name, const Arguments& args) {
    // Extract ExprNode trees from positional args in numeric order
    auto arg_trees = args_to_expr_trees(args);

    // Build a list of arg values by evaluating trees
    std::map<std::string, Value> empty_bindings;
    std::vector<Value> arg_vals;
    for (const auto& tree : arg_trees) {
        arg_vals.push_back(evaluate_expr_tree(tree, empty_bindings));
    }

    Value result = evaluate_function_call(name, arg_vals, empty_bindings);

    // If we got a concrete result, return it
    if (result.isNumber()) {
        result.setExprTree(ExprNode::makeLiteral(result.toNumber()));
        return result;
    }
    if (result.isVector()) {
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
    // 1. Try relative to the referring file's directory
    if (!from_dir.empty()) {
        std::string path = from_dir + "/" + filename;
        FILE* test = fopen(path.c_str(), "r");
        if (test) { fclose(test); return path; }
    }
    // 2. Try each include path
    for (const auto& p : get_include_paths()) {
        std::string path = p + "/" + filename;
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
%token TOK_UNION TOK_DIFFERENCE TOK_INTERSECTION

/* Extrusions */
%token TOK_LINEAR_EXTRUDE TOK_ROTATE_EXTRUDE

/* Other modules */
%token TOK_HULL TOK_MINKOWSKI TOK_PROJECTION TOK_IMPORT TOK_SURFACE TOK_CHILDREN

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
%type <node_list> statements child_statements
%type <value> expr vector_expr
%type <args> arguments argument_list
%type <str_list> parameter_list
%type <str> func_name

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
            $$->push_back(ASTNodePtr($2));
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
    | TOK_ID '=' expr ';' {
        // Try to evaluate expression to a concrete value before storing
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
    | TOK_SPECIAL_VAR '=' expr ';' {
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
    | TOK_FOR '(' TOK_ID '=' expr ')' child_statement {
        auto node = new ForLoopNode(*$3, *$5);
        if ($7) node->addChild(ASTNodePtr($7));
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
    ;

module_stmt:
    TOK_MODULE TOK_ID '(' parameter_list ')' child_statement {
        auto node = new ModuleNode(*$2, *$4);
        // Store parameter defaults from symbol table into module node
        for (const auto& param : *$4) {
            Value v = lookup_variable(param);
            if (!v.isUndefined()) {
                node->setParameterDefault(param, v);
            }
        }
        if ($6) node->addChild(ASTNodePtr($6));
        $$ = node;
        delete $2;
        delete $4;
    }
    | TOK_MODULE TOK_ID '(' parameter_list ')' {
        // Module with no body (empty module)
        auto node = new ModuleNode(*$2, *$4);
        for (const auto& param : *$4) {
            Value v = lookup_variable(param);
            if (!v.isUndefined()) {
                node->setParameterDefault(param, v);
            }
        }
        $$ = node;
        delete $2;
        delete $4;
    }
    ;

function_stmt:
    TOK_FUNCTION func_name '(' parameter_list ')' '=' expr ';' {
        FunctionDef fdef;
        fdef.params = *$4;
        fdef.body = $7->exprTree();
        if (!fdef.body && $7->isNumber())
            fdef.body = ExprNode::makeLiteral($7->toNumber());
        if (!fdef.body) {
            // Handle vector-valued function bodies
            if ($7->isVector()) {
                std::vector<ExprNodePtr> elems;
                for (size_t i = 0; i < $7->size(); i++) {
                    ExprNodePtr et = (*$7)[i].exprTree();
                    if (!et && (*$7)[i].isNumber()) et = ExprNode::makeLiteral((*$7)[i].toNumber());
                    elems.push_back(et ? et : ExprNode::makeLiteral(0));
                }
                fdef.body = ExprNode::makeVectorLiteral(elems);
            }
        }
        g_function_table[*$2] = fdef;
        auto fn = new FunctionNode(*$2, *$4);
        fn->setBody(fdef.body);
        $$ = fn;
        delete $2; delete $4; delete $7;
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

parameter_list:
    /* empty */ { $$ = new std::vector<std::string>(); }
    | TOK_ID {
        $$ = new std::vector<std::string>();
        $$->push_back(*$1);
        delete $1;
    }
    | TOK_ID '=' expr {
        $$ = new std::vector<std::string>();
        $$->push_back(*$1);
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
        delete $1;
        delete $3;
    }
    | parameter_list ',' TOK_ID {
        $$ = $1;
        $$->push_back(*$3);
        delete $3;
    }
    | parameter_list ',' TOK_ID '=' expr {
        $$ = $1;
        $$->push_back(*$3);
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
    | TOK_MINKOWSKI '(' ')' {
        $$ = new TransformNode(ASTNode::Type::Minkowski, Arguments());
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
        delete $1;
    }
    | TOK_ID '=' expr {
        $$ = new Arguments();
        (*$$)[*$1] = *$3;
        delete $1;
        delete $3;
    }
    | TOK_SPECIAL_VAR '=' expr {
        $$ = new Arguments();
        (*$$)[*$1] = *$3;
        delete $1;
        delete $3;
    }
    | TOK_SCALE '=' expr {
        $$ = new Arguments();
        (*$$)["scale"] = *$3;
        delete $3;
    }
    | argument_list ',' expr {
        $$ = $1;
        size_t idx = $$->size();
        // Find next available positional index
        while ($$->find("_" + std::to_string(idx)) != $$->end()) idx++;
        (*$$)["_" + std::to_string(idx)] = *$3;
        delete $3;
    }
    | argument_list ',' TOK_ID '=' expr {
        $$ = $1;
        (*$$)[*$3] = *$5;
        delete $3;
        delete $5;
    }
    | argument_list ',' TOK_SPECIAL_VAR '=' expr {
        $$ = $1;
        (*$$)[*$3] = *$5;
        delete $3;
        delete $5;
    }
    | argument_list ',' TOK_SCALE '=' expr {
        $$ = $1;
        (*$$)["scale"] = *$5;
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
        // Always store as expression to support linking to group inputs
        // The code generator will resolve whether to use default_value or link
        auto tree = ExprNode::makeVarRef(*$1);
        $$ = new Value(Value::expressionWithTree(*$1, tree));
        delete $1;
    }
    | TOK_SPECIAL_VAR {
        // Always store as expression to support linking to group inputs
        auto tree = ExprNode::makeVarRef(*$1);
        $$ = new Value(Value::expressionWithTree(*$1, tree));
        delete $1;
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
            auto ltree = $1->exprTree() ? $1->exprTree() : ExprNode::makeLiteral($1->toNumber());
            auto rtree = $3->exprTree() ? $3->exprTree() : ExprNode::makeLiteral($3->toNumber());
            auto tree = ExprNode::makeBinary(ExprNode::Op::ADD, ltree, rtree);
            $$ = new Value(Value::expressionWithTree("(" + $1->toPython() + " + " + $3->toPython() + ")", tree));
        } else if ($1->isNumber() && $3->isNumber()) {
            $$ = new Value($1->toNumber() + $3->toNumber());
            $$->setExprTree(ExprNode::makeLiteral($1->toNumber() + $3->toNumber()));
        } else if ($1->isVector() && $3->isVector()) {
            Vector v;
            size_t len = std::min($1->size(), $3->size());
            for (size_t i = 0; i < len; i++) {
                if ((*$1)[i].isNumber() && (*$3)[i].isNumber())
                    v.push_back(Value((*$1)[i].toNumber() + (*$3)[i].toNumber()));
                else v.push_back(Value());
            }
            $$ = new Value(v);
        } else {
            $$ = new Value();
        }
        delete $1; delete $3;
    }
    | expr '-' expr {
        if ($1->isExpression() || $3->isExpression()) {
            auto ltree = $1->exprTree() ? $1->exprTree() : ExprNode::makeLiteral($1->toNumber());
            auto rtree = $3->exprTree() ? $3->exprTree() : ExprNode::makeLiteral($3->toNumber());
            auto tree = ExprNode::makeBinary(ExprNode::Op::SUBTRACT, ltree, rtree);
            $$ = new Value(Value::expressionWithTree("(" + $1->toPython() + " - " + $3->toPython() + ")", tree));
        } else if ($1->isNumber() && $3->isNumber()) {
            $$ = new Value($1->toNumber() - $3->toNumber());
            $$->setExprTree(ExprNode::makeLiteral($1->toNumber() - $3->toNumber()));
        } else if ($1->isVector() && $3->isVector()) {
            Vector v;
            size_t len = std::min($1->size(), $3->size());
            for (size_t i = 0; i < len; i++) {
                if ((*$1)[i].isNumber() && (*$3)[i].isNumber())
                    v.push_back(Value((*$1)[i].toNumber() - (*$3)[i].toNumber()));
                else v.push_back(Value());
            }
            $$ = new Value(v);
        } else {
            $$ = new Value();
        }
        delete $1; delete $3;
    }
    | expr '*' expr {
        if ($1->isExpression() || $3->isExpression()) {
            auto ltree = $1->exprTree() ? $1->exprTree() : ExprNode::makeLiteral($1->toNumber());
            auto rtree = $3->exprTree() ? $3->exprTree() : ExprNode::makeLiteral($3->toNumber());
            auto tree = ExprNode::makeBinary(ExprNode::Op::MULTIPLY, ltree, rtree);
            $$ = new Value(Value::expressionWithTree("(" + $1->toPython() + " * " + $3->toPython() + ")", tree));
        } else if ($1->isNumber() && $3->isNumber()) {
            $$ = new Value($1->toNumber() * $3->toNumber());
            $$->setExprTree(ExprNode::makeLiteral($1->toNumber() * $3->toNumber()));
        } else if ($1->isNumber() && $3->isVector()) {
            double s = $1->toNumber();
            Vector v;
            for (size_t i = 0; i < $3->size(); i++) {
                if ((*$3)[i].isNumber()) v.push_back(Value((*$3)[i].toNumber() * s));
                else v.push_back((*$3)[i]);
            }
            $$ = new Value(v);
        } else if ($1->isVector() && $3->isNumber()) {
            double s = $3->toNumber();
            Vector v;
            for (size_t i = 0; i < $1->size(); i++) {
                if ((*$1)[i].isNumber()) v.push_back(Value((*$1)[i].toNumber() * s));
                else v.push_back((*$1)[i]);
            }
            $$ = new Value(v);
        } else {
            $$ = new Value();
        }
        delete $1; delete $3;
    }
    | expr '/' expr {
        if ($1->isExpression() || $3->isExpression()) {
            auto ltree = $1->exprTree() ? $1->exprTree() : ExprNode::makeLiteral($1->toNumber());
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
        if ($2->isExpression()) {
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
            $$ = new Value(Value::expression("(not " + $2->toPython() + ")"));
        } else if ($2->isBool()) {
            $$ = new Value(!$2->toBool());
        } else {
            $$ = new Value();
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
            $$ = new Value();
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
            $$ = new Value();
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
            $$ = new Value();
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
            $$ = new Value();
        }
        delete $1; delete $3;
    }
    | expr TOK_EQ expr {
        $$ = new Value(false); // Simplified
        delete $1; delete $3;
    }
    | expr TOK_NE expr {
        $$ = new Value(true); // Simplified
        delete $1; delete $3;
    }
    | expr TOK_AND expr {
        $$ = new Value($1->toBool() && $3->toBool());
        delete $1; delete $3;
    }
    | expr TOK_OR expr {
        $$ = new Value($1->toBool() || $3->toBool());
        delete $1; delete $3;
    }
    | expr '?' expr ':' expr {
        // Try to resolve variable reference for ternary condition
        Value condVal = *$1;
        if ($1->isExpression()) {
            Value resolved = lookup_variable($1->toString());
            if (!resolved.isUndefined()) {
                condVal = resolved;
            }
        }
        bool cond = condVal.toBool();
        delete $1;
        if (cond) { $$ = $3; delete $5; }
        else      { $$ = $5; delete $3; }
    }
    | expr '[' expr ']' {
        if ($1->isVector() && $3->isNumber()) {
            size_t idx = static_cast<size_t>($3->toNumber());
            if (idx < $1->size()) {
                $$ = new Value($1->toVector()[idx]);
            } else {
                $$ = new Value();
            }
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
        } else {
            // Store as expression for unresolved cases
            auto tree = ExprNode::makeVarRef($1->toPython() + "." + *$3);
            $$ = new Value(Value::expressionWithTree($1->toPython() + "." + *$3, tree));
        }
        delete $1; delete $3;
    }
    | '[' expr ':' expr ']' {
        // Range [start:end]
        $$ = new Value();
        double start_val = 0, end_val = 0;
        bool start_ok = false, end_ok = false;
        if ($2->isNumber()) { start_val = $2->toNumber(); start_ok = true; }
        else if ($2->isExpression() && $2->exprTree()) {
            std::map<std::string, Value> empty;
            Value r = evaluate_expr_tree($2->exprTree(), empty);
            if (r.isNumber()) { start_val = r.toNumber(); start_ok = true; }
        }
        if ($4->isNumber()) { end_val = $4->toNumber(); end_ok = true; }
        else if ($4->isExpression() && $4->exprTree()) {
            std::map<std::string, Value> empty;
            Value r = evaluate_expr_tree($4->exprTree(), empty);
            if (r.isNumber()) { end_val = r.toNumber(); end_ok = true; }
        }
        if (start_ok && end_ok) {
            *$$ = Value::range(start_val, end_val);
        }
        delete $2; delete $4;
    }
    | '[' expr ':' expr ':' expr ']' {
        // Range [start:step:end]
        $$ = new Value();
        double start_val = 0, step_val = 0, end_val = 0;
        bool start_ok = false, step_ok = false, end_ok = false;
        if ($2->isNumber()) { start_val = $2->toNumber(); start_ok = true; }
        else if ($2->isExpression() && $2->exprTree()) {
            std::map<std::string, Value> empty;
            Value r = evaluate_expr_tree($2->exprTree(), empty);
            if (r.isNumber()) { start_val = r.toNumber(); start_ok = true; }
        }
        if ($4->isNumber()) { step_val = $4->toNumber(); step_ok = true; }
        else if ($4->isExpression() && $4->exprTree()) {
            std::map<std::string, Value> empty;
            Value r = evaluate_expr_tree($4->exprTree(), empty);
            if (r.isNumber()) { step_val = r.toNumber(); step_ok = true; }
        }
        if ($6->isNumber()) { end_val = $6->toNumber(); end_ok = true; }
        else if ($6->isExpression() && $6->exprTree()) {
            std::map<std::string, Value> empty;
            Value r = evaluate_expr_tree($6->exprTree(), empty);
            if (r.isNumber()) { end_val = r.toNumber(); end_ok = true; }
        }
        if (start_ok && step_ok && end_ok) {
            *$$ = Value::range(start_val, end_val, step_val);
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

void yyerror(const char* s) {
    std::cerr << "Parse error at line " << yylineno << ": " << s << std::endl;
}
