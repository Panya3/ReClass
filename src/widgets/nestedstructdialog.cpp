#include "nestedstructdialog.h"
#include "widgets/dialog_button.h"
#include "themes/thememanager.h"
#include <QComboBox>
#include <QCompleter>
#include <QAbstractItemView>
#include <QMouseEvent>
#include <QStyle>
#include <QStyledItemDelegate>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QVBoxLayout>
#include <QSettings>
#include <QFont>
#include <QFontMetrics>
#include <QPainter>
#include <QApplication>

namespace rcx {

namespace {
// Tree where a single left click on the Offset (0) / Name (2) / Type name
// (3) columns drops straight into edit mode; the Type column keeps normal
// behavior (it hosts the combo). Offset edits are honored as manual
// overrides — see onOffsetEdited.
class ClickToEditTree : public QTreeWidget {
public:
    using QTreeWidget::QTreeWidget;

protected:
    void mousePressEvent(QMouseEvent* e) override {
        QTreeWidget::mousePressEvent(e);
        const QModelIndex idx = indexAt(e->pos());
        if (!idx.isValid() || e->button() != Qt::LeftButton) return;
        if (idx.column() != 0 && idx.column() != 2 && idx.column() != 3) return;
        // Clicks on the expand/collapse arrow (the branch strip QTreeView
        // reserves at the left of column 0) must toggle the node, not drop
        // into offset editing — the base class already handled the toggle.
        if (idx.column() == 0 && e->pos().x() < visualRect(idx).left())
            return;
        edit(idx);
    }
};

// Offset-cell editor: force a left-aligned QLineEdit that fills the whole
// cell. Without this the editor can inherit the item's alignment or shrink
// to its sizeHint, so the input neither reads left nor spans the column.
class OffsetEditorDelegate : public QStyledItemDelegate {
public:
    using QStyledItemDelegate::QStyledItemDelegate;

    QWidget* createEditor(QWidget* parent, const QStyleOptionViewItem& opt,
                          const QModelIndex& index) const override {
        auto* le = new QLineEdit(parent);
        le->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
        // Same font as the tree so the edit looks identical to the display.
        if (auto* tv = qobject_cast<const QTreeWidget*>(parent->parentWidget()))
            le->setFont(tv->font());
        return le;
    }

    void setEditorData(QWidget* editor, const QModelIndex& index) const override {
        auto* le = qobject_cast<QLineEdit*>(editor);
        if (le) le->setText(index.data(Qt::DisplayRole).toString());
    }

    void setModelData(QWidget* editor, QAbstractItemModel* model,
                      const QModelIndex& index) const override {
        auto* le = qobject_cast<QLineEdit*>(editor);
        if (le) model->setData(index, le->text(), Qt::EditRole);
    }

    void paint(QPainter* painter, const QStyleOptionViewItem& opt,
               const QModelIndex& index) const override {
        // Draw the normal cell background/selection within the item's own
        // rect (the branch strip and its arrow stay untouched), then center
        // the number within the FULL column. QTreeView reserves a 20px
        // strip at the left of column 0, so centering inside opt.rect alone
        // would sit the text half a strip (10px) right of the column's true
        // center — which reads as "shifted right".
        QStyleOptionViewItem bg = opt;
        initStyleOption(&bg, index);
        bg.text.clear();
        bg.features &= ~QStyleOptionViewItem::HasDisplay;
        QStyle* st = opt.widget ? opt.widget->style() : QApplication::style();
        st->drawControl(QStyle::CE_ItemViewItem, &bg, painter, opt.widget);

        QRect r = opt.rect;
        if (auto* tv = qobject_cast<const QTreeWidget*>(opt.widget)) {
            QHeaderView* h = tv->header();
            r.setLeft(h->sectionViewportPosition(0));
            r.setWidth(h->sectionSize(0));
        }
        const QPalette::ColorGroup cg =
            (opt.state & QStyle::State_Enabled)
                ? (opt.state & QStyle::State_Selected ? QPalette::Active
                                                      : QPalette::Normal)
                : QPalette::Disabled;
        const QPalette::ColorRole cr =
            (opt.state & QStyle::State_Selected) ? QPalette::HighlightedText
                                                 : QPalette::Text;
        painter->save();
        painter->setFont(opt.font);
        painter->setPen(QPen(opt.palette.color(cg, cr)));
        painter->drawText(r, Qt::AlignHCenter | Qt::AlignVCenter,
                          index.data(Qt::DisplayRole).toString());
        painter->restore();
    }

