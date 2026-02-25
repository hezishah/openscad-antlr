/**
 * @file blender_generator.h
 * @brief Blender Geometry Nodes Python code generator
 */

#ifndef BLENDER_GENERATOR_H
#define BLENDER_GENERATOR_H

#include <string>
#include <sstream>
#include <set>
#include <map>
#include "ast.h"

namespace scad2blender {

/**
 * @brief Result of emitting an expression node tree
 */
struct EmitResult {
    std::string nodeId;       // Blender node var name, or "__literal__" for constants
    std::string socketName;   // Output socket name
    double literalValue = 0.0; // Value when nodeId == "__literal__"
    bool isGroupInput = false; // True if directly from group_input
};

/**
 * @brief Generates Blender Geometry Nodes Python code from AST
 */
class BlenderGenerator : public ASTVisitor {
public:
    BlenderGenerator() = default;

    /**
     * @brief Generate Python code from the AST
     * @param root The root node of the AST
     * @return Complete Python script for Blender
     */
    std::string generate(ASTNodePtr root);

    // Visitor implementations
    void visit(RootNode& node) override;
    void visit(PrimitiveNode& node) override;
    void visit(TransformNode& node) override;
    void visit(BooleanNode& node) override;
    void visit(ExtrudeNode& node) override;
    void visit(ModuleNode& node) override;
    void visit(ModuleCallNode& node) override;
    void visit(ForLoopNode& node) override;
    void visit(IfElseNode& node) override;
    void visit(AssignmentNode& node) override;
    void visit(ChildrenNode& node) override;

private:
    std::ostringstream code_;
    int indent_ = 0;
    int node_counter_ = 0;
    std::string last_output_;
    std::set<std::string> defined_modules_;
    std::map<std::string, Value> variables_;
    bool in_module_ = false;
    bool module_uses_children_ = false;
    std::set<std::string> group_input_vars_;  // Variables with group_input sockets
    std::set<std::string> top_level_vars_;     // Variables from top-level assignments only

    // Helper methods
    void emit(const std::string& line);
    void emitBlank();
    std::string indent() const;
    std::string newNodeId();

    void emitHeader();
    void emitHelperFunctions();
    void emitVariables();
    void emitModuleFunction(ModuleNode& node);
    void emitBuildGeometry(RootNode& node);
    void emitFooter();

    // Primitive generators
    void emitCube(const Arguments& args);
    void emitSphere(const Arguments& args);
    void emitCylinder(const Arguments& args);
    void emitPolyhedron(const Arguments& args);
    void emitCircle(const Arguments& args);
    void emitSquare(const Arguments& args);
    void emitText(const Arguments& args);

    // Transform generators
    void emitTranslate(const Arguments& args);
    void emitRotate(const Arguments& args);
    void emitScale(const Arguments& args);
    void emitMirror(const Arguments& args);
    void emitOffset(const Arguments& args);
    void emitHull(const Arguments& args);
    void emitMinkowski(const Arguments& args);

    // Boolean generators
    void emitBooleanOp(BooleanNode& node);

    // Extrude generators
    void emitLinearExtrude(const Arguments& args);
    void emitRotateExtrude(const Arguments& args);

    // Utility
    void processChildren(ASTNode& node);
    std::string vectorToPython(const Value& v);
    bool vectorHasExpressions(const Value& v);
    bool moduleUsesChildren(ASTNode& node);

    // Resolve $fn: check args, then global variables_, then default
    Value resolveFn(const Arguments& args);

    // Helper to check if a value is a simple variable reference (no operators)
    bool isSimpleVariableRef(const Value& value);

    // Helper to check if all VarRefs in a tree are group_input sockets
    bool exprTreeHasOnlyGroupInputVars(const ExprNodePtr& tree);

    // Helper to set input or create link from group_input for expressions
    void emitSetInputOrLink(const std::string& nodeId, const std::string& inputName,
                            const Value& value, const std::string& pythonValue);

    // Expression tree emission
    EmitResult emitExpressionNodeTree(const ExprNodePtr& expr);
    void connectExprResult(const EmitResult& result, const std::string& targetNode, int inputIndex);
    void connectExprResultNamed(const EmitResult& result, const std::string& targetNode, const std::string& inputName);
    std::string emitVectorWithExprTrees(const std::string& targetNodeId, const std::string& inputName, const Value& vec);
    ExprNodePtr resolveExprTree(const Value& value);
    bool vectorHasExprTrees(const Value& v);
    void collectGroupInputVars(ASTNode& node);
    void collectModulesRecursive(ASTNode* node);
    void emitModulesRecursive(ASTNode* node);

    // Helper to evaluate an expression Value to a numeric result by resolving variable refs
    double evaluateExpr(const Value& value);
    double evaluateExprTree(const ExprNodePtr& tree);

    // Helper to get a resolved expression tree or create a literal fallback
    ExprNodePtr getOrMakeLiteralTree(const Value& value);

    // Helper to build Value with expression tree for (value / divisor)
    Value makeDivisionValue(const Value& numerator, double divisor);

    // Helper to emit a scalar expression into one component of a CombineXYZ vector input
    // component: 0=X, 1=Y, 2=Z; other components get the specified defaults
    void emitScalarToVectorInput(const std::string& targetNodeId, const std::string& inputName,
                                  const ExprNodePtr& tree, int component,
                                  double defaultX, double defaultY, double defaultZ);

    // Helper to emit a scalar expression into all 3 components of a CombineXYZ vector input
    void emitScalarToAllVectorComponents(const std::string& targetNodeId, const std::string& inputName,
                                          const ExprNodePtr& tree);
};

} // namespace scad2blender

#endif // BLENDER_GENERATOR_H
