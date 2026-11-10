#include "multi_combo.h"

#include <QAbstractItemView>
#include <QKeyEvent>
#include <QLineEdit>
#include <QMouseEvent>
#include <QSignalBlocker>
#include <QStandardItemModel>

namespace xrkit::shell {

MultiSelectComboBox::MultiSelectComboBox(QWidget* parent) : QComboBox(parent) {
    model_ = new QStandardItemModel(this);
    setModel(model_);
    setEditable(true);
    if (lineEdit() != nullptr) {
        lineEdit()->setReadOnly(true);
        lineEdit()->setFrame(false);
        lineEdit()->setFocusPolicy(Qt::NoFocus);
        lineEdit()->installEventFilter(this);
    }
    view()->setSelectionMode(QAbstractItemView::NoSelection);
    view()->viewport()->installEventFilter(this);
}

void MultiSelectComboBox::setMode(Mode mode) {
    mode_ = mode;
    if (mode_ == Mode::Multi) {
        view()->setSelectionMode(QAbstractItemView::NoSelection);
        view()->viewport()->installEventFilter(this);
    }
    refreshDisplay();
}

void MultiSelectComboBox::addChoice(const QString& text, const QVariant& data, bool checked,
                                    bool enabled) {
    auto* item = new QStandardItem(text);
    item->setData(data, Qt::UserRole);
    item->setEnabled(enabled);
    if (mode_ == Mode::Multi) {
        item->setCheckable(true);
        item->setCheckState(checked ? Qt::Checked : Qt::Unchecked);
        item->setFlags((enabled ? Qt::ItemIsEnabled : Qt::NoItemFlags) |
                       Qt::ItemIsUserCheckable);
        item->setSelectable(false);
    } else {
        item->setFlags(enabled ? (Qt::ItemIsEnabled | Qt::ItemIsSelectable) : Qt::NoItemFlags);
    }
    model_->appendRow(item);
    if (mode_ == Mode::Single && currentIndex() < 0) setCurrentIndex(0);
    refreshDisplay();
}

void MultiSelectComboBox::clearChoices() {
    const QSignalBlocker block(this);
    model_->clear();
    if (lineEdit() != nullptr) lineEdit()->clear();
}

QVariantList MultiSelectComboBox::checkedData() const {
    QVariantList out;
    for (int row = 0; row < model_->rowCount(); ++row) {
        const QStandardItem* item = model_->item(row);
        if (item != nullptr && item->checkState() == Qt::Checked) {
            out.push_back(item->data(Qt::UserRole));
        }
    }
    return out;
}

void MultiSelectComboBox::setCheckedData(const QVariantList& data) {
    const QSignalBlocker block(this);
    updating_ = true;
    for (int row = 0; row < model_->rowCount(); ++row) {
        QStandardItem* item = model_->item(row);
        if (item == nullptr) continue;
        item->setCheckState(data.contains(item->data(Qt::UserRole)) ? Qt::Checked
                                                                    : Qt::Unchecked);
    }
    updating_ = false;
    refreshDisplay();
}

void MultiSelectComboBox::setChoiceChecked(const QVariant& data, bool checked) {
    for (int row = 0; row < model_->rowCount(); ++row) {
        QStandardItem* item = model_->item(row);
        if (item != nullptr && item->data(Qt::UserRole) == data) {
            item->setCheckState(checked ? Qt::Checked : Qt::Unchecked);
            refreshDisplay();
            emit checkedChanged();
            return;
        }
    }
}

bool MultiSelectComboBox::isChoiceChecked(const QVariant& data) const {
    for (int row = 0; row < model_->rowCount(); ++row) {
        const QStandardItem* item = model_->item(row);
        if (item != nullptr && item->data(Qt::UserRole) == data) {
            return item->checkState() == Qt::Checked;
        }
    }
    return false;
}

QString MultiSelectComboBox::summary() const {
    if (mode_ == Mode::Single) {
        return currentText();
    }
    QStringList parts;
    for (int row = 0; row < model_->rowCount(); ++row) {
        const QStandardItem* item = model_->item(row);
        if (item != nullptr && item->checkState() == Qt::Checked) {
            QString label = item->text();
            // 显示用短名：完整项名留给弹出列表，避免把控件撑宽/截断。
            if (label.startsWith(QStringLiteral("标准"))) label = QStringLiteral("标准");
            parts << label;
        }
    }
    if (parts.isEmpty()) return QStringLiteral("未选择");
    return parts.join(QStringLiteral(" · "));
}

void MultiSelectComboBox::refreshDisplay() {
    if (mode_ == Mode::Single) return;
    const QSignalBlocker block(this);
    if (lineEdit() != nullptr) lineEdit()->setText(summary());
}

void MultiSelectComboBox::toggleAt(const QModelIndex& index) {
    if (!index.isValid()) return;
    QStandardItem* item = model_->itemFromIndex(index);
    if (item == nullptr || !item->isEnabled() || !item->isCheckable()) return;
    item->setCheckState(item->checkState() == Qt::Checked ? Qt::Unchecked : Qt::Checked);
    refreshDisplay();
    emit checkedChanged();
}

bool MultiSelectComboBox::eventFilter(QObject* watched, QEvent* event) {
    // editable QComboBox 的文字区是 QLineEdit：点文字只会聚焦，不会弹下拉。
    // 多选模式点文字/空白也应展开，只有箭头走 Qt 默认路径。
    if (mode_ == Mode::Multi && watched == lineEdit() &&
        (event->type() == QEvent::MouseButtonPress ||
         event->type() == QEvent::MouseButtonRelease)) {
        showPopup();
        event->accept();
        return true;
    }
    if (mode_ != Mode::Multi || watched != view()->viewport()) {
        return QComboBox::eventFilter(watched, event);
    }
    if (event->type() == QEvent::MouseButtonRelease) {
        auto* mouse = static_cast<QMouseEvent*>(event);
        if (mouse->button() == Qt::LeftButton) {
            toggleAt(view()->indexAt(mouse->position().toPoint()));
            event->accept();
            return true;   // 不把 release 交给弹出层 ⇒ 菜单不收起
        }
    }
    if (event->type() == QEvent::KeyPress) {
        auto* key = static_cast<QKeyEvent*>(event);
        if (key->key() == Qt::Key_Space) {
            toggleAt(view()->currentIndex());
            event->accept();
            return true;
        }
    }
    return QComboBox::eventFilter(watched, event);
}

}  // namespace xrkit::shell
