#include "generator.h"
#include <QHash>
#include <QVector>
#include <QStringList>
#include <algorithm>

namespace rcx {

namespace {

// ── Identifier sanitisation ──

static QString sanitizeIdent(const QString& name) {
    if (name.isEmpty()) return QStringLiteral("unnamed");
    QString out;
    out.reserve(name.size());
    for (QChar c : name) {
        if (c.isLetterOrNumber() || c == '_') out += c;
        else out += '_';
    }
    if (!out[0].isLetter() && out[0] != '_')
        out.prepend('_');
    return out;
}

// ── C type name for a primitive NodeKind ──

static QString cTypeName(NodeKind kind) {
    switch (kind) {
    case NodeKind::Hex8:      return QStringLiteral("uint8_t");
    case NodeKind::Hex16:     return QStringLiteral("uint16_t");
    case NodeKind::Hex32:     return QStringLiteral("uint32_t");
    case NodeKind::Hex64:     return QStringLiteral("uint64_t");
    case NodeKind::Hex128:    return QStringLiteral("uint8_t");  // no C int128; emitted as array in emitField
    case NodeKind::Int8:      return QStringLiteral("int8_t");
    case NodeKind::Int16:     return QStringLiteral("int16_t");
    case NodeKind::Int32:     return QStringLiteral("int32_t");
    case NodeKind::Int64:     return QStringLiteral("int64_t");
    case NodeKind::Int128:    return QStringLiteral("__int128");
    case NodeKind::UInt8:     return QStringLiteral("uint8_t");
    case NodeKind::UInt16:    return QStringLiteral("uint16_t");
    case NodeKind::UInt32:    return QStringLiteral("uint32_t");
    case NodeKind::UInt64:    return QStringLiteral("uint64_t");
    case NodeKind::UInt128:   return QStringLiteral("unsigned __int128");
    case NodeKind::Float16:   return QStringLiteral("_Float16");
    case NodeKind::Float:     return QStringLiteral("float");
    case NodeKind::Double:    return QStringLiteral("double");
    case NodeKind::Bool:      return QStringLiteral("bool");
    case NodeKind::Pointer32: return QStringLiteral("uint32_t");
    case NodeKind::Pointer64: return QStringLiteral("uint64_t");
    case NodeKind::FuncPtr32: return QStringLiteral("uint32_t");
    case NodeKind::FuncPtr64: return QStringLiteral("uint64_t");
    case NodeKind::Vec2:      return QStringLiteral("float");
    case NodeKind::Vec3:      return QStringLiteral("float");
    case NodeKind::Vec4:      return QStringLiteral("float");
    case NodeKind::Mat4x4:    return QStringLiteral("float");
    case NodeKind::UTF8:      return QStringLiteral("char");
    case NodeKind::UTF16:     return QStringLiteral("wchar_t");
    default:                  return QStringLiteral("uint8_t");
    }
}

// ── Generator context ──

struct GenContext {
    const NodeTree& tree;
    QHash<uint64_t, QVector<int>> childMap;
    QSet<QString>   emittedTypeNames;   // struct type names already emitted
    QSet<uint64_t>  emittedIds;         // struct node IDs already emitted
    QSet<uint64_t>  visiting;           // cycle guard
    QSet<uint64_t>  forwardDeclared;    // forward-declared type IDs
    QString         output;
    const QHash<NodeKind, QString>* typeAliases = nullptr;
    bool            emitAsserts = false;
    // Pre-computed unique name per struct id. Populated by assignUniqueNames()
    // before any emission. Use nameFor(node) at emission/reference sites so
    // two root structs sharing the same structTypeName both get output —
    // instead of the second being silently dropped by the dedup check.
    // Kept at the end so existing aggregate initialisers still bind positions
    // 1..10 correctly; nameById defaults to an empty hash.
    QHash<uint64_t, QString> nameById;
    // When true, class-keyword types split generated padding into `private:`
    // sections and user-declared fields into `public:` sections (ReClass.NET
    // style). struct/union are unaffected — they default to public in C++.
    bool privatePads = false;
    // Zero-padded hex width for offset annotations and auto-named members,
    // derived from the top-level class size (digitsForSize). Set per class
    // before its body is emitted; nested containers inherit it.
    int padDigits = 2;
    // Pad-name dedup scope: reset per top-level class, shared by nested
    // anonymous containers (their members hoist into the class scope).
    QSet<QString> usedPadNames;

    void prepare() { output.reserve(tree.nodes.size() * 80); }

    // Children sorted by offset.
    QVector<int> prepareChildren(uint64_t structId) const {
        QVector<int> children = childMap.value(structId);
        std::sort(children.begin(), children.end(), [&](int a, int b) {
            return tree.nodes[a].offset < tree.nodes[b].offset;
        });
        return children;
    }

    QString uniquePadName(int offset) {
        QString base = QStringLiteral("_pad_%1").arg(offset, padDigits, 16, QChar('0'));
        QString name = base;
        for (int n = 2; usedPadNames.contains(name); n++)
            name = base + QStringLiteral("_%1").arg(n);
        usedPadNames.insert(name);
        return name;
    }

    // Resolve the C type name for a primitive, consulting aliases first
    QString cType(NodeKind kind) const {
        if (typeAliases) {
            auto it = typeAliases->find(kind);
            if (it != typeAliases->end() && !it.value().isEmpty())
                return it.value();
        }
        return cTypeName(kind);
    }

    // Resolve the canonical type name for a struct/array node
    QString structName(const Node& n) const {
        if (!n.structTypeName.isEmpty()) return sanitizeIdent(n.structTypeName);
        if (!n.name.isEmpty())           return sanitizeIdent(n.name);
        return QStringLiteral("anon_%1").arg(n.id, 0, 16);
    }

    // Post-disambiguation name lookup. Falls back to structName() for nodes
    // not yet in nameById (e.g. anonymous inline structs, which collide by
    // id rather than by name).
    QString nameFor(const Node& n) const {
        auto it = nameById.find(n.id);
        if (it != nameById.end()) return it.value();
        return structName(n);
    }

    // Pre-pass: walk every struct that could be referenced by a generated
    // identifier (root structs first for stable naming, then named nested
    // structs) and assign a unique C identifier. Duplicates get a `_v2`,
    // `_v3`, … suffix so both survive in the generated output. Anonymous
    // inline structs aren't processed here because they don't need a name —
    // they're emitted inline via the kind keyword ("struct { … }").
    void assignUniqueNames() {
        QSet<QString> used;
        auto assign = [&](uint64_t id, const QString& base) {
            QString name = base;
            int suffix = 2;
            while (used.contains(name))
                name = QStringLiteral("%1_v%2").arg(base).arg(suffix++);
            used.insert(name);
            nameById[id] = name;
        };
        for (const Node& n : tree.nodes) {
            if (n.parentId != 0 || n.kind != NodeKind::Struct) continue;
            assign(n.id, structName(n));
        }
        for (const Node& n : tree.nodes) {
            if (n.parentId == 0 || n.kind != NodeKind::Struct) continue;
            if (n.structTypeName.isEmpty()) continue;
            assign(n.id, structName(n));
        }
    }
};

// Forward declarations
static void emitStruct(GenContext& ctx, uint64_t structId);

// ── Field line with offset comment (code + marker + comment) ──
// We use a \x01 marker to separate the code part from the offset comment.
// After all output is generated, alignComments() replaces markers with padding.

static const QChar kCommentMarker = QChar(0x01);

// Offset annotation for a generated line. width is the zero-padded hex
// digit count for the class being emitted (2/4/8 depending on its size).
// Member lines get a front comment `/* XX */`; the closing-size comment
// stays as a trailing `// sizeof 0x...` on the `};`/`}` line.
static QString offsetComment(int offset, int width, bool isSizeof = false) {
    if (isSizeof)
        return QString(kCommentMarker) + QStringLiteral("// sizeof 0x%1").arg(QString::number(offset, 16).toUpper());
    return QStringLiteral("/* %1 */ ").arg(QString::number(offset, 16).toUpper(), width, QChar('0'));
}

// Hex digit count for offset annotations: grows with the class size so a
// small class stays compact (/* 28 */) while a big one aligns (/* 0028 */).
static int digitsForSize(int size) {
    if (size <= 0xFF) return 2;
    if (size <= 0xFFFF) return 4;
    return 8;
}

static QString indent(int depth) {
    return QString(depth * 4, ' ');
}

static QString emitField(GenContext& ctx, const Node& node, int depth, int baseOffset) {
    const NodeTree& tree = ctx.tree;
    QString ind = indent(depth);
    QString name = sanitizeIdent(node.name.isEmpty()
        ? QStringLiteral("field_%1").arg(node.offset, ctx.padDigits, 16, QChar('0'))
        : node.name);
    QString oc = offsetComment(baseOffset + node.offset, ctx.padDigits);

    switch (node.kind) {
    case NodeKind::Vec2:
        return ind + oc + QStringLiteral("%1 %2[2];").arg(ctx.cType(NodeKind::Float), name);
    case NodeKind::Vec3:
        return ind + oc + QStringLiteral("%1 %2[3];").arg(ctx.cType(NodeKind::Float), name);
    case NodeKind::Vec4:
        return ind + oc + QStringLiteral("%1 %2[4];").arg(ctx.cType(NodeKind::Float), name);
    case NodeKind::Mat4x4:
        return ind + oc + QStringLiteral("%1 %2[4][4];").arg(ctx.cType(NodeKind::Float), name);
    case NodeKind::UTF8:
        return ind + oc + QStringLiteral("%1 %2[%3];").arg(ctx.cType(NodeKind::UTF8), name).arg(node.strLen);
    case NodeKind::UTF16:
        return ind + oc + QStringLiteral("%1 %2[%3];").arg(ctx.cType(NodeKind::UTF16), name).arg(node.strLen);
    case NodeKind::Pointer32:
    case NodeKind::Pointer64: {
        if (node.refId != 0) {
            int refIdx = tree.indexOfId(node.refId);
            if (refIdx >= 0) {
                QString target = ctx.nameFor(tree.nodes[refIdx]);
                return ind + oc + QStringLiteral("struct %1* %2;").arg(target, name);
            }
        }
        // Native pointer: use void* when this is the target's natural pointer kind
        bool isNativePtr = (node.kind == NodeKind::Pointer32 && ctx.tree.pointerSize <= 4)
                        || (node.kind == NodeKind::Pointer64 && ctx.tree.pointerSize >= 8);
        if (isNativePtr)
            return ind + oc + QStringLiteral("void* %1;").arg(name);
        // Cross-size pointer: fall back to raw integer type
        return ind + oc + QStringLiteral("%1 %2;").arg(ctx.cType(node.kind), name);
    }
    case NodeKind::FuncPtr32:
        return ind + oc + QStringLiteral("void (*%1)();").arg(name);
    case NodeKind::FuncPtr64:
        return ind + oc + QStringLiteral("void (*%1)();").arg(name);
    default:
        return ind + oc + QStringLiteral("%1 %2;").arg(ctx.cType(node.kind), name);
    }
}    // ── Emit struct body (fields + padding) — Vergilius-style ──

