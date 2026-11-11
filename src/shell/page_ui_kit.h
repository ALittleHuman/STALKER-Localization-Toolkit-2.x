#pragma once
// xrkit::shell::ui —— 新页面骨架的共用件。
//
// 硬规则（2026-11-11 UI 重写）：
//   · 容器默认不画底；需要底的地方只有 SurfacePanel / ResultPane；
//   · 一个 ResultPane 只有一个 stretch 目标（content）；
//   · 工具行/页脚按 0 stretch 固定，不参与剩余高度分配。

#include <QWidget>

class QVBoxLayout;

namespace xrkit::shell::ui {

class SurfacePanel : public QWidget {
public:
    explicit SurfacePanel(QWidget* parent = nullptr);
};

class ResultPane : public SurfacePanel {
public:
    explicit ResultPane(QWidget* parent = nullptr);

    void add_header(QWidget* widget, int stretch = 0);
    void add_content(QWidget* widget, int stretch = 1);
    void add_footer(QWidget* widget, int stretch = 0);

    QVBoxLayout* content_layout() const { return layout_; }

private:
    QVBoxLayout* layout_ = nullptr;
};

// 透明的“行”容器：仍保留 field_row 的 QSS 约定，但不再依赖全局 QWidget 底色。
QWidget* make_transparent_row(QWidget* parent = nullptr);

}  // namespace xrkit::shell::ui
