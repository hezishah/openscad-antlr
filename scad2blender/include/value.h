/**
 * @file value.h
 * @brief Value types for OpenSCAD expressions
 *
 * Represents the various value types in OpenSCAD: numbers, strings,
 * vectors, booleans, and undefined.
 */

#ifndef VALUE_H
#define VALUE_H

#include <string>
#include <vector>
#include <variant>
#include <map>
#include <memory>
#include <optional>

namespace scad2blender {

// Forward declarations
class Value;
struct ExprNode;

using ValuePtr = std::shared_ptr<Value>;
using Vector = std::vector<Value>;
using Arguments = std::map<std::string, Value>;
using ExprNodePtr = std::shared_ptr<ExprNode>;

/**
 * @brief Tree structure for representing arithmetic expressions
 *
 * Used to generate Blender ShaderNodeMath chains that wire group_input
 * outputs through expression trees to geometry node inputs.
 */
struct ExprNode {
    enum class Kind { Literal, VarRef, UnaryOp, BinaryOp, FunctionCall, VectorLiteral,
                      Conditional, ForLoop, LetBinding };
    enum class Op { ADD, SUBTRACT, MULTIPLY, DIVIDE, MODULO, POWER, NEGATE,
                    LESS, GREATER, LESS_EQ, GREATER_EQ, EQUAL, NOT_EQUAL,
                    AND, OR, NOT };

    Kind kind;
    double literal_value = 0.0;   // Literal
    std::string var_name;         // VarRef
    Op op = Op::ADD;              // UnaryOp/BinaryOp
    ExprNodePtr left, right;      // BinaryOp (left only for UnaryOp)

    std::string func_name;                  // FunctionCall
    std::vector<ExprNodePtr> func_args;     // FunctionCall
    std::vector<std::string> arg_names;     // FunctionCall: named param mapping
    std::vector<ExprNodePtr> vec_elements;  // VectorLiteral

    ExprNodePtr else_branch;                                       // Conditional: else expr
    std::vector<std::pair<std::string, ExprNodePtr>> let_bindings; // LetBinding: name→expr pairs

    static ExprNodePtr makeLiteral(double v);
    static ExprNodePtr makeVarRef(const std::string& name);
    static ExprNodePtr makeUnary(Op op, ExprNodePtr operand);
    static ExprNodePtr makeBinary(Op op, ExprNodePtr left, ExprNodePtr right);
    static ExprNodePtr makeFunctionCall(const std::string& name, const std::vector<ExprNodePtr>& args);
    static ExprNodePtr makeVectorLiteral(const std::vector<ExprNodePtr>& elements);
    static ExprNodePtr makeConditional(ExprNodePtr cond, ExprNodePtr then_expr, ExprNodePtr else_expr);
    static ExprNodePtr makeForLoop(const std::string& var, ExprNodePtr range, ExprNodePtr body);
    static ExprNodePtr makeLetBinding(std::vector<std::pair<std::string, ExprNodePtr>> bindings, ExprNodePtr body);
    bool hasVariableRefs() const;
};

/**
 * @brief Represents an OpenSCAD value (number, string, vector, bool, undef)
 */
class Value {
public:
    enum class Type {
        Undefined,
        Boolean,
        Number,
        String,
        Vector,
        Range,
        Function,
        Expression  // Unevaluated expression (stores Python code)
    };

    // Constructors
    Value() : type_(Type::Undefined) {}
    Value(bool b) : type_(Type::Boolean), bool_val_(b), num_val_(b ? 1.0 : 0.0) {}
    Value(double d) : type_(Type::Number), num_val_(d) {}
    Value(int i) : type_(Type::Number), num_val_(static_cast<double>(i)) {}
    Value(const std::string& s) : type_(Type::String), str_val_(s) {}
    Value(const char* s) : type_(Type::String), str_val_(s) {}
    Value(const Vector& v) : type_(Type::Vector), vec_val_(v) {}
    Value(std::initializer_list<Value> v) : type_(Type::Vector), vec_val_(v) {}

