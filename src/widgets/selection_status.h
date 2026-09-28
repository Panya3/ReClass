#pragma once

#include "core.h"

#include <QString>

namespace rcx {

// ── The status bar's location segment ──
// main = the click target ("UnnamedClass0.field_10"), dim = the detail
// ("  +0x10  ←→ hex64 (1/6)  ·  0x80 (128)").
struct SelectionStatus { QString main; QString dim; };

namespace detail {

// Root class of `nodeIdx` — walk up to the parentless ancestor. Returns the
// display name (structTypeName, else the node name).
inline QString rootNameOf(const NodeTree& tree, int nodeIdx) {
    int cur = nodeIdx;
    while (cur >= 0 && tree.nodes[cur].parentId != 0)
        cur = tree.indexOfId(tree.nodes[cur].parentId);
    if (cur < 0) return QString();
    const auto& root = tree.nodes[cur];
    return root.structTypeName.isEmpty() ? root.name : root.structTypeName;
}

// The class whose SIZE the status quotes: the view root, or (when the view is
// the whole document) the first top-level struct.
inline uint64_t sizeRootOf(const NodeTree& tree, uint64_t viewRootId) {
    if (viewRootId != 0) return viewRootId;
    for (const auto& n : tree.nodes)
        if (n.parentId == 0 && n.kind == NodeKind::Struct) return n.id;
    return 0;
}

// "  ·  0x80 (128)" / "  ·  3 members" — the trailing size clause. The class
// NAME is deliberately absent: it is already the head of `main` (and the doc
// tab, and the address bar's trail), so repeating it here spent the widest part of the
// status bar on the word the user is looking straight at.
inline QString sizeClause(const NodeTree& tree, uint64_t viewRootId, bool needSep) {
    const uint64_t rootId = sizeRootOf(tree, viewRootId);
    if (rootId == 0) return QString();
    const int ri = tree.indexOfId(rootId);
    if (ri < 0) return QString();
    const auto& rn = tree.nodes[ri];
    const QString sep = needSep ? QStringLiteral("  ·") : QString();
    if (rn.isEnum())
        return sep + QStringLiteral("  %1 members").arg(rn.enumMembers.size());
    const int structSz = tree.structSpan(rootId);
    if (structSz <= 0) return QString();
    return sep + QStringLiteral("  0x%1 (%2)")
        .arg(QString::number(structSz, 16).toUpper()).arg(structSz);
}

} // namespace detail

// Status text for ONE selection in ONE document. A pure function of the tree so
// the segment can be REBUILT on demand — which is what lets it follow the
// active tab instead of keeping whatever the last tab to emit nodeSelected
// wrote (it used to name UnnamedClass0 while UnnamedClass4 was on screen).
// Lives in a header because MainWindow is not linkable from any test target.
inline SelectionStatus buildSelectionStatus(const NodeTree& tree, int nodeIdx,
                                            uint64_t viewRootId, int selCount) {
    SelectionStatus s;
    if (nodeIdx < 0 || nodeIdx >= tree.nodes.size()) return s;
    const auto& node = tree.nodes[nodeIdx];
    const QString rootName = detail::rootNameOf(tree, nodeIdx);

    const auto* km = kindMeta(node.kind);
    const QString typeName = km ? QString::fromLatin1(km->typeName)
                                : QStringLiteral("?");

    if (selCount > 1)            s.main = QStringLiteral("%1 ×%2").arg(typeName).arg(selCount);
    else if (node.parentId == 0) s.main = rootName;
    else if (!rootName.isEmpty()) s.main = rootName + QLatin1Char('.') + node.name;
    else                          s.main = node.name;

    if (selCount <= 1)
        s.dim = QStringLiteral("  +0x%1").arg(node.offset, 2, 16, QChar('0'));

    // Variant position among the same-size types ←/→ actually cycles through.
    const int sz = sizeForKind(node.kind);
    if (sz > 0) {
        const bool curIsString = isStringKind(node.kind);
        const bool curIsVector = isVectorKind(node.kind);
        int pos = 0, total = 0;
        for (const auto& m : kKindMeta) {
            if (m.size != sz || isContainerKind(m.kind)) continue;
            if (!curIsString && isStringKind(m.kind)) continue;
            if (!curIsVector && isVectorKind(m.kind)) continue;
            total++;
            if (m.kind == node.kind) pos = total;
        }
        if (total > 1)
            s.dim += QStringLiteral("  ←→ %1 (%2/%3)")
                .arg(typeName).arg(pos).arg(total);
        else
            s.dim += QStringLiteral("  (no variants for %1 bytes)").arg(sz);
        // The permanent "P=ptr F=float S=int U=uint" legend that used to be
        // appended here is now the label's tooltip: it never changed, so it
        // was pure noise competing with the part that does.
    }

    s.dim += detail::sizeClause(tree, viewRootId, !s.dim.isEmpty());
    return s;
}

// No single selection: name the class in view and quote its size.
inline SelectionStatus buildRootStatus(const NodeTree& tree, uint64_t viewRootId) {
    SelectionStatus s;
    const uint64_t rootId = detail::sizeRootOf(tree, viewRootId);
    if (rootId == 0) return s;
    const int ri = tree.indexOfId(rootId);
    if (ri < 0) return s;
    const auto& rn = tree.nodes[ri];
    s.main = rn.structTypeName.isEmpty() ? rn.name : rn.structTypeName;
    s.dim  = detail::sizeClause(tree, viewRootId, /*needSep=*/false);
    return s;
}

} // namespace rcx
