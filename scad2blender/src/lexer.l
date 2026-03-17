/**
 * @file lexer.l
 * @brief Flex lexer for OpenSCAD
 *
 * Based on OpenSCAD's lexer, simplified for scad2blender.
 */

%{
#include <string>
#include <cstdlib>
#include <stack>
#include <vector>
#include <libgen.h>
#include "ast.h"
#include "parser.h"

using namespace scad2blender;

// String buffer for building strings
static std::string string_buffer;

// Include file stack
struct IncludeState {
    YY_BUFFER_STATE buffer;
    std::string file_dir;
    int lineno;
};
static std::stack<IncludeState> include_stack;
static std::string include_filename;
static std::vector<std::string> include_paths;
static std::string current_file_dir;

void set_include_paths(const std::vector<std::string>& paths) {
    include_paths = paths;
}

void set_current_file_dir(const std::string& dir) {
    current_file_dir = dir;
}

const std::vector<std::string>& get_include_paths() {
    return include_paths;
}

const std::string& get_current_file_dir() {
    return current_file_dir;
}

%}

%option noyywrap
%option yylineno

%x COMMENT
%x STRING
%x INCL

DIGIT       [0-9]
LETTER      [a-zA-Z_]
ID          {LETTER}({LETTER}|{DIGIT})*
NUMBER      {DIGIT}+("."{DIGIT}*)?([eE][+-]?{DIGIT}+)?|"."{DIGIT}+([eE][+-]?{DIGIT}+)?
WS          [ \t\r]+

%%

 /* Include/Use directives - must be before keyword rules */
"include"[ \t]*"<"     { include_filename.clear(); BEGIN(INCL); }
"include"[ \t]*\"      { include_filename.clear(); BEGIN(INCL); }
"use"[ \t]*"<"         { include_filename.clear(); BEGIN(INCL); }
"use"[ \t]*\"          { include_filename.clear(); BEGIN(INCL); }

<INCL>[^>""\n]+        { include_filename += yytext; }
<INCL>[>"]             {
    BEGIN(INITIAL);
    FILE* f = nullptr;
    std::string resolved_path;

    /* 1. Try each include path first (allows filtered library overrides) */
    for (const auto& path : include_paths) {
        resolved_path = path + "/" + include_filename;
        f = fopen(resolved_path.c_str(), "r");
        if (f) break;
    }
    /* 2. Try relative to current file's directory */
    if (!f && !current_file_dir.empty()) {
        resolved_path = current_file_dir + "/" + include_filename;
        f = fopen(resolved_path.c_str(), "r");
    }
    /* 3. Try as absolute path */
    if (!f) {
        resolved_path = include_filename;
        f = fopen(resolved_path.c_str(), "r");
    }

    if (!f) {
        fprintf(stderr, "Warning: Cannot open include file: %s\n", include_filename.c_str());
    } else {
        /* Push current buffer state */
        IncludeState state;
        state.buffer = YY_CURRENT_BUFFER;
        state.file_dir = current_file_dir;
        state.lineno = yylineno;
        include_stack.push(state);

        /* Update current file directory */
        char* path_copy = strdup(resolved_path.c_str());
        current_file_dir = dirname(path_copy);
        free(path_copy);

        /* Switch to new buffer */
        yy_switch_to_buffer(yy_create_buffer(f, YY_BUF_SIZE));
        yylineno = 1;
    }
}
<INCL>\n               { /* skip newlines in include filename */ }
<INCL><<EOF>>          { fprintf(stderr, "Error: Unexpected EOF in include directive\n"); BEGIN(INITIAL); yyterminate(); }

 /* Comments */
"//".*                      { /* Single line comment - ignore */ }
"/*"                        { BEGIN(COMMENT); }
<COMMENT>"*/"               { BEGIN(INITIAL); }
<COMMENT>.                  { /* Ignore comment content */ }
<COMMENT>\n                 { /* Ignore newlines in comments */ }

 /* Strings */
\"                          { string_buffer.clear(); BEGIN(STRING); }
<STRING>\"                  { BEGIN(INITIAL); yylval.str = new std::string(string_buffer); return TOK_STRING; }
<STRING>\\n                 { string_buffer += '\n'; }
<STRING>\\t                 { string_buffer += '\t'; }
<STRING>\\\\                { string_buffer += '\\'; }
<STRING>\\\"                { string_buffer += '"'; }
<STRING>.                   { string_buffer += yytext[0]; }
<STRING>\n                  { string_buffer += '\n'; }

 /* Keywords */
