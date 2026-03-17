/**
 * @file blender_generator.cpp
 * @brief Blender Geometry Nodes Python code generator
 */

#include "blender_generator.h"
#include <cmath>
#include <algorithm>
#include <iostream>
#include <fstream>
namespace scad2blender {

// Convert a double to a Python-safe string, handling NaN and infinity
static std::string pyDouble(double v) {
    if (std::isnan(v)) return "float('nan')";
    if (std::isinf(v)) return v > 0 ? "float('inf')" : "float('-inf')";
    return std::to_string(v);
}

std::string BlenderGenerator::generate(ASTNodePtr root) {
    code_.str("");
    code_.clear();
    indent_ = 0;
    node_counter_ = 0;
    last_output_ = "";
    defined_modules_.clear();
    variables_.clear();
    functions_.clear();
    group_input_vars_.clear();
    top_level_vars_.clear();
    for_loop_range_vars_.clear();
    in_module_ = false;
    module_uses_children_ = false;

    // Note: OpenSCAD special variables ($preview, $fn, $fa, $fs) are NOT set here
    // because that would change how global expressions like fn=$fn?$fn:$preview?36:72
    // evaluate, potentially losing group_input sockets.
    // Instead, $preview is handled in visit(IfElseNode&) for conditional evaluation.

    emitHeader();
    emitHelperFunctions();

    // First pass: collect modules (recursively, to find nested modules), functions, and variables
    for (auto& child : root->children()) {
        if (!child) continue;
        collectModulesRecursive(child.get());
        collectFunctionsRecursive(child.get());
        if (child->type() == ASTNode::Type::Assignment) {
            auto* assign = dynamic_cast<AssignmentNode*>(child.get());
            if (assign) {
                variables_[assign->name()] = assign->value();
                top_level_vars_.insert(assign->name());
            }
        }
    }

    // If there are no top-level variables, promote simple constant assignments from
    // the main module(s) called at the top level. This allows the user to adjust them
    // from the Blender UI via group_input sockets.
    if (top_level_vars_.empty()) {
        // Find which modules are called at the top level
        std::set<std::string> calledModules;
        for (auto& child : root->children()) {
            if (!child) continue;
            if (child->type() == ASTNode::Type::ModuleCall) {
                auto* call = dynamic_cast<ModuleCallNode*>(child.get());
                if (call) calledModules.insert(call->name());
            }
        }
        // For each called module, promote its direct constant assignments
        // that are bare literals (not computed from other variables).
        std::function<void(ASTNode&)> promoteAssignments = [&](ASTNode& container) {
            for (auto& mc : container.children()) {
                if (!mc) continue;
                if (mc->type() == ASTNode::Type::Assignment) {
                    auto* assign = dynamic_cast<AssignmentNode*>(mc.get());
                    if (!assign) continue;
                    const Value& val = assign->value();
                    bool isSimple = val.isNumber() || val.isBool() || val.isString();
                    if (!isSimple && val.isVector() && val.size() == 3) {
                        isSimple = true;
                        for (size_t i = 0; i < val.size(); ++i) {
                            if (val[i].isExpression()) { isSimple = false; break; }
                        }
                    }
                    // Skip derived variables (those whose expression tree references other vars)
                    if (val.exprTree() && val.exprTree()->hasVariableRefs()) {
                        isSimple = false;
                    }
                    // Skip expression-type values (derived from other variables at parse time)
                    if (val.isExpression()) {
                        isSimple = false;
                    }
                    if (isSimple) {
                        variables_[assign->name()] = val;
                        top_level_vars_.insert(assign->name());
                    }
                } else if (mc->type() == ASTNode::Type::Union ||
                           mc->type() == ASTNode::Type::Difference ||
                           mc->type() == ASTNode::Type::Intersection) {
                    promoteAssignments(*mc);
                }
            }
        };
        for (auto& child : root->children()) {
            if (!child) continue;
            if (child->type() == ASTNode::Type::Module) {
                auto* mod = dynamic_cast<ModuleNode*>(child.get());
                if (mod && calledModules.count(mod->name())) {
                    promoteAssignments(*mod);
                }
            }
        }
    }

    // Pre-scan for-loop range expressions to find variables that should be integers
    for (auto& child : root->children()) {
        if (!child) continue;
        collectForLoopRangeVars(child.get());
    }

    emitVariables();

    // Pre-scan all modules (recursively) to collect variables that will become group_input sockets
    // This must happen before emitting module functions so resolveExprTree knows
    // which variables should be group_input leaf nodes
    for (auto& child : root->children()) {
        if (!child) continue;
        if (child->type() == ASTNode::Type::Module) {
            auto* mod = dynamic_cast<ModuleNode*>(child.get());
            if (mod) {
                collectGroupInputVars(*mod);
            }
        }
    }

    // Also register top-level variables as group_input vars (they get sockets in build_geometry)
    for (const auto& [name, value] : variables_) {
        if (value.isNumber() || value.isBool() || value.isString() ||
            (value.isVector() && value.size() == 3)) {
            group_input_vars_.insert(name);
        }
    }

    // Classify modules as geometric or non-geometric (fixed-point iteration)
    // First collect all module definitions and mark them all as non-geometric
    for (auto& child : root->children()) {
        if (!child) continue;
        classifyModuleGeometry(child.get());
    }
    // Iteratively remove modules from non_geometric set if their body produces geometry
    // (Need multiple passes because a module might call another module that gets reclassified)
    bool changed = true;
    while (changed) {
        changed = false;
        std::function<void(ASTNode*)> reclassify = [&](ASTNode* node) {
            if (!node) return;
            if (node->type() == ASTNode::Type::Module) {
                auto* mod = dynamic_cast<ModuleNode*>(node);
                if (mod && non_geometric_modules_.count(mod->name())) {
                    if (nodeProducesGeometry(node)) {
                        non_geometric_modules_.erase(mod->name());
                        changed = true;
                    }
                }
            }
            for (auto& child : node->children()) {
                reclassify(child.get());
            }
        };
        for (auto& child : root->children()) {
            reclassify(child.get());
        }
    }

    // Emit module functions (recursively, to emit nested modules too)
    for (auto& child : root->children()) {
        if (!child) continue;
        emitModulesRecursive(child.get());
    }

    // Emit Python helper functions for recursive user-defined functions
    if (!python_helper_functions_.empty()) {
        emitPythonHelperFunctions();
    }

    // Emit main build_geometry function
    auto* rootNode = dynamic_cast<RootNode*>(root.get());
    if (rootNode) {
        emitBuildGeometry(*rootNode);
    }

    emitFooter();

    return code_.str();
}

void BlenderGenerator::emit(const std::string& line) {
    code_ << indent() << line << "\n";
}

// Sanitize OpenSCAD variable/parameter names for Python output
// Replaces leading '$' with '_' and appends '_' to Python reserved words
static std::string pyName(const std::string& name) {
    std::string result = name;
    if (!result.empty() && result[0] == '$') {
        result = "_" + result.substr(1);
    }
    // Python reserved words that could appear as OpenSCAD identifiers
    static const std::set<std::string> reserved = {
        "False", "None", "True", "and", "as", "assert", "async", "await",
        "break", "class", "continue", "def", "del", "elif", "else", "except",
        "finally", "for", "from", "global", "if", "import", "in", "is",
        "lambda", "nonlocal", "not", "or", "pass", "raise", "return",
        "try", "while", "with", "yield"
    };
    if (reserved.count(result)) {
        result += "_";
    }
    return result;
}

void BlenderGenerator::emitBlank() {
    code_ << "\n";
}

std::string BlenderGenerator::indent() const {
    return std::string(indent_ * 4, ' ');
}

std::string BlenderGenerator::newNodeId() {
    return "node_" + std::to_string(node_counter_++);
}

void BlenderGenerator::emitHeader() {
    emit("\"\"\"");
    emit("Blender Geometry Nodes script generated from OpenSCAD");
    emit("Generated by scad2blender");
    emit("\"\"\"");
    emitBlank();
    emit("import bpy");
    emit("import math");
    emit("import sys");
    emit("import os");
    emit("from mathutils import Vector, Matrix");
    emitBlank();
    emit("_module_call_count = 0");
    emit("_MODULE_CALL_LIMIT = 500");
    emit("_module_depth = {}");
    emit("_MAX_MODULE_DEPTH = 8");
    emitBlank();
}

void BlenderGenerator::emitHelperFunctions() {
    emit("def create_geometry_nodes_modifier(obj, name='GeometryNodes'):");
    indent_++;
    emit("\"\"\"Create and setup geometry nodes modifier\"\"\"");
    emit("modifier = obj.modifiers.new(name=name, type='NODES')");
    emit("node_group = bpy.data.node_groups.new(name=name, type='GeometryNodeTree')");
    emit("modifier.node_group = node_group");
    emit("return node_group, modifier");
    indent_--;
    emitBlank();

    emit("def add_input_socket(node_group, name, socket_type, default_value=None):");
    indent_++;
    emit("\"\"\"Add an input socket to the node group\"\"\"");
    emit("socket = node_group.interface.new_socket(name=name, in_out='INPUT', socket_type=socket_type)");
    emit("if default_value is not None and hasattr(socket, 'default_value'):");
    indent_++;
    emit("socket.default_value = default_value");
    indent_--;
    emit("return socket");
    indent_--;
    emitBlank();

    emit("def setup_node_tree(node_group):");
    indent_++;
    emit("\"\"\"Setup input/output nodes\"\"\"");
    emit("nodes = node_group.nodes");
    emit("links = node_group.links");
    emit("nodes.clear()");
    emitBlank();
    emit("group_input = nodes.new('NodeGroupInput')");
    emit("group_input.location = (-400, 0)");
    emitBlank();
    emit("group_output = nodes.new('NodeGroupOutput')");
    emit("group_output.location = (400, 0)");
    emitBlank();
    emit("# Add Geometry socket to output");
    emit("node_group.interface.new_socket(name='Geometry', in_out='OUTPUT', socket_type='NodeSocketGeometry')");
    emitBlank();
    emit("return nodes, links, group_input, group_output");
    indent_--;
    emitBlank();

    emit("def link_nodes(links, from_node, from_socket, to_node, to_socket):");
    indent_++;
    emit("\"\"\"Create a link between two node sockets\"\"\"");
    emit("if from_node is None or to_node is None:");
    indent_++;
    emit("return");
    indent_--;
    emit("# Try the specified socket name, fall back to alternatives");
    emit("try:");
    indent_++;
    emit("out_socket = from_node.outputs.get(from_socket)");
    emit("if out_socket is None:");
    indent_++;
    emit("# Try common alternatives");
    emit("for alt in ['Mesh', 'Geometry', 'Curve']:");
    indent_++;
    emit("out_socket = from_node.outputs.get(alt)");
    emit("if out_socket is not None:");
    indent_++;
    emit("break");
    indent_--;
    indent_--;
    indent_--;
    emit("if out_socket is None and len(from_node.outputs) > 0:");
    indent_++;
    emit("out_socket = from_node.outputs[0]  # Use first output as fallback");
    indent_--;
    emit("if out_socket is not None:");
    indent_++;
    emit("links.new(out_socket, to_node.inputs[to_socket])");
    indent_--;
    indent_--;
    emit("except Exception as e:");
    indent_++;
    emit("print(f'Link error: {e}')");
    indent_--;
    indent_--;
    emitBlank();

    // Helper to get geometry output socket from any node
    emit("def geo_out(node):");
    indent_++;
    emit("\"\"\"Get the geometry output socket from a node (handles ObjectInfo etc.)\"\"\"");
    emit("for name in ['Geometry', 'Mesh', 'Curve']:");
    indent_++;
    emit("s = node.outputs.get(name)");
    emit("if s is not None: return s");
    indent_--;
    emit("return node.outputs[0]");
    indent_--;
    emitBlank();

    // OpenSCAD rands() helper
    emit("def _scad_rands(min_val, max_val, num, seed=None):");
    indent_++;
    emit("\"\"\"OpenSCAD rands() — generate pseudo-random numbers\"\"\"");
    emit("import random");
    emit("rng = random.Random(seed)");
    emit("return [min_val + rng.random() * (max_val - min_val) for _ in range(int(num))]");
    indent_--;
    emitBlank();

    // OpenSCAD vector-safe arithmetic operations
    emit("def _vneg(a):");
    indent_++;
    emit("if isinstance(a, (list, tuple)):"); indent_++;
    emit("return type(a)(_vneg(x) for x in a)"); indent_--;
    emit("return -a if isinstance(a, (int, float)) else 0");
    indent_--;
    emitBlank();
    emit("def _vadd(a, b):");
    indent_++;
    emit("if isinstance(a, (list, tuple)) and isinstance(b, (list, tuple)):"); indent_++;
    emit("return [_vadd(x, y) for x, y in zip(a, b)]"); indent_--;
    emit("if isinstance(a, (list, tuple)):"); indent_++;
    emit("return [_vadd(x, b) for x in a]"); indent_--;
    emit("if isinstance(b, (list, tuple)):"); indent_++;
    emit("return [_vadd(a, x) for x in b]"); indent_--;
    emit("try:"); indent_++;
    emit("return a + b"); indent_--;
    emit("except TypeError:"); indent_++;
    emit("return 0"); indent_--;
    indent_--;
    emitBlank();
    emit("def _vsub(a, b):");
    indent_++;
    emit("if isinstance(a, (list, tuple)) and isinstance(b, (list, tuple)):"); indent_++;
    emit("return [_vsub(x, y) for x, y in zip(a, b)]"); indent_--;
    emit("if isinstance(a, (list, tuple)):"); indent_++;
    emit("return [_vsub(x, b) for x in a]"); indent_--;
    emit("if isinstance(b, (list, tuple)):"); indent_++;
    emit("return [_vsub(a, x) for x in b]"); indent_--;
    emit("try:"); indent_++;
    emit("return a - b"); indent_--;
    emit("except TypeError:"); indent_++;
    emit("return 0"); indent_--;
    indent_--;
    emitBlank();
    emit("def _vmul(a, b):");
    indent_++;
    emit("if isinstance(a, (list, tuple)) and isinstance(b, (list, tuple)):"); indent_++;
    emit("return [_vmul(x, y) for x, y in zip(a, b)]"); indent_--;
    emit("if isinstance(a, (list, tuple)):"); indent_++;
    emit("return [_vmul(x, b) for x in a]"); indent_--;
    emit("if isinstance(b, (list, tuple)):"); indent_++;
    emit("return [_vmul(a, x) for x in b]"); indent_--;
    emit("try:"); indent_++;
    emit("return a * b"); indent_--;
    emit("except TypeError:"); indent_++;
    emit("return 0"); indent_--;
    indent_--;
    emitBlank();
    emit("def _vdiv(a, b):");
    indent_++;
    emit("if isinstance(a, (list, tuple)) and isinstance(b, (list, tuple)):"); indent_++;
    emit("return [_vdiv(x, y) for x, y in zip(a, b)]"); indent_--;
    emit("if isinstance(a, (list, tuple)):"); indent_++;
    emit("return [_vdiv(x, b) for x in a]"); indent_--;
    emit("if isinstance(b, (list, tuple)):"); indent_++;
    emit("return [_vdiv(a, x) for x in b]"); indent_--;
    emit("try:"); indent_++;
    emit("return a / b if b != 0 else 0"); indent_--;
    emit("except (TypeError, ZeroDivisionError):"); indent_++;
    emit("return 0"); indent_--;
    indent_--;
    emitBlank();
    emit("def _vlt(a, b):");
    indent_++;
    emit("if isinstance(a, (list, tuple)):"); indent_++;
    emit("a = a[0] if a else 0"); indent_--;
    emit("if isinstance(b, (list, tuple)):"); indent_++;
    emit("b = b[0] if b else 0"); indent_--;
    emit("try:"); indent_++;
    emit("return a < b"); indent_--;
    emit("except TypeError:"); indent_++;
    emit("return False"); indent_--;
    indent_--;
    emit("def _vgt(a, b):");
    indent_++;
    emit("if isinstance(a, (list, tuple)):"); indent_++;
    emit("a = a[0] if a else 0"); indent_--;
    emit("if isinstance(b, (list, tuple)):"); indent_++;
    emit("b = b[0] if b else 0"); indent_--;
    emit("try:"); indent_++;
    emit("return a > b"); indent_--;
    emit("except TypeError:"); indent_++;
    emit("return False"); indent_--;
    indent_--;
    emitBlank();
}

void BlenderGenerator::emitVariables() {
    // Variables are now added as node group inputs in build_geometry
    // This function is kept for compatibility but doesn't emit global Python variables
}

bool BlenderGenerator::isSimpleVariableRef(const Value& value) {
    // Check if this is a simple variable reference (just a name, no operators)
    // that corresponds to a group_input socket
    if (!value.isExpression()) return false;
    const std::string& expr = value.toString();
    // If it contains operators or parentheses, it's a computed expression
    if (expr.find('+') != std::string::npos ||
        expr.find('-') != std::string::npos ||
        expr.find('*') != std::string::npos ||
        expr.find('/') != std::string::npos ||
        expr.find('(') != std::string::npos ||
        expr.find(' ') != std::string::npos) {
        return false;
    }
    // Only treat as a linkable variable ref if it has a group_input socket
    if (group_input_vars_.find(expr) == group_input_vars_.end()) {
        return false;
    }
    return true;
}

bool BlenderGenerator::exprTreeHasOnlyGroupInputVars(const ExprNodePtr& tree) {
    if (!tree) return true;
    switch (tree->kind) {
        case ExprNode::Kind::Literal:
            return true;
        case ExprNode::Kind::VarRef:
            return group_input_vars_.find(tree->var_name) != group_input_vars_.end();
        case ExprNode::Kind::UnaryOp:
            return exprTreeHasOnlyGroupInputVars(tree->left);
        case ExprNode::Kind::BinaryOp:
            return exprTreeHasOnlyGroupInputVars(tree->left) &&
                   exprTreeHasOnlyGroupInputVars(tree->right);
        case ExprNode::Kind::FunctionCall:
            for (const auto& arg : tree->func_args) {
                if (!exprTreeHasOnlyGroupInputVars(arg)) return false;
            }
            return true;
        case ExprNode::Kind::VectorLiteral:
            for (const auto& elem : tree->vec_elements) {
                if (!exprTreeHasOnlyGroupInputVars(elem)) return false;
            }
            return true;
        case ExprNode::Kind::Conditional:
            return exprTreeHasOnlyGroupInputVars(tree->left) &&
                   exprTreeHasOnlyGroupInputVars(tree->right) &&
                   (!tree->else_branch || exprTreeHasOnlyGroupInputVars(tree->else_branch));
        case ExprNode::Kind::ForLoop:
            return exprTreeHasOnlyGroupInputVars(tree->left) &&
                   exprTreeHasOnlyGroupInputVars(tree->right);
        case ExprNode::Kind::LetBinding:
            for (const auto& b : tree->let_bindings) {
                if (!exprTreeHasOnlyGroupInputVars(b.second)) return false;
            }
            return exprTreeHasOnlyGroupInputVars(tree->right);
    }
    return true;
}

bool BlenderGenerator::isRuntimePythonVar(const std::string& varName) const {
    std::string py = pyName(varName);
    return current_module_params_.find(py) != current_module_params_.end() ||
           loop_variables_.find(varName) != loop_variables_.end();
}

bool BlenderGenerator::exprTreeReferencesModuleParams(const ExprNodePtr& tree) {
    if (!tree) return false;
    switch (tree->kind) {
        case ExprNode::Kind::Literal:
            return false;
        case ExprNode::Kind::VarRef:
            return isRuntimePythonVar(tree->var_name);
        case ExprNode::Kind::UnaryOp:
            return exprTreeReferencesModuleParams(tree->left);
        case ExprNode::Kind::BinaryOp:
            return exprTreeReferencesModuleParams(tree->left) ||
                   exprTreeReferencesModuleParams(tree->right);
        case ExprNode::Kind::FunctionCall:
            for (const auto& arg : tree->func_args) {
                if (exprTreeReferencesModuleParams(arg)) return true;
            }
            return false;
        case ExprNode::Kind::VectorLiteral:
            for (const auto& elem : tree->vec_elements) {
                if (exprTreeReferencesModuleParams(elem)) return true;
            }
            return false;
        case ExprNode::Kind::Conditional:
            return exprTreeReferencesModuleParams(tree->left) ||
                   exprTreeReferencesModuleParams(tree->right) ||
                   (tree->else_branch && exprTreeReferencesModuleParams(tree->else_branch));
        case ExprNode::Kind::ForLoop:
            return exprTreeReferencesModuleParams(tree->left) ||
                   exprTreeReferencesModuleParams(tree->right);
        case ExprNode::Kind::LetBinding:
            for (const auto& b : tree->let_bindings) {
                if (exprTreeReferencesModuleParams(b.second)) return true;
            }
            return exprTreeReferencesModuleParams(tree->right);
    }
    return false;
}

void BlenderGenerator::emitSetInputOrLink(const std::string& nodeId, const std::string& inputName,
                                          const Value& value, const std::string& pythonValue) {
    // Try expression tree path first
    ExprNodePtr tree = resolveExprTree(value);
    if (tree && tree->hasVariableRefs()) {
        // Only use the node-tree path if all variable refs are group_input sockets.
        // Runtime Python variables (module params, loop vars) can't be linked as Blender nodes.
        if (exprTreeHasOnlyGroupInputVars(tree)) {
            auto result = emitExpressionNodeTree(tree);
            connectExprResultNamed(result, nodeId, inputName);
            return;
        }
        // If the tree references runtime Python variables (module params, loop vars),
        // emit a Python expression using runtime variable names.
        // If the tree is a simple VarRef to a module param, also handle the case
        // where the param might be a Blender node (passed from a call site that
        // linked it to group_input sockets).
        if (exprTreeReferencesModuleParams(tree)) {
            if (tree->kind == ExprNode::Kind::VarRef) {
                std::string varName = pyName(tree->var_name);
                emit("if hasattr(" + varName + ", 'outputs'):");
                indent_++;
                emit("links.new(" + varName + ".outputs['Value'], " + nodeId + ".inputs['" + inputName + "'])");
                indent_--;
                emit("else:");
                indent_++;
                emit(nodeId + ".inputs['" + inputName + "'].default_value = " + varName);
                indent_--;
            } else {
                std::string pyExpr = exprTreeToPython(tree);
                emit(nodeId + ".inputs['" + inputName + "'].default_value = " + pyExpr);
            }
            return;
        }
        // Fall through to use pythonValue (evaluated at compile time)
    }

    if (isSimpleVariableRef(value)) {
        std::string varName = value.toString();
        // Only link to group_input for variables that actually have group_input sockets
        if (group_input_vars_.find(varName) != group_input_vars_.end()) {
            std::string socketName = varName;
            if (varName[0] == '$') {
                socketName = varName.substr(1);  // Remove $ prefix for socket name
            }
            emit("links.new(group_input.outputs['" + socketName + "'], " + nodeId + ".inputs['" + inputName + "'])");
        } else if (isRuntimePythonVar(varName)) {
            // Runtime Python variable — might be a Blender node ref (if caller
            // linked it to group_input) or a scalar value
            std::string py = pyName(varName);
            emit("if hasattr(" + py + ", 'outputs'):");
            indent_++;
            emit("links.new(" + py + ".outputs['Value'], " + nodeId + ".inputs['" + inputName + "'])");
            indent_--;
            emit("else:");
            indent_++;
            emit(nodeId + ".inputs['" + inputName + "'].default_value = " + py);
            indent_--;
        } else {
            // Module-local variable — evaluate to concrete value
            double val = evaluateExpr(value);
            emit(nodeId + ".inputs['" + inputName + "'].default_value = " + pyDouble(val));
        }
    } else {
        // For computed expressions or literal values, use default_value
        emit(nodeId + ".inputs['" + inputName + "'].default_value = " + pythonValue);
    }
}

void BlenderGenerator::emitModuleFunction(ModuleNode& node) {
    in_module_ = true;
    module_uses_children_ = moduleUsesChildren(node);

    // Track current module's parameter names so body assignments that shadow
    // them can be suppressed (the parameter already provides the value)
    auto saved_module_params = current_module_params_;
    auto saved_loop_vars = loop_variables_;
    current_module_params_.clear();
    loop_variables_.clear();
    for (const auto& p : node.parameters()) {
        current_module_params_.insert(pyName(p));
    }

    // Collect captured parent module params (pre-computed in collectModulesRecursive)
    std::vector<std::string> captured_params;
    auto cpit = captured_parent_params_.find(node.name());
    if (cpit != captured_parent_params_.end()) {
        captured_params = cpit->second;
        for (const auto& cp : captured_params) {
            current_module_params_.insert(cp);  // Treat as runtime Python vars
        }
    }

    // Save existing variables and push parameter defaults so evaluateExprTree
    // can resolve references to module parameters (like mm_per_tooth, thickness, etc.)
    auto saved_variables = variables_;
    for (const auto& [pname, pval] : node.parameterDefaults()) {
        variables_[pname] = pval;
    }

    // Build function signature with parameters (deduplicate, keeping last occurrence)
    std::string params = "nodes, links, group_input, x_pos, y_pos";
    {
        // Collect unique params preserving last-occurrence order (OpenSCAD shadowing)
        std::vector<std::string> uniqueParams;
        std::set<std::string> seen;
        const auto& allParams = node.parameters();
        const auto& defaults = node.parameterDefaults();
        // Walk backwards to find last occurrence, then reverse for original order
        for (int i = static_cast<int>(allParams.size()) - 1; i >= 0; --i) {
            const std::string py = pyName(allParams[i]);
            if (seen.insert(py).second) {
                uniqueParams.push_back(py);
            }
        }
        std::reverse(uniqueParams.begin(), uniqueParams.end());
        for (const auto& p : uniqueParams) {
            // Look up the original (unsanitized) parameter name to find its default
            std::string defaultStr = "0.0";
            for (const auto& origParam : allParams) {
                if (pyName(origParam) == p) {
                    auto dit = defaults.find(origParam);
                    if (dit != defaults.end()) {
                        const Value& defVal = dit->second;
                        if (defVal.isBool()) {
                            defaultStr = defVal.toPython();
                        } else if (defVal.isNumber()) {
                            defaultStr = pyDouble(defVal.toNumber());
                        } else if (defVal.isExpression()) {
                            // Evaluate expression to a concrete number
                            double v = evaluateExpr(defVal);
                            defaultStr = std::to_string(v);
                        } else if (defVal.isVector()) {
                            // Evaluate each vector component to a concrete number
                            defaultStr = "(";
                            for (size_t vi = 0; vi < defVal.size(); ++vi) {
                                if (vi > 0) defaultStr += ", ";
                                if (defVal[vi].isExpression()) {
                                    defaultStr += pyDouble(evaluateExpr(defVal[vi]));
                                } else {
                                    defaultStr += pyDouble(defVal[vi].toNumber());
                                }
                            }
                            if (defVal.size() == 1) defaultStr += ",";
                            defaultStr += ")";
                        } else if (defVal.isString()) {
                            defaultStr = defVal.toPython();
                        }
                        // else leave as 0.0
                    }
                    break;
                }
            }
            params += ", " + p + "=" + defaultStr;
        }
    }
    // Add captured parent module params to signature
    for (const auto& cp : captured_params) {
        params += ", " + cp + "=0.0";
    }
    params += ", children_geo=None";

    emit("def module_" + node.name() + "(" + params + "):");
    indent_++;
    emit("\"\"\"OpenSCAD module: " + node.name() + "\"\"\"");
    emit("global _module_call_count");
    emit("_module_call_count += 1");
    emit("if _module_call_count > _MODULE_CALL_LIMIT:");
    indent_++;
    emit("return children_geo, y_pos");
    indent_--;
    // Per-module recursion depth tracking
    emit("_module_depth['" + node.name() + "'] = _module_depth.get('" + node.name() + "', 0) + 1");
    emit("if _module_depth['" + node.name() + "'] > _MAX_MODULE_DEPTH:");
    indent_++;
    emit("_module_depth['" + node.name() + "'] -= 1");
    emit("return children_geo, y_pos");
    indent_--;
    emit("start_y = y_pos");
    emit("last_geo = children_geo");
    emitBlank();

    // Process module body
    for (auto& child : node.children()) {
        if (!child) continue;
        if (!child->isDisabled() && !child->isBackground()) {
            child->accept(*this);
        }
    }

    emitBlank();
    emit("_module_depth['" + node.name() + "'] -= 1");
    emit("return last_geo, y_pos");
    indent_--;
    emitBlank();

    // Restore variables and module state
    variables_ = saved_variables;
    current_module_params_ = saved_module_params;
    loop_variables_ = saved_loop_vars;
    in_module_ = false;
    module_uses_children_ = false;
}

void BlenderGenerator::emitBuildGeometry(RootNode& node) {
    emit("def build_geometry(node_group):");
    indent_++;
    emit("\"\"\"Build the geometry nodes graph\"\"\"");
    emitBlank();

    // Add input sockets for all top-level SCAD variables BEFORE setting up node tree
    // This ensures group_input node will have the outputs available
    if (!variables_.empty()) {
        emit("# Add input sockets for OpenSCAD variables");
        // Snapshot to avoid iterator invalidation
        std::vector<std::pair<std::string, Value>> socketSnapshot(variables_.begin(), variables_.end());
        for (const auto& [name, value] : socketSnapshot) {
            // Only process top-level variables, not module-internal ones
            if (top_level_vars_.find(name) == top_level_vars_.end()) continue;

            std::string socketName = name;
            if (name[0] == '$') {
                socketName = name.substr(1);  // Remove $ prefix
            }

            // Track this as a group_input variable
            group_input_vars_.insert(name);

            // Determine socket type based on value type
            if (value.isBool()) {
                emit("add_input_socket(node_group, '" + socketName + "', 'NodeSocketBool', " + value.toPython() + ")");
            } else if (value.isNumber()) {
                // Use integer socket for variables used as for-loop range bounds
                if (for_loop_range_vars_.count(name) > 0) {
                    int intVal = (int)value.toNumber();
                    emit("add_input_socket(node_group, '" + socketName + "', 'NodeSocketInt', " + std::to_string(intVal) + ")");
                } else {
                    emit("add_input_socket(node_group, '" + socketName + "', 'NodeSocketFloat', " + value.toPython() + ")");
                }
            } else if (value.isString()) {
                emit("add_input_socket(node_group, '" + socketName + "', 'NodeSocketString', " + value.toPython() + ")");
            } else if (value.isVector()) {
                if (value.size() == 3) {
                    // Evaluate each component numerically to avoid expression references
                    std::string vecDefault = "(";
                    for (size_t i = 0; i < 3; ++i) {
                        if (i > 0) vecDefault += ", ";
                        vecDefault += pyDouble(value[i].toNumber());
                    }
                    vecDefault += ")";
                    emit("add_input_socket(node_group, '" + socketName + "', 'NodeSocketVector', " + vecDefault + ")");
                } else {
                    // For other vector sizes, use float for now
                    emit("# Vector " + socketName + " has non-standard size, skipping");
                }
            } else {
                emit("# Unsupported type for variable: " + socketName);
            }
        }
        emitBlank();
    }

    // Now setup the node tree - group_input will have the socket outputs
    emit("nodes, links, group_input, group_output = setup_node_tree(node_group)");
    emitBlank();

    // Emit Python variable assignments for group_input parameters
    // so that Python-level code (e.g. polyhedron vertex lists) can reference them
    if (!variables_.empty()) {
        // Snapshot to avoid iterator invalidation during iteration
        std::vector<std::pair<std::string, Value>> varSnapshot(variables_.begin(), variables_.end());
        // If there are Python helper functions, declare top-level vars as global
        // so the helper functions can access them
        if (!python_helper_functions_.empty()) {
            std::string globalDecl = "global ";
            bool first = true;
            for (const auto& [name, value] : varSnapshot) {
                if (top_level_vars_.find(name) == top_level_vars_.end()) continue;
                if (!first) globalDecl += ", ";
                globalDecl += pyName(name);
                first = false;
            }
            if (!first) emit(globalDecl);
        }
        emit("# Python variables for group_input parameters");
        for (const auto& [name, value] : varSnapshot) {
            // Only process top-level variables, not module-internal ones
            if (top_level_vars_.find(name) == top_level_vars_.end()) continue;

            std::string varName = pyName(name);
            if (value.isUndefined()) {
                emit(varName + " = None");
            } else if (value.isNumber() || value.isBool() || value.isString()) {
                emit(varName + " = " + value.toPython());
            } else if (value.isVector()) {
                // Emit Python tuple for vectors of any size
                std::string vecVal = "(";
                for (size_t i = 0; i < value.size(); ++i) {
                    if (i > 0) vecVal += ", ";
                    if (value[i].isString()) {
                        vecVal += value[i].toPython();
                    } else {
                        vecVal += pyDouble(value[i].toNumber());
                    }
                }
                if (value.size() == 1) vecVal += ",";
                vecVal += ")";
                emit(varName + " = " + vecVal);
            } else if (value.isExpression()) {
                // Try to evaluate expression to a concrete value
                double v = evaluateExpr(value);
                emit(varName + " = " + pyDouble(v));
            } else {
                // Fallback for any unhandled types — emit None to prevent NameError
                emit(varName + " = None  # unhandled type: " + std::to_string(static_cast<int>(value.type())));
            }
        }
        emitBlank();
    }

    emit("x_pos = 0");
    emit("y_pos = 0");
    emit("last_geo = None");
    emit("all_geometry = []  # Collect all top-level geometry");
    emitBlank();

    // Process top-level statements (excluding modules and assignments)
    // Collect geometry from each statement
    for (auto& child : node.children()) {
        if (!child) continue;
        if (child->type() != ASTNode::Type::Module &&
            child->type() != ASTNode::Type::Assignment &&
            child->type() != ASTNode::Type::FunctionDef &&
            !child->isDisabled() && !child->isBackground()) {
            emit("# --- Top-level statement ---");
            emit("last_geo = None");
            child->accept(*this);
            emit("if last_geo is not None:");
            indent_++;
            emit("all_geometry.append(last_geo)");
            indent_--;
            emitBlank();
        }
    }

    emitBlank();
    emit("# Join all top-level geometry");
    emit("if len(all_geometry) == 0:");
    indent_++;
    emit("pass  # No geometry to output");
    indent_--;
    emit("elif len(all_geometry) == 1:");
    indent_++;
    emit("link_nodes(links, all_geometry[0], 'Geometry', group_output, 'Geometry')");
    indent_--;
    emit("else:");
    indent_++;
    emit("join_node = nodes.new('GeometryNodeJoinGeometry')");
    emit("join_node.location = (x_pos, y_pos)");
    emit("for geo in all_geometry:");
    indent_++;
    emit("link_nodes(links, geo, 'Geometry', join_node, 'Geometry')");
    indent_--;
    emit("link_nodes(links, join_node, 'Geometry', group_output, 'Geometry')");
    indent_--;

    emitBlank();
    // Remove unconnected input sockets (variables only used inside DIFFERENCE temp groups)
    // But keep sockets for top-level variables used as Python variables (for-loops, if-statements, module calls)
    emit("# Clean up unconnected input sockets");
    {
        std::string keepSet = "_keep_vars = {";
        bool first = true;
        for (const auto& name : top_level_vars_) {
            std::string socketName = name;
            if (!socketName.empty() && socketName[0] == '$') {
                socketName = socketName.substr(1);
            }
            if (!first) keepSet += ", ";
            keepSet += "'" + socketName + "'";
            first = false;
        }
        keepSet += "}";
        emit(keepSet);
    }
    emit("for _n in nodes:");
    indent_++;
    emit("if _n.type == 'GROUP_INPUT':");
    indent_++;
    emit("_unused = [_o.name for _o in _n.outputs if _o.name and not _o.links and 'Virtual' not in _o.bl_idname and _o.name not in _keep_vars]");
    emit("for _uname in _unused:");
    indent_++;
    emit("for _item in list(node_group.interface.items_tree):");
    indent_++;
    emit("if hasattr(_item, 'item_type') and _item.item_type == 'SOCKET' and _item.in_out == 'INPUT' and _item.name == _uname:");
    indent_++;
    emit("node_group.interface.remove(_item)");
    emit("break");
    indent_--;
    indent_--;
    indent_--;
    emit("break");
    indent_--;
    indent_--;

    indent_--;
    emitBlank();
}

void BlenderGenerator::emitFooter() {
    emit("def main():");
    indent_++;
    emit("\"\"\"Main entry point\"\"\"");
    emit("# Create mesh object");
    emit("mesh = bpy.data.meshes.new('OpenSCAD_Mesh')");
    emit("obj = bpy.data.objects.new('OpenSCAD_Object', mesh)");
    emit("bpy.context.collection.objects.link(obj)");
    emitBlank();
    emit("# Create geometry nodes");
    emit("node_group, modifier = create_geometry_nodes_modifier(obj)");
    emit("build_geometry(node_group)");
    emitBlank();
    emit("# Set modifier input values from socket defaults");
    emit("for item in node_group.interface.items_tree:");
    indent_++;
    emit("if hasattr(item, 'item_type') and item.item_type == 'SOCKET' and item.in_out == 'INPUT':");
    indent_++;
    emit("if hasattr(item, 'default_value') and hasattr(item, 'identifier'):");
    indent_++;
    emit("try:");
    indent_++;
    emit("modifier[item.identifier] = item.default_value");
    indent_--;
    emit("except Exception:");
    indent_++;
    emit("pass");
    indent_--;
    indent_--;
    indent_--;
    indent_--;
    emitBlank();
    emit("# Select the object");
    emit("bpy.context.view_layer.objects.active = obj");
    emit("obj.select_set(True)");
    emitBlank();
    emit("# Evaluate the geometry nodes modifier to produce mesh");
    emit("depsgraph = bpy.context.evaluated_depsgraph_get()");
    emit("eval_obj = obj.evaluated_get(depsgraph)");
    emit("eval_mesh = eval_obj.to_mesh()");
    emitBlank();
    emit("# Copy evaluated mesh back so the object has real geometry for export");
    emit("obj.data = bpy.data.meshes.new_from_object(eval_obj)");
    emit("eval_obj.to_mesh_clear()");
    emitBlank();
    emit("# Remove the modifier so the exporter uses the baked mesh");
    emit("obj.modifiers.clear()");
    emit("bpy.data.node_groups.remove(node_group)");
    emitBlank();
    emit("# Clean up temporary boolean objects");
    emit("for _o in list(bpy.data.objects):");
    indent_++;
    emit("if _o.name.startswith('_diff_') or _o.name.startswith('_dxf_') or _o.name.startswith('_surf_'):");
    indent_++;
    emit("_mesh = _o.data");
    emit("bpy.data.objects.remove(_o, do_unlink=True)");
    emit("if _mesh and _mesh.users == 0:");
    indent_++;
    emit("if isinstance(_mesh, bpy.types.Mesh):");
    indent_++;
    emit("bpy.data.meshes.remove(_mesh)");
    indent_--;
    emit("elif isinstance(_mesh, bpy.types.Curve):");
    indent_++;
    emit("bpy.data.curves.remove(_mesh)");
    indent_--;
    indent_--;
    indent_--;
    indent_--;
    emit("for _ng in list(bpy.data.node_groups):");
    indent_++;
    emit("if _ng.name.startswith('_diff_') and _ng.users == 0:");
    indent_++;
    emit("bpy.data.node_groups.remove(_ng)");
    indent_--;
    indent_--;
    emitBlank();
    emit("# Determine STL output path from script filename");
    emit("script_path = os.path.abspath(__file__)");
    emit("stl_path = os.path.splitext(script_path)[0] + '.stl'");
    emitBlank();
    emit("# Export to STL");
    emit("bpy.ops.object.select_all(action='DESELECT')");
    emit("obj.select_set(True)");
    emit("try:");
    indent_++;
    emit("bpy.ops.export_mesh.stl(filepath=stl_path, use_selection=True)");
    indent_--;
    emit("except (AttributeError, RuntimeError):");
    indent_++;
    emit("bpy.ops.wm.stl_export(filepath=stl_path, export_selected_objects=True)");
    indent_--;
    emit("print(f'Exported STL to: {stl_path}')");
    indent_--;
    emitBlank();
    emit("if __name__ == '__main__':");
    indent_++;
    emit("main()");
    indent_--;
}

// Visitor implementations
void BlenderGenerator::visit(RootNode& node) {
    // Root is handled by emitBuildGeometry
}

void BlenderGenerator::visit(PrimitiveNode& node) {
    switch (node.type()) {
        case ASTNode::Type::Cube:
            emitCube(node.args());
            break;
        case ASTNode::Type::Sphere:
            emitSphere(node.args());
            break;
        case ASTNode::Type::Cylinder:
            emitCylinder(node.args());
            break;
        case ASTNode::Type::Circle:
            emitCircle(node.args());
            break;
        case ASTNode::Type::Square:
            emitSquare(node.args());
            break;
        case ASTNode::Type::Polyhedron:
            emitPolyhedron(node.args());
            break;
        case ASTNode::Type::Polygon:
            emitPolygon(node.args());
            break;
        case ASTNode::Type::Text:
            emitText(node.args());
            break;
        case ASTNode::Type::Import:
            emitImport(node.args());
            break;
        case ASTNode::Type::Surface:
            emitSurface(node.args());
            break;
        default:
            emit("# Unsupported primitive type");
            break;
    }
}

void BlenderGenerator::visit(TransformNode& node) {
    switch (node.type()) {
        case ASTNode::Type::Translate:
            processChildren(node);
            emitTranslate(node.args());
            break;
        case ASTNode::Type::Rotate:
            processChildren(node);
            emitRotate(node.args());
            break;
        case ASTNode::Type::Scale:
            processChildren(node);
            emitScale(node.args());
            break;
        case ASTNode::Type::Mirror:
            processChildren(node);
            emitMirror(node.args());
            break;
        case ASTNode::Type::Color:
            // Color is not directly supported in Geometry Nodes,
            // just process children and pass geometry through
            emit("# Color transform (visual only, not applied in Geometry Nodes)");
            processChildren(node);
            break;
        case ASTNode::Type::Offset:
            processChildren(node);
            emitOffset(node.args());
            break;
        case ASTNode::Type::Hull: {
            bool wasInHull = in_hull_;
            in_hull_ = true;
            processChildren(node);
            in_hull_ = wasInHull;
            emitHull(node.args());
            break;
        }
        case ASTNode::Type::Minkowski:
            processChildren(node);
            emitMinkowski(node.args());
            break;
        case ASTNode::Type::Roof:
            processChildren(node);
            emitRoof(node.args());
            break;
        default:
            emit("# Unsupported transform type");
            processChildren(node);
            break;
    }
}

void BlenderGenerator::visit(BooleanNode& node) {
    emitBooleanOp(node);
}

void BlenderGenerator::visit(ExtrudeNode& node) {
    processChildren(node);

    switch (node.type()) {
        case ASTNode::Type::LinearExtrude:
            emitLinearExtrude(node.args());
            break;
        case ASTNode::Type::RotateExtrude:
            emitRotateExtrude(node.args());
            break;
        default:
            emit("# Unsupported extrude type");
            break;
    }
}

void BlenderGenerator::visit(ModuleNode& node) {
    // Module definitions are handled separately
}

void BlenderGenerator::visit(ModuleCallNode& node) {
    if (defined_modules_.find(node.name()) != defined_modules_.end()) {
        // Skip non-geometric modules (Echo, HelpTxt, MO, InfoTxt, etc.)
        if (non_geometric_modules_.count(node.name())) {
            return;
        }
        // Call a user-defined module
        std::string nodeId = newNodeId();
        emit("# Call module: " + node.name());

        // If the module call has children (e.g. outline(wall=2) circle(15)),
        // process them first to produce geometry that becomes children_geo
        if (!node.children().empty()) {
            emit("# Process children geometry for " + node.name());
            std::string savedGeo = "saved_geo_" + std::to_string(node_counter_++);
            emit(savedGeo + " = last_geo");
            emit("last_geo = None");
            for (auto& child : node.children()) {
                if (!child) continue;
                if (!child->isDisabled() && !child->isBackground()) {
                    child->accept(*this);
                }
            }
            emit("children_geo_tmp = last_geo");
            emit("last_geo = " + savedGeo);
        }

        // Build argument list
        std::string argStr = "nodes, links, group_input, x_pos, y_pos";

        // Get the module's parameter names for positional arg mapping
        const auto& modParams = defined_modules_[node.name()];

        // Build positional-to-param mapping that skips params already given by name.
        // In OpenSCAD, positional args fill unspecified params in declaration order.
        std::set<std::string> namedArgs;
        for (const auto& arg : node.args()) {
            if (!arg.first.empty() && arg.first[0] != '_') {
                namedArgs.insert(arg.first);
            }
        }
        std::vector<int> posToParamIdx;
        for (int pi = 0; pi < static_cast<int>(modParams.size()); ++pi) {
            if (namedArgs.find(modParams[pi]) == namedArgs.end()) {
                posToParamIdx.push_back(pi);
            }
        }

        // Add arguments from the call (both positional and named)
        for (const auto& arg : node.args()) {
            std::string paramName = arg.first;
            if (paramName[0] == '_') {
                // Positional arg: map to next unspecified module parameter
                int posIdx = std::stoi(paramName.substr(1));
                if (posIdx >= 0 && posIdx < static_cast<int>(posToParamIdx.size())) {
                    paramName = modParams[posToParamIdx[posIdx]];
                } else {
                    continue;  // Out of range, skip
                }
            }
            const Value& v = arg.second;
            std::string valStr;
            if (v.isUndefined()) {
                valStr = "0";
            } else if (v.isVector()) {
                // Convert vector with Undefined components to use 0
                valStr = "(";
                for (size_t vi = 0; vi < v.size(); ++vi) {
                    if (vi > 0) valStr += ", ";
                    if (v[vi].isUndefined()) {
                        valStr += "0";
                    } else if (v[vi].isExpression() && v[vi].exprTree()) {
                        valStr += exprTreeToPython(v[vi].exprTree());
                    } else if (v[vi].exprTree() && v[vi].exprTree()->hasVariableRefs()) {
                        valStr += exprTreeToPython(v[vi].exprTree());
                    } else {
                        valStr += v[vi].toPython();
                    }
                }
                if (v.size() == 1) valStr += ",";
                valStr += ")";
            } else if (v.isExpression() && v.exprTree()) {
                // Use expression tree for proper Python output
                // (toPython() may mangle variable names as "unsafe")
                ExprNodePtr tree = resolveExprTree(v);
                if (tree && tree->hasVariableRefs()) {
                    // Module functions are plain Python — they do arithmetic on
                    // parameters (e.g. e-1, 360/e).  Always resolve to concrete
                    // Python values, never Blender node objects.
                    valStr = exprTreeToPython(tree);
                } else if (tree) {
                    double numVal = evaluateExprTree(tree);
                    valStr = pyDouble(numVal);
                } else {
                    valStr = v.toPython();
                }
            } else {
                valStr = v.toPython();
            }
            argStr += ", " + pyName(paramName) + "=" + valStr;
        }

        // Pass captured parent module params to nested modules
        auto cpit = captured_parent_params_.find(node.name());
        if (cpit != captured_parent_params_.end()) {
            for (const auto& cp : cpit->second) {
                argStr += ", " + cp + "=" + cp;
            }
        }

        if (!node.children().empty()) {
            argStr += ", children_geo=children_geo_tmp";
        } else {
            argStr += ", children_geo=last_geo";
        }

        emit("try:");
        indent_++;
        emit(nodeId + ", y_pos = module_" + node.name() + "(" + argStr + ")");
        emit("last_geo = " + nodeId);
        indent_--;
        emit("except Exception:");
        indent_++;
        emit("pass  # module " + node.name() + " failed");
        indent_--;
        emit("x_pos += 200");
    } else {
        emit("# Unknown module: " + node.name());
    }
}

// ─── Instance on Points helpers for for-loop optimization ───

// Check if an expression tree references a specific variable name
static bool exprTreeReferencesVar(const ExprNodePtr& tree, const std::string& varName) {
    if (!tree) return false;
    switch (tree->kind) {
        case ExprNode::Kind::VarRef:
            return tree->var_name == varName;
        case ExprNode::Kind::Literal:
            return false;
        case ExprNode::Kind::UnaryOp:
            return exprTreeReferencesVar(tree->left, varName);
        case ExprNode::Kind::BinaryOp:
            return exprTreeReferencesVar(tree->left, varName) ||
                   exprTreeReferencesVar(tree->right, varName);
        case ExprNode::Kind::FunctionCall:
            for (const auto& arg : tree->func_args) {
                if (exprTreeReferencesVar(arg, varName)) return true;
            }
            return false;
        case ExprNode::Kind::VectorLiteral:
            for (const auto& elem : tree->vec_elements) {
                if (exprTreeReferencesVar(elem, varName)) return true;
            }
            return false;
        case ExprNode::Kind::Conditional:
            return exprTreeReferencesVar(tree->left, varName) ||
                   exprTreeReferencesVar(tree->right, varName) ||
                   exprTreeReferencesVar(tree->else_branch, varName);
        case ExprNode::Kind::ForLoop:
        case ExprNode::Kind::LetBinding:
            return exprTreeReferencesVar(tree->left, varName) ||
                   exprTreeReferencesVar(tree->right, varName);
    }
    return false;
}

// Check if a Value references a specific variable name
static bool valueReferencesVar(const Value& value, const std::string& varName) {
    if (value.isExpression()) {
        if (value.toString() == varName) return true;
        if (value.exprTree()) {
            return exprTreeReferencesVar(value.exprTree(), varName);
        }
    }
    if (value.isVector()) {
        for (size_t i = 0; i < value.size(); ++i) {
            if (valueReferencesVar(value[i], varName)) return true;
        }
    }
    return false;
}

// Check if any node in the subtree references a specific variable in its arguments
static bool subtreeReferencesVar(const ASTNodePtr& node, const std::string& varName) {
    if (!node) return false;

    // Check arguments for node types that have them
    switch (node->type()) {
        case ASTNode::Type::Translate:
        case ASTNode::Type::Rotate:
        case ASTNode::Type::Scale:
        case ASTNode::Type::Mirror:
        case ASTNode::Type::Color:
        case ASTNode::Type::Offset:
        case ASTNode::Type::Resize:
        case ASTNode::Type::Multmatrix: {
            auto* transform = dynamic_cast<TransformNode*>(node.get());
            if (transform) {
                for (const auto& kv : transform->args()) {
                    if (valueReferencesVar(kv.second, varName)) return true;
                }
            }
            break;
        }
        case ASTNode::Type::Cube:
        case ASTNode::Type::Sphere:
        case ASTNode::Type::Cylinder:
        case ASTNode::Type::Circle:
        case ASTNode::Type::Square:
        case ASTNode::Type::Polygon:
        case ASTNode::Type::Text:
        case ASTNode::Type::Polyhedron: {
            auto* prim = dynamic_cast<PrimitiveNode*>(node.get());
            if (prim) {
                for (const auto& kv : prim->args()) {
                    if (valueReferencesVar(kv.second, varName)) return true;
                }
            }
            break;
        }
        case ASTNode::Type::LinearExtrude:
        case ASTNode::Type::RotateExtrude: {
            auto* ext = dynamic_cast<ExtrudeNode*>(node.get());
            if (ext) {
                for (const auto& kv : ext->args()) {
                    if (valueReferencesVar(kv.second, varName)) return true;
                }
            }
            break;
        }
        case ASTNode::Type::ModuleCall: {
            auto* call = dynamic_cast<ModuleCallNode*>(node.get());
            if (call) {
                for (const auto& kv : call->args()) {
                    if (valueReferencesVar(kv.second, varName)) return true;
                }
            }
            break;
        }
        case ASTNode::Type::ForLoop: {
            auto* loop = dynamic_cast<ForLoopNode*>(node.get());
            if (loop) {
                if (valueReferencesVar(loop->range(), varName)) return true;
                // If the inner loop shadows the same variable, stop recursion
                if (loop->variable() == varName) return false;
            }
            break;
        }
        default:
            break;
    }

    // Recursively check children
    for (const auto& child : node->children()) {
        if (subtreeReferencesVar(child, varName)) return true;
    }
    return false;
}

// Information about an instanceable rotation array pattern
struct RotationArrayInfo {
    bool valid = false;
    int rotComponent = -1;  // 0=X, 1=Y, 2=Z
    double rangeStart = 0;
    double rangeEnd = 0;
    double rangeStep = 1;
    int iterCount = 0;
    // Other (constant) rotation components in degrees
    double otherRot[3] = {0, 0, 0};
};

// Detect if a for-loop is a "rotation array" pattern:
//   for (var = [start:step:end]) rotate([..., var, ...]) { body }
// where body does NOT reference var, and var appears in exactly one rotation component.
static RotationArrayInfo detectRotationArrayPattern(ForLoopNode& node) {
    RotationArrayInfo info;

    const Value& range = node.range();
    if (!range.isRange()) return info;

    // Range must be fully constant (no runtime expressions in bounds)
    if (range.rangeStartExpr() && range.rangeStartExpr()->hasVariableRefs()) return info;
    if (range.rangeEndExpr() && range.rangeEndExpr()->hasVariableRefs()) return info;
    if (range.rangeStepExpr() && range.rangeStepExpr()->hasVariableRefs()) return info;

    info.rangeStart = range.rangeStart();
    info.rangeEnd = range.rangeEnd();
    info.rangeStep = range.rangeStep();

    if (info.rangeStep == 0) return info;
    info.iterCount = static_cast<int>((info.rangeEnd - info.rangeStart) / info.rangeStep) + 1;
    if (info.iterCount < 2 || info.iterCount > 1000) return info;

    // The for loop must have exactly one direct child
    auto& children = node.children();
    if (children.size() != 1) return info;
    auto& child = children[0];
    if (!child || child->type() != ASTNode::Type::Rotate) return info;

    auto* rotateNode = dynamic_cast<TransformNode*>(child.get());
    if (!rotateNode) return info;

    // Get the rotation argument (key "a" or positional "_0")
    const auto& rotArgs = rotateNode->args();
    Value rotVal;
    {
        auto it = rotArgs.find("a");
        if (it == rotArgs.end()) it = rotArgs.find("_0");
        if (it == rotArgs.end()) return info;
        rotVal = it->second;
    }

    if (!rotVal.isVector() || rotVal.size() < 3) return info;

    const std::string& loopVar = node.variable();
    int varComponent = -1;

    for (int i = 0; i < 3; ++i) {
        if (valueReferencesVar(rotVal[i], loopVar)) {
            if (varComponent != -1) return info;  // Variable used in multiple components
            varComponent = i;
            // Must be a simple direct reference to the variable
            bool isSimple = false;
            if (rotVal[i].isExpression() && rotVal[i].toString() == loopVar) {
                isSimple = true;
            } else if (rotVal[i].exprTree() &&
                       rotVal[i].exprTree()->kind == ExprNode::Kind::VarRef &&
                       rotVal[i].exprTree()->var_name == loopVar) {
                isSimple = true;
            }
            if (!isSimple) return info;
        } else {
            // Constant component — extract its value
            double val = 0;
            if (rotVal[i].isNumber()) {
                val = rotVal[i].toNumber();
            } else if (rotVal[i].isExpression() && rotVal[i].exprTree()) {
                // Can't use non-constant values in the static rotation
                if (rotVal[i].exprTree()->hasVariableRefs()) return info;
            }
            info.otherRot[i] = val;
        }
    }

    if (varComponent == -1) return info;

    // Check that no children below the Rotate reference the loop variable
    for (const auto& rotChild : child->children()) {
        if (subtreeReferencesVar(rotChild, loopVar)) return info;
    }

    info.valid = true;
    info.rotComponent = varComponent;
    return info;
}

// ─── Scale mirror pattern detection (for vector loops) ───
// Detects: for (var = [v0, v1, ...]) body where var is ONLY used in Scale transforms,
// in exactly one component (e.g., scale([1, var, 1])). The body can be arbitrarily nested.
struct ScaleArrayInfo {
    bool valid = false;
    int scaleComponent = -1;  // 0=X, 1=Y, 2=Z
    std::vector<double> values;  // The vector values
    double otherScale[3] = {1, 1, 1};  // Other (constant) scale components
};

// Scan a subtree to check if a variable is used ONLY in Scale transforms.
// Returns the component index (0-2) if used consistently in one component, or -1 if invalid.
struct ScaleUsageScan {
    bool valid = true;
    bool found = false;
    int component = -1;
    double otherScale[3] = {1, 1, 1};
};

static void scanScaleUsage(const ASTNodePtr& node, const std::string& varName, ScaleUsageScan& scan) {
    if (!node || !scan.valid) return;

    // If this is a ForLoop that shadows the variable, stop recursion
    if (node->type() == ASTNode::Type::ForLoop) {
        auto* loop = dynamic_cast<ForLoopNode*>(node.get());
        if (loop && loop->variable() == varName) return;
        // Check range for variable reference (would make it invalid)
        if (loop && valueReferencesVar(loop->range(), varName)) {
            scan.valid = false;
            return;
        }
    }

    if (node->type() == ASTNode::Type::Scale) {
        auto* scaleNode = dynamic_cast<TransformNode*>(node.get());
        if (scaleNode) {
            const auto& args = scaleNode->args();
            auto it = args.find("v");
            if (it == args.end()) it = args.find("_0");
            if (it != args.end() && it->second.isVector() && it->second.size() >= 3) {
                const Value& scaleVal = it->second;
                for (int i = 0; i < 3; ++i) {
                    if (valueReferencesVar(scaleVal[i], varName)) {
                        // Must be simple VarRef
                        bool isSimple = (scaleVal[i].isExpression() && scaleVal[i].toString() == varName) ||
                                        (scaleVal[i].exprTree() &&
                                         scaleVal[i].exprTree()->kind == ExprNode::Kind::VarRef &&
                                         scaleVal[i].exprTree()->var_name == varName);
                        if (!isSimple) { scan.valid = false; return; }
                        if (scan.component != -1 && scan.component != i) { scan.valid = false; return; }
                        scan.component = i;
                        scan.found = true;
                        // Record other components
                        for (int j = 0; j < 3; ++j) {
                            if (j != i && scaleVal[j].isNumber()) {
                                scan.otherScale[j] = scaleVal[j].toNumber();
                            }
                        }
                    }
                }
            }
        }
    } else {
        // For non-Scale nodes, check if this node's arguments reference varName (invalid)
        auto checkArgs = [&](const Arguments& args) {
            for (const auto& kv : args) {
                if (valueReferencesVar(kv.second, varName)) { scan.valid = false; return; }
            }
        };
        switch (node->type()) {
            case ASTNode::Type::Translate:
            case ASTNode::Type::Rotate:
            case ASTNode::Type::Mirror:
            case ASTNode::Type::Color:
            case ASTNode::Type::Offset:
            case ASTNode::Type::Resize:
            case ASTNode::Type::Multmatrix: {
                auto* t = dynamic_cast<TransformNode*>(node.get());
                if (t) checkArgs(t->args());
                break;
            }
            case ASTNode::Type::Cube:
            case ASTNode::Type::Sphere:
            case ASTNode::Type::Cylinder:
            case ASTNode::Type::Circle:
            case ASTNode::Type::Square:
            case ASTNode::Type::Polygon:
            case ASTNode::Type::Text:
            case ASTNode::Type::Polyhedron: {
                auto* p = dynamic_cast<PrimitiveNode*>(node.get());
                if (p) checkArgs(p->args());
                break;
            }
            case ASTNode::Type::LinearExtrude:
            case ASTNode::Type::RotateExtrude: {
                auto* e = dynamic_cast<ExtrudeNode*>(node.get());
                if (e) checkArgs(e->args());
                break;
            }
            case ASTNode::Type::ModuleCall: {
                auto* m = dynamic_cast<ModuleCallNode*>(node.get());
                if (m) checkArgs(m->args());
                break;
            }
            default: break;
        }
    }

    if (!scan.valid) return;

    // Recurse into children
    for (const auto& child : node->children()) {
        scanScaleUsage(child, varName, scan);
    }
}

static ScaleArrayInfo detectScaleArrayPattern(ForLoopNode& node) {
    ScaleArrayInfo info;

    const Value& range = node.range();
    if (!range.isVector()) return info;

    // All vector elements must be constant numbers
    for (size_t i = 0; i < range.size(); ++i) {
        if (!range[i].isNumber()) return info;
        info.values.push_back(range[i].toNumber());
    }
    if (info.values.size() < 2 || info.values.size() > 100) return info;

    const std::string& loopVar = node.variable();

    // Scan entire body subtree for variable usage
    ScaleUsageScan scan;
    for (const auto& child : node.children()) {
        scanScaleUsage(child, loopVar, scan);
    }

    if (!scan.valid || !scan.found || scan.component == -1) return info;

    info.valid = true;
    info.scaleComponent = scan.component;
    for (int i = 0; i < 3; ++i) info.otherScale[i] = scan.otherScale[i];
    return info;
}

// ─── Data-driven instance array pattern detection (for range loops) ───
// Detects: for (var = [start:step:end])
//              translate([f(var), ...])
//                  [constant_transforms]
//                      single_primitive(with at most one var-dependent param)
struct InstanceArrayInfo {
    bool valid = false;

