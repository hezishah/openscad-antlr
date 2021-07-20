import sys
from antlr4 import *
from scad_py.scadLexer import scadLexer
from scad_py.scadParser import scadParser
from scad_py.scadListener import scadListener

class KeyPrinter(scadListener):     
    variables = {}
    def enterExpr(self, ctx):         
        print("Oh, an expr!")
    def enterAssignmentLine(self, ctx):         
        print("Oh, an assignment!")
        try:
            self.variables[ctx.children[0].symbol.text] = eval(ctx.children[2].children[0].symbol.text, self.variables)
        except Exception as e:
            print(e)

def main(argv):
    input_stream = FileStream(argv[1])
    lexer = scadLexer(input_stream)
    stream = CommonTokenStream(lexer)
    parser = scadParser(stream)
    tree = parser.prog()
    printer = KeyPrinter()
    walker = ParseTreeWalker()
    walker.walk(printer, tree)
    print(printer.variables)
if __name__ == '__main__':
    main(sys.argv)