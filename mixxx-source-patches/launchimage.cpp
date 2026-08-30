#include "skin/legacy/launchimage.h"

#include <QDate>
#include <QAbstractAnimation>
#include <QEasingCurve>
#include <QGraphicsOpacityEffect>
#include <QHBoxLayout>
#include <QLabel>
#include <QParallelAnimationGroup>
#include <QPainter>
#include <QPropertyAnimation>
#include <QProgressBar>
#include <QSequentialAnimationGroup>
#include <QStyleOption>
#include <QVBoxLayout>

#include "moc_launchimage.cpp"

namespace {
bool isIn2024ChristmasHolidays() {
    auto currentDate = QDate::currentDate();
    return (currentDate.month() == 12 && currentDate.day() >= 24) ||
            (currentDate.month() == 1 && currentDate.day() <= 6);
}
} // namespace

LaunchImage::LaunchImage(QWidget* pParent, const QString& styleSheet)
        : QWidget(pParent) {
    const bool useZedLetterAnimation =
            styleSheet.contains(QStringLiteral("zedLaunchLetterZ"));
    if (isIn2024ChristmasHolidays()) {
        setStyleSheet(
                "LaunchImage { background-color: #202020; }"
                "QLabel { "
                "image: url(:/images/mixxx-icon-logo-christmas.svg);"
                "padding:0;"
                "margin:0;"
                "border:none;"
                "min-width: 236px;"
                "min-height: 48px;"
                "max-width: 236px;"
                "max-height: 48px;"
                "}"
                "QProgressBar {"
                "background-color: #202020; "
                "border:none;"
                "min-width: 236px;"
                "min-height: 3px;"
                "max-width: 236px;"
                "max-height: 3px;"
                "}"
                "QProgressBar::chunk { background-color: #f3efed; }");
    } else if (styleSheet.isEmpty()) {
        setStyleSheet(
                "LaunchImage { background-color: #202020; }"
                "QLabel { "
                "image: url(:/images/mixxx-icon-logo-symbolic.svg);"
                "padding:0;"
                "margin:0;"
                "border:none;"
                "min-width: 236px;"
                "min-height: 48px;"
                "max-width: 236px;"
                "max-height: 48px;"
                "}"
                "QProgressBar {"
                "background-color: #202020; "
                "border:none;"
                "min-width: 236px;"
                "min-height: 3px;"
                "max-width: 236px;"
                "max-height: 3px;"
                "}"
                "QProgressBar::chunk { background-color: #f3efed; }");
    } else {
        setStyleSheet(styleSheet);
    }

    m_pProgressBar = new QProgressBar(this);
    m_pProgressBar->setTextVisible(false);

    QHBoxLayout* hbox = new QHBoxLayout(this);
    QVBoxLayout* vbox = new QVBoxLayout();
    vbox->addStretch();
    if (useZedLetterAnimation) {
        auto* logoLayout = new QHBoxLayout();
        logoLayout->setContentsMargins(0, 0, 0, 0);
        logoLayout->setSpacing(2);
        auto* animationGroup = new QParallelAnimationGroup(this);
        const char* const objectNames[] = {
                "zedLaunchLetterZ", "zedLaunchLetterE", "zedLaunchLetterD"};
        for (int letter = 0; letter < 3; ++letter) {
            auto* label = new QLabel(this);
            label->setObjectName(QString::fromLatin1(objectNames[letter]));
            logoLayout->addWidget(label);

            auto* opacity = new QGraphicsOpacityEffect(label);
            opacity->setOpacity(0.0);
            label->setGraphicsEffect(opacity);

            auto* sequence = new QSequentialAnimationGroup();
            sequence->addPause(letter * 135);
            auto* reveal = new QPropertyAnimation(opacity, "opacity");
            reveal->setDuration(520);
            reveal->setKeyValueAt(0.0, 0.0);
            reveal->setKeyValueAt(0.52, 1.0);
            reveal->setKeyValueAt(0.68, 0.58);
            reveal->setKeyValueAt(1.0, 1.0);
            reveal->setEasingCurve(QEasingCurve::OutCubic);
            sequence->addAnimation(reveal);
            animationGroup->addAnimation(sequence);
        }
        vbox->addLayout(logoLayout);
        animationGroup->start(QAbstractAnimation::DeleteWhenStopped);
    } else {
        vbox->addWidget(new QLabel(this));
    }
    vbox->addWidget(m_pProgressBar);
    vbox->addStretch();
    hbox->addStretch();
    hbox->addLayout(vbox);
    hbox->addStretch();
}

void LaunchImage::progress(int value, const QString& serviceName) {
    m_pProgressBar->setValue(value);
    // TODO: show serviceName
    Q_UNUSED(serviceName);
}

void LaunchImage::paintEvent(QPaintEvent *)
{
    QStyleOption opt;
    opt.initFrom(this);
    QPainter p(this);
    style()->drawPrimitive(QStyle::PE_Widget, &opt, &p, this);
}