    void updateEditorGeometry(QWidget* editor, const QStyleOptionViewItem& opt,
                              const QModelIndex&) const override {
        // Fill the entire cell — never shrink to the content width.
        QRect r = opt.rect;
        // QTreeView reserves a 20px branch/indentation strip at the left of
        // column 0 (visualRect starts after it, even for leaf rows), so
        // opt.rect alone leaves the input indented from the column's true
        // start. Stretch the editor out to the real column edges so the
        // text sits flush at the column start X and spans the full width.
        if (auto* tv = qobject_cast<const QTreeWidget*>(editor->parentWidget()->parentWidget())) {
            QHeaderView* h = tv->header();
            r.setLeft(h->sectionViewportPosition(0));
            r.setWidth(h->sectionSize(0));
        }
        editor->setGeometry(r);
    }
};

// Fill a row's type combo with primitive kinds plus the three container
// keywords. Item data: "k<int>" for primitives, "c<keyword>" for containers.
void populateTypeCombo(QComboBox* combo) {
    for (const auto& m : kKindMeta) {
        if (m.kind == NodeKind::Struct || m.kind == NodeKind::Array) continue;
        combo->addItem(QString::fromLatin1(m.typeName),
                       QStringLiteral("k%1").arg(int(m.kind)));
    }
    combo->addItem(QStringLiteral("struct"), QStringLiteral("cstruct"));
    combo->addItem(QStringLiteral("union"),  QStringLiteral("cunion"));
    combo->addItem(QStringLiteral("class"),  QStringLiteral("cclass"));
}

QString encodeData(const NestedStructDialog::RowData& d) {
    return d.kind == NodeKind::Struct
        ? QStringLiteral("c") + d.keyword
        : QStringLiteral("k%1").arg(int(d.kind));
}
} // namespace

NestedStructDialog::NestedStructDialog(int defaultOffset, QWidget* parent)
    : ThemedDialog(parent) {
    setWindowTitle(QStringLiteral("Insert Nested Struct"));
    setModal(true);
    resize(640, 460);

    QSettings s("REECLASS", "REECLASS");
    QFont font(s.value("font", "JetBrains Mono").toString(), 10);
    font.setFixedPitch(true);

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(12, 10, 12, 10);
    layout->setSpacing(8);

    // ── Member declaration ──
    auto* form = new QFormLayout;
    form->setSpacing(8);
    // Fields grow to fill the row — the Offset input spans the full width.
    form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);

    m_typeNameEdit = new QLineEdit;
    m_typeNameEdit->setFont(font);
    m_typeNameEdit->setPlaceholderText(QStringLiteral("(empty = anonymous)"));
    form->addRow(QStringLiteral("Type name:"), m_typeNameEdit);

    m_keywordCombo = new QComboBox;
    m_keywordCombo->setFont(font);
    m_keywordCombo->addItem(QStringLiteral("struct"), QStringLiteral("struct"));
    m_keywordCombo->addItem(QStringLiteral("union"),  QStringLiteral("union"));
    m_keywordCombo->addItem(QStringLiteral("class"),  QStringLiteral("class"));
    form->addRow(QStringLiteral("Keyword:"), m_keywordCombo);

    m_nameEdit = new QLineEdit;
    m_nameEdit->setFont(font);
    m_nameEdit->setText(QStringLiteral("field"));
    form->addRow(QStringLiteral("Member name:"), m_nameEdit);

    m_offsetEdit = new QLineEdit;
    m_offsetEdit->setFont(font);
    // Plain hex (no 0x prefix), left-aligned. parseOffset still accepts
    // "0x"-prefixed input.
    m_offsetEdit->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    m_offsetEdit->setPlaceholderText(QStringLiteral("0"));
    m_offsetEdit->setText(QString::number(defaultOffset, 16));
    // Full width: with AllNonFixedFieldsGrow the field fills the row; an
    // explicit Expanding policy guarantees it even if the style default
    // disagrees.
    m_offsetEdit->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    form->addRow(QStringLiteral("Offset:"), m_offsetEdit);

    layout->addLayout(form);

