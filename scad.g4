grammar scad;		
prog:	(WS* scope+ WS)+ EOF ;

module:	moduleprefix STR WS* '(' WS* expr* WS* ')' WS* moduleBody+ ;
moduleCall:	STR WS* '(' WS* expr* ')' WS* ';' WS*;

moduleprefix : 'module';
expr:	expr WS* ('*'|'/') WS* expr
    |	expr WS* ('+'|'-') WS* expr
    |	'(' WS* expr WS* ')'
    |	'[' WS* expr WS* ','* WS* ']'
    |	WS* ('*'|'/') WS* expr
    |	WS* ('+'|'-') WS* expr
    |	NUMBER
    |   STR
    ;
NEW_LINE
        : '\r\n' | '\r' | '\n'
        | '\u0085' // <Next Line CHARACTER (U+0085)>'
        | '\u2028' //'<Line Separator CHARACTER (U+2028)>'
        | '\u2029' //'<Paragraph Separator CHARACTER (U+2029)>'
        ;

INTEGER:        [0-9];
INTEGER_HEXA:   '0' [xX] '_'* [0-9a-fA-F];
INTEGER_OCTAL:  '0' [oO] '_'* [0-7];
INTEGER_BINARY: '0' [bB] '_'* [0-1];
FLOAT: [0-9] ( ([eE] [-+]? [0-9]) | '.' [0-9] ([eE] [-+]? [0-9])?);

NUMBER     : INTEGER
    |        INTEGER_HEXA
    |        INTEGER_OCTAL
    |        INTEGER_BINARY
    |        FLOAT;

STR  : [a-z_]+
    |  [A-Z]+
    |  [0-9]+;

WS      : (' ' | '\t' | NEW_LINE)+ -> skip;

transparent : '#';
bodyLine : (transparent WS)* (expr WS*)+ ';';
assignmentLine : STR WS* '=' WS* (expr WS* ','* WS*)+ ';';

scope   : moduleCall
    |     module;

COMMENT:            '//' ( ~[/\r\n] ~[\r\n]* )? -> skip;

body    : assignmentLine
    |     bodyLine
    |     COMMENT;

moduleBody: '{' (WS* body WS*)+ '}';