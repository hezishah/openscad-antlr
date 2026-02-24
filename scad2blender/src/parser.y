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
%type <node> program statement module_stmt module_instantiation
%type <node> single_module_instantiation child_statement
%type <node> primitive_call transform_call boolean_call extrude_call other_call
%type <node> if_statement
%type <node_list> statements child_statements
%type <value> expr vector_expr
%type <args> arguments argument_list
%type <str_list> parameter_list

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
    | module_stmt { $$ = $1; }
    | module_instantiation child_statement {
        if ($1 && $2) {
            $1->addChild(ASTNodePtr($2));
        }
        $$ = $1;
    }
    | module_instantiation ';' { $$ = $1; }
    | TOK_ID '=' expr ';' {
        // Store in symbol table for later lookups
        set_variable(*$1, *$3);
        $$ = new AssignmentNode(*$1, *$3);
        delete $1;
        delete $3;
    }
    | TOK_SPECIAL_VAR '=' expr ';' {
        set_variable(*$1, *$3);
        $$ = new AssignmentNode(*$1, *$3);
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
        if ($6) node->addChild(ASTNodePtr($6));
        $$ = node;
        delete $2;
        delete $4;
    }
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
        if ($1->isNumber() && $3->isNumber()) {
            $$ = new Value($1->toNumber() < $3->toNumber());
        } else {
            $$ = new Value();
        }
        delete $1; delete $3;
    }
    | expr '>' expr {
        if ($1->isNumber() && $3->isNumber()) {
            $$ = new Value($1->toNumber() > $3->toNumber());
        } else {
            $$ = new Value();
        }
        delete $1; delete $3;
    }
    | expr TOK_LE expr {
        if ($1->isNumber() && $3->isNumber()) {
            $$ = new Value($1->toNumber() <= $3->toNumber());
        } else {
            $$ = new Value();
        }
        delete $1; delete $3;
    }
    | expr TOK_GE expr {
        if ($1->isNumber() && $3->isNumber()) {
            $$ = new Value($1->toNumber() >= $3->toNumber());
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
        bool cond = $1->toBool();
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
    | '[' expr ':' expr ']' {
        // Range [start:end]
        $$ = new Value();
        if ($2->isNumber() && $4->isNumber()) {
            *$$ = Value::range($2->toNumber(), $4->toNumber());
        }
        delete $2; delete $4;
    }
    | '[' expr ':' expr ':' expr ']' {
        // Range [start:step:end]
        $$ = new Value();
        if ($2->isNumber() && $4->isNumber() && $6->isNumber()) {
            *$$ = Value::range($2->toNumber(), $6->toNumber(), $4->toNumber());
        }
        delete $2; delete $4; delete $6;
    }
    | TOK_ID '(' arguments ')' {
        // Function call - return undefined for now
        $$ = new Value();
        delete $1; delete $3;
    }
    ;

vector_expr:
    '[' ']' { $$ = new Value(Vector()); }
    | '[' argument_list ']' {
        Vector v;
        for (const auto& pair : *$2) {
            v.push_back(pair.second);
        }
        $$ = new Value(v);
        delete $2;
    }
    | '[' argument_list ',' ']' {
        Vector v;
        for (const auto& pair : *$2) {
            v.push_back(pair.second);
        }
        $$ = new Value(v);
        delete $2;
    }
    ;

%%

void yyerror(const char* s) {
    std::cerr << "Parse error at line " << yylineno << ": " << s << std::endl;
}