    // ── Children tree ──
    m_tree = new ClickToEditTree;
    m_tree->setFont(font);
    m_tree->setColumnCount(4);
    m_tree->setHeaderLabels({QStringLiteral("Offset"), QStringLiteral("Type"),
                             QStringLiteral("Name"), QStringLiteral("Type name")});
    m_tree->setRootIsDecorated(true);
    m_tree->setUniformRowHeights(true);
    m_tree->header()->setStretchLastSection(false);
    // Column order: Offset | Type | Name | Type name.
    // 0 (Offset) fixed; 1 (Type) fixed 25% wider than Offset (the combo
    // fills its cell as an item widget, so a fixed width works); 2 (Name)
    // and 3 (Type name) both Stretch, splitting the remaining space equally.
    m_tree->header()->setSectionResizeMode(0, QHeaderView::Interactive);
    m_tree->setColumnWidth(0, 72);              // Offset
    // Left-aligned, full-cell editor for the Offset column (see
    // OffsetEditorDelegate).
    m_tree->setItemDelegateForColumn(0, new OffsetEditorDelegate(m_tree));
    m_tree->header()->setSectionResizeMode(1, QHeaderView::Interactive);
    m_tree->setColumnWidth(1, 90);              // Type — Offset * 1.25 exactly
    m_tree->header()->setSectionResizeMode(2, QHeaderView::Stretch);   // Name
    m_tree->header()->setSectionResizeMode(3, QHeaderView::Stretch);   // Type name
    m_tree->setMinimumHeight(220);
    layout->addWidget(m_tree, /*stretch=*/1);

    // Row buttons
    auto* rowBtns = new QHBoxLayout;
    auto* addField = new DialogButton(QStringLiteral("Add Field"), DialogButton::Secondary);
    auto* addContainer = new DialogButton(QStringLiteral("Add Nested Struct"), DialogButton::Secondary);
    auto* removeRow = new DialogButton(QStringLiteral("Remove Row"), DialogButton::Secondary);
    connect(addField, &QPushButton::clicked, this, &NestedStructDialog::onAddField);
    connect(addContainer, &QPushButton::clicked, this, &NestedStructDialog::onAddContainer);
    connect(removeRow, &QPushButton::clicked, this, &NestedStructDialog::onRemoveRow);
    rowBtns->addWidget(addField);
    rowBtns->addWidget(addContainer);
    rowBtns->addWidget(removeRow);
    rowBtns->addStretch();
    layout->addLayout(rowBtns);

    // Live offset preview on member keyword change
    connect(m_keywordCombo, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, &NestedStructDialog::onMemberChanged);

    m_status = new QLabel(QStringLiteral(" "));
    m_status->setFont(font);
    m_status->setWordWrap(true);
    m_status->setMinimumHeight(QFontMetrics(font).height() * 2 + 4);
    layout->addWidget(m_status);

    auto* ok = new DialogButton(QStringLiteral("Insert"), DialogButton::Primary);
    auto* cancel = new DialogButton(QStringLiteral("Cancel"), DialogButton::Secondary);
    connect(ok, &QPushButton::clicked, this, &NestedStructDialog::onOkClicked);
    connect(cancel, &QPushButton::clicked, this, &QDialog::reject);
    layout->addLayout(ThemedDialog::makeButtonRow({cancel, ok}));

    // Seed with one empty field so the shape is obvious. The type combo
    // installs only after the item is in the tree (see installRowWidget).
    const RowData seedData{NodeKind::Hex64, QStringLiteral("struct"), QString()};
    QTreeWidgetItem* seed = makeRow(seedData, QStringLiteral("field"));
    m_tree->addTopLevelItem(seed);
    installRowWidget(seed, seedData);

    // Offset column edits: the tree is single-click editable there; a text
    // change made by the user (not by packChildren) marks the row manual so
    // its value survives re-packing and is honored at insert time.
    connect(m_tree, &QTreeWidget::itemChanged, this,
            [this](QTreeWidgetItem* item, int column) {
        if (!item || column != 0 || m_packingDepth > 0) return;
        item->setData(0, Qt::UserRole, true);   // manual override
        packChildren(m_tree, nullptr,
                     m_keywordCombo->currentData().toString() == QStringLiteral("union"));
    });

    restyle();
    packChildren(m_tree, nullptr, false);
}

void NestedStructDialog::applyTheme() {
    ThemedDialog::applyTheme();
    restyle();
}

