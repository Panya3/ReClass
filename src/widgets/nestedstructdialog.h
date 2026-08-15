#pragma once

#include "widgets/themed_dialog.h"
#include "core.h"
#include <QTreeWidget>

class QLineEdit;
class QComboBox;
class QLabel;

namespace rcx {

// Recursive tree editor behind "Insert Nested Struct...". The member being
// created is a container (struct/union/class) with an optional inline type
// name; its children are entered as a tree where each row picks a type
// (primitive or a nested container) and a name, and container rows expand
// with their own children — unlimited depth in one shot.
//
// Columns: Offset (read-only, auto-packed: sequential + natural alignment
// for struct/class, all-at-0 for union) | Type (combo) | Name (editable) |
// Type name (inline type of container rows). The offset preview mirrors
// the controller's layout pass exactly, so what you see is what
// insertNestedStruct builds.
//
// Row type combos behave like the Insert Field dialog's type combo: they
// are editable (typing filters the list, case-insensitive substring) and
// height-bounded (never grows with the item count). Text that resolves to
// a known type is accepted; ambiguous/unknown text blocks OK until fixed.
// Children of a row are only pruned on a committed selection (combo index
// change), never while the user is typing free text.
class NestedStructDialog : public ThemedDialog {
    Q_OBJECT
public:
    explicit NestedStructDialog(int defaultOffset, QWidget* parent = nullptr);    // Result accessors — valid after exec() == Accepted.
    void collectResult(QString& name, QString& typeName, QString& keyword,
                       int& offset, QVector<NestedStructSpec>& children) const;

    // One tree row: primitive kind, or a container (keyword + optional
    // inline type name). Public so the row-combo helpers can use it.
    struct RowData {
        NodeKind kind = NodeKind::Hex64;
        QString  keyword;       // struct/union/class for container rows
        QString  typeName;      // optional inline type name (containers only)
    };

protected:
    void applyTheme() override;

private slots:
    void onAddField();
    void onAddContainer();
    void onRemoveRow();
    void onMemberChanged();     // member keyword/offset edits → re-pack preview
    void onComboChanged(int index);
    void onOkClicked();

private:
    void restyle();

    static RowData rowData(const QTreeWidgetItem* item);
    void setRowData(QTreeWidgetItem* item, const RowData& data);
    QTreeWidgetItem* makeRow(const RowData& data, const QString& name);
    // Attach a row's type combo + completer. Must run AFTER the item is in
    // the tree — Qt's setItemWidget does nothing for unattached items.
    void installRowWidget(QTreeWidgetItem* item, const RowData& data);
    void collectChildren(const QTreeWidgetItem* parent,
                         QVector<NestedStructSpec>& out) const;
    QTreeWidgetItem* selectedContainer() const;   // selected row if it's a container
    // Find the row whose type combo is `combo` (depth-first scan of the
    // itemWidget map). Robust where a geometry hit-test is not — works even
    // before the dialog is laid out/shown.
    QTreeWidgetItem* rowForCombo(const QComboBox* combo) const;
    // Resolve typed combo text against the combo's own items: exact text,
    // case-insensitive exact, then a unique case-insensitive substring.
    // Returns false when the text is unknown or ambiguous (same policy as
    // the Insert Field dialog's type combo).
    static bool resolveComboText(const QComboBox* combo, const QString& text,
                                 RowData& out);
    // First row (depth-first) whose type text resolves to nothing, or an
    // empty string when every row is valid.
    QString firstUnresolvedTypeRow(const QTreeWidgetItem* parent) const;
    // Drop children of rows whose resolved type is a primitive — typing a
    // new type never prunes live (safety), so a row switched to a primitive
    // can still carry orphan children that insert would silently omit.
    // Runs on OK so what gets inserted matches what is displayed.
    void pruneInconsistentRows();

    // Offset preview: pack one container's children (top-level items when
    // `container` is null — the member's own children) and write the offset
    // column. Rows whose offset the user typed (manual override, flagged via
    // Qt::UserRole on the item) keep their text and pack around it. Returns
    // the container's {size, alignment}.
    QPair<int, int> packChildren(QTreeWidget* tree,
                                 QTreeWidgetItem* container, bool isUnion);
    static int parseOffset(const QString& text, bool* ok);

    // Guard: packChildren's own setText() must not be mistaken for a user
    // edit in onOffsetEdited.
    int m_packingDepth = 0;

    QLineEdit*   m_nameEdit     = nullptr;
    QLineEdit*   m_typeNameEdit = nullptr;
    QComboBox*   m_keywordCombo = nullptr;
    QLineEdit*   m_offsetEdit   = nullptr;
    QTreeWidget* m_tree         = nullptr;
    QLabel*      m_status       = nullptr;
};

} // namespace rcx
