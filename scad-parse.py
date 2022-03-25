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
    intermidiate = []
    transparent = False
    insideModule = False
    primitiveName = ""
    insideArgs = 0
    insideFunction = False
    moduleDecleration = False
    def identStr(self):
        return '\n' +'    ' * self.ident
        # Enter a parse tree produced by scadParser#module.
    def exitAssignment(self, ctx: scadParser.AssignmentContext):
        a = str(ctx.children[0].getText())
        b = str(ctx.children[2].getText())
        a.replace('true', 'True')
        a.replace('false', 'False')
        b.replace('true', 'True')
        b.replace('false', 'True')
        self.contextStr += self.identStr() + a + " = " + b
    def enterFor_loop(self, ctx: scadParser.For_loopContext):
        a = str(ctx.children[2].getText())
        b = str(ctx.children[4].getText())
        self.contextStr += self.identStr() + "for " + a + " in " + b + ":"
        self.ident += 1
    def exitFor_loop(self, ctx: scadParser.For_loopContext):
        a = str(ctx.children[6].children[1].getText())
        self.contextStr += self.identStr() + a
        self.ident -= 1
    def enterSingle_module_instantiation(self, ctx: scadParser.Single_module_instantiationContext):
        self.contextStr += self.identStr()
    def exitSingle_module_instantiation(self, ctx: scadParser.Single_module_instantiationContext):
        ttt = list(t.getText() for t in ctx.children)
        pass
    def enterModule(self, ctx: scadParser.ModuleContext):
        t = ctx.getText()
        if t.startswith('#'):
            self.transparent = True
            return
        if t.startswith('module'):
            self.moduleDecleration = True
            self.modules.append( self.identStr() + "def " + ctx.children[1].getText() + "(" + ctx.children[3].getText() +")" + ":\n")
        else:
            a = ctx.children[0].getText().replace('()','(')
            a = ctx.children[0].getText().replace('])','],')
            self.contextStr += self.identStr() + a
            if not '()' in t.strip():
                self.contextStr += ','
            self.contextStr = self.contextStr.replace('()','(')
        self.ident += 1
    def exitModule(self, ctx: scadParser.ModuleContext):
        t = ctx.getText()
        if t.startswith('#'):
            self.transparent = False
            return
        if self.transparent:
            self.contextStr += self.identStr() + "transparent = True,"
            self.transparent = False
        localStr = ""
        self.ident -= 1
        if t != "":
            localStr = self.identStr() + self.contextStr
        localStr = localStr.replace('$fn', 'nfrags')
        if self.moduleDecleration:
            if t.startswith('module'):
                self.moduleDecleration = False
                self.primitives.append(self.identStr()+ ")")
            else:
                localStr += self.identStr() + "),"
                localStr = localStr.replace('true', 'True')
                localStr = localStr.replace('false', 'False') 
                self.modules.append(localStr)
                #a = str(ctx.children[0].children[0].getText())
                #b = str(ctx.children[0].children[2].getText())
        else:
            self.ident -= 1
            self.primitives.append(localStr + ")")
        self.contextStr = ""

def main(argv):
    input_stream = FileStream(argv[1])
    lexer = scadLexer(input_stream)
    stream = CommonTokenStream(lexer)
    parser = scadParser(stream)
    tree = parser.parse()
    printer = KeyPrinter()
    walker = ParseTreeWalker()
    walker.walk(printer, tree)
    print("".join(printer.modules))
    print("".join(printer.primitives))
    #print(printer.variables)
if __name__ == '__main__':
    main(sys.argv)