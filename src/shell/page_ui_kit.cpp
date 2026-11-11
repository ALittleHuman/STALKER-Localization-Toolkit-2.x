#include "page_ui_kit.h"

#include <QVBoxLayout>

namespace xrkit::shell::ui {

SurfacePanel::SurfacePanel(QWidget* parent) : QWidget(parent) {
    setAutoFillBackground(false);
    setProperty("ui_surface", true);
}

ResultPane::ResultPane(QWidget* parent) : SurfacePanel(parent) {
    layout_ = new QVBoxLayout(this);
    layout_->setContentsMargins(0, 0, 0, 0);
    layout_->setSpacing(0);
}

void ResultPane::add_header(QWidget* widget, int stretch) {
    if (widget != nullptr) layout_->addWidget(widget, stretch);
}

void ResultPane::add_content(QWidget* widget, int stretch) {
    if (widget != nullptr) layout_->addWidget(widget, stretch);
}

void ResultPane::add_footer(QWidget* widget, int stretch) {
    if (widget != nullptr) layout_->addWidget(widget, stretch);
}

QWidget* make_transparent_row(QWidget* parent) {
    auto* row = new QWidget(parent);
    row->setObjectName(QStringLiteral("field_row"));
    row->setAutoFillBackground(false);
    row->setStyleSheet(QStringLiteral("background: transparent;"));
    return row;
}

}  // namespace xrkit::shell::ui

