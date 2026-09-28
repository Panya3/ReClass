#pragma once

// Navigation history for the address bar's Back / Forward — the Explorer
// model, not the undo model. An entry is a PLACE (which class you were
// viewing, how deep you had drilled, what the base was, which source was
// active); going back restores that place without touching the undo stack,
// and undo/redo never touch this history. Kept pure (Qt containers only) so
// it is testable without a controller.
//
// RcxController owns one: it records at its navigation gestures (F12,
// rebase, source switch, sibling / root pick, path edit, crumb click, the
// line-0 root pick) and drives goBack / goForward / jumpToHistory from it.
// Wired in P5; see docs/ADDRESS_BAR.md for the history-vs-undo rule.

#include "core.h"

#include <QString>
#include <QVector>
#include <functional>
#include <optional>

namespace rcx {

// activeSourceIdx of an entry whose saved source has since been removed
// (or all sources cleared): the place is still restorable — root, trail,
// base — but under whatever source is current, and the restore says so.
// Distinct from -1, which is "no source was active" (a fresh document) and
// restores silently.
inline constexpr int kNavSourceRemoved = -2;

struct NavEntry {
    uint64_t          viewRootId = 0;   // RcxController::m_viewRootId (0 = show-all)
    QVector<uint64_t> focusPath;        // RcxController::m_focusPath
    uint64_t          baseAddress = 0;  // NodeTree::baseAddress
    QString           baseFormula;      // NodeTree::baseAddressFormula
    int               activeSourceIdx = -1;   // index into the controller's saved
                                              // sources; -1 none; kNavSourceRemoved
    // Presentation only — where to scroll on restore, what the history
    // menu prints. Two entries at the same place with different anchors or
    // labels are still the same place.
    uint64_t          anchorNodeId = 0;
    QString           label;

    bool samePlace(const NavEntry& o) const {
        return viewRootId == o.viewRootId
            && focusPath == o.focusPath
            && baseAddress == o.baseAddress
            && baseFormula == o.baseFormula
            && activeSourceIdx == o.activeSourceIdx;
    }
};

// The place an entry names must still exist in the tree before it can be
// restored: a deleted root or a pointer that was removed leaves a stale
// entry, and back()/forward() skip those rather than restoring a torn
// state. 0 as viewRootId is show-all, which always resolves.
inline bool navEntryValid(const NodeTree& tree, const NavEntry& e) {
    if (e.viewRootId != 0 && tree.indexOfId(e.viewRootId) < 0) return false;
    for (uint64_t id : e.focusPath)
        if (tree.indexOfId(id) < 0) return false;
    return true;
}

class NavHistory {
public:
    // Explorer keeps a short list; 50 is more than anyone walks back through
    // and bounds the memory of a long live session that rebases every few
    // seconds through a scanner.
    static constexpr int kMax = 50;

    // Record the place being LEFT. Called before a navigation gesture changes
    // the controller's state. Pushing the place already on top is a no-op so
    // a gesture that lands where it started (rebase to the same address,
    // re-selecting the current root) never grows the stack; any push
    // truncates the forward stack, as it does in every browser.
    void push(const NavEntry& e) {
        if (!m_back.isEmpty() && m_back.last().samePlace(e)) return;
        m_forward.clear();
        m_back.push_back(e);
        while (m_back.size() > kMax) m_back.removeFirst();
    }

    bool canBack() const { return !m_back.isEmpty(); }
    bool canForward() const { return !m_forward.isEmpty(); }

    // Pop back to the most recent entry that still passes `valid`, moving
    // `current` (the place being left) onto the forward stack. Entries that
    // fail `valid` are discarded on the way — they name places that no
    // longer exist. std::nullopt when nothing restorable remains; then
    // nothing was moved either, so the caller's state is untouched.
    std::optional<NavEntry> back(const NavEntry& current,
                                 const std::function<bool(const NavEntry&)>& valid) {
        return step(m_back, m_forward, current, valid);
    }
    std::optional<NavEntry> forward(const NavEntry& current,
                                    const std::function<bool(const NavEntry&)>& valid) {
        return step(m_forward, m_back, current, valid);
    }

    const QVector<NavEntry>& backEntries() const { return m_back; }
    const QVector<NavEntry>& forwardEntries() const { return m_forward; }

    void clear() { m_back.clear(); m_forward.clear(); }

    // The controller's saved-source list changed under the entries'
    // indices. An entry is a raw index into that list, so a removal must
    // be mirrored here or an old entry silently restores a DIFFERENT
    // source: the one that slid into the removed slot.
    //   forgetSource(i)  — the entry at i was removed: entries pointing at
    //                      it become kNavSourceRemoved, later ones shift
    //                      down by one (the list did).
    //   forgetAllSources — Clear All: every recorded source is gone.
    void forgetSource(int removedIdx) {
        if (removedIdx < 0) return;
        auto remap = [removedIdx](QVector<NavEntry>& stack) {
            for (NavEntry& e : stack) {
                if (e.activeSourceIdx == removedIdx)     e.activeSourceIdx = kNavSourceRemoved;
                else if (e.activeSourceIdx > removedIdx) --e.activeSourceIdx;
            }
        };
        remap(m_back);
        remap(m_forward);
    }
    void forgetAllSources() {
        for (NavEntry& e : m_back)    if (e.activeSourceIdx >= 0) e.activeSourceIdx = kNavSourceRemoved;
        for (NavEntry& e : m_forward) if (e.activeSourceIdx >= 0) e.activeSourceIdx = kNavSourceRemoved;
    }

private:
    static std::optional<NavEntry> step(QVector<NavEntry>& from, QVector<NavEntry>& to,
                                        const NavEntry& current,
                                        const std::function<bool(const NavEntry&)>& valid) {
        while (!from.isEmpty()) {
            NavEntry e = from.takeLast();
            if (valid && !valid(e)) continue;
            to.push_back(current);
            while (to.size() > kMax) to.removeFirst();
            return e;
        }
        return std::nullopt;
    }

    QVector<NavEntry> m_back;
    QVector<NavEntry> m_forward;
};

} // namespace rcx
