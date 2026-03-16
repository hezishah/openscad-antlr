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
#include <vector>
#include <set>
#include <map>
#include <algorithm>
#include <sys/stat.h>
#include <unistd.h>
#include <libgen.h>
#include "ast.h"
#include "blender_generator.h"

// External parser declarations
extern int yyparse();
extern FILE* yyin;
extern scad2blender::ASTNodePtr g_root;
extern void prescan_variables(FILE* f);

// External lexer declarations
extern void set_include_paths(const std::vector<std::string>& paths);
extern void set_current_file_dir(const std::string& dir);

// Minimum library file size (in lines) to trigger filtering
static const int LIBRARY_FILTER_THRESHOLD = 2000;

// Temp directory for filtered library files
static std::string g_temp_dir;

// --------------------------------------------------------------------------
// Library filtering: extract only needed definitions from large .scad files
// --------------------------------------------------------------------------

// Collect identifiers from file content (skipping comments and strings)
// callIds: identifiers that appear followed by '(' (function/module calls)
// allIds: all identifiers
static void collectIdentifiers(const std::string& content,
                                std::set<std::string>& callIds,
                                std::set<std::string>& allIds) {
    bool in_line_comment = false;
    bool in_block_comment = false;
    bool in_string = false;

    for (size_t i = 0; i < content.size(); i++) {
        char c = content[i];
        char next = (i + 1 < content.size()) ? content[i + 1] : 0;

        if (in_block_comment) {
            if (c == '*' && next == '/') { in_block_comment = false; i++; }
            continue;
        }
        if (in_line_comment) {
            if (c == '\n') in_line_comment = false;
            continue;
        }
        if (in_string) {
            if (c == '\\') { i++; continue; }
            if (c == '"') in_string = false;
            continue;
        }
        if (c == '/' && next == '/') { in_line_comment = true; i++; continue; }
        if (c == '/' && next == '*') { in_block_comment = true; i++; continue; }
        if (c == '"') { in_string = true; continue; }

        if (isalpha(c) || c == '_' || c == '$') {
            std::string id;
            while (i < content.size() && (isalnum(content[i]) || content[i] == '_' || content[i] == '$')) {
                id += content[i++];
            }
            // Check if followed by '(' (skip whitespace)
            size_t j = i;
            while (j < content.size() && (content[j] == ' ' || content[j] == '\t')) j++;
            if (j < content.size() && content[j] == '(') {
                callIds.insert(id);
            }
            allIds.insert(id);
            i--;
        }
    }
}

// Find include directives in file content, return list of filenames
static std::vector<std::string> findIncludes(const std::string& content) {
    std::vector<std::string> includes;
    size_t pos = 0;
    while (pos < content.size()) {
        // Skip to next potential include/use
        size_t inc = content.find("include", pos);
        size_t use = content.find("use", pos);
        size_t found = std::min(inc, use);
        if (found == std::string::npos) break;

        bool isInclude = (found == inc && inc != std::string::npos);
        size_t kwLen = isInclude ? 7 : 3;
        pos = found + kwLen;

        // Skip whitespace
        while (pos < content.size() && (content[pos] == ' ' || content[pos] == '\t')) pos++;

        // Expect < or "
        if (pos >= content.size()) break;
        char delim = content[pos];
        if (delim != '<' && delim != '"') continue;
        char endDelim = (delim == '<') ? '>' : '"';
        pos++;

        std::string filename;
        while (pos < content.size() && content[pos] != endDelim && content[pos] != '\n') {
            filename += content[pos++];
        }
        if (pos < content.size() && content[pos] == endDelim) pos++;

        if (!filename.empty()) {
            includes.push_back(filename);
        }
    }
    return includes;
}

