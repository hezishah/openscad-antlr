# OpenSCAD to Blender Geometry Nodes Converter

## Overview

This project uses ANTLR4 to parse OpenSCAD (.scad) files and convert them into Blender Geometry Nodes Python scripts using the bpy API.

## Features

- **AST Generation**: Parses .scad files and creates an Abstract Syntax Tree (AST)
- **Geometry Nodes Conversion**: Converts OpenSCAD primitives and operations to Blender Geometry Nodes
- **Supported Operations**:
  - **Primitives**: cube, sphere, cylinder (polyhedron is partially supported)
  - **Transforms**: translate, rotate, scale
  - **Boolean Operations**: difference, union, intersection
  - **Variables**: Variable assignments and usage

## Usage

### 1. Build ANTLR Parsers

First, generate the parsers (if not already done):

```bash
./build.sh
```

This will:
- Generate Java parsers in the `scad/` directory
- Generate Python parsers in the `scad_py/` directory
- Compile and test the parser with DoorStop.scad

### 2. Convert OpenSCAD to Blender

Convert a .scad file to a Blender Python script:

```bash
python3 scad_to_blender_geonodes.py <input.scad> [output.py]
```

**Example**:
```bash
python3 scad_to_blender_geonodes.py DoorStop.scad blender_doorstop.py
```

**Output**:
- Displays the AST (Abstract Syntax Tree) in JSON format
- Displays variables extracted from the .scad file
- Generates and displays Blender Python code
- Saves the Blender code to the specified output file

### 3. Use in Blender

To use the generated Blender script:

1. Open Blender
2. Go to Scripting tab
3. Open the generated .py file (e.g., `blender_doorstop.py`)
4. Run the script (Alt+P or click "Run Script")

The script will:
- Clear existing mesh objects
- Create a new object with Geometry Nodes modifier
- Build the geometry node tree to recreate the OpenSCAD model

## Examples

### Simple Example

**Input** (simple_test.scad):
```openscad
difference() {
    cube([30, 20, 10], center=true);
    translate([0, 0, 2]) {
        cube([10, 10, 15], center=true);
    }
}
```

**Generated Output**:
- Creates a Blender Geometry Nodes setup with:
  - Cube node (30x20x10)
  - Transform node (translate by [0,0,2])
  - Second cube node (10x10x15)
  - Boolean Difference node

### Complex Example

**Input** (DoorStop.scad):
- Module definition with variables
- Polyhedron primitive
- Multiple translate operations
- Boolean difference with multiple children

## Architecture

### Components

1. **scad.g4**: ANTLR4 grammar for OpenSCAD language
2. **scad_to_blender_geonodes.py**: Main converter script
   - `SCADtoASTBuilder`: Listener that builds AST from parse tree
   - AST Node Classes: `ModuleNode`, `PrimitiveNode`, `TransformNode`, `BooleanOpNode`, `AssignmentNode`
   - Code Generators: Functions to convert AST to Blender Python code

### AST Structure

The AST represents OpenSCAD code as a tree of nodes:

- **ModuleNode**: Function/module definitions
- **PrimitiveNode**: Geometric primitives (cube, sphere, etc.)
- **TransformNode**: Transformations (translate, rotate, scale)
- **BooleanOpNode**: CSG operations (difference, union, intersection)
- **AssignmentNode**: Variable assignments

### Mapping: OpenSCAD → Blender

| OpenSCAD | Blender Geometry Node |
|----------|----------------------|
| cube() | GeometryNodeMeshCube |
| sphere() | GeometryNodeMeshUVSphere |
| cylinder() | GeometryNodeMeshCylinder |
| translate() | GeometryNodeTransform (Translation) |
| rotate() | GeometryNodeTransform (Rotation) |
| scale() | GeometryNodeTransform (Scale) |
| difference() | GeometryNodeMeshBoolean (DIFFERENCE) |
| union() | GeometryNodeMeshBoolean (UNION) |
| intersection() | GeometryNodeMeshBoolean (INTERSECT) |

## Known Limitations

1. **Polyhedron Support**:
   - Polyhedrons are parsed but not fully implemented in Blender
   - Requires custom mesh creation which is complex in Geometry Nodes
   - Currently generates a placeholder cube

2. **Array/Vector Handling**:
   - Cube sizes with different dimensions may not work correctly
   - Blender's Cube node uses a single size value
   - Non-uniform cubes would require additional Transform/Scale nodes

3. **Module Calls**:
   - Custom module calls are not yet fully supported
   - Only built-in primitives and transforms are implemented

4. **Advanced Features**:
   - For loops are partially supported
   - Conditionals (if/else) are parsed but not converted
   - List comprehensions are not yet supported

## Files

- `scad.g4` - ANTLR4 grammar for OpenSCAD
- `scad_to_blender_geonodes.py` - Main converter script
- `scad-parse.py` - Legacy parser (for reference)
- `build.sh` - Build script for ANTLR parsers
- `DoorStop.scad` - Example OpenSCAD file
- `simple_test.scad` - Simple test example
- `blender_doorstop.py` - Generated Blender script
- `blender_simple.py` - Generated Blender script for simple test

## Development

### Adding New Primitives

To add support for a new OpenSCAD primitive:

1. Add to `generate_node_code()` function in `scad_to_blender_geonodes.py`
2. Map to appropriate Blender Geometry Node type
3. Handle parameters and arguments correctly

### Improving AST Building

The AST builder uses ANTLR's Listener pattern. To improve parsing:

1. Modify `SCADtoASTBuilder` class methods
2. Handle `enter*` and `exit*` events for grammar rules
3. Maintain the `ast_stack` correctly for nested structures

## Future Improvements

- [ ] Full polyhedron support with custom mesh creation
- [ ] Better handling of non-uniform scaling
- [ ] Support for custom module calls
- [ ] For loop and conditional conversion
- [ ] More primitives (text, linear_extrude, rotate_extrude, etc.)
- [ ] Better error handling and validation
- [ ] GUI interface for easier conversion

## License

This project uses ANTLR4 which is licensed under BSD.

## Author

Generated with Claude Code
