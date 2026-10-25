#pragma once
// What a GuiShell is, by its SubType: SAP reports every shell (grid, tree, toolbar, HTML viewer, ...) as Type
// "GuiShell". The screen reader, the metadata extractor, the renderers and the audit trail ask here.
#include <string>

namespace fairyfly {
namespace sap {

enum class ShellExtractionKind { Grid, Tree, Metadata };

inline ShellExtractionKind classify_shell_extraction(const std::string& subtype) {
    if (subtype == "GridView") return ShellExtractionKind::Grid;
    if (subtype == "Tree" || subtype == "TableTreeControl")
        return ShellExtractionKind::Tree;
    // Everything else (Toolbar, HTMLViewer, AbapEditor, TextEdit, Calendar,
    // unknown or empty SubType) goes through the metadata probe so its
    // content is not silently dropped (ERR-137).
    return ShellExtractionKind::Metadata;
}

/// An ALV grid (SubType GridView).
inline bool is_grid_shell(const std::string& subtype) { return classify_shell_extraction(subtype) == ShellExtractionKind::Grid; }

/// Shells with a dedicated reader; any other SubType gets the generic probe (grid-like, HTML/text, accessibility).
inline bool has_dedicated_shell_reader(const std::string& subtype) {
    return subtype == "GridView" || subtype == "Tree" || subtype == "TableTreeControl" || subtype == "Toolbar" ||
           subtype == "HTMLViewer" || subtype == "AbapEditor" || subtype == "TextEdit";
}

} // namespace sap
} // namespace fairyfly
