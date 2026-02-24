/**
 * @file lexer.l
 * @brief Flex lexer for OpenSCAD
 *
 * Based on OpenSCAD's lexer, simplified for scad2blender.
 */

%{
#include <string>
#include <cstdlib>
#include "ast.h"
#include "parser.h"

using namespace scad2blender;

// String buffer for building strings
static std::string string_buffer;

%}

%option noyywrap
%option yylineno

%x COMMENT
%x STRING

DIGIT       [0-9]
LETTER      [a-zA-Z_]
ID          {LETTER}({LETTER}|{DIGIT})*
NUMBER      {DIGIT}+("."{DIGIT}+)?([eE][+-]?{DIGIT}+)?
WS          [ \t\r]+

%%

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
"intersection"              { return TOK_INTERSECTION; }

 /* Built-in modules - Extrusions */
"linear_extrude"            { return TOK_LINEAR_EXTRUDE; }
"rotate_extrude"            { return TOK_ROTATE_EXTRUDE; }

 /* Built-in modules - Other */
"hull"                      { return TOK_HULL; }
"minkowski"                 { return TOK_MINKOWSKI; }
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

%%