// Resolve a library filename against include paths
static std::string resolveLibrary(const std::string& filename, const std::vector<std::string>& paths) {
    for (const auto& dir : paths) {
        std::string full = dir + "/" + filename;
        struct stat st;
        if (stat(full.c_str(), &st) == 0 && S_ISREG(st.st_mode)) {
            return full;
        }
    }
    // Try as absolute
    struct stat st;
    if (stat(filename.c_str(), &st) == 0 && S_ISREG(st.st_mode)) {
        return filename;
    }
    return "";
}

// Read entire file to string
static std::string readFile(const std::string& path) {
    std::ifstream in(path);
    if (!in) return "";
    return std::string((std::istreambuf_iterator<char>(in)),
                        std::istreambuf_iterator<char>());
}

// Split content into lines
static std::vector<std::string> splitLines(const std::string& content) {
    std::vector<std::string> lines;
    std::istringstream stream(content);
    std::string line;
    while (std::getline(stream, line)) {
        lines.push_back(line);
    }
    return lines;
}

// Structure for a module/function definition
struct LibDef {
    std::string name;
    bool isModule;  // true = module, false = function
    int startLine;  // 0-based inclusive
    int endLine;    // 0-based inclusive
};

// Scan library for top-level module/function definitions with line ranges.
// First finds all column-0 definitions, then removes nested ones.
static std::vector<LibDef> scanDefinitions(const std::vector<std::string>& lines) {
    std::vector<LibDef> defs;

    for (size_t i = 0; i < lines.size(); i++) {
        const std::string& line = lines[i];

        // Only check lines starting at column 0 with module/function
        bool isModule = false;
        size_t pos = 0;
        if (line.substr(0, 7) == "module ") {
            isModule = true; pos = 7;
        } else if (line.substr(0, 9) == "function ") {
            isModule = false; pos = 9;
        } else {
            continue;
        }

        // Extract name
        while (pos < line.size() && isspace(line[pos])) pos++;
        std::string name;
        while (pos < line.size() && (isalnum(line[pos]) || line[pos] == '_' || line[pos] == '$')) {
            name += line[pos++];
        }
        if (name.empty()) continue;

        LibDef def;
        def.name = name;
        def.isModule = isModule;
        def.startLine = (int)i;
        def.endLine = (int)i;

        if (isModule) {
            // Track braces to find end of module body
            int braceDepth = 0;
            bool foundOpen = false;
            bool inStr = false, inLC = false, inBC = false;
            for (size_t j = i; j < lines.size(); j++) {
                const std::string& l = lines[j];
                inLC = false;
                for (size_t k = 0; k < l.size(); k++) {
                    char c = l[k];
                    char nc = (k + 1 < l.size()) ? l[k + 1] : 0;
                    if (inBC) { if (c == '*' && nc == '/') { inBC = false; k++; } continue; }
                    if (inLC) continue;
                    if (inStr) { if (c == '\\') { k++; continue; } if (c == '"') inStr = false; continue; }
                    if (c == '/' && nc == '/') { inLC = true; continue; }
                    if (c == '/' && nc == '*') { inBC = true; k++; continue; }
                    if (c == '"') { inStr = true; continue; }
                    if (c == '{') { braceDepth++; foundOpen = true; }
                    else if (c == '}') {
                        braceDepth--;
                        if (foundOpen && braceDepth <= 0) {
                            def.endLine = (int)j;
                            // If we're inside a block comment, extend to close it
                            if (inBC) {
                                for (size_t jj = j + 1; jj < lines.size(); jj++) {
                                    if (lines[jj].find("*/") != std::string::npos) {
                                        def.endLine = (int)jj;
                                        break;
                                    }
                                }
                            }
                            goto done_mod;
                        }
                    }
                }
            }
            done_mod:
            if (!foundOpen) def.endLine = (int)i;
        } else {
            // Function: find terminating semicolon
            int parenDepth = 0;
            bool inStr = false, inLC = false, inBC = false;
            for (size_t j = i; j < lines.size(); j++) {
                const std::string& l = lines[j];
                size_t startK = (j == i) ? pos : 0;
                inLC = false;
                for (size_t k = startK; k < l.size(); k++) {
                    char c = l[k];
                    char nc = (k + 1 < l.size()) ? l[k + 1] : 0;
                    if (inBC) { if (c == '*' && nc == '/') { inBC = false; k++; } continue; }
                    if (inLC) continue;
                    if (inStr) { if (c == '\\') { k++; continue; } if (c == '"') inStr = false; continue; }
                    if (c == '/' && nc == '/') { inLC = true; continue; }
                    if (c == '/' && nc == '*') { inBC = true; k++; continue; }
                    if (c == '"') { inStr = true; continue; }
                    if (c == '(' || c == '[') parenDepth++;
                    else if (c == ')' || c == ']') parenDepth--;
                    else if (c == ';' && parenDepth <= 0) {
                        def.endLine = (int)j;
                        // If we're inside a block comment at the semicolon, extend to close it
                        // Check remaining chars on this line and subsequent lines
                        bool trailingBC = false;
                        for (size_t kk = k + 1; kk < l.size(); kk++) {
                            char cc = l[kk];
                            char nnc = (kk + 1 < l.size()) ? l[kk + 1] : 0;
                            if (cc == '/' && nnc == '*') { trailingBC = true; break; }
                            if (cc == '/' && nnc == '/') break; // line comment, no block
                        }
                        if (trailingBC) {
                            // Find the closing */
                            bool found = (l.find("*/", k + 1) != std::string::npos);
                            if (!found) {
                                for (size_t jj = j + 1; jj < lines.size(); jj++) {
                                    if (lines[jj].find("*/") != std::string::npos) {
                                        def.endLine = (int)jj;
                                        break;
                                    }
                                }
                            }
                        }
                        goto done_fn;
                    }
                }
            }
            done_fn:;
        }

        defs.push_back(def);
    }

    // Post-process: remove definitions nested inside another module's range.
    // Sort by start line first.
    std::sort(defs.begin(), defs.end(),
              [](const LibDef& a, const LibDef& b) { return a.startLine < b.startLine; });

    std::vector<LibDef> topLevel;
    for (size_t i = 0; i < defs.size(); i++) {
        bool nested = false;
        // Check if this definition falls inside any module's range
        for (size_t j = 0; j < defs.size(); j++) {
            if (i == j) continue;
            if (defs[j].isModule &&
                defs[i].startLine > defs[j].startLine &&
                defs[i].endLine <= defs[j].endLine) {
                nested = true;
                break;
            }
        }
        if (!nested) {
            topLevel.push_back(defs[i]);
        }
    }

    return topLevel;
}