void NestedStructDialog::restyle() {
    const auto& t = ThemeManager::instance().current();
    const QString editStyle = QStringLiteral(
        "QLineEdit, QComboBox, QTreeWidget { background: %1; color: %2;"
        " border: 1px solid %3; padding: 2px 4px; }"
        "QLineEdit:focus, QComboBox:focus { border-color: %4; }")
        .arg(t.backgroundAlt.name(), t.text.name(), t.border.name(),
             t.borderFocused.name());
    if (m_typeNameEdit) m_typeNameEdit->setStyleSheet(editStyle);
    if (m_keywordCombo) m_keywordCombo->setStyleSheet(editStyle);
    if (m_nameEdit)     m_nameEdit->setStyleSheet(editStyle);
    if (m_offsetEdit)   m_offsetEdit->setStyleSheet(editStyle);
    if (m_tree)         m_tree->setStyleSheet(editStyle);
}

// ── Row helpers ──

bool NestedStructDialog::resolveComboText(const QComboBox* combo,
                                          const QString& text, RowData& out) {
    if (!combo) return false;
    const QString t = text.trimmed();
    if (t.isEmpty()) return false;
    auto decode = [](const QString& data) {
        RowData d;
        if (data.startsWith(QLatin1Char('k'))) {
            d.kind = static_cast<NodeKind>(data.mid(1).toInt());
        } else if (data.startsWith(QLatin1Char('c'))) {
            d.kind = NodeKind::Struct;
            d.keyword = data.mid(1);
        }
        return d;
    };
    // Exact text, then case-insensitive exact.
    for (int i = 0; i < combo->count(); i++) {
        if (combo->itemText(i) == t) {
            out = decode(combo->itemData(i).toString());
            return true;
        }
    }
    for (int i = 0; i < combo->count(); i++) {
        if (combo->itemText(i).compare(t, Qt::CaseInsensitive) == 0) {
            out = decode(combo->itemData(i).toString());
            return true;
        }
    }
    // Unique case-insensitive substring.
    RowData hit;
    int hits = 0;
    for (int i = 0; i < combo->count(); i++) {
        if (combo->itemText(i).contains(t, Qt::CaseInsensitive)) {
            hit = decode(combo->itemData(i).toString());
            hits++;
        }
    }
    if (hits == 1) {
        out = hit;
        return true;
    }
    return false;
}

NestedStructDialog::RowData NestedStructDialog::rowData(const QTreeWidgetItem* item) {
    RowData d;
    if (!item) return d;
    auto* combo = qobject_cast<QComboBox*>(
        item->treeWidget()->itemWidget(const_cast<QTreeWidgetItem*>(item), 1));
    if (!combo) return d;
    // Resolve from the displayed text so a typed-but-uncommitted value
    // (editable combo) is honored; unknown/ambiguous text keeps the default.
    resolveComboText(combo, combo->currentText(), d);
    return d;
}

void NestedStructDialog::setRowData(QTreeWidgetItem* item, const RowData& data) {
    auto* combo = qobject_cast<QComboBox*>(
        m_tree->itemWidget(item, 1));
    if (!combo) return;
    const QString want = encodeData(data);
    for (int i = 0; i < combo->count(); i++) {
        if (combo->itemData(i).toString() == want) {
            combo->setCurrentIndex(i);
            break;
        }
    }
}

QTreeWidgetItem* NestedStructDialog::makeRow(const RowData& data, const QString& name) {
    auto* item = new QTreeWidgetItem();
    item->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable | Qt::ItemIsEditable);
    item->setText(2, name);          // Name column (index 2)
    item->setText(3, data.kind == NodeKind::Struct ? data.typeName : QString());  // Type name (3)
    return item;
}

