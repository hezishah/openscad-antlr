/**
 * @file main.cpp
 * @brief scad2blender main entry point
 *
 * Converts OpenSCAD files to Blender Geometry Nodes Python scripts.
 */

#include <iostream>
#include <fstream>
#include <sstream>
#include <string>
#include <cstring>
#include "ast.h"
#include "blender_generator.h"

// External parser declarations
extern int yyparse();
extern FILE* yyin;
extern scad2blender::ASTNodePtr g_root;
extern void prescan_variables(FILE* f);

void printUsage(const char* program) {
    std::cerr << "Usage: " << program << " [options] <input.scad> [output.py]\n"
              << "\nOptions:\n"
              << "  -h, --help     Show this help message\n"
              << "  -v, --verbose  Enable verbose output\n"
              << "  -o <file>      Specify output file\n"
              << "\nIf output file is not specified, writes to stdout.\n"
              << "\nExamples:\n"
              << "  " << program << " model.scad output.py\n"
              << "  " << program << " model.scad > output.py\n"
              << "  " << program << " -o output.py model.scad\n";
}

int main(int argc, char* argv[]) {
    std::string inputFile;
    std::string outputFile;
    bool verbose = false;

    // Parse command line arguments
    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            printUsage(argv[0]);
            return 0;
        } else if (strcmp(argv[i], "-v") == 0 || strcmp(argv[i], "--verbose") == 0) {
            verbose = true;
        } else if (strcmp(argv[i], "-o") == 0) {
            if (i + 1 < argc) {
                outputFile = argv[++i];
            } else {
                std::cerr << "Error: -o requires an argument\n";
                return 1;
            }
        } else if (argv[i][0] == '-') {
            std::cerr << "Error: Unknown option: " << argv[i] << "\n";
            printUsage(argv[0]);
            return 1;
        } else if (inputFile.empty()) {
            inputFile = argv[i];
        } else if (outputFile.empty()) {
            outputFile = argv[i];
        } else {
            std::cerr << "Error: Too many arguments\n";
            printUsage(argv[0]);
            return 1;
        }
    }

    if (inputFile.empty()) {
        std::cerr << "Error: No input file specified\n";
        printUsage(argv[0]);
        return 1;
    }

    // Open input file
    FILE* input = fopen(inputFile.c_str(), "r");
    if (!input) {
        std::cerr << "Error: Cannot open input file: " << inputFile << "\n";
        return 1;
    }

    if (verbose) {
        std::cerr << "Parsing: " << inputFile << "\n";
    }

    // Set up lexer input
    yyin = input;

    // Pre-scan for top-level variable assignments (OpenSCAD hoists these)
    prescan_variables(input);

    // Parse the input
    int parseResult = yyparse();
    fclose(input);

    if (parseResult != 0) {
        std::cerr << "Error: Parsing failed\n";
        return 1;
    }

    if (!g_root) {
        std::cerr << "Error: No AST generated\n";
        return 1;
    }

    if (verbose) {
        std::cerr << "Parsing successful\n";
        std::cerr << "Generating Blender Python code...\n";
    }

    // Generate Blender Python code
    scad2blender::BlenderGenerator generator;
    std::string pythonCode = generator.generate(g_root);

    // Output the result
    if (outputFile.empty()) {
        std::cout << pythonCode;
    } else {
        std::ofstream out(outputFile);
        if (!out) {
            std::cerr << "Error: Cannot open output file: " << outputFile << "\n";
            return 1;
        }
        out << pythonCode;

        if (verbose) {
            std::cerr << "Output written to: " << outputFile << "\n";
        }
    }

    if (verbose) {
        std::cerr << "Done.\n";
    }

    return 0;
}
