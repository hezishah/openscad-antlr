/**
 * @file blender_generator.cpp
 * @brief Blender Geometry Nodes Python code generator
 */

#include "blender_generator.h"
#include <cmath>
#include <algorithm>

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
    in_module_ = false;
    module_uses_children_ = false;

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
        // For each called module, promote its direct constant assignments.
        // The module body may be wrapped in an implicit Union, so recurse
        // into container nodes (Union/Difference/Intersection) to find assignments.
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
                    if (isSimple && !val.isExpression()) {
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

    // Emit module functions (recursively, to emit nested modules too)
    for (auto& child : root->children()) {
        if (!child) continue;
        emitModulesRecursive(child.get());
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
        // emit a Python expression using runtime variable names
        if (exprTreeReferencesModuleParams(tree)) {
            std::string pyExpr = exprTreeToPython(tree);
            emit(nodeId + ".inputs['" + inputName + "'].default_value = " + pyExpr);
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
            // Runtime Python variable — use the Python variable name
            emit(nodeId + ".inputs['" + inputName + "'].default_value = " + pyName(varName));
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
    current_module_params_.clear();
    for (const auto& p : node.parameters()) {
        current_module_params_.insert(pyName(p));
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
    params += ", children_geo=None";

    emit("def module_" + node.name() + "(" + params + "):");
    indent_++;
    emit("\"\"\"OpenSCAD module: " + node.name() + "\"\"\"");
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
    emit("return last_geo, y_pos");
    indent_--;
    emitBlank();

    // Restore variables
    variables_ = saved_variables;
    in_module_ = false;
    module_uses_children_ = false;
    current_module_params_.clear();
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
        for (const auto& [name, value] : variables_) {
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
                emit("add_input_socket(node_group, '" + socketName + "', 'NodeSocketFloat', " + value.toPython() + ")");
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
        emit("# Python variables for group_input parameters");
        for (const auto& [name, value] : variables_) {
            // Only process top-level variables, not module-internal ones
            if (top_level_vars_.find(name) == top_level_vars_.end()) continue;

            std::string varName = pyName(name);
            // Only emit for types that got add_input_socket calls
            if (value.isNumber() || value.isBool() || value.isString()) {
                emit(varName + " = " + value.toPython());
            } else if (value.isVector() && value.size() == 3) {
                // Evaluate each component numerically to avoid expression references
                std::string vecVal = "(";
                for (size_t i = 0; i < 3; ++i) {
                    if (i > 0) vecVal += ", ";
                    vecVal += pyDouble(value[i].toNumber());
                }
                vecVal += ")";
                emit(varName + " = " + vecVal);
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
    emit("bpy.ops.wm.stl_export(filepath=stl_path, export_selected_objects=True)");
    indent_--;
    emit("except AttributeError:");
    indent_++;
    emit("bpy.ops.export_mesh.stl(filepath=stl_path, use_selection=True)");
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
        case ASTNode::Type::Hull:
            processChildren(node);
            emitHull(node.args());
            break;
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

        // Add arguments from the call (both positional and named)
        for (const auto& arg : node.args()) {
            std::string paramName = arg.first;
            if (paramName[0] == '_') {
                // Positional arg: map _0, _1, ... to module parameter names
                int idx = std::stoi(paramName.substr(1));
                if (idx >= 0 && idx < static_cast<int>(modParams.size())) {
                    paramName = modParams[idx];
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

        if (!node.children().empty()) {
            argStr += ", children_geo=children_geo_tmp";
        } else {
            argStr += ", children_geo=last_geo";
        }

        emit(nodeId + ", y_pos = module_" + node.name() + "(" + argStr + ")");
        emit("last_geo = " + nodeId);
        emit("x_pos += 200");
    } else {
        emit("# Unknown module: " + node.name());
    }
}

void BlenderGenerator::visit(ForLoopNode& node) {
    const Value& range = node.range();

    if (range.isRange()) {
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
        emit("# For loop over vector: " + node.variable());
        std::string loopVar = node.variable();

        // Track loop variable
        loop_variables_.insert(loopVar);

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
        // Runtime condition - emit both branches
        emit("# Runtime conditional - emitting if branch");
        for (auto& child : node.children()) {
            if (!child) continue;
            child->accept(*this);
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
            // Evaluate vector components numerically to avoid expression references
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
                    auto result = emitExpressionNodeTree(halfTree);
                    connectExprResultNamed(result, combineId, components[i]);
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
    Value v = getArg(args, "v", Value());

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
        if (aTree->hasVariableRefs()) {
            if (exprTreeReferencesModuleParams(aTree)) {
                // Runtime Python variables — emit as Python expression
                std::string pyExpr = exprTreeToPython(aTree);
                emit(nodeId + ".inputs['Rotation'].default_value = (0, 0, math.radians(" + pyExpr + "))");
            } else {
                // Build: a * PI / 180
                ExprNodePtr radTree = ExprNode::makeBinary(
                    ExprNode::Op::MULTIPLY, aTree,
                    ExprNode::makeLiteral(M_PI / 180.0));
                // Emit as Z component (default rotation axis)
                emitScalarToVectorInput(nodeId, "Rotation", radTree, 2, 0.0, 0.0, 0.0);
            }
        } else {
            double angle = a.toNumber() * M_PI / 180.0;
            if (v.isVector() && v.size() >= 3) {
                // Axis-angle rotation
                emit("# Axis-angle rotation around " + v.repr());
                emit(nodeId + ".inputs['Rotation'].default_value = (0, 0, " + pyDouble(angle) + ")");
            } else {
                // Z-axis rotation by default
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
}

void BlenderGenerator::emitOffset(const Arguments& args) {
    std::string nodeId = newNodeId();

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
        // r mode: use FilletCurve to round corners, then offset outward.
        // Default BEZIER mode creates smooth circular arcs at each corner.
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

        // Try to set POLY mode with $fn-controlled count for corner resolution.
        // Falls back to default BEZIER mode (smooth Bézier arcs) if unavailable.
        int count = fnVal / 4;
        if (count < 1) count = 1;
        emit("try:");
        indent_++;
        emit(filletId + ".mode = 'POLY'");
        emit(filletId + ".inputs['Count'].default_value = " + std::to_string(count));
        indent_--;
        emit("except:");
        indent_++;
        emit("pass  # BEZIER mode (default) creates smooth arcs");
        indent_--;

        emit("link_nodes(links, last_geo, 'Curve', " + filletId + ", 'Curve')");
        emit("last_geo = " + filletId);
        emit("x_pos += 200");
    } else {
        // delta mode: subdivide straight edges for smooth offset without corner rounding.
        std::string subdivId = newNodeId();
        emit(subdivId + " = nodes.new('GeometryNodeSubdivideCurve')");
        emit(subdivId + ".location = (x_pos, y_pos)");

        // Use $fn/4 cuts per edge for consistent resolution
        int cuts = fnVal / 4;
        if (cuts < 1) cuts = 1;
        emit(subdivId + ".inputs['Cuts'].default_value = " + std::to_string(cuts));
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
        // Check if this is a container Union (marked with debug flag)
        if (child->type() == ASTNode::Type::Union && child->isDebug()) {
            // Unpack the container's children
            for (auto& grandchild : child->children()) {
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

    // Use a unique counter for this boolean scope to avoid variable name collisions
    int boolScopeId = node_counter_++;

    emit("# " + opName);

    // Helper: check if a node outputs curve geometry (2D)
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
    std::string firstGeo = "bool_first_" + std::to_string(boolScopeId);
    emit(firstGeo + " = None");

    actualChildren[0]->accept(*this);

    if (blenderOp == "DIFFERENCE") {
        // For DIFFERENCE, check at runtime if operands are 2D curves.
        // If so, use curve-join-with-reverse to create a multi-spline curve
        // with holes, which FillCurve handles correctly (proper inner walls).
        // Otherwise fall back to mesh boolean.
        emit(firstGeo + " = last_geo");

        for (size_t i = 1; i < actualChildren.size(); ++i) {
            actualChildren[i]->accept(*this);

            // Skip boolean if child produced no geometry (e.g. false conditional)
            emit("if last_geo is not None and last_geo is not " + firstGeo + ":");
            indent_++;

            // If first geo is None (e.g. conditional produced nothing), adopt this child
            emit("if " + firstGeo + " is None:");
            indent_++;
            emit(firstGeo + " = last_geo");
            indent_--;
            emit("else:");
            indent_++;

            // Runtime check: if both first and current are curves, use curve difference
            emit("if " + isCurveHelper + "(" + firstGeo + ") and " + isCurveHelper + "(last_geo):");
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
            emit("else:");
            indent_++;
            // 3D path: use mesh boolean
            emit(firstGeo + " = " + fillHelper + "(" + firstGeo + ")");
            emit("last_geo = " + fillHelper + "(last_geo)");
            std::string boolId = newNodeId();
            emit(boolId + " = nodes.new('GeometryNodeMeshBoolean')");
            emit(boolId + ".location = (x_pos, y_pos)");
            emit(boolId + ".operation = 'DIFFERENCE'");
            emit("links.new(" + firstGeo + ".outputs[0], " + boolId + ".inputs[0])");
            emit("links.new(last_geo.outputs[0], " + boolId + ".inputs[1])");
            emit(firstGeo + " = " + boolId);
            emit("x_pos += 200");
            indent_--;
            indent_--;  // end else (firstGeo not None)
            indent_--;  // end if last_geo is not None
            emit("y_pos -= 50");
        }
    } else {
        // UNION and INTERSECT: always use mesh boolean
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

            // In Blender 5.1+, UNION/INTERSECT use inputs[1] as multi-input
            // (inputs[0] is disabled). Both operands go to inputs[1].
            emit("links.new(" + firstGeo + ".outputs[0], " + boolId + ".inputs[1])");
            emit("links.new(last_geo.outputs[0], " + boolId + ".inputs[1])");
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
    std::string nodeId = newNodeId();

    Value height = getArg(args, "height", getPositionalArg(args, 0, Value(1.0)));
    Value twist = getArg(args, "twist", Value(0.0));
    Value scale_val = getArg(args, "scale", Value(1.0));
    Value center = getArg(args, "center", Value(false));

    emit("# Linear Extrude");

    // Fill the curve to create a mesh face for extrusion.
    // We need two fills: one becomes the bottom cap (flipped normals),
    // one gets extruded to create sides + top.
    std::string fillId = newNodeId();
    emit(fillId + " = nodes.new('GeometryNodeFillCurve')");
    emit(fillId + ".location = (x_pos, y_pos)");
    emit("link_nodes(links, last_geo, 'Curve', " + fillId + ", 'Curve')");
    emit("x_pos += 200");

    // Bottom cap: fill the same curve again and flip normals to face downward
    std::string fillBottomId = newNodeId();
    emit(fillBottomId + " = nodes.new('GeometryNodeFillCurve')");
    emit(fillBottomId + ".location = (x_pos, y_pos - 200)");
    emit("link_nodes(links, last_geo, 'Curve', " + fillBottomId + ", 'Curve')");
    std::string flipId = newNodeId();
    emit(flipId + " = nodes.new('GeometryNodeFlipFaces')");
    emit(flipId + ".location = (x_pos, y_pos - 200)");
    emit("links.new(" + fillBottomId + ".outputs['Mesh'], " + flipId + ".inputs['Mesh'])");

    // Extrude with Individual=False for proper solid extrusion
    emit(nodeId + " = nodes.new('GeometryNodeExtrudeMesh')");
    emit(nodeId + ".location = (x_pos, y_pos)");
    emit(nodeId + ".inputs['Individual'].default_value = False");

    // Set offset direction (0,0,1) and use Offset Scale for height
    emit(nodeId + ".inputs['Offset'].default_value = (0, 0, 1)");
    ExprNodePtr heightTree = getOrMakeLiteralTree(height);
    if (heightTree->hasVariableRefs() && exprTreeHasOnlyGroupInputVars(heightTree)) {
        auto result = emitExpressionNodeTree(heightTree);
        connectExprResultNamed(result, nodeId, "Offset Scale");
    } else if (in_module_ && heightTree->hasVariableRefs() && exprTreeReferencesModuleParams(heightTree)) {
        emit(nodeId + ".inputs['Offset Scale'].default_value = " +
             exprTreeToPython(heightTree));
    } else {
        double hVal = height.isExpression() ? evaluateExpr(height) : height.toNumber();
        emit(nodeId + ".inputs['Offset Scale'].default_value = " +
             std::to_string(hVal));
    }
    emit("link_nodes(links, " + fillId + ", 'Mesh', " + nodeId + ", 'Mesh')");
    emit("x_pos += 200");

    // Track the last node from the extrude chain (may be ScaleElements or ExtrudeMesh)
    std::string extrudeLastId = nodeId;

    // Handle scale parameter — scale the top face
    double scaleNum = scale_val.isExpression() ? evaluateExpr(scale_val) : scale_val.toNumber();
    if (scaleNum != 1.0) {
        std::string scaleId = newNodeId();
        emit("# Scale top face for linear_extrude(scale=" + std::to_string(scaleNum) + ")");
        emit(scaleId + " = nodes.new('GeometryNodeScaleElements')");
        emit(scaleId + ".location = (x_pos, y_pos)");
        emit(scaleId + ".inputs['Scale'].default_value = " + std::to_string(scaleNum));
        emit("links.new(" + nodeId + ".outputs['Mesh'], " + scaleId + ".inputs['Geometry'])");
        emit("links.new(" + nodeId + ".outputs['Top'], " + scaleId + ".inputs['Selection'])");
        extrudeLastId = scaleId;
        emit("x_pos += 200");
    }

    // Join bottom cap with extruded mesh and merge overlapping vertices
    std::string joinId = newNodeId();
    emit(joinId + " = nodes.new('GeometryNodeJoinGeometry')");
    emit(joinId + ".location = (x_pos, y_pos)");
    emit("links.new(" + flipId + ".outputs['Mesh'], " + joinId + ".inputs['Geometry'])");
    emit("link_nodes(links, " + extrudeLastId + ", 'Geometry', " + joinId + ", 'Geometry')");
    std::string mergeId = newNodeId();
    emit(mergeId + " = nodes.new('GeometryNodeMergeByDistance')");
    emit(mergeId + ".location = (x_pos, y_pos)");
    emit(mergeId + ".inputs['Distance'].default_value = 0.001");
    emit("links.new(" + joinId + ".outputs['Geometry'], " + mergeId + ".inputs['Geometry'])");
    emit("last_geo = " + mergeId);
    emit("x_pos += 200");
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
        // Inline user-defined functions
        auto fit = functions_.find(tree->func_name);
        if (fit != functions_.end() && fit->second.body) {
            const FuncDef& fd = fit->second;
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
            // Runtime Python variables (module params, loop vars) stay as variable references
            if (isRuntimePythonVar(tree->var_name)) {
                return pyName(tree->var_name);
            }
            // If the variable can be resolved to a concrete value, use that
            auto it = variables_.find(tree->var_name);
            if (it != variables_.end() && it->second.isNumber()) {
                return pyDouble(it->second.toNumber());
            }
            // Otherwise use the Python variable name
            return pyName(tree->var_name);
        }
        case ExprNode::Kind::UnaryOp:
            if (tree->op == ExprNode::Op::NEGATE)
                return "(-" + exprTreeToPython(tree->left) + ")";
            return exprTreeToPython(tree->left);
        case ExprNode::Kind::BinaryOp: {
            std::string l = exprTreeToPython(tree->left);
            std::string r = exprTreeToPython(tree->right);
            switch (tree->op) {
                case ExprNode::Op::ADD: return "(" + l + " + " + r + ")";
                case ExprNode::Op::SUBTRACT: return "(" + l + " - " + r + ")";
                case ExprNode::Op::MULTIPLY: return "(" + l + " * " + r + ")";
                case ExprNode::Op::DIVIDE: return "(" + l + " / " + r + ")";
                case ExprNode::Op::MODULO: return "(" + l + " % " + r + ")";
                case ExprNode::Op::POWER: return "(" + l + " ** " + r + ")";
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
                    // Build substitution map: formal param name → actual arg tree
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
            std::string argsStr;
            for (size_t i = 0; i < tree->func_args.size(); i++) {
                if (i > 0) argsStr += ", ";
                argsStr += exprTreeToPython(tree->func_args[i]);
            }
            return fn + "(" + argsStr + ")";
        }
        case ExprNode::Kind::VectorLiteral:
            return "0";
        case ExprNode::Kind::Conditional:
        case ExprNode::Kind::ForLoop:
        case ExprNode::Kind::LetBinding:
            return "0";
    }
    return "0";
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
        }
    }
    for (auto& child : node->children()) {
        collectModulesRecursive(child.get());
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
