#pragma once
#include "widgets/themed_dialog.h"
#include "widgets/dialog_button.h"
#include "core.h"
#include <QLineEdit>
#include <QComboBox>
#include <QLabel>
#include <QFormLayout>
#include <functional>

class QPushButton;

namespace rcx {

// Modal dialog for the "new variable declaration" flow. Four modes:
//
//   InsertField  — declare a brand-new field: offset, type (full kind list,
//                  editable + filterable combo), name (optional, auto
//                  "field_XX" when empty). A conflicting offset does NOT
//                  block the commit: the controller pushes existing fields
//                  at/after the insertion point down by the new field's
//                  size, so the placement always lands free.
//   CreateField   — same form as InsertField, but the placement is exact:
//                  no sibling shifting, overlaps allowed (union-style
//                  annotation fields).
//   EditOffset   — retarget an existing field's offset. Same live conflict
//                  check; the move is committed as-is even when it overlaps
//                  (positioning is explicit — no draft, no push).
//   ShiftOffsets — move a selected block so its first field lands exactly at
//                  the given offset (relative gaps preserved). Conflicts
//                  disable the OK button entirely — the whole block is
//                  skipped.
//
// Live validation: the caller supplies a validate(offset, kind) callback
// returning an empty string when the placement is legal and a human-readable
// conflict description otherwise. It re-runs on every keystroke / type
// change so the user sees the problem before committing. The type combo is
// an editable dropdown: typing filters the list (case-insensitive contains)
// and the popup is height-bounded.
class FieldLayoutDialog : public ThemedDialog {
    Q_OBJECT
public:
    enum Mode { InsertField, CreateField, EditOffset, ShiftOffsets };

    using ValidateFn = std::function<QString(int offset, NodeKind kind)>;

    struct Result {
        bool     accepted = false;
        int      offset   = 0;
        NodeKind kind     = NodeKind::Hex64;
        QString  name;
    };

    FieldLayoutDialog(Mode mode, int defaultOffset, NodeKind defaultKind,
                      const QString& defaultName, ValidateFn validate,
                      const QString& title, QWidget* parent = nullptr);

    Result result() const { return m_result; }

protected:
    void applyTheme() override;

private slots:
    void onOkClicked();

private:
    void restyle();
    void revalidate();
    static int parseOffset(const QString& text, bool* ok);
    // Resolve the combo's text to a NodeKind: exact typeName, then
    // case-insensitive exact, then a unique case-insensitive substring
    // match. `ok` is false when the text names no single kind.
    static NodeKind kindFromComboText(const QString& text, bool* ok);

    Mode        m_mode;
    ValidateFn  m_validate;
    Result      m_result;

    QLineEdit*    m_offsetEdit = nullptr;
    QComboBox*    m_typeCombo  = nullptr;
    QLineEdit*    m_nameEdit   = nullptr;
    QLabel*       m_status     = nullptr;
    DialogButton* m_okBtn      = nullptr;
    QString       m_conflict;   // current validation error (empty = valid)
};

} // namespace rcx
