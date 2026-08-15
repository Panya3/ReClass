#include "fieldlayoutdialog.h"
#include "themes/thememanager.h"
#include <QSettings>
#include <QFont>
#include <QFontMetrics>
#include <QCompleter>
#include <QAbstractItemView>

namespace rcx {

FieldLayoutDialog::FieldLayoutDialog(Mode mode, int defaultOffset,
                                     NodeKind defaultKind,
                                     const QString& defaultName,
                                     ValidateFn validate,
                                     const QString& title, QWidget* parent)
    : ThemedDialog(parent), m_mode(mode), m_validate(std::move(validate)) {
    setWindowTitle(title);
    setModal(true);
    resize(440, (m_mode == InsertField || m_mode == CreateField) ? 250 : 170);

    QSettings s("REECLASS", "REECLASS");
    QFont font(s.value("font", "JetBrains Mono").toString(), 10);
    font.setFixedPitch(true);

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(12, 10, 12, 10);
    layout->setSpacing(8);

    auto* form = new QFormLayout;
    form->setSpacing(8);

    m_offsetEdit = new QLineEdit;
    m_offsetEdit->setFont(font);
    m_offsetEdit->setPlaceholderText(QStringLiteral("0x..."));
    m_offsetEdit->setText(QStringLiteral("0x%1").arg(defaultOffset, 0, 16));
    form->addRow(QStringLiteral("Offset:"), m_offsetEdit);

    if (m_mode == InsertField || m_mode == CreateField) {
        m_typeCombo = new QComboBox;
        m_typeCombo->setFont(font);
        // Editable dropdown: typing filters the kind list (case-insensitive
        // substring) instead of scrolling a long fixed list; the popup is
        // height-bounded so it never grows with the item count.
        m_typeCombo->setEditable(true);
        m_typeCombo->setInsertPolicy(QComboBox::NoInsert);
        m_typeCombo->setMaxVisibleItems(12);
        int defIdx = 0;
        for (int i = 0; i < (int)std::size(kKindMeta); ++i) {
            m_typeCombo->addItem(QString::fromLatin1(kKindMeta[i].typeName),
                                 QVariant::fromValue((int)kKindMeta[i].kind));
            if (kKindMeta[i].kind == defaultKind) defIdx = i;
        }
        m_typeCombo->setCurrentIndex(defIdx);
        auto* completer = new QCompleter(m_typeCombo->model(), m_typeCombo);
        completer->setCaseSensitivity(Qt::CaseInsensitive);
        completer->setFilterMode(Qt::MatchContains);
        m_typeCombo->setCompleter(completer);
        // The completer swaps in its own popup while typing; bound it to the
        // same ~12 rows as the plain dropdown.
        const int rowH = QFontMetrics(font).height() + 8;
        completer->popup()->setMaximumHeight(rowH * 12);
        form->addRow(QStringLiteral("Type:"), m_typeCombo);

        m_nameEdit = new QLineEdit;
        m_nameEdit->setFont(font);
        m_nameEdit->setText(defaultName);
        form->addRow(QStringLiteral("Name:"), m_nameEdit);
    }

    layout->addLayout(form);

    m_status = new QLabel(QStringLiteral(" "));
    m_status->setFont(font);
    m_status->setWordWrap(true);
    m_status->setMinimumHeight(QFontMetrics(font).height() * 2 + 4);
    layout->addWidget(m_status);

    QString okText = m_mode == InsertField ? QStringLiteral("Insert")
                   : m_mode == CreateField ? QStringLiteral("Create")
                   : m_mode == EditOffset  ? QStringLiteral("Save")
                   : QStringLiteral("Shift");
    m_okBtn = new DialogButton(okText, DialogButton::Primary);
    auto* cancel = new DialogButton(QStringLiteral("Cancel"), DialogButton::Secondary);
    connect(cancel, &QPushButton::clicked, this, &QDialog::reject);
    connect(m_okBtn, &QPushButton::clicked, this, &FieldLayoutDialog::onOkClicked);
    layout->addLayout(ThemedDialog::makeButtonRow({cancel, m_okBtn}));

    connect(m_offsetEdit, &QLineEdit::textChanged,
            this, [this](const QString&) { revalidate(); });
    if (m_typeCombo) {
        // Editing text (not picking an index) must re-validate live too.
        connect(m_typeCombo, &QComboBox::editTextChanged,
                this, [this](const QString&) { revalidate(); });
        connect(m_typeCombo, QOverload<int>::of(&QComboBox::currentIndexChanged),
                this, [this](int) { revalidate(); });
    }

    restyle();
    revalidate();
}

void FieldLayoutDialog::applyTheme() {
    ThemedDialog::applyTheme();
    restyle();
}

void FieldLayoutDialog::restyle() {
    const auto& t = ThemeManager::instance().current();
    const QString editStyle = QStringLiteral(
        "QLineEdit, QComboBox { background: %1; color: %2; border: 1px solid %3;"
        " padding: 5px 6px; }"
        "QLineEdit:focus, QComboBox:focus { border-color: %4; }")
        .arg(t.backgroundAlt.name(), t.text.name(), t.border.name(),
             t.borderFocused.name());
    if (m_offsetEdit) m_offsetEdit->setStyleSheet(editStyle);
    if (m_typeCombo)  m_typeCombo->setStyleSheet(editStyle);
    if (m_nameEdit)   m_nameEdit->setStyleSheet(editStyle);
}

int FieldLayoutDialog::parseOffset(const QString& text, bool* ok) {
    QString s = text.trimmed();
    if (s.isEmpty()) {
        if (ok) *ok = false;
        return 0;
    }
    bool neg = false;
    if (s.startsWith(QLatin1Char('-'))) { neg = true; s = s.mid(1); }
    if (s.startsWith(QStringLiteral("0x"), Qt::CaseInsensitive)) s = s.mid(2);
    bool h = false;
    qlonglong v = s.toLongLong(&h, 16);
    if (!h) {
        if (ok) *ok = false;
        return 0;
    }
    if (neg) v = -v;
    if (ok) *ok = true;
    return (int)v;
}

void FieldLayoutDialog::onOkClicked() {
    bool ok = false;
    int off = parseOffset(m_offsetEdit->text(), &ok);
    if (!ok) return;  // shouldn't happen — OK is only reachable via revalidate

    NodeKind kind = NodeKind::Hex64;
    if (m_typeCombo) {
        kind = kindFromComboText(m_typeCombo->currentText(), &ok);
        if (!ok) return;  // unknown type — OK is disabled by revalidate
    }

    m_result.accepted = true;
    m_result.offset   = off;
    m_result.kind     = kind;
    m_result.name     = m_nameEdit ? m_nameEdit->text().trimmed() : QString();
    accept();
}

NodeKind FieldLayoutDialog::kindFromComboText(const QString& text, bool* ok) {
    const QString t = text.trimmed();
    // Exact typeName, then case-insensitive exact, then a unique
    // case-insensitive substring match. Anything else is ambiguous.
    for (const auto& m : kKindMeta)
        if (t == QLatin1String(m.typeName)) {
            if (ok) *ok = true;
            return m.kind;
        }
    for (const auto& m : kKindMeta)
        if (t.compare(QLatin1String(m.typeName), Qt::CaseInsensitive) == 0) {
            if (ok) *ok = true;
            return m.kind;
        }
    NodeKind hit = NodeKind::Hex8;
    int count = 0;
    for (const auto& m : kKindMeta)
        if (QString::fromLatin1(m.typeName).contains(t, Qt::CaseInsensitive)) {
            hit = m.kind;
            count++;
        }
    if (count == 1) {
        if (ok) *ok = true;
        return hit;
    }
    if (ok) *ok = false;
    return NodeKind::Hex8;
}

void FieldLayoutDialog::revalidate() {
    const auto& t = ThemeManager::instance().current();

    bool offOk = false;
    int off = parseOffset(m_offsetEdit->text(), &offOk);

    bool typeOk = true;
    NodeKind kind = NodeKind::Hex64;
    if (m_typeCombo) kind = kindFromComboText(m_typeCombo->currentText(), &typeOk);

    if (!offOk) {
        m_conflict = QStringLiteral("Enter a valid hex offset (e.g. 0x10)");
    } else if (off < 0) {
        m_conflict = QStringLiteral("Offset must be >= 0");
    } else if (!typeOk) {
        m_conflict = QStringLiteral("Unknown type '%1'").arg(m_typeCombo->currentText().trimmed());
    } else if (m_validate) {
        m_conflict = m_validate(off, kind);
    } else {
        m_conflict.clear();
    }

    if (m_conflict.isEmpty()) {
        m_status->setText(QStringLiteral("Offset is free"));
        m_status->setStyleSheet(QStringLiteral("color: %1;").arg(t.indHintGreen.name()));
        m_okBtn->setEnabled(true);
        if (m_mode == InsertField)      m_okBtn->setText(QStringLiteral("Insert"));
        else if (m_mode == CreateField) m_okBtn->setText(QStringLiteral("Create"));
        else if (m_mode == EditOffset)  m_okBtn->setText(QStringLiteral("Save"));
        else                            m_okBtn->setText(QStringLiteral("Shift"));
    } else {
        QString hint;
        if (m_mode == InsertField) {
            // Containers (struct/array) have unknown span at insert time —
            // the controller places them as-is instead of pushing.
            hint = sizeForKind(kind) > 0
                ? QStringLiteral(" \u2014 existing fields will be pushed down")
                : QStringLiteral(" \u2014 container size unknown, placed as-is (no push)");
        } else if (m_mode == CreateField) {
            hint = QStringLiteral(" \u2014 placed as-is (may overlap)");
        } else if (m_mode == EditOffset) {
            hint = QStringLiteral(" \u2014 will overlap (no push)");
        }
        m_status->setText(QStringLiteral("\u26A0 ") + m_conflict + hint);
        m_status->setStyleSheet(QStringLiteral("color: %1;").arg(t.indRttiHint.name()));
        // A malformed offset / negative offset / unknown type has nothing
        // committable — disable OK. A block move that would collide is
        // refused entirely (skip the whole move). Single-field placements
        // (Insert / Create / Edit Offset) stay committable: the controller
        // resolves the overlap per-mode (push / allow / allow).
        const bool malformed = !offOk || off < 0 || !typeOk;
        m_okBtn->setEnabled(!malformed && m_mode != ShiftOffsets);
        if (m_okBtn->isEnabled()) {
            if (m_mode == InsertField)      m_okBtn->setText(QStringLiteral("Insert"));
            else if (m_mode == CreateField) m_okBtn->setText(QStringLiteral("Create"));
            else if (m_mode == EditOffset)  m_okBtn->setText(QStringLiteral("Save"));
        }
    }
}

} // namespace rcx
