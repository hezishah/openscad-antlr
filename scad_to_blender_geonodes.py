#!/usr/bin/env python3
"""
OpenSCAD to Blender Geometry Nodes Converter
Parse .scad files using ANTLR and generate Blender Geometry Nodes Python code
"""

import sys
import json
from antlr4 import *
from scad_py.scadLexer import scadLexer
from scad_py.scadParser import scadParser
from scad_py.scadListener import scadListener


class ASTNode:
    """Base class for AST nodes"""
    def __init__(self, node_type):
        self.node_type = node_type
        self.children = []

    def to_dict(self):
        return {
            'type': self.node_type,
            'children': [c.to_dict() if isinstance(c, ASTNode) else c for c in self.children]
        }


class ModuleNode(ASTNode):
    def __init__(self, name, parameters=None):
        super().__init__('module')
        self.name = name
        self.parameters = parameters or []

    def to_dict(self):
        return {
            'type': self.node_type,
            'name': self.name,
            'parameters': self.parameters,
            'children': [c.to_dict() if isinstance(c, ASTNode) else c for c in self.children]
        }


class PrimitiveNode(ASTNode):
    def __init__(self, primitive_type, args=None):
        super().__init__('primitive')
        self.primitive_type = primitive_type
        self.args = args or {}

    def to_dict(self):
        return {
            'type': self.node_type,
            'primitive_type': self.primitive_type,
            'args': self.args,
            'children': [c.to_dict() if isinstance(c, ASTNode) else c for c in self.children]
        }


class TransformNode(ASTNode):
    def __init__(self, transform_type, params=None):
        super().__init__('transform')
        self.transform_type = transform_type
        self.params = params or []

    def to_dict(self):
        return {
            'type': self.node_type,
            'transform_type': self.transform_type,
            'params': self.params,
            'children': [c.to_dict() if isinstance(c, ASTNode) else c for c in self.children]
        }


class ExtrudeNode(ASTNode):
    """Node for linear_extrude and rotate_extrude operations"""
    def __init__(self, extrude_type, args=None):
        super().__init__('extrude')
        self.extrude_type = extrude_type
        self.args = args or {}

    def to_dict(self):
        return {
            'type': self.node_type,
            'extrude_type': self.extrude_type,
            'args': self.args,
            'children': [c.to_dict() if isinstance(c, ASTNode) else c for c in self.children]
        }


class BooleanOpNode(ASTNode):
    def __init__(self, op_type):
        super().__init__('boolean_op')
        self.op_type = op_type

    def to_dict(self):
        return {
            'type': self.node_type,
            'op_type': self.op_type,
            'children': [c.to_dict() if isinstance(c, ASTNode) else c for c in self.children]
        }


class AssignmentNode(ASTNode):
    def __init__(self, var_name, value):
        super().__init__('assignment')
        self.var_name = var_name
        self.value = value

    def to_dict(self):
        return {
            'type': self.node_type,
            'var_name': self.var_name,
            'value': self.value
        }


class SCADtoASTBuilder(scadListener):
    """
    Build a structured AST from OpenSCAD parse tree
    """
    def __init__(self):
        self.ast_stack = []
        self.current_module = None
        self.variables = {}
        self.modules = []
        self._handled_modules = set()  # track module ctx ids we handle via single_module_instantiation

    def enterModule(self, ctx: scadParser.ModuleContext):
        text = ctx.getText()

        # Handle module declaration (e.g. "module foo(...) { ... }")
        if text.startswith('module'):
            module_name = ctx.children[1].getText()
            params = ctx.children[3].getText() if len(ctx.children) > 3 else ""
            self.current_module = ModuleNode(module_name, params)
            self.ast_stack.append(self.current_module)

        # Module instantiations are handled by enterSingle_module_instantiation

    def enterSingle_module_instantiation(self, ctx: scadParser.Single_module_instantiationContext):
        """Handle a single module instantiation like cube(10), translate([1,2,3]), etc.

        The grammar rule is:
            single_module_instantiation
                : module_id '(' arguments ')'
                | single_module_instantiation single_module_instantiation

        For chained calls like rotate([0,90,0]) holeObject(), ANTLR builds
        a left-recursive tree.  We extract only the module_id and arguments
        from THIS node, ignoring any chained child instantiations (which will
        get their own enterSingle_module_instantiation call).
        """
        # Get module name from the module_id child
        module_id_ctx = ctx.module_id()
        if module_id_ctx is None:
            # This is the chaining rule (single_module_instantiation single_module_instantiation)
            # The children will be handled by their own enter calls
            return

        module_name = module_id_ctx.getText()

        # Get arguments text from the arguments child
        args_ctx = ctx.arguments()
        args_text = args_ctx.getText() if args_ctx else ""

        # Create the appropriate AST node
        if module_name in ['cube', 'sphere', 'cylinder', 'polyhedron',
                           'circle', 'square', 'polygon', 'text',
                           'surface', 'import', 'projection']:
            node = PrimitiveNode(module_name, self._parse_args(args_text))
            self.ast_stack.append(node)
        elif module_name in ['translate', 'rotate', 'scale', 'mirror',
                             'color', 'offset', 'hull', 'minkowski',
                             'resize', 'multmatrix']:
            node = TransformNode(module_name, self._parse_params(args_text))
            self.ast_stack.append(node)
        elif module_name in ['linear_extrude', 'rotate_extrude']:
            node = ExtrudeNode(module_name, self._parse_args(args_text))
            self.ast_stack.append(node)
        elif module_name in ['difference', 'union', 'intersection']:
            node = BooleanOpNode(module_name)
            self.ast_stack.append(node)
        else:
            # Custom module call
            node = PrimitiveNode(module_name, self._parse_args(args_text))
            self.ast_stack.append(node)

        # Mark the parent module context as handled so exitModule
        # knows this instantiation created a node
        parent = ctx.parentCtx
        while parent and not isinstance(parent, scadParser.ModuleContext):
            parent = parent.parentCtx
        if parent:
            self._handled_modules.add(id(parent))

    def exitSingle_module_instantiation(self, ctx: scadParser.Single_module_instantiationContext):
        """Pop chained inner instantiations and attach as child of the outer one.

        For a chain like `rotate(...) cube(...)`, the grammar is:
            single_module_instantiation  (chain)
              single_module_instantiation  (rotate — has module_id)
              single_module_instantiation  (cube — has module_id)

        Both rotate and cube are pushed.  When we exit cube's context, its
        parent is the chain context (also a single_module_instantiation).
        When we exit rotate, its parent is also the chain.  In that case
        we pop and attach as a child of whatever is on the stack.

        But when the single_module_instantiation is the direct child of
        a `module` rule, exitModule handles the pop.
        """
        module_id_ctx = ctx.module_id()
        if module_id_ctx is None:
            # Chaining rule — nothing was pushed
            return

        # If our parent is another single_module_instantiation (chaining),
        # pop now and attach as child of whatever is on the stack.
        parent = ctx.parentCtx
        if isinstance(parent, scadParser.Single_module_instantiationContext):
            if self.ast_stack:
                node = self.ast_stack.pop()
                if self.ast_stack:
                    self.ast_stack[-1].children.append(node)
                else:
                    self.modules.append(node)
        # Otherwise, exitModule will handle the pop

    def exitModule(self, ctx: scadParser.ModuleContext):
        """Pop the node pushed by either enterModule (module declaration)
        or enterSingle_module_instantiation (module instantiation)."""
        if self.ast_stack:
            node = self.ast_stack.pop()
            if self.ast_stack:
                self.ast_stack[-1].children.append(node)
            else:
                self.modules.append(node)

    def exitAssignment(self, ctx: scadParser.AssignmentContext):
        var_name = ctx.children[0].getText()
        # The expression is at index 2 (after '=')
        value = ctx.children[2].getText()

        # Convert OpenSCAD expression syntax to Python
        value = _scad_expr_to_python(value)

        # Convert $fn/$fa/$fs to valid Python identifiers
        var_name = _sanitize_var_name(var_name)

        self.variables[var_name] = value

        # Add to current module if inside one
        if self.ast_stack:
            assignment = AssignmentNode(var_name, value)
            self.ast_stack[-1].children.append(assignment)

    def _parse_args(self, args_text):
        """Parse argument string into dictionary, handling nested arrays"""
        args = {}
        if not args_text:
            return args

        # Split by commas, but respect brackets
        parts = []
        current_part = ""
        bracket_depth = 0

        for char in args_text:
            if char == '[':
                bracket_depth += 1
            elif char == ']':
                bracket_depth -= 1
            elif char == ',' and bracket_depth == 0:
                if current_part.strip():
                    parts.append(current_part.strip())
                current_part = ""
                continue
            current_part += char

        if current_part.strip():
            parts.append(current_part.strip())

        # Process parts
        for part in parts:
            if '=' in part:
                key, val = part.split('=', 1)
                key = key.strip()
                val = val.strip()
                # Don't convert values inside brackets (vectors) here —
                # they'll be converted when parse_vector_string splits them.
                if not val.startswith('['):
                    val = _scad_expr_to_python(val)
                args[key] = val
            else:
                # Positional argument
                val = part.strip()
                if not val.startswith('['):
                    val = _scad_expr_to_python(val)
                args[f'_pos_{len(args)}'] = val
        return args

    def _parse_params(self, params_text):
        """Parse parameters (arrays, vectors, etc.)"""
        # For vectors like [x,y,z] we leave them as-is since
        # parse_vector_string + _scad_expr_to_python handles components.
        # For single values, convert now.
        if params_text and not params_text.startswith('['):
            return _scad_expr_to_python(params_text)
        return params_text