// Attach the row's type combo. Must run AFTER the item is added to the
// tree: Qt's setItemWidget silently does nothing for items not yet
// associated with a QTreeWidget, so attaching first would leave the Type
// column empty.
void NestedStructDialog::installRowWidget(QTreeWidgetItem* item, const RowData& data) {
    auto* combo = new QComboBox(m_tree);
    combo->setFont(m_tree->font());
    // Editable + filterable + height-bounded — the same treatment as the
    // Insert Field dialog's type combo: typing filters the list
    // (case-insensitive substring) and the popup never grows with the item
    // count.
    combo->setEditable(true);
    combo->setInsertPolicy(QComboBox::NoInsert);
    combo->setMaxVisibleItems(12);
    auto* completer = new QCompleter(combo->model(), combo);
    completer->setCaseSensitivity(Qt::CaseInsensitive);
    completer->setFilterMode(Qt::MatchContains);
    combo->setCompleter(completer);
    // The completer swaps in its own popup while typing; bound it to the
    // same ~12 rows as the plain dropdown.
    const int rowH = QFontMetrics(combo->font()).height() + 8;
    completer->popup()->setMaximumHeight(rowH * 12);
    m_tree->setItemWidget(item, 1, combo);   // Type column (index 1)
    populateTypeCombo(combo);
    setRowData(item, data);
    // Size the Type column to this combo: ResizeToContents only reads the
    // item's SizeHintRole, so mirror the combo's hint onto the item. Fit
    // the combo's actual text (current entry + arrow + frame) rather than
    // the widest popup item (uint128_t / mat4x4 would bloat the column).
    QFontMetrics fm(combo->font());
    const int textW = fm.horizontalAdvance(combo->currentText());
    const int arrow = combo->style()->pixelMetric(QStyle::PM_ScrollBarExtent,
                                                  nullptr, combo);
    const int frame = combo->style()->pixelMetric(QStyle::PM_ComboBoxFrameWidth,
                                                  nullptr, combo);
    item->setSizeHint(1, QSize(textW + arrow + frame * 2 + 8,
                               combo->sizeHint().height()));
    // Connect after the programmatic selection above so construction-time
    // index changes don't fire the re-pack slot.
    connect(combo, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, &NestedStructDialog::onComboChanged);
}

void NestedStructDialog::collectChildren(const QTreeWidgetItem* parent,
                                         QVector<NestedStructSpec>& out) const {
    const int count = parent ? parent->childCount() : m_tree->topLevelItemCount();
    for (int i = 0; i < count; i++) {
        const QTreeWidgetItem* item = parent ? parent->child(i) : m_tree->topLevelItem(i);
        RowData d = rowData(item);
        NestedStructSpec spec;
        spec.kind = d.kind;
        spec.name = item->text(2);        // Name column
        // User-typed offset (Qt::UserRole flag set by itemChanged): honor it
        // verbatim instead of letting the controller's layout pass decide.
        if (item->data(0, Qt::UserRole).toBool()) {
            bool ok = false;
            spec.offset       = parseOffset(item->text(0), &ok);
            spec.offsetManual = ok;
        }
        if (d.kind == NodeKind::Struct) {
            spec.keyword  = d.keyword;
            spec.typeName = item->text(3);  // Type name column
            collectChildren(item, spec.children);
        }
        out.append(spec);
    }
}

QTreeWidgetItem* NestedStructDialog::selectedContainer() const {
    QTreeWidgetItem* item = m_tree->currentItem();
    if (!item) return nullptr;
    return rowData(item).kind == NodeKind::Struct ? item : nullptr;
}

QTreeWidgetItem* NestedStructDialog::rowForCombo(const QComboBox* combo) const {
    if (!combo) return nullptr;
    std::function<QTreeWidgetItem*(const QTreeWidgetItem*)> scan =
        [&](const QTreeWidgetItem* parent) -> QTreeWidgetItem* {
        const int count = parent ? parent->childCount() : m_tree->topLevelItemCount();
        for (int i = 0; i < count; i++) {
            const QTreeWidgetItem* it = parent ? parent->child(i) : m_tree->topLevelItem(i);
            if (m_tree->itemWidget(const_cast<QTreeWidgetItem*>(it), 1) == combo)
                return const_cast<QTreeWidgetItem*>(it);
            if (it->childCount() > 0) {
                if (QTreeWidgetItem* found = scan(it)) return found;
            }
        }
        return nullptr;
    };
    return scan(nullptr);
}

// ── Slots ──

void NestedStructDialog::onAddField() {
    QTreeWidgetItem* parent = selectedContainer();
    RowData d{NodeKind::Hex64, QStringLiteral("struct"), QString()};
    QTreeWidgetItem* row = makeRow(d, QStringLiteral("field"));
    if (parent) parent->addChild(row);
    else        m_tree->addTopLevelItem(row);
    installRowWidget(row, d);
    m_tree->setCurrentItem(row);
    m_tree->editItem(row, 2);   // edit the Name column
    packChildren(m_tree, nullptr,
                 m_keywordCombo->currentData().toString() == QStringLiteral("union"));
}

