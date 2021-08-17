import sys
from antlr4 import *
from scad_py.scadLexer import scadLexer
from scad_py.scadParser import scadParser
from scad_py.scadListener import scadListener

class KeyPrinter(scadListener):     
    variables = {}
    ident = 0
    contextStr = ""
    modules = []
    primitives = []
    transparent = False
    insideModule = False
    primitiveName = ""
    insideArgs = 0

    def identStr(self):
        return '    ' * self.ident
        # Enter a parse tree produced by scadParser#module.

    def enterModule(self, ctx:scadParser.ModuleContext):
        self.contextStr = "def "+ctx.children[1].symbol.text+"():\n"
        self.ident += 1
        self.insideModule = True
    def exitModule(self, ctx:scadParser.ModuleContext):
        if(self.ident > 0):
            self.ident -= 1
            self.modules.append(self.contextStr)
            self.contextStr = ""
            self.insideModule = False
        else:
            print("Error - Ident incorrect")
    def enterFunction(self, ctx:scadParser.FunctionContext):
        self.contextStr = "def "+ctx.children[1].symbol.text+"():\n"
        self.ident += 1
        self.contextStr += self.identStr() + "return \n"
        self.insideModule = True
    def exitFunction(self, ctx:scadParser.FunctionContext):
        if(self.ident > 0):
            self.ident -= 1
            self.modules.append(self.contextStr)
            self.contextStr = ""
            self.insideModule = False
        else:
            print("Error - Ident incorrect")
    def enterPrimitive(self, ctx: scadParser.PrimitiveContext):
        if not ctx.start.text.startswith('#'):
            self.primitiveName = ctx.start.text
            self.contextStr += self.identStr() + self.primitiveName + "(\n"
            self.ident += 1
        else:
            self.primitiveName = ctx.children[1].symbol.text
    def exitIsTransparent(self, ctx: scadParser.IsTransparentContext):
        self.transparent = True
        self.contextStr += self.identStr() + self.primitiveName + "(\n"
        self.ident += 1
    def exitPrimitive(self, ctx: scadParser.PrimitiveContext):
        if self.transparent:
            self.transparent = False
            self.contextStr += self.identStr() + "transparent = True\n"
        self.ident -= 1
        self.contextStr += self.identStr()
        self.contextStr += "),"
        if self.insideArgs:
            self.contextStr += ','
        self.contextStr += "\n"
        if not self.insideModule:
            self.primitives.append(self.contextStr)
            self.contextStr = ""
        self.primitiveName = ""
    def enterPrimitiveArgs(self, ctx):
        self.insideArgs += 1
    def exitPrimitiveArgs(self, ctx):
        self.insideArgs -= 1
        t = ctx.getText()
        if len(t) and t!=',':
            self.contextStr += self.identStr() + t.replace(';','') + ",\n"
    def exitAssignment(self, ctx: scadParser.AssignmentContext):
        t = ctx.getText()
        t = t.replace('true', 'True')
        t = t.replace('false', 'False')
        if True: #not self.insideArgs:
            self.contextStr += self.identStr() + t.replace(';','') + "\n"
    def exitArgs(self, ctx: scadParser.ArgsContext):
        t = ctx.getText()
        t = t.replace('true', 'True')
        t = t.replace('false', 'False')
        self.contextStr += self.identStr() + t
        if not t.endswith('='):
            self.contextStr += ",\n"
        else:
            self.contextStr += "\n"
def main(argv):
    input_stream = FileStream(argv[1])
    lexer = scadLexer(input_stream)
    stream = CommonTokenStream(lexer)
    parser = scadParser(stream)
    tree = parser.compilationUnit()
    printer = KeyPrinter()
    walker = ParseTreeWalker()
    walker.walk(printer, tree)
    print("".join(printer.modules))
    print("".join(printer.primitives))
    print(printer.contextStr)
    #print(printer.variables)
if __name__ == '__main__':
    main(sys.argv)