"module"                    { return TOK_MODULE; }
"function"                  { return TOK_FUNCTION; }
"if"                        { return TOK_IF; }
"else"                      { return TOK_ELSE; }
"for"                       { return TOK_FOR; }
"let"                       { return TOK_LET; }
"each"                      { return TOK_EACH; }
"assert"                    { return TOK_ASSERT; }
"echo"                      { return TOK_ECHO; }
"true"                      { yylval.boolean = true; return TOK_TRUE; }
"false"                     { yylval.boolean = false; return TOK_FALSE; }
"undef"                     { return TOK_UNDEF; }

 /* Built-in modules - Primitives */
"cube"                      { return TOK_CUBE; }
"sphere"                    { return TOK_SPHERE; }
"cylinder"                  { return TOK_CYLINDER; }
"polyhedron"                { return TOK_POLYHEDRON; }
"circle"                    { return TOK_CIRCLE; }
"square"                    { return TOK_SQUARE; }
"polygon"                   { return TOK_POLYGON; }
"text"                      { return TOK_TEXT; }

 /* Built-in modules - Transforms */
"translate"                 { return TOK_TRANSLATE; }
"rotate"                    { return TOK_ROTATE; }
"scale"                     { return TOK_SCALE; }
"mirror"                    { return TOK_MIRROR; }
"color"                     { return TOK_COLOR; }
"offset"                    { return TOK_OFFSET; }
"resize"                    { return TOK_RESIZE; }
"multmatrix"                { return TOK_MULTMATRIX; }

 /* Built-in modules - Boolean operations */
"union"                     { return TOK_UNION; }
"difference"                { return TOK_DIFFERENCE; }
"intersection_for"          { return TOK_INTERSECTION_FOR; }
"intersection"              { return TOK_INTERSECTION; }

 /* Built-in modules - Extrusions */
"linear_extrude"            { return TOK_LINEAR_EXTRUDE; }
"rotate_extrude"            { return TOK_ROTATE_EXTRUDE; }

 /* Built-in modules - Other */
"hull"                      { return TOK_HULL; }
"minkowski"                 { return TOK_MINKOWSKI; }
"roof"                      { return TOK_ROOF; }
"projection"                { return TOK_PROJECTION; }
"import"                    { return TOK_IMPORT; }
"surface"                   { return TOK_SURFACE; }
"children"                  { return TOK_CHILDREN; }

 /* Special variables - any identifier starting with $ */
"$"{ID}                     { yylval.str = new std::string(yytext); return TOK_SPECIAL_VAR; }

 /* Operators */
"&&"                        { return TOK_AND; }
"||"                        { return TOK_OR; }
"=="                        { return TOK_EQ; }
"!="                        { return TOK_NE; }
"<="                        { return TOK_LE; }
">="                        { return TOK_GE; }

 /* Numbers */
{NUMBER}                    { yylval.number = std::stod(yytext); return TOK_NUMBER; }

 /* Identifiers */
{ID}                        { yylval.str = new std::string(yytext); return TOK_ID; }

 /* Single character tokens */
"+"                         { return '+'; }
"-"                         { return '-'; }
"*"                         { return '*'; }
"/"                         { return '/'; }
"%"                         { return '%'; }
"^"                         { return '^'; }
"<"                         { return '<'; }
">"                         { return '>'; }
"!"                         { return '!'; }
"#"                         { return '#'; }
"="                         { return '='; }
";"                         { return ';'; }
","                         { return ','; }
":"                         { return ':'; }
"?"                         { return '?'; }
"."                         { return '.'; }
"("                         { return '('; }
")"                         { return ')'; }
"["                         { return '['; }
"]"                         { return ']'; }
"{"                         { return '{'; }
"}"                         { return '}'; }

 /* Whitespace */
{WS}                        { /* Ignore whitespace */ }
\n                          { /* Ignore newlines */ }

 /* Unknown character */
.                           { fprintf(stderr, "Unknown character: %c\n", yytext[0]); }

 /* EOF handling for include stack */
<INITIAL,COMMENT,STRING><<EOF>> {
    if (!include_stack.empty()) {
        IncludeState state = include_stack.top();
        include_stack.pop();
        yy_delete_buffer(YY_CURRENT_BUFFER);
        yy_switch_to_buffer(state.buffer);
        current_file_dir = state.file_dir;
        yylineno = state.lineno;
    } else {
        yyterminate();
    }
}

%%