    // Expression constructor - for unevaluated expressions
    static Value expression(const std::string& expr) {
        Value v;
        v.type_ = Type::Expression;
        v.str_val_ = expr;
        return v;
    }

    // Expression with tree constructor
    static Value expressionWithTree(const std::string& expr, ExprNodePtr tree) {
        Value v;
        v.type_ = Type::Expression;
        v.str_val_ = expr;
        v.expr_tree_ = tree;
        return v;
    }

    // Range constructor [start:end] or [start:step:end]
    static Value range(double start, double end, double step = 1.0) {
        Value v;
        v.type_ = Type::Range;
        v.range_start_ = start;
        v.range_end_ = end;
        v.range_step_ = step;
        return v;
    }

    // Range constructor with expression trees for bounds
    static Value rangeWithExprs(double start, double end, double step,
                                ExprNodePtr startExpr, ExprNodePtr endExpr, ExprNodePtr stepExpr = nullptr) {
        Value v;
        v.type_ = Type::Range;
        v.range_start_ = start;
        v.range_end_ = end;
        v.range_step_ = step;
        v.range_start_expr_ = startExpr;
        v.range_end_expr_ = endExpr;
        v.range_step_expr_ = stepExpr;
        return v;
    }

    // Type checking
    Type type() const { return type_; }
    bool isUndefined() const { return type_ == Type::Undefined; }
    bool isBool() const { return type_ == Type::Boolean; }
    bool isNumber() const { return type_ == Type::Number; }
    bool isString() const { return type_ == Type::String; }
    bool isVector() const { return type_ == Type::Vector; }
    bool isRange() const { return type_ == Type::Range; }
    bool isExpression() const { return type_ == Type::Expression; }

    // Value access
    bool toBool() const { return bool_val_; }
    double toNumber() const { return num_val_; }
    const std::string& toString() const { return str_val_; }
    const Vector& toVector() const { return vec_val_; }

    // Range access
    double rangeStart() const { return range_start_; }
    double rangeEnd() const { return range_end_; }
    double rangeStep() const { return range_step_; }
    const ExprNodePtr& rangeStartExpr() const { return range_start_expr_; }
    const ExprNodePtr& rangeEndExpr() const { return range_end_expr_; }
    const ExprNodePtr& rangeStepExpr() const { return range_step_expr_; }

    // Vector operations
    size_t size() const { return vec_val_.size(); }
    const Value& operator[](size_t i) const { return vec_val_[i]; }
    Value& operator[](size_t i) { return vec_val_[i]; }

    // Expression tree access
    const ExprNodePtr& exprTree() const { return expr_tree_; }
    void setExprTree(ExprNodePtr tree) { expr_tree_ = tree; }

    // Convert to string representation
    std::string repr() const;

    // Convert to Python literal
    std::string toPython() const;

private:
    Type type_ = Type::Undefined;
    bool bool_val_ = false;
    double num_val_ = 0.0;
    std::string str_val_;
    Vector vec_val_;
    double range_start_ = 0.0;
    double range_end_ = 0.0;
    double range_step_ = 1.0;
    ExprNodePtr range_start_expr_;
    ExprNodePtr range_end_expr_;
    ExprNodePtr range_step_expr_;
    ExprNodePtr expr_tree_;
};

/**
 * @brief Get a named argument with optional default value
 */
inline Value getArg(const Arguments& args, const std::string& name,
                    const Value& defaultVal = Value()) {
    auto it = args.find(name);
    return (it != args.end()) ? it->second : defaultVal;
}

/**
 * @brief Get a positional argument (named as "_0", "_1", etc.)
 */
inline Value getPositionalArg(const Arguments& args, size_t index,
                               const Value& defaultVal = Value()) {
    return getArg(args, "_" + std::to_string(index), defaultVal);
}

/**
 * @brief Check if argument exists
 */
inline bool hasArg(const Arguments& args, const std::string& name) {
    return args.find(name) != args.end();
}

// Built-in math function evaluation (trig uses degrees like OpenSCAD)
double evaluateBuiltinMath(const std::string& name, const std::vector<double>& args);
bool isBuiltinFunction(const std::string& name);

} // namespace scad2blender

#endif // VALUE_H