void NestedStructDialog::onAddContainer() {
    QTreeWidgetItem* parent = selectedContainer();
    RowData d{NodeKind::Struct, QStringLiteral("struct"), QString()};
    QTreeWidgetItem* row = makeRow(d, QStringLiteral("inner"));
    if (parent) parent->addChild(row);
    else        m_tree->addTopLevelItem(row);
    installRowWidget(row, d);
    row->setExpanded(true);
    m_tree->setCurrentItem(row);
    m_tree->editItem(row, 2);   // edit the Name column
    packChildren(m_tree, nullptr,
                 m_keywordCombo->currentData().toString() == QStringLiteral("union"));
}

void NestedStructDialog::onRemoveRow() {
    QTreeWidgetItem* item = m_tree->currentItem();
    if (!item) return;
    if (item->parent()) item->parent()->removeChild(item);
    else                m_tree->takeTopLevelItem(m_tree->indexOfTopLevelItem(item));
    delete item;
    packChildren(m_tree, nullptr,
                 m_keywordCombo->currentData().toString() == QStringLiteral("union"));
}

void NestedStructDialog::onMemberChanged() {
    packChildren(m_tree, nullptr,
                 m_keywordCombo->currentData().toString() == QStringLiteral("union"));
}

void NestedStructDialog::onComboChanged(int index) {
    // Free-text editing drives the index to -1 (no item matches yet); never
    // touch the row on those — pruning would destroy children while the
    // user is still typing. Only a committed selection (a real item) acts.
    if (index < 0) return;
    // Resolve the owning row from the sender — currentItem() follows
    // keyboard/click selection, not the combo that actually changed, so a
    // combo edited in a non-current row would prune the wrong row's
    // children. A scan of the itemWidget map beats a geometry hit-test:
    // it stays correct even before the dialog is laid out.
    auto* combo = qobject_cast<QComboBox*>(sender());
    QTreeWidgetItem* item = combo ? rowForCombo(combo) : nullptr;
    if (!item) return;
    RowData d = rowData(item);
    if (d.kind != NodeKind::Struct) {
        // Switched to a primitive: child rows make no sense — prune them,
        // and the inline type name column is container-only.
        while (item->childCount() > 0)
            delete item->takeChild(0);
        item->setText(3, QString());
    }
    // Container rows keep their inline type name — never wipe column 3
    // when switching struct/union/class (the user typed it).
    packChildren(m_tree, nullptr,
                 m_keywordCombo->currentData().toString() == QStringLiteral("union"));
}

void NestedStructDialog::onOkClicked() {
    bool ok = false;
    parseOffset(m_offsetEdit->text(), &ok);
    if (!ok) {
        m_status->setText(QStringLiteral(
            "Offset must be a number (decimal or 0x-hex)."));
        return;
    }
    const QString badRow = firstUnresolvedTypeRow(nullptr);
    if (!badRow.isEmpty()) {
        m_status->setText(QStringLiteral(
            "Type of row '%1' doesn't resolve to a single known type \u2014 "
            "pick one from the list or clear the text.").arg(badRow));
        return;
    }
    pruneInconsistentRows();
    accept();
}

void NestedStructDialog::pruneInconsistentRows() {
    std::function<void(QTreeWidgetItem*)> walk = [&](QTreeWidgetItem* item) {
        const RowData d = rowData(item);
        if (d.kind != NodeKind::Struct && item->childCount() > 0) {
            // Resolved to a primitive: child rows are meaningless and would
            // be silently omitted from the inserted tree.
            while (item->childCount() > 0)
                delete item->takeChild(0);
            item->setText(3, QString());
        }
        for (int i = 0; i < item->childCount(); i++)
            walk(item->child(i));
    };
    for (int i = 0; i < m_tree->topLevelItemCount(); i++)
        walk(m_tree->topLevelItem(i));
}

QString NestedStructDialog::firstUnresolvedTypeRow(const QTreeWidgetItem* parent) const {
    const int count = parent ? parent->childCount() : m_tree->topLevelItemCount();
    for (int i = 0; i < count; i++) {
        const QTreeWidgetItem* item = parent ? parent->child(i) : m_tree->topLevelItem(i);
        auto* combo = qobject_cast<QComboBox*>(
            m_tree->itemWidget(const_cast<QTreeWidgetItem*>(item), 1));  // Type column (index 1)
        if (combo) {
            RowData d;
            if (!resolveComboText(combo, combo->currentText(), d)) {
                QString name = item->text(2).trimmed();
                return name.isEmpty() ? QStringLiteral("(unnamed)") : name;
            }
        }
        if (item->childCount() > 0) {
            const QString sub = firstUnresolvedTypeRow(item);
            if (!sub.isEmpty()) return sub;
        }
    }
    return QString();
}