    // Translate node (position depends on loop var)
    TransformNode* translateNode = nullptr;

    // Constant transforms between translate and primitive
    std::vector<ASTNode*> constantChain;

    // The terminal primitive
    PrimitiveNode* primitiveNode = nullptr;

    // Which primitive arg varies (if any) — key in Arguments map
    std::string varyingPrimArg;
};

static InstanceArrayInfo detectInstanceArrayPattern(ForLoopNode& node) {
    InstanceArrayInfo info;

    const Value& range = node.range();
    if (!range.isRange()) return info;

    // Must have exactly one child (possibly wrapped in an implicit Union)
    if (node.children().size() != 1) return info;

    const std::string& loopVar = node.variable();
    ASTNode* current = node.children()[0].get();

    // Unwrap implicit Union/Intersection wrappers with single child
    while (current && (current->type() == ASTNode::Type::Union ||
                       current->type() == ASTNode::Type::Intersection) &&
           current->children().size() == 1) {
        current = current->children()[0].get();
    }
    if (!current) return info;

    // First child must be Translate that references the loop variable
    if (current->type() != ASTNode::Type::Translate) return info;
    auto* translate = dynamic_cast<TransformNode*>(current);
    if (!translate) return info;

    bool translateRefVar = false;
    for (const auto& kv : translate->args()) {
        if (valueReferencesVar(kv.second, loopVar)) translateRefVar = true;
    }
    if (!translateRefVar) return info;

    info.translateNode = translate;
    if (current->children().size() != 1) return info;
    current = current->children()[0].get();

    // Walk through constant transforms (Rotate, Scale, Mirror that don't ref loop var)
    while (current && current->children().size() == 1) {
        bool isTransform = false;
        switch (current->type()) {
            case ASTNode::Type::Rotate:
            case ASTNode::Type::Scale:
            case ASTNode::Type::Mirror:
                isTransform = true;
                break;
            default:
                break;
        }
        if (!isTransform) break;

        auto* tNode = dynamic_cast<TransformNode*>(current);
        if (tNode) {
            for (const auto& kv : tNode->args()) {
                if (valueReferencesVar(kv.second, loopVar)) return info;  // Not constant
            }
        }

        info.constantChain.push_back(current);
        current = current->children()[0].get();
    }

    // Terminal must be a primitive with no children
    auto* prim = dynamic_cast<PrimitiveNode*>(current);
    if (!prim || !current->children().empty()) return info;

    info.primitiveNode = prim;

    // Check which primitive args reference the loop variable (at most one)
    int varyingCount = 0;
    for (const auto& kv : prim->args()) {
        if (valueReferencesVar(kv.second, loopVar)) {
            info.varyingPrimArg = kv.first;
            varyingCount++;
        }
    }
    if (varyingCount > 1) return info;

    info.valid = true;
    return info;
}

void BlenderGenerator::visit(ForLoopNode& node) {
    const Value& range = node.range();

    if (range.isRange()) {
        // ─── Check for Instance on Points rotation array pattern ───
        // Detect: for (var = [start:step:end]) rotate([..., var, ...]) { body }
        // where body doesn't reference var. Emit as Geo Nodes Instance on Points
        // instead of a Python loop.
        RotationArrayInfo rotInfo = detectRotationArrayPattern(node);
        if (rotInfo.valid) {
            emit("# Rotation array (Instance on Points): " + node.variable() +
                 " [" + pyDouble(rotInfo.rangeStart) + ":" + pyDouble(rotInfo.rangeStep) +
                 ":" + pyDouble(rotInfo.rangeEnd) + "] — " +
                 std::to_string(rotInfo.iterCount) + " instances");

            // Get the Rotate node's children — this is the body geometry
            auto& rotateChildren = node.children()[0]->children();

            // Emit the body geometry (children of the rotate) without the loop variable
            for (auto& bodyChild : rotateChildren) {
                if (bodyChild) bodyChild->accept(*this);
            }

            // Now last_geo holds the base geometry. Instance it N times with rotation.

            // Create N points at origin using MeshLine with zero offset
            std::string lineId = newNodeId();
            emit(lineId + " = nodes.new('GeometryNodeMeshLine')");
            emit(lineId + ".location = (x_pos, y_pos - 200)");
            emit(lineId + ".mode = 'OFFSET'");
            emit(lineId + ".inputs['Count'].default_value = " + std::to_string(rotInfo.iterCount));
            emit(lineId + ".inputs['Offset'].default_value = (0, 0, 0)");
            emit("x_pos += 200");

            // Instance on Points: place the base geometry on each point
            std::string instanceId = newNodeId();
            emit(instanceId + " = nodes.new('GeometryNodeInstanceOnPoints')");
            emit(instanceId + ".location = (x_pos, y_pos)");
            emit("links.new(" + lineId + ".outputs['Mesh'], " + instanceId + ".inputs['Points'])");
            emit("link_nodes(links, last_geo, 'Geometry', " + instanceId + ", 'Instance')");
            emit("x_pos += 200");

            // Compute per-instance rotation: Index * step + start (in radians)
            std::string indexId = newNodeId();
            emit(indexId + " = nodes.new('GeometryNodeInputIndex')");
            emit(indexId + ".location = (x_pos, y_pos - 350)");

            // Index * step_radians
            double stepRad = rotInfo.rangeStep * M_PI / 180.0;
            std::string mulId = newNodeId();
            emit(mulId + " = nodes.new('ShaderNodeMath')");
            emit(mulId + ".operation = 'MULTIPLY'");
            emit(mulId + ".location = (x_pos, y_pos - 350)");
            emit("links.new(" + indexId + ".outputs['Index'], " + mulId + ".inputs[0])");
            emit(mulId + ".inputs[1].default_value = " + pyDouble(stepRad));

            // + start_radians
            double startRad = rotInfo.rangeStart * M_PI / 180.0;
            std::string angleNodeId = mulId;
            if (std::abs(startRad) > 0.0001) {
                std::string addId = newNodeId();
                emit(addId + " = nodes.new('ShaderNodeMath')");
                emit(addId + ".operation = 'ADD'");
                emit(addId + ".location = (x_pos, y_pos - 350)");
                emit("links.new(" + mulId + ".outputs['Value'], " + addId + ".inputs[0])");
                emit(addId + ".inputs[1].default_value = " + pyDouble(startRad));
                angleNodeId = addId;
            }

            // Build rotation vector with the varying component and constant others
            std::string combineId = newNodeId();
            emit(combineId + " = nodes.new('ShaderNodeCombineXYZ')");
            emit(combineId + ".location = (x_pos, y_pos - 200)");
            const char* components[] = {"X", "Y", "Z"};
            for (int i = 0; i < 3; ++i) {
                if (i == rotInfo.rotComponent) {
                    emit("links.new(" + angleNodeId + ".outputs['Value'], " +
                         combineId + ".inputs['" + components[i] + "'])");
                } else {
                    double constRad = rotInfo.otherRot[i] * M_PI / 180.0;
                    if (std::abs(constRad) > 0.0001) {
                        emit(combineId + ".inputs['" + std::string(components[i]) +
                             "'].default_value = " + pyDouble(constRad));
                    }
                }
            }

            // Rotate instances
            std::string rotateInstId = newNodeId();
            emit(rotateInstId + " = nodes.new('GeometryNodeRotateInstances')");
            emit(rotateInstId + ".location = (x_pos, y_pos)");
            emit("links.new(" + instanceId + ".outputs['Instances'], " +
                 rotateInstId + ".inputs['Instances'])");
            emit("links.new(" + combineId + ".outputs['Vector'], " +
                 rotateInstId + ".inputs['Rotation'])");
            emit("x_pos += 200");

            // Realize instances to produce actual mesh geometry
            std::string realizeId = newNodeId();
            emit(realizeId + " = nodes.new('GeometryNodeRealizeInstances')");
            emit(realizeId + ".location = (x_pos, y_pos)");
            emit("links.new(" + rotateInstId + ".outputs['Instances'], " +
                 realizeId + ".inputs['Geometry'])");
            emit("last_geo = " + realizeId);
            emit("x_pos += 200");
            return;
        }

        // ─── Check for Data-driven Instance Array pattern ───
        // Detect: for (var = [start:step:end]) translate([f(var),...]) [const_transforms] primitive(with optional var-dep param)
        // Emit: template geometry + data mesh (bmesh) + InstanceOnPoints + ScaleInstances
        InstanceArrayInfo instInfo = detectInstanceArrayPattern(node);
        if (instInfo.valid) {
            std::string loopVar = node.variable();
            emit("# Data-driven instance array: " + loopVar);

            // Temporarily add loop var to loop_variables_ so exprTreeToPython treats it as runtime
            loop_variables_.insert(loopVar);

            // ─── 1. Emit template geometry ───
            // Emit the primitive with default varying parameter (1.0 for height/depth)
            Arguments templateArgs = instInfo.primitiveNode->args();
            if (!instInfo.varyingPrimArg.empty()) {
                templateArgs[instInfo.varyingPrimArg] = Value(1.0);
            }

            // Emit template primitive
            switch (instInfo.primitiveNode->type()) {
                case ASTNode::Type::Cube: emitCube(templateArgs); break;
                case ASTNode::Type::Sphere: emitSphere(templateArgs); break;
                case ASTNode::Type::Cylinder: emitCylinder(templateArgs); break;
                case ASTNode::Type::Circle: emitCircle(templateArgs); break;
                case ASTNode::Type::Square: emitSquare(templateArgs); break;
                case ASTNode::Type::Polygon: emitPolygon(templateArgs); break;
                case ASTNode::Type::Text: emitText(templateArgs); break;
                case ASTNode::Type::Polyhedron: emitPolyhedron(templateArgs); break;
                default: break;
            }
            std::string templateGeo = "last_geo";

            // Apply constant transforms (in order from inner to outer — reverse of chain)
            for (auto it = instInfo.constantChain.rbegin(); it != instInfo.constantChain.rend(); ++it) {
                auto* tNode = dynamic_cast<TransformNode*>(*it);
                if (!tNode) continue;
                switch ((*it)->type()) {
                    case ASTNode::Type::Rotate: emitRotate(tNode->args()); break;
                    case ASTNode::Type::Scale: emitScale(tNode->args()); break;
                    case ASTNode::Type::Mirror: emitMirror(tNode->args()); break;
                    default: break;
                }
            }
            emit("_template_geo = last_geo");

            // ─── 2. Emit Python data mesh creation ───
            // Get position expressions from Translate arguments
            const auto& transArgs = instInfo.translateNode->args();
            Value transVal;
            {
                auto it2 = transArgs.find("v");
                if (it2 == transArgs.end()) it2 = transArgs.find("_0");
                if (it2 != transArgs.end()) transVal = it2->second;
            }

            // Build Python expressions for position components
            std::string posExprs[3] = {"0", "0", "0"};
            if (transVal.isVector() && transVal.size() >= 3) {
                for (int i = 0; i < 3 && i < static_cast<int>(transVal.size()); ++i) {
                    ExprNodePtr tree = getOrMakeLiteralTree(transVal[i]);
                    posExprs[i] = exprTreeToPython(tree);
                }
            }

            // Build Python expression for the varying primitive parameter (if any)
            std::string varyingExpr;
            if (!instInfo.varyingPrimArg.empty()) {
                const auto& primArgs = instInfo.primitiveNode->args();
                auto it2 = primArgs.find(instInfo.varyingPrimArg);
                if (it2 != primArgs.end()) {
                    ExprNodePtr tree = getOrMakeLiteralTree(it2->second);
                    varyingExpr = exprTreeToPython(tree);
                }
            }

            // Emit range computation
            ExprNodePtr startTree = range.rangeStartExpr();
            ExprNodePtr endTree = range.rangeEndExpr();
            ExprNodePtr stepTree = range.rangeStepExpr();
            if (startTree) startTree = resolveExprTree(Value::expressionWithTree("", startTree));
            if (endTree) endTree = resolveExprTree(Value::expressionWithTree("", endTree));
            if (stepTree) stepTree = resolveExprTree(Value::expressionWithTree("", stepTree));

            std::string startPy = startTree ? exprTreeToPython(startTree) : pyDouble(range.rangeStart());
            std::string endPy = endTree ? exprTreeToPython(endTree) : pyDouble(range.rangeEnd());
            std::string stepPy = stepTree ? exprTreeToPython(stepTree) : pyDouble(range.rangeStep());

            emit("import bmesh as _bmesh");
            emit("_inst_n = max(0, int((" + endPy + " - " + startPy + ") / " + stepPy + ") + 1)");
            emit("_inst_bm = _bmesh.new()");
            emit("_inst_heights = []");
            emit("for _inst_i in range(_inst_n):");
            indent_++;
            emit(loopVar + " = " + startPy + " + _inst_i * " + stepPy);
            emit("_inst_bm.verts.new((" + posExprs[0] + ", " + posExprs[1] + ", " + posExprs[2] + "))");
            if (!varyingExpr.empty()) {
                emit("_inst_heights.append(" + varyingExpr + ")");
            }
            indent_--;

            emit("_inst_mesh = bpy.data.meshes.new('inst_data_' + str(id(nodes)))");
            emit("_inst_bm.to_mesh(_inst_mesh)");
            emit("_inst_bm.free()");

            // Store varying parameter as a named attribute
            if (!varyingExpr.empty()) {
                emit("if _inst_n > 0:");
                indent_++;
                emit("_inst_attr = _inst_mesh.attributes.new('inst_scale', 'FLOAT', 'POINT')");
                emit("for _inst_i in range(_inst_n):");
                indent_++;
                emit("_inst_attr.data[_inst_i].value = _inst_heights[_inst_i]");
                indent_--;
                indent_--;
            }

            // Create data object
            emit("_inst_obj = bpy.data.objects.new('inst_data_' + str(id(nodes)), _inst_mesh)");
            emit("bpy.context.collection.objects.link(_inst_obj)");
            emit("_inst_obj.hide_viewport = True");
            emit("_inst_obj.hide_render = True");

            // ─── 3. Emit instancing node tree ───
            emit("if _inst_n > 0:");
            indent_++;

            // Object Info node to reference data mesh
            std::string objInfoId = newNodeId();
            emit(objInfoId + " = nodes.new('GeometryNodeObjectInfo')");
            emit(objInfoId + ".location = (x_pos, y_pos - 200)");
            emit(objInfoId + ".transform_space = 'RELATIVE'");
            emit(objInfoId + ".inputs['Object'].default_value = _inst_obj");
            emit("x_pos += 200");

            // Instance on Points
            std::string iopId = newNodeId();
            emit(iopId + " = nodes.new('GeometryNodeInstanceOnPoints')");
            emit(iopId + ".location = (x_pos, y_pos)");
            emit("links.new(" + objInfoId + ".outputs['Geometry'], " + iopId + ".inputs['Points'])");
            emit("link_nodes(links, _template_geo, 'Geometry', " + iopId + ", 'Instance')");
            emit("x_pos += 200");

            std::string currentInstOutput = iopId;

            // If there's a varying parameter, scale instances by it
            if (!varyingExpr.empty()) {
                // NamedAttribute to read the stored scale data
                std::string namedAttrId = newNodeId();
                emit(namedAttrId + " = nodes.new('GeometryNodeInputNamedAttribute')");
                emit(namedAttrId + ".location = (x_pos, y_pos - 300)");
                emit(namedAttrId + ".data_type = 'FLOAT'");
                emit(namedAttrId + ".inputs['Name'].default_value = 'inst_scale'");

                // Determine which axis to scale — for cylinder height it's Z
                // (MeshCone creates geometry along Z axis from 0 to Depth)
                std::string combineId = newNodeId();
                emit(combineId + " = nodes.new('ShaderNodeCombineXYZ')");
                emit(combineId + ".location = (x_pos, y_pos - 200)");
                emit(combineId + ".inputs['X'].default_value = 1.0");
                emit(combineId + ".inputs['Y'].default_value = 1.0");
                emit("links.new(" + namedAttrId + ".outputs['Attribute'], " + combineId + ".inputs['Z'])");

                std::string scaleInstId = newNodeId();
                emit(scaleInstId + " = nodes.new('GeometryNodeScaleInstances')");
                emit(scaleInstId + ".location = (x_pos, y_pos)");
                emit("links.new(" + currentInstOutput + ".outputs['Instances'], " +
                     scaleInstId + ".inputs['Instances'])");
                emit("links.new(" + combineId + ".outputs['Vector'], " +
                     scaleInstId + ".inputs['Scale'])");
                emit("x_pos += 200");
                currentInstOutput = scaleInstId;
            }

            // Realize instances
            std::string realizeId = newNodeId();
            emit(realizeId + " = nodes.new('GeometryNodeRealizeInstances')");
            emit(realizeId + ".location = (x_pos, y_pos)");
            emit("links.new(" + currentInstOutput + ".outputs['Instances'], " +
                 realizeId + ".inputs['Geometry'])");
            emit("last_geo = " + realizeId);
            emit("x_pos += 200");

            indent_--;  // end of if _inst_n > 0
            emit("else:");
            indent_++;
            emit("last_geo = None");
            indent_--;

            loop_variables_.erase(loopVar);
            return;
        }

        // ─── Standard Python loop fallback ───
        emit("# For loop: " + node.variable());
        std::string loopVar = node.variable();

        // Track loop variable so expressions inside the body use it as a Python variable
        loop_variables_.insert(loopVar);

        // Create a JoinGeometry node to accumulate geometry from each iteration
        std::string joinId = newNodeId();
        emit(joinId + " = nodes.new('GeometryNodeJoinGeometry')");
        emit(joinId + ".location = (x_pos, y_pos)");

        // Emit the range using Python expressions for runtime variables
        ExprNodePtr startTree = range.rangeStartExpr();
        ExprNodePtr endTree = range.rangeEndExpr();
        if (startTree) startTree = resolveExprTree(Value::expressionWithTree("", startTree));
        if (endTree) endTree = resolveExprTree(Value::expressionWithTree("", endTree));
        bool runtimeRange = (startTree && exprTreeReferencesModuleParams(startTree)) ||
                            (endTree && exprTreeReferencesModuleParams(endTree));
        if (runtimeRange) {
            std::string startPy = startTree ? exprTreeToPython(startTree) : std::to_string(static_cast<int>(range.rangeStart()));
            std::string endPy = endTree ? exprTreeToPython(endTree) : std::to_string(static_cast<int>(range.rangeEnd()));
            emit("for " + loopVar + " in range(int(" + startPy + "), int(" + endPy + ") + 1):");
        } else {
            emit("for " + loopVar + " in " + range.toPython() + ":");
        }
        indent_++;

        for (auto& child : node.children()) {
            if (!child) continue;
            child->accept(*this);
        }

        // Link each iteration's result into the join node
        emit("if last_geo is not None:");
        indent_++;
        emit("link_nodes(links, last_geo, 'Geometry', " + joinId + ", 'Geometry')");
        indent_--;

        indent_--;

        emit("last_geo = " + joinId);

        loop_variables_.erase(loopVar);
    } else if (range.isVector()) {
        // ─── Check for Scale mirror array pattern ───
        // Detect: for (var = [v0, v1, ...]) scale([..., var, ...]) { body }
        // where body doesn't reference var. Emit as Instance on Points + ScaleInstances.
        ScaleArrayInfo scaleInfo = detectScaleArrayPattern(node);
        if (scaleInfo.valid) {
            int N = static_cast<int>(scaleInfo.values.size());
            emit("# Scale mirror array (Instance on Points): " + node.variable() +
                 " — " + std::to_string(N) + " instances");

            // Emit the body geometry with the loop variable set to 1.0 (neutral for scale)
            // This way scale([1, var, 1]) becomes scale([1, 1, 1]) = identity
            std::string loopVar = node.variable();
            variables_[loopVar] = Value(1.0);
            for (auto& bodyChild : node.children()) {
                if (bodyChild) bodyChild->accept(*this);
            }
            variables_.erase(loopVar);

            // Create N points at origin
            std::string lineId = newNodeId();
            emit(lineId + " = nodes.new('GeometryNodeMeshLine')");
            emit(lineId + ".location = (x_pos, y_pos - 200)");
            emit(lineId + ".mode = 'OFFSET'");
            emit(lineId + ".inputs['Count'].default_value = " + std::to_string(N));
            emit(lineId + ".inputs['Offset'].default_value = (0, 0, 0)");
            emit("x_pos += 200");

            // Instance on Points
            std::string instanceId = newNodeId();
            emit(instanceId + " = nodes.new('GeometryNodeInstanceOnPoints')");
            emit(instanceId + ".location = (x_pos, y_pos)");
            emit("links.new(" + lineId + ".outputs['Mesh'], " + instanceId + ".inputs['Points'])");
            emit("link_nodes(links, last_geo, 'Geometry', " + instanceId + ", 'Instance')");
            emit("x_pos += 200");

            // Compute per-instance scale using Index
            // Check if values form a linear sequence: v0 + Index * step
            bool isLinear = true;
            double v0 = scaleInfo.values[0];
            double step = (N >= 2) ? (scaleInfo.values[1] - scaleInfo.values[0]) : 0;
            for (size_t i = 2; i < scaleInfo.values.size(); ++i) {
                if (std::abs(scaleInfo.values[i] - scaleInfo.values[i-1] - step) > 0.0001) {
                    isLinear = false;
                    break;
                }
            }

            std::string scaleValueId;
            if (isLinear) {
                // Linear: value = v0 + Index * step
                std::string indexId = newNodeId();
                emit(indexId + " = nodes.new('GeometryNodeInputIndex')");
                emit(indexId + ".location = (x_pos, y_pos - 350)");

                if (std::abs(step) > 0.0001) {
                    std::string mulId = newNodeId();
                    emit(mulId + " = nodes.new('ShaderNodeMath')");
                    emit(mulId + ".operation = 'MULTIPLY'");
                    emit(mulId + ".location = (x_pos + 150, y_pos - 350)");
                    emit("links.new(" + indexId + ".outputs['Index'], " + mulId + ".inputs[0])");
                    emit(mulId + ".inputs[1].default_value = " + pyDouble(step));

                    if (std::abs(v0) > 0.0001) {
                        std::string addId = newNodeId();
                        emit(addId + " = nodes.new('ShaderNodeMath')");
                        emit(addId + ".operation = 'ADD'");
                        emit(addId + ".location = (x_pos + 300, y_pos - 350)");
                        emit("links.new(" + mulId + ".outputs['Value'], " + addId + ".inputs[0])");
                        emit(addId + ".inputs[1].default_value = " + pyDouble(v0));
                        scaleValueId = addId;
                    } else {
                        scaleValueId = mulId;
                    }
                } else {
                    // All values are the same — just use a constant
                    scaleValueId = "";  // Will use default_value below
                }
            }

            // Build CombineXYZ for scale vector
            std::string combineId = newNodeId();
            emit(combineId + " = nodes.new('ShaderNodeCombineXYZ')");
            emit(combineId + ".location = (x_pos, y_pos - 200)");
            const char* components[] = {"X", "Y", "Z"};
            for (int i = 0; i < 3; ++i) {
                if (i == scaleInfo.scaleComponent) {
                    if (!scaleValueId.empty()) {
                        emit("links.new(" + scaleValueId + ".outputs['Value'], " +
                             combineId + ".inputs['" + components[i] + "'])");
                    } else {
                        emit(combineId + ".inputs['" + std::string(components[i]) +
                             "'].default_value = " + pyDouble(v0));
                    }
                } else {
                    emit(combineId + ".inputs['" + std::string(components[i]) +
                         "'].default_value = " + pyDouble(scaleInfo.otherScale[i]));
                }
            }

            // ScaleInstances
            std::string scaleInstId = newNodeId();
            emit(scaleInstId + " = nodes.new('GeometryNodeScaleInstances')");
            emit(scaleInstId + ".location = (x_pos, y_pos)");
            emit("links.new(" + instanceId + ".outputs['Instances'], " +
                 scaleInstId + ".inputs['Instances'])");
            emit("links.new(" + combineId + ".outputs['Vector'], " +
                 scaleInstId + ".inputs['Scale'])");
            emit("x_pos += 200");

            // Realize instances
            std::string realizeId = newNodeId();
            emit(realizeId + " = nodes.new('GeometryNodeRealizeInstances')");
            emit(realizeId + ".location = (x_pos, y_pos)");
            emit("links.new(" + scaleInstId + ".outputs['Instances'], " +
                 realizeId + ".inputs['Geometry'])");
            emit("last_geo = " + realizeId);
            emit("x_pos += 200");
            return;
        }

        // ─── Standard Python vector loop fallback ───
        emit("# For loop over vector: " + node.variable());
        std::string loopVar = node.variable();

        // Track loop variable
        loop_variables_.insert(loopVar);

        if (node.isIntersect()) {
            // intersection_for: intersect each iteration's geometry
            std::string firstGeo = newNodeId() + "_isect_first";
            emit(firstGeo + " = None");

            emit("for " + loopVar + " in " + range.toPython() + ":");
            indent_++;

            for (auto& child : node.children()) {
                if (!child) continue;
                child->accept(*this);
            }

            emit("if last_geo is not None:");
            indent_++;
            emit("if " + firstGeo + " is None:");
            indent_++;
            emit(firstGeo + " = last_geo");
            indent_--;
            emit("else:");
            indent_++;
            std::string boolId = newNodeId();
            emit(boolId + " = nodes.new('GeometryNodeMeshBoolean')");
            emit(boolId + ".location = (x_pos, y_pos)");
            emit(boolId + ".operation = 'INTERSECT'");
            emit(boolId + ".solver = 'EXACT'");
            emit("links.new(geo_out(" + firstGeo + "), " + boolId + ".inputs[1])");
            emit("links.new(geo_out(last_geo), " + boolId + ".inputs[1])");
            emit(firstGeo + " = " + boolId);
            emit("x_pos += 200");
            emit("y_pos -= 50");
            indent_--;
            indent_--;

            indent_--;

            emit("last_geo = " + firstGeo);
        } else {
            // Create a JoinGeometry node to accumulate geometry from each iteration
            std::string joinId = newNodeId();
            emit(joinId + " = nodes.new('GeometryNodeJoinGeometry')");
            emit(joinId + ".location = (x_pos, y_pos)");

            emit("for " + loopVar + " in " + range.toPython() + ":");
            indent_++;

            for (auto& child : node.children()) {
                if (!child) continue;
                child->accept(*this);
            }

            // Link each iteration's result into the join node
            emit("if last_geo is not None:");
            indent_++;
            emit("link_nodes(links, last_geo, 'Geometry', " + joinId + ", 'Geometry')");
            indent_--;

            indent_--;

            emit("last_geo = " + joinId);
        }

        loop_variables_.erase(loopVar);
    } else if (range.isExpression() && range.exprTree()) {
        // Expression-based range (e.g., for (j = i[2]) where i is an outer loop variable)
        // Emit as Python for loop using the expression's Python representation
        std::string loopVar = node.variable();
        loop_variables_.insert(loopVar);

        std::string joinId = newNodeId();
        emit(joinId + " = nodes.new('GeometryNodeJoinGeometry')");
        emit(joinId + ".location = (x_pos, y_pos)");

        std::string rangeExpr = exprTreeToPython(range.exprTree());
        emit("for " + loopVar + " in " + rangeExpr + ":");
        indent_++;

        for (auto& child : node.children()) {
            if (!child) continue;
            child->accept(*this);
        }

        emit("if last_geo is not None:");
        indent_++;
        emit("link_nodes(links, last_geo, 'Geometry', " + joinId + ", 'Geometry')");
        indent_--;

        indent_--;

        emit("last_geo = " + joinId);

        loop_variables_.erase(loopVar);
    } else {
        // Try to evaluate expression-based range as a fallback
        emit("# For loop range could not be resolved: " + node.variable());
    }
}

void BlenderGenerator::visit(IfElseNode& node) {
    emit("# Conditional");
    const Value& cond = node.condition();

    // Try to resolve condition to a concrete boolean
    bool resolved = false;
    bool condValue = false;

    if (cond.isBool()) {
        resolved = true;
        condValue = cond.toBool();
    } else if (cond.isNumber()) {
        resolved = true;
        condValue = (cond.toNumber() != 0.0);
    } else if (cond.isExpression()) {
        // Try to resolve variable reference from variables_ map
        const std::string& expr = cond.toString();
        auto it = variables_.find(expr);
        if (it != variables_.end()) {
            const Value& val = it->second;
            if (val.isBool()) {
                resolved = true;
                condValue = val.toBool();
            } else if (val.isNumber()) {
                resolved = true;
                condValue = (val.toNumber() != 0.0);
            }
        }
        // If simple lookup failed, try evaluating the expression tree
        if (!resolved && cond.exprTree()) {
            // Only resolve if the expression tree doesn't reference module params
            // or loop variables (which are only known at runtime)
            if (!exprTreeReferencesModuleParams(cond.exprTree())) {
                // Temporarily inject $preview=true for conditional evaluation
                bool had_preview = variables_.count("$preview") > 0;
                Value old_preview;
                if (had_preview) old_preview = variables_["$preview"];
                variables_["$preview"] = Value(true);
                try {
                    double val = evaluateExprTree(cond.exprTree());
                    resolved = true;
                    condValue = (val != 0.0);
                } catch (...) {
                    // Can't evaluate — leave as unresolved
                }
                // Restore $preview
                if (had_preview) variables_["$preview"] = old_preview;
                else variables_.erase("$preview");
            }
        }
    }

    if (resolved) {
        if (condValue) {
            emit("# if (true):");
            for (auto& child : node.children()) {
                if (!child) continue;
                child->accept(*this);
            }
        } else if (node.elseBranch()) {
            emit("# if (false) - else branch:");
            node.elseBranch()->accept(*this);
        } else {
            // Condition is false and no else branch — signal no geometry produced
            // so parent boolean operations can skip this child
            emit("last_geo = None");
        }
    } else {
        // Runtime condition - emit Python if statement
        ExprNodePtr condTree = cond.exprTree();
        if (condTree) {
            std::string pyCond = exprTreeToPython(condTree);
            emit("if " + pyCond + ":");
            indent_++;
            emit("pass  # placeholder for empty if branch");
            for (auto& child : node.children()) {
                if (!child) continue;
                child->accept(*this);
            }
            indent_--;
            if (node.elseBranch()) {
                emit("else:");
                indent_++;
                emit("pass  # placeholder for empty else branch");
                node.elseBranch()->accept(*this);
                indent_--;
            }
        } else {
            // No expression tree available - emit if branch unconditionally
            emit("# Runtime conditional - emitting if branch");
            for (auto& child : node.children()) {
                if (!child) continue;
                child->accept(*this);
            }
        }
    }
}

void BlenderGenerator::visit(AssignmentNode& node) {
    // Store variable in local scope
    variables_[node.name()] = node.value();
    // Emit Python variable assignment if inside module
    if (in_module_) {
        // Skip assignments that shadow module parameters — the parameter already
        // provides the value and emitting a constant would override the caller's input
        if (current_module_params_.count(pyName(node.name()))) {
            return;
        }
        const Value& val = node.value();
        if (val.isUndefined()) {
            // Undefined values become 0 to avoid None in arithmetic
            emit(pyName(node.name()) + " = 0");
        } else if (val.isExpression()) {
            // Check if expression tree references runtime vars (module params, loop vars)
            ExprNodePtr tree = resolveExprTree(val);
            if (tree && exprTreeReferencesModuleParams(tree)) {
                std::string pyExpr = exprTreeToPython(tree);
                emit(pyName(node.name()) + " = " + pyExpr);
                // Track this variable as a runtime variable since it depends on runtime vars
                loop_variables_.insert(node.name());
            } else {
                // Resolve expression to numeric value
                double numVal = evaluateExpr(val);
                emit(pyName(node.name()) + " = " + pyDouble(numVal));
            }
        } else if (val.isVector()) {
            // Use the overall vector exprTree (VectorLiteral) if available,
            // as individual element Values may have been resolved away
            ExprNodePtr vecTree = val.exprTree();
            bool hasModuleParamRef = false;
            if (vecTree && exprTreeReferencesModuleParams(vecTree)) {
                hasModuleParamRef = true;
            }
            if (!hasModuleParamRef) {
                // Also check individual element trees
                for (size_t i = 0; i < val.size(); ++i) {
                    ExprNodePtr et = val[i].exprTree();
                    if (et && exprTreeReferencesModuleParams(et)) {
                        hasModuleParamRef = true;
                        break;
                    }
                }
            }
            if (hasModuleParamRef) {
                // Emit as Python tuple with runtime expressions
                if (vecTree && vecTree->kind == ExprNode::Kind::BinaryOp &&
                    (vecTree->op == ExprNode::Op::ADD || vecTree->op == ExprNode::Op::SUBTRACT)) {
                    // Vector arithmetic: decompose into per-element operations
                    // E.g. top + [w/2, 0, 0] becomes (top[0] + w/2, top[1] + 0, top[2] + 0)
                    std::string opStr = (vecTree->op == ExprNode::Op::ADD) ? " + " : " - ";
                    auto& left = vecTree->left;
                    auto& right = vecTree->right;

                    // Determine which side is VectorLiteral (or both might be VarRef)
                    auto getElemExpr = [&](const ExprNodePtr& tree, size_t idx) -> std::string {
                        if (tree->kind == ExprNode::Kind::VectorLiteral && idx < tree->vec_elements.size()) {
                            return exprTreeToPython(tree->vec_elements[idx]);
                        } else if (tree->kind == ExprNode::Kind::VarRef) {
                            return exprTreeToPython(tree) + "[" + std::to_string(idx) + "]";
                        } else {
                            // Generic case
                            return exprTreeToPython(tree) + "[" + std::to_string(idx) + "]";
                        }
                    };

                    std::string vecStr = "(";
                    for (size_t i = 0; i < val.size(); ++i) {
                        if (i > 0) vecStr += ", ";
                        vecStr += "(" + getElemExpr(left, i) + opStr + getElemExpr(right, i) + ")";
                    }
                    if (val.size() == 1) vecStr += ",";
                    vecStr += ")";
                    emit(pyName(node.name()) + " = " + vecStr);
                } else if (vecTree && vecTree->kind == ExprNode::Kind::FunctionCall) {
                    // Function call returning a vector — emit as Python call directly
                    emit(pyName(node.name()) + " = " + exprTreeToPython(vecTree));
                } else if (vecTree && vecTree->kind != ExprNode::Kind::VectorLiteral && val.size() > 0) {
                    // Other complex expression — try element-wise extraction
                    std::string vecStr = "(";
                    for (size_t i = 0; i < val.size(); ++i) {
                        if (i > 0) vecStr += ", ";
                        vecStr += exprTreeToPython(vecTree) + "[" + std::to_string(i) + "]";
                    }
                    if (val.size() == 1) vecStr += ",";
                    vecStr += ")";
                    emit(pyName(node.name()) + " = " + vecStr);
                } else {
                    // VectorLiteral or per-element trees available
                    std::vector<ExprNodePtr> elemTrees;
                    if (vecTree && vecTree->kind == ExprNode::Kind::VectorLiteral) {
                        elemTrees = vecTree->vec_elements;
                    }
                    std::string vecStr = "(";
                    for (size_t i = 0; i < val.size(); ++i) {
                        if (i > 0) vecStr += ", ";
                        // Use element tree from VectorLiteral if available
                        ExprNodePtr et = (i < elemTrees.size()) ? elemTrees[i] : val[i].exprTree();
                        if (et && exprTreeReferencesModuleParams(et)) {
                            vecStr += exprTreeToPython(et);
                        } else if (et) {
                            // Non-module-param expression tree — evaluate to constant
                            double v = evaluateExprTree(et);
                            vecStr += pyDouble(v);
                        } else if (val[i].isUndefined()) {
                            vecStr += "0";
                        } else if (val[i].isNumber()) {
                            vecStr += pyDouble(val[i].toNumber());
                        } else {
                            vecStr += val[i].toPython();
                        }
                    }
                    if (val.size() == 1) vecStr += ",";
                    vecStr += ")";
                    emit(pyName(node.name()) + " = " + vecStr);
                }
                // Track as runtime variable
                loop_variables_.insert(node.name());
            } else {
                // Evaluate vector components numerically
                std::string vecStr = "(";
                for (size_t i = 0; i < val.size(); ++i) {
                    if (i > 0) vecStr += ", ";
                    if (val[i].isExpression()) {
                        vecStr += pyDouble(evaluateExpr(val[i]));
                    } else if (val[i].isUndefined()) {
                        vecStr += "0";
                    } else {
                        vecStr += val[i].toPython();
                    }
                }
                if (val.size() == 1) vecStr += ",";
                vecStr += ")";
                emit(pyName(node.name()) + " = " + vecStr);
            }
        } else {
            emit(pyName(node.name()) + " = " + val.toPython());
        }
    }
}

void BlenderGenerator::visit(ChildrenNode& node) {
    if (in_module_) {
        emit("# children() - restore passed geometry");
        emit("last_geo = children_geo");
    }
}

void BlenderGenerator::visit(FunctionNode& node) {
    // No-op: function bodies are evaluated at parse time
}

// Primitive generators
void BlenderGenerator::emitCube(const Arguments& args) {
    std::string nodeId = newNodeId();

    Value size = getArg(args, "size", getPositionalArg(args, 0, Value(1.0)));
    Value center = getArg(args, "center", getPositionalArg(args, 1, Value(false)));

    // Resolve variable references that aren't group_input sockets to their values
    if (size.isExpression() && !isSimpleVariableRef(size)) {
        auto it = variables_.find(size.toString());
        if (it != variables_.end() && !it->second.isExpression()) {
            size = it->second;
        }
    }

    emit("# Cube");
    emit(nodeId + " = nodes.new('GeometryNodeMeshCube')");
    emit(nodeId + ".location = (x_pos, y_pos)");

    if (isSimpleVariableRef(size)) {
        // Link size from group_input - need to handle scalar vs vector
        std::string varName = size.toString();
        std::string socketName = varName;
        if (varName[0] == '$') {
            socketName = varName.substr(1);
        }
        emit("# Size from variable: " + varName);
        emit("links.new(group_input.outputs['" + socketName + "'], " + nodeId + ".inputs['Size'])");
    } else if (size.isVector() && size.size() >= 3 && vectorHasExprTrees(size)) {
        emitVectorWithExprTrees(nodeId, "Size", size);
    } else if (size.isVector() && size.size() >= 3) {
        emit(nodeId + ".inputs['Size'].default_value = " + vectorToPython(size));
    } else {
        // Scalar size — check for expression tree
        ExprNodePtr sizeTree = getOrMakeLiteralTree(size);
        if (sizeTree->hasVariableRefs()) {
            emitScalarToAllVectorComponents(nodeId, "Size", sizeTree);
        } else {
            double s = size.toNumber();
            emit(nodeId + ".inputs['Size'].default_value = (" +
                 pyDouble(s) + ", " + pyDouble(s) + ", " + pyDouble(s) + ")");
        }
    }

    emit("last_geo = " + nodeId);
    emit("x_pos += 200");
    emit("y_pos -= 50");

    // Handle center=false (translate by half size)
    if (!center.toBool() && !isSimpleVariableRef(center)) {
        emitBlank();
        emit("# Translate for center=false");
        std::string transId = newNodeId();
        emit(transId + " = nodes.new('GeometryNodeTransform')");
        emit(transId + ".location = (x_pos, y_pos)");

        if (size.isVector() && size.size() >= 3) {
            // Vector size — check each component for expression trees
            bool hasExprs = false;
            ExprNodePtr compTrees[3];
            for (int i = 0; i < 3; ++i) {
                compTrees[i] = getOrMakeLiteralTree(size[i]);
                if (compTrees[i]->hasVariableRefs()) hasExprs = true;
            }

            if (hasExprs || vectorHasExprTrees(size)) {
                // Build size[i] / 2 expression trees
                std::string combineId = newNodeId();
                emit("# CombineXYZ for center=false Translation");
                emit(combineId + " = nodes.new('ShaderNodeCombineXYZ')");
                emit(combineId + ".location = (x_pos, y_pos)");
                emit("y_pos -= 50");

                const char* components[] = {"X", "Y", "Z"};
                for (int i = 0; i < 3; ++i) {
                    ExprNodePtr halfTree = ExprNode::makeBinary(
                        ExprNode::Op::DIVIDE, compTrees[i], ExprNode::makeLiteral(2.0));
                    if (exprTreeReferencesModuleParams(halfTree)) {
                        // Module params are Python runtime variables — emit as Python expression
                        emit(combineId + ".inputs['" + std::string(components[i]) +
                             "'].default_value = " + exprTreeToPython(halfTree));
                    } else if (exprTreeHasOnlyGroupInputVars(halfTree)) {
                        auto result = emitExpressionNodeTree(halfTree);
                        connectExprResultNamed(result, combineId, components[i]);
                    } else {
                        // Literal — evaluate directly
                        double val = evaluateExprTree(halfTree);
                        emit(combineId + ".inputs['" + std::string(components[i]) +
                             "'].default_value = " + pyDouble(val));
                    }
                }

                emit("links.new(" + combineId + ".outputs['Vector'], " + transId + ".inputs['Translation'])");
            } else {
                emit(transId + ".inputs['Translation'].default_value = (" +
                     pyDouble(size[0].toNumber() / 2) + ", " +
                     pyDouble(size[1].toNumber() / 2) + ", " +
                     pyDouble(size[2].toNumber() / 2) + ")");
            }
        } else {
            // Scalar size — check for expression tree
            ExprNodePtr sizeTree = getOrMakeLiteralTree(size);
            if (sizeTree->hasVariableRefs()) {
                ExprNodePtr halfTree = ExprNode::makeBinary(
                    ExprNode::Op::DIVIDE, sizeTree, ExprNode::makeLiteral(2.0));
                emitScalarToAllVectorComponents(transId, "Translation", halfTree);
            } else {
                double s = size.toNumber() / 2;
                emit(transId + ".inputs['Translation'].default_value = (" +
                     pyDouble(s) + ", " + pyDouble(s) + ", " + pyDouble(s) + ")");
            }
        }

        emit("link_nodes(links, last_geo, 'Mesh', " + transId + ", 'Geometry')");
        emit("last_geo = " + transId);
        emit("x_pos += 200");
    }
}

void BlenderGenerator::emitSphere(const Arguments& args) {
    std::string nodeId = newNodeId();

    Value r = getArg(args, "r", getPositionalArg(args, 0, Value(1.0)));
    Value d = getArg(args, "d", Value());
    Value fn = resolveFn(args);

    // Determine which value to use for radius
    Value radiusValue = r;
    std::string radiusPython;
    if (!d.isUndefined()) {
        radiusValue = makeDivisionValue(d, 2.0);
        double dVal = d.isExpression() ? evaluateExpr(d) : d.toNumber();
        radiusPython = pyDouble(dVal / 2.0);
    } else {
        double rVal = r.isExpression() ? evaluateExpr(r) : r.toNumber();
        radiusValue = r;
        radiusPython = pyDouble(rVal);
    }

    emit("# Sphere");
    emit(nodeId + " = nodes.new('GeometryNodeMeshUVSphere')");
    emit(nodeId + ".location = (x_pos, y_pos)");
    emitSetInputOrLink(nodeId, "Radius", radiusValue, radiusPython);

    // Segments and Rings from $fn
    int fnVal = static_cast<int>(evaluateExpr(fn));
    emitSetInputOrLink(nodeId, "Segments", fn, std::to_string(fnVal));

    // Rings = fn / 2 — build expression tree
    Value ringsValue = makeDivisionValue(fn, 2.0);
    emitSetInputOrLink(nodeId, "Rings", ringsValue, std::to_string(fnVal / 2));

    emit("last_geo = " + nodeId);
    emit("x_pos += 200");
    emit("y_pos -= 50");
}

void BlenderGenerator::emitCylinder(const Arguments& args) {
    std::string nodeId = newNodeId();

    Value h = getArg(args, "h", getPositionalArg(args, 0, Value(1.0)));
    Value r = getArg(args, "r", Value());
    Value r1 = getArg(args, "r1", Value());
    Value r2 = getArg(args, "r2", Value());
    Value d = getArg(args, "d", Value());
    Value d1 = getArg(args, "d1", Value());
    Value d2 = getArg(args, "d2", Value());
    Value center = getArg(args, "center", Value(false));
    Value fn = resolveFn(args);

    // Use Cone node to support different top/bottom radii
    emit("# Cylinder (using Cone for radius support)");
    emit(nodeId + " = nodes.new('GeometryNodeMeshCone')");
    emit(nodeId + ".location = (x_pos, y_pos)");

    // Handle height
    double hVal = h.isExpression() ? evaluateExpr(h) : h.toNumber();
    emitSetInputOrLink(nodeId, "Depth", h, std::to_string(hVal));

    // Determine radius values - handle expressions and computed values
    Value radiusTop, radiusBottom;
    std::string radiusTopPython, radiusBottomPython;

    // Default values
    radiusTop = Value(1.0);
    radiusBottom = Value(1.0);
    radiusTopPython = "1.0";
    radiusBottomPython = "1.0";

    // Handle various radius specifications
    if (!r.isUndefined()) {
        radiusTop = radiusBottom = r;
        double rVal = r.isExpression() ? evaluateExpr(r) : r.toNumber();
        radiusTopPython = radiusBottomPython = std::to_string(rVal);
    }
    if (!d.isUndefined()) {
        radiusTop = radiusBottom = makeDivisionValue(d, 2.0);
        double dVal = d.isExpression() ? evaluateExpr(d) : d.toNumber();
        double radius = dVal / 2.0;
        radiusTopPython = radiusBottomPython = pyDouble(radius);
    }
    if (!r1.isUndefined()) {
        radiusBottom = r1;
        double r1Val = r1.isExpression() ? evaluateExpr(r1) : r1.toNumber();
        radiusBottomPython = std::to_string(r1Val);
    }
    if (!r2.isUndefined()) {
        radiusTop = r2;
        double r2Val = r2.isExpression() ? evaluateExpr(r2) : r2.toNumber();
        radiusTopPython = std::to_string(r2Val);
    }
    if (!d1.isUndefined()) {
        radiusBottom = makeDivisionValue(d1, 2.0);
        double d1Val = d1.isExpression() ? evaluateExpr(d1) : d1.toNumber();
        radiusBottomPython = std::to_string(d1Val / 2.0);
    }
    if (!d2.isUndefined()) {
        radiusTop = makeDivisionValue(d2, 2.0);
        double d2Val = d2.isExpression() ? evaluateExpr(d2) : d2.toNumber();
        radiusTopPython = std::to_string(d2Val / 2.0);
    }

    // Emit radius settings
    emitSetInputOrLink(nodeId, "Radius Top", radiusTop, radiusTopPython);
    emitSetInputOrLink(nodeId, "Radius Bottom", radiusBottom, radiusBottomPython);

    // Handle $fn
    emitSetInputOrLink(nodeId, "Vertices", fn, std::to_string(static_cast<int>(evaluateExpr(fn))));

    emit("last_geo = " + nodeId);
    emit("x_pos += 200");
    emit("y_pos -= 50");

    // Handle center parameter
    // Blender's MeshCone already places bottom at Z=0 and top at Z=Depth,
    // which matches OpenSCAD's center=false behavior.
    // For center=true, we need to translate by -h/2 to center it.
    if (center.toBool() || isSimpleVariableRef(center)) {
        emitBlank();
        emit("# Translate for center=true");
        std::string transId = newNodeId();
        emit(transId + " = nodes.new('GeometryNodeTransform')");
        emit(transId + ".location = (x_pos, y_pos)");

        ExprNodePtr hTree = getOrMakeLiteralTree(h);
        if (hTree && hTree->hasVariableRefs() && exprTreeHasOnlyGroupInputVars(hTree)) {
            // Build -h / 2 expression tree and emit as Z component of CombineXYZ
            ExprNodePtr negHalfH = ExprNode::makeBinary(
                ExprNode::Op::DIVIDE,
                ExprNode::makeUnary(ExprNode::Op::NEGATE, hTree),
                ExprNode::makeLiteral(2.0));
            emitScalarToVectorInput(transId, "Translation", negHalfH, 2, 0.0, 0.0, 0.0);
        } else if (hTree && hTree->hasVariableRefs() && exprTreeReferencesModuleParams(hTree)) {
            // Runtime Python variable — might be a Blender node ref or scalar.
            // For simple VarRef to a module param, emit conditional code.
            if (hTree->kind == ExprNode::Kind::VarRef) {
                std::string varName = pyName(hTree->var_name);
                emit("if hasattr(" + varName + ", 'outputs'):");
                indent_++;
                // Build -h/2 as Math node chain + CombineXYZ
                std::string mulId = newNodeId();
                emit(mulId + " = nodes.new('ShaderNodeMath')");
                emit(mulId + ".operation = 'MULTIPLY'");
                emit(mulId + ".location = (x_pos, y_pos)");
                emit("links.new(" + varName + ".outputs['Value'], " + mulId + ".inputs[0])");
                emit(mulId + ".inputs[1].default_value = -0.5");
                std::string xyzId = newNodeId();
                emit(xyzId + " = nodes.new('ShaderNodeCombineXYZ')");
                emit(xyzId + ".location = (x_pos, y_pos)");
                emit("links.new(" + mulId + ".outputs['Value'], " + xyzId + ".inputs['Z'])");
                emit("links.new(" + xyzId + ".outputs['Vector'], " + transId + ".inputs['Translation'])");
                emit("x_pos += 200");
                indent_--;
                emit("else:");
                indent_++;
                emit(transId + ".inputs['Translation'].default_value = (0, 0, -(" + varName + ") / 2)");
                indent_--;
            } else {
                std::string pyExpr = exprTreeToPython(hTree);
                emit(transId + ".inputs['Translation'].default_value = (0, 0, -(" + pyExpr + ") / 2)");
            }
        } else {
            double hCenterVal = h.isExpression() ? evaluateExpr(h) : h.toNumber();
            emit(transId + ".inputs['Translation'].default_value = (0, 0, " +
                 std::to_string(-hCenterVal / 2) + ")");
        }

        emit("link_nodes(links, last_geo, 'Mesh', " + transId + ", 'Geometry')");
        emit("last_geo = " + transId);
        emit("x_pos += 200");
    }
}

void BlenderGenerator::emitCircle(const Arguments& args) {
    std::string nodeId = newNodeId();

    Value r = getArg(args, "r", getPositionalArg(args, 0, Value(1.0)));
    Value d = getArg(args, "d", Value());
    Value fn = resolveFn(args);

    // Determine radius value
    Value radiusValue = r;
    std::string radiusPython;
    if (!d.isUndefined()) {
        if (isSimpleVariableRef(d)) {
            radiusValue = d;
            radiusPython = d.toPython() + " / 2.0";
        } else {
            double radius = d.toNumber() / 2.0;
            radiusValue = Value(radius);
            radiusPython = pyDouble(radius);
        }
    } else {
        radiusPython = isSimpleVariableRef(r) ? r.toPython() : pyDouble(r.toNumber());
    }

    emit("# Circle (2D - using curve)");
    emit(nodeId + " = nodes.new('GeometryNodeCurvePrimitiveCircle')");
    emit(nodeId + ".location = (x_pos, y_pos)");
    emitSetInputOrLink(nodeId, "Radius", radiusValue, radiusPython);

    // Handle $fn
    if (isSimpleVariableRef(fn)) {
        std::string varName = fn.toString();
        std::string socketName = varName;
        if (varName[0] == '$') socketName = varName.substr(1);
        emit("links.new(group_input.outputs['" + socketName + "'], " + nodeId + ".inputs['Resolution'])");
    } else {
        emit(nodeId + ".inputs['Resolution'].default_value = " +
             std::to_string(static_cast<int>(evaluateExpr(fn))));
    }

    emit("last_geo = " + nodeId);
    emit("x_pos += 200");
    emit("y_pos -= 50");
}

void BlenderGenerator::emitSquare(const Arguments& args) {
    std::string nodeId = newNodeId();

    Value size = getArg(args, "size", getPositionalArg(args, 0, Value(1.0)));
    Value center = getArg(args, "center", getPositionalArg(args, 1, Value(false)));

    emit("# Square (2D - using curve quadrilateral)");
    emit(nodeId + " = nodes.new('GeometryNodeCurvePrimitiveQuadrilateral')");
    emit(nodeId + ".location = (x_pos, y_pos)");
    emit(nodeId + ".mode = 'RECTANGLE'");

    if (isSimpleVariableRef(size)) {
        // Link size from group_input - for scalar, link to both Width and Height
        std::string varName = size.toString();
        std::string socketName = varName;
        if (varName[0] == '$') {
            socketName = varName.substr(1);
        }
        emit("# Size from variable: " + varName);
        emit("links.new(group_input.outputs['" + socketName + "'], " + nodeId + ".inputs['Width'])");
        emit("links.new(group_input.outputs['" + socketName + "'], " + nodeId + ".inputs['Height'])");
    } else if (size.isVector() && size.size() >= 2) {
        double width = size[0].toNumber();
        double height = size[1].toNumber();
        emit(nodeId + ".inputs['Width'].default_value = " + std::to_string(width));
        emit(nodeId + ".inputs['Height'].default_value = " + pyDouble(height));
    } else if (size.isNumber()) {
        double s = size.toNumber();
        emit(nodeId + ".inputs['Width'].default_value = " + pyDouble(s));
        emit(nodeId + ".inputs['Height'].default_value = " + pyDouble(s));
    }

    emit("last_geo = " + nodeId);
    emit("x_pos += 200");
    emit("y_pos -= 50");
}

void BlenderGenerator::emitText(const Arguments& args) {
    std::string nodeId = newNodeId();

    Value text = getArg(args, "text", getPositionalArg(args, 0, Value("Text")));
    Value size = getArg(args, "size", Value(10.0));
    Value font = getArg(args, "font", Value(""));
    Value halign = getArg(args, "halign", Value("left"));
    Value valign = getArg(args, "valign", Value("baseline"));
    Value spacing = getArg(args, "spacing", Value(1.0));

    emit("# Text (2D - using String to Curves)");
    emit(nodeId + " = nodes.new('GeometryNodeStringToCurves')");
    emit(nodeId + ".location = (x_pos, y_pos)");

    // Set text string - handle expression (variable) or literal string
    if (isSimpleVariableRef(text)) {
        std::string varName = text.toString();
        std::string socketName = varName;
        if (varName[0] == '$') {
            socketName = varName.substr(1);
        }
        emit("links.new(group_input.outputs['" + socketName + "'], " + nodeId + ".inputs['String'])");
    } else if (text.isExpression() && in_module_) {
        // Module parameter or expression referencing module params — use Python variable
        ExprNodePtr tree = resolveExprTree(text);
        if (tree && exprTreeReferencesModuleParams(tree)) {
            emit(nodeId + ".inputs['String'].default_value = str(" + exprTreeToPython(tree) + ")");
        } else {
            std::string varName = text.toString();
            if (current_module_params_.find(pyName(varName)) != current_module_params_.end()) {
                emit(nodeId + ".inputs['String'].default_value = str(" + pyName(varName) + ")");
            } else {
                emit(nodeId + ".inputs['String'].default_value = str(" + text.toPython() + ")");
            }
        }
    } else if (text.isString()) {
        emit(nodeId + ".inputs['String'].default_value = " + text.toPython());
    } else {
        // Other types - convert to string
        emit(nodeId + ".inputs['String'].default_value = str(" + text.toPython() + ")");
    }

    // Set size - handle expression
    emitSetInputOrLink(nodeId, "Size", size, pyDouble(size.toNumber()));

    // Set character spacing - handle expression
    emitSetInputOrLink(nodeId, "Character Spacing", spacing, pyDouble(spacing.toNumber()));

    // Set alignment
    std::string halignStr = halign.isString() ? halign.toString() : "left";
    std::string valignStr = valign.isString() ? valign.toString() : "baseline";

    if (halignStr == "center") {
        emit(nodeId + ".align_x = 'CENTER'");
    } else if (halignStr == "right") {
        emit(nodeId + ".align_x = 'RIGHT'");
    } else {
        emit(nodeId + ".align_x = 'LEFT'");
    }

    if (valignStr == "center") {
        emit(nodeId + ".align_y = 'MIDDLE'");
    } else if (valignStr == "top") {
        emit(nodeId + ".align_y = 'TOP'");
    } else if (valignStr == "bottom") {
        emit(nodeId + ".align_y = 'BOTTOM'");
    } else {
        emit(nodeId + ".align_y = 'BOTTOM'");  // baseline maps to bottom
    }

    emit("last_geo = " + nodeId);
    emit("x_pos += 200");
    emit("y_pos -= 50");
}

void BlenderGenerator::emitPolyhedron(const Arguments& args) {
    Value points = getArg(args, "points", getPositionalArg(args, 0, Value()));
    Value faces = getArg(args, "faces", getArg(args, "triangles", getPositionalArg(args, 1, Value())));

    emit("# Polyhedron (pure GeoNodes: MeshCircle + SetPosition per face)");

    // Evaluate all vertex coordinates to concrete (x, y, z) doubles
    struct Vec3 { double x, y, z; };
    std::vector<Vec3> verts;
    if (points.isVector()) {
        for (size_t i = 0; i < points.size(); ++i) {
            const Value& pt = points[i];
            if (pt.isVector() && pt.size() >= 3) {
                double x = pt[0].isNumber() ? pt[0].toNumber() :
                           pt[0].isExpression() ? evaluateExpr(pt[0]) : 0.0;
                double y = pt[1].isNumber() ? pt[1].toNumber() :
                           pt[1].isExpression() ? evaluateExpr(pt[1]) : 0.0;
                double z = pt[2].isNumber() ? pt[2].toNumber() :
                           pt[2].isExpression() ? evaluateExpr(pt[2]) : 0.0;
                verts.push_back({x, y, z});
            }
        }
    }

    // Parse face index lists and reverse winding (OpenSCAD CW → Blender CCW)
    std::vector<std::vector<int>> faceList;
    if (faces.isVector()) {
        for (size_t i = 0; i < faces.size(); ++i) {
            const Value& face = faces[i];
            if (face.isVector() && face.size() >= 3) {
                std::vector<int> faceIndices;
                // Reverse vertex order for correct normals
                for (size_t j = face.size(); j > 0; --j) {
                    int idx = static_cast<int>(face[j - 1].isNumber() ? face[j - 1].toNumber() :
                              face[j - 1].isExpression() ? evaluateExpr(face[j - 1]) : 0);
                    faceIndices.push_back(idx);
                }
                faceList.push_back(faceIndices);
            }
        }
    }

    if (verts.empty() || faceList.empty()) {
        emit("# Polyhedron skipped: empty vertices or faces");
        return;
    }

    // For each face, create a MeshCircle (K-gon) and set vertex positions
    std::vector<std::string> faceGeoIds;
    for (size_t fi = 0; fi < faceList.size(); ++fi) {
        const auto& face = faceList[fi];
        int K = static_cast<int>(face.size());

        // Create K-vertex polygon mesh with radius=0 (all verts at origin)
        std::string circleId = newNodeId();
        emit(circleId + " = nodes.new('GeometryNodeMeshCircle')");
        emit(circleId + ".location = (x_pos, y_pos)");
        emit(circleId + ".fill_type = 'NGON'");
        emit(circleId + ".inputs['Vertices'].default_value = " + std::to_string(K));
        emit(circleId + ".inputs['Radius'].default_value = 0.0");
        emit("y_pos -= 50");

        std::string lastGeo = circleId;
        std::string lastGeoSocket = "Mesh";

        // For each vertex in the face, offset it to the target position.
        // Since radius=0, all vertices start at origin, so Offset = target position.
        // We use Offset (not Position) because Position ignores Selection.
        for (int j = 0; j < K; ++j) {
            int vertIdx = face[j];
            if (vertIdx < 0 || vertIdx >= static_cast<int>(verts.size())) continue;
            double px = verts[vertIdx].x;
            double py = verts[vertIdx].y;
            double pz = verts[vertIdx].z;

            // Index node
            std::string idxId = newNodeId();
            emit(idxId + " = nodes.new('GeometryNodeInputIndex')");
            emit(idxId + ".location = (x_pos, y_pos)");
            emit("y_pos -= 50");

            // Compare: Index == j
            std::string cmpId = newNodeId();
            emit(cmpId + " = nodes.new('FunctionNodeCompare')");
            emit(cmpId + ".location = (x_pos, y_pos)");
            emit(cmpId + ".data_type = 'INT'");
            emit(cmpId + ".operation = 'EQUAL'");
            emit("links.new(" + idxId + ".outputs['Index'], " + cmpId + ".inputs[2])");
            emit(cmpId + ".inputs[3].default_value = " + std::to_string(j));
            emit("y_pos -= 50");

            // SetPosition: offset selected vertex to target position
            std::string setPosId = newNodeId();
            emit(setPosId + " = nodes.new('GeometryNodeSetPosition')");
            emit(setPosId + ".location = (x_pos, y_pos)");
            emit("links.new(" + lastGeo + ".outputs['" + lastGeoSocket + "'], " + setPosId + ".inputs['Geometry'])");
            emit("links.new(" + cmpId + ".outputs['Result'], " + setPosId + ".inputs['Selection'])");
            emit(setPosId + ".inputs['Offset'].default_value = (" +
                 std::to_string(px) + ", " + std::to_string(py) + ", " + std::to_string(pz) + ")");
            emit("y_pos -= 50");

            lastGeo = setPosId;
            lastGeoSocket = "Geometry";
        }
        faceGeoIds.push_back(lastGeo);
    }

    // Join all face meshes
    std::string joinId = newNodeId();
    emit(joinId + " = nodes.new('GeometryNodeJoinGeometry')");
    emit(joinId + ".location = (x_pos, y_pos)");
    for (auto& id : faceGeoIds) {
        emit("link_nodes(links, " + id + ", 'Geometry', " + joinId + ", 'Geometry')");
    }

    // Merge by distance to weld shared vertices
    std::string mergeId = newNodeId();
    emit(mergeId + " = nodes.new('GeometryNodeMergeByDistance')");
    emit(mergeId + ".location = (x_pos, y_pos)");
    emit("links.new(" + joinId + ".outputs['Geometry'], " + mergeId + ".inputs['Geometry'])");
    emit(mergeId + ".inputs['Distance'].default_value = 0.0001");

    emit("last_geo = " + mergeId);
    emit("x_pos += 200");
    emit("y_pos -= 50");
}

void BlenderGenerator::emitPolygon(const Arguments& args) {
    Value points = getArg(args, "points", getPositionalArg(args, 0, Value()));
    Value paths = getArg(args, "paths", getPositionalArg(args, 1, Value()));

    emit("# Polygon (pure GeoNodes: CurvePrimitiveLine segments)");

    // Try to evaluate all coordinates to concrete (x,y) doubles
    std::vector<std::pair<double, double>> coords;
    bool canEvaluate = points.isVector();
    if (canEvaluate) {
        for (size_t i = 0; i < points.size(); i++) {
            Value pt = points[i];

            // If the point is an expression (e.g. a function call returning [x,y]),
            // try to evaluate it to a concrete vector
            if (pt.isExpression()) {
                Value resolved = evaluateExprToValue(pt);
                if (resolved.isVector()) {
                    pt = resolved;
                } else {
                    canEvaluate = false;
                    break;
                }
            }

            if (pt.isVector() && pt.size() >= 2) {
                double x = pt[0].isNumber() ? pt[0].toNumber() :
                           pt[0].isExpression() ? evaluateExpr(pt[0]) : 0.0;
                double y = pt[1].isNumber() ? pt[1].toNumber() :
                           pt[1].isExpression() ? evaluateExpr(pt[1]) : 0.0;
                coords.push_back({x, y});
            }
        }
    }

    int validPts = static_cast<int>(coords.size());

    if (canEvaluate && validPts >= 2) {
        // Compile-time path: emit one CurvePrimitiveLine node per edge
        std::vector<std::string> lineIds;
        for (int i = 0; i < validPts; i++) {
            int next = (i + 1) % validPts;
            std::string lineId = newNodeId();
            emit(lineId + " = nodes.new('GeometryNodeCurvePrimitiveLine')");
            emit(lineId + ".location = (x_pos, y_pos)");
            emit(lineId + ".inputs['Start'].default_value = (" +
                 std::to_string(coords[i].first) + ", " +
                 std::to_string(coords[i].second) + ", 0)");
            emit(lineId + ".inputs['End'].default_value = (" +
                 std::to_string(coords[next].first) + ", " +
                 std::to_string(coords[next].second) + ", 0)");
            emit("y_pos -= 50");
            lineIds.push_back(lineId);
        }

        // Join all line segments
        std::string joinId = newNodeId();
        emit(joinId + " = nodes.new('GeometryNodeJoinGeometry')");
        emit(joinId + ".location = (x_pos, y_pos)");
        for (auto& id : lineIds) {
            emit("links.new(" + id + ".outputs['Curve'], " + joinId + ".inputs['Geometry'])");
        }

        emit("last_geo = " + joinId);
        emit("x_pos += 200");
        emit("y_pos -= 50");
    } else {
        // Runtime fallback: points computed at Python runtime (e.g. from function calls)
        // Emit a Python loop that creates CurvePrimitiveLine nodes dynamically
        std::string ptsVar = newNodeId() + "_pts";

        // Build the points list using toPython() to preserve runtime expressions
        std::string ptsCode = "[";
        if (points.isVector()) {
            for (size_t i = 0; i < points.size(); ++i) {
                if (i > 0) ptsCode += ", ";
                const Value& pt = points[i];
                if (pt.isVector() && pt.size() >= 2) {
                    ptsCode += "(" + pt[0].toPython() + ", " + pt[1].toPython() + ")";
                } else {
                    ptsCode += pt.toPython();
                }
            }
        }
        ptsCode += "]";

        emit(ptsVar + " = [p for p in " + ptsCode + " if isinstance(p, (list, tuple)) and len(p) >= 2]");

        // Guard: if fewer than 2 valid points at runtime, skip
        std::string joinId = newNodeId();
        emit(joinId + " = nodes.new('GeometryNodeJoinGeometry')");
        emit(joinId + ".location = (x_pos, y_pos)");
        emit("if len(" + ptsVar + ") >= 2:");
        indent_++;
        emit("for _i in range(len(" + ptsVar + ")):");
        indent_++;
        emit("_next = (_i + 1) % len(" + ptsVar + ")");
        std::string lineVar = newNodeId();
        emit(lineVar + " = nodes.new('GeometryNodeCurvePrimitiveLine')");
        emit(lineVar + ".location = (x_pos, y_pos)");
        emit(lineVar + ".inputs['Start'].default_value = (" + ptsVar + "[_i][0], " + ptsVar + "[_i][1], 0)");
        emit(lineVar + ".inputs['End'].default_value = (" + ptsVar + "[_next][0], " + ptsVar + "[_next][1], 0)");
        emit("links.new(" + lineVar + ".outputs['Curve'], " + joinId + ".inputs['Geometry'])");
        emit("y_pos -= 50");
        indent_--;
        indent_--;

        emit("last_geo = " + joinId);
        emit("x_pos += 200");
        emit("y_pos -= 50");
    }
}

// Transform generators
void BlenderGenerator::emitTranslate(const Arguments& args) {
    std::string nodeId = newNodeId();

    Value v = getArg(args, "v", getPositionalArg(args, 0, Value({0.0, 0.0, 0.0})));

    emit("# Translate");
    emit(nodeId + " = nodes.new('GeometryNodeTransform')");
    emit(nodeId + ".location = (x_pos, y_pos)");

    // If the value is an expression (not a vector), try to resolve the expression tree
    // to extract vector components (e.g., scalar * [x, y, z] → per-component expressions)
    if (v.isExpression() && v.exprTree()) {
        ExprNodePtr tree = resolveExprTree(v);
        if (tree && tree->kind == ExprNode::Kind::BinaryOp &&
            tree->op == ExprNode::Op::MULTIPLY) {
            // Check for scalar * vector or vector * scalar
            ExprNodePtr scalarSide = nullptr;
            ExprNodePtr vecSide = nullptr;
            if (tree->right && tree->right->kind == ExprNode::Kind::VectorLiteral) {
                scalarSide = tree->left;
                vecSide = tree->right;
            } else if (tree->left && tree->left->kind == ExprNode::Kind::VectorLiteral) {
                scalarSide = tree->right;
                vecSide = tree->left;
            }
            if (scalarSide && vecSide && vecSide->vec_elements.size() >= 3) {
                // Expand scalar * [x, y, z] into per-component expressions
                std::string combineId = newNodeId();
                emit("# CombineXYZ for Translation");
                emit(combineId + " = nodes.new('ShaderNodeCombineXYZ')");
                emit(combineId + ".location = (x_pos, y_pos)");
                emit("y_pos -= 50");
                const char* components[] = {"X", "Y", "Z"};
                for (size_t i = 0; i < 3 && i < vecSide->vec_elements.size(); ++i) {
                    auto compTree = ExprNode::makeBinary(ExprNode::Op::MULTIPLY,
                                                         scalarSide, vecSide->vec_elements[i]);
                    if (exprTreeReferencesModuleParams(compTree)) {
                        emit(combineId + ".inputs['" + std::string(components[i]) +
                             "'].default_value = " + exprTreeToPython(compTree));
                    } else if (exprTreeHasOnlyGroupInputVars(compTree)) {
                        auto result = emitExpressionNodeTree(compTree);
                        connectExprResultNamed(result, combineId, components[i]);
                    } else {
                        double val = evaluateExprTree(compTree);
                        emit(combineId + ".inputs['" + std::string(components[i]) +
                             "'].default_value = " + pyDouble(val));
                    }
                }
                emit("links.new(" + combineId + ".outputs['Vector'], " + nodeId + ".inputs['Translation'])");
                // Skip the normal vector handling below
                emit("link_nodes(links, last_geo, 'Geometry', " + nodeId + ", 'Geometry')");
                emit("last_geo = " + nodeId);
                emit("x_pos += 200");
                return;
            }
        }
        // Fallback: try evaluating the expression to a vector
        Value resolved = evaluateExprTreeToValue(tree);
        if (resolved.isVector() && resolved.size() >= 3) {
            v = resolved;
            // Fall through to vector handling below
        }
    }

    if (isSimpleVariableRef(v)) {
        // Link translation from group_input
        std::string varName = v.toString();
        std::string socketName = varName;
        if (varName[0] == '$') {
            socketName = varName.substr(1);
        }
        emit("links.new(group_input.outputs['" + socketName + "'], " + nodeId + ".inputs['Translation'])");
    } else if (v.isExpression() && v.exprTree() && exprTreeReferencesModuleParams(v.exprTree())) {
        // Module parameter vector: set translation from runtime Python variable
        std::string pyExpr = exprTreeToPython(v.exprTree());
        emit("_tv = " + pyExpr);
        emit("if isinstance(_tv, (list, tuple)) and len(_tv) >= 3:");
        emit("    " + nodeId + ".inputs['Translation'].default_value = (_tv[0], _tv[1], _tv[2])");
        emit("elif isinstance(_tv, (int, float)):");
        emit("    " + nodeId + ".inputs['Translation'].default_value = (_tv, _tv, _tv)");
    } else if (v.isExpression() && isRuntimePythonVar(v.toString())) {
        // Runtime Python variable (e.g. loop var or previously assigned var)
        std::string varName = pyName(v.toString());
        emit("if isinstance(" + varName + ", (list, tuple)) and len(" + varName + ") >= 3:");
        emit("    " + nodeId + ".inputs['Translation'].default_value = (" + varName + "[0], " + varName + "[1], " + varName + "[2])");
        emit("elif isinstance(" + varName + ", (int, float)):");
        emit("    " + nodeId + ".inputs['Translation'].default_value = (" + varName + ", " + varName + ", " + varName + ")");
    } else if (v.isVector() && v.size() >= 3 && vectorHasExprTrees(v)) {
        emitVectorWithExprTrees(nodeId, "Translation", v);
    } else if (v.isVector() && v.size() >= 3) {
        emit(nodeId + ".inputs['Translation'].default_value = " + vectorToPython(v));
    }

    emit("link_nodes(links, last_geo, 'Geometry', " + nodeId + ", 'Geometry')");
    emit("last_geo = " + nodeId);
    emit("x_pos += 200");
}

void BlenderGenerator::emitRotate(const Arguments& args) {
    std::string nodeId = newNodeId();

    Value a = getArg(args, "a", getPositionalArg(args, 0, Value(0.0)));
    Value v = getArg(args, "v", getPositionalArg(args, 1, Value()));

    emit("# Rotate (converting degrees to radians)");
    emit(nodeId + " = nodes.new('GeometryNodeTransform')");
    emit(nodeId + ".location = (x_pos, y_pos)");

    if (a.isVector() && a.size() >= 3) {
        // Check if any component has variable refs
        bool hasVarRefs = false;
        ExprNodePtr compTrees[3];
        for (int i = 0; i < 3; ++i) {
            compTrees[i] = getOrMakeLiteralTree(a[i]);
            if (compTrees[i]->hasVariableRefs()) hasVarRefs = true;
        }

        if (hasVarRefs) {
            // Check if all variable refs are group_input sockets
            bool allGroupInput = true;
            for (int i = 0; i < 3; ++i) {
                if (compTrees[i]->hasVariableRefs() && !exprTreeHasOnlyGroupInputVars(compTrees[i])) {
                    allGroupInput = false;
                    break;
                }
            }

            if (allGroupInput) {
                // Emit math nodes for component * PI / 180 per component
                std::string combineId = newNodeId();
                emit("# CombineXYZ for Rotation (degrees to radians via math nodes)");
                emit(combineId + " = nodes.new('ShaderNodeCombineXYZ')");
                emit(combineId + ".location = (x_pos, y_pos)");
                emit("y_pos -= 50");

                const char* components[] = {"X", "Y", "Z"};
                for (int i = 0; i < 3; ++i) {
                    // Build: component * PI / 180
                    ExprNodePtr radTree = ExprNode::makeBinary(
                        ExprNode::Op::MULTIPLY, compTrees[i],
                        ExprNode::makeLiteral(M_PI / 180.0));
                    auto result = emitExpressionNodeTree(radTree);
                    connectExprResultNamed(result, combineId, components[i]);
                }

                emit("links.new(" + combineId + ".outputs['Vector'], " + nodeId + ".inputs['Rotation'])");
            } else {
                // Non-group-input variables (e.g. loop variables, module params) — emit as Python expression
                std::string rx_expr = exprTreeToPython(compTrees[0]);
                std::string ry_expr = exprTreeToPython(compTrees[1]);
                std::string rz_expr = exprTreeToPython(compTrees[2]);
                emit(nodeId + ".inputs['Rotation'].default_value = (math.radians(" +
                     rx_expr + "), math.radians(" + ry_expr + "), math.radians(" + rz_expr + "))");
            }
        } else {
            // Euler angles - compile-time conversion
            double rx = (a[0].isExpression() ? evaluateExpr(a[0]) : a[0].toNumber()) * M_PI / 180.0;
            double ry = (a[1].isExpression() ? evaluateExpr(a[1]) : a[1].toNumber()) * M_PI / 180.0;
            double rz = (a[2].isExpression() ? evaluateExpr(a[2]) : a[2].toNumber()) * M_PI / 180.0;
            emit(nodeId + ".inputs['Rotation'].default_value = (" +
                 std::to_string(rx) + ", " + std::to_string(ry) + ", " + std::to_string(rz) + ")");
        }
    } else if (a.isNumber() || a.isExpression()) {
        // Scalar angle — check for expression tree
        ExprNodePtr aTree = getOrMakeLiteralTree(a);

        // Determine if v (axis vector) is provided
        bool hasAxisVec = false;
        bool axisIsRuntime = false;
        ExprNodePtr vTree;

        if (v.isVector() && v.size() >= 3) {
            hasAxisVec = true;
            for (size_t i = 0; i < 3; i++) {
                ExprNodePtr ct = getOrMakeLiteralTree(v[i]);
                if (ct->hasVariableRefs()) { axisIsRuntime = true; break; }
            }
        } else if (v.isExpression() && v.exprTree()) {
            hasAxisVec = true;
            vTree = resolveExprTree(v);
            if (vTree && vTree->hasVariableRefs()) axisIsRuntime = true;
        }

        bool angleIsRuntime = aTree->hasVariableRefs();

        if (hasAxisVec) {
            // Axis-angle rotation: rotate(angle, axis_vector)
            if (angleIsRuntime || axisIsRuntime) {
                // Runtime: emit Python code for axis-angle → Euler conversion
                std::string angleExpr;
                if (angleIsRuntime) {
                    angleExpr = "math.radians(" + exprTreeToPython(aTree) + ")";
                } else {
                    angleExpr = pyDouble(a.toNumber() * M_PI / 180.0);
                }

                std::string axisExpr;
                if (vTree) {
                    axisExpr = exprTreeToPython(vTree);
                } else if (v.isVector() && v.size() >= 3) {
                    // Build tuple from components (may have mixed var/literal)
                    std::string parts[3];
                    for (int i = 0; i < 3; i++) {
                        ExprNodePtr ct = getOrMakeLiteralTree(v[i]);
                        if (ct->hasVariableRefs()) {
                            parts[i] = exprTreeToPython(ct);
                        } else {
                            parts[i] = pyDouble(v[i].toNumber());
                        }
                    }
                    axisExpr = "(" + parts[0] + ", " + parts[1] + ", " + parts[2] + ")";
                }

                emit("# Axis-angle rotation");
                emit("_axis = Vector(" + axisExpr + ")");
                emit("if _axis.length > 0:");
                indent_++;
                emit("_axis.normalize()");
                emit("_rot_mat = Matrix.Rotation(" + angleExpr + ", 4, _axis)");
                emit("_euler = _rot_mat.to_euler()");
                emit(nodeId + ".inputs['Rotation'].default_value = (_euler.x, _euler.y, _euler.z)");
                indent_--;
                emit("else:");
                indent_++;
                emit(nodeId + ".inputs['Rotation'].default_value = (0, 0, 0)");
                indent_--;
            } else {
                // Compile-time: compute axis-angle → Euler in C++
                double angle_rad = a.toNumber() * M_PI / 180.0;
                double vx = v[0].toNumber(), vy = v[1].toNumber(), vz = v[2].toNumber();
                double len = sqrt(vx*vx + vy*vy + vz*vz);
                if (len > 1e-10) {
                    vx /= len; vy /= len; vz /= len;
                    // Rotation matrix from axis-angle (Rodrigues' formula)
                    double c = cos(angle_rad), s = sin(angle_rad), t = 1.0 - c;
                    double r00 = t*vx*vx + c,    r01 = t*vx*vy - s*vz, r02 = t*vx*vz + s*vy;
                    double r10 = t*vx*vy + s*vz, r11 = t*vy*vy + c,    r12 = t*vy*vz - s*vx;
                    double r20 = t*vx*vz - s*vy, r21 = t*vy*vz + s*vx, r22 = t*vz*vz + c;
                    // Decompose to Euler XYZ
                    double rx, ry, rz;
                    ry = asin(std::clamp(-(r20), -1.0, 1.0));
                    if (fabs(cos(ry)) > 1e-10) {
                        rx = atan2(r21, r22);
                        rz = atan2(r10, r00);
                    } else {
                        rx = atan2(-r12, r11);
                        rz = 0;
                    }
                    emit("# Axis-angle rotation (compile-time)");
                    emit(nodeId + ".inputs['Rotation'].default_value = (" +
                         pyDouble(rx) + ", " + pyDouble(ry) + ", " + pyDouble(rz) + ")");
                } else {
                    emit(nodeId + ".inputs['Rotation'].default_value = (0, 0, 0)");
                }
            }
        } else {
            // No axis vector — Z-axis rotation by default, or Euler if variable is a vector
            if (angleIsRuntime) {
                std::string pyExpr = exprTreeToPython(aTree);
                // Check if the variable might be a vector at runtime (e.g., loop variable in intersection_for)
                bool mightBeVector = false;
                if (aTree && aTree->kind == ExprNode::Kind::VarRef &&
                    loop_variables_.count(aTree->var_name)) {
                    mightBeVector = true;
                }

                if (mightBeVector) {
                    // Emit runtime check: if vector, use as Euler XYZ; if scalar, use as Z rotation
                    emit("if isinstance(" + pyExpr + ", (list, tuple)):");
                    indent_++;
                    emit(nodeId + ".inputs['Rotation'].default_value = (math.radians(" +
                         pyExpr + "[0]), math.radians(" + pyExpr + "[1]), math.radians(" + pyExpr + "[2]))");
                    indent_--;
                    emit("else:");
                    indent_++;
                    emit(nodeId + ".inputs['Rotation'].default_value = (0, 0, math.radians(" + pyExpr + "))");
                    indent_--;
                } else if (exprTreeReferencesModuleParams(aTree)) {
                    emit(nodeId + ".inputs['Rotation'].default_value = (0, 0, math.radians(" + pyExpr + "))");
                } else {
                    ExprNodePtr radTree = ExprNode::makeBinary(
                        ExprNode::Op::MULTIPLY, aTree,
                        ExprNode::makeLiteral(M_PI / 180.0));
                    emitScalarToVectorInput(nodeId, "Rotation", radTree, 2, 0.0, 0.0, 0.0);
                }
            } else {
                double angle = a.toNumber() * M_PI / 180.0;
                emit(nodeId + ".inputs['Rotation'].default_value = (0, 0, " + pyDouble(angle) + ")");
            }
        }
    }

    emit("link_nodes(links, last_geo, 'Geometry', " + nodeId + ", 'Geometry')");
    emit("last_geo = " + nodeId);
    emit("x_pos += 200");
}

void BlenderGenerator::emitScale(const Arguments& args) {
    std::string nodeId = newNodeId();

    Value v = getArg(args, "v", getPositionalArg(args, 0, Value(1.0)));

    emit("# Scale");
    emit(nodeId + " = nodes.new('GeometryNodeTransform')");
    emit(nodeId + ".location = (x_pos, y_pos)");

    if (isSimpleVariableRef(v)) {
        // Link scale from group_input
        std::string varName = v.toString();
        std::string socketName = varName;
        if (varName[0] == '$') {
            socketName = varName.substr(1);
        }
        emit("links.new(group_input.outputs['" + socketName + "'], " + nodeId + ".inputs['Scale'])");
    } else if (v.isVector() && v.size() >= 3 && vectorHasExprTrees(v)) {
        emitVectorWithExprTrees(nodeId, "Scale", v);
    } else if (v.isVector() && v.size() >= 3) {
        emit(nodeId + ".inputs['Scale'].default_value = " + vectorToPython(v));
    } else if (v.isNumber()) {
        double s = v.toNumber();
        emit(nodeId + ".inputs['Scale'].default_value = (" +
             pyDouble(s) + ", " + pyDouble(s) + ", " + pyDouble(s) + ")");
    }

    emit("link_nodes(links, last_geo, 'Geometry', " + nodeId + ", 'Geometry')");
    emit("last_geo = " + nodeId);
    emit("x_pos += 200");
}

void BlenderGenerator::emitMirror(const Arguments& args) {
    std::string nodeId = newNodeId();

    Value v = getArg(args, "v", getPositionalArg(args, 0, Value({1.0, 0.0, 0.0})));

    emit("# Mirror (using scale with negative values)");
    emit(nodeId + " = nodes.new('GeometryNodeTransform')");
    emit(nodeId + ".location = (x_pos, y_pos)");

    if (v.isVector() && v.size() >= 3) {
        double sx = v[0].toNumber() != 0 ? -1.0 : 1.0;
        double sy = v[1].toNumber() != 0 ? -1.0 : 1.0;
        double sz = v[2].toNumber() != 0 ? -1.0 : 1.0;
        emit(nodeId + ".inputs['Scale'].default_value = (" +
             std::to_string(sx) + ", " + std::to_string(sy) + ", " + std::to_string(sz) + ")");
    }

    emit("link_nodes(links, last_geo, 'Geometry', " + nodeId + ", 'Geometry')");
    emit("last_geo = " + nodeId);
    emit("x_pos += 200");

    // Negative scale flips face normals — add FlipFaces to fix winding order
    std::string flipId = newNodeId();
    emit("# FlipFaces to fix normals after mirror");
    emit(flipId + " = nodes.new('GeometryNodeFlipFaces')");
    emit(flipId + ".location = (x_pos, y_pos)");
    emit("link_nodes(links, last_geo, 'Geometry', " + flipId + ", 'Geometry')");
    emit("last_geo = " + flipId);
    emit("x_pos += 200");
}

// ─── DXF Import ────────────────────────────────────────────────────────────

// DXF entity types we care about
struct DxfSegment {
    enum Type { LINE_SEG, ARC_SEG, CIRCLE_SEG };
    Type type;
    std::string layer;
    // LINE: (x1,y1)-(x2,y2)
    double x1, y1, x2, y2;
    // ARC/CIRCLE: center (cx,cy), radius, start_angle, end_angle (degrees)
    double cx, cy, radius, startAngle, endAngle;
};

// Parse DXF file and extract LINE, ARC, and CIRCLE entities (with INSERT/BLOCK expansion)
static std::vector<DxfSegment> parseDxfEntities(const std::string& filepath) {
    std::vector<DxfSegment> segments;
    std::ifstream file(filepath);
    if (!file.is_open()) return segments;

    // Read all code/value pairs
    std::vector<std::pair<int,std::string>> pairs;
    std::string codeLine, valLine;
    while (std::getline(file, codeLine) && std::getline(file, valLine)) {
        auto trim = [](std::string& s) {
            size_t a = s.find_first_not_of(" \t\r\n");
            size_t b = s.find_last_not_of(" \t\r\n");
            s = (a == std::string::npos) ? "" : s.substr(a, b - a + 1);
        };
        trim(codeLine); trim(valLine);
        int code = 0;
        try { code = std::stoi(codeLine); } catch (...) { continue; }
        pairs.push_back({code, valLine});
    }

    // Helper to parse entities from a range of pairs into a vector
    auto parseSegments = [](const std::vector<std::pair<int,std::string>>& pairs,
                            size_t start, size_t end, const std::string& defaultLayer) {
        std::vector<DxfSegment> segs;
        std::string entityType;
        DxfSegment cur = {};
        auto flush = [&]() {
            if (entityType == "LINE") { cur.type = DxfSegment::LINE_SEG; if (cur.layer.empty()) cur.layer = defaultLayer; segs.push_back(cur); }
            else if (entityType == "ARC") { cur.type = DxfSegment::ARC_SEG; if (cur.layer.empty()) cur.layer = defaultLayer; segs.push_back(cur); }
            else if (entityType == "CIRCLE") { cur.type = DxfSegment::CIRCLE_SEG; if (cur.layer.empty()) cur.layer = defaultLayer; segs.push_back(cur); }
        };
        for (size_t i = start; i < end; i++) {
            int code = pairs[i].first;
            const std::string& val = pairs[i].second;
            if (code == 0) {
                flush();
                entityType = val;
                cur = {};
                continue;
            }
            if (code == 8) cur.layer = val;
            else if (code == 10) { cur.x1 = std::stod(val); cur.cx = std::stod(val); }
            else if (code == 20) { cur.y1 = std::stod(val); cur.cy = std::stod(val); }
            else if (code == 11) cur.x2 = std::stod(val);
            else if (code == 21) cur.y2 = std::stod(val);
            else if (code == 40) cur.radius = std::stod(val);
            else if (code == 50) cur.startAngle = std::stod(val);
            else if (code == 51) cur.endAngle = std::stod(val);
        }
        flush();
        return segs;
    };

    // Pass 1: Find section boundaries
    size_t blocksStart = 0, blocksEnd = 0, entitiesStart = 0, entitiesEnd = 0;
    for (size_t i = 0; i < pairs.size(); i++) {
        if (pairs[i].first == 2 && pairs[i].second == "BLOCKS") blocksStart = i + 1;
        if (pairs[i].first == 2 && pairs[i].second == "ENTITIES") entitiesStart = i + 1;
        if (pairs[i].first == 0 && pairs[i].second == "ENDSEC") {
            if (entitiesStart > 0 && entitiesEnd == 0 && i > entitiesStart) entitiesEnd = i;
            else if (blocksStart > 0 && blocksEnd == 0 && i > blocksStart) blocksEnd = i;
        }
    }

    // Pass 2: Parse blocks
    struct DxfBlock {
        std::string name;
        std::vector<DxfSegment> entities;
    };
    std::map<std::string, DxfBlock> blocks;
    if (blocksStart > 0 && blocksEnd > blocksStart) {
        size_t i = blocksStart;
        while (i < blocksEnd) {
            // Find BLOCK entity
            if (pairs[i].first == 0 && pairs[i].second == "BLOCK") {
                DxfBlock block;
                i++;
                // Read block properties until first sub-entity or ENDBLK
                size_t blockBodyStart = 0;
                while (i < blocksEnd) {
                    if (pairs[i].first == 2 && block.name.empty()) block.name = pairs[i].second;
                    if (pairs[i].first == 0) { blockBodyStart = i; break; }
                    i++;
                }
                // Find ENDBLK
                size_t blockEnd = blockBodyStart;
                while (blockEnd < blocksEnd) {
                    if (pairs[blockEnd].first == 0 && pairs[blockEnd].second == "ENDBLK") break;
                    blockEnd++;
                }
                if (blockBodyStart > 0 && blockEnd > blockBodyStart) {
                    block.entities = parseSegments(pairs, blockBodyStart, blockEnd, "0");
                }
                if (!block.name.empty() && !block.entities.empty()) {
                    blocks[block.name] = block;
                }
                i = blockEnd + 1;
            } else {
                i++;
            }
        }
    }

    // Pass 3: Parse ENTITIES section (direct entities)
    if (entitiesStart > 0 && entitiesEnd > entitiesStart) {
        segments = parseSegments(pairs, entitiesStart, entitiesEnd, "0");

        // Also look for INSERT entities and expand them
        struct InsertRef {
            std::string blockName, layer;
            double x = 0, y = 0, rotation = 0, xscale = 1, yscale = 1;
        };
        std::vector<InsertRef> inserts;
        std::string entityType;
        InsertRef curInsert;
        bool inInsert = false;
        for (size_t i = entitiesStart; i < entitiesEnd; i++) {
            if (pairs[i].first == 0) {
                if (inInsert) inserts.push_back(curInsert);
                inInsert = (pairs[i].second == "INSERT");
                curInsert = InsertRef{};
                continue;
            }
            if (!inInsert) continue;
            int code = pairs[i].first;
            const std::string& val = pairs[i].second;
            if (code == 2) curInsert.blockName = val;
            else if (code == 8) curInsert.layer = val;
            else if (code == 10) curInsert.x = std::stod(val);
            else if (code == 20) curInsert.y = std::stod(val);
            else if (code == 41) curInsert.xscale = std::stod(val);
            else if (code == 42) curInsert.yscale = std::stod(val);
            else if (code == 50) curInsert.rotation = std::stod(val);
        }
        if (inInsert) inserts.push_back(curInsert);

        // Expand INSERT references
        for (const auto& ins : inserts) {
            auto it = blocks.find(ins.blockName);
            if (it == blocks.end()) continue;

            double rotRad = ins.rotation * M_PI / 180.0;
            double cosR = cos(rotRad), sinR = sin(rotRad);

            auto transform = [&](double x, double y) -> std::pair<double,double> {
                double sx = x * ins.xscale;
                double sy = y * ins.yscale;
                double rx = sx * cosR - sy * sinR;
                double ry = sx * sinR + sy * cosR;
                return {rx + ins.x, ry + ins.y};
            };

            for (const auto& seg : it->second.entities) {
                DxfSegment transformed = seg;
                transformed.layer = ins.layer.empty() ? seg.layer : ins.layer;
                if (seg.type == DxfSegment::LINE_SEG) {
                    auto [nx1, ny1] = transform(seg.x1, seg.y1);
                    auto [nx2, ny2] = transform(seg.x2, seg.y2);
                    transformed.x1 = nx1; transformed.y1 = ny1;
                    transformed.x2 = nx2; transformed.y2 = ny2;
                } else if (seg.type == DxfSegment::ARC_SEG || seg.type == DxfSegment::CIRCLE_SEG) {
                    auto [ncx, ncy] = transform(seg.cx, seg.cy);
                    transformed.cx = ncx; transformed.cy = ncy;
                    transformed.radius = seg.radius * std::abs(ins.xscale);
                    if (seg.type == DxfSegment::ARC_SEG) {
                        transformed.startAngle = seg.startAngle + ins.rotation;
                        transformed.endAngle = seg.endAngle + ins.rotation;
                    }
                }
                segments.push_back(transformed);
            }
        }
    }

    return segments;
}

// Tessellate an arc into polyline points (start and end points included)
static std::vector<std::pair<double, double>> tessellateArc(
    double cx, double cy, double radius, double startDeg, double endDeg, int numSegs = 32) {
    std::vector<std::pair<double, double>> pts;
    double startRad = startDeg * M_PI / 180.0;
    double endRad = endDeg * M_PI / 180.0;
    // ARC goes counter-clockwise from startAngle to endAngle
    if (endRad <= startRad) endRad += 2.0 * M_PI;
    for (int i = 0; i <= numSegs; i++) {
        double t = static_cast<double>(i) / numSegs;
        double angle = startRad + t * (endRad - startRad);
        pts.push_back({cx + radius * cos(angle), cy + radius * sin(angle)});
    }
    return pts;
}

// Tessellate a full circle
static std::vector<std::pair<double, double>> tessellateCircle(
    double cx, double cy, double radius, int numSegs = 64) {
    std::vector<std::pair<double, double>> pts;
    for (int i = 0; i < numSegs; i++) {
        double angle = 2.0 * M_PI * i / numSegs;
        pts.push_back({cx + radius * cos(angle), cy + radius * sin(angle)});
    }
    return pts;
}

// Convert a segment to a pair of endpoints (for LINEs and ARCs, used in chaining)
static std::pair<std::pair<double,double>, std::pair<double,double>> segmentEndpoints(const DxfSegment& seg) {
    if (seg.type == DxfSegment::LINE_SEG) {
        return {{seg.x1, seg.y1}, {seg.x2, seg.y2}};
    } else if (seg.type == DxfSegment::ARC_SEG) {
        double startRad = seg.startAngle * M_PI / 180.0;
        double endRad = seg.endAngle * M_PI / 180.0;
        double sx = seg.cx + seg.radius * cos(startRad);
        double sy = seg.cy + seg.radius * sin(startRad);
        double ex = seg.cx + seg.radius * cos(endRad);
        double ey = seg.cy + seg.radius * sin(endRad);
        return {{sx, sy}, {ex, ey}};
    }
    return {{0,0},{0,0}};
}

// Get tessellated points for a segment (in forward direction)
static std::vector<std::pair<double,double>> segmentPoints(const DxfSegment& seg, bool reverse = false) {
    std::vector<std::pair<double,double>> pts;
    if (seg.type == DxfSegment::LINE_SEG) {
        if (reverse) {
            pts.push_back({seg.x2, seg.y2});
            pts.push_back({seg.x1, seg.y1});
        } else {
            pts.push_back({seg.x1, seg.y1});
            pts.push_back({seg.x2, seg.y2});
        }
    } else if (seg.type == DxfSegment::ARC_SEG) {
        pts = tessellateArc(seg.cx, seg.cy, seg.radius, seg.startAngle, seg.endAngle);
        if (reverse) std::reverse(pts.begin(), pts.end());
    }
    return pts;
}

// Chain segments (LINEs and ARCs) into ordered polygon points
// Returns multiple chains if there are disconnected loops
static std::vector<std::vector<std::pair<double, double>>> chainSegments(const std::vector<DxfSegment>& segs) {
    if (segs.empty()) return {};

    auto close = [](double a, double b) { return std::abs(a - b) < 0.01; };
    auto ptMatch = [&](double x1, double y1, double x2, double y2) {
        return close(x1, x2) && close(y1, y2);
    };

    std::vector<bool> used(segs.size(), false);
    std::vector<std::vector<std::pair<double,double>>> chains;

    while (true) {
        // Find first unused non-circle segment
        int startIdx = -1;
        for (size_t i = 0; i < segs.size(); i++) {
            if (!used[i] && segs[i].type != DxfSegment::CIRCLE_SEG) {
                startIdx = static_cast<int>(i);
                break;
            }
        }
        if (startIdx < 0) break;

        std::vector<std::pair<double,double>> chain;
        used[startIdx] = true;
        auto pts = segmentPoints(segs[startIdx]);
        for (auto& p : pts) chain.push_back(p);

        bool found = true;
        while (found) {
            found = false;
            double lastX = chain.back().first;
            double lastY = chain.back().second;

            for (size_t i = 0; i < segs.size(); i++) {
                if (used[i] || segs[i].type == DxfSegment::CIRCLE_SEG) continue;
                auto [ep1, ep2] = segmentEndpoints(segs[i]);
                if (ptMatch(lastX, lastY, ep1.first, ep1.second)) {
                    used[i] = true;
                    auto npts = segmentPoints(segs[i], false);
                    // Skip first point (it matches last)
                    for (size_t j = 1; j < npts.size(); j++) chain.push_back(npts[j]);
                    found = true;
                    break;
                }
                if (ptMatch(lastX, lastY, ep2.first, ep2.second)) {
                    used[i] = true;
                    auto npts = segmentPoints(segs[i], true);
                    for (size_t j = 1; j < npts.size(); j++) chain.push_back(npts[j]);
                    found = true;
                    break;
                }
            }
        }

        // Remove last point if it matches first (closed polygon)
        if (chain.size() > 2) {
            auto& f = chain.front();
            auto& l = chain.back();
            if (close(f.first, l.first) && close(f.second, l.second)) {
                chain.pop_back();
            }
        }

        if (chain.size() >= 3) chains.push_back(chain);
    }

    // Handle standalone circles
    for (size_t i = 0; i < segs.size(); i++) {
        if (!used[i] && segs[i].type == DxfSegment::CIRCLE_SEG) {
            auto pts = tessellateCircle(segs[i].cx, segs[i].cy, segs[i].radius);
            if (pts.size() >= 3) chains.push_back(pts);
            used[i] = true;
        }
    }

    return chains;
}

void BlenderGenerator::emitImport(const Arguments& args) {
    Value fileVal = getArg(args, "file", getPositionalArg(args, 0, Value()));
    Value layerVal = getArg(args, "layer", Value());
    Value originVal = getArg(args, "origin", Value());

    if (fileVal.isUndefined() || !fileVal.isString()) {
        emit("# Import: no file specified");
        return;
    }

    std::string filename = fileVal.toString();
    if (filename.size() >= 2 && filename.front() == '"' && filename.back() == '"') {
        filename = filename.substr(1, filename.size() - 2);
    }

    std::string ext = filename;
    size_t dotPos = ext.rfind('.');
    if (dotPos != std::string::npos) {
        ext = ext.substr(dotPos + 1);
        std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
    }

    if (ext == "stl") {
        // Use Geometry Nodes "Import STL" node
        std::string filepath = filename;
        if (!source_dir_.empty() && filename[0] != '/') {
            filepath = source_dir_ + "/" + filename;
        }
        emit("# Import STL: " + filename);
        std::string importId = newNodeId();
        emit(importId + " = nodes.new('GeometryNodeImportSTL')");
        emit(importId + ".location = (x_pos, y_pos)");
        emit(importId + ".inputs['Path'].default_value = '" + filepath + "'");
        emit("last_geo = " + importId);
        emit("x_pos += 200");
        return;
    }

    if (ext != "dxf") {
        emit("# Import: unsupported file type: " + filename);
        return;
    }

    // Resolve file path
    std::string filepath = filename;
    if (!source_dir_.empty() && filename[0] != '/') {
        filepath = source_dir_ + "/" + filename;
    }

    // Parse DXF
    auto allSegments = parseDxfEntities(filepath);
    if (allSegments.empty()) {
        emit("# Import: could not read DXF file: " + filepath);
        return;
    }

    // Filter by layer if specified
    std::string layer;
    if (!layerVal.isUndefined()) {
        layer = layerVal.toString();
        if (layer.size() >= 2 && layer.front() == '"' && layer.back() == '"') {
            layer = layer.substr(1, layer.size() - 2);
        }
    }

    std::vector<DxfSegment> layerSegs;
    for (auto& s : allSegments) {
        if (layer.empty() || s.layer == layer) {
            layerSegs.push_back(s);
        }
    }

    if (layerSegs.empty()) {
        emit("# Import: no entities found on layer '" + layer + "' in " + filename);
        return;
    }

    // Get origin offset
    double originX = 0, originY = 0;
    if (!originVal.isUndefined() && originVal.isVector() && originVal.size() >= 2) {
        originX = originVal[0].toNumber();
        originY = originVal[1].toNumber();
    }

    // Chain segments into polygon(s)
    auto chains = chainSegments(layerSegs);
    if (chains.empty()) {
        emit("# Import: could not chain entities into polygon on layer '" + layer + "'");
        return;
    }

    // Apply origin offset to all points
    for (auto& chain : chains) {
        for (auto& pt : chain) {
            pt.first -= originX;
            pt.second -= originY;
        }
    }

    // Emit each chain as a separate spline in the same curve object
    int totalPoints = 0;
    for (auto& chain : chains) totalPoints += static_cast<int>(chain.size());

    emit("# Import DXF: " + filename + " layer=" + layer +
         " (" + std::to_string(chains.size()) + " chain(s), " +
         std::to_string(totalPoints) + " points)");

    std::string objVar = newNodeId() + "_dxf_obj";
    emit("_cd = bpy.data.curves.new('_dxf_" + layer + "', type='CURVE')");
    emit("_cd.dimensions = '3D'");

    for (size_t c = 0; c < chains.size(); c++) {
        auto& points = chains[c];
        int N = static_cast<int>(points.size());
        std::string spVar = (c == 0) ? "_sp" : "_sp" + std::to_string(c);
        emit(spVar + " = _cd.splines.new('POLY')");
        emit(spVar + ".points.add(" + std::to_string(N - 1) + ")");
        for (int i = 0; i < N; i++) {
            emit(spVar + ".points[" + std::to_string(i) + "].co = (" +
                 pyDouble(points[i].first) + ", " +
                 pyDouble(points[i].second) + ", 0, 1)");
        }
        emit(spVar + ".use_cyclic_u = True");
    }

    emit(objVar + " = bpy.data.objects.new('_dxf_" + layer + "', _cd)");
    emit("bpy.context.collection.objects.link(" + objVar + ")");

    // Use ObjectInfo to bring the curve into the node tree
    std::string objInfoId = newNodeId();
    emit(objInfoId + " = nodes.new('GeometryNodeObjectInfo')");
    emit(objInfoId + ".location = (x_pos, y_pos)");
    emit(objInfoId + ".transform_space = 'RELATIVE'");
    emit(objInfoId + ".inputs['Object'].default_value = " + objVar);
    emit("last_geo = " + objInfoId);
    emit("x_pos += 200");
}

// ─── Surface (heightmap from .dat file) ────────────────────────────────────

// Parse an Octave-format .dat file into a 2D grid of doubles
static std::vector<std::vector<double>> parseSurfaceData(const std::string& filepath) {
    std::vector<std::vector<double>> grid;
    std::ifstream file(filepath);
    if (!file.is_open()) return grid;

    std::string line;
    while (std::getline(file, line)) {
        // Skip comment lines
        if (line.empty() || line[0] == '#') continue;
        // Parse space-separated values
        std::vector<double> row;
        std::istringstream iss(line);
        double val;
        while (iss >> val) {
            row.push_back(val);
        }
        if (!row.empty()) grid.push_back(row);
    }
    return grid;
}

void BlenderGenerator::emitSurface(const Arguments& args) {
    Value fileVal = getArg(args, "file", getPositionalArg(args, 0, Value()));
    Value centerVal = getArg(args, "center", Value(false));

    if (fileVal.isUndefined() || !fileVal.isString()) {
        emit("# Surface: no file specified");
        return;
    }

    std::string filename = fileVal.toString();
    if (filename.size() >= 2 && filename.front() == '"' && filename.back() == '"') {
        filename = filename.substr(1, filename.size() - 2);
    }

    // Resolve file path
    std::string filepath = filename;
    if (!source_dir_.empty() && filename[0] != '/') {
        filepath = source_dir_ + "/" + filename;
    }

    auto grid = parseSurfaceData(filepath);
    if (grid.empty()) {
        emit("# Surface: could not read data file: " + filepath);
        return;
    }

    int rows = static_cast<int>(grid.size());
    int cols = static_cast<int>(grid[0].size());

    // Find minimum value for ground plane
    double minZ = grid[0][0];
    for (const auto& row : grid) {
        for (double v : row) {
            if (v < minZ) minZ = v;
        }
    }

    bool center = false;
    if (!centerVal.isUndefined()) {
        if (centerVal.isBool()) center = centerVal.toBool();
        else if (centerVal.isNumber()) center = centerVal.toNumber() != 0;
    }

    emit("# Surface heightmap: " + filename + " (" + std::to_string(rows) + "x" + std::to_string(cols) + ")");

    // Create the mesh via bmesh in Python, then wrap in ObjectInfo
    std::string objVar = newNodeId() + "_surf_obj";
    emit("import bmesh as _bmesh");
    emit("_bm = _bmesh.new()");

    // Emit vertices: one per grid point, with Z = height value
    // OpenSCAD surface: each data point at integer x,y coordinates
    double offsetX = center ? -(cols - 1) / 2.0 : 0;
    double offsetY = center ? -(rows - 1) / 2.0 : 0;

    emit("_surf_verts = []");
    for (int r = 0; r < rows; r++) {
        for (int c = 0; c < cols; c++) {
            double x = c + offsetX;
            double y = r + offsetY;
            double z = (c < static_cast<int>(grid[r].size())) ? grid[r][c] : 0;
            emit("_surf_verts.append(_bm.verts.new((" +
                 pyDouble(x) + ", " + pyDouble(y) + ", " + pyDouble(z) + ")))");
        }
    }
    emit("_bm.verts.ensure_lookup_table()");

    // Emit faces: quads connecting adjacent grid points
    // Also add bottom faces (z=0 plane) and side walls for a solid mesh
    for (int r = 0; r < rows - 1; r++) {
        for (int c = 0; c < cols - 1; c++) {
            int i00 = r * cols + c;
            int i10 = r * cols + (c + 1);
            int i01 = (r + 1) * cols + c;
            int i11 = (r + 1) * cols + (c + 1);
            emit("_bm.faces.new([_surf_verts[" + std::to_string(i00) + "], _surf_verts[" +
                 std::to_string(i10) + "], _surf_verts[" + std::to_string(i11) + "], _surf_verts[" +
                 std::to_string(i01) + "]])");
        }
    }

    // Add bottom plane at z=minZ (ground plane at minimum data value)
    int baseStart = rows * cols;
    emit("_base_verts = []");
    for (int r = 0; r < rows; r++) {
        for (int c = 0; c < cols; c++) {
            double x = c + offsetX;
            double y = r + offsetY;
            emit("_base_verts.append(_bm.verts.new((" +
                 pyDouble(x) + ", " + pyDouble(y) + ", " + pyDouble(minZ) + ")))");
        }
    }
    emit("_bm.verts.ensure_lookup_table()");

    // Bottom faces (reversed winding for outward normal)
    for (int r = 0; r < rows - 1; r++) {
        for (int c = 0; c < cols - 1; c++) {
            int i00 = baseStart + r * cols + c;
            int i10 = baseStart + r * cols + (c + 1);
            int i01 = baseStart + (r + 1) * cols + c;
            int i11 = baseStart + (r + 1) * cols + (c + 1);
            emit("_bm.faces.new([_bm.verts[" + std::to_string(i00) + "], _bm.verts[" +
                 std::to_string(i01) + "], _bm.verts[" + std::to_string(i11) + "], _bm.verts[" +
                 std::to_string(i10) + "]])");
        }
    }

    // Side walls: connect top edge vertices to bottom edge vertices
    // Front edge (r=0)
    for (int c = 0; c < cols - 1; c++) {
        int t0 = c, t1 = c + 1;
        int b0 = baseStart + c, b1 = baseStart + c + 1;
        emit("_bm.faces.new([_bm.verts[" + std::to_string(t0) + "], _bm.verts[" +
             std::to_string(b0) + "], _bm.verts[" + std::to_string(b1) + "], _bm.verts[" +
             std::to_string(t1) + "]])");
    }
    // Back edge (r=rows-1)
    for (int c = 0; c < cols - 1; c++) {
        int t0 = (rows-1) * cols + c, t1 = (rows-1) * cols + c + 1;
        int b0 = baseStart + (rows-1) * cols + c, b1 = baseStart + (rows-1) * cols + c + 1;
        emit("_bm.faces.new([_bm.verts[" + std::to_string(t0) + "], _bm.verts[" +
             std::to_string(t1) + "], _bm.verts[" + std::to_string(b1) + "], _bm.verts[" +
             std::to_string(b0) + "]])");
    }
    // Left edge (c=0)
    for (int r = 0; r < rows - 1; r++) {
        int t0 = r * cols, t1 = (r+1) * cols;
        int b0 = baseStart + r * cols, b1 = baseStart + (r+1) * cols;
        emit("_bm.faces.new([_bm.verts[" + std::to_string(t0) + "], _bm.verts[" +
             std::to_string(t1) + "], _bm.verts[" + std::to_string(b1) + "], _bm.verts[" +
             std::to_string(b0) + "]])");
    }
    // Right edge (c=cols-1)
    for (int r = 0; r < rows - 1; r++) {
        int t0 = r * cols + (cols-1), t1 = (r+1) * cols + (cols-1);
        int b0 = baseStart + r * cols + (cols-1), b1 = baseStart + (r+1) * cols + (cols-1);
        emit("_bm.faces.new([_bm.verts[" + std::to_string(t0) + "], _bm.verts[" +
             std::to_string(b0) + "], _bm.verts[" + std::to_string(b1) + "], _bm.verts[" +
             std::to_string(t1) + "]])");
    }

    // Convert bmesh to mesh object
    emit("_surf_mesh = bpy.data.meshes.new('_surf_" + filename + "')");
    emit("_bm.to_mesh(_surf_mesh)");
    emit("_bm.free()");
    emit(objVar + " = bpy.data.objects.new('_surf_" + filename + "', _surf_mesh)");
    emit("bpy.context.collection.objects.link(" + objVar + ")");

    // Use ObjectInfo to bring it into the node tree
    std::string objInfoId = newNodeId();
    emit(objInfoId + " = nodes.new('GeometryNodeObjectInfo')");
    emit(objInfoId + ".location = (x_pos, y_pos)");
    emit(objInfoId + ".transform_space = 'RELATIVE'");
    emit(objInfoId + ".inputs['Object'].default_value = " + objVar);
    emit("last_geo = " + objInfoId);
    emit("x_pos += 200");
}

void BlenderGenerator::emitOffset(const Arguments& args) {
    Value r = getArg(args, "r", getPositionalArg(args, 0, Value()));
    Value delta = getArg(args, "delta", Value());

    // Determine offset value and whether to use corner rounding (r mode vs delta mode)
    bool useRounding = !r.isUndefined() || delta.isUndefined();
    Value offsetVal;
    if (!r.isUndefined()) {
        offsetVal = r;
    } else if (!delta.isUndefined()) {
        offsetVal = delta;
    } else {
        // Positional arg defaults to r mode
        offsetVal = getPositionalArg(args, 0, Value(1.0));
    }

    // Use toPython() for the Python value string — this preserves variable references
    // (e.g., "wall / 2") that are valid Python inside module functions.
    std::string offsetPython = offsetVal.isExpression() ?
        offsetVal.toPython() : pyDouble(offsetVal.toNumber());

    // Resolve $fn for corner rounding segments
    Value fn = resolveFn(args);
    int fnVal = static_cast<int>(evaluateExpr(fn));

    emit("# Offset");

    if (useRounding) {
        // r mode: first fillet corners for rounding, then offset the curve.
        // FilletCurve rounds corners with the specified radius.
        std::string filletId = newNodeId();
        emit(filletId + " = nodes.new('GeometryNodeFilletCurve')");
        emit(filletId + ".location = (x_pos, y_pos)");

        // Fillet radius = absolute value of offset amount
        ExprNodePtr offsetTree = getOrMakeLiteralTree(offsetVal);
        if (offsetTree->hasVariableRefs() && exprTreeHasOnlyGroupInputVars(offsetTree)) {
            // Build abs(offset) tree for the radius
            ExprNodePtr absTree = ExprNode::makeFunctionCall("abs", {offsetTree});
            auto result = emitExpressionNodeTree(absTree);
            connectExprResultNamed(result, filletId, "Radius");
        } else {
            double absOffset = std::abs(offsetVal.toNumber());
            emit(filletId + ".inputs['Radius'].default_value = " + std::to_string(absOffset));
        }

        // Set Poly mode with $fn-controlled count for corner resolution.
        // In Blender 5.1+, mode is a menu input socket ('Bézier'/'Poly'), not a property.
        emit(filletId + ".inputs['Mode'].default_value = 'Poly'");
        // Count = max(1, $fn / 4)
        ExprNodePtr fnTree = getOrMakeLiteralTree(fn);
        if (fnTree->hasVariableRefs() && exprTreeHasOnlyGroupInputVars(fnTree)) {
            // Build $fn / 4 node tree, linked from group_input
            ExprNodePtr fnDiv4 = ExprNode::makeBinary(ExprNode::Op::DIVIDE, fnTree, ExprNode::makeLiteral(4.0));
            auto fnResult = emitExpressionNodeTree(fnDiv4);
            // Clamp: max(result, 1) using Math node
            std::string maxId = newNodeId();
            emit(maxId + " = nodes.new('ShaderNodeMath')");
            emit(maxId + ".operation = 'MAXIMUM'");
            emit(maxId + ".location = (x_pos, y_pos)");
            connectExprResult(fnResult, maxId, 0);
            emit(maxId + ".inputs[1].default_value = 1");
            emit("links.new(" + maxId + ".outputs['Value'], " + filletId + ".inputs['Count'])");
        } else if (in_module_ && fnTree->hasVariableRefs() && exprTreeReferencesModuleParams(fnTree)) {
            std::string fnPy = exprTreeToPython(fnTree);
            emit(filletId + ".inputs['Count'].default_value = max(1, int(" + fnPy + ") // 4)");
        } else {
            int count = fnVal / 4;
            if (count < 1) count = 1;
            emit(filletId + ".inputs['Count'].default_value = " + std::to_string(count));
        }

        emit("link_nodes(links, last_geo, 'Curve', " + filletId + ", 'Curve')");
        emit("last_geo = " + filletId);
        emit("x_pos += 200");
    } else {
        // delta mode: subdivide straight edges for smooth offset without corner rounding.
        std::string subdivId = newNodeId();
        emit(subdivId + " = nodes.new('GeometryNodeSubdivideCurve')");
        emit(subdivId + ".location = (x_pos, y_pos)");

        // Use $fn/4 cuts per edge for consistent resolution
        ExprNodePtr fnTreeD = getOrMakeLiteralTree(fn);
        if (fnTreeD->hasVariableRefs() && exprTreeHasOnlyGroupInputVars(fnTreeD)) {
            ExprNodePtr fnDiv4 = ExprNode::makeBinary(ExprNode::Op::DIVIDE, fnTreeD, ExprNode::makeLiteral(4.0));
            auto fnResult = emitExpressionNodeTree(fnDiv4);
            std::string maxId = newNodeId();
            emit(maxId + " = nodes.new('ShaderNodeMath')");
            emit(maxId + ".operation = 'MAXIMUM'");
            emit(maxId + ".location = (x_pos, y_pos)");
            connectExprResult(fnResult, maxId, 0);
            emit(maxId + ".inputs[1].default_value = 1");
            emit("links.new(" + maxId + ".outputs['Value'], " + subdivId + ".inputs['Cuts'])");
        } else if (in_module_ && fnTreeD->hasVariableRefs() && exprTreeReferencesModuleParams(fnTreeD)) {
            std::string fnPy = exprTreeToPython(fnTreeD);
            emit(subdivId + ".inputs['Cuts'].default_value = max(1, int(" + fnPy + ") // 4)");
        } else {
            int cuts = fnVal / 4;
            if (cuts < 1) cuts = 1;
            emit(subdivId + ".inputs['Cuts'].default_value = " + std::to_string(cuts));
        }
        emit("link_nodes(links, last_geo, 'Curve', " + subdivId + ", 'Curve')");
        emit("last_geo = " + subdivId);
        emit("x_pos += 200");
    }

    // Offset each point perpendicular to the curve tangent.
    // For a 2D curve in the XY plane, cross(Tangent, Z) gives the outward
    // perpendicular direction. Scaling by the offset amount moves each point
    // outward (positive) or inward (negative).
    std::string tangentId = newNodeId();
    emit(tangentId + " = nodes.new('GeometryNodeInputTangent')");
    emit(tangentId + ".location = (x_pos, y_pos)");

    std::string crossId = newNodeId();
    emit(crossId + " = nodes.new('ShaderNodeVectorMath')");
    emit(crossId + ".operation = 'CROSS_PRODUCT'");
    emit(crossId + ".location = (x_pos, y_pos)");
    emit("links.new(" + tangentId + ".outputs['Tangent'], " + crossId + ".inputs[0])");

    std::string zAxisId = newNodeId();
    emit(zAxisId + " = nodes.new('ShaderNodeCombineXYZ')");
    emit(zAxisId + ".location = (x_pos, y_pos)");
    emit(zAxisId + ".inputs['X'].default_value = 0");
    emit(zAxisId + ".inputs['Y'].default_value = 0");
    emit(zAxisId + ".inputs['Z'].default_value = 1");
    emit("links.new(" + zAxisId + ".outputs['Vector'], " + crossId + ".inputs[1])");

    std::string scaleId = newNodeId();
    emit(scaleId + " = nodes.new('ShaderNodeVectorMath')");
    emit(scaleId + ".operation = 'SCALE'");
    emit(scaleId + ".location = (x_pos, y_pos)");
    emit("links.new(" + crossId + ".outputs['Vector'], " + scaleId + ".inputs[0])");
    emitSetInputOrLink(scaleId, "Scale", offsetVal, offsetPython);

    std::string setposId = newNodeId();
    emit(setposId + " = nodes.new('GeometryNodeSetPosition')");
    emit(setposId + ".location = (x_pos, y_pos)");
    emit("link_nodes(links, last_geo, 'Geometry', " + setposId + ", 'Geometry')");
    emit("links.new(" + scaleId + ".outputs['Vector'], " + setposId + ".inputs['Offset'])");
    emit("last_geo = " + setposId);
    emit("x_pos += 200");
}

void BlenderGenerator::emitHull(const Arguments& args) {
    std::string nodeId = newNodeId();

    emit("# Hull (convex hull)");
    emit(nodeId + " = nodes.new('GeometryNodeConvexHull')");
    emit(nodeId + ".location = (x_pos, y_pos)");
    emit("link_nodes(links, last_geo, 'Geometry', " + nodeId + ", 'Geometry')");
    emit("last_geo = " + nodeId);
    emit("x_pos += 200");
}

void BlenderGenerator::emitMinkowski(const Arguments& args) {
    // Minkowski sum is not directly available in Blender geometry nodes
    // We'll just pass through the geometry with a comment
    emit("# Minkowski sum (not directly supported - passing geometry through)");
    emit("# To implement Minkowski, you would need custom node setup or scripting");
}

void BlenderGenerator::emitRoof(const Arguments& args) {
    // OpenSCAD roof() creates a 3D shape from a 2D profile where each vertex's
    // height equals its distance to the nearest boundary edge (straight skeleton).
    // Approximation: fill curve → subdivide → compute per-vertex distance to boundary → set Z.
    //
    // Child geometry may be disconnected curve segments (e.g. polygon made of
    // CurvePrimitiveLine nodes).  We merge endpoints into a proper edge loop
    // via CurveToMesh → MergeByDistance → MeshToCurve before filling.

    emit("# Roof (straight skeleton approximation)");

    // Convert disconnected curve segments to a mesh edge loop and merge endpoints
    std::string ctmId = newNodeId();
    emit(ctmId + " = nodes.new('GeometryNodeCurveToMesh')");
    emit(ctmId + ".location = (x_pos, y_pos)");
    emit("link_nodes(links, last_geo, 'Curve', " + ctmId + ", 'Curve')");
    emit("x_pos += 200");

    std::string mergeId = newNodeId();
    emit(mergeId + " = nodes.new('GeometryNodeMergeByDistance')");
    emit(mergeId + ".location = (x_pos, y_pos)");
    emit(mergeId + ".inputs['Distance'].default_value = 0.001");
    emit("links.new(" + ctmId + ".outputs['Mesh'], " + mergeId + ".inputs['Geometry'])");
    emit("x_pos += 200");

    // Convert the merged edge loop back to a curve for FillCurve
    std::string mtcId = newNodeId();
    emit(mtcId + " = nodes.new('GeometryNodeMeshToCurve')");
    emit(mtcId + ".location = (x_pos, y_pos)");
    emit("links.new(" + mergeId + ".outputs['Geometry'], " + mtcId + ".inputs['Mesh'])");
    emit("x_pos += 200");

    // Fill the curve to create a mesh face
    std::string fillId = newNodeId();
    emit(fillId + " = nodes.new('GeometryNodeFillCurve')");
    emit(fillId + ".location = (x_pos, y_pos)");
    emit("links.new(" + mtcId + ".outputs['Curve'], " + fillId + ".inputs['Curve'])");
    emit("x_pos += 200");

    // Subdivide the filled mesh to create interior vertices for the distance field
    std::string subdivId = newNodeId();
    emit(subdivId + " = nodes.new('GeometryNodeSubdivideMesh')");
    emit(subdivId + ".location = (x_pos, y_pos)");
    emit(subdivId + ".inputs['Level'].default_value = 4");
    emit("links.new(" + fillId + ".outputs['Mesh'], " + subdivId + ".inputs['Mesh'])");
    emit("x_pos += 200");

    // Compute distance from each vertex of the filled mesh to the boundary edges
    std::string proxId = newNodeId();
    emit(proxId + " = nodes.new('GeometryNodeProximity')");
    emit(proxId + ".location = (x_pos, y_pos)");
    emit(proxId + ".target_element = 'EDGES'");
    emit("links.new(" + mergeId + ".outputs['Geometry'], " + proxId + ".inputs['Target'])");

    // Build a Z-offset vector: (0, 0, distance)
    std::string combineId = newNodeId();
    emit(combineId + " = nodes.new('ShaderNodeCombineXYZ')");
    emit(combineId + ".location = (x_pos, y_pos - 150)");
    emit(combineId + ".inputs['X'].default_value = 0.0");
    emit(combineId + ".inputs['Y'].default_value = 0.0");
    emit("links.new(" + proxId + ".outputs['Distance'], " + combineId + ".inputs['Z'])");

    // Set position: offset the filled mesh vertices upward by the distance
    std::string setPosId = newNodeId();
    emit(setPosId + " = nodes.new('GeometryNodeSetPosition')");
    emit(setPosId + ".location = (x_pos, y_pos)");
    emit("links.new(" + subdivId + ".outputs['Mesh'], " + setPosId + ".inputs['Geometry'])");
    emit("links.new(" + combineId + ".outputs['Vector'], " + setPosId + ".inputs['Offset'])");
    emit("x_pos += 200");

    emit("last_geo = " + setPosId);
    emit("x_pos += 200");
}

// Boolean operation
// Helper: determine if an AST node produces 2D curve geometry
static bool is2DGeometry(const ASTNodePtr& node) {
    if (!node) return false;
    switch (node->type()) {
        case ASTNode::Type::Circle:
        case ASTNode::Type::Square:
        case ASTNode::Type::Polygon:
        case ASTNode::Type::Text:
        case ASTNode::Type::Offset:
        case ASTNode::Type::Projection:
            return true;
        case ASTNode::Type::Translate:
        case ASTNode::Type::Rotate:
        case ASTNode::Type::Scale:
        case ASTNode::Type::Mirror:
        case ASTNode::Type::Color:
        case ASTNode::Type::Resize:
        case ASTNode::Type::Multmatrix:
            if (!node->children().empty())
                return is2DGeometry(node->children()[0]);
            return false;
        case ASTNode::Type::Union:
        case ASTNode::Type::Difference:
        case ASTNode::Type::Intersection:
        case ASTNode::Type::Hull:
        case ASTNode::Type::Minkowski:
            if (!node->children().empty())
                return is2DGeometry(node->children()[0]);
            return false;
        default:
            return false;
    }
}

void BlenderGenerator::emitBooleanOp(BooleanNode& node) {
    if (node.children().empty()) {
        return;
    }

    std::string opName;
    std::string blenderOp;

    switch (node.type()) {
        case ASTNode::Type::Union:
            opName = "Union";
            blenderOp = "UNION";
            break;
        case ASTNode::Type::Difference:
            opName = "Difference";
            blenderOp = "DIFFERENCE";
            break;
        case ASTNode::Type::Intersection:
            opName = "Intersection";
            blenderOp = "INTERSECT";
            break;
        default:
            return;
    }

    // Collect all actual children (unpacking container unions)
    // Separate assignments and module definitions from geometry-producing nodes
    std::vector<ASTNodePtr> actualChildren;
    std::vector<ASTNodePtr> assignments;
    std::vector<ASTNodePtr> moduleDefinitions;

    for (auto& child : node.children()) {
        if (!child) continue;
        if (child->isDisabled()) continue;  // Skip disabled children (* modifier)
        // Check if this is a container Union (marked with debug flag)
        if (child->type() == ASTNode::Type::Union && child->isDebug()) {
            // Unpack the container's children
            for (auto& grandchild : child->children()) {
                if (!grandchild || grandchild->isDisabled()) continue;
                if (grandchild->type() == ASTNode::Type::Assignment) {
                    assignments.push_back(grandchild);
                } else if (grandchild->type() == ASTNode::Type::Module) {
                    // Nested module definition - doesn't produce geometry
                    moduleDefinitions.push_back(grandchild);
                } else if (grandchild->type() == ASTNode::Type::FunctionDef) {
                    // Function definition - doesn't produce geometry
                } else {
                    actualChildren.push_back(grandchild);
                }
            }
        } else if (child->type() == ASTNode::Type::Assignment) {
            assignments.push_back(child);
        } else if (child->type() == ASTNode::Type::Module) {
            // Nested module definition - doesn't produce geometry
            moduleDefinitions.push_back(child);
        } else if (child->type() == ASTNode::Type::FunctionDef) {
            // Function definition - doesn't produce geometry
        } else {
            actualChildren.push_back(child);
        }
    }

    // Process assignments first (they define variables)
    for (size_t ai = 0; ai < assignments.size(); ai++) {
        assignments[ai]->accept(*this);
    }

    // Process nested module definitions (register them but don't generate geometry)
    for (auto& mod : moduleDefinitions) {
        // Just skip nested modules for now - they're not supported yet
        // In a full implementation, we would register them in a local scope
    }

    // Only emit boolean op comment if we have geometry children
    if (actualChildren.empty()) {
        return;
    }

    // If only one child, just process it without boolean operation
    if (actualChildren.size() == 1) {
        actualChildren[0]->accept(*this);
        return;
    }

    // Inside hull(), use JoinGeometry instead of boolean operations
    // ConvexHull only needs point data — boolean UNION is unnecessary and can produce non-manifold artifacts
    if (in_hull_ && blenderOp == "UNION") {
        std::string joinId = newNodeId();
        emit("# Union (JoinGeometry for hull)");
        emit(joinId + " = nodes.new('GeometryNodeJoinGeometry')");
        emit(joinId + ".location = (x_pos, y_pos)");

        for (auto& child : actualChildren) {
            child->accept(*this);
            emit("if last_geo is not None:");
            indent_++;
            emit("link_nodes(links, last_geo, 'Geometry', " + joinId + ", 'Geometry')");
            indent_--;
        }

        emit("last_geo = " + joinId);
        return;
    }

    // Use a unique counter for this boolean scope to avoid variable name collisions
    int boolScopeId = node_counter_++;

    emit("# " + opName);

    std::string firstGeo = "bool_first_" + std::to_string(boolScopeId);
    emit(firstGeo + " = None");

    if (blenderOp == "DIFFERENCE") {
        // Determine at compile time if this is a 2D or 3D difference
        bool is2D = is2DGeometry(actualChildren[0]);

        if (is2D) {
            // 2D path: use curve reverse + join approach (keeps curves as curves)
            std::string isCurveHelper = "is_curve_" + std::to_string(boolScopeId);
            emit("def " + isCurveHelper + "(geo_node):");
            indent_++;
            emit("if geo_node is None: return False");
            emit("curve_types = {'GeometryNodeCurvePrimitiveCircle', 'GeometryNodeCurvePrimitiveQuadrilateral',");
            emit("               'GeometryNodeCurvePrimitiveLine', 'GeometryNodeCurvePrimitiveStar',");
            emit("               'GeometryNodeFilletCurve', 'GeometryNodeSetPosition',");
            emit("               'GeometryNodeStringToCurves', 'GeometryNodeCurveToPoints',");
            emit("               'GeometryNodeCurvePrimitiveArc', 'GeometryNodeCurvePrimitiveBezierSegment',");
            emit("               'GeometryNodeSubdivideCurve', 'GeometryNodeReverseCurve',");
            emit("               'GeometryNodeJoinGeometry'}");
            emit("return geo_node.bl_idname in curve_types");
            indent_--;

            // Process first child normally
            actualChildren[0]->accept(*this);
            emit(firstGeo + " = last_geo");

            for (size_t i = 1; i < actualChildren.size(); ++i) {
                actualChildren[i]->accept(*this);

                emit("if last_geo is not None and last_geo is not " + firstGeo + ":");
                indent_++;
                emit("if " + firstGeo + " is None:");
                indent_++;
                emit(firstGeo + " = last_geo");
                indent_--;
                emit("else:");
                indent_++;
                // Reverse the subtracted curve so FillCurve treats it as a hole
                std::string revId = newNodeId();
                emit(revId + " = nodes.new('GeometryNodeReverseCurve')");
                emit(revId + ".location = (x_pos, y_pos)");
                emit("link_nodes(links, last_geo, 'Curve', " + revId + ", 'Curve')");
                std::string joinId = newNodeId();
                emit(joinId + " = nodes.new('GeometryNodeJoinGeometry')");
                emit(joinId + ".location = (x_pos, y_pos)");
                emit("link_nodes(links, " + firstGeo + ", 'Curve', " + joinId + ", 'Geometry')");
                emit("link_nodes(links, " + revId + ", 'Curve', " + joinId + ", 'Geometry')");
                emit(firstGeo + " = " + joinId);
                emit("x_pos += 200");
                indent_--;
                indent_--;
                emit("y_pos -= 50");
            }
        } else {
            // 3D path: use in-graph GeometryNodeMeshBoolean with DIFFERENCE operation.
            // Chain one boolean per tool: (((base - tool1) - tool2) - tool3)

            // Helper: check if a node outputs curve geometry (2D)
            // Note: JoinGeometry is excluded because it can contain mesh data
            // (e.g., for-loop collecting boolean results)
            std::string isCurveHelper = "is_curve_" + std::to_string(boolScopeId);
            emit("def " + isCurveHelper + "(geo_node):");
            indent_++;
            emit("if geo_node is None: return False");
            emit("curve_types = {'GeometryNodeCurvePrimitiveCircle', 'GeometryNodeCurvePrimitiveQuadrilateral',");
            emit("               'GeometryNodeCurvePrimitiveLine', 'GeometryNodeCurvePrimitiveStar',");
            emit("               'GeometryNodeFilletCurve', 'GeometryNodeSetPosition',");
            emit("               'GeometryNodeStringToCurves', 'GeometryNodeCurveToPoints',");
            emit("               'GeometryNodeCurvePrimitiveArc', 'GeometryNodeCurvePrimitiveBezierSegment',");
            emit("               'GeometryNodeSubdivideCurve', 'GeometryNodeReverseCurve'}");
            emit("return geo_node.bl_idname in curve_types");
            indent_--;

            // Helper: ensure geometry is mesh (fill curves for 3D boolean operands)
            std::string fillHelper = "ensure_mesh_" + std::to_string(boolScopeId);
            emit("def " + fillHelper + "(geo_node):");
            indent_++;
            emit("\"\"\"Fill curve geometry for boolean input\"\"\"");
            emit("nonlocal x_pos, y_pos");
            emit("if " + isCurveHelper + "(geo_node):");
            indent_++;
            emit("fill = nodes.new('GeometryNodeFillCurve')");
            emit("fill.location = (x_pos, y_pos)");
            emit("link_nodes(links, geo_node, 'Curve', fill, 'Curve')");
            emit("x_pos += 200");
            emit("return fill");
            indent_--;
            emit("return geo_node");
            indent_--;

            // Process first child as base
            actualChildren[0]->accept(*this);

            emit("last_geo = " + fillHelper + "(last_geo)");
            emit(firstGeo + " = last_geo");

            for (size_t i = 1; i < actualChildren.size(); ++i) {
                actualChildren[i]->accept(*this);

                // Skip boolean if child produced no geometry
                emit("if last_geo is not None and last_geo is not " + firstGeo + ":");
                indent_++;
                emit("last_geo = " + fillHelper + "(last_geo)");

                // If first geo is None, adopt this child
                emit("if " + firstGeo + " is None:");
                indent_++;
                emit(firstGeo + " = last_geo");
                indent_--;
                emit("else:");
                indent_++;

                // If the tool is a JoinGeometry (e.g., from a for-loop), merge
                // overlapping parts with UNION first to avoid EXACT solver failures.
                std::string unionedTool = "last_geo";
                std::string unionId = newNodeId();
                emit("if last_geo.bl_idname == 'GeometryNodeJoinGeometry':");
                indent_++;
                emit(unionId + " = nodes.new('GeometryNodeMeshBoolean')");
                emit(unionId + ".location = (x_pos, y_pos)");
                emit(unionId + ".operation = 'UNION'");
                emit(unionId + ".solver = 'EXACT'");
                emit("links.new(geo_out(last_geo), " + unionId + ".inputs[1])");
                emit("last_geo = " + unionId);
                emit("x_pos += 200");
                indent_--;

                std::string boolId = newNodeId();
                emit(boolId + " = nodes.new('GeometryNodeMeshBoolean')");
                emit(boolId + ".location = (x_pos, y_pos)");
                emit(boolId + ".operation = 'DIFFERENCE'");
                emit(boolId + ".solver = 'EXACT'");

                // DIFFERENCE: base -> inputs[0], tool -> inputs[1]
                emit("links.new(geo_out(" + firstGeo + "), " + boolId + ".inputs[0])");
                emit("links.new(geo_out(last_geo), " + boolId + ".inputs[1])");
                emit(firstGeo + " = " + boolId);
                emit("x_pos += 200");
                emit("y_pos -= 50");
                indent_--;  // end else
                indent_--;  // end if last_geo is not None
            }
        }  // end 3D DIFFERENCE path

        // Clean up near-coincident vertices from boolean operations
        // EXACT solver can produce nearly-duplicate vertices that cause non-manifold edges
        std::string mergeId = newNodeId();
        emit("# MergeByDistance to clean up boolean artifacts");
        emit(mergeId + " = nodes.new('GeometryNodeMergeByDistance')");
        emit(mergeId + ".location = (x_pos, y_pos)");
        emit(mergeId + ".inputs['Distance'].default_value = 0.0001");
        emit("if " + firstGeo + " is not None:");
        indent_++;
        emit("links.new(geo_out(" + firstGeo + "), " + mergeId + ".inputs['Geometry'])");
        emit(firstGeo + " = " + mergeId);
        indent_--;
        emit("x_pos += 200");
    } else {
        // UNION and INTERSECT: use geometry nodes mesh boolean

        // Helper: check if a node outputs curve geometry (2D)
        // Note: JoinGeometry is excluded because it can contain mesh data
        std::string isCurveHelper = "is_curve_" + std::to_string(boolScopeId);
        emit("def " + isCurveHelper + "(geo_node):");
        indent_++;
        emit("if geo_node is None: return False");
        emit("curve_types = {'GeometryNodeCurvePrimitiveCircle', 'GeometryNodeCurvePrimitiveQuadrilateral',");
        emit("               'GeometryNodeCurvePrimitiveLine', 'GeometryNodeCurvePrimitiveStar',");
        emit("               'GeometryNodeFilletCurve', 'GeometryNodeSetPosition',");
        emit("               'GeometryNodeStringToCurves', 'GeometryNodeCurveToPoints',");
        emit("               'GeometryNodeCurvePrimitiveArc', 'GeometryNodeCurvePrimitiveBezierSegment',");
        emit("               'GeometryNodeSubdivideCurve', 'GeometryNodeReverseCurve'}");
        emit("return geo_node.bl_idname in curve_types");
        indent_--;

        // Helper: ensure geometry is mesh (fill curves for 3D boolean operands)
        std::string fillHelper = "ensure_mesh_" + std::to_string(boolScopeId);
        emit("def " + fillHelper + "(geo_node):");
        indent_++;
        emit("\"\"\"Fill curve geometry for boolean input\"\"\"");
        emit("nonlocal x_pos, y_pos");
        emit("if " + isCurveHelper + "(geo_node):");
        indent_++;
        emit("fill = nodes.new('GeometryNodeFillCurve')");
        emit("fill.location = (x_pos, y_pos)");
        emit("link_nodes(links, geo_node, 'Curve', fill, 'Curve')");
        emit("x_pos += 200");
        emit("return fill");
        indent_--;
        emit("return geo_node");
        indent_--;

        // Process first child
        actualChildren[0]->accept(*this);

        emit("last_geo = " + fillHelper + "(last_geo)");
        emit(firstGeo + " = last_geo");

        for (size_t i = 1; i < actualChildren.size(); ++i) {
            actualChildren[i]->accept(*this);

            // Skip boolean if child produced no geometry (e.g. false conditional)
            emit("if last_geo is not None and last_geo is not " + firstGeo + ":");
            indent_++;
            emit("last_geo = " + fillHelper + "(last_geo)");

            // If first geo is None (e.g. conditional produced nothing), adopt this child
            emit("if " + firstGeo + " is None:");
            indent_++;
            emit(firstGeo + " = last_geo");
            indent_--;
            emit("else:");
            indent_++;

            std::string boolId = newNodeId();
            emit(boolId + " = nodes.new('GeometryNodeMeshBoolean')");
            emit(boolId + ".location = (x_pos, y_pos)");
            emit(boolId + ".operation = '" + blenderOp + "'");
            emit(boolId + ".solver = 'EXACT'");

            // In Blender 5.1+, UNION/INTERSECT use inputs[1] as multi-input
            // (inputs[0] is disabled). Both operands go to inputs[1].
            emit("links.new(geo_out(" + firstGeo + "), " + boolId + ".inputs[1])");
            emit("links.new(geo_out(last_geo), " + boolId + ".inputs[1])");
            emit(firstGeo + " = " + boolId);
            emit("x_pos += 200");
            emit("y_pos -= 50");
            indent_--;  // end else
            indent_--;  // end if last_geo is not None
        }
    }

    emit("last_geo = " + firstGeo);
}

// Extrude generators
void BlenderGenerator::emitLinearExtrude(const Arguments& args) {
    Value height = getArg(args, "height", getPositionalArg(args, 0, Value(1.0)));
    Value twist = getArg(args, "twist", Value(0.0));
    Value scale_val = getArg(args, "scale", Value(1.0));
    Value center = getArg(args, "center", Value(false));
    Value fn = resolveFn(args);

    double hVal = height.isExpression() ? evaluateExpr(height) : height.toNumber();
    double twistVal = twist.isExpression() ? evaluateExpr(twist) : twist.toNumber();
    double scaleNum = scale_val.isExpression() ? evaluateExpr(scale_val) : scale_val.toNumber();
    bool hasTwist = std::abs(twistVal) > 0.001;
    bool hasScale = std::abs(scaleNum - 1.0) > 0.001;

    ExprNodePtr heightTree = getOrMakeLiteralTree(height);

    // Save the 2D profile curve for use as the cross-section
    std::string profileGeo = "last_geo";

    if (!hasTwist && !hasScale) {
        // CurveToMesh (no caps) + FillCurve caps + MergeByDistance
        // Handles compound curves (2D booleans with holes) with correct normals
        emit("# Linear Extrude (CurveToMesh + capped approach)");

        // Create a straight line path from Z=0 to Z=height
        std::string lineId = newNodeId();
        emit(lineId + " = nodes.new('GeometryNodeCurvePrimitiveLine')");
        emit(lineId + ".location = (x_pos, y_pos - 200)");
        emit(lineId + ".mode = 'POINTS'");
        emit(lineId + ".inputs['Start'].default_value = (0, 0, 0)");

        // Height end-point (variable-aware)
        std::string heightEndExpr;
        if (heightTree->hasVariableRefs() && exprTreeHasOnlyGroupInputVars(heightTree)) {
            std::string combId = newNodeId();
            emit(combId + " = nodes.new('ShaderNodeCombineXYZ')");
            emit(combId + ".location = (x_pos, y_pos - 350)");
            auto result = emitExpressionNodeTree(heightTree);
            connectExprResultNamed(result, combId, "Z");
            emit("links.new(" + combId + ".outputs['Vector'], " + lineId + ".inputs['End'])");
        } else if (in_module_ && heightTree->hasVariableRefs() && exprTreeReferencesModuleParams(heightTree)) {
            std::string hPy = exprTreeToPython(heightTree);
            emit(lineId + ".inputs['End'].default_value = (0, 0, " + hPy + ")");
            heightEndExpr = hPy;
        } else {
            emit(lineId + ".inputs['End'].default_value = (0, 0, " + pyDouble(hVal) + ")");
            heightEndExpr = pyDouble(hVal);
        }
        emit("x_pos += 200");

        // CurveToMesh with Fill Caps = True (handles simple and compound curves)
        std::string ctmId = newNodeId();
        emit(ctmId + " = nodes.new('GeometryNodeCurveToMesh')");
        emit(ctmId + ".location = (x_pos, y_pos)");
        emit("links.new(" + lineId + ".outputs['Curve'], " + ctmId + ".inputs['Curve'])");
        emit("link_nodes(links, " + profileGeo + ", 'Curve', " + ctmId + ", 'Profile Curve')");
        emit(ctmId + ".inputs['Fill Caps'].default_value = True");

        emit("last_geo = " + ctmId);
        emit("x_pos += 200");
    } else {
        // CurveToMesh approach — supports twist and scale (only works for simple single-spline profiles)
        emit("# Linear Extrude (CurveToMesh approach)");

        // Create a straight line path from Z=0 to Z=height
        std::string lineId = newNodeId();
        emit(lineId + " = nodes.new('GeometryNodeCurvePrimitiveLine')");
        emit(lineId + ".location = (x_pos, y_pos - 200)");
        emit(lineId + ".mode = 'POINTS'");
        emit(lineId + ".inputs['Start'].default_value = (0, 0, 0)");
        if (heightTree->hasVariableRefs() && exprTreeHasOnlyGroupInputVars(heightTree)) {
            // Height linked from group_input — build CombineXYZ for End point
            std::string combId = newNodeId();
            emit(combId + " = nodes.new('ShaderNodeCombineXYZ')");
            emit(combId + ".location = (x_pos, y_pos - 350)");
            auto result = emitExpressionNodeTree(heightTree);
            connectExprResultNamed(result, combId, "Z");
            emit("links.new(" + combId + ".outputs['Vector'], " + lineId + ".inputs['End'])");
        } else if (in_module_ && heightTree->hasVariableRefs() && exprTreeReferencesModuleParams(heightTree)) {
            std::string hPy = exprTreeToPython(heightTree);
            emit(lineId + ".inputs['End'].default_value = (0, 0, " + hPy + ")");
        } else {
            emit(lineId + ".inputs['End'].default_value = (0, 0, " + pyDouble(hVal) + ")");
        }
        emit("x_pos += 200");

        // Track the current path curve node/output
        std::string pathCurve = lineId;
        std::string pathOutput = "Curve";

        // Subdivide the line for smooth twist/scale interpolation
        {
            int fnVal = static_cast<int>(evaluateExpr(fn));
            int segments = std::max(fnVal, static_cast<int>(std::abs(twistVal) / 5.0));
            if (segments < 8) segments = 8;

            std::string subdivId = newNodeId();
            emit(subdivId + " = nodes.new('GeometryNodeSubdivideCurve')");
            emit(subdivId + ".location = (x_pos, y_pos - 200)");
            emit(subdivId + ".inputs['Cuts'].default_value = " + std::to_string(segments));
            emit("links.new(" + pathCurve + ".outputs['" + pathOutput + "'], " + subdivId + ".inputs['Curve'])");
            pathCurve = subdivId;
            pathOutput = "Curve";
            emit("x_pos += 200");
        }

        // Apply twist using SetCurveTilt with SplineParameter
        if (hasTwist) {
            // SplineParameter gives 0..1 along the spline
            std::string paramId = newNodeId();
            emit(paramId + " = nodes.new('GeometryNodeSplineParameter')");
            emit(paramId + ".location = (x_pos, y_pos - 350)");

            // Multiply factor by twist angle in radians
            double twistRad = twistVal * M_PI / 180.0;
            std::string mulId = newNodeId();
            emit(mulId + " = nodes.new('ShaderNodeMath')");
            emit(mulId + ".operation = 'MULTIPLY'");
            emit(mulId + ".location = (x_pos, y_pos - 350)");
            emit("links.new(" + paramId + ".outputs['Factor'], " + mulId + ".inputs[0])");
            emit(mulId + ".inputs[1].default_value = " + pyDouble(twistRad));

            // Set the tilt on the path curve
            std::string tiltId = newNodeId();
            emit(tiltId + " = nodes.new('GeometryNodeSetCurveTilt')");
            emit(tiltId + ".location = (x_pos, y_pos - 200)");
            emit("links.new(" + pathCurve + ".outputs['" + pathOutput + "'], " + tiltId + ".inputs['Curve'])");
            emit("links.new(" + mulId + ".outputs['Value'], " + tiltId + ".inputs['Tilt'])");
            pathCurve = tiltId;
            pathOutput = "Curve";
            emit("x_pos += 200");
        }

        // CurveToMesh: sweep the 2D profile along the line path
        std::string ctmId = newNodeId();
        emit(ctmId + " = nodes.new('GeometryNodeCurveToMesh')");
        emit(ctmId + ".location = (x_pos, y_pos)");
        emit("links.new(" + pathCurve + ".outputs['" + pathOutput + "'], " + ctmId + ".inputs['Curve'])");
        emit("link_nodes(links, " + profileGeo + ", 'Curve', " + ctmId + ", 'Profile Curve')");
        emit(ctmId + ".inputs['Fill Caps'].default_value = True");
        emit("last_geo = " + ctmId);
        emit("x_pos += 200");

        // Apply scale after CurveToMesh using SetPosition
        // scaleFactor = 1 + (Z / height) * (targetScale - 1)
        // New position: (X * scaleFactor, Y * scaleFactor, Z)
        if (hasScale) {
            emit("# Scale extrusion: taper XY based on Z position");

            // Position → SeparateXYZ
            std::string posId = newNodeId();
            emit(posId + " = nodes.new('GeometryNodeInputPosition')");
            emit(posId + ".location = (x_pos, y_pos - 400)");

            std::string sepId = newNodeId();
            emit(sepId + " = nodes.new('ShaderNodeSeparateXYZ')");
            emit(sepId + ".location = (x_pos + 150, y_pos - 400)");
            emit("links.new(" + posId + ".outputs['Position'], " + sepId + ".inputs['Vector'])");

            // Z / height → factor along extrusion (0 at base, 1 at top)
            std::string divId = newNodeId();
            emit(divId + " = nodes.new('ShaderNodeMath')");
            emit(divId + ".operation = 'DIVIDE'");
            emit(divId + ".location = (x_pos + 300, y_pos - 400)");
            emit("links.new(" + sepId + ".outputs['Z'], " + divId + ".inputs[0])");

            // Use the height value for division
            if (heightTree->hasVariableRefs() && exprTreeHasOnlyGroupInputVars(heightTree)) {
                auto hResult = emitExpressionNodeTree(heightTree);
                if (hResult.nodeId == "__literal__") {
                    emit(divId + ".inputs[1].default_value = " + pyDouble(hResult.literalValue));
                } else {
                    emit("links.new(" + hResult.nodeId + ".outputs['" + hResult.socketName + "'], " +
                         divId + ".inputs[1])");
                }
            } else if (in_module_ && heightTree->hasVariableRefs() && exprTreeReferencesModuleParams(heightTree)) {
                std::string hPy = exprTreeToPython(heightTree);
                emit(divId + ".inputs[1].default_value = " + hPy);
            } else {
                emit(divId + ".inputs[1].default_value = " + pyDouble(hVal));
            }

            // factor * (scale - 1)
            double scaleMinusOne = scaleNum - 1.0;
            std::string mulFactorId = newNodeId();
            emit(mulFactorId + " = nodes.new('ShaderNodeMath')");
            emit(mulFactorId + ".operation = 'MULTIPLY'");
            emit(mulFactorId + ".location = (x_pos + 450, y_pos - 400)");
            emit("links.new(" + divId + ".outputs['Value'], " + mulFactorId + ".inputs[0])");
            emit(mulFactorId + ".inputs[1].default_value = " + pyDouble(scaleMinusOne));

            // + 1 → scaleFactor
            std::string addOneId = newNodeId();
            emit(addOneId + " = nodes.new('ShaderNodeMath')");
            emit(addOneId + ".operation = 'ADD'");
            emit(addOneId + ".location = (x_pos + 600, y_pos - 400)");
            emit("links.new(" + mulFactorId + ".outputs['Value'], " + addOneId + ".inputs[0])");
            emit(addOneId + ".inputs[1].default_value = 1.0");

            // X * scaleFactor
            std::string mulXId = newNodeId();
            emit(mulXId + " = nodes.new('ShaderNodeMath')");
            emit(mulXId + ".operation = 'MULTIPLY'");
            emit(mulXId + ".location = (x_pos + 750, y_pos - 350)");
            emit("links.new(" + sepId + ".outputs['X'], " + mulXId + ".inputs[0])");
            emit("links.new(" + addOneId + ".outputs['Value'], " + mulXId + ".inputs[1])");

            // Y * scaleFactor
            std::string mulYId = newNodeId();
            emit(mulYId + " = nodes.new('ShaderNodeMath')");
            emit(mulYId + ".operation = 'MULTIPLY'");
            emit(mulYId + ".location = (x_pos + 750, y_pos - 450)");
            emit("links.new(" + sepId + ".outputs['Y'], " + mulYId + ".inputs[0])");
            emit("links.new(" + addOneId + ".outputs['Value'], " + mulYId + ".inputs[1])");

            // CombineXYZ(scaled_x, scaled_y, original_z) → SetPosition
            std::string combineId = newNodeId();
            emit(combineId + " = nodes.new('ShaderNodeCombineXYZ')");
            emit(combineId + ".location = (x_pos + 900, y_pos - 400)");
            emit("links.new(" + mulXId + ".outputs['Value'], " + combineId + ".inputs['X'])");
            emit("links.new(" + mulYId + ".outputs['Value'], " + combineId + ".inputs['Y'])");
            emit("links.new(" + sepId + ".outputs['Z'], " + combineId + ".inputs['Z'])");

            std::string setposId = newNodeId();
            emit(setposId + " = nodes.new('GeometryNodeSetPosition')");
            emit(setposId + ".location = (x_pos + 1050, y_pos)");
            emit("link_nodes(links, last_geo, 'Geometry', " + setposId + ", 'Geometry')");
            emit("links.new(" + combineId + ".outputs['Vector'], " + setposId + ".inputs['Position'])");
            emit("last_geo = " + setposId);
            emit("x_pos += 1200");
        }
    }

    // Handle center=true — translate by -height/2 in Z
    bool centerVal = center.toBool() || isSimpleVariableRef(center);
    if (centerVal) {
        emitBlank();
        emit("# Translate for center=true");
        std::string transId = newNodeId();
        emit(transId + " = nodes.new('GeometryNodeTransform')");
        emit(transId + ".location = (x_pos, y_pos)");

        if (heightTree->hasVariableRefs() && exprTreeHasOnlyGroupInputVars(heightTree)) {
            ExprNodePtr negHalfH = ExprNode::makeBinary(
                ExprNode::Op::DIVIDE,
                ExprNode::makeUnary(ExprNode::Op::NEGATE, heightTree),
                ExprNode::makeLiteral(2.0));
            emitScalarToVectorInput(transId, "Translation", negHalfH, 2, 0.0, 0.0, 0.0);
        } else if (in_module_ && heightTree->hasVariableRefs() && exprTreeReferencesModuleParams(heightTree)) {
            std::string hPy = exprTreeToPython(heightTree);
            emit(transId + ".inputs['Translation'].default_value = (0, 0, -(" + hPy + ") / 2)");
        } else {
            emit(transId + ".inputs['Translation'].default_value = (0, 0, " +
                 pyDouble(-hVal / 2.0) + ")");
        }

        emit("link_nodes(links, last_geo, 'Mesh', " + transId + ", 'Geometry')");
        emit("last_geo = " + transId);
        emit("x_pos += 200");
    }
}

void BlenderGenerator::emitRotateExtrude(const Arguments& args) {
    Value angle = getArg(args, "angle", Value(360.0));
    Value fn = resolveFn(args);

    double angleVal = angle.isExpression() ? evaluateExpr(angle) : angle.toNumber();
    int fnVal = static_cast<int>(evaluateExpr(fn));
    if (fnVal < 3) fnVal = 32;  // Sensible default

    emit("# Rotate Extrude (Lathe/Revolution)");

    // Flip the profile's Y axis so that OpenSCAD Y (height) maps to Blender +Z.
    // In CurveToMesh, profile Y maps to the curve's binormal which is -Z for
    // a circle in the XY plane, so we negate Y to compensate.
    std::string flipId = newNodeId();
    emit(flipId + " = nodes.new('GeometryNodeTransform')");
    emit(flipId + ".location = (x_pos, y_pos)");
    emit(flipId + ".inputs['Scale'].default_value = (1.0, -1.0, 1.0)");
    emit("link_nodes(links, last_geo, 'Geometry', " + flipId + ", 'Geometry')");
    emit("x_pos += 200");

    // Create a near-zero radius circle as the revolution path.
    // Profile X coordinates directly become the radial distance from the Z axis.
    std::string circleId = newNodeId();
    emit(circleId + " = nodes.new('GeometryNodeCurvePrimitiveCircle')");
    emit(circleId + ".location = (x_pos, y_pos - 200)");
    emit(circleId + ".mode = 'RADIUS'");
    emit(circleId + ".inputs['Radius'].default_value = 0.0001");
    emit(circleId + ".inputs['Resolution'].default_value = " + std::to_string(fnVal));

    // The sweep curve: full circle or trimmed arc
    std::string sweepCurve = circleId;

    if (std::abs(angleVal) < 360.0 - 0.001) {
        // Partial revolution — trim the circle to the desired arc
        double fraction = std::abs(angleVal) / 360.0;
        std::string trimId = newNodeId();
        emit(trimId + " = nodes.new('GeometryNodeTrimCurve')");
        emit(trimId + ".location = (x_pos, y_pos - 200)");
        emit(trimId + ".mode = 'FACTOR'");
        emit(trimId + ".inputs['Start'].default_value = 0.0");
        emit(trimId + ".inputs['End'].default_value = " + std::to_string(fraction));
        emit("links.new(" + circleId + ".outputs['Curve'], " + trimId + ".inputs['Curve'])");
        sweepCurve = trimId;
        emit("x_pos += 200");
    }

    // If the angle is negative, reverse the sweep direction
    if (angleVal < 0) {
        std::string reverseId = newNodeId();
        emit(reverseId + " = nodes.new('GeometryNodeReverseCurve')");
        emit(reverseId + ".location = (x_pos, y_pos - 200)");
        emit("links.new(" + sweepCurve + ".outputs['Curve'], " + reverseId + ".inputs['Curve'])");
        sweepCurve = reverseId;
        emit("x_pos += 200");
    }

    // CurveToMesh: sweep the profile along the revolution path
    std::string ctmId = newNodeId();
    emit(ctmId + " = nodes.new('GeometryNodeCurveToMesh')");
    emit(ctmId + ".location = (x_pos, y_pos)");
    emit("links.new(" + sweepCurve + ".outputs['Curve'], " + ctmId + ".inputs['Curve'])");
    emit("link_nodes(links, " + flipId + ", 'Geometry', " + ctmId + ", 'Profile Curve')");

    if (std::abs(angleVal) < 360.0 - 0.001) {
        // Cap the ends for partial revolutions
        emit(ctmId + ".inputs['Fill Caps'].default_value = True");
    }

    emit("last_geo = " + ctmId);
    emit("x_pos += 200");

    // Clean up degenerate faces from pole vertices (where profile has x=0,
    // creating zero-radius points on the revolution axis)
    std::string mergeId = newNodeId();
    emit("# MergeByDistance to clean pole vertices");
    emit(mergeId + " = nodes.new('GeometryNodeMergeByDistance')");
    emit(mergeId + ".location = (x_pos, y_pos)");
    emit("links.new(" + ctmId + ".outputs['Mesh'], " + mergeId + ".inputs['Geometry'])");
    emit(mergeId + ".inputs['Distance'].default_value = 0.0001");
    emit("last_geo = " + mergeId);
    emit("x_pos += 200");
}

void BlenderGenerator::processChildren(ASTNode& node) {
    for (auto& child : node.children()) {
        if (!child) continue;
        if (!child->isDisabled() && !child->isBackground()) {
            child->accept(*this);
        }
    }
}

std::string BlenderGenerator::vectorToPython(const Value& v) {
    if (v.isVector()) {
        std::ostringstream ss;
        ss << "(";
        for (size_t i = 0; i < v.size(); ++i) {
            if (i > 0) ss << ", ";
            if (v[i].isUndefined()) {
                ss << "0";
            } else if (v[i].isExpression() && v[i].exprTree()) {
                ss << exprTreeToPython(v[i].exprTree());
            } else if (v[i].exprTree() && v[i].exprTree()->hasVariableRefs()) {
                ss << exprTreeToPython(v[i].exprTree());
            } else {
                ss << v[i].toPython();
            }
        }
        ss << ")";
        return ss.str();
    }
    if (v.isUndefined()) return "0";
    return v.toPython();
}

bool BlenderGenerator::vectorHasExpressions(const Value& v) {
    if (!v.isVector()) return false;
    for (size_t i = 0; i < v.size(); ++i) {
        if (v[i].isExpression()) return true;
    }
    return false;
}

bool BlenderGenerator::vectorHasExprTrees(const Value& v) {
    if (!v.isVector()) return false;
    for (size_t i = 0; i < v.size(); ++i) {
        if (v[i].exprTree() && v[i].exprTree()->hasVariableRefs()) return true;
    }
    return false;
}

// Forward declaration
static ExprNodePtr substituteVarRefs(const ExprNodePtr& tree,
                                      const std::map<std::string, ExprNodePtr>& subst);

ExprNodePtr BlenderGenerator::resolveExprTree(const Value& value) {
    ExprNodePtr tree = value.exprTree();
    if (!tree) return nullptr;

    // If it's a VarRef for a non-group-input variable, expand it
    // But keep runtime Python variable refs (module params, loop vars) as VarRefs
    if (tree->kind == ExprNode::Kind::VarRef &&
        group_input_vars_.find(tree->var_name) == group_input_vars_.end()) {
        // Keep references to runtime Python variables (module params, loop vars) unresolved
        if (isRuntimePythonVar(tree->var_name)) {
            return tree;
        }
        // Guard against infinite recursion
        if (eval_visiting_.count(tree->var_name)) return tree;
        eval_visiting_.insert(tree->var_name);
        // Look up in variables_ map
        auto it = variables_.find(tree->var_name);
        ExprNodePtr result = tree;
        if (it != variables_.end()) {
            result = resolveExprTree(it->second);
            if (!result) result = tree;
        }
        eval_visiting_.erase(tree->var_name);
        return result;
    }
    // For binary/unary ops, recursively resolve children
    if (tree->kind == ExprNode::Kind::BinaryOp) {
        auto resolved = std::make_shared<ExprNode>();
        resolved->kind = ExprNode::Kind::BinaryOp;
        resolved->op = tree->op;
        // Create temp Values to recurse on left/right
        Value leftVal;
        leftVal.setExprTree(tree->left);
        Value rightVal;
        rightVal.setExprTree(tree->right);
        resolved->left = resolveExprTree(leftVal);
        resolved->right = resolveExprTree(rightVal);
        return resolved;
    }
    if (tree->kind == ExprNode::Kind::UnaryOp) {
        auto resolved = std::make_shared<ExprNode>();
        resolved->kind = ExprNode::Kind::UnaryOp;
        resolved->op = tree->op;
        Value operandVal;
        operandVal.setExprTree(tree->left);
        resolved->left = resolveExprTree(operandVal);
        return resolved;
    }
    if (tree->kind == ExprNode::Kind::FunctionCall) {
        // Resolve args first
        std::vector<ExprNodePtr> resolvedArgs;
        for (const auto& arg : tree->func_args) {
            Value argVal;
            argVal.setExprTree(arg);
            resolvedArgs.push_back(resolveExprTree(argVal));
        }
        // Inline user-defined functions (but not recursive ones)
        auto fit = functions_.find(tree->func_name);
        if (fit != functions_.end() && fit->second.body) {
            const FuncDef& fd = fit->second;
            // Check if function is recursive — don't inline recursive functions
            bool isRecursive = exprTreeCallsFunction(fd.body, tree->func_name);
            if (isRecursive) {
                python_helper_functions_.insert(tree->func_name);
                auto resolved = std::make_shared<ExprNode>();
                resolved->kind = ExprNode::Kind::FunctionCall;
                resolved->func_name = tree->func_name;
                resolved->func_args = resolvedArgs;
                return resolved;
            }
            std::map<std::string, ExprNodePtr> subst;
            for (size_t i = 0; i < fd.params.size() && i < resolvedArgs.size(); i++) {
                subst[fd.params[i]] = resolvedArgs[i];
            }
            ExprNodePtr inlined = substituteVarRefs(fd.body, subst);
            // Recursively resolve the inlined body
            Value inlinedVal;
            inlinedVal.setExprTree(inlined);
            return resolveExprTree(inlinedVal);
        }
        auto resolved = std::make_shared<ExprNode>();
        resolved->kind = ExprNode::Kind::FunctionCall;
        resolved->func_name = tree->func_name;
        resolved->func_args = resolvedArgs;
        return resolved;
    }
    if (tree->kind == ExprNode::Kind::VectorLiteral) {
        auto resolved = std::make_shared<ExprNode>();
        resolved->kind = ExprNode::Kind::VectorLiteral;
        for (const auto& elem : tree->vec_elements) {
            Value elemVal;
            elemVal.setExprTree(elem);
            resolved->vec_elements.push_back(resolveExprTree(elemVal));
        }
        return resolved;
    }
    if (tree->kind == ExprNode::Kind::ForLoop) {
        auto resolved = std::make_shared<ExprNode>();
        resolved->kind = ExprNode::Kind::ForLoop;
        resolved->var_name = tree->var_name;
        Value rangeVal;
        rangeVal.setExprTree(tree->left);
        resolved->left = resolveExprTree(rangeVal);
        Value bodyVal;
        bodyVal.setExprTree(tree->right);
        resolved->right = resolveExprTree(bodyVal);
        return resolved;
    }
    if (tree->kind == ExprNode::Kind::LetBinding) {
        auto resolved = std::make_shared<ExprNode>();
        resolved->kind = ExprNode::Kind::LetBinding;
        for (const auto& binding : tree->let_bindings) {
            Value bval;
            bval.setExprTree(binding.second);
            resolved->let_bindings.push_back({binding.first, resolveExprTree(bval)});
        }
        Value bodyVal;
        bodyVal.setExprTree(tree->right);
        resolved->right = resolveExprTree(bodyVal);
        return resolved;
    }
    if (tree->kind == ExprNode::Kind::Conditional) {
        auto resolved = std::make_shared<ExprNode>();
        resolved->kind = ExprNode::Kind::Conditional;
        Value condVal, thenVal, elseVal;
        condVal.setExprTree(tree->left);
        thenVal.setExprTree(tree->right);
        elseVal.setExprTree(tree->else_branch);
        resolved->left = resolveExprTree(condVal);
        resolved->right = resolveExprTree(thenVal);
        resolved->else_branch = resolveExprTree(elseVal);
        return resolved;
    }
    return tree;  // Literal or group_input VarRef
}

EmitResult BlenderGenerator::emitExpressionNodeTree(const ExprNodePtr& expr) {
    if (!expr) return {"__literal__", "Value", 0.0, false};

    switch (expr->kind) {
        case ExprNode::Kind::Literal:
            return {"__literal__", "Value", expr->literal_value, false};

        case ExprNode::Kind::VarRef: {
            // Should be a group_input variable at this point (after resolution)
            std::string socketName = expr->var_name;
            if (!socketName.empty() && socketName[0] == '$') {
                socketName = socketName.substr(1);
            }
            // If this variable is not a group_input socket, evaluate it numerically
            if (group_input_vars_.find(expr->var_name) == group_input_vars_.end()) {
                auto it = variables_.find(expr->var_name);
                double val = 0.0;
                if (it != variables_.end()) {
                    val = it->second.toNumber();
                }
                return {"__literal__", "Value", val, false};
            }
            return {"group_input", socketName, 0.0, true};
        }

        case ExprNode::Kind::UnaryOp: {
            // NEGATE: emit MULTIPLY by -1
            auto operandResult = emitExpressionNodeTree(expr->left);
            std::string mathId = newNodeId();
            emit("# Negate");
            emit(mathId + " = nodes.new('ShaderNodeMath')");
            emit(mathId + ".operation = 'MULTIPLY'");
            emit(mathId + ".location = (x_pos, y_pos)");
            emit("y_pos -= 50");
            connectExprResult(operandResult, mathId, 0);
            emit(mathId + ".inputs[1].default_value = -1.0");
            return {mathId, "Value", 0.0, false};
        }

        case ExprNode::Kind::BinaryOp: {
            auto leftResult = emitExpressionNodeTree(expr->left);
            auto rightResult = emitExpressionNodeTree(expr->right);

            std::string opStr;
            switch (expr->op) {
                case ExprNode::Op::ADD: opStr = "ADD"; break;
                case ExprNode::Op::SUBTRACT: opStr = "SUBTRACT"; break;
                case ExprNode::Op::MULTIPLY: opStr = "MULTIPLY"; break;
                case ExprNode::Op::DIVIDE: opStr = "DIVIDE"; break;
                case ExprNode::Op::MODULO: opStr = "MODULO"; break;
                case ExprNode::Op::POWER: opStr = "POWER"; break;
                default: opStr = "ADD"; break;
            }

            std::string mathId = newNodeId();
            emit(mathId + " = nodes.new('ShaderNodeMath')");
            emit(mathId + ".operation = '" + opStr + "'");
            emit(mathId + ".location = (x_pos, y_pos)");
            emit("y_pos -= 50");
            connectExprResult(leftResult, mathId, 0);
            connectExprResult(rightResult, mathId, 1);
            return {mathId, "Value", 0.0, false};
        }

        case ExprNode::Kind::FunctionCall: {
            // Evaluate to a literal value using evaluateExprTree
            double val = evaluateExprTree(expr);
            return {"__literal__", "Value", val, false};
        }

        case ExprNode::Kind::VectorLiteral: {
            // Vectors can't be emitted as single math nodes; evaluate to literal
            double val = evaluateExprTree(expr);
            return {"__literal__", "Value", val, false};
        }
        case ExprNode::Kind::Conditional:
        case ExprNode::Kind::ForLoop:
        case ExprNode::Kind::LetBinding: {
            // These produce complex values; evaluate to literal
            double val = evaluateExprTree(expr);
            return {"__literal__", "Value", val, false};
        }
    }
    return {"__literal__", "Value", 0.0, false};
}

void BlenderGenerator::connectExprResult(const EmitResult& result, const std::string& targetNode, int inputIndex) {
    if (result.nodeId == "__literal__") {
        emit(targetNode + ".inputs[" + std::to_string(inputIndex) + "].default_value = " +
             std::to_string(result.literalValue));
    } else if (result.isGroupInput) {
        emit("links.new(group_input.outputs['" + result.socketName + "'], " +
             targetNode + ".inputs[" + std::to_string(inputIndex) + "])");
    } else {
        emit("links.new(" + result.nodeId + ".outputs['" + result.socketName + "'], " +
             targetNode + ".inputs[" + std::to_string(inputIndex) + "])");
    }
}

void BlenderGenerator::connectExprResultNamed(const EmitResult& result, const std::string& targetNode, const std::string& inputName) {
    if (result.nodeId == "__literal__") {
        emit(targetNode + ".inputs['" + inputName + "'].default_value = " +
             std::to_string(result.literalValue));
    } else if (result.isGroupInput) {
        emit("links.new(group_input.outputs['" + result.socketName + "'], " +
             targetNode + ".inputs['" + inputName + "'])");
    } else {
        emit("links.new(" + result.nodeId + ".outputs['" + result.socketName + "'], " +
             targetNode + ".inputs['" + inputName + "'])");
    }
}

std::string BlenderGenerator::emitVectorWithExprTrees(const std::string& targetNodeId,
                                                       const std::string& inputName,
                                                       const Value& vec) {
    std::string combineId = newNodeId();
    emit("# CombineXYZ for " + inputName);
    emit(combineId + " = nodes.new('ShaderNodeCombineXYZ')");
    emit(combineId + ".location = (x_pos, y_pos)");
    emit("y_pos -= 50");

    const char* components[] = {"X", "Y", "Z"};
    for (size_t i = 0; i < 3 && i < vec.size(); ++i) {
        const Value& comp = vec[i];
        ExprNodePtr tree = resolveExprTree(comp);
        if (tree && tree->hasVariableRefs() && exprTreeHasOnlyGroupInputVars(tree)) {
            auto result = emitExpressionNodeTree(tree);
            connectExprResultNamed(result, combineId, components[i]);
        } else if (tree && tree->hasVariableRefs() && exprTreeReferencesModuleParams(tree)) {
            // Runtime Python variable refs — emit as Python expression
            emit(combineId + ".inputs['" + components[i] + "'].default_value = " +
                 exprTreeToPython(tree));
        } else if (comp.exprTree() && comp.exprTree()->kind == ExprNode::Kind::Literal) {
            emit(combineId + ".inputs['" + components[i] + "'].default_value = " +
                 std::to_string(comp.exprTree()->literal_value));
        } else if (comp.isNumber()) {
            emit(combineId + ".inputs['" + components[i] + "'].default_value = " +
                 pyDouble(comp.toNumber()));
        } else if (comp.isExpression()) {
            ExprNodePtr compTree = resolveExprTree(comp);
            if (in_module_ && compTree && exprTreeReferencesModuleParams(compTree)) {
                emit(combineId + ".inputs['" + components[i] + "'].default_value = " +
                     exprTreeToPython(compTree));
            } else {
                double numVal = evaluateExpr(comp);
                emit(combineId + ".inputs['" + components[i] + "'].default_value = " +
                     pyDouble(numVal));
            }
        } else if (tree && !tree->hasVariableRefs()) {
            // Literal tree from resolved computation
            auto result = emitExpressionNodeTree(tree);
            connectExprResultNamed(result, combineId, components[i]);
        } else {
            emit(combineId + ".inputs['" + components[i] + "'].default_value = " + comp.toPython());
        }
    }

    emit("links.new(" + combineId + ".outputs['Vector'], " + targetNodeId + ".inputs['" + inputName + "'])");
    return combineId;
}

Value BlenderGenerator::resolveFn(const Arguments& args) {
    // 1. Check function args for explicit $fn
    Value fn = getArg(args, "$fn", Value());
    if (!fn.isUndefined()) return fn;

    // 2. Check global variables_ for $fn
    auto it = variables_.find("$fn");
    if (it != variables_.end()) {
        // If $fn is a group_input variable, return an expression reference
        // so the generated code links to the group_input socket
        if (group_input_vars_.find("$fn") != group_input_vars_.end()) {
            return Value::expressionWithTree("$fn", ExprNode::makeVarRef("$fn"));
        }
        // Otherwise return the literal value
        return it->second;
    }

    // 3. Default
    return Value(32.0);
}

double BlenderGenerator::evaluateExpr(const Value& value) {
    ExprNodePtr tree = value.exprTree();
    if (tree) return evaluateExprTree(tree);
    return value.toNumber();
}

double BlenderGenerator::evaluateExprTree(const ExprNodePtr& tree) {
    if (!tree) return 0.0;
    switch (tree->kind) {
        case ExprNode::Kind::Literal:
            return tree->literal_value;
        case ExprNode::Kind::VarRef: {
            // Guard against infinite recursion (self-referencing variables)
            if (eval_visiting_.count(tree->var_name)) return 0.0;
            eval_visiting_.insert(tree->var_name);
            auto it = variables_.find(tree->var_name);
            double result = 0.0;
            if (it != variables_.end()) {
                result = evaluateExpr(it->second);
            } else {
                // Handle var.x / var.y / var.z member access
                std::string varName = tree->var_name;
                int memberIdx = -1;
                if (varName.size() > 2 && varName[varName.size()-2] == '.') {
                    char member = varName.back();
                    if (member == 'x') memberIdx = 0;
                    else if (member == 'y') memberIdx = 1;
                    else if (member == 'z') memberIdx = 2;
                    if (memberIdx >= 0) {
                        std::string baseName = varName.substr(0, varName.size() - 2);
                        auto bit = variables_.find(baseName);
                        if (bit != variables_.end() && bit->second.isVector() &&
                            static_cast<size_t>(memberIdx) < bit->second.size()) {
                            result = bit->second[memberIdx].toNumber();
                        }
                    }
                }
            }
            eval_visiting_.erase(tree->var_name);
            return result;
        }
        case ExprNode::Kind::UnaryOp: {
            double operand = evaluateExprTree(tree->left);
            if (tree->op == ExprNode::Op::NEGATE) return -operand;
            return operand;
        }
        case ExprNode::Kind::BinaryOp: {
            double left = evaluateExprTree(tree->left);
            double right = evaluateExprTree(tree->right);
            switch (tree->op) {
                case ExprNode::Op::ADD: return left + right;
                case ExprNode::Op::SUBTRACT: return left - right;
                case ExprNode::Op::MULTIPLY: return left * right;
                case ExprNode::Op::DIVIDE: return right != 0 ? left / right : 0.0;
                case ExprNode::Op::MODULO: return right != 0 ? std::fmod(left, right) : 0.0;
                case ExprNode::Op::POWER: return std::pow(left, right);
                default: return 0.0;
            }
        }
        case ExprNode::Kind::FunctionCall: {
            // Try user-defined functions first (they may return vectors that get indexed)
            auto fit = functions_.find(tree->func_name);
            if (fit != functions_.end()) {
                const FuncDef& fd = fit->second;
                // Temporarily bind parameters to evaluated args
                auto saved = variables_;
                for (size_t i = 0; i < fd.params.size() && i < tree->func_args.size(); i++) {
                    double argVal = evaluateExprTree(tree->func_args[i]);
                    variables_[fd.params[i]] = Value(argVal);
                }
                double result = evaluateExprTree(fd.body);
                variables_ = saved;
                return result;
            }
            // Fall back to built-in math functions
            std::vector<double> args;
            for (const auto& arg : tree->func_args) {
                args.push_back(evaluateExprTree(arg));
            }
            return evaluateBuiltinMath(tree->func_name, args);
        }
        case ExprNode::Kind::VectorLiteral:
            return 0.0;  // Vectors can't be reduced to a single double
        case ExprNode::Kind::Conditional:
        case ExprNode::Kind::ForLoop:
        case ExprNode::Kind::LetBinding:
            return 0.0;  // Complex expressions can't be reduced to a single double
    }
    return 0.0;
}

Value BlenderGenerator::evaluateExprToValue(const Value& value) {
    ExprNodePtr tree = value.exprTree();
    if (tree) return evaluateExprTreeToValue(tree);
    if (value.isNumber()) return value;
    if (value.isVector()) return value;
    return Value(value.toNumber());
}

Value BlenderGenerator::evaluateExprTreeToValue(const ExprNodePtr& tree) {
    if (!tree) return Value();
    switch (tree->kind) {
        case ExprNode::Kind::Literal:
            return Value(tree->literal_value);
        case ExprNode::Kind::VarRef: {
            // Guard against infinite recursion (self-referencing variables)
            if (eval_visiting_.count(tree->var_name)) return Value(0.0);
            eval_visiting_.insert(tree->var_name);
            auto it = variables_.find(tree->var_name);
            Value result(0.0);
            if (it != variables_.end()) {
                result = evaluateExprToValue(it->second);
            }
            eval_visiting_.erase(tree->var_name);
            return result;
        }
        case ExprNode::Kind::UnaryOp: {
            Value operand = evaluateExprTreeToValue(tree->left);
            if (tree->op == ExprNode::Op::NEGATE) {
                if (operand.isNumber()) return Value(-operand.toNumber());
                if (operand.isVector()) {
                    Vector v;
                    for (size_t i = 0; i < operand.size(); i++) {
                        if (operand[i].isNumber()) v.push_back(Value(-operand[i].toNumber()));
                        else v.push_back(operand[i]);
                    }
                    return Value(v);
                }
            }
            return operand;
        }
        case ExprNode::Kind::BinaryOp: {
            Value left = evaluateExprTreeToValue(tree->left);
            Value right = evaluateExprTreeToValue(tree->right);
            if (left.isNumber() && right.isNumber()) {
                double l = left.toNumber(), r = right.toNumber();
                switch (tree->op) {
                    case ExprNode::Op::ADD: return Value(l + r);
                    case ExprNode::Op::SUBTRACT: return Value(l - r);
                    case ExprNode::Op::MULTIPLY: return Value(l * r);
                    case ExprNode::Op::DIVIDE: return r != 0 ? Value(l / r) : Value();
                    case ExprNode::Op::MODULO: return r != 0 ? Value(std::fmod(l, r)) : Value();
                    case ExprNode::Op::POWER: return Value(std::pow(l, r));
                    default: return Value();
                }
            }
            // Scalar * Vector or Vector * Scalar
            if (tree->op == ExprNode::Op::MULTIPLY) {
                Value scalar, vec;
                if (left.isNumber() && right.isVector()) { scalar = left; vec = right; }
                else if (left.isVector() && right.isNumber()) { scalar = right; vec = left; }
                if (vec.isVector() && scalar.isNumber()) {
                    double s = scalar.toNumber();
                    Vector result;
                    for (size_t i = 0; i < vec.size(); i++) {
                        if (vec[i].isNumber()) result.push_back(Value(vec[i].toNumber() * s));
                        else result.push_back(vec[i]);
                    }
                    return Value(result);
                }
            }
            // Vector +/- Vector
            if ((tree->op == ExprNode::Op::ADD || tree->op == ExprNode::Op::SUBTRACT) &&
                left.isVector() && right.isVector()) {
                Vector result;
                size_t len = std::min(left.size(), right.size());
                for (size_t i = 0; i < len; i++) {
                    if (left[i].isNumber() && right[i].isNumber()) {
                        double v = (tree->op == ExprNode::Op::ADD)
                            ? left[i].toNumber() + right[i].toNumber()
                            : left[i].toNumber() - right[i].toNumber();
                        result.push_back(Value(v));
                    } else {
                        result.push_back(Value());
                    }
                }
                return Value(result);
            }
            return Value();
        }
        case ExprNode::Kind::FunctionCall: {
            // Try user-defined functions
            auto fit = functions_.find(tree->func_name);
            if (fit != functions_.end()) {
                const FuncDef& fd = fit->second;
                auto saved = variables_;
                for (size_t i = 0; i < fd.params.size() && i < tree->func_args.size(); i++) {
                    Value argVal = evaluateExprTreeToValue(tree->func_args[i]);
                    variables_[fd.params[i]] = argVal;
                }
                Value result = evaluateExprTreeToValue(fd.body);
                variables_ = saved;
                return result;
            }
            // Built-in math (returns double)
            std::vector<double> args;
            for (const auto& arg : tree->func_args) {
                args.push_back(evaluateExprTree(arg));
            }
            if (isBuiltinFunction(tree->func_name)) {
                return Value(evaluateBuiltinMath(tree->func_name, args));
            }
            return Value();
        }
        case ExprNode::Kind::VectorLiteral: {
            Vector v;
            for (const auto& elem : tree->vec_elements) {
                v.push_back(evaluateExprTreeToValue(elem));
            }
            return Value(v);
        }
        case ExprNode::Kind::Conditional:
        case ExprNode::Kind::ForLoop:
        case ExprNode::Kind::LetBinding:
            return Value();
    }
    return Value();
}

// Substitute VarRefs in an expression tree using a substitution map
static ExprNodePtr substituteVarRefs(const ExprNodePtr& tree,
                                      const std::map<std::string, ExprNodePtr>& subst) {
    if (!tree) return nullptr;
    switch (tree->kind) {
        case ExprNode::Kind::Literal:
            return tree;
        case ExprNode::Kind::VarRef: {
            auto it = subst.find(tree->var_name);
            if (it != subst.end()) return it->second;
            return tree;
        }
        case ExprNode::Kind::UnaryOp: {
            auto result = std::make_shared<ExprNode>();
            result->kind = ExprNode::Kind::UnaryOp;
            result->op = tree->op;
            result->left = substituteVarRefs(tree->left, subst);
            return result;
        }
        case ExprNode::Kind::BinaryOp: {
            auto result = std::make_shared<ExprNode>();
            result->kind = ExprNode::Kind::BinaryOp;
            result->op = tree->op;
            result->left = substituteVarRefs(tree->left, subst);
            result->right = substituteVarRefs(tree->right, subst);
            return result;
        }
        case ExprNode::Kind::FunctionCall: {
            auto result = std::make_shared<ExprNode>();
            result->kind = ExprNode::Kind::FunctionCall;
            result->func_name = tree->func_name;
            for (const auto& arg : tree->func_args) {
                result->func_args.push_back(substituteVarRefs(arg, subst));
            }
            result->arg_names = tree->arg_names;
            return result;
        }
        case ExprNode::Kind::VectorLiteral: {
            auto result = std::make_shared<ExprNode>();
            result->kind = ExprNode::Kind::VectorLiteral;
            for (const auto& elem : tree->vec_elements) {
                result->vec_elements.push_back(substituteVarRefs(elem, subst));
            }
            return result;
        }
        case ExprNode::Kind::Conditional: {
            auto result = std::make_shared<ExprNode>();
            result->kind = ExprNode::Kind::Conditional;
            result->left = substituteVarRefs(tree->left, subst);
            result->right = substituteVarRefs(tree->right, subst);
            result->else_branch = substituteVarRefs(tree->else_branch, subst);
            return result;
        }
        case ExprNode::Kind::ForLoop: {
            auto result = std::make_shared<ExprNode>();
            result->kind = ExprNode::Kind::ForLoop;
            result->var_name = tree->var_name;
            result->left = substituteVarRefs(tree->left, subst);  // range
            // Don't substitute the loop variable in the body — it shadows outer vars
            auto innerSubst = subst;
            innerSubst.erase(tree->var_name);
            result->right = substituteVarRefs(tree->right, innerSubst);  // body
            return result;
        }
        case ExprNode::Kind::LetBinding: {
            auto result = std::make_shared<ExprNode>();
            result->kind = ExprNode::Kind::LetBinding;
            // Substitute in binding expressions, but remove bound names from body subst
            auto innerSubst = subst;
            for (const auto& binding : tree->let_bindings) {
                result->let_bindings.push_back({binding.first, substituteVarRefs(binding.second, subst)});
                innerSubst.erase(binding.first);  // shadow outer var
            }
            result->right = substituteVarRefs(tree->right, innerSubst);  // body
            return result;
        }
        default:
            return tree;
    }
}

std::string BlenderGenerator::exprTreeToPython(const ExprNodePtr& tree) {
    if (!tree) return "0";
    switch (tree->kind) {
        case ExprNode::Kind::Literal:
            return std::to_string(tree->literal_value);
        case ExprNode::Kind::VarRef: {
            // OpenSCAD special variables ($children, $parent_modules, etc.)
            // have no meaningful representation in Blender geometry nodes — use defaults.
            if (!tree->var_name.empty() && tree->var_name[0] == '$') {
                if (tree->var_name == "$preview") return "True";
                if (tree->var_name == "$children" ||
                    tree->var_name == "$parent_modules" ||
                    tree->var_name == "$vpd" || tree->var_name == "$vpr" ||
                    tree->var_name == "$vpt" || tree->var_name == "$t" ||
                    tree->var_name == "$idx" || tree->var_name == "$idxON") {
                    return "0";
                }
                // Check if it's a group_input var before defaulting
                if (!group_input_vars_.count(tree->var_name)) {
                    return "0";
                }
            }
            // Runtime Python variables (module params, loop vars) stay as variable references
            if (isRuntimePythonVar(tree->var_name)) {
                return pyName(tree->var_name);
            }
            // Group input variables should use their Python variable name
            // so that changes to the socket propagate through module calls.
            // But inside module function bodies, these variables don't exist as
            // Python locals — resolve to their concrete values instead.
            if (group_input_vars_.count(tree->var_name)) {
                if (in_module_) {
                    auto it = variables_.find(tree->var_name);
                    if (it != variables_.end()) {
                        return pyDouble(it->second.toNumber());
                    }
                    return "0";
                }
                return pyName(tree->var_name);
            }
            // If the variable can be resolved to a concrete value, use that
            {
                auto it = variables_.find(tree->var_name);
                if (it != variables_.end()) {
                    return pyDouble(it->second.toNumber());
                }
            }
            // Unresolvable variable — cannot exist as a Python local.
            // Return 0 rather than an undefined identifier.
            return "0";
        }
        case ExprNode::Kind::UnaryOp:
            if (tree->op == ExprNode::Op::NEGATE)
                return "_vneg(" + exprTreeToPython(tree->left) + ")";
            if (tree->op == ExprNode::Op::NOT)
                return "(not " + exprTreeToPython(tree->left) + ")";
            return exprTreeToPython(tree->left);
        case ExprNode::Kind::BinaryOp: {
            std::string l = exprTreeToPython(tree->left);
            std::string r = exprTreeToPython(tree->right);
            switch (tree->op) {
                case ExprNode::Op::ADD: return "_vadd(" + l + ", " + r + ")";
                case ExprNode::Op::SUBTRACT: return "_vsub(" + l + ", " + r + ")";
                case ExprNode::Op::MULTIPLY: return "_vmul(" + l + ", " + r + ")";
                case ExprNode::Op::DIVIDE: return "_vdiv(" + l + ", " + r + ")";
                case ExprNode::Op::MODULO: return "(" + l + " % " + r + ")";
                case ExprNode::Op::POWER: return "(" + l + " ** " + r + ")";
                case ExprNode::Op::LESS: return "_vlt(" + l + ", " + r + ")";
                case ExprNode::Op::GREATER: return "_vgt(" + l + ", " + r + ")";
                case ExprNode::Op::LESS_EQ: return "(" + l + " <= " + r + ")";
                case ExprNode::Op::GREATER_EQ: return "(" + l + " >= " + r + ")";
                case ExprNode::Op::EQUAL: return "(" + l + " == " + r + ")";
                case ExprNode::Op::NOT_EQUAL: return "(" + l + " != " + r + ")";
                case ExprNode::Op::AND: return "(" + l + " and " + r + ")";
                case ExprNode::Op::OR: return "(" + l + " or " + r + ")";
                default: return "0";
            }
        }
        case ExprNode::Kind::FunctionCall: {
            // Map OpenSCAD function names to Python equivalents
            std::string fn = tree->func_name;
            // Type-check functions → isinstance() checks
            if (fn == "is_bool" && tree->func_args.size() == 1)
                return "isinstance(" + exprTreeToPython(tree->func_args[0]) + ", bool)";
            if (fn == "is_num" && tree->func_args.size() == 1)
                return "isinstance(" + exprTreeToPython(tree->func_args[0]) + ", (int, float))";
            if (fn == "is_list" && tree->func_args.size() == 1)
                return "isinstance(" + exprTreeToPython(tree->func_args[0]) + ", (list, tuple))";
            if (fn == "is_string" && tree->func_args.size() == 1)
                return "isinstance(" + exprTreeToPython(tree->func_args[0]) + ", str)";
            if (fn == "is_undef" && tree->func_args.size() == 1)
                return "(" + exprTreeToPython(tree->func_args[0]) + " is None)";
            // Inline user-defined functions by substituting parameters in body
            {
                auto fit = functions_.find(fn);
                if (fit != functions_.end() && fit->second.body) {
                    const FuncDef& fd = fit->second;
                    // Check if function is recursive (body references itself)
                    bool isRecursive = exprTreeCallsFunction(fd.body, fn);
                    if (isRecursive) {
                        // Emit as Python function call — the function will be emitted
                        // as a helper in emitPythonHelperFunctions
                        python_helper_functions_.insert(fn);
                        std::string argsStr;
                        for (size_t i = 0; i < tree->func_args.size(); i++) {
                            if (i > 0) argsStr += ", ";
                            argsStr += exprTreeToPython(tree->func_args[i]);
                        }
                        return "_scad_" + pyName(fn) + "(" + argsStr + ")";
                    }
                    // Non-recursive: inline by substituting parameters in body
                    std::map<std::string, ExprNodePtr> subst;
                    for (size_t i = 0; i < fd.params.size() && i < tree->func_args.size(); i++) {
                        subst[fd.params[i]] = tree->func_args[i];
                    }
                    ExprNodePtr inlined = substituteVarRefs(fd.body, subst);
                    return exprTreeToPython(inlined);
                }
            }
            if (fn == "sin" || fn == "cos" || fn == "tan") {
                // OpenSCAD trig functions take degrees, Python math takes radians
                std::string argExpr = exprTreeToPython(tree->func_args[0]);
                return "math." + fn + "(math.radians(" + argExpr + "))";
            }
            if (fn == "asin" || fn == "acos" || fn == "atan") {
                // OpenSCAD inverse trig returns degrees
                std::string argExpr = exprTreeToPython(tree->func_args[0]);
                return "math.degrees(math." + fn + "(" + argExpr + "))";
            }
            if (fn == "atan2") {
                std::string argsStr;
                for (size_t i = 0; i < tree->func_args.size(); i++) {
                    if (i > 0) argsStr += ", ";
                    argsStr += exprTreeToPython(tree->func_args[i]);
                }
                return "math.degrees(math.atan2(" + argsStr + "))";
            }
            if (fn == "sqrt") fn = "math.sqrt";
            else if (fn == "abs") fn = "abs";
            else if (fn == "pow") fn = "math.pow";
            else if (fn == "str") {
                // OpenSCAD str() concatenates all args into a string
                // Convert to: str(a) + str(b) + str(c) ...
                if (tree->func_args.empty()) return "\"\"";
                std::string result;
                for (size_t i = 0; i < tree->func_args.size(); i++) {
                    if (i > 0) result += " + ";
                    result += "str(" + exprTreeToPython(tree->func_args[i]) + ")";
                }
                return result;
            }
            else if (fn == "norm") fn = "abs";  // scalar approximation
            else if (fn == "len") fn = "len";
            else if (fn == "max") fn = "max";
            else if (fn == "min") fn = "min";
            else if (fn == "round") fn = "round";
            else if (fn == "floor") fn = "math.floor";
            else if (fn == "ceil") fn = "math.ceil";
            else if (fn == "exp") fn = "math.exp";
            else if (fn == "ln" || fn == "log") fn = "math.log";
            else if (fn == "sign") {
                std::string a = exprTreeToPython(tree->func_args[0]);
                return "(1 if " + a + " > 0 else (-1 if " + a + " < 0 else 0))";
            }
            else if (fn == "rands") fn = "_scad_rands";
            else if (fn == "__index__" && tree->func_args.size() == 2) {
                // Array indexing: __index__(vec, idx) → safe subscript
                std::string vec = exprTreeToPython(tree->func_args[0]);
                std::string idx = exprTreeToPython(tree->func_args[1]);
                return "(" + vec + "[int(" + idx + ")] if isinstance(" + vec + ", (list, tuple)) else " + vec + ")";
            }
            else if (fn == "__range__" && tree->func_args.size() >= 2) {
                // Range: __range__(start, end[, step]) → range(int(start), int(end)+1[, int(step)])
                std::string start = exprTreeToPython(tree->func_args[0]);
                std::string end = exprTreeToPython(tree->func_args[1]);
                if (tree->func_args.size() > 2) {
                    std::string step = exprTreeToPython(tree->func_args[2]);
                    return "range(int(" + start + "), int(" + end + ")+1, int(" + step + "))";
                }
                return "range(int(" + start + "), int(" + end + ")+1)";
            }
            else if (fn == "concat") {
                // concat in Python: list concatenation
                // Use lambda to avoid evaluating each argument twice
                std::string result = "(";
                for (size_t i = 0; i < tree->func_args.size(); i++) {
                    if (i > 0) result += " + ";
                    std::string arg = exprTreeToPython(tree->func_args[i]);
                    result += "(lambda _c: list(_c) if isinstance(_c, (list, tuple)) else [_c])(" + arg + ")";
                }
                result += ")";
                return result;
            }
            else {
                // Unknown function — can't evaluate in Python, return 0
                return "0";
            }
            std::string argsStr;
            for (size_t i = 0; i < tree->func_args.size(); i++) {
                if (i > 0) argsStr += ", ";
                argsStr += exprTreeToPython(tree->func_args[i]);
            }
            return fn + "(" + argsStr + ")";
        }
        case ExprNode::Kind::VectorLiteral: {
            std::string result = "[";
            for (size_t i = 0; i < tree->vec_elements.size(); i++) {
                if (i > 0) result += ", ";
                result += exprTreeToPython(tree->vec_elements[i]);
            }
            result += "]";
            return result;
        }
        case ExprNode::Kind::Conditional: {
            std::string cond = exprTreeToPython(tree->left);
            std::string then_expr = exprTreeToPython(tree->right);
            std::string else_expr = tree->else_branch ? exprTreeToPython(tree->else_branch) : "0";
            return "(" + then_expr + " if " + cond + " else " + else_expr + ")";
        }
        case ExprNode::Kind::ForLoop: {
            // [for (var = range) body] → [body for var in range]
            std::string var = pyName(tree->var_name);
            // Handle range expression
            std::string rangeExpr;
            if (tree->left && tree->left->kind == ExprNode::Kind::FunctionCall &&
                tree->left->func_name == "__range__") {
                std::string start = exprTreeToPython(tree->left->func_args[0]);
                std::string end = exprTreeToPython(tree->left->func_args[1]);
                if (tree->left->func_args.size() > 2) {
                    std::string step = exprTreeToPython(tree->left->func_args[2]);
                    rangeExpr = "range(int(" + start + "), int(" + end + ")+1, int(" + step + "))";
                } else {
                    rangeExpr = "range(int(" + start + "), int(" + end + ")+1)";
                }
            } else if (tree->left) {
                rangeExpr = exprTreeToPython(tree->left);
            } else {
                rangeExpr = "[]";
            }
            // Temporarily add loop var as runtime var
            bool wasPresent = loop_variables_.count(tree->var_name) > 0;
            loop_variables_.insert(tree->var_name);
            std::string body = exprTreeToPython(tree->right);
            if (!wasPresent) loop_variables_.erase(tree->var_name);
            return "[" + body + " for " + var + " in " + rangeExpr + "]";
        }
        case ExprNode::Kind::LetBinding: {
            // let(a=expr1, b=expr2) body → (lambda a, b: body)(expr1, expr2)
            if (tree->let_bindings.empty()) {
                return exprTreeToPython(tree->right);
            }
            std::string params, args;
            // Temporarily add let-bound names as runtime vars
            std::vector<std::string> added;
            for (size_t i = 0; i < tree->let_bindings.size(); i++) {
                if (i > 0) { params += ", "; args += ", "; }
                std::string pname = pyName(tree->let_bindings[i].first);
                params += pname;
                args += exprTreeToPython(tree->let_bindings[i].second);
                if (loop_variables_.find(tree->let_bindings[i].first) == loop_variables_.end()) {
                    loop_variables_.insert(tree->let_bindings[i].first);
                    added.push_back(tree->let_bindings[i].first);
                }
            }
            std::string body = exprTreeToPython(tree->right);
            // Remove the temporarily added let-bound names
            for (const auto& a : added) {
                loop_variables_.erase(a);
            }
            return "(lambda " + params + ": " + body + ")(" + args + ")";
        }
    }
    return "0";
}

bool BlenderGenerator::exprTreeCallsFunction(const ExprNodePtr& tree, const std::string& funcName) {
    if (!tree) return false;
    switch (tree->kind) {
        case ExprNode::Kind::Literal:
        case ExprNode::Kind::VarRef:
            return false;
        case ExprNode::Kind::UnaryOp:
            return exprTreeCallsFunction(tree->left, funcName);
        case ExprNode::Kind::BinaryOp:
            return exprTreeCallsFunction(tree->left, funcName) ||
                   exprTreeCallsFunction(tree->right, funcName);
        case ExprNode::Kind::FunctionCall:
            if (tree->func_name == funcName) return true;
            for (const auto& arg : tree->func_args) {
                if (exprTreeCallsFunction(arg, funcName)) return true;
            }
            return false;
        case ExprNode::Kind::VectorLiteral:
            for (const auto& elem : tree->vec_elements) {
                if (exprTreeCallsFunction(elem, funcName)) return true;
            }
            return false;
        case ExprNode::Kind::Conditional:
            return exprTreeCallsFunction(tree->left, funcName) ||
                   exprTreeCallsFunction(tree->right, funcName) ||
                   (tree->else_branch && exprTreeCallsFunction(tree->else_branch, funcName));
        case ExprNode::Kind::ForLoop:
            return exprTreeCallsFunction(tree->left, funcName) ||
                   exprTreeCallsFunction(tree->right, funcName);
        case ExprNode::Kind::LetBinding:
            for (const auto& b : tree->let_bindings) {
                if (exprTreeCallsFunction(b.second, funcName)) return true;
            }
            return exprTreeCallsFunction(tree->right, funcName);
    }
    return false;
}

void BlenderGenerator::emitPythonHelperFunctions() {
    for (const auto& fn : python_helper_functions_) {
        auto fit = functions_.find(fn);
        if (fit == functions_.end() || !fit->second.body) continue;
        const FuncDef& fd = fit->second;

        // Emit Python function definition
        std::string params;
        for (size_t i = 0; i < fd.params.size(); i++) {
            if (i > 0) params += ", ";
            params += pyName(fd.params[i]);
        }
        emit("def _scad_" + pyName(fn) + "(" + params + "):");
        indent_++;
        // Temporarily treat function params as runtime Python vars
        // so exprTreeToPython emits them as variable references
        std::set<std::string> saved_loop_vars = loop_variables_;
        for (const auto& p : fd.params) {
            loop_variables_.insert(p);
        }
        std::string body = exprTreeToPython(fd.body);
        loop_variables_ = saved_loop_vars;
        emit("return " + body);
        indent_--;
        emitBlank();
    }
}

ExprNodePtr BlenderGenerator::getOrMakeLiteralTree(const Value& value) {
    ExprNodePtr tree = resolveExprTree(value);
    if (tree) return tree;
    // No expression tree — create a literal from the numeric value
    return ExprNode::makeLiteral(value.toNumber());
}

Value BlenderGenerator::makeDivisionValue(const Value& numerator, double divisor) {
    ExprNodePtr numTree = getOrMakeLiteralTree(numerator);
    ExprNodePtr divTree = ExprNode::makeBinary(
        ExprNode::Op::DIVIDE, numTree, ExprNode::makeLiteral(divisor));
    Value result;
    result.setExprTree(divTree);
    return result;
}

void BlenderGenerator::emitScalarToVectorInput(const std::string& targetNodeId,
                                                const std::string& inputName,
                                                const ExprNodePtr& tree,
                                                int component,
                                                double defaultX, double defaultY, double defaultZ) {
    std::string combineId = newNodeId();
    emit("# CombineXYZ for " + inputName);
    emit(combineId + " = nodes.new('ShaderNodeCombineXYZ')");
    emit(combineId + ".location = (x_pos, y_pos)");
    emit("y_pos -= 50");

    double defaults[] = {defaultX, defaultY, defaultZ};
    const char* components[] = {"X", "Y", "Z"};

    for (int i = 0; i < 3; ++i) {
        if (i == component) {
            auto result = emitExpressionNodeTree(tree);
            connectExprResultNamed(result, combineId, components[i]);
        } else {
            emit(combineId + ".inputs['" + std::string(components[i]) + "'].default_value = " +
                 std::to_string(defaults[i]));
        }
    }

    emit("links.new(" + combineId + ".outputs['Vector'], " + targetNodeId + ".inputs['" + inputName + "'])");
}

void BlenderGenerator::emitScalarToAllVectorComponents(const std::string& targetNodeId,
                                                        const std::string& inputName,
                                                        const ExprNodePtr& tree) {
    // If the tree references runtime Python variables (module params, loop vars),
    // use Python expression path instead of creating Blender math nodes
    if (exprTreeReferencesModuleParams(tree)) {
        std::string pyExpr = exprTreeToPython(tree);
        std::string combineId = newNodeId();
        emit("# CombineXYZ (scalar to all components) for " + inputName);
        emit(combineId + " = nodes.new('ShaderNodeCombineXYZ')");
        emit(combineId + ".location = (x_pos, y_pos)");
        emit("y_pos -= 50");
        const char* components[] = {"X", "Y", "Z"};
        for (int i = 0; i < 3; ++i) {
            emit(combineId + ".inputs['" + std::string(components[i]) + "'].default_value = " + pyExpr);
        }
        emit("links.new(" + combineId + ".outputs['Vector'], " + targetNodeId + ".inputs['" + inputName + "'])");
        return;
    }

    // Emit the expression once, then wire it to all three components of a CombineXYZ
    auto result = emitExpressionNodeTree(tree);

    std::string combineId = newNodeId();
    emit("# CombineXYZ (scalar to all components) for " + inputName);
    emit(combineId + " = nodes.new('ShaderNodeCombineXYZ')");
    emit(combineId + ".location = (x_pos, y_pos)");
    emit("y_pos -= 50");

    const char* components[] = {"X", "Y", "Z"};
    for (int i = 0; i < 3; ++i) {
        connectExprResultNamed(result, combineId, components[i]);
    }

    emit("links.new(" + combineId + ".outputs['Vector'], " + targetNodeId + ".inputs['" + inputName + "'])");
}

void BlenderGenerator::collectModulesRecursive(ASTNode* node) {
    if (!node) return;
    if (node->type() == ASTNode::Type::Module) {
        auto* mod = dynamic_cast<ModuleNode*>(node);
        if (mod) {
            defined_modules_[mod->name()] = mod->parameters();

            // Compute captured parent params for this nested module
            if (!parent_module_params_stack_.empty()) {
                const auto& parent_params = parent_module_params_stack_.back();
                std::set<std::string> own_params;
                for (const auto& p : mod->parameters()) {
                    own_params.insert(pyName(p));
                }
                std::vector<std::string> captured;
                for (const auto& pp : parent_params) {
                    if (own_params.find(pp) == own_params.end()) {
                        captured.push_back(pp);
                    }
                }
                if (!captured.empty()) {
                    std::sort(captured.begin(), captured.end());
                    captured_parent_params_[mod->name()] = captured;
                }
            }

            // Push this module's params (plus any parent params) for nested modules
            std::set<std::string> params;
            for (const auto& p : mod->parameters()) {
                params.insert(pyName(p));
            }
            for (const auto& parent_set : parent_module_params_stack_) {
                params.insert(parent_set.begin(), parent_set.end());
            }
            parent_module_params_stack_.push_back(params);
        }
    }
    for (auto& child : node->children()) {
        collectModulesRecursive(child.get());
    }
    if (node->type() == ASTNode::Type::Module) {
        if (!parent_module_params_stack_.empty()) {
            parent_module_params_stack_.pop_back();
        }
    }
}

void BlenderGenerator::collectFunctionsRecursive(ASTNode* node) {
    if (!node) return;
    if (node->type() == ASTNode::Type::FunctionDef) {
        auto* fn = dynamic_cast<FunctionNode*>(node);
        if (fn && fn->body()) {
            FuncDef fd;
            fd.params = fn->parameters();
            fd.body = fn->body();
            functions_[fn->name()] = fd;
        }
    }
    for (auto& child : node->children()) {
        collectFunctionsRecursive(child.get());
    }
}

void BlenderGenerator::collectVarRefsFromExpr(const ExprNodePtr& expr) {
    if (!expr) return;
    switch (expr->kind) {
        case ExprNode::Kind::VarRef:
            for_loop_range_vars_.insert(expr->var_name);
            break;
        case ExprNode::Kind::BinaryOp:
            collectVarRefsFromExpr(expr->left);
            collectVarRefsFromExpr(expr->right);
            break;
        case ExprNode::Kind::UnaryOp:
            collectVarRefsFromExpr(expr->left);
            break;
        case ExprNode::Kind::FunctionCall:
            for (auto& arg : expr->func_args)
                collectVarRefsFromExpr(arg);
            break;
        case ExprNode::Kind::Conditional:
            collectVarRefsFromExpr(expr->left);
            collectVarRefsFromExpr(expr->right);
            collectVarRefsFromExpr(expr->else_branch);
            break;
        default:
            break;
    }
}

void BlenderGenerator::collectForLoopRangeVars(ASTNode* node) {
    if (!node) return;
    if (node->type() == ASTNode::Type::ForLoop) {
        auto* forNode = dynamic_cast<ForLoopNode*>(node);
        if (forNode) {
            const Value& range = forNode->range();
            if (range.isRange()) {
                collectVarRefsFromExpr(range.rangeStartExpr());
                collectVarRefsFromExpr(range.rangeEndExpr());
                collectVarRefsFromExpr(range.rangeStepExpr());
            }
        }
    }
    for (auto& child : node->children()) {
        collectForLoopRangeVars(child.get());
    }
}

void BlenderGenerator::emitModulesRecursive(ASTNode* node) {
    if (!node) return;
    if (node->type() == ASTNode::Type::Module) {
        auto* mod = dynamic_cast<ModuleNode*>(node);
        if (mod) {
            emitModuleFunction(*mod);
        }
    }
    for (auto& child : node->children()) {
        emitModulesRecursive(child.get());
    }
}

bool BlenderGenerator::nodeProducesGeometry(ASTNode* node) {
    if (!node) return false;
    auto t = node->type();
    // Geometry-producing node types
    if (t == ASTNode::Type::Cube || t == ASTNode::Type::Sphere ||
        t == ASTNode::Type::Cylinder || t == ASTNode::Type::Circle ||
        t == ASTNode::Type::Square || t == ASTNode::Type::Polygon ||
        t == ASTNode::Type::Polyhedron || t == ASTNode::Type::Text ||
        t == ASTNode::Type::Import || t == ASTNode::Type::Surface ||
        t == ASTNode::Type::LinearExtrude || t == ASTNode::Type::RotateExtrude ||
        t == ASTNode::Type::Hull || t == ASTNode::Type::Minkowski ||
        t == ASTNode::Type::Roof || t == ASTNode::Type::Projection ||
        t == ASTNode::Type::Translate || t == ASTNode::Type::Rotate ||
        t == ASTNode::Type::Scale || t == ASTNode::Type::Mirror ||
        t == ASTNode::Type::Offset || t == ASTNode::Type::Resize ||
        t == ASTNode::Type::Multmatrix || t == ASTNode::Type::Children) {
        return true;
    }
    // A module call to a geometry-producing module
    if (t == ASTNode::Type::ModuleCall) {
        auto* mc = dynamic_cast<ModuleCallNode*>(node);
        if (mc && non_geometric_modules_.find(mc->name()) == non_geometric_modules_.end()) {
            // Calls a module not known to be non-geometric — assume it produces geometry
            return true;
        }
    }
    // Recurse into children (for loops, if-else, union, etc.)
    for (auto& child : node->children()) {
        if (nodeProducesGeometry(child.get())) return true;
    }
    return false;
}

void BlenderGenerator::classifyModuleGeometry(ASTNode* node) {
    if (!node) return;
    // First, recurse to find all module definitions
    for (auto& child : node->children()) {
        classifyModuleGeometry(child.get());
    }
    if (node->type() == ASTNode::Type::Module) {
        auto* mod = dynamic_cast<ModuleNode*>(node);
        if (mod) {
            // Initially mark all modules as non-geometric
            non_geometric_modules_.insert(mod->name());
        }
    }
}


void BlenderGenerator::collectGroupInputVars(ASTNode& node) {
    for (auto& child : node.children()) {
        if (!child) continue;
        if (child->type() == ASTNode::Type::Assignment) {
            auto* assign = dynamic_cast<AssignmentNode*>(child.get());
            if (assign) {
                // Only promote to group_input if this is a top-level variable.
                // Module-internal variables (like length, cutoff_size) should NOT
                // become group_input sockets.
                if (top_level_vars_.find(assign->name()) == top_level_vars_.end()) {
                    // Skip module-internal variables
                } else {
                    const Value& val = assign->value();
                    if (val.isNumber() || val.isBool() || val.isString()) {
                        group_input_vars_.insert(assign->name());
                    } else if (val.isVector() && val.size() == 3) {
                        bool allConcrete = true;
                        for (size_t i = 0; i < val.size(); ++i) {
                            if (val[i].isExpression()) { allConcrete = false; break; }
                        }
                        if (allConcrete) {
                            group_input_vars_.insert(assign->name());
                        }
                    }
                }
            }
        }
        // Recurse into children (container unions, etc.)
        collectGroupInputVars(*child);
    }
}

bool BlenderGenerator::moduleUsesChildren(ASTNode& node) {

    // Check if any descendant is a ChildrenNode
    for (auto& child : node.children()) {
        if (!child) continue;
        if (child->type() == ASTNode::Type::Children) {
            return true;
        }
        if (moduleUsesChildren(*child)) {
            return true;
        }
    }
    return false;
}

} // namespace scad2blender