def generate_blender_geonodes(ast_modules, variables):
    """
    Generate Blender Python code that creates a single Geometry Nodes graph from AST.

    All top-level operations are placed in one node group.
    SCAD module definitions become sub-groups (GeometryNodeGroup) that can be
    instantiated from the main graph.
    """
    code_lines = [
        "import bpy",
        "import math",
        "",
        "# Clear existing geometry nodes",
        "def clear_geometry_nodes():",
        "    for obj in bpy.data.objects:",
        "        if obj.type == 'MESH':",
        "            bpy.data.objects.remove(obj, do_unlink=True)",
        "    for ng in list(bpy.data.node_groups):",
        "        bpy.data.node_groups.remove(ng)",
        "",
        "def add_geometry_socket(node_group, name, in_out):",
        "    \"\"\"Add a Geometry socket to a node group, compatible with Blender 3.x and 4.x.\"\"\"",
        "    if hasattr(node_group, 'interface'):",
        "        node_group.interface.new_socket(name=name, in_out=in_out, socket_type='NodeSocketGeometry')",
        "    else:",
        "        if in_out == 'OUTPUT':",
        "            node_group.outputs.new('NodeSocketGeometry', name)",
        "        else:",
        "            node_group.inputs.new('NodeSocketGeometry', name)",
        "",
        "def create_subgroup(name):",
        "    \"\"\"Create a node sub-group (for SCAD module definitions).\"\"\"",
        "    ng = bpy.data.node_groups.new(name, 'GeometryNodeTree')",
        "    nodes = ng.nodes",
        "    gi = nodes.new('NodeGroupInput')",
        "    go = nodes.new('NodeGroupOutput')",
        "    add_geometry_socket(ng, 'Geometry', 'OUTPUT')",
        "    gi.location = (-400, 0)",
        "    go.location = (800, 0)",
        "    return ng, nodes, gi, go",
        "",
    ]

    # Add variables
    code_lines.append("# Variables from SCAD file")
    for var_name, value in variables.items():
        code_lines.append(f"{var_name} = {value}")
    code_lines.append("")

    # Separate module definitions from top-level operations
    module_defs = [m for m in ast_modules if isinstance(m, ModuleNode)]
    top_level_ops = [m for m in ast_modules if not isinstance(m, ModuleNode)]

    # Collect defined module names so we can resolve custom module calls
    defined_modules = {m.name for m in module_defs}

    # --- Generate sub-group creation functions for each SCAD module ---
    for mod in module_defs:
        code_lines.extend(generate_subgroup_code(mod, variables, defined_modules))

    # --- Generate the main build function ---
    code_lines.append("def build_geometry():")
    code_lines.append("    # Create mesh object")
    code_lines.append("    mesh = bpy.data.meshes.new('OpenSCAD')")
    code_lines.append("    obj = bpy.data.objects.new('OpenSCAD', mesh)")
    code_lines.append("    bpy.context.collection.objects.link(obj)")
    code_lines.append("    ")
    code_lines.append("    # Add Geometry Nodes modifier")
    code_lines.append("    modifier = obj.modifiers.new(name='GeometryNodes', type='NODES')")
    code_lines.append("    node_group = bpy.data.node_groups.new('OpenSCAD', 'GeometryNodeTree')")
    code_lines.append("    modifier.node_group = node_group")
    code_lines.append("    ")
    code_lines.append("    # Main node group I/O")
    code_lines.append("    nodes = node_group.nodes")
    code_lines.append("    links = node_group.links")
    code_lines.append("    group_input = nodes.new('NodeGroupInput')")
    code_lines.append("    group_output = nodes.new('NodeGroupOutput')")
    code_lines.append("    add_geometry_socket(node_group, 'Geometry', 'OUTPUT')")
    code_lines.append("    group_input.location = (-400, 0)")
    code_lines.append("    group_output.location = (800, 0)")
    code_lines.append("    ")

    # First, create all sub-groups so they exist for node-group references
    if module_defs:
        code_lines.append("    # Build SCAD module sub-groups")
        for mod in module_defs:
            code_lines.append(f"    subgroup_{mod.name} = create_{mod.name}_group()")
        code_lines.append("    ")

    # Generate code for all top-level operations inside the main node group
    node_counter = [0]
    child_outputs = []

    for op in top_level_ops:
        op_code, output = generate_node_code(op, variables, indent=1,
                                             node_counter=node_counter,
                                             defined_modules=defined_modules)
        code_lines.extend(op_code)
        if output:
            child_outputs.append(output)

    # Join all top-level outputs together and connect to group_output
    ind = "    "
    if len(child_outputs) > 1:
        join_id = f"node_{node_counter[0]}"
        node_counter[0] += 1
        code_lines.append(f"{ind}# Join all top-level geometry")
        code_lines.append(f"{ind}{join_id} = nodes.new('GeometryNodeJoinGeometry')")
        for co in child_outputs:
            code_lines.append(f"{ind}links.new({co}, {join_id}.inputs['Geometry'])")
        code_lines.append(f"{ind}links.new({join_id}.outputs['Geometry'], group_output.inputs['Geometry'])")
    elif len(child_outputs) == 1:
        _connect_to_group_output(code_lines, child_outputs[0], indent=1, node_counter=node_counter)
    code_lines.append("")

    # Main execution
    code_lines.append("")
    code_lines.append("# Main execution")
    code_lines.append("if __name__ == '__main__':")
    code_lines.append("    clear_geometry_nodes()")
    code_lines.append("    build_geometry()")

    return "\n".join(code_lines)


