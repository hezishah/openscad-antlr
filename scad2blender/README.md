# scad2blender - OpenSCAD to Blender Geometry Nodes Converter

A C++ tool that converts OpenSCAD (.scad) files to Blender Geometry Nodes Python scripts.

## Overview

This project reuses parsing concepts from the OpenSCAD project to build a CSG (Constructive Solid Geometry) tree, then traverses that tree to generate equivalent Blender Geometry Nodes Python code.

## Architecture

```
┌─────────────────┐     ┌─────────────────┐     ┌─────────────────┐
│   .scad file    │────▶│   Flex/Bison    │────▶│    AST/CSG      │
│                 │     │   Parser        │     │    Tree         │
└─────────────────┘     └─────────────────┘     └────────┬────────┘
                                                         │
                                                         ▼
┌─────────────────┐     ┌─────────────────┐     ┌─────────────────┐
│  Blender .py    │◀────│   Python Code   │◀────│   Tree Walker   │
│  script         │     │   Generator     │     │                 │
└─────────────────┘     └─────────────────┘     └─────────────────┘
```

## Building

### Prerequisites

- CMake 3.16+
- C++17 compatible compiler
- Flex (lexer generator)
- Bison (parser generator)

### Build Steps

```bash
mkdir build && cd build
cmake ..
make
```

## Usage

```bash
./scad2blender input.scad -o output.py
```

The generated Python script can be run in Blender to create the geometry.

## Project Structure

```
scad2blender/
├── src/
│   ├── main.cpp              # Entry point
│   ├── lexer.l               # Flex lexer definition
│   ├── parser.y              # Bison parser definition
│   ├── ast.cpp               # AST node implementations
│   ├── csg_tree.cpp          # CSG tree builder
│   └── blender_generator.cpp # Python code generator
├── include/
│   ├── ast.h                 # AST node definitions
│   ├── csg_tree.h            # CSG tree classes
│   └── blender_generator.h   # Code generator interface
├── examples/
│   └── *.scad                # Example OpenSCAD files
├── CMakeLists.txt
└── README.md
```

## Supported OpenSCAD Features

### Primitives
- `cube([x, y, z], center)`
- `sphere(r)` / `sphere(d)`
- `cylinder(h, r1, r2, center)`
- `polyhedron(points, faces)`
- `circle(r)` / `circle(d)`
- `square([x, y], center)`

### Transforms
- `translate([x, y, z])`
- `rotate([x, y, z])` / `rotate(a, v)`
- `scale([x, y, z])`
- `mirror([x, y, z])`
- `color([r, g, b, a])`

### Boolean Operations
- `union()`
- `difference()`
- `intersection()`

### Extrusions
- `linear_extrude(height, twist, scale)`
- `rotate_extrude(angle)`

### Modules
- Module definitions with parameters
- `children()` for passed geometry

### Other
- Variables and expressions
- For loops
- Conditionals (if/else)
- Comments

## Blender Geometry Nodes Mapping

| OpenSCAD | Blender Geometry Node |
|----------|----------------------|
| cube | GeometryNodeMeshCube |
| sphere | GeometryNodeMeshUVSphere |
| cylinder | GeometryNodeMeshCone |
| translate | GeometryNodeTransform (Translation) |
| rotate | GeometryNodeTransform (Rotation) |
| scale | GeometryNodeTransform (Scale) |
| difference | GeometryNodeMeshBoolean (DIFFERENCE) |
| union | GeometryNodeMeshBoolean (UNION) |
| intersection | GeometryNodeMeshBoolean (INTERSECT) |
| linear_extrude | GeometryNodeExtrudeMesh |

## License

This project is licensed under the GPL v2, consistent with the OpenSCAD project
from which parsing concepts are derived.

## Credits

- OpenSCAD project: https://openscad.org/
- Blender: https://blender.org/
