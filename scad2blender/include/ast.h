/**
 * @file ast.h
 * @brief Abstract Syntax Tree node definitions for OpenSCAD parser
 *
 * Based on OpenSCAD's node architecture, adapted for Blender GN code generation.
 */

#ifndef AST_H
#define AST_H

#include <string>
#include <vector>
#include <memory>
#include <map>
#include <variant>
#include "value.h"

namespace scad2blender {

// Forward declarations
class ASTNode;
class ASTVisitor;

using ASTNodePtr = std::shared_ptr<ASTNode>;
using ASTNodeList = std::vector<ASTNodePtr>;

/**
 * @brief Base class for all AST nodes
 */
class ASTNode {
public:
    enum class Type {
        // Primitives
        Cube,
        Sphere,
        Cylinder,
        Polyhedron,
        Circle,
        Square,
        Polygon,
        Text,

        // Transforms
        Translate,
        Rotate,
        Scale,
        Mirror,
        Color,
        Offset,
        Resize,
        Multmatrix,

        // Boolean operations
        Union,
        Difference,
        Intersection,

        // Extrusions
        LinearExtrude,
        RotateExtrude,

        // Other
        Hull,
        Minkowski,
        Roof,
        Projection,
        Import,
        Surface,

        // Control structures
        Module,
        ModuleCall,
        ForLoop,
        IfElse,
        Children,

        // Expressions
        Assignment,
        Expression,
        FunctionCall,
        FunctionDef,

        // Root
        Root
    };

    ASTNode(Type type) : type_(type) {}
    virtual ~ASTNode() = default;

    Type type() const { return type_; }
    const ASTNodeList& children() const { return children_; }
    ASTNodeList& children() { return children_; }

    void addChild(ASTNodePtr child) { children_.push_back(child); }

    // Visitor pattern
    virtual void accept(ASTVisitor& visitor) = 0;

    // Modifier flags (OpenSCAD's !, #, %, *)
    bool isRoot() const { return is_root_; }      // !
    bool isDebug() const { return is_debug_; }    // #
    bool isBackground() const { return is_background_; } // %
    bool isDisabled() const { return is_disabled_; }     // *

    void setRoot(bool v) { is_root_ = v; }
    void setDebug(bool v) { is_debug_ = v; }
    void setBackground(bool v) { is_background_ = v; }
    void setDisabled(bool v) { is_disabled_ = v; }

protected:
    Type type_;
    ASTNodeList children_;

    bool is_root_ = false;
    bool is_debug_ = false;
    bool is_background_ = false;
    bool is_disabled_ = false;
};

/**
 * @brief Root node containing the entire program
 */
class RootNode : public ASTNode {
public:
    RootNode() : ASTNode(Type::Root) {}
    void accept(ASTVisitor& visitor) override;
};

/**
 * @brief Primitive nodes (cube, sphere, cylinder, etc.)
 */
class PrimitiveNode : public ASTNode {
public:
    PrimitiveNode(Type type, const Arguments& args)
        : ASTNode(type), args_(args) {}

    const Arguments& args() const { return args_; }
    void accept(ASTVisitor& visitor) override;

private:
    Arguments args_;
};

/**
 * @brief Transform nodes (translate, rotate, scale, etc.)
 */
class TransformNode : public ASTNode {
public:
    TransformNode(Type type, const Arguments& args)
        : ASTNode(type), args_(args) {}

    const Arguments& args() const { return args_; }
    void accept(ASTVisitor& visitor) override;

private:
    Arguments args_;
};

/**
 * @brief Boolean operation nodes (union, difference, intersection)
 */
class BooleanNode : public ASTNode {
public:
    BooleanNode(Type type) : ASTNode(type) {}
    void accept(ASTVisitor& visitor) override;
};

/**
 * @brief Extrusion nodes (linear_extrude, rotate_extrude)
 */
class ExtrudeNode : public ASTNode {
public:
    ExtrudeNode(Type type, const Arguments& args)
        : ASTNode(type), args_(args) {}

