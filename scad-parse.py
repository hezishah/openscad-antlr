import sys
from antlr4 import *
from scad_py.scadLexer import scadLexer
from scad_py.scadParser import scadParser
from scad_py.scadListener import scadListener

class KeyPrinter(scadListener):     
    def enterProg(self, ctx):         
        print("Oh, a key!") 

def main(argv):
    input_stream = FileStream(argv[1])
    lexer = scadLexer(input_stream)
    stream = CommonTokenStream(lexer)
    parser = scadParser(stream)
    tree = parser.prog()
    printer = KeyPrinter()
    walker = ParseTreeWalker()
    walker.walk(printer, tree)
if __name__ == '__main__':
    main(sys.argv)