def _connect_to_group_output(lines, output_socket_str, indent, node_counter):
    """Connect an output socket to group_output.

    Blender Geometry Nodes group outputs expect a Geometry socket.
    Nodes that output 'Geometry' can link directly.  Nodes that output
    'Mesh', 'Convex Hull', 'Curve', etc. must first pass through a
    Join Geometry node which accepts any geometry sub-type and produces
    a proper Geometry output."""
    ind = "    " * indent
    if output_socket_str.endswith("outputs['Geometry']"):
        lines.append(f"{ind}# Connect to output")
        lines.append(f"{ind}links.new({output_socket_str}, group_output.inputs['Geometry'])")
    else:
        conv_id = f"node_{node_counter[0]}"
        node_counter[0] += 1
        lines.append(f"{ind}# Route through Join Geometry so the output type is Geometry")
        lines.append(f"{ind}{conv_id} = nodes.new('GeometryNodeJoinGeometry')")
        lines.append(f"{ind}links.new({output_socket_str}, {conv_id}.inputs['Geometry'])")
        lines.append(f"{ind}links.new({conv_id}.outputs['Geometry'], group_output.inputs['Geometry'])")


def generate_subgroup_code(module, variables, defined_modules):
    """Generate a function that creates a Geometry Nodes sub-group for a SCAD module."""
    lines = []
    func_name = f"create_{module.name}_group"
    lines.append(f"def {func_name}():")
    lines.append(f"    ng, nodes, group_input, group_output = create_subgroup('{module.name}')")
    lines.append(f"    links = ng.links")
    lines.append(f"    ")

    # Process children
    node_counter = [0]
    last_output = None

    for child in module.children:
        child_code, output = generate_node_code(child, variables, indent=1,
                                                node_counter=node_counter,
                                                defined_modules=defined_modules)
        lines.extend(child_code)
        if output:
            last_output = output

    # Connect final output to group output
    if last_output:
        _connect_to_group_output(lines, last_output, indent=1, node_counter=node_counter)

    lines.append(f"    return ng")
    lines.append("")
    return lines


def _sanitize_var_name(name):
    """Convert OpenSCAD variable names to valid Python identifiers.

    $fn → _fn, $fa → _fa, $fs → _fs, etc.
    """
    if name.startswith('$'):
        return '_' + name[1:]
    return name


def _scad_expr_to_python(expr):
    """Convert an OpenSCAD expression string to valid Python syntax.

    Handles:
      - Ternary:  cond ? a : b  →  (a if cond else b)
      - Booleans: true / false  →  True / False
      - Undef:    undef         →  None
    """
    if not expr:
        return expr

    s = expr.strip()

    # Convert OpenSCAD ternary  cond ? val_true : val_false
    # We need to handle nested ternaries so we find the *first* top-level '?'
    # that isn't inside brackets/parens.
    depth = 0
    q_pos = -1
    for i, ch in enumerate(s):
        if ch in '([':
            depth += 1
        elif ch in ')]':
            depth -= 1
        elif ch == '?' and depth == 0:
            q_pos = i
            break

    if q_pos != -1:
        cond = s[:q_pos].strip()
        rest = s[q_pos + 1:]
        # Find the matching ':' at depth 0
        depth = 0
        c_pos = -1
        for i, ch in enumerate(rest):
            if ch in '([':
                depth += 1
            elif ch in ')]':
                depth -= 1
            elif ch == ':' and depth == 0:
                c_pos = i
                break
        if c_pos != -1:
            val_true = rest[:c_pos].strip()
            val_false = rest[c_pos + 1:].strip()
            # Recursively convert sub-expressions
            cond = _scad_expr_to_python(cond)
            val_true = _scad_expr_to_python(val_true)
            val_false = _scad_expr_to_python(val_false)
            return f"({val_true} if {cond} else {val_false})"

    # Replace standalone boolean/undef keywords (word-boundary aware)
    import re
    s = re.sub(r'\btrue\b', 'True', s)
    s = re.sub(r'\bfalse\b', 'False', s)
    s = re.sub(r'\bundef\b', 'None', s)

    # Replace $fn/$fa/$fs with _fn/_fa/_fs
    s = re.sub(r'\$fn\b', '_fn', s)
    s = re.sub(r'\$fa\b', '_fa', s)
    s = re.sub(r'\$fs\b', '_fs', s)

    return s


def parse_vector_string(vector_str):
    """Parse a vector string like '[x,y,z]' into a tuple of values"""
    if not vector_str:
        return None

    # Remove brackets and split by comma
    if vector_str.startswith('[') and vector_str.endswith(']'):
        content = vector_str[1:-1]
        # Split respecting bracket nesting
        parts = []
        depth = 0
        current = []
        for ch in content:
            if ch in '([':
                depth += 1
                current.append(ch)
            elif ch in ')]':
                depth -= 1
                current.append(ch)
            elif ch == ',' and depth == 0:
                parts.append(''.join(current).strip())
                current = []
            else:
                current.append(ch)
        if current:
            parts.append(''.join(current).strip())
        # Convert each component to valid Python
        return tuple(_scad_expr_to_python(c) for c in parts)
    else:
        # Single value, return as single-element tuple
        return (_scad_expr_to_python(vector_str),)


