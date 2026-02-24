/**
 * @file ast.cpp
 * @brief AST node implementations
 */

#include "ast.h"

namespace scad2blender {

// Visitor accept implementations
void RootNode::accept(ASTVisitor& visitor) {
    visitor.visit(*this);
}

void PrimitiveNode::accept(ASTVisitor& visitor) {
    visitor.visit(*this);
}

void TransformNode::accept(ASTVisitor& visitor) {
    visitor.visit(*this);
}

void BooleanNode::accept(ASTVisitor& visitor) {
    visitor.visit(*this);
}

void ExtrudeNode::accept(ASTVisitor& visitor) {
    visitor.visit(*this);
}

void ModuleNode::accept(ASTVisitor& visitor) {
    visitor.visit(*this);
}

void ModuleCallNode::accept(ASTVisitor& visitor) {
    visitor.visit(*this);
}

void ForLoopNode::accept(ASTVisitor& visitor) {
    visitor.visit(*this);
}

void IfElseNode::accept(ASTVisitor& visitor) {
    visitor.visit(*this);
}

void AssignmentNode::accept(ASTVisitor& visitor) {
    visitor.visit(*this);
}

void ChildrenNode::accept(ASTVisitor& visitor) {
    visitor.visit(*this);
}

} // namespace scad2blender
