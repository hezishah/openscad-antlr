/**
 * @file value.cpp
 * @brief Value class implementation
 */

#include "value.h"
#include <sstream>
#include <iomanip>
#include <cmath>
#include <set>
#include <algorithm>

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

ExprNodePtr ExprNode::makeFunctionCall(const std::string& name, const std::vector<ExprNodePtr>& args) {
    auto node = std::make_shared<ExprNode>();
    node->kind = Kind::FunctionCall;
    node->func_name = name;
    node->func_args = args;
    return node;
}

ExprNodePtr ExprNode::makeVectorLiteral(const std::vector<ExprNodePtr>& elements) {
    auto node = std::make_shared<ExprNode>();
    node->kind = Kind::VectorLiteral;
    node->vec_elements = elements;
    return node;
}

bool ExprNode::hasVariableRefs() const {
    if (kind == Kind::VarRef) return true;
    if (kind == Kind::Literal) return false;
    if (left && left->hasVariableRefs()) return true;
    if (right && right->hasVariableRefs()) return true;
    for (const auto& arg : func_args) {
        if (arg && arg->hasVariableRefs()) return true;
    }
    for (const auto& elem : vec_elements) {
        if (elem && elem->hasVariableRefs()) return true;
    }
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

static const double DEG2RAD = M_PI / 180.0;
static const double RAD2DEG = 180.0 / M_PI;

bool isBuiltinFunction(const std::string& name) {
    static const std::set<std::string> builtins = {
        "sin", "cos", "tan", "asin", "acos", "atan", "atan2",
        "sqrt", "abs", "pow", "exp", "ln", "log", "floor", "ceil", "round", "sign",
        "max", "min", "norm", "len", "concat", "cross"
    };
    return builtins.count(name) > 0;
}

double evaluateBuiltinMath(const std::string& name, const std::vector<double>& args) {
    if (args.empty()) return 0.0;
    double a = args[0];

    if (name == "sin") return std::sin(a * DEG2RAD);
    if (name == "cos") return std::cos(a * DEG2RAD);
    if (name == "tan") return std::tan(a * DEG2RAD);
    if (name == "asin") return std::asin(a) * RAD2DEG;
    if (name == "acos") return std::acos(a) * RAD2DEG;
    if (name == "atan") {
        if (args.size() >= 2) return std::atan2(a, args[1]) * RAD2DEG;
        return std::atan(a) * RAD2DEG;
    }
    if (name == "atan2") {
        if (args.size() >= 2) return std::atan2(a, args[1]) * RAD2DEG;
        return 0.0;
    }
    if (name == "sqrt") return std::sqrt(a);
    if (name == "abs") return std::abs(a);
    if (name == "pow") return args.size() >= 2 ? std::pow(a, args[1]) : a;
    if (name == "exp") return std::exp(a);
    if (name == "ln" || name == "log") return std::log(a);
    if (name == "floor") return std::floor(a);
    if (name == "ceil") return std::ceil(a);
    if (name == "round") return std::round(a);
    if (name == "sign") return (a > 0) ? 1.0 : (a < 0) ? -1.0 : 0.0;
    if (name == "max") {
        double result = a;
        for (size_t i = 1; i < args.size(); i++) result = std::max(result, args[i]);
        return result;
    }
    if (name == "min") {
        double result = a;
        for (size_t i = 1; i < args.size(); i++) result = std::min(result, args[i]);
        return result;
    }
    if (name == "norm") {
        double sum = 0;
        for (double v : args) sum += v * v;
        return std::sqrt(sum);
    }
    return 0.0;
}

} // namespace scad2blender