def generate_vector_assignment(lines, node_id, input_name, vector_str, indent):
    """Generate code to assign vector components to default_value[0], [1], [2]"""
    ind = "    " * indent
    components = parse_vector_string(vector_str)

    if components:
        if len(components) == 1:
            # Single value
            lines.append(f"{ind}{node_id}.inputs['{input_name}'].default_value = {components[0]}")
        elif len(components) == 3:
            # Vector with 3 components - assign each individually
            lines.append(f"{ind}{node_id}.inputs['{input_name}'].default_value[0] = {components[0]}")
            lines.append(f"{ind}{node_id}.inputs['{input_name}'].default_value[1] = {components[1]}")
            lines.append(f"{ind}{node_id}.inputs['{input_name}'].default_value[2] = {components[2]}")
        else:
            # Fallback - try to assign as tuple
            lines.append(f"{ind}{node_id}.inputs['{input_name}'].default_value = ({', '.join(components)})")


def generate_node_code(node, variables, indent=0, node_counter=None, defined_modules=None):
    """Generate Blender node code for an AST node"""
    if node_counter is None:
        node_counter = [0]
    if defined_modules is None:
        defined_modules = set()

    ind = "    " * indent
    lines = []
    output_socket = None

    if isinstance(node, AssignmentNode):
        # Variables are already defined globally
        lines.append(f"{ind}# {node.var_name} = {node.value}")
        return lines, None

    elif isinstance(node, PrimitiveNode):
        node_id = f"node_{node_counter[0]}"
        node_counter[0] += 1

        if node.primitive_type == 'cube':
            lines.append(f"{ind}# Create Cube")
            lines.append(f"{ind}{node_id} = nodes.new('GeometryNodeMeshCube')")

            # Parse size from args
            size = node.args.get('size', node.args.get('_pos_0'))
            if size:
                lines.append(f"{ind}# Size: {size}")
                generate_vector_assignment(lines, node_id, 'Size', size, indent)

            # Handle center parameter (cube defaults to not centered in OpenSCAD)
            center = node.args.get('center', 'false')
            if center.lower() == 'true':
                lines.append(f"{ind}# Cube is centered (default in Blender Geo Nodes)")
            else:
                # Need to translate by half-size to match OpenSCAD's corner-origin default
                lines.append(f"{ind}# OpenSCAD cube is not centered by default")
                lines.append(f"{ind}# A Transform node may be needed to shift to corner origin")

            output_socket = f"{node_id}.outputs['Mesh']"

        elif node.primitive_type == 'sphere':
            lines.append(f"{ind}# Create Sphere")
            lines.append(f"{ind}{node_id} = nodes.new('GeometryNodeMeshUVSphere')")

            # 'r' = radius, 'd' = diameter
            if 'd' in node.args:
                lines.append(f"{ind}{node_id}.inputs['Radius'].default_value = ({node.args['d']}) / 2")
            elif 'r' in node.args or '_pos_0' in node.args:
                radius = node.args.get('r', node.args.get('_pos_0', '1'))
                lines.append(f"{ind}{node_id}.inputs['Radius'].default_value = {radius}")

            # $fn controls segment count
            fn = node.args.get('$fn')
            if fn:
                lines.append(f"{ind}{node_id}.inputs['Segments'].default_value = {fn}")
                lines.append(f"{ind}{node_id}.inputs['Rings'].default_value = max(2, {fn} // 2)")

            output_socket = f"{node_id}.outputs['Mesh']"

        elif node.primitive_type == 'cylinder':
            lines.append(f"{ind}# Create Cylinder")
            lines.append(f"{ind}{node_id} = nodes.new('GeometryNodeMeshCylinder')")

            # Height: 'h' or first positional arg (default 1)
            height = node.args.get('h', node.args.get('_pos_0', '1'))
            lines.append(f"{ind}{node_id}.inputs['Depth'].default_value = {height}")

            # Radius: 'r' (uniform), 'r1'/'r2' (top/bottom for cone), 'd'/'d1'/'d2' (diameters)
            if 'd' in node.args:
                lines.append(f"{ind}{node_id}.inputs['Radius Top'].default_value = ({node.args['d']}) / 2")
                lines.append(f"{ind}{node_id}.inputs['Radius Bottom'].default_value = ({node.args['d']}) / 2")
            elif 'd1' in node.args or 'd2' in node.args:
                d1 = node.args.get('d1', '2')
                d2 = node.args.get('d2', '2')
                lines.append(f"{ind}{node_id}.inputs['Radius Bottom'].default_value = ({d1}) / 2")
                lines.append(f"{ind}{node_id}.inputs['Radius Top'].default_value = ({d2}) / 2")
            elif 'r1' in node.args or 'r2' in node.args:
                r1 = node.args.get('r1', '1')
                r2 = node.args.get('r2', '1')
                lines.append(f"{ind}{node_id}.inputs['Radius Bottom'].default_value = {r1}")
                lines.append(f"{ind}{node_id}.inputs['Radius Top'].default_value = {r2}")
            elif 'r' in node.args:
                lines.append(f"{ind}{node_id}.inputs['Radius Top'].default_value = {node.args['r']}")
                lines.append(f"{ind}{node_id}.inputs['Radius Bottom'].default_value = {node.args['r']}")

            # center parameter
            center = node.args.get('center', 'false')
            if center.lower() == 'true':
                lines.append(f"{ind}# Cylinder is centered (default in Blender Geo Nodes)")

            # $fn controls vertices
            fn = node.args.get('$fn')
            if fn:
                lines.append(f"{ind}{node_id}.inputs['Vertices'].default_value = {fn}")

            output_socket = f"{node_id}.outputs['Mesh']"

        elif node.primitive_type == 'polyhedron':
            lines.append(f"{ind}# Create Polyhedron (custom mesh via script)")
            lines.append(f"{ind}# Polyhedron requires direct mesh creation outside Geometry Nodes")

            points_str = node.args.get('points', node.args.get('_pos_0', '[]'))
            faces_str = node.args.get('faces', node.args.get('triangles',
                                      node.args.get('_pos_1', '[]')))
            convexity = node.args.get('convexity', '1')

            lines.append(f"{ind}# Points: {points_str}")
            lines.append(f"{ind}# Faces: {faces_str}")
            lines.append(f"{ind}# Convexity: {convexity}")
            lines.append(f"{ind}# Creating polyhedron via bpy.data.meshes")
            lines.append(f"{ind}polyhedron_mesh = bpy.data.meshes.new('polyhedron_{node_id}')")
            lines.append(f"{ind}polyhedron_points = {points_str}")
            lines.append(f"{ind}polyhedron_faces = {faces_str}")
            lines.append(f"{ind}polyhedron_mesh.from_pydata(polyhedron_points, [], polyhedron_faces)")
            lines.append(f"{ind}polyhedron_mesh.update()")
            lines.append(f"{ind}# Use Object Info node to reference the polyhedron mesh object")
            lines.append(f"{ind}polyhedron_obj = bpy.data.objects.new('polyhedron_{node_id}', polyhedron_mesh)")
            lines.append(f"{ind}bpy.context.collection.objects.link(polyhedron_obj)")
            lines.append(f"{ind}{node_id} = nodes.new('GeometryNodeObjectInfo')")
            lines.append(f"{ind}{node_id}.inputs['Object'].default_value = polyhedron_obj")
            lines.append(f"{ind}{node_id}.transform_space = 'RELATIVE'")

            output_socket = f"{node_id}.outputs['Geometry']"

        elif node.primitive_type == 'circle':
            lines.append(f"{ind}# Create Circle (2D)")
            lines.append(f"{ind}{node_id} = nodes.new('GeometryNodeCurvePrimitiveCircle')")
            lines.append(f"{ind}{node_id}.mode = 'RADIUS'")

            # 'r' = radius, 'd' = diameter
            if 'd' in node.args:
                lines.append(f"{ind}{node_id}.inputs['Radius'].default_value = ({node.args['d']}) / 2")
            else:
                radius = node.args.get('r', node.args.get('_pos_0', '1'))
                lines.append(f"{ind}{node_id}.inputs['Radius'].default_value = {radius}")

            # $fn controls resolution
            fn = node.args.get('$fn')
            if fn:
                lines.append(f"{ind}{node_id}.inputs['Resolution'].default_value = {fn}")

            # Fill the circle to make it a face
            fill_id = f"node_{node_counter[0]}"
            node_counter[0] += 1
            lines.append(f"{ind}# Fill circle to create 2D face")
            lines.append(f"{ind}{fill_id} = nodes.new('GeometryNodeFillCurve')")
            lines.append(f"{ind}{fill_id}.mode = 'NGONS'")
            lines.append(f"{ind}links.new({node_id}.outputs['Curve'], {fill_id}.inputs['Curve'])")

            output_socket = f"{fill_id}.outputs['Mesh']"

        elif node.primitive_type == 'square':
            lines.append(f"{ind}# Create Square (2D)")

            # Parse size from args
            size = node.args.get('size', node.args.get('_pos_0', '[1,1]'))

            # Use a Grid node with 1x1 subdivisions to create a quad
            lines.append(f"{ind}{node_id} = nodes.new('GeometryNodeMeshGrid')")
            lines.append(f"{ind}{node_id}.inputs['Vertices X'].default_value = 2")
            lines.append(f"{ind}{node_id}.inputs['Vertices Y'].default_value = 2")

            # Set size
            components = parse_vector_string(size)
            if components and len(components) >= 2:
                lines.append(f"{ind}{node_id}.inputs['Size X'].default_value = {components[0]}")
                lines.append(f"{ind}{node_id}.inputs['Size Y'].default_value = {components[1]}")
            elif components and len(components) == 1:
                lines.append(f"{ind}{node_id}.inputs['Size X'].default_value = {components[0]}")
                lines.append(f"{ind}{node_id}.inputs['Size Y'].default_value = {components[0]}")

            # Handle center parameter (square defaults to not centered in OpenSCAD)
            center = node.args.get('center', 'false')
            if center.lower() == 'true':
                lines.append(f"{ind}# Square is centered")
            else:
                lines.append(f"{ind}# OpenSCAD square is not centered by default")

            output_socket = f"{node_id}.outputs['Mesh']"

        elif node.primitive_type == 'polygon':
            lines.append(f"{ind}# Create Polygon (2D)")

            points_str = node.args.get('points', node.args.get('_pos_0', '[]'))
            paths_str = node.args.get('paths', node.args.get('_pos_1', 'None'))
            convexity = node.args.get('convexity', '1')

            lines.append(f"{ind}# Points: {points_str}")
            lines.append(f"{ind}# Paths: {paths_str}")
            lines.append(f"{ind}# Convexity: {convexity}")

            # Create polygon mesh directly and reference via Object Info
            lines.append(f"{ind}polygon_points = {points_str}")
            lines.append(f"{ind}polygon_verts = [(p[0], p[1], 0) for p in polygon_points]")
            lines.append(f"{ind}if {paths_str} is not None:")
            lines.append(f"{ind}    polygon_faces = {paths_str}")
            lines.append(f"{ind}else:")
            lines.append(f"{ind}    polygon_faces = [list(range(len(polygon_verts)))]")
            lines.append(f"{ind}polygon_mesh = bpy.data.meshes.new('polygon_{node_id}')")
            lines.append(f"{ind}polygon_mesh.from_pydata(polygon_verts, [], polygon_faces)")
            lines.append(f"{ind}polygon_mesh.update()")
            lines.append(f"{ind}polygon_obj = bpy.data.objects.new('polygon_{node_id}', polygon_mesh)")
            lines.append(f"{ind}bpy.context.collection.objects.link(polygon_obj)")
            lines.append(f"{ind}{node_id} = nodes.new('GeometryNodeObjectInfo')")
            lines.append(f"{ind}{node_id}.inputs['Object'].default_value = polygon_obj")
            lines.append(f"{ind}{node_id}.transform_space = 'RELATIVE'")

            output_socket = f"{node_id}.outputs['Geometry']"

        elif node.primitive_type == 'text':
            lines.append(f"{ind}# Create Text")

            text_val = node.args.get('text', node.args.get('_pos_0', '"Text"'))
            size_val = node.args.get('size', '10')
            font_val = node.args.get('font', None)
            halign = node.args.get('halign', '"left"')
            valign = node.args.get('valign', '"baseline"')
            spacing = node.args.get('spacing', '1')

            lines.append(f"{ind}{node_id} = nodes.new('GeometryNodeStringToCurves')")
            lines.append(f"{ind}{node_id}.inputs['String'].default_value = {text_val}")
            lines.append(f"{ind}{node_id}.inputs['Size'].default_value = {size_val}")
            lines.append(f"{ind}{node_id}.inputs['Character Spacing'].default_value = {spacing}")

            # Alignment
            lines.append(f"{ind}# Horizontal alignment: {halign}")
            lines.append(f"{ind}# Vertical alignment: {valign}")
            if halign.strip('"\'') == 'center':
                lines.append(f"{ind}{node_id}.align_x = 'CENTER'")
            elif halign.strip('"\'') == 'right':
                lines.append(f"{ind}{node_id}.align_x = 'RIGHT'")
            else:
                lines.append(f"{ind}{node_id}.align_x = 'LEFT'")

            if valign.strip('"\'') == 'center':
                lines.append(f"{ind}{node_id}.align_y = 'MIDDLE'")
            elif valign.strip('"\'') == 'top':
                lines.append(f"{ind}{node_id}.align_y = 'TOP'")
            elif valign.strip('"\'') == 'bottom':
                lines.append(f"{ind}{node_id}.align_y = 'BOTTOM'")

            # Fill the curves to create 2D geometry
            fill_id = f"node_{node_counter[0]}"
            node_counter[0] += 1
            lines.append(f"{ind}# Fill text curves to create mesh")
            lines.append(f"{ind}{fill_id} = nodes.new('GeometryNodeFillCurve')")
            lines.append(f"{ind}{fill_id}.mode = 'NGONS'")
            lines.append(f"{ind}links.new({node_id}.outputs['Curve Instances'], {fill_id}.inputs['Curve'])")

            output_socket = f"{fill_id}.outputs['Mesh']"

        elif node.primitive_type == 'surface':
            lines.append(f"{ind}# Surface import")
            file_val = node.args.get('file', node.args.get('_pos_0', '""'))
            center = node.args.get('center', 'false')
            convexity = node.args.get('convexity', '1')
            lines.append(f"{ind}# Surface from file: {file_val}")
            lines.append(f"{ind}# center: {center}, convexity: {convexity}")
            lines.append(f"{ind}# NOTE: Surface import requires reading height-map data files")
            lines.append(f"{ind}# Using placeholder grid")
            lines.append(f"{ind}{node_id} = nodes.new('GeometryNodeMeshGrid')")
            lines.append(f"{ind}{node_id}.inputs['Vertices X'].default_value = 10")
            lines.append(f"{ind}{node_id}.inputs['Vertices Y'].default_value = 10")

            output_socket = f"{node_id}.outputs['Mesh']"

        elif node.primitive_type == 'import':
            lines.append(f"{ind}# Import external file")
            file_val = node.args.get('file', node.args.get('_pos_0', '""'))
            convexity = node.args.get('convexity', '1')
            lines.append(f"{ind}# Import file: {file_val}")
            lines.append(f"{ind}# Convexity: {convexity}")
            lines.append(f"{ind}# NOTE: File import must be handled outside Geometry Nodes")
            lines.append(f"{ind}# Importing STL/OFF/AMF/3MF via bpy.ops")
            lines.append(f"{ind}import_path = {file_val}")
            lines.append(f"{ind}if import_path.lower().endswith('.stl'):")
            lines.append(f"{ind}    bpy.ops.import_mesh.stl(filepath=import_path)")
            lines.append(f"{ind}elif import_path.lower().endswith('.obj'):")
            lines.append(f"{ind}    bpy.ops.import_scene.obj(filepath=import_path)")
            lines.append(f"{ind}imported_obj = bpy.context.selected_objects[-1] if bpy.context.selected_objects else None")
            lines.append(f"{ind}{node_id} = nodes.new('GeometryNodeObjectInfo')")
            lines.append(f"{ind}if imported_obj:")
            lines.append(f"{ind}    {node_id}.inputs['Object'].default_value = imported_obj")
            lines.append(f"{ind}{node_id}.transform_space = 'RELATIVE'")

            output_socket = f"{node_id}.outputs['Geometry']"

        elif node.primitive_type == 'projection':
            lines.append(f"{ind}# Projection (3D to 2D)")
            cut = node.args.get('cut', 'false')
            convexity = node.args.get('convexity', '1')
            lines.append(f"{ind}# cut: {cut}, convexity: {convexity}")
            lines.append(f"{ind}# NOTE: Projection requires slicing 3D geometry at Z=0 plane")

            # Process child geometry
            if node.children:
                child_code, child_output = generate_node_code(node.children[0], variables, indent, node_counter, defined_modules)
                lines.extend(child_code)

                if cut.lower() == 'true':
                    lines.append(f"{ind}# Cut mode: intersect with XY plane")
                    lines.append(f"{ind}{node_id} = nodes.new('GeometryNodeMeshBoolean')")
                    lines.append(f"{ind}{node_id}.operation = 'INTERSECT'")
                    # Create a thin plane for intersection
                    plane_id = f"node_{node_counter[0]}"
                    node_counter[0] += 1
                    lines.append(f"{ind}{plane_id} = nodes.new('GeometryNodeMeshGrid')")
                    lines.append(f"{ind}{plane_id}.inputs['Size X'].default_value = 1000")
                    lines.append(f"{ind}{plane_id}.inputs['Size Y'].default_value = 1000")
                    if child_output:
                        lines.append(f"{ind}links.new({child_output}, {node_id}.inputs['Mesh 1'])")
                    lines.append(f"{ind}links.new({plane_id}.outputs['Mesh'], {node_id}.inputs['Mesh 2'])")
                    output_socket = f"{node_id}.outputs['Mesh']"
                else:
                    lines.append(f"{ind}# Drop mode: project all geometry onto XY plane")
                    lines.append(f"{ind}# Using child geometry directly (manual projection needed)")
                    output_socket = child_output
            else:
                output_socket = None

            return lines, output_socket

        else:
            # Custom module call
            if node.primitive_type in defined_modules:
                lines.append(f"{ind}# Instantiate module: {node.primitive_type}")
                lines.append(f"{ind}{node_id} = nodes.new('GeometryNodeGroup')")
                lines.append(f"{ind}{node_id}.node_tree = bpy.data.node_groups['{node.primitive_type}']")
                output_socket = f"{node_id}.outputs['Geometry']"
            else:
                lines.append(f"{ind}# Unknown module: {node.primitive_type}")
                lines.append(f"{ind}# TODO: Implement custom module {node.primitive_type}")
                output_socket = None

    elif isinstance(node, ExtrudeNode):
        node_id = f"node_{node_counter[0]}"
        node_counter[0] += 1

        if node.extrude_type == 'linear_extrude':
            lines.append(f"{ind}# Linear Extrude (2D to 3D)")

            height = node.args.get('height', node.args.get('_pos_0', '1'))
            center_val = node.args.get('center', 'false')
            twist = node.args.get('twist', '0')
            slices = node.args.get('slices', None)
            scale_val = node.args.get('scale', '1')
            convexity = node.args.get('convexity', '1')
            fn = node.args.get('$fn', None)

            # Process child 2D geometry first
            child_output = None
            if node.children:
                child_code, child_output = generate_node_code(node.children[0], variables, indent, node_counter, defined_modules)
                lines.extend(child_code)

            # Use Extrude Mesh node for basic linear extrusion
            lines.append(f"{ind}{node_id} = nodes.new('GeometryNodeExtrudeMesh')")
            lines.append(f"{ind}{node_id}.mode = 'FACES'")
            lines.append(f"{ind}{node_id}.inputs['Offset Scale'].default_value = {height}")

            if child_output:
                lines.append(f"{ind}links.new({child_output}, {node_id}.inputs['Mesh'])")

            lines.append(f"{ind}# Twist: {twist}, Scale: {scale_val}, Center: {center_val}")
            if twist != '0':
                lines.append(f"{ind}# NOTE: Twist extrusion requires subdivided extrusion with rotation per slice")

            output_socket = f"{node_id}.outputs['Mesh']"

        elif node.extrude_type == 'rotate_extrude':
            lines.append(f"{ind}# Rotate Extrude (2D profile to 3D solid of revolution)")

            angle = node.args.get('angle', '360')
            convexity = node.args.get('convexity', '2')
            fn = node.args.get('$fn', None)

            # Process child 2D profile first
            child_output = None
            if node.children:
                child_code, child_output = generate_node_code(node.children[0], variables, indent, node_counter, defined_modules)
                lines.extend(child_code)

            # For rotate_extrude we need a Curve to Mesh approach:
            # 1. Create a circle path for the revolution
            # 2. Use the child profile as the cross-section
            circle_id = f"node_{node_counter[0]}"
            node_counter[0] += 1

            lines.append(f"{ind}# Create revolution path (circle)")
            lines.append(f"{ind}{circle_id} = nodes.new('GeometryNodeCurvePrimitiveCircle')")
            lines.append(f"{ind}{circle_id}.mode = 'RADIUS'")
            lines.append(f"{ind}{circle_id}.inputs['Radius'].default_value = 1.0")
            if fn:
                lines.append(f"{ind}{circle_id}.inputs['Resolution'].default_value = {fn}")
            else:
                lines.append(f"{ind}{circle_id}.inputs['Resolution'].default_value = 32")

            lines.append(f"{ind}# Angle: {angle} degrees")
            if angle != '360':
                lines.append(f"{ind}# NOTE: Partial rotation ({angle} deg) requires trimming the curve")
                trim_id = f"node_{node_counter[0]}"
                node_counter[0] += 1
                lines.append(f"{ind}{trim_id} = nodes.new('GeometryNodeTrimCurve')")
                lines.append(f"{ind}{trim_id}.mode = 'FACTOR'")
                lines.append(f"{ind}{trim_id}.inputs['End'].default_value = ({angle}) / 360.0")
                lines.append(f"{ind}links.new({circle_id}.outputs['Curve'], {trim_id}.inputs['Curve'])")

            lines.append(f"{ind}# NOTE: Full rotate_extrude requires Curve to Mesh with profile curve")
            lines.append(f"{ind}# The child 2D shape should be converted to a curve profile")
            lines.append(f"{ind}{node_id} = nodes.new('GeometryNodeCurveToMesh')")
            if angle != '360':
                lines.append(f"{ind}links.new({trim_id}.outputs['Curve'], {node_id}.inputs['Curve'])")
            else:
                lines.append(f"{ind}links.new({circle_id}.outputs['Curve'], {node_id}.inputs['Curve'])")

            if child_output:
                lines.append(f"{ind}# Connect child profile as cross-section")
                lines.append(f"{ind}links.new({child_output}, {node_id}.inputs['Profile Curve'])")

            output_socket = f"{node_id}.outputs['Mesh']"

    elif isinstance(node, TransformNode):
        node_id = f"node_{node_counter[0]}"
        node_counter[0] += 1

        if node.transform_type == 'color':
            lines.append(f"{ind}# Color modifier")
            lines.append(f"{ind}# Color: {node.params}")
            lines.append(f"{ind}# NOTE: Color is applied as material in Blender, not in Geometry Nodes directly")

            # Pass through child geometry
            if node.children:
                child_code, child_output = generate_node_code(node.children[0], variables, indent, node_counter, defined_modules)
                lines.extend(child_code)
                output_socket = child_output
            else:
                output_socket = None

            return lines, output_socket

        elif node.transform_type == 'hull':
            lines.append(f"{ind}# Hull operation")
            lines.append(f"{ind}# Convex hull of all children")
            lines.append(f"{ind}{node_id} = nodes.new('GeometryNodeConvexHull')")

            # Process all children and join them first
            child_outputs = []
            for child in node.children:
                child_code, child_output = generate_node_code(child, variables, indent, node_counter, defined_modules)
                lines.extend(child_code)
                if child_output:
                    child_outputs.append(child_output)

            if len(child_outputs) > 1:
                # Join all child geometries first
                join_id = f"node_{node_counter[0]}"
                node_counter[0] += 1
                lines.append(f"{ind}{join_id} = nodes.new('GeometryNodeJoinGeometry')")
                for co in child_outputs:
                    lines.append(f"{ind}links.new({co}, {join_id}.inputs['Geometry'])")
                lines.append(f"{ind}links.new({join_id}.outputs['Geometry'], {node_id}.inputs['Geometry'])")
            elif len(child_outputs) == 1:
                lines.append(f"{ind}links.new({child_outputs[0]}, {node_id}.inputs['Geometry'])")

            output_socket = f"{node_id}.outputs['Convex Hull']"
            return lines, output_socket

        elif node.transform_type == 'minkowski':
            lines.append(f"{ind}# Minkowski sum")
            lines.append(f"{ind}# NOTE: Minkowski sum has no direct Geometry Nodes equivalent")
            lines.append(f"{ind}# Approximation: process children and use convex hull as fallback")

            child_outputs = []
            for child in node.children:
                child_code, child_output = generate_node_code(child, variables, indent, node_counter, defined_modules)
                lines.extend(child_code)
                if child_output:
                    child_outputs.append(child_output)

            if child_outputs:
                join_id = f"node_{node_counter[0]}"
                node_counter[0] += 1
                lines.append(f"{ind}{join_id} = nodes.new('GeometryNodeJoinGeometry')")
                for co in child_outputs:
                    lines.append(f"{ind}links.new({co}, {join_id}.inputs['Geometry'])")
                output_socket = f"{join_id}.outputs['Geometry']"
            else:
                output_socket = None

            return lines, output_socket

        elif node.transform_type == 'offset':
            lines.append(f"{ind}# Offset (2D)")
            lines.append(f"{ind}# NOTE: 2D offset has no direct Geometry Nodes equivalent")
            lines.append(f"{ind}# Parameters: {node.params}")

            # Pass through child geometry
            if node.children:
                child_code, child_output = generate_node_code(node.children[0], variables, indent, node_counter, defined_modules)
                lines.extend(child_code)
                output_socket = child_output
            else:
                output_socket = None

            return lines, output_socket

        elif node.transform_type == 'resize':
            lines.append(f"{ind}# Resize")
            lines.append(f"{ind}{node_id} = nodes.new('GeometryNodeTransform')")
            lines.append(f"{ind}# Resize params: {node.params}")
            lines.append(f"{ind}# NOTE: OpenSCAD resize sets absolute dimensions; requires bounding box calculation")
            generate_vector_assignment(lines, node_id, 'Scale', node.params, indent)

            if node.children:
                child_code, child_output = generate_node_code(node.children[0], variables, indent, node_counter, defined_modules)
                lines.extend(child_code)
                if child_output:
                    lines.append(f"{ind}links.new({child_output}, {node_id}.inputs['Geometry'])")

            output_socket = f"{node_id}.outputs['Geometry']"

        elif node.transform_type == 'multmatrix':
            lines.append(f"{ind}# Multmatrix (4x4 transformation matrix)")
            lines.append(f"{ind}# Matrix: {node.params}")
            lines.append(f"{ind}# NOTE: Blender Transform node doesn't support arbitrary 4x4 matrices directly")
            lines.append(f"{ind}{node_id} = nodes.new('GeometryNodeTransform')")
            lines.append(f"{ind}# TODO: Decompose 4x4 matrix into translate/rotate/scale components")

            if node.children:
                child_code, child_output = generate_node_code(node.children[0], variables, indent, node_counter, defined_modules)
                lines.extend(child_code)
                if child_output:
                    lines.append(f"{ind}links.new({child_output}, {node_id}.inputs['Geometry'])")

            output_socket = f"{node_id}.outputs['Geometry']"

        elif node.transform_type == 'mirror':
            lines.append(f"{ind}# Mirror")
            lines.append(f"{ind}{node_id} = nodes.new('GeometryNodeTransform')")

            # Parse mirror vector to determine scale
            mirror_vec = parse_vector_string(node.params)
            if mirror_vec and len(mirror_vec) == 3:
                sx = f"-1" if mirror_vec[0].strip() not in ('0', '0.0') else "1"
                sy = f"-1" if mirror_vec[1].strip() not in ('0', '0.0') else "1"
                sz = f"-1" if mirror_vec[2].strip() not in ('0', '0.0') else "1"
                lines.append(f"{ind}{node_id}.inputs['Scale'].default_value[0] = {sx}")
                lines.append(f"{ind}{node_id}.inputs['Scale'].default_value[1] = {sy}")
                lines.append(f"{ind}{node_id}.inputs['Scale'].default_value[2] = {sz}")
            else:
                lines.append(f"{ind}# Mirror params: {node.params}")

            if node.children:
                child_code, child_output = generate_node_code(node.children[0], variables, indent, node_counter, defined_modules)
                lines.extend(child_code)
                if child_output:
                    lines.append(f"{ind}links.new({child_output}, {node_id}.inputs['Geometry'])")

            output_socket = f"{node_id}.outputs['Geometry']"

        else:
            # Standard transforms: translate, rotate, scale
            lines.append(f"{ind}# Transform: {node.transform_type}")
            lines.append(f"{ind}{node_id} = nodes.new('GeometryNodeTransform')")

            if node.transform_type == 'translate':
                generate_vector_assignment(lines, node_id, 'Translation', node.params, indent)
            elif node.transform_type == 'rotate':
                generate_vector_assignment(lines, node_id, 'Rotation', node.params, indent)
            elif node.transform_type == 'scale':
                generate_vector_assignment(lines, node_id, 'Scale', node.params, indent)

            # Process child and connect
            if node.children:
                child_code, child_output = generate_node_code(node.children[0], variables, indent, node_counter, defined_modules)
                lines.extend(child_code)
                if child_output:
                    lines.append(f"{ind}links.new({child_output}, {node_id}.inputs['Geometry'])")

            output_socket = f"{node_id}.outputs['Geometry']"

    elif isinstance(node, BooleanOpNode):
        node_id = f"node_{node_counter[0]}"
        node_counter[0] += 1

        lines.append(f"{ind}# Boolean Operation: {node.op_type}")

        # For difference with multiple children, we need to handle it differently
        # OpenSCAD difference subtracts all children from the first child
        if node.op_type == 'difference' and len(node.children) > 2:
            # Process first child (the base object)
            base_code, base_output = generate_node_code(node.children[0], variables, indent, node_counter, defined_modules)
            lines.extend(base_code)

            # Subtract each subsequent child iteratively
            current_output = base_output
            for i, child in enumerate(node.children[1:]):
                bool_node_id = f"node_{node_counter[0]}"
                node_counter[0] += 1

                lines.append(f"{ind}# Subtract child {i+1}")
                lines.append(f"{ind}{bool_node_id} = nodes.new('GeometryNodeMeshBoolean')")
                lines.append(f"{ind}{bool_node_id}.operation = 'DIFFERENCE'")

                # Process child
                child_code, child_output = generate_node_code(child, variables, indent, node_counter, defined_modules)
                lines.extend(child_code)

                # Connect
                if current_output:
                    lines.append(f"{ind}links.new({current_output}, {bool_node_id}.inputs['Mesh 1'])")
                if child_output:
                    lines.append(f"{ind}links.new({child_output}, {bool_node_id}.inputs['Mesh 2'])")

                current_output = f"{bool_node_id}.outputs['Mesh']"

            output_socket = current_output

        else:
            # Standard 2-operand boolean operation
            lines.append(f"{ind}{node_id} = nodes.new('GeometryNodeMeshBoolean')")

            if node.op_type == 'difference':
                lines.append(f"{ind}{node_id}.operation = 'DIFFERENCE'")
            elif node.op_type == 'union':
                lines.append(f"{ind}{node_id}.operation = 'UNION'")
            elif node.op_type == 'intersection':
                lines.append(f"{ind}{node_id}.operation = 'INTERSECT'")

            # Process children and connect them
            for i, child in enumerate(node.children[:2]):  # Only use first 2 children
                child_code, child_output = generate_node_code(child, variables, indent, node_counter, defined_modules)
                lines.extend(child_code)
                if child_output:
                    input_name = 'Mesh 1' if i == 0 else 'Mesh 2'
                    lines.append(f"{ind}links.new({child_output}, {node_id}.inputs['{input_name}'])")

            output_socket = f"{node_id}.outputs['Mesh']"

    return lines, output_socket


