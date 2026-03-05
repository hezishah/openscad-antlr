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

ExprNodePtr ExprNode::makeConditional(ExprNodePtr cond, ExprNodePtr then_expr, ExprNodePtr else_expr) {
    auto node = std::make_shared<ExprNode>();
    node->kind = Kind::Conditional;
    node->left = cond;          // condition
    node->right = then_expr;    // then-branch
    node->else_branch = else_expr; // else-branch (may be null)
    return node;
}

ExprNodePtr ExprNode::makeForLoop(const std::string& var, ExprNodePtr range, ExprNodePtr body) {
    auto node = std::make_shared<ExprNode>();
    node->kind = Kind::ForLoop;
    node->var_name = var;       // loop variable name
    node->left = range;         // range expression
    node->right = body;         // body expression
    return node;
}

ExprNodePtr ExprNode::makeLetBinding(std::vector<std::pair<std::string, ExprNodePtr>> bindings, ExprNodePtr body) {
    auto node = std::make_shared<ExprNode>();
    node->kind = Kind::LetBinding;
    node->let_bindings = std::move(bindings);
    node->right = body;         // body expression
    return node;
}

bool ExprNode::hasVariableRefs() const {
    if (kind == Kind::VarRef) return true;
    if (kind == Kind::Literal) return false;
    if (left && left->hasVariableRefs()) return true;
    if (right && right->hasVariableRefs()) return true;
    if (else_branch && else_branch->hasVariableRefs()) return true;
    for (const auto& arg : func_args) {
        if (arg && arg->hasVariableRefs()) return true;
    }
    for (const auto& elem : vec_elements) {
        if (elem && elem->hasVariableRefs()) return true;
    }
    for (const auto& binding : let_bindings) {
        if (binding.second && binding.second->hasVariableRefs()) return true;
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
            ss << "0";
            break;

        case Type::Boolean:
            ss << (bool_val_ ? "True" : "False");
            break;

        case Type::Number:
            if (std::isnan(num_val_)) {
                ss << "float('nan')";
            } else if (std::isinf(num_val_)) {
                ss << (num_val_ > 0 ? "float('inf')" : "float('-inf')");
            } else if (std::floor(num_val_) == num_val_ && std::abs(num_val_) < 1e10) {
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
            ss << "0  # function reference";
            break;

        case Type::Expression: {
            // Output the expression directly (unquoted)
            // Replace $ prefix with _ for Python-valid identifiers
            std::string pyExpr = str_val_;
            size_t pos = 0;
            // Replace OpenSCAD runtime special variables with safe defaults
            // before the $ -> _ conversion (so we match $name patterns)
            static const struct { const char* var; const char* val; } specialVarDefaults[] = {
                {"$parent_modules", "0"},
                {"$children", "0"},
                {"$vpd", "0"},
                {"$vpr", "0"},
                {"$vpt", "0"},
                {"$preview", "True"},
                {"$t", "0"},
                {nullptr, nullptr}
            };
            for (auto* sv = specialVarDefaults; sv->var; ++sv) {
                pos = 0;
                std::string varName(sv->var);
                while ((pos = pyExpr.find(varName, pos)) != std::string::npos) {
                    // Check word boundaries
                    bool startOk = (pos == 0 || (!std::isalnum(pyExpr[pos - 1]) && pyExpr[pos - 1] != '_' && pyExpr[pos - 1] != '$'));
                    size_t endPos = pos + varName.size();
                    bool endOk = (endPos >= pyExpr.size() || (!std::isalnum(pyExpr[endPos]) && pyExpr[endPos] != '_'));
                    if (startOk && endOk) {
                        pyExpr.replace(pos, varName.size(), sv->val);
                        pos += std::strlen(sv->val);
                    } else {
                        pos += varName.size();
                    }
                }
            }
            pos = 0;
            while ((pos = pyExpr.find('$', pos)) != std::string::npos) {
                pyExpr.replace(pos, 1, "_");
                pos++;
            }
            // If the expression contains '...' (unresolved arguments), the whole
            // expression can't be meaningfully represented in Python. Fall back to 0.
            if (pyExpr.find("...") != std::string::npos) {
                ss << "0";
                break;
            }
            // Replace OpenSCAD vector member access .x/.y/.z with Python indexing [0]/[1]/[2]
            {
                size_t p = 0;
                while (p < pyExpr.size()) {
                    // Look for .x, .y, or .z followed by a non-alphanumeric/non-underscore char (or end)
                    if (pyExpr[p] == '.' && p > 0 && p + 1 < pyExpr.size()) {
                        char member = pyExpr[p + 1];
                        if ((member == 'x' || member == 'y' || member == 'z') &&
                            (p + 2 >= pyExpr.size() || (!std::isalnum(pyExpr[p + 2]) && pyExpr[p + 2] != '_'))) {
                            // Check that the char before '.' is part of an identifier (letter, digit, underscore, or ')')
                            char before = pyExpr[p - 1];
                            if (std::isalnum(before) || before == '_' || before == ')') {
                                std::string replacement;
                                if (member == 'x') replacement = "[0]";
                                else if (member == 'y') replacement = "[1]";
                                else replacement = "[2]";
                                pyExpr.replace(p, 2, replacement);
                                p += replacement.size();
                                continue;
                            }
                        }
                    }
                    p++;
                }
            }
            // Replace OpenSCAD type-check functions with False (can't evaluate at codegen time)
            static const char* typeCheckFuncs[] = {
                "is_bool(", "is_num(", "is_list(", "is_string(", "is_undef(", nullptr
            };
            for (const char** tc = typeCheckFuncs; *tc; ++tc) {
                pos = 0;
                std::string scadFunc(*tc);
                while ((pos = pyExpr.find(scadFunc, pos)) != std::string::npos) {
                    size_t start = pos;
                    size_t depth = 0;
                    size_t i = pos + scadFunc.size() - 1;
                    for (; i < pyExpr.size(); ++i) {
                        if (pyExpr[i] == '(') depth++;
                        else if (pyExpr[i] == ')') { depth--; if (depth == 0) break; }
                    }
                    if (i < pyExpr.size()) {
                        pyExpr.replace(start, i - start + 1, "False");
                        pos = start + 5;
                    } else {
                        pos += scadFunc.size();
                    }
                }
            }
            // Replace standalone "None" tokens with "0" in expressions
            // (from unresolved variables that were Undefined)
            {
                size_t p = 0;
                while ((p = pyExpr.find("None", p)) != std::string::npos) {
                    // Check word boundaries
                    bool startOk = (p == 0 || (!std::isalnum(pyExpr[p - 1]) && pyExpr[p - 1] != '_'));
                    bool endOk = (p + 4 >= pyExpr.size() || (!std::isalnum(pyExpr[p + 4]) && pyExpr[p + 4] != '_'));
                    if (startOk && endOk) {
                        pyExpr.replace(p, 4, "0");
                        p += 1;
                    } else {
                        p += 4;
                    }
                }
            }
            // Append _ to Python reserved words used as identifiers
            static const char* reserved[] = {
                "False", "None", "True", "and", "as", "assert", "async", "await",
                "break", "class", "continue", "def", "del", "elif", "else", "except",
                "finally", "for", "from", "global", "if", "import", "in", "is",
                "lambda", "nonlocal", "not", "or", "pass", "raise", "return",
                "try", "while", "with", "yield", nullptr
            };
            for (const char** kw = reserved; *kw; ++kw) {
                if (pyExpr == *kw) {
                    pyExpr += "_";
                    break;
                }
            }
            // Check if the expression contains bare identifiers that would
            // cause NameError at Python runtime (unresolvable OpenSCAD variables).
            // If any are found, replace the entire expression with "0" rather than
            // selectively replacing identifiers (which creates invalid code like
            // 0[0] or 0/0).
            {
                static const std::set<std::string> safeNames = {
                    // Python keywords & constants
                    "False", "True", "None", "and", "or", "not", "if", "else", "in", "is",
                    // Python builtins used in generated code
                    "abs", "int", "float", "len", "max", "min", "round", "range",
                    "isinstance", "bool", "list", "tuple", "str",
                    // math module
                    "math",
                    // Values that look like identifiers but are numeric
                    "e",
                };
                bool hasUnsafe = false;
                size_t i = 0;
                while (i < pyExpr.size()) {
                    if (std::isalpha(pyExpr[i]) || pyExpr[i] == '_') {
                        size_t start = i;
                        while (i < pyExpr.size() && (std::isalnum(pyExpr[i]) || pyExpr[i] == '_'))
                            i++;
                        std::string ident = pyExpr.substr(start, i - start);
                        // Check if followed by '(' — it's a function call, safe
                        size_t j = i;
                        while (j < pyExpr.size() && pyExpr[j] == ' ') j++;
                        bool isCall = (j < pyExpr.size() && pyExpr[j] == '(');
                        // Check if preceded by '.' — it's an attribute, safe
                        bool isAttr = (start > 0 && pyExpr[start - 1] == '.');
                        // Check if it's a numeric literal suffix like 'e' in "1e-06"
                        bool isNumSuffix = (start > 0 && std::isdigit(pyExpr[start - 1]));
                        if (!isCall && !isAttr && !isNumSuffix &&
                            !safeNames.count(ident) && ident.find("math") != 0) {
                            hasUnsafe = true;
                            break;
                        }
                    } else {
                        i++;
                    }
                }
                if (hasUnsafe) {
                    pyExpr = "0";
                }
            }
            // Guard against division by zero: replace "/ 0)" or "/ 0,"
            // patterns that arise from unresolved variables being replaced with 0.
            {
                size_t p = 0;
                while ((p = pyExpr.find("/ 0", p)) != std::string::npos) {
                    size_t after = p + 3;
                    // Skip whitespace
                    while (after < pyExpr.size() && pyExpr[after] == ' ') after++;
                    // Check if followed by end, ')' or ',' (not a digit/letter — that would be a different token)
                    if (after >= pyExpr.size() || pyExpr[after] == ')' || pyExpr[after] == ',' ||
                        pyExpr[after] == '+' || pyExpr[after] == '-' || pyExpr[after] == '*') {
                        pyExpr.replace(p + 2, 1, "1");  // Replace the "0" with "1"
                        p += 3;
                    } else {
                        p += 3;
                    }
                }
            }
            ss << pyExpr;
            break;
        }
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