// Find the end of a top-level variable assignment (may span multiple lines)
static int findAssignmentEnd(const std::vector<std::string>& lines, int startLine) {
    int parenDepth = 0;
    int bracketDepth = 0;
    bool inStr = false;
    bool inLC = false;
    bool inBC = false;

    for (size_t j = startLine; j < lines.size(); j++) {
        const std::string& l = lines[j];
        for (size_t k = 0; k < l.size(); k++) {
            char c = l[k];
            char nc = (k + 1 < l.size()) ? l[k + 1] : 0;

            if (inBC) {
                if (c == '*' && nc == '/') { inBC = false; k++; }
                continue;
            }
            if (inLC) continue;
            if (inStr) {
                if (c == '\\') { k++; continue; }
                if (c == '"') inStr = false;
                continue;
            }
            if (c == '/' && nc == '/') { inLC = true; continue; }
            if (c == '/' && nc == '*') { inBC = true; k++; continue; }
            if (c == '"') { inStr = true; continue; }

            if (c == '(' || c == '[') { parenDepth++; }
            else if (c == ')' || c == ']') { parenDepth--; }
            else if (c == ';' && parenDepth <= 0) {
                return (int)j;
            }
        }
        inLC = false;
    }
    return startLine;
}

// Filter a library: keep only needed definitions + referenced top-level variables
static std::string filterLibrary(const std::string& libContent,
                                  const std::set<std::string>& mainCallIds,
                                  const std::set<std::string>& mainAllIds,
                                  bool verbose) {
    auto lines = splitLines(libContent);
    auto defs = scanDefinitions(lines);

    // Build name → def index map
    std::map<std::string, int> defByName;
    for (size_t i = 0; i < defs.size(); i++) {
        defByName[defs[i].name] = (int)i;
    }

    // Start with definitions that the main file calls (identifier followed by '(')
    // For short names (len<=2), require call-position match to avoid false positives
    std::set<std::string> needed;
    for (const auto& id : mainCallIds) {
        if (defByName.count(id)) {
            needed.insert(id);
        }
    }
    // Also add longer identifiers (len>=3) that appear anywhere in main file
    // These are likely intentional references (e.g., variable names matching lib functions)
    for (const auto& id : mainAllIds) {
        if (id.size() >= 3 && defByName.count(id) && !needed.count(id)) {
            needed.insert(id);
        }
    }

    // Transitive closure: for each needed definition, scan its body for more references
    // Use call-position matching for the body too
    std::set<std::string> processed;
    bool changed = true;
    while (changed) {
        changed = false;
        for (const auto& name : std::set<std::string>(needed)) {
            if (processed.count(name)) continue;
            processed.insert(name);

            int idx = defByName[name];
            const auto& def = defs[idx];

            // Collect body text
            std::string body;
            for (int j = def.startLine; j <= def.endLine; j++) {
                body += lines[j] + "\n";
            }

            // Find call identifiers and all identifiers in body
            std::set<std::string> bodyCallIds, bodyAllIds;
            collectIdentifiers(body, bodyCallIds, bodyAllIds);

            // Add call-position matches
            for (const auto& id : bodyCallIds) {
                if (defByName.count(id) && !needed.count(id)) {
                    needed.insert(id);
                    changed = true;
                }
            }
            // Add longer identifier matches (variable references to lib functions)
            for (const auto& id : bodyAllIds) {
                if (id.size() >= 3 && defByName.count(id) && !needed.count(id)) {
                    needed.insert(id);
                    changed = true;
                }
            }
        }
    }

    // Post-filter: remove function definitions that use anonymous function syntax
    // (e.g., "function (params)=" inside let() blocks) which our parser can't handle
    std::set<std::string> toRemove;
    for (const auto& name : needed) {
        int idx = defByName[name];
        const auto& def = defs[idx];
        if (!def.isModule) {
            for (int j = def.startLine; j <= def.endLine; j++) {
                const std::string& l = lines[j];
                size_t pos = 0;
                while ((pos = l.find("function", pos)) != std::string::npos) {
                    if (j == def.startLine && pos == 0) { pos += 8; continue; }
                    if (pos > 0 && (isalnum(l[pos-1]) || l[pos-1] == '_')) { pos += 8; continue; }
                    toRemove.insert(name);
                    goto next_def;
                }
            }
            next_def:;
        }
    }
    for (const auto& name : toRemove) {
        if (verbose) {
            std::cerr << "  Skipping " << name << " (uses anonymous functions)\n";
        }
        needed.erase(name);
    }

    if (verbose) {
        std::cerr << "Library filter: " << needed.size() << " definitions needed out of "
                  << defs.size() << " total\n";
        for (const auto& n : needed) {
            std::cerr << "  - " << n << " (lines "
                      << defs[defByName[n]].startLine + 1 << "-"
                      << defs[defByName[n]].endLine + 1 << ")\n";
        }
    }

    // Collect all identifiers referenced by needed definitions (for variable filtering)
    std::set<std::string> allNeededIds;
    for (const auto& name : needed) {
        int idx = defByName[name];
        const auto& def = defs[idx];
        std::string body;
        for (int j = def.startLine; j <= def.endLine; j++) {
            body += lines[j] + "\n";
        }
        std::set<std::string> c, a;
        collectIdentifiers(body, c, a);
        allNeededIds.insert(a.begin(), a.end());
    }
    // Also add main file identifiers
    allNeededIds.insert(mainAllIds.begin(), mainAllIds.end());

    // Build set of line ranges occupied by definitions
    struct Range {
        int start, end;
        bool include;
    };
    std::vector<Range> defRanges;
    for (const auto& def : defs) {
        bool inc = needed.count(def.name) > 0;
        defRanges.push_back({def.startLine, def.endLine, inc});
    }
    std::sort(defRanges.begin(), defRanges.end(),
              [](const Range& a, const Range& b) { return a.start < b.start; });

    // Debug-only module calls to strip from definitions (not needed for geometry)
    static const std::vector<std::string> debugModules = {"InfoTxt", "HelpTxt", "Echo"};

    // Output: needed definitions + only referenced top-level variable assignments
    std::ostringstream out;
    size_t rangeIdx = 0;
    bool skippingDebugCall = false;
    int debugParenDepth = 0;
    for (size_t i = 0; i < lines.size(); i++) {
        // Advance past ranges that end before current line
        while (rangeIdx < defRanges.size() && defRanges[rangeIdx].end < (int)i) {
            rangeIdx++;
        }

        // Check if current line is inside a definition range
        if (rangeIdx < defRanges.size() &&
            (int)i >= defRanges[rangeIdx].start && (int)i <= defRanges[rangeIdx].end) {
            if (defRanges[rangeIdx].include) {
                // Check if we're skipping a multi-line debug call
                if (skippingDebugCall) {
                    // Count parens to find end of call
                    const std::string& l = lines[i];
                    for (size_t k = 0; k < l.size(); k++) {
                        char c = l[k];
                        if (c == '(') debugParenDepth++;
                        else if (c == ')') {
                            debugParenDepth--;
                            if (debugParenDepth <= 0) {
                                skippingDebugCall = false;
                                break;
                            }
                        }
                    }
                    // Skip this line (part of debug call)
                    continue;
                }

                // Check if this line starts a debug module call
                const std::string& l = lines[i];
                size_t p = 0;
                while (p < l.size() && isspace(l[p])) p++;
                bool isDebugCall = false;
                for (const auto& dm : debugModules) {
                    if (l.substr(p, dm.size()) == dm) {
                        size_t after = p + dm.size();
                        while (after < l.size() && isspace(l[after])) after++;
                        if (after < l.size() && l[after] == '(') {
                            isDebugCall = true;
                            // Check if call ends on this line
                            debugParenDepth = 0;
                            for (size_t k = after; k < l.size(); k++) {
                                if (l[k] == '(') debugParenDepth++;
                                else if (l[k] == ')') {
                                    debugParenDepth--;
                                    if (debugParenDepth <= 0) break;
                                }
                            }
                            if (debugParenDepth > 0) {
                                skippingDebugCall = true;
                            }
                            break;
                        }
                    }
                }
                if (!isDebugCall) {
                    out << lines[i] << "\n";
                }
            }
            continue;
        }

        // Line is outside any definition — it's a top-level statement
        const std::string& line = lines[i];
        size_t pos = 0;
        while (pos < line.size() && isspace(line[pos])) pos++;

        // Skip empty lines
        if (pos >= line.size()) continue;

        // Skip comments (block comments may span lines)
        if (line[pos] == '/' && pos + 1 < line.size() && line[pos + 1] == '/') continue;
        if (line[pos] == '/' && pos + 1 < line.size() && line[pos + 1] == '*') {
            // Skip block comment lines
            bool closed = (line.find("*/", pos + 2) != std::string::npos);
            if (!closed) {
                while (i + 1 < lines.size()) {
                    i++;
                    if (lines[i].find("*/") != std::string::npos) break;
                }
            }
            continue;
        }
        if (line[pos] == '*') continue; // Inside block comment continuation

        // Check if this is a variable assignment: identifier = ...;
        if (isalpha(line[pos]) || line[pos] == '_' || line[pos] == '$') {
            std::string firstId;
            size_t p = pos;
            while (p < line.size() && (isalnum(line[p]) || line[p] == '_' || line[p] == '$')) {
                firstId += line[p++];
            }
            while (p < line.size() && isspace(line[p])) p++;

            if (p < line.size() && line[p] == '=') {
                // This is a variable assignment — only include if variable is referenced
                // Skip short names (<3 chars) to avoid false positives (e.g., 'm', 'n')
                bool isNeeded = (firstId[0] == '$') ||
                                (firstId.size() >= 3 && allNeededIds.count(firstId));
                if (isNeeded) {
                    // Check if the assignment uses anonymous function syntax
                    int endLine = findAssignmentEnd(lines, (int)i);
                    bool hasAnonFunc = false;
                    for (int j = (int)i; j <= endLine; j++) {
                        size_t fpos = 0;
                        while ((fpos = lines[j].find("function", fpos)) != std::string::npos) {
                            // Skip if part of a larger word
                            if (fpos > 0 && (isalnum(lines[j][fpos-1]) || lines[j][fpos-1] == '_')) {
                                fpos += 8; continue;
                            }
                            size_t after = fpos + 8;
                            while (after < lines[j].size() && isspace(lines[j][after])) after++;
                            if (after < lines[j].size() && lines[j][after] == '(') {
                                hasAnonFunc = true;
                                break;
                            }
                            fpos += 8;
                        }
                        if (hasAnonFunc) break;
                    }
                    if (!hasAnonFunc) {
                        for (int j = (int)i; j <= endLine; j++) {
                            out << lines[j] << "\n";
                        }
                    }
                    i = endLine;
                }  else {
                    // Skip unreferenced variable (may span multiple lines)
                    int endLine = findAssignmentEnd(lines, (int)i);
                    i = endLine;
                }
                continue;
            }

            // Top-level module call — skip if module is filtered out
            if (p < line.size() && line[p] == '(') {
                if (defByName.count(firstId) && !needed.count(firstId)) {
                    int endLine = findAssignmentEnd(lines, (int)i);
                    i = endLine;
                    continue;
                }
            }

            // Skip other top-level executable statements (assert, echo, etc.)
            // Only include if it's a simple keyword we recognize
            if (firstId == "assert" || firstId == "echo") {
                // Include asserts and echos that reference needed variables
                int endLine = findAssignmentEnd(lines, (int)i);
                // Just skip them — they're not needed for geometry
                i = endLine;
                continue;
            }
        }

        // Skip everything else at top level (don't include random statements)
    }

    return out.str();
}

