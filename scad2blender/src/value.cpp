/**
 * @file value.cpp
 * @brief Value class implementation
 */

#include "value.h"
#include <sstream>
#include <iomanip>
#include <cmath>

namespace scad2blender {

// ExprNode factory methods
ExprNodePtr ExprNode::makeLiteral(double v) {
    auto node = std::make_shared<ExprNode>();
    node->kind = Kind::Literal;
    node->literal_value = v;
    return node;
}

ExprNodePtr ExprNode::makeVarRef(const std::string& name) {
    auto node = std::make_shared<ExprNode>();
    node->kind = Kind::VarRef;
    node->var_name = name;
    return node;
}

ExprNodePtr ExprNode::makeUnary(Op op, ExprNodePtr operand) {
    auto node = std::make_shared<ExprNode>();
    node->kind = Kind::UnaryOp;
    node->op = op;
    node->left = operand;
    return node;
}

ExprNodePtr ExprNode::makeBinary(Op op, ExprNodePtr left, ExprNodePtr right) {
    auto node = std::make_shared<ExprNode>();
    node->kind = Kind::BinaryOp;
    node->op = op;
    node->left = left;
    node->right = right;
    return node;
}

bool ExprNode::hasVariableRefs() const {
    if (kind == Kind::VarRef) return true;
    if (kind == Kind::Literal) return false;
    if (left && left->hasVariableRefs()) return true;
    if (right && right->hasVariableRefs()) return true;
    return false;
}

std::string Value::repr() const {
    std::ostringstream ss;

    switch (type_) {
        case Type::Undefined:
            ss << "undef";
            break;

        case Type::Boolean:
            ss << (bool_val_ ? "true" : "false");
            break;

        case Type::Number:
            if (std::floor(num_val_) == num_val_ && std::abs(num_val_) < 1e10) {
                ss << static_cast<long long>(num_val_);
            } else {
                ss << std::setprecision(15) << num_val_;
            }
            break;

        case Type::String:
            ss << "\"" << str_val_ << "\"";
            break;

        case Type::Vector:
            ss << "[";
            for (size_t i = 0; i < vec_val_.size(); ++i) {
                if (i > 0) ss << ", ";
                ss << vec_val_[i].repr();
            }
            ss << "]";
            break;

        case Type::Range:
            ss << "[" << range_start_ << ":" << range_step_ << ":" << range_end_ << "]";
            break;

        case Type::Function:
            ss << "<function>";
            break;

        case Type::Expression:
            ss << "<expr: " << str_val_ << ">";
            break;
    }

    return ss.str();
}

std::string Value::toPython() const {
    std::ostringstream ss;

    switch (type_) {
        case Type::Undefined:
            ss << "None";
            break;

        case Type::Boolean:
            ss << (bool_val_ ? "True" : "False");
            break;

        case Type::Number:
            if (std::floor(num_val_) == num_val_ && std::abs(num_val_) < 1e10) {
                ss << static_cast<long long>(num_val_);
            } else {
                ss << std::setprecision(15) << num_val_;
            }
            break;

        case Type::String:
            ss << "\"" << str_val_ << "\"";
            break;

        case Type::Vector:
            ss << "(";
            for (size_t i = 0; i < vec_val_.size(); ++i) {
                if (i > 0) ss << ", ";
                ss << vec_val_[i].toPython();
            }
            if (vec_val_.size() == 1) ss << ",";
            ss << ")";
            break;

        case Type::Range:
            // Python range representation
            if (range_step_ == 1.0) {
                ss << "range(" << static_cast<int>(range_start_)
                   << ", " << static_cast<int>(range_end_) + 1 << ")";
            } else {
                ss << "range(" << static_cast<int>(range_start_)
                   << ", " << static_cast<int>(range_end_) + 1
                   << ", " << static_cast<int>(range_step_) << ")";
            }
            break;

        case Type::Function:
            ss << "None  # function reference";
            break;

        case Type::Expression:
            // Output the expression directly (unquoted)
            ss << str_val_;
            break;
    }

    return ss.str();
}

} // namespace scad2blender