    // Build a trailing comment listing a vftable block's virtual-function
    // entries, e.g. " // virtual: void speak(), int age(bool fast)". Used by
    // the non-C++ backends (Rust/C#/Python), which have no native virtual
    // declaration — the entries live outside the object, so emitting them as
    // members would corrupt the layout; a comment preserves the information.
    static QString vfEntriesComment(const NodeTree& tree, const Node& block) {
        if (!(block.isVftable() && isPointerKind(block.kind))) return {};
        QStringList sigs;
        for (int ci : tree.childrenOf(block.id)) {
            const Node& vf = tree.nodes[ci];
            if (vf.draft) continue;
            QString ret = vf.vfReturnType.isEmpty()
                ? QStringLiteral("void") : vf.vfReturnType;
            sigs << ret + QStringLiteral(" ") + vf.name
                 + QStringLiteral("(") + vf.vfParams + QStringLiteral(")");
        }
        if (sigs.isEmpty()) return {};
        return QStringLiteral(" // virtual: %1").arg(sigs.join(QStringLiteral(", ")));
    }

    static void emitStructBody(GenContext& ctx, uint64_t structId,
                           bool isUnion, int depth, int baseOffset,
                           bool classSections = false) {
    const NodeTree& tree = ctx.tree;
    int idx = tree.indexOfId(structId);
    if (idx < 0) return;

    int structSize = tree.structSpan(structId, &ctx.childMap);
    QString ind = indent(depth);
    // Access labels (public:/private:) sit at the container's own level
    // (same indent as the `class` keyword line). For a top-level class
    // depth is 1, so labelInd is the 0-indent column.
    QString labelInd = indent(depth - 1);

    auto children = ctx.prepareChildren(structId);

    // Section tracking for class-keyword bodies (privatePads on): generated
    // padding goes under `private:` and user-declared members under
    // `public:`. A class body starts in the implicit private section — a
    // leading pad gets no label, matching ReClass.NET output.
    bool inPublic = false;
    auto ensurePublic = [&]() {
        if (classSections && !inPublic) {
            ctx.output += labelInd + QStringLiteral("public:\n");
            inPublic = true;
        }
    };

    // ── Virtual-function declarations ──
    // A vftable block (classKeyword="vftable" pointer at offset 0) is not a
    // member field: its FuncPtr children become pure-virtual declarations
    // emitted at the TOP of the class body, before any members. The block's
    // pointer size still counts toward the layout (members follow after it),
    // which the field loop below handles by skipping the block node itself.
    for (int ci : children) {
        const Node& block = tree.nodes[ci];
        // Guarded by kind: only a pointer-marked vftable block is special —
        // a hand-authored struct that happens to carry classKeyword
        // "vftable" stays a normal member.
        if (!(block.isVftable() && isPointerKind(block.kind))) continue;
        ensurePublic();
        for (int vci : tree.childrenOf(block.id)) {
            const Node& vf = tree.nodes[vci];
            if (vf.draft) continue;
            QString ret = vf.vfReturnType.isEmpty()
                ? QStringLiteral("void") : vf.vfReturnType;
            QString vfName = sanitizeIdent(vf.name);
            // No offset comment on virtual declarations: the block.offset
            // (the __vptr slot in the object) is NOT the function's offset —
            // virtual functions live in the vtable, outside the object's
            // layout, so a front `/* XX */` here would be wrong/duplicated
            // on every entry.
            ctx.output += ind + QStringLiteral("virtual %1 %2(%3) = 0;\n")
                .arg(ret, vfName, vf.vfParams);
        }
    }

    // Helper: emit a padding/hex run as a single collapsed byte array
    auto emitPadRun = [&](int relOffset, int size) {
        if (size <= 0) return;
        // Padding is never part of the public API of a class
        if (classSections && inPublic) {
            ctx.output += labelInd + QStringLiteral("private:\n");
            inPublic = false;
        }
        ctx.output += ind + offsetComment(baseOffset + relOffset, ctx.padDigits)
            + QStringLiteral("uint8_t %1[0x%2];\n")
            .arg(ctx.uniquePadName(baseOffset + relOffset))
            .arg(QString::number(size, 16).toUpper());
    };

    int cursor = 0;
    int i = 0;

    while (i < children.size()) {
        const Node& child = tree.nodes[children[i]];
        // Draft fields are acknowledged-broken placeholders: never emitted.
        // Their bytes fall into the pad/gap runs below, so the generated
        // layout stays contiguous without the conflicting field.
        if (child.draft) { i++; continue; }
        // Vftable block: its FuncPtr children were already emitted as
        // virtual declarations above; the block itself is not a member
        // field, but it does occupy pointer size at its offset (members
        // follow after it in a real object). Kind-guarded: only pointer
        // nodes can be vftable blocks.
        if (child.isVftable() && isPointerKind(child.kind)) {
            int blockSz = child.byteSize();
            if (blockSz <= 0) blockSz = 8;
            cursor = qMax(cursor, child.offset + blockSz);
            i++;
            continue;
        }
        int childSize;
        if (child.kind == NodeKind::Struct || child.kind == NodeKind::Array)
            childSize = tree.structSpan(child.id, &ctx.childMap);
        else
            childSize = child.byteSize();

        // Gap/overlap handling (skip for unions)
        if (!isUnion) {
            if (child.offset > cursor)
                emitPadRun(cursor, child.offset - cursor);
            else if (child.offset < cursor)
                ctx.output += ind + QStringLiteral("// WARNING: overlap at offset 0x%1 (previous field ends at 0x%2)\n")
                    .arg(QString::number(baseOffset + child.offset, 16).toUpper())
                    .arg(QString::number(baseOffset + cursor, 16).toUpper());
        }

        // Collapse consecutive hex nodes into a single padding array
        if (isHexNode(child.kind)) {
            int runStart = child.offset;
            int runEnd = child.offset + childSize;
            int j = i + 1;
            while (j < children.size()) {
                const Node& next = tree.nodes[children[j]];
                if (!isHexNode(next.kind)) break;
                int nextSize = next.byteSize();
                if (next.offset < runEnd) break;
                runEnd = next.offset + nextSize;
                j++;
            }
            emitPadRun(runStart, runEnd - runStart);
            cursor = runEnd;
            i = j;
            continue;
        }

        // Emit the field
        if (child.kind == NodeKind::Struct) {
            // Bitfield container — emit inline bitfield members
            if (child.isBitfield()
                && !child.bitfieldMembers.isEmpty()) {
                QString bfType = ctx.cType(child.elementKind);
                if (bfType.isEmpty()) bfType = QStringLiteral("uint32_t");
                QString fieldName = child.name.isEmpty()
                    ? QString() : QStringLiteral(" ") + sanitizeIdent(child.name);
                ensurePublic();
                ctx.output += ind + QStringLiteral("struct {\n");
                QString bfInd = indent(depth + 1);
                for (const auto& m : child.bitfieldMembers) {
                    ctx.output += bfInd + offsetComment(baseOffset + child.offset, ctx.padDigits)
                        + bfType + QStringLiteral(" ")
                        + sanitizeIdent(m.name) + QStringLiteral(" : ")
                        + QString::number(m.bitWidth) + QStringLiteral(";")
                        + QStringLiteral("\n");
                }
                ctx.output += ind + offsetComment(baseOffset + child.offset, ctx.padDigits)
                    + QStringLiteral("}") + fieldName + QStringLiteral(";")
                    + QStringLiteral("\n");
            } else {

            bool isAnonymous = child.structTypeName.isEmpty();

            if (isAnonymous) {
                // Inline anonymous struct/union/class — a user member of the
                // enclosing body, so it belongs in the public section.
                ensurePublic();
                QString kw = child.resolvedClassKeyword();
                ctx.output += ind + kw + QStringLiteral(" {\n");
                bool childIsUnion = (kw == QStringLiteral("union"));
                bool childSections = (kw == QStringLiteral("class")) && ctx.privatePads;
                // Same rule as emitStruct: an inline anonymous `class` would
                // default its members to private without an explicit section.
                if (kw == QStringLiteral("class") && !ctx.privatePads)
                    ctx.output += ind + QStringLiteral("public:\n");
                emitStructBody(ctx, child.id, childIsUnion, depth + 1,
                               baseOffset + child.offset, childSections);
                QString fieldName = child.name.isEmpty()
                    ? QString() : QStringLiteral(" ") + sanitizeIdent(child.name);
                ctx.output += ind + offsetComment(baseOffset + child.offset, ctx.padDigits)
                    + QStringLiteral("}") + fieldName + QStringLiteral(";")
                    + QStringLiteral("\n");
            } else {
                // Named struct — inline declared form when it carries its own
                // body (its children live under it): `struct B { ... } Inner;`.
                // Otherwise it is an embedded field referencing a top-level
                // definition, emitted by name (`B Inner;`).
                QString kw = child.resolvedClassKeyword();
                if (kw == QStringLiteral("enum") && child.enumMembers.isEmpty())
                    kw = QStringLiteral("struct");
                QString typeName = ctx.nameFor(child);
                if (!ctx.childMap.value(child.id).isEmpty()) {
                    // Same shape as the anonymous branch above, with the
                    // type name after the keyword.
                    ensurePublic();
                    ctx.output += ind + kw + QStringLiteral(" ") + typeName
                        + QStringLiteral(" {\n");
                    bool childIsUnion = (kw == QStringLiteral("union"));
                    bool childSections = (kw == QStringLiteral("class")) && ctx.privatePads;
                    // An inline named `class` would default its members to
                    // private without an explicit section (same rule as
                    // emitStruct and the anonymous branch).
                    if (kw == QStringLiteral("class") && !ctx.privatePads)
                        ctx.output += ind + QStringLiteral("public:\n");
                    emitStructBody(ctx, child.id, childIsUnion, depth + 1,
                                   baseOffset + child.offset, childSections);
                    QString fieldName = child.name.isEmpty()
                        ? QString() : QStringLiteral(" ") + sanitizeIdent(child.name);
                    ctx.output += ind + offsetComment(baseOffset + child.offset, ctx.padDigits)
                        + QStringLiteral("}") + fieldName + QStringLiteral(";")
                        + QStringLiteral("\n");
                } else {
                    QString fieldName = sanitizeIdent(child.name);
                    ensurePublic();
                    ctx.output += ind + offsetComment(baseOffset + child.offset, ctx.padDigits)
                        + kw + QStringLiteral(" ") + typeName
                        + QStringLiteral(" ") + fieldName + QStringLiteral(";")
                        + QStringLiteral("\n");
                }
            }
            } // end bitfield else
        } else if (child.kind == NodeKind::Array) {
            QVector<int> arrayKids = ctx.childMap.value(child.id);
            bool hasStructChild = false;
            QString elemTypeName;

            for (int ak : arrayKids) {
                if (tree.nodes[ak].kind == NodeKind::Struct) {
                    hasStructChild = true;
                    elemTypeName = ctx.nameFor(tree.nodes[ak]);
                    break;
                }
            }

            QString fieldName = sanitizeIdent(child.name);
            ensurePublic();
            if (hasStructChild && !elemTypeName.isEmpty()) {
                ctx.output += ind + offsetComment(baseOffset + child.offset, ctx.padDigits)
                    + QStringLiteral("struct %1 %2[%3];\n")
                    .arg(elemTypeName, fieldName).arg(child.arrayLen);
            } else {
                ctx.output += ind + offsetComment(baseOffset + child.offset, ctx.padDigits)
                    + QStringLiteral("%1 %2[%3];\n")
                    .arg(ctx.cType(child.elementKind), fieldName).arg(child.arrayLen);
            }
        } else {
            ensurePublic();
            ctx.output += emitField(ctx, child, depth, baseOffset) + QStringLiteral("\n");
        }

        int childEnd = child.offset + childSize;
        if (childEnd > cursor) cursor = childEnd;
        i++;
    }

    // Tail padding (skip for unions)
    if (!isUnion && cursor < structSize)
        emitPadRun(cursor, structSize - cursor);

}

// ── Emit a complete top-level struct definition (Vergilius-style) ──

static void emitStruct(GenContext& ctx, uint64_t structId) {
    if (ctx.emittedIds.contains(structId)) return;
    if (ctx.visiting.contains(structId)) return; // cycle
    ctx.visiting.insert(structId);

    int idx = ctx.tree.indexOfId(structId);
    if (idx < 0) { ctx.visiting.remove(structId); return; }

    const Node& node = ctx.tree.nodes[idx];
    if (node.kind != NodeKind::Struct && node.kind != NodeKind::Array) {
        ctx.visiting.remove(structId);
        return;
    }

//TODO-DELETE(emitStruct (redundant Array guard))     if (node.kind == NodeKind::Array) {
//        ctx.visiting.remove(structId);
//        return;
//    }

    // Deduplicate by struct type name
    QString typeName = ctx.nameFor(node);
    if (ctx.emittedTypeNames.contains(typeName)) {
        ctx.emittedIds.insert(structId);
        ctx.visiting.remove(structId);
        return;
    }

    ctx.emittedIds.insert(structId);
    ctx.emittedTypeNames.insert(typeName);

    // Forward-declare pointer targets not yet emitted (prevents compilation errors)
    for (int ci : ctx.childMap.value(structId)) {
        const Node& child = ctx.tree.nodes[ci];
        if (isPointerKind(child.kind) && child.refId != 0) {
            int ri = ctx.tree.indexOfId(child.refId);
            if (ri >= 0 && !ctx.emittedIds.contains(child.refId)
                && !ctx.forwardDeclared.contains(child.refId)) {
                ctx.output += QStringLiteral("struct %1;\n").arg(ctx.nameFor(ctx.tree.nodes[ri]));
                ctx.forwardDeclared.insert(child.refId);
            }
        }
    }

    // A union's sizeof() in C is the largest member, not the extent of its
    // members' offsets — use unionSize so the emitted sizeof comment and
    // static_assert match what the compiler computes.
    int structSize = node.isUnion()
        ? ctx.tree.unionSize(structId, &ctx.childMap)
        : ctx.tree.structSpan(structId, &ctx.childMap);

    QString kw = node.resolvedClassKeyword();

    // Enum with members: emit as proper C enum
    if (kw == QStringLiteral("enum") && !node.enumMembers.isEmpty()) {
        ctx.output += QStringLiteral("enum %1 {\n").arg(typeName);
        for (const auto& m : node.enumMembers) {
            ctx.output += QStringLiteral("    %1 = %2,\n")
                .arg(sanitizeIdent(m.first))
                .arg(m.second);
        }
        ctx.output += QStringLiteral("};\n\n");
        ctx.visiting.remove(structId);
        return;
    }

    if (kw == QStringLiteral("enum")) kw = QStringLiteral("struct");

    // Per-class annotation context: offset digit width and pad-name dedup
    // scope (nested anonymous containers share the class's member namespace).
    ctx.padDigits = digitsForSize(structSize);
    ctx.usedPadNames.clear();

    ctx.output += kw + QStringLiteral(" ") + typeName + QStringLiteral(" {\n");

    bool classSections = (kw == QStringLiteral("class")) && ctx.privatePads;
    // C++ `class` defaults members to private — export an explicit
    // `public:` section first so the fields stay accessible (matches
    // ReClass.NET generated output). struct/union default to public
    // and need no specifier. With privatePads enabled the section
    // layout is handled inside emitStructBody (padding → private:,
    // user members → public:) instead.
    if (kw == QStringLiteral("class") && !ctx.privatePads)
        ctx.output += QStringLiteral("public:\n");

    emitStructBody(ctx, structId, kw == QStringLiteral("union"), 1, 0, classSections);

    ctx.output += QStringLiteral("};")
        + offsetComment(structSize, 0, true)
        + QStringLiteral("\n");
    if (ctx.emitAsserts)
        ctx.output += QStringLiteral("static_assert(sizeof(%1) == 0x%2, \"Size mismatch for %1\");\n")
            .arg(typeName)
            .arg(QString::number(structSize, 16).toUpper());
    ctx.output += QStringLiteral("\n");

    ctx.visiting.remove(structId);
}

// ── Build the child map used by all generators ──

static QHash<uint64_t, QVector<int>> buildChildMap(const NodeTree& tree) {
    QHash<uint64_t, QVector<int>> map;
    for (int i = 0; i < tree.nodes.size(); i++)
        map[tree.nodes[i].parentId].append(i);
    return map;
}

// ── Align offset comments ──
// Replaces kCommentMarker with spaces so all "// 0x..." comments align to
// the same column (the longest code portion + 1 space).

static QString alignComments(const QString& raw) {
    QStringList lines = raw.split('\n');

    // First pass: find the maximum code width (text before the marker)
    int maxCode = 0;
    for (const QString& line : lines) {
        int pos = line.indexOf(kCommentMarker);
        if (pos >= 0)
            maxCode = qMax(maxCode, pos);
    }

    // Second pass: replace markers with padding
    QString result;
    result.reserve(raw.size() + lines.size() * 8);
    for (int i = 0; i < lines.size(); i++) {
        if (i > 0) result += '\n';
        const QString& line = lines[i];
        int pos = line.indexOf(kCommentMarker);
        if (pos >= 0) {
            result += line.left(pos);
            int pad = maxCode - pos + 1;
            if (pad < 1) pad = 1;
            result += QString(pad, ' ');
            result += line.mid(pos + 1);  // skip the marker char
        } else {
            result += line;
        }
    }
    return result;
}

// ═══════════════════════════════════════════════════════════════════
// ── Rust backend ──
// ═══════════════════════════════════════════════════════════════════

static QString rustTypeName(NodeKind kind) {
    switch (kind) {
    case NodeKind::Hex8:      return QStringLiteral("u8");
    case NodeKind::Hex16:     return QStringLiteral("u16");
    case NodeKind::Hex32:     return QStringLiteral("u32");
    case NodeKind::Hex64:     return QStringLiteral("u64");
    case NodeKind::Hex128:    return QStringLiteral("u128");
    case NodeKind::Int8:      return QStringLiteral("i8");
    case NodeKind::Int16:     return QStringLiteral("i16");
    case NodeKind::Int32:     return QStringLiteral("i32");
    case NodeKind::Int64:     return QStringLiteral("i64");
    case NodeKind::Int128:    return QStringLiteral("i128");
    case NodeKind::UInt8:     return QStringLiteral("u8");
    case NodeKind::UInt16:    return QStringLiteral("u16");
    case NodeKind::UInt32:    return QStringLiteral("u32");
    case NodeKind::UInt64:    return QStringLiteral("u64");
    case NodeKind::UInt128:   return QStringLiteral("u128");
    case NodeKind::Float16:   return QStringLiteral("f16");
    case NodeKind::Float:     return QStringLiteral("f32");
    case NodeKind::Double:    return QStringLiteral("f64");
    case NodeKind::Bool:      return QStringLiteral("bool");
    case NodeKind::Pointer32: return QStringLiteral("u32");
    case NodeKind::Pointer64: return QStringLiteral("u64");
    case NodeKind::FuncPtr32: return QStringLiteral("u32");
    case NodeKind::FuncPtr64: return QStringLiteral("u64");
    case NodeKind::Vec2:      return QStringLiteral("f32");
    case NodeKind::Vec3:      return QStringLiteral("f32");
    case NodeKind::Vec4:      return QStringLiteral("f32");
    case NodeKind::Mat4x4:    return QStringLiteral("f32");
    case NodeKind::UTF8:      return QStringLiteral("u8");
    case NodeKind::UTF16:     return QStringLiteral("u16");
    default:                  return QStringLiteral("u8");
    }
}

// Forward declaration
static void emitRustStruct(GenContext& ctx, uint64_t structId);

static QString rustType(GenContext& ctx, NodeKind kind) {
    if (ctx.typeAliases) {
        auto it = ctx.typeAliases->find(kind);
        if (it != ctx.typeAliases->end() && !it.value().isEmpty())
            return it.value();
    }
    return rustTypeName(kind);
}

static QString emitRustField(GenContext& ctx, const Node& node, int depth, int baseOffset) {
    const NodeTree& tree = ctx.tree;
    QString ind = indent(depth);
    QString name = sanitizeIdent(node.name.isEmpty()
        ? QStringLiteral("field_%1").arg(node.offset, ctx.padDigits, 16, QChar('0'))
        : node.name);
    QString oc = offsetComment(baseOffset + node.offset, ctx.padDigits);

    switch (node.kind) {
    case NodeKind::Vec2:
        return ind + oc + QStringLiteral("pub %1: [f32; 2],").arg(name);
    case NodeKind::Vec3:
        return ind + oc + QStringLiteral("pub %1: [f32; 3],").arg(name);
    case NodeKind::Vec4:
        return ind + oc + QStringLiteral("pub %1: [f32; 4],").arg(name);
    case NodeKind::Mat4x4:
        return ind + oc + QStringLiteral("pub %1: [[f32; 4]; 4],").arg(name);
    case NodeKind::UTF8:
        return ind + oc + QStringLiteral("pub %1: [u8; %2],").arg(name).arg(node.strLen);
    case NodeKind::UTF16:
        return ind + oc + QStringLiteral("pub %1: [u16; %2],").arg(name).arg(node.strLen);
    case NodeKind::Pointer32:
    case NodeKind::Pointer64: {
        if (node.refId != 0) {
            int refIdx = tree.indexOfId(node.refId);
            if (refIdx >= 0) {
                QString target = ctx.nameFor(tree.nodes[refIdx]);
                return ind + oc + QStringLiteral("pub %1: *mut %2,").arg(name, target);
            }
        }
        bool isNativePtr = (node.kind == NodeKind::Pointer32 && ctx.tree.pointerSize <= 4)
                        || (node.kind == NodeKind::Pointer64 && ctx.tree.pointerSize >= 8);
        if (isNativePtr)
            return ind + oc + QStringLiteral("pub %1: *mut core::ffi::c_void,").arg(name);
        return ind + oc + QStringLiteral("pub %1: %2,").arg(name, rustType(ctx, node.kind));
    }
    case NodeKind::FuncPtr32:
    case NodeKind::FuncPtr64:
        return ind + oc + QStringLiteral("pub %1: Option<unsafe extern \"C\" fn()>,").arg(name);
    default:
        return ind + oc + QStringLiteral("pub %1: %2,").arg(name, rustType(ctx, node.kind));
    }
}

static void emitRustStructBody(GenContext& ctx, uint64_t structId,
                                bool isUnion, int depth, int baseOffset) {
    const NodeTree& tree = ctx.tree;
    int idx = tree.indexOfId(structId);
    if (idx < 0) return;

    int structSize = tree.structSpan(structId, &ctx.childMap);
    QString ind = indent(depth);

    auto children = ctx.prepareChildren(structId);

    auto emitPadRun = [&](int relOffset, int size) {
        if (size <= 0) return;
        ctx.output += ind + offsetComment(baseOffset + relOffset, ctx.padDigits)
            + QStringLiteral("pub %1: [u8; 0x%2],")
            .arg(ctx.uniquePadName(baseOffset + relOffset))
            .arg(QString::number(size, 16).toUpper())
            + QStringLiteral("\n");
    };

    int cursor = 0;
    int i = 0;

    while (i < children.size()) {
        const Node& child = tree.nodes[children[i]];
        int childSize;
        if (child.kind == NodeKind::Struct || child.kind == NodeKind::Array)
            childSize = tree.structSpan(child.id, &ctx.childMap);
        else
            childSize = child.byteSize();

        if (!isUnion) {
            if (child.offset > cursor)
                emitPadRun(cursor, child.offset - cursor);
        }

        if (isHexNode(child.kind)) {
            int runStart = child.offset;
            int runEnd = child.offset + childSize;
            int j = i + 1;
            while (j < children.size()) {
                const Node& next = tree.nodes[children[j]];
                if (!isHexNode(next.kind)) break;
                int nextSize = next.byteSize();
                if (next.offset < runEnd) break;
                runEnd = next.offset + nextSize;
                j++;
            }
            emitPadRun(runStart, runEnd - runStart);
            cursor = runEnd;
            i = j;
            continue;
        }

        if (child.kind == NodeKind::Struct) {
            if (child.isBitfield()
                && !child.bitfieldMembers.isEmpty()) {
                // Rust has no native bitfields — emit container + comment
                QString bfType = rustType(ctx, child.elementKind);
                if (bfType.isEmpty()) bfType = QStringLiteral("u32");
                QString fieldName = sanitizeIdent(child.name.isEmpty()
                    ? QStringLiteral("bitfield_%1").arg(child.offset, ctx.padDigits, 16, QChar('0'))
                    : child.name);
                QStringList bits;
                for (const auto& m : child.bitfieldMembers)
                    bits << QStringLiteral("%1:%2").arg(sanitizeIdent(m.name)).arg(m.bitWidth);
                ctx.output += ind + offsetComment(baseOffset + child.offset, ctx.padDigits)
                    + QStringLiteral("pub %1: %2,")
                    .arg(fieldName, bfType)
                    + QStringLiteral(" // bits: ") + bits.join(QStringLiteral(", "))
                    + QStringLiteral("\n");
            } else {
                bool isAnonymous = child.structTypeName.isEmpty();
                bool hasOwnBody = !ctx.childMap.value(child.id).isEmpty();
                if (isAnonymous || hasOwnBody) {
                    // Rust can't do anonymous inline structs — flatten as
                    // byte array. Named inline structs (a type declared
                    // inline, no top-level definition) get the same
                    // treatment: there is no definition to reference.
                    int span = tree.structSpan(child.id, &ctx.childMap);
                    QString fieldName = sanitizeIdent(child.name.isEmpty()
                        ? QStringLiteral("anon_%1").arg(child.offset, ctx.padDigits, 16, QChar('0'))
                        : child.name);
                    ctx.output += ind + offsetComment(baseOffset + child.offset, ctx.padDigits)
                        + QStringLiteral("pub %1: [u8; 0x%2],")
                        .arg(fieldName)
                        .arg(QString::number(span, 16).toUpper())
                        + QStringLiteral("\n");
                } else {
                    QString kw = child.resolvedClassKeyword();
                    if (kw == QStringLiteral("enum") && child.enumMembers.isEmpty())
                        kw = QStringLiteral("struct");
                    QString typeName = ctx.nameFor(child);
                    QString fieldName = sanitizeIdent(child.name);
                    ctx.output += ind + offsetComment(baseOffset + child.offset, ctx.padDigits)
                        + QStringLiteral("pub %1: %2,")
                        .arg(fieldName, typeName)
                        + QStringLiteral("\n");
                }
            }
        } else if (child.kind == NodeKind::Array) {
            QVector<int> arrayKids = ctx.childMap.value(child.id);
            bool hasStructChild = false;
            QString elemTypeName;
            for (int ak : arrayKids) {
                if (tree.nodes[ak].kind == NodeKind::Struct) {
                    hasStructChild = true;
                    elemTypeName = ctx.nameFor(tree.nodes[ak]);
                    break;
                }
            }
            QString fieldName = sanitizeIdent(child.name);
            if (hasStructChild && !elemTypeName.isEmpty()) {
                ctx.output += ind + offsetComment(baseOffset + child.offset, ctx.padDigits)
                    + QStringLiteral("pub %1: [%2; %3],")
                    .arg(fieldName, elemTypeName).arg(child.arrayLen)
                    + QStringLiteral("\n");
            } else {
                ctx.output += ind + offsetComment(baseOffset + child.offset, ctx.padDigits)
                    + QStringLiteral("pub %1: [%2; %3],")
                    .arg(fieldName, rustType(ctx, child.elementKind)).arg(child.arrayLen)
                    + QStringLiteral("\n");
            }
        } else {
            // Vftable block: the pointer field is real layout (the vptr), the
            // virtual entries live outside the object — surface them as a
            // comment rather than members (members would corrupt the layout).
            QString vfNote = vfEntriesComment(tree, child);
            ctx.output += emitRustField(ctx, child, depth, baseOffset)
                + vfNote + QStringLiteral("\n");
        }

        int childEnd = child.offset + childSize;
        if (childEnd > cursor) cursor = childEnd;
        i++;
    }

    if (!isUnion && cursor < structSize)
        emitPadRun(cursor, structSize - cursor);

}

static void emitRustStruct(GenContext& ctx, uint64_t structId) {
    if (ctx.emittedIds.contains(structId)) return;
    if (ctx.visiting.contains(structId)) return;
    ctx.visiting.insert(structId);

    int idx = ctx.tree.indexOfId(structId);
    if (idx < 0) { ctx.visiting.remove(structId); return; }

    const Node& node = ctx.tree.nodes[idx];
    if (node.kind != NodeKind::Struct) { ctx.visiting.remove(structId); return; }

    QString typeName = ctx.nameFor(node);
    if (ctx.emittedTypeNames.contains(typeName)) {
        ctx.emittedIds.insert(structId);
        ctx.visiting.remove(structId);
        return;
    }

    ctx.emittedIds.insert(structId);
    ctx.emittedTypeNames.insert(typeName);
    // A union's sizeof() in Rust is the largest member, not the extent of its
    // members' offsets — use unionSize so the emitted size comment and
    // size_of assert match what the compiler computes.
    int structSize = node.isUnion()
        ? ctx.tree.unionSize(structId, &ctx.childMap)
        : ctx.tree.structSpan(structId, &ctx.childMap);

    QString kw = node.resolvedClassKeyword();

    // Enum with members
    if (kw == QStringLiteral("enum") && !node.enumMembers.isEmpty()) {
        ctx.output += QStringLiteral("#[repr(i64)]\npub enum %1 {\n").arg(typeName);
        for (const auto& m : node.enumMembers) {
            ctx.output += QStringLiteral("    %1 = %2,\n")
                .arg(sanitizeIdent(m.first))
                .arg(m.second);
        }
        ctx.output += QStringLiteral("}\n\n");
        ctx.visiting.remove(structId);
        return;
    }

    ctx.padDigits = digitsForSize(structSize);
    ctx.usedPadNames.clear();

    bool isUnion = (kw == QStringLiteral("union"));

    if (isUnion)
        ctx.output += QStringLiteral("#[repr(C)]\n#[derive(Copy, Clone)]\n#[allow(dead_code)]\npub union %1 {\n").arg(typeName);
    else
        ctx.output += QStringLiteral("#[repr(C)]\n#[derive(Debug)]\n#[allow(dead_code)]\npub struct %1 {\n").arg(typeName);

    emitRustStructBody(ctx, structId, isUnion, 1, 0);

    ctx.output += QStringLiteral("}")
        + offsetComment(structSize, 0, true)
        + QStringLiteral("\n");
    if (ctx.emitAsserts)
        ctx.output += QStringLiteral("const _: () = assert!(core::mem::size_of::<%1>() == 0x%2);\n")
            .arg(typeName)
            .arg(QString::number(structSize, 16).toUpper());
    ctx.output += QStringLiteral("\n");

    ctx.visiting.remove(structId);
}

// ═══════════════════════════════════════════════════════════════════
// ── #define offsets backend ──
// ═══════════════════════════════════════════════════════════════════

static void emitDefinesForStruct(GenContext& ctx, uint64_t structId,
                                  const QString& prefix, int baseOffset) {
    int idx = ctx.tree.indexOfId(structId);
    if (idx < 0) return;

    const Node& node = ctx.tree.nodes[idx];
    QString typeName = prefix.isEmpty() ? ctx.nameFor(node) : prefix;
    QString kw = node.resolvedClassKeyword();

    // Enum with members: emit #define EnumName_MemberName value
    if (kw == QStringLiteral("enum") && !node.enumMembers.isEmpty()) {
        ctx.output += QStringLiteral("// %1 (enum)\n").arg(typeName);
        for (const auto& m : node.enumMembers) {
            ctx.output += QStringLiteral("#define %1_%2 %3\n")
                .arg(typeName, sanitizeIdent(m.first))
                .arg(m.second);
        }
        ctx.output += QStringLiteral("\n");
        return;
    }

    int structSize = node.isUnion()
        ? ctx.tree.unionSize(structId, &ctx.childMap)
        : ctx.tree.structSpan(structId, &ctx.childMap);
    ctx.output += QStringLiteral("// %1 (0x%2 bytes)\n")
        .arg(typeName)
        .arg(QString::number(structSize, 16).toUpper());

    QVector<int> children = ctx.childMap.value(structId);
    std::sort(children.begin(), children.end(), [&](int a, int b) {
        return ctx.tree.nodes[a].offset < ctx.tree.nodes[b].offset;
    });

    for (int ci : children) {
        const Node& child = ctx.tree.nodes[ci];
        if (isHexNode(child.kind)) continue;

        QString fieldName = sanitizeIdent(child.name.isEmpty()
            ? QStringLiteral("field_%1").arg(child.offset, ctx.padDigits, 16, QChar('0'))
            : child.name);
        int absOffset = baseOffset + child.offset;

        ctx.output += QStringLiteral("#define %1_%2 0x%3\n")
            .arg(typeName, fieldName)
            .arg(QString::number(absOffset, 16).toUpper());

        // Recurse into named sub-structs
        if (child.kind == NodeKind::Struct && !child.structTypeName.isEmpty()
            && child.classKeyword != QStringLiteral("bitfield")) {
            emitDefinesForStruct(ctx, child.id,
                typeName + QStringLiteral("_") + fieldName, absOffset);
        }
    }
    ctx.output += QStringLiteral("\n");
}

// ═══════════════════════════════════════════════════════════════════
// ── C# backend ──
// ═══════════════════════════════════════════════════════════════════

static QString csTypeName(NodeKind kind) {
    switch (kind) {
    case NodeKind::Hex8:      return QStringLiteral("byte");
    case NodeKind::Hex16:     return QStringLiteral("ushort");
    case NodeKind::Hex32:     return QStringLiteral("uint");
    case NodeKind::Hex64:     return QStringLiteral("ulong");
    case NodeKind::Hex128:    return QStringLiteral("byte");  // emitted as fixed byte[16]
    case NodeKind::Int8:      return QStringLiteral("sbyte");
    case NodeKind::Int16:     return QStringLiteral("short");
    case NodeKind::Int32:     return QStringLiteral("int");
    case NodeKind::Int64:     return QStringLiteral("long");
    case NodeKind::Int128:    return QStringLiteral("Int128");
    case NodeKind::UInt8:     return QStringLiteral("byte");
    case NodeKind::UInt16:    return QStringLiteral("ushort");
    case NodeKind::UInt32:    return QStringLiteral("uint");
    case NodeKind::UInt64:    return QStringLiteral("ulong");
    case NodeKind::UInt128:   return QStringLiteral("UInt128");
    case NodeKind::Float16:   return QStringLiteral("Half");
    case NodeKind::Float:     return QStringLiteral("float");
    case NodeKind::Double:    return QStringLiteral("double");
    case NodeKind::Bool:      return QStringLiteral("bool");
    case NodeKind::Pointer32: return QStringLiteral("uint");
    case NodeKind::Pointer64: return QStringLiteral("ulong");
    case NodeKind::FuncPtr32: return QStringLiteral("uint");
    case NodeKind::FuncPtr64: return QStringLiteral("ulong");
    case NodeKind::Vec2:      return QStringLiteral("float");
    case NodeKind::Vec3:      return QStringLiteral("float");
    case NodeKind::Vec4:      return QStringLiteral("float");
    case NodeKind::Mat4x4:    return QStringLiteral("float");
    case NodeKind::UTF8:      return QStringLiteral("byte");
    case NodeKind::UTF16:     return QStringLiteral("char");
    default:                  return QStringLiteral("byte");
    }
}

// Forward declaration
static void emitCSharpStruct(GenContext& ctx, uint64_t structId);

static QString csType(GenContext& ctx, NodeKind kind) {
    if (ctx.typeAliases) {
        auto it = ctx.typeAliases->find(kind);
        if (it != ctx.typeAliases->end() && !it.value().isEmpty())
            return it.value();
    }
    return csTypeName(kind);
}

static void emitCSharpStructBody(GenContext& ctx, uint64_t structId,
                                  bool isUnion, int depth, int baseOffset) {
    const NodeTree& tree = ctx.tree;
    int idx = tree.indexOfId(structId);
    if (idx < 0) return;

    QString ind = indent(depth);

    auto children = ctx.prepareChildren(structId);

    // C# uses [FieldOffset(N)] for explicit layout — no manual padding needed
    for (int ci : children) {
        const Node& child = tree.nodes[ci];
        if (isHexNode(child.kind)) continue; // skip padding/hex nodes

        int absOffset = baseOffset + child.offset;
        QString name = sanitizeIdent(child.name.isEmpty()
            ? QStringLiteral("field_%1").arg(child.offset, ctx.padDigits, 16, QChar('0'))
            : child.name);
        QString oc = offsetComment(absOffset, ctx.padDigits);

        if (child.kind == NodeKind::Struct) {
            if (child.isBitfield()
                && !child.bitfieldMembers.isEmpty()) {
                QString bfType = csType(ctx, child.elementKind);
                if (bfType.isEmpty()) bfType = QStringLiteral("uint");
                QStringList bits;
                for (const auto& m : child.bitfieldMembers)
                    bits << QStringLiteral("%1:%2").arg(sanitizeIdent(m.name)).arg(m.bitWidth);
                ctx.output += ind + oc + QStringLiteral("[FieldOffset(0x%1)] public %2 %3;")
                    .arg(QString::number(absOffset, 16).toUpper(), bfType, name)
                    + QStringLiteral(" // bits: ") + bits.join(QStringLiteral(", "))
                    + QStringLiteral("\n");
            } else if (child.structTypeName.isEmpty()) {
                // Anonymous inline — emit as fixed byte array
                // (structExtent: Size= must cover the members' byte range;
                // structSpan would report the C-size footprint for unions)
                int span = tree.structExtent(child.id, &ctx.childMap);
                ctx.output += ind + oc + QStringLiteral("[FieldOffset(0x%1)] public fixed byte %2[0x%3];")
                    .arg(QString::number(absOffset, 16).toUpper(), name)
                    .arg(QString::number(span, 16).toUpper())
                    + QStringLiteral("\n");
            } else {
                QString typeName = ctx.nameFor(child);
                if (!ctx.childMap.value(child.id).isEmpty()) {
                    // Named inline struct: C# has no anonymous member structs
                    // or inline type declarations, so declare the type as a
                    // nested struct first, then the field referencing it.
                    bool childUnion =
                        (child.resolvedClassKeyword() == QStringLiteral("union"));
                    int childSize = tree.structExtent(child.id, &ctx.childMap);
                    ctx.output += ind + QStringLiteral(
                        "[StructLayout(LayoutKind.Explicit, Size = 0x%1)]\n")
                        .arg(QString::number(childSize, 16).toUpper());
                    ctx.output += ind + QStringLiteral("public unsafe struct %1\n")
                        .arg(typeName)
                        + ind + QStringLiteral("{\n");
                    // Nested type: member FieldOffsets are relative to ITS
                    // own origin, not to the enclosing struct — so the base
                    // offset passed down is 0 (same as a top-level type).
                    emitCSharpStructBody(ctx, child.id, childUnion, depth + 1, 0);
                    ctx.output += ind + QStringLiteral("}\n");
                }
                ctx.output += ind + oc + QStringLiteral("[FieldOffset(0x%1)] public %2 %3;")
                    .arg(QString::number(absOffset, 16).toUpper(), typeName, name)
                    + QStringLiteral("\n");
            }
        } else if (child.kind == NodeKind::Array) {
            QVector<int> arrayKids = ctx.childMap.value(child.id);
            bool hasStructChild = false;
            QString elemTypeName;
            for (int ak : arrayKids) {
                if (tree.nodes[ak].kind == NodeKind::Struct) {
                    hasStructChild = true;
                    elemTypeName = ctx.nameFor(tree.nodes[ak]);
                    break;
                }
            }
            if (hasStructChild && !elemTypeName.isEmpty()) {
                // MarshalAs for struct arrays
                ctx.output += ind + oc + QStringLiteral("[FieldOffset(0x%1)] [MarshalAs(UnmanagedType.ByValArray, SizeConst = %2)] public %3[] %4;")
                    .arg(QString::number(absOffset, 16).toUpper())
                    .arg(child.arrayLen)
                    .arg(elemTypeName, name)
                    + QStringLiteral("\n");
            } else {
                QString elemType = csType(ctx, child.elementKind);
                ctx.output += ind + oc + QStringLiteral("[FieldOffset(0x%1)] public fixed %2 %3[%4];")
                    .arg(QString::number(absOffset, 16).toUpper(), elemType, name)
                    .arg(child.arrayLen)
                    + QStringLiteral("\n");
            }
        } else {
            // Primitive fields
            switch (child.kind) {
            case NodeKind::Vec2:
                ctx.output += ind + oc + QStringLiteral("[FieldOffset(0x%1)] public fixed float %2[2];")
                    .arg(QString::number(absOffset, 16).toUpper(), name) + QStringLiteral("\n");
                break;
            case NodeKind::Vec3:
                ctx.output += ind + oc + QStringLiteral("[FieldOffset(0x%1)] public fixed float %2[3];")
                    .arg(QString::number(absOffset, 16).toUpper(), name) + QStringLiteral("\n");
                break;
            case NodeKind::Vec4:
                ctx.output += ind + oc + QStringLiteral("[FieldOffset(0x%1)] public fixed float %2[4];")
                    .arg(QString::number(absOffset, 16).toUpper(), name) + QStringLiteral("\n");
                break;
            case NodeKind::Mat4x4:
                ctx.output += ind + oc + QStringLiteral("[FieldOffset(0x%1)] public fixed float %2[16];")
                    .arg(QString::number(absOffset, 16).toUpper(), name) + QStringLiteral("\n");
                break;
            case NodeKind::UTF8:
                ctx.output += ind + oc + QStringLiteral("[FieldOffset(0x%1)] public fixed byte %2[%3];")
                    .arg(QString::number(absOffset, 16).toUpper(), name)
                    .arg(child.strLen) + QStringLiteral("\n");
                break;
            case NodeKind::UTF16:
                ctx.output += ind + oc + QStringLiteral("[FieldOffset(0x%1)] public fixed char %2[%3];")
                    .arg(QString::number(absOffset, 16).toUpper(), name)
                    .arg(child.strLen) + QStringLiteral("\n");
                break;
            case NodeKind::Pointer32:
            case NodeKind::Pointer64: {
                bool isNativePtr = (child.kind == NodeKind::Pointer32 && ctx.tree.pointerSize <= 4)
                                || (child.kind == NodeKind::Pointer64 && ctx.tree.pointerSize >= 8);
                // Vftable block: vptr is real layout, entries live outside
                // the object — comment, never members.
                QString vfNote = vfEntriesComment(tree, child);
                if (isNativePtr)
                    ctx.output += ind + oc + QStringLiteral("[FieldOffset(0x%1)] public IntPtr %2;")
                        .arg(QString::number(absOffset, 16).toUpper(), name)
                        + vfNote + QStringLiteral("\n");
                else
                    ctx.output += ind + oc + QStringLiteral("[FieldOffset(0x%1)] public %2 %3;")
                        .arg(QString::number(absOffset, 16).toUpper(), csType(ctx, child.kind), name)
                        + vfNote + QStringLiteral("\n");
                break;
            }
            case NodeKind::FuncPtr32:
            case NodeKind::FuncPtr64:
                ctx.output += ind + oc + QStringLiteral("[FieldOffset(0x%1)] public IntPtr %2;")
                    .arg(QString::number(absOffset, 16).toUpper(), name)
                    + QStringLiteral(" // fn ptr") + QStringLiteral("\n");
                break;
            default:
                ctx.output += ind + oc + QStringLiteral("[FieldOffset(0x%1)] public %2 %3;")
                    .arg(QString::number(absOffset, 16).toUpper(), csType(ctx, child.kind), name)
                    + QStringLiteral("\n");
                break;
            }
        }
    }

}

static void emitCSharpStruct(GenContext& ctx, uint64_t structId) {
    if (ctx.emittedIds.contains(structId)) return;
    if (ctx.visiting.contains(structId)) return;
    ctx.visiting.insert(structId);

    int idx = ctx.tree.indexOfId(structId);
    if (idx < 0) { ctx.visiting.remove(structId); return; }

    const Node& node = ctx.tree.nodes[idx];
    if (node.kind != NodeKind::Struct) { ctx.visiting.remove(structId); return; }

    QString typeName = ctx.nameFor(node);
    if (ctx.emittedTypeNames.contains(typeName)) {
        ctx.emittedIds.insert(structId);
        ctx.visiting.remove(structId);
        return;
    }

    ctx.emittedIds.insert(structId);
    ctx.emittedTypeNames.insert(typeName);
    // structExtent, not structSpan: [StructLayout(Explicit, Size=)] must
    // cover the members' actual byte range (a member at offset 4 sized
    // 0x10 spans to 0x14), or C# throws a TypeLoadException.
    int structSize = ctx.tree.structExtent(structId, &ctx.childMap);

    QString kw = node.resolvedClassKeyword();

    // Enum with members
    if (kw == QStringLiteral("enum") && !node.enumMembers.isEmpty()) {
        ctx.output += QStringLiteral("public enum %1 : long\n{\n").arg(typeName);
        for (const auto& m : node.enumMembers) {
            ctx.output += QStringLiteral("    %1 = %2,\n")
                .arg(sanitizeIdent(m.first))
                .arg(m.second);
        }
        ctx.output += QStringLiteral("}\n\n");
        ctx.visiting.remove(structId);
        return;
    }

    ctx.padDigits = digitsForSize(structSize);
    ctx.usedPadNames.clear();

    bool isUnion = (kw == QStringLiteral("union"));

    ctx.output += QStringLiteral("[StructLayout(LayoutKind.Explicit, Size = 0x%1)]\n")
        .arg(QString::number(structSize, 16).toUpper());
    ctx.output += QStringLiteral("public unsafe struct %1\n{\n").arg(typeName);

    emitCSharpStructBody(ctx, structId, isUnion, 1, 0);

    ctx.output += QStringLiteral("}")
        + offsetComment(structSize, 0, true)
        + QStringLiteral("\n\n");

    ctx.visiting.remove(structId);
}

// ═══════════════════════════════════════════════════════════════════
// ── Python ctypes backend ──
// ═══════════════════════════════════════════════════════════════════

static QString pyTypeName(NodeKind kind) {
    switch (kind) {
    case NodeKind::Hex8:      return QStringLiteral("ctypes.c_uint8");
    case NodeKind::Hex16:     return QStringLiteral("ctypes.c_uint16");
    case NodeKind::Hex32:     return QStringLiteral("ctypes.c_uint32");
    case NodeKind::Hex64:     return QStringLiteral("ctypes.c_uint64");
    case NodeKind::Hex128:    return QStringLiteral("ctypes.c_uint8 * 16");
    case NodeKind::Int8:      return QStringLiteral("ctypes.c_int8");
    case NodeKind::Int16:     return QStringLiteral("ctypes.c_int16");
    case NodeKind::Int32:     return QStringLiteral("ctypes.c_int32");
    case NodeKind::Int64:     return QStringLiteral("ctypes.c_int64");
    case NodeKind::Int128:    return QStringLiteral("ctypes.c_int8 * 16");
    case NodeKind::UInt8:     return QStringLiteral("ctypes.c_uint8");
    case NodeKind::UInt16:    return QStringLiteral("ctypes.c_uint16");
    case NodeKind::UInt32:    return QStringLiteral("ctypes.c_uint32");
    case NodeKind::UInt64:    return QStringLiteral("ctypes.c_uint64");
    case NodeKind::UInt128:   return QStringLiteral("ctypes.c_uint8 * 16");
    case NodeKind::Float16:   return QStringLiteral("ctypes.c_uint16");  // no native half
    case NodeKind::Float:     return QStringLiteral("ctypes.c_float");
    case NodeKind::Double:    return QStringLiteral("ctypes.c_double");
    case NodeKind::Bool:      return QStringLiteral("ctypes.c_bool");
    case NodeKind::Pointer32: return QStringLiteral("ctypes.c_uint32");
    case NodeKind::Pointer64: return QStringLiteral("ctypes.c_uint64");
    case NodeKind::FuncPtr32: return QStringLiteral("ctypes.c_uint32");
    case NodeKind::FuncPtr64: return QStringLiteral("ctypes.c_uint64");
    case NodeKind::Vec2:      return QStringLiteral("ctypes.c_float");
    case NodeKind::Vec3:      return QStringLiteral("ctypes.c_float");
    case NodeKind::Vec4:      return QStringLiteral("ctypes.c_float");
    case NodeKind::Mat4x4:    return QStringLiteral("ctypes.c_float");
    case NodeKind::UTF8:      return QStringLiteral("ctypes.c_char");
    case NodeKind::UTF16:     return QStringLiteral("ctypes.c_wchar");
    default:                  return QStringLiteral("ctypes.c_uint8");
    }
}

// Forward declaration
static void emitPythonStruct(GenContext& ctx, uint64_t structId);

static void emitPythonStructBody(GenContext& ctx, uint64_t structId,
                                  bool isUnion, int baseOffset) {
    const NodeTree& tree = ctx.tree;
    int idx = tree.indexOfId(structId);
    if (idx < 0) return;

    int structSize = tree.structSpan(structId, &ctx.childMap);
    QString ind = QStringLiteral("        ");  // 2 levels for inside _fields_

    auto children = ctx.prepareChildren(structId);

    auto emitPadField = [&](int relOffset, int size) {
        if (size <= 0) return;
        ctx.output += ind + offsetComment(baseOffset + relOffset, ctx.padDigits)
            + QStringLiteral("(\"%1\", ctypes.c_uint8 * 0x%2),")
            .arg(ctx.uniquePadName(baseOffset + relOffset))
            .arg(QString::number(size, 16).toUpper())
            + QStringLiteral("\n");
    };

    int cursor = 0;
    int i = 0;

    while (i < children.size()) {
        const Node& child = tree.nodes[children[i]];
        int childSize;
        if (child.kind == NodeKind::Struct || child.kind == NodeKind::Array)
            childSize = tree.structSpan(child.id, &ctx.childMap);
        else
            childSize = child.byteSize();

        if (!isUnion) {
            if (child.offset > cursor)
                emitPadField(cursor, child.offset - cursor);
        }

        // Collapse hex nodes into padding
        if (isHexNode(child.kind)) {
            int runStart = child.offset;
            int runEnd = child.offset + childSize;
            int j = i + 1;
            while (j < children.size()) {
                const Node& next = tree.nodes[children[j]];
                if (!isHexNode(next.kind)) break;
                int nextSize = next.byteSize();
                if (next.offset < runEnd) break;
                runEnd = next.offset + nextSize;
                j++;
            }
            emitPadField(runStart, runEnd - runStart);
            cursor = runEnd;
            i = j;
            continue;
        }

        int absOffset = baseOffset + child.offset;
        QString name = sanitizeIdent(child.name.isEmpty()
            ? QStringLiteral("field_%1").arg(child.offset, ctx.padDigits, 16, QChar('0'))
            : child.name);
        QString oc = offsetComment(absOffset, ctx.padDigits);

        if (child.kind == NodeKind::Struct) {
            if (child.isBitfield()
                && !child.bitfieldMembers.isEmpty()) {
                QString bfType = pyTypeName(child.elementKind);
                if (bfType.isEmpty()) bfType = QStringLiteral("ctypes.c_uint32");
                QStringList bits;
                for (const auto& m : child.bitfieldMembers)
                    bits << QStringLiteral("%1:%2").arg(sanitizeIdent(m.name)).arg(m.bitWidth);
                ctx.output += ind + oc + QStringLiteral("(\"%1\", %2),")
                    .arg(name, bfType)
                    + QStringLiteral(" # bits: ") + bits.join(QStringLiteral(", "))
                    + QStringLiteral("\n");
            } else if (child.structTypeName.isEmpty()
                       || !ctx.childMap.value(child.id).isEmpty()) {
                // Anonymous inline structs flatten to a byte array; named
                // inline structs (inline-declared type, no top-level
                // definition to reference) get the same treatment.
                int span = tree.structSpan(child.id, &ctx.childMap);
                ctx.output += ind + oc + QStringLiteral("(\"%1\", ctypes.c_uint8 * 0x%2),")
                    .arg(name)
                    .arg(QString::number(span, 16).toUpper())
                    + QStringLiteral("\n");
            } else {
                QString typeName = ctx.nameFor(child);
                ctx.output += ind + oc + QStringLiteral("(\"%1\", %2),")
                    .arg(name, typeName)
                    + QStringLiteral("\n");
            }
        } else if (child.kind == NodeKind::Array) {
            QVector<int> arrayKids = ctx.childMap.value(child.id);
            bool hasStructChild = false;
            QString elemTypeName;
            for (int ak : arrayKids) {
                if (tree.nodes[ak].kind == NodeKind::Struct) {
                    hasStructChild = true;
                    elemTypeName = ctx.nameFor(tree.nodes[ak]);
                    break;
                }
            }
            if (hasStructChild && !elemTypeName.isEmpty()) {
                ctx.output += ind + oc + QStringLiteral("(\"%1\", %2 * %3),")
                    .arg(name, elemTypeName).arg(child.arrayLen) + QStringLiteral("\n");
            } else {
                ctx.output += ind + oc + QStringLiteral("(\"%1\", %2 * %3),")
                    .arg(name, pyTypeName(child.elementKind)).arg(child.arrayLen)
                    + QStringLiteral("\n");
            }
        } else {
            // Primitive fields
            switch (child.kind) {
            case NodeKind::Vec2:
                ctx.output += ind + oc + QStringLiteral("(\"%1\", ctypes.c_float * 2),").arg(name)
                    + QStringLiteral("\n");
                break;
            case NodeKind::Vec3:
                ctx.output += ind + oc + QStringLiteral("(\"%1\", ctypes.c_float * 3),").arg(name)
                    + QStringLiteral("\n");
                break;
            case NodeKind::Vec4:
                ctx.output += ind + oc + QStringLiteral("(\"%1\", ctypes.c_float * 4),").arg(name)
                    + QStringLiteral("\n");
                break;
            case NodeKind::Mat4x4:
                ctx.output += ind + oc + QStringLiteral("(\"%1\", (ctypes.c_float * 4) * 4),").arg(name)
                    + QStringLiteral("\n");
                break;
            case NodeKind::UTF8:
                ctx.output += ind + oc + QStringLiteral("(\"%1\", ctypes.c_char * %2),").arg(name)
                    .arg(child.strLen) + QStringLiteral("\n");
                break;
            case NodeKind::UTF16:
                ctx.output += ind + oc + QStringLiteral("(\"%1\", ctypes.c_wchar * %2),").arg(name)
                    .arg(child.strLen) + QStringLiteral("\n");
                break;
            case NodeKind::Pointer32:
            case NodeKind::Pointer64: {
                bool isNativePtr = (child.kind == NodeKind::Pointer32 && ctx.tree.pointerSize <= 4)
                                || (child.kind == NodeKind::Pointer64 && ctx.tree.pointerSize >= 8);
                QString vfNote = vfEntriesComment(tree, child);
                if (child.refId != 0) {
                    int refIdx = tree.indexOfId(child.refId);
                    if (refIdx >= 0) {
                        QString target = ctx.nameFor(tree.nodes[refIdx]);
                        ctx.output += ind + oc + QStringLiteral("(\"%1\", ctypes.POINTER(%2)),").arg(name, target)
                            + vfNote + QStringLiteral("\n");
                        break;
                    }
                }
                if (isNativePtr)
                    ctx.output += ind + oc + QStringLiteral("(\"%1\", ctypes.c_void_p),").arg(name)
                        + vfNote + QStringLiteral("\n");
                else
                    ctx.output += ind + oc + QStringLiteral("(\"%1\", %2),").arg(name, pyTypeName(child.kind))
                        + vfNote + QStringLiteral("\n");
                break;
            }
            case NodeKind::FuncPtr32:
            case NodeKind::FuncPtr64:
                ctx.output += ind + oc + QStringLiteral("(\"%1\", ctypes.CFUNCTYPE(None)),").arg(name)
                    + QStringLiteral("\n");
                break;
            default:
                ctx.output += ind + oc + QStringLiteral("(\"%1\", %2),").arg(name, pyTypeName(child.kind))
                    + QStringLiteral("\n");
                break;
            }
        }

        int childEnd = child.offset + childSize;
        if (childEnd > cursor) cursor = childEnd;
        i++;
    }

    // Tail padding
    if (!isUnion && cursor < structSize)
        emitPadField(cursor, structSize - cursor);
}

static void emitPythonStruct(GenContext& ctx, uint64_t structId) {
    if (ctx.emittedIds.contains(structId)) return;
    if (ctx.visiting.contains(structId)) return;
    ctx.visiting.insert(structId);

    int idx = ctx.tree.indexOfId(structId);
    if (idx < 0) { ctx.visiting.remove(structId); return; }

    const Node& node = ctx.tree.nodes[idx];
    if (node.kind != NodeKind::Struct) { ctx.visiting.remove(structId); return; }

    QString typeName = ctx.nameFor(node);
    if (ctx.emittedTypeNames.contains(typeName)) {
        ctx.emittedIds.insert(structId);
        ctx.visiting.remove(structId);
        return;
    }

    ctx.emittedIds.insert(structId);
    ctx.emittedTypeNames.insert(typeName);
    // A union's size in ctypes is the largest member, not the extent of its
    // members' offsets — use unionSize so the emitted size comment matches
    // what ctypes computes.
    int structSize = node.isUnion()
        ? ctx.tree.unionSize(structId, &ctx.childMap)
        : ctx.tree.structSpan(structId, &ctx.childMap);

    QString kw = node.resolvedClassKeyword();

    // Enum with members — emit as class with constants
    if (kw == QStringLiteral("enum") && !node.enumMembers.isEmpty()) {
        ctx.output += QStringLiteral("class %1:  # enum\n    __slots__ = ()\n").arg(typeName);
        for (const auto& m : node.enumMembers) {
            ctx.output += QStringLiteral("    %1 = %2\n")
                .arg(sanitizeIdent(m.first))
                .arg(m.second);
        }
        ctx.output += QStringLiteral("\n");
        ctx.visiting.remove(structId);
        return;
    }

    ctx.padDigits = digitsForSize(structSize);
    ctx.usedPadNames.clear();

    bool isUnion = (kw == QStringLiteral("union"));
    QString baseClass = isUnion ? QStringLiteral("ctypes.Union") : QStringLiteral("ctypes.Structure");

    ctx.output += QStringLiteral("class %1(%2):").arg(typeName, baseClass)
        + offsetComment(structSize, 0, true) + QStringLiteral("\n");
    ctx.output += QStringLiteral("    _fields_ = [\n");

    emitPythonStructBody(ctx, structId, isUnion, 0);

    ctx.output += QStringLiteral("    ]\n");
    ctx.output += QStringLiteral("\n");

    ctx.visiting.remove(structId);
}

// ═══════════════════════════════════════════════════════════════════
// ── Reachable struct collector (for "Current + Children" scope) ──
// ═══════════════════════════════════════════════════════════════════

// Walk the tree from rootId, collecting all struct IDs reachable via
// named struct children and pointer references. Returns them in
// dependency order (leaves first, root last).
static QVector<uint64_t> collectReachableStructs(
    const NodeTree& tree, const QHash<uint64_t, QVector<int>>& childMap,
    uint64_t rootId)
{
    QVector<uint64_t> result;
    QSet<uint64_t> visited;

    std::function<void(uint64_t)> walk = [&](uint64_t id) {
        if (visited.contains(id)) return;
        visited.insert(id);

        int idx = tree.indexOfId(id);
        if (idx < 0) return;
        const Node& node = tree.nodes[idx];
        if (node.kind != NodeKind::Struct) return;

        // Walk children first so dependencies come before the parent
        for (int ci : childMap.value(id)) {
            const Node& child = tree.nodes[ci];
            if (child.kind == NodeKind::Struct && !child.structTypeName.isEmpty())
                walk(child.id);
            if ((child.kind == NodeKind::Pointer32 || child.kind == NodeKind::Pointer64)
                && child.refId != 0)
                walk(child.refId);
            if (child.kind == NodeKind::Array) {
                for (int ak : childMap.value(child.id))
                    if (tree.nodes[ak].kind == NodeKind::Struct)
                        walk(tree.nodes[ak].id);
            }
        }
        result.append(id);
    };
    walk(rootId);
    return result;
}

} // anonymous namespace

// ── Public API ──

const char* codeFormatName(CodeFormat fmt) {
    switch (fmt) {
    case CodeFormat::CppHeader:     return "C/C++";
    case CodeFormat::RustStruct:    return "Rust";
    case CodeFormat::DefineOffsets: return "#define";
    case CodeFormat::CSharpStruct:  return "C#";
    case CodeFormat::PythonCtypes:  return "Python";
    default:                        return "C/C++";
    }
}

const char* codeFormatFileFilter(CodeFormat fmt) {
    switch (fmt) {
    case CodeFormat::CppHeader:     return "C++ Header (*.h);;All Files (*)";
    case CodeFormat::RustStruct:    return "Rust Source (*.rs);;All Files (*)";
    case CodeFormat::DefineOffsets: return "C Header (*.h);;All Files (*)";
    case CodeFormat::CSharpStruct:  return "C# Source (*.cs);;All Files (*)";
    case CodeFormat::PythonCtypes:  return "Python Source (*.py);;All Files (*)";
    default:                        return "All Files (*)";
    }
}

const char* codeScopeName(CodeScope scope) {
    switch (scope) {
    case CodeScope::Current:       return "Current";
    case CodeScope::WithChildren:  return "Current + Deps";
    case CodeScope::FullSdk:       return "Full SDK";
    default:                       return "Current";
    }
}

QString renderCpp(const NodeTree& tree, uint64_t rootStructId,
                  const QHash<NodeKind, QString>* typeAliases,
                  bool emitAsserts, bool privatePads) {
    int idx = tree.indexOfId(rootStructId);
    if (idx < 0) return {};

    const Node& root = tree.nodes[idx];
    if (root.kind != NodeKind::Struct) return {};

    GenContext ctx{tree, buildChildMap(tree), {}, {}, {}, {}, {}, typeAliases, emitAsserts, {}, privatePads};
    ctx.prepare();
    ctx.assignUniqueNames();

    ctx.output += QStringLiteral("#pragma once\n#include <cstdint>\n\n");

    emitStruct(ctx, rootStructId);

    return alignComments(ctx.output);
}

QString renderCppTree(const NodeTree& tree, uint64_t rootStructId,
                      const QHash<NodeKind, QString>* typeAliases,
                      bool emitAsserts, bool privatePads) {
    int idx = tree.indexOfId(rootStructId);
    if (idx < 0) return {};
    if (tree.nodes[idx].kind != NodeKind::Struct) return {};

    auto childMap = buildChildMap(tree);
    GenContext ctx{tree, childMap, {}, {}, {}, {}, {}, typeAliases, emitAsserts, {}, privatePads};
    ctx.prepare();
    ctx.assignUniqueNames();
    ctx.output += QStringLiteral("#pragma once\n#include <cstdint>\n\n");

    for (uint64_t sid : collectReachableStructs(tree, childMap, rootStructId))
        emitStruct(ctx, sid);

    return alignComments(ctx.output);
}

QString renderCppAll(const NodeTree& tree,
                     const QHash<NodeKind, QString>* typeAliases,
                     bool emitAsserts, bool privatePads) {
    GenContext ctx{tree, buildChildMap(tree), {}, {}, {}, {}, {}, typeAliases, emitAsserts, {}, privatePads};
    ctx.prepare();
    ctx.assignUniqueNames();

    ctx.output += QStringLiteral("#pragma once\n#include <cstdint>\n\n");

    QVector<int> roots = ctx.childMap.value(0);
    std::sort(roots.begin(), roots.end(), [&](int a, int b) {
        return tree.nodes[a].offset < tree.nodes[b].offset;
    });

    for (int ri : roots) {
        if (tree.nodes[ri].kind == NodeKind::Struct)
            emitStruct(ctx, tree.nodes[ri].id);
    }

    return alignComments(ctx.output);
}

// ── Rust public API ──

QString renderRust(const NodeTree& tree, uint64_t rootStructId,
                   const QHash<NodeKind, QString>* typeAliases,
                   bool emitAsserts) {
    int idx = tree.indexOfId(rootStructId);
    if (idx < 0) return {};
    if (tree.nodes[idx].kind != NodeKind::Struct) return {};

    GenContext ctx{tree, buildChildMap(tree), {}, {}, {}, {}, {}, typeAliases, emitAsserts};
    ctx.prepare();
    ctx.assignUniqueNames();
    ctx.output += QStringLiteral("// Generated by REECLASS 2027\n\n");
    emitRustStruct(ctx, rootStructId);
    return alignComments(ctx.output);
}

QString renderRustTree(const NodeTree& tree, uint64_t rootStructId,
                       const QHash<NodeKind, QString>* typeAliases,
                       bool emitAsserts) {
    int idx = tree.indexOfId(rootStructId);
    if (idx < 0) return {};
    if (tree.nodes[idx].kind != NodeKind::Struct) return {};

    auto childMap = buildChildMap(tree);
    GenContext ctx{tree, childMap, {}, {}, {}, {}, {}, typeAliases, emitAsserts};
    ctx.prepare();
    ctx.assignUniqueNames();
    ctx.output += QStringLiteral("// Generated by REECLASS 2027\n\n");

    for (uint64_t sid : collectReachableStructs(tree, childMap, rootStructId))
        emitRustStruct(ctx, sid);

    return alignComments(ctx.output);
}

QString renderRustAll(const NodeTree& tree,
                      const QHash<NodeKind, QString>* typeAliases,
                      bool emitAsserts) {
    GenContext ctx{tree, buildChildMap(tree), {}, {}, {}, {}, {}, typeAliases, emitAsserts};
    ctx.prepare();
    ctx.assignUniqueNames();
    ctx.output += QStringLiteral("// Generated by REECLASS 2027\n\n");

    QVector<int> roots = ctx.childMap.value(0);
    std::sort(roots.begin(), roots.end(), [&](int a, int b) {
        return tree.nodes[a].offset < tree.nodes[b].offset;
    });
    for (int ri : roots) {
        if (tree.nodes[ri].kind == NodeKind::Struct)
            emitRustStruct(ctx, tree.nodes[ri].id);
    }
    return alignComments(ctx.output);
}

// ── #define public API ──

QString renderDefines(const NodeTree& tree, uint64_t rootStructId) {
    int idx = tree.indexOfId(rootStructId);
    if (idx < 0) return {};
    if (tree.nodes[idx].kind != NodeKind::Struct) return {};

    GenContext ctx{tree, buildChildMap(tree), {}, {}, {}, {}, {}, nullptr, false};
    ctx.prepare();
    ctx.assignUniqueNames();
    ctx.output += QStringLiteral("#pragma once\n#include <cstdint>\n\n");
    emitDefinesForStruct(ctx, rootStructId, QString(), 0);
    return ctx.output;
}

QString renderDefinesTree(const NodeTree& tree, uint64_t rootStructId) {
    int idx = tree.indexOfId(rootStructId);
    if (idx < 0) return {};
    if (tree.nodes[idx].kind != NodeKind::Struct) return {};

    auto childMap = buildChildMap(tree);
    GenContext ctx{tree, childMap, {}, {}, {}, {}, {}, nullptr, false};
    ctx.prepare();
    ctx.assignUniqueNames();
    ctx.output += QStringLiteral("#pragma once\n#include <cstdint>\n\n");

    for (uint64_t sid : collectReachableStructs(tree, childMap, rootStructId))
        emitDefinesForStruct(ctx, sid, QString(), 0);

    return ctx.output;
}

QString renderDefinesAll(const NodeTree& tree) {
    GenContext ctx{tree, buildChildMap(tree), {}, {}, {}, {}, {}, nullptr, false};
    ctx.prepare();
    ctx.assignUniqueNames();
    ctx.output += QStringLiteral("#pragma once\n#include <cstdint>\n\n");

    QVector<int> roots = ctx.childMap.value(0);
    std::sort(roots.begin(), roots.end(), [&](int a, int b) {
        return tree.nodes[a].offset < tree.nodes[b].offset;
    });
    for (int ri : roots) {
        if (tree.nodes[ri].kind == NodeKind::Struct)
            emitDefinesForStruct(ctx, tree.nodes[ri].id, QString(), 0);
    }
    return ctx.output;
}

// ── C# public API ──

QString renderCSharp(const NodeTree& tree, uint64_t rootStructId,
                     const QHash<NodeKind, QString>* typeAliases,
                     bool emitAsserts) {
    int idx = tree.indexOfId(rootStructId);
    if (idx < 0) return {};
    if (tree.nodes[idx].kind != NodeKind::Struct) return {};

    GenContext ctx{tree, buildChildMap(tree), {}, {}, {}, {}, {}, typeAliases, emitAsserts};
    ctx.prepare();
    ctx.assignUniqueNames();
    ctx.output += QStringLiteral("using System.Runtime.InteropServices;\n#nullable disable\n\n");
    emitCSharpStruct(ctx, rootStructId);
    return alignComments(ctx.output);
}

QString renderCSharpTree(const NodeTree& tree, uint64_t rootStructId,
                         const QHash<NodeKind, QString>* typeAliases,
                         bool emitAsserts) {
    int idx = tree.indexOfId(rootStructId);
    if (idx < 0) return {};
    if (tree.nodes[idx].kind != NodeKind::Struct) return {};

    auto childMap = buildChildMap(tree);
    GenContext ctx{tree, childMap, {}, {}, {}, {}, {}, typeAliases, emitAsserts};
    ctx.prepare();
    ctx.assignUniqueNames();
    ctx.output += QStringLiteral("using System.Runtime.InteropServices;\n#nullable disable\n\n");

    for (uint64_t sid : collectReachableStructs(tree, childMap, rootStructId))
        emitCSharpStruct(ctx, sid);

    return alignComments(ctx.output);
}

QString renderCSharpAll(const NodeTree& tree,
                        const QHash<NodeKind, QString>* typeAliases,
                        bool emitAsserts) {
    GenContext ctx{tree, buildChildMap(tree), {}, {}, {}, {}, {}, typeAliases, emitAsserts};
    ctx.prepare();
    ctx.assignUniqueNames();
    ctx.output += QStringLiteral("using System.Runtime.InteropServices;\n#nullable disable\n\n");

    QVector<int> roots = ctx.childMap.value(0);
    std::sort(roots.begin(), roots.end(), [&](int a, int b) {
        return tree.nodes[a].offset < tree.nodes[b].offset;
    });
    for (int ri : roots) {
        if (tree.nodes[ri].kind == NodeKind::Struct)
            emitCSharpStruct(ctx, tree.nodes[ri].id);
    }
    return alignComments(ctx.output);
}

// ── Python public API ──

QString renderPython(const NodeTree& tree, uint64_t rootStructId) {
    int idx = tree.indexOfId(rootStructId);
    if (idx < 0) return {};
    if (tree.nodes[idx].kind != NodeKind::Struct) return {};

    GenContext ctx{tree, buildChildMap(tree), {}, {}, {}, {}, {}, nullptr, false};
    ctx.prepare();
    ctx.assignUniqueNames();
    ctx.output += QStringLiteral("import ctypes\n\n");
    emitPythonStruct(ctx, rootStructId);
    return alignComments(ctx.output);
}

QString renderPythonTree(const NodeTree& tree, uint64_t rootStructId) {
    int idx = tree.indexOfId(rootStructId);
    if (idx < 0) return {};
    if (tree.nodes[idx].kind != NodeKind::Struct) return {};

    auto childMap = buildChildMap(tree);
    GenContext ctx{tree, childMap, {}, {}, {}, {}, {}, nullptr, false};
    ctx.prepare();
    ctx.assignUniqueNames();
    ctx.output += QStringLiteral("import ctypes\n\n");

    for (uint64_t sid : collectReachableStructs(tree, childMap, rootStructId))
        emitPythonStruct(ctx, sid);

    return alignComments(ctx.output);
}

QString renderPythonAll(const NodeTree& tree) {
    GenContext ctx{tree, buildChildMap(tree), {}, {}, {}, {}, {}, nullptr, false};
    ctx.prepare();
    ctx.assignUniqueNames();
    ctx.output += QStringLiteral("import ctypes\n\n");

    QVector<int> roots = ctx.childMap.value(0);
    std::sort(roots.begin(), roots.end(), [&](int a, int b) {
        return tree.nodes[a].offset < tree.nodes[b].offset;
    });
    for (int ri : roots) {
        if (tree.nodes[ri].kind == NodeKind::Struct)
            emitPythonStruct(ctx, tree.nodes[ri].id);
    }
    return alignComments(ctx.output);
}

// ── Format dispatch ──

QString renderCode(CodeFormat fmt, const NodeTree& tree, uint64_t rootStructId,
                   const QHash<NodeKind, QString>* typeAliases, bool emitAsserts,
                   bool privatePads) {
    switch (fmt) {
    case CodeFormat::RustStruct:    return renderRust(tree, rootStructId, typeAliases, emitAsserts);
    case CodeFormat::DefineOffsets: return renderDefines(tree, rootStructId);
    case CodeFormat::CSharpStruct:  return renderCSharp(tree, rootStructId, typeAliases, emitAsserts);
    case CodeFormat::PythonCtypes:  return renderPython(tree, rootStructId);
    default:                        return renderCpp(tree, rootStructId, typeAliases, emitAsserts, privatePads);
    }
}

QString renderCodeTree(CodeFormat fmt, const NodeTree& tree, uint64_t rootStructId,
                       const QHash<NodeKind, QString>* typeAliases, bool emitAsserts,
                       bool privatePads) {
    switch (fmt) {
    case CodeFormat::RustStruct:    return renderRustTree(tree, rootStructId, typeAliases, emitAsserts);
    case CodeFormat::DefineOffsets: return renderDefinesTree(tree, rootStructId);
    case CodeFormat::CSharpStruct:  return renderCSharpTree(tree, rootStructId, typeAliases, emitAsserts);
    case CodeFormat::PythonCtypes:  return renderPythonTree(tree, rootStructId);
    default:                        return renderCppTree(tree, rootStructId, typeAliases, emitAsserts, privatePads);
    }
}

QString renderCodeAll(CodeFormat fmt, const NodeTree& tree,
                      const QHash<NodeKind, QString>* typeAliases, bool emitAsserts,
                      bool privatePads) {
    switch (fmt) {
    case CodeFormat::RustStruct:    return renderRustAll(tree, typeAliases, emitAsserts);
    case CodeFormat::DefineOffsets: return renderDefinesAll(tree);
    case CodeFormat::CSharpStruct:  return renderCSharpAll(tree, typeAliases, emitAsserts);
    case CodeFormat::PythonCtypes:  return renderPythonAll(tree);
    default:                        return renderCppAll(tree, typeAliases, emitAsserts, privatePads);
    }
}

QString renderNull(const NodeTree&, uint64_t) {
    return {};
}

} // namespace rcx