// Create temp directory for filtered files
static std::string createTempDir() {
    char tmpl[] = "/tmp/scad2blender_XXXXXX";
    char* dir = mkdtemp(tmpl);
    if (!dir) return "";
    return std::string(dir);
}

// Clean up temp directory
static void cleanupTempDir(const std::string& dir) {
    if (dir.empty()) return;
    // Remove files in dir, then dir itself
    // Simple approach: just use system() for cleanup
    std::string cmd = "rm -rf '" + dir + "'";
    (void)system(cmd.c_str());
}

// Pre-filter large included libraries
static void prefilterLibraries(const std::string& inputFile,
                                const std::vector<std::string>& includePaths,
                                std::vector<std::string>& paths,
                                bool verbose) {
    std::string mainContent = readFile(inputFile);
    if (mainContent.empty()) return;

    auto includes = findIncludes(mainContent);
    if (includes.empty()) return;

    std::set<std::string> mainCallIds, mainAllIds;
    collectIdentifiers(mainContent, mainCallIds, mainAllIds);

    for (const auto& incFile : includes) {
        std::string resolved = resolveLibrary(incFile, includePaths);
        if (resolved.empty()) continue;

        std::string libContent = readFile(resolved);
        auto libLines = splitLines(libContent);

        if ((int)libLines.size() < LIBRARY_FILTER_THRESHOLD) continue;

        if (verbose) {
            std::cerr << "Filtering large library: " << incFile
                      << " (" << libLines.size() << " lines)\n";
        }

        std::string filtered = filterLibrary(libContent, mainCallIds, mainAllIds, verbose);

        // Create temp dir if needed
        if (g_temp_dir.empty()) {
            g_temp_dir = createTempDir();
            if (g_temp_dir.empty()) {
                std::cerr << "Warning: Cannot create temp directory for library filtering\n";
                return;
            }
        }

        // Write filtered file with same name
        std::string filteredPath = g_temp_dir + "/" + incFile;
        std::ofstream out(filteredPath);
        if (out) {
            out << filtered;
            out.close();

            auto filteredLines = splitLines(filtered);
            if (verbose) {
                std::cerr << "Filtered " << incFile << ": "
                          << libLines.size() << " → " << filteredLines.size() << " lines\n";
            }
        }
    }

    // Add temp dir as highest-priority include path
    if (!g_temp_dir.empty()) {
        paths.insert(paths.begin(), g_temp_dir);
    }
}

