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

    def enterModule(self, ctx: scadParser.ModuleContext):
        text = ctx.getText()

        # Handle module declaration
        if text.startswith('module'):
            module_name = ctx.children[1].getText()
            params = ctx.children[3].getText() if len(ctx.children) > 3 else ""
            self.current_module = ModuleNode(module_name, params)
            self.ast_stack.append(self.current_module)
        else:
            # Handle module instantiation (primitives, transforms, boolean ops)
            if ctx.children:
                first_child = ctx.children[0]
                module_text = first_child.getText()

                # Parse the module name and arguments
                if '(' in module_text:
                    module_name = module_text.split('(')[0]
                    args_text = module_text.split('(', 1)[1].rsplit(')', 1)[0] if ')' in module_text else ""

                    # Determine node type based on module name
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

    def exitModule(self, ctx: scadParser.ModuleContext):
        if self.ast_stack:
            node = self.ast_stack.pop()

            # Add to parent or to modules list
            if self.ast_stack:
                self.ast_stack[-1].children.append(node)
            else:
                self.modules.append(node)

    def exitAssignment(self, ctx: scadParser.AssignmentContext):
        var_name = ctx.children[0].getText()
        value = ctx.children[2].getText()
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
                args[key.strip()] = val.strip()
            else:
                # Positional argument
                args[f'_pos_{len(args)}'] = part.strip()
        return args

    def _parse_params(self, params_text):
        """Parse parameters (arrays, vectors, etc.)"""
        # For now, just return as string
        # Can be enhanced to parse arrays properly
        return params_text


def generate_blender_geonodes(ast_modules, variables):
    """
    Generate Blender Python code that creates Geometry Nodes from AST
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
        "",
        "# Create a new geometry nodes modifier",
        "def setup_geometry_nodes(name):",
        "    # Create a new mesh and object",
        "    mesh = bpy.data.meshes.new(name)",
        "    obj = bpy.data.objects.new(name, mesh)",
        "    bpy.context.collection.objects.link(obj)",
        "    ",
        "    # Add Geometry Nodes modifier",
        "    modifier = obj.modifiers.new(name='GeometryNodes', type='NODES')",
        "    ",
        "    # Create a new node group",
        "    node_group = bpy.data.node_groups.new(name, 'GeometryNodeTree')",
        "    modifier.node_group = node_group",
        "    ",
        "    # Add Group Input and Output nodes",
        "    nodes = node_group.nodes",
        "    group_input = nodes.new('NodeGroupInput')",
        "    group_output = nodes.new('NodeGroupOutput')",
        "    ",
        "    # Add Geometry output socket to the group",
        "    # Blender 4.x uses node_group.interface, older versions use node_group.outputs",
        "    if hasattr(node_group, 'interface'):",
        "        node_group.interface.new_socket(name='Geometry', in_out='OUTPUT', socket_type='NodeSocketGeometry')",
        "    else:",
        "        node_group.outputs.new('NodeSocketGeometry', 'Geometry')",
        "    ",
        "    group_input.location = (-200, 0)",
        "    group_output.location = (600, 0)",
        "    ",
        "    return obj, node_group, nodes, group_input, group_output",
        "",
    ]

    # Add variables
    code_lines.append("# Variables from SCAD file")
    for var_name, value in variables.items():
        code_lines.append(f"{var_name} = {value}")
    code_lines.append("")

    # Generate node creation code for each module
    for module in ast_modules:
        code_lines.extend(generate_module_code(module, variables))

    code_lines.append("")
    code_lines.append("# Main execution")
    code_lines.append("if __name__ == '__main__':")
    code_lines.append("    clear_geometry_nodes()")

    # Call appropriate functions
    for module in ast_modules:
        if isinstance(module, ModuleNode):
            code_lines.append(f"    {module.name}()")
        else:
            # Top-level operation
            code_lines.append(f"    main_geometry()")

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


def generate_module_code(module, variables, indent=0):
    """Generate Blender Python code for a module or top-level operation"""
    ind = "    " * indent
    lines = []

    if isinstance(module, ModuleNode):
        lines.append(f"{ind}def {module.name}():")
        lines.append(f"{ind}    obj, node_group, nodes, group_input, group_output = setup_geometry_nodes('{module.name}')")
        lines.append(f"{ind}    links = node_group.links")
        lines.append(f"{ind}    ")

        # Process children
        node_counter = [0]  # Use list to allow mutation in nested function
        last_output = None

        for child in module.children:
            child_code, output = generate_node_code(child, variables, indent + 1, node_counter)
            lines.extend(child_code)
            last_output = output

        # Connect final output to group output
        if last_output:
            _connect_to_group_output(lines, last_output, indent + 1, node_counter)

        lines.append("")

    else:
        # Top-level operation (not inside a module)
        # Create a default function to wrap it
        lines.append(f"{ind}def main_geometry():")
        lines.append(f"{ind}    obj, node_group, nodes, group_input, group_output = setup_geometry_nodes('main_geometry')")
        lines.append(f"{ind}    links = node_group.links")
        lines.append(f"{ind}    ")

        # Process the operation
        node_counter = [0]
        operation_code, output = generate_node_code(module, variables, indent + 1, node_counter)
        lines.extend(operation_code)

        # Connect final output to group output
        if output:
            _connect_to_group_output(lines, output, indent + 1, node_counter)

        lines.append("")

    return lines


def parse_vector_string(vector_str):
    """Parse a vector string like '[x,y,z]' into a tuple of values"""
    if not vector_str:
        return None

    # Remove brackets and split by comma
    if vector_str.startswith('[') and vector_str.endswith(']'):
        content = vector_str[1:-1]
        components = [c.strip() for c in content.split(',')]
        return tuple(components)
    else:
        # Single value, return as single-element tuple
        return (vector_str,)


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


def generate_node_code(node, variables, indent=0, node_counter=None):
    """Generate Blender node code for an AST node"""
    if node_counter is None:
        node_counter = [0]

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
                child_code, child_output = generate_node_code(node.children[0], variables, indent, node_counter)
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
            # Custom module or unknown primitive
            lines.append(f"{ind}# Custom module: {node.primitive_type}")
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
                child_code, child_output = generate_node_code(node.children[0], variables, indent, node_counter)
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
                child_code, child_output = generate_node_code(node.children[0], variables, indent, node_counter)
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
                child_code, child_output = generate_node_code(node.children[0], variables, indent, node_counter)
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
                child_code, child_output = generate_node_code(child, variables, indent, node_counter)
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
                child_code, child_output = generate_node_code(child, variables, indent, node_counter)
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
                child_code, child_output = generate_node_code(node.children[0], variables, indent, node_counter)
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
                child_code, child_output = generate_node_code(node.children[0], variables, indent, node_counter)
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
                child_code, child_output = generate_node_code(node.children[0], variables, indent, node_counter)
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
                child_code, child_output = generate_node_code(node.children[0], variables, indent, node_counter)
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
                child_code, child_output = generate_node_code(node.children[0], variables, indent, node_counter)
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
            base_code, base_output = generate_node_code(node.children[0], variables, indent, node_counter)
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
                child_code, child_output = generate_node_code(child, variables, indent, node_counter)
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
                child_code, child_output = generate_node_code(child, variables, indent, node_counter)
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