// ── Offset preview ──

QPair<int, int> NestedStructDialog::packChildren(QTreeWidget* tree,
                                                 QTreeWidgetItem* container,
                                                 bool isUnion) {
    ++m_packingDepth;
    int align = 1, cursor = 0, maxSize = 0;
    const int count = container ? container->childCount() : tree->topLevelItemCount();
    auto itemAt = [&](int i) {
        return container ? container->child(i) : tree->topLevelItem(i);
    };
    for (int i = 0; i < count; i++) {
        QTreeWidgetItem* item = itemAt(i);
        RowData d = rowData(item);
        int cAlign = 1, cSize = 0;
        if (d.kind == NodeKind::Struct) {
            auto l = packChildren(tree, item, d.keyword == QStringLiteral("union"));
            cAlign = l.second;
            cSize  = l.first;
        } else {
            cAlign = alignmentFor(d.kind);
            cSize  = sizeForKind(d.kind);
        }
        align = qMax(align, cAlign);
        // Plain hex, no 0x prefix. Display is centered in the full column;
        // the edit editor is left-aligned (OffsetEditorDelegate) — that
        // contrast is intentional.
        const auto setOffset = [&](int off) {
            item->setText(0, QString::number(off, 16).toUpper());
            item->setTextAlignment(0, Qt::AlignHCenter | Qt::AlignVCenter);
        };
        // A row the user typed an offset for keeps its value verbatim (the
        // itemChanged handler set Qt::UserRole on column 0); subsequent
        // rows pack after it so the preview matches the controller. Text
        // that doesn't parse is NOT manual — collectChildren only sets
        // offsetManual when the text parses, so fall through to the auto
        // path here or the preview and the inserted layout would diverge.
        const bool manual = item->data(0, Qt::UserRole).toBool();
        if (manual) {
            bool ok = false;
            const int off = parseOffset(item->text(0), &ok);
            if (ok) {
                if (isUnion) {
                    maxSize = qMax(maxSize, (cSize + cAlign - 1) / cAlign * cAlign);
                } else {
                    cursor = qMax(cursor, off + cSize);
                }
                continue;
            }
            item->setData(0, Qt::UserRole, false);   // invalid text → drop manual status
        }
        if (isUnion) {
            setOffset(0);
            maxSize = qMax(maxSize, (cSize + cAlign - 1) / cAlign * cAlign);
        } else {
            int off = cursor + (cAlign - (cursor % cAlign)) % cAlign;
            setOffset(off);
            cursor = off + cSize;
        }
    }
    int size = isUnion ? maxSize : cursor;
    if (size > 0)
        size = (size + align - 1) / align * align;
    --m_packingDepth;
    return {size, align};
}

int NestedStructDialog::parseOffset(const QString& text, bool* ok) {
    QString s = text.trimmed();
    if (s.isEmpty()) { if (ok) *ok = false; return 0; }
    bool neg = false;
    if (s.startsWith(QLatin1Char('-'))) { neg = true; s = s.mid(1); }
    if (s.startsWith(QStringLiteral("0x"), Qt::CaseInsensitive)) s = s.mid(2);
    bool h = false;
    qlonglong v = s.toLongLong(&h, 16);
    if (!h) { if (ok) *ok = false; return 0; }
    if (neg) v = -v;
    if (ok) *ok = true;
    return (int)v;
}

void NestedStructDialog::collectResult(QString& name, QString& typeName,
                                       QString& keyword, int& offset,
                                       QVector<NestedStructSpec>& children) const {
    name = m_nameEdit->text().trimmed();
    if (name.isEmpty()) name = QStringLiteral("field");
    typeName = m_typeNameEdit->text().trimmed();
    keyword = m_keywordCombo->currentData().toString();
    bool ok = false;
    offset = parseOffset(m_offsetEdit->text(), &ok);
    if (!ok) offset = 0;
    collectChildren(nullptr, children);
}

} // namespace rcx