// --------------------------------------------------------------------------

void printUsage(const char* program) {
    std::cerr << "Usage: " << program << " [options] <input.scad> [output.py]\n"
              << "\nOptions:\n"
              << "  -h, --help     Show this help message\n"
              << "  -v, --verbose  Enable verbose output\n"
              << "  -o <file>      Specify output file\n"
              << "  -I <dir>       Add include search path\n"
              << "\nIf output file is not specified, writes to stdout.\n"
              << "\nExamples:\n"
              << "  " << program << " model.scad output.py\n"
              << "  " << program << " model.scad > output.py\n"
              << "  " << program << " -o output.py model.scad\n"
              << "  " << program << " -I /path/to/libs model.scad output.py\n";
}

int main(int argc, char* argv[]) {
    std::string inputFile;
    std::string outputFile;
    bool verbose = false;
    std::vector<std::string> extra_include_paths;

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
        } else if (strcmp(argv[i], "-I") == 0) {
            if (i + 1 < argc) {
                extra_include_paths.push_back(argv[++i]);
            } else {
                std::cerr << "Error: -I requires an argument\n";
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

    // Set up include paths
    char* input_copy = strdup(inputFile.c_str());
    std::string input_dir = dirname(input_copy);
    free(input_copy);

    set_current_file_dir(input_dir);

    std::vector<std::string> paths;
    paths.push_back(input_dir);
    // Add user-specified include paths
    for (const auto& p : extra_include_paths) {
        paths.push_back(p);
    }
    // Add standard OpenSCAD library paths
    const char* home = getenv("HOME");
    if (home) {
        paths.push_back(std::string(home) + "/Documents/OpenSCAD/libraries");
        paths.push_back(std::string(home) + "/.local/share/OpenSCAD/libraries");
    }

    // Pre-filter large included libraries before parsing
    prefilterLibraries(inputFile, paths, paths, verbose);

    set_include_paths(paths);

    if (verbose) {
        std::cerr << "Include search paths:\n";
        for (const auto& p : paths) {
            std::cerr << "  " << p << "\n";
        }
    }

    // Set up lexer input
    yyin = input;

    // Pre-scan for top-level variable assignments (OpenSCAD hoists these)
    prescan_variables(input);

    // Parse the input
    int parseResult = yyparse();
    fclose(input);

    // Clean up temp directory
    if (!g_temp_dir.empty()) {
        cleanupTempDir(g_temp_dir);
    }

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
    generator.setSourceDir(input_dir);
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
