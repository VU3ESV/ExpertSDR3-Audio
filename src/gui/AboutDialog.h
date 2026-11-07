#pragma once

#include <QDialog>

namespace esa {

class AboutDialog : public QDialog {
    Q_OBJECT

public:
    explicit AboutDialog(QWidget* parent = nullptr);
};

} // namespace esa
