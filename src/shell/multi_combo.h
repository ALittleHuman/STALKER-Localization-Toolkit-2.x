#pragma once
// xrkit::shell —— 一个下拉实现、两种模式：
//   · Single：普通 QComboBox 行为（选中即收起）；
//   · Multi ：勾选式，点一项保持展开，只改勾选状态；显示文本是汇总。
//
// 字体页的「字号」用它；其余页仍是同一 QComboBox 观感，不另造一套控件。

#include <QComboBox>
#include <QVariantList>

class QStandardItemModel;

namespace xrkit::shell {

class MultiSelectComboBox : public QComboBox {
    Q_OBJECT
public:
    enum class Mode { Single, Multi };

    explicit MultiSelectComboBox(QWidget* parent = nullptr);

    void setMode(Mode mode);
    Mode mode() const { return mode_; }

    void addChoice(const QString& text, const QVariant& data, bool checked = false,
                   bool enabled = true);
    void clearChoices();

    QVariantList checkedData() const;
    void setCheckedData(const QVariantList& data);
    void setChoiceChecked(const QVariant& data, bool checked);
    bool isChoiceChecked(const QVariant& data) const;

    // multi 模式下把勾选项汇总成一行；single 模式回当前项文本。
    QString summary() const;
    void refreshDisplay();

signals:
    void checkedChanged();

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    void toggleAt(const QModelIndex& index);

    Mode mode_ = Mode::Single;
    bool updating_ = false;
    QStandardItemModel* model_ = nullptr;
};

}  // namespace xrkit::shell