    const Arguments& args() const { return args_; }
    void accept(ASTVisitor& visitor) override;

private:
    Arguments args_;
};

/**
 * @brief Module definition
 */
class ModuleNode : public ASTNode {
public:
    ModuleNode(const std::string& name, const std::vector<std::string>& params)
        : ASTNode(Type::Module), name_(name), parameters_(params) {}

    const std::string& name() const { return name_; }
    const std::vector<std::string>& parameters() const { return parameters_; }
    const std::map<std::string, Value>& parameterDefaults() const { return param_defaults_; }
    void setParameterDefault(const std::string& name, const Value& val) { param_defaults_[name] = val; }
    void accept(ASTVisitor& visitor) override;

private:
    std::string name_;
    std::vector<std::string> parameters_;
    std::map<std::string, Value> param_defaults_;
};

/**
 * @brief Module call (instantiation)
 */
class ModuleCallNode : public ASTNode {
public:
    ModuleCallNode(const std::string& name, const Arguments& args)
        : ASTNode(Type::ModuleCall), name_(name), args_(args) {}

    const std::string& name() const { return name_; }
    const Arguments& args() const { return args_; }
    void accept(ASTVisitor& visitor) override;

private:
    std::string name_;
    Arguments args_;
};

/**
 * @brief For loop
 */
class ForLoopNode : public ASTNode {
public:
    ForLoopNode(const std::string& var, const Value& range)
        : ASTNode(Type::ForLoop), variable_(var), range_(range) {}

    const std::string& variable() const { return variable_; }
    const Value& range() const { return range_; }
    void accept(ASTVisitor& visitor) override;

private:
    std::string variable_;
    Value range_;
};

/**
 * @brief If/else statement
 */
class IfElseNode : public ASTNode {
public:
    IfElseNode(const Value& condition)
        : ASTNode(Type::IfElse), condition_(condition) {}

    const Value& condition() const { return condition_; }

    void setElseBranch(ASTNodePtr node) { else_branch_ = node; }
    ASTNodePtr elseBranch() const { return else_branch_; }

    void accept(ASTVisitor& visitor) override;

private:
    Value condition_;
    ASTNodePtr else_branch_;
};

/**
 * @brief Variable assignment
 */
class AssignmentNode : public ASTNode {
public:
    AssignmentNode(const std::string& name, const Value& value)
        : ASTNode(Type::Assignment), name_(name), value_(value) {}

    const std::string& name() const { return name_; }
    const Value& value() const { return value_; }
    void accept(ASTVisitor& visitor) override;

private:
    std::string name_;
    Value value_;
};

/**
 * @brief children() call inside modules
 */
class ChildrenNode : public ASTNode {
public:
    ChildrenNode() : ASTNode(Type::Children) {}
    void accept(ASTVisitor& visitor) override;
};

/**
 * @brief Function definition (function name(params) = expr;)
 */
class FunctionNode : public ASTNode {
public:
    FunctionNode(const std::string& name, const std::vector<std::string>& params)
        : ASTNode(Type::FunctionDef), name_(name), parameters_(params) {}
    const std::string& name() const { return name_; }
    const std::vector<std::string>& parameters() const { return parameters_; }
    const ExprNodePtr& body() const { return body_; }
    void setBody(const ExprNodePtr& body) { body_ = body; }
    void accept(ASTVisitor& visitor) override;
private:
    std::string name_;
    std::vector<std::string> parameters_;
    ExprNodePtr body_;
};

/**
 * @brief Visitor interface for AST traversal
 */
class ASTVisitor {
public:
    virtual ~ASTVisitor() = default;

    virtual void visit(RootNode& node) = 0;
    virtual void visit(PrimitiveNode& node) = 0;
    virtual void visit(TransformNode& node) = 0;
    virtual void visit(BooleanNode& node) = 0;
    virtual void visit(ExtrudeNode& node) = 0;
    virtual void visit(ModuleNode& node) = 0;
    virtual void visit(ModuleCallNode& node) = 0;
    virtual void visit(ForLoopNode& node) = 0;
    virtual void visit(IfElseNode& node) = 0;
    virtual void visit(AssignmentNode& node) = 0;
    virtual void visit(ChildrenNode& node) = 0;
    virtual void visit(FunctionNode& node) = 0;
};

} // namespace scad2blender

#endif // AST_H
