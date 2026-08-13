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

// Modal dialog for the "new variable declaration" flow. Three modes:
//
//   InsertField  — declare a brand-new field: offset (exact placement, no
//                  sibling shifting), type (full kind list), name (optional,
//                  auto "field_XX" when empty). If the offset collides with
//                  an existing sibling (duplicate offset, or the field's size
//                  eating into another field), the OK button becomes
//                  "Insert as Draft" — committing keeps the field as a
//                  non-counted placeholder until the offset is fixed.
//   EditOffset   — retarget an existing field's offset. Same live conflict
//                  check; on conflict OK becomes "Save as Draft".
//   ShiftOffsets — move a selected block so its first field lands exactly at
//                  the given offset (relative gaps preserved). Conflicts
//                  disable the OK button entirely — the whole block is
//                  skipped rather than committed as a draft.
//
// Live validation: the caller supplies a validate(offset, kind) callback
// returning an empty string when the placement is legal and a human-readable
// conflict description otherwise. It re-runs on every keystroke / type
// change so the user sees the problem before committing.
class FieldLayoutDialog : public ThemedDialog {
    Q_OBJECT
public:
    enum Mode { InsertField, EditOffset, ShiftOffsets };

    using ValidateFn = std::function<QString(int offset, NodeKind kind)>;

    struct Result {
        bool     accepted = false;
        int      offset   = 0;
        NodeKind kind     = NodeKind::Hex64;
        QString  name;
        bool     asDraft  = false;  // true when committed despite a conflict
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