def main(argv):
    if len(argv) < 2:
        print("Usage: python scad_to_blender_geonodes.py <input.scad> [output.py]")
        sys.exit(1)

    input_file = argv[1]
    output_file = argv[2] if len(argv) > 2 else None

    # Parse SCAD file
    input_stream = FileStream(input_file)
    lexer = scadLexer(input_stream)
    stream = CommonTokenStream(lexer)
    parser = scadParser(stream)
    tree = parser.parse()

    # Build AST
    ast_builder = SCADtoASTBuilder()
    walker = ParseTreeWalker()
    walker.walk(ast_builder, tree)

    # Print AST for debugging
    print("=" * 80)
    print("ABSTRACT SYNTAX TREE (AST)")
    print("=" * 80)
    print(json.dumps([m.to_dict() for m in ast_builder.modules], indent=2))
    print("\n" + "=" * 80)
    print("VARIABLES")
    print("=" * 80)
    print(json.dumps(ast_builder.variables, indent=2))
    print("\n" + "=" * 80)

    # Generate Blender code
    blender_code = generate_blender_geonodes(ast_builder.modules, ast_builder.variables)

    print("GENERATED BLENDER GEOMETRY NODES CODE")
    print("=" * 80)
    print(blender_code)

    # Save to file if specified
    if output_file:
        with open(output_file, 'w') as f:
            f.write(blender_code)
        print(f"\n\nBlender code saved to: {output_file}")


if __name__ == '__main__':
    main(sys.argv)
