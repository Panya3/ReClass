#pragma once
#include "core.h"
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QString>
#include <QVector>

namespace rcx {

// ── Session (open-tabs) persistence ──
//
// The session file records which tabs were open and what each tab was
// viewing, so a restart can land the user back where they left off. It is
// deliberately separate from the .rcx project format (which keeps its own
// kRcxFileVersion): the open-tab set is per-user UI state, not project
// data, and a shared .rcx should not carry one viewer's tab layout. The
// session file carries its own version (kSessionFileVersion) for the same
// forward-compat reason.

constexpr int kSessionFileVersion = 1;

struct SessionTab {
    int      docIndex   = -1;   // index into Session::docs
    uint64_t viewRootId = 0;    // struct the tab is viewing
    QString  title;             // tab title (struct name)
};

struct SessionDoc {
    QString    filePath;     // empty = untitled document
    QString    title;        // doc label (root name)
    bool       hasContent = false;   // true when a content snapshot is embedded
    QByteArray contentJson;  // tree.toJson() + typeAliases (untitled docs only)
};

struct Session {
    int                version   = kSessionFileVersion;
    int                activeTab = -1;   // index into tabs; -1 = none
    QVector<SessionDoc> docs;
    QVector<SessionTab> tabs;
};

// Serialize a session to JSON. Untitled-doc content is embedded as a nested
// object; saved docs travel by path alone (their bytes live in the .rcx,
// plus the autosave shadow for unsaved edits).
inline QJsonObject sessionToJson(const Session& s) {
    QJsonObject o;
    o["sessionVersion"] = kSessionFileVersion;
    o["activeTab"] = s.activeTab;

    QJsonArray docs;
    for (const auto& d : s.docs) {
        QJsonObject doj;
        doj["filePath"] = d.filePath;
        if (!d.title.isEmpty())
            doj["title"] = d.title;
        if (d.hasContent) {
            QJsonParseError e;
            QJsonDocument cd = QJsonDocument::fromJson(d.contentJson, &e);
            if (!cd.isNull() && cd.isObject())
                doj["content"] = cd.object();
        }
        docs.append(doj);
    }
    o["docs"] = docs;

    QJsonArray tabs;
    for (const auto& t : s.tabs) {
        QJsonObject toj;
        toj["doc"] = t.docIndex;
        toj["viewRootId"] = QString::number(t.viewRootId);
        if (!t.title.isEmpty())
            toj["title"] = t.title;
        tabs.append(toj);
    }
    o["tabs"] = tabs;
    return o;
}

// Parse a session file. Returns false for a missing/unknown version — the
// caller then falls back to the normal startup (e.g. start page) rather
// than half-restoring a file from a newer build.
inline bool sessionFromJson(const QJsonObject& o, Session& out) {
    const int v = o["sessionVersion"].toInt(0);
    if (v < 1 || v > kSessionFileVersion)
        return false;
    out = Session{};
    out.version = v;
    out.activeTab = o["activeTab"].toInt(-1);

    const QJsonArray docs = o["docs"].toArray();
    out.docs.reserve(docs.size());
    for (const auto& vv : docs) {
        const QJsonObject doj = vv.toObject();
        SessionDoc d;
        d.filePath = doj["filePath"].toString();
        d.title    = doj["title"].toString();
        if (doj.contains("content") && doj["content"].isObject()) {
            d.contentJson = QJsonDocument(doj["content"].toObject())
                                .toJson(QJsonDocument::Compact);
            d.hasContent = true;
        }
        out.docs.append(d);
    }

    const QJsonArray tabs = o["tabs"].toArray();
    out.tabs.reserve(tabs.size());
    for (const auto& vv : tabs) {
        const QJsonObject toj = vv.toObject();
        SessionTab t;
        t.docIndex   = toj["doc"].toInt(-1);
        t.viewRootId = toj["viewRootId"].toString("0").toULongLong();
        t.title      = toj["title"].toString();
        out.tabs.append(t);
    }
    return true;
}

// ── Restore-domain helpers (pure; unit-tested in test_workspace.cpp) ──

// Resolve the view root a restored tab should show. 0 is the VALID "show
// all roots" view (a tab left on the document overview) and must be kept
// as-is; only a NONZERO id that no longer exists in the tree (stale
// session / file edited elsewhere) falls back to the first top-level
// struct so the tab lands somewhere useful. Returns 0 when the tree has
// no root struct to fall back to.
inline uint64_t resolveRestoredViewRoot(const NodeTree& tree, uint64_t recorded) {
    if (recorded != 0 && tree.indexOfId(recorded) < 0) {
        for (const auto& n : tree.nodes)
            if (n.parentId == 0 && n.kind == NodeKind::Struct)
                return n.id;
        return 0;
    }
    return recorded;
}

// True when a fresher autosave shadow sits next to the real file — the
// same check the interactive open flow (MainWindow::project_open) uses, so
// a session restore recovers in-flight edits instead of silently showing
// the last committed version on disk.
inline bool shadowIsFresher(const QString& filePath) {
    const QString shadow = filePath + QStringLiteral(".autosave");
    const QFileInfo orig(filePath);
    const QFileInfo sh(shadow);
    return sh.exists() && sh.isFile()
        && sh.lastModified() > orig.lastModified();
}

// Index into Session::docs of the saved doc whose filePath matches
// `filePath` (path-normalized on both sides), or -1 when nothing matches
// (including untitled docs — they have no path to match on). Lets a file
// open (project_open) look up the tab layout the session remembered for
// that exact file.
inline int sessionDocIndexForPath(const Session& s, const QString& filePath) {
    if (filePath.isEmpty()) return -1;
    const QString norm = QFileInfo(filePath).absoluteFilePath();
    for (int i = 0; i < s.docs.size(); ++i) {
        if (s.docs[i].filePath.isEmpty()) continue;
        if (QFileInfo(s.docs[i].filePath).absoluteFilePath() == norm)
            return i;
    }
    return -1;
}

} // namespace rcx
