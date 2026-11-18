#pragma once

#include <QLabel>
#include <QPainter>
#include <QStyle>
#include <QStyleOption>

// A one-line label that shortens a long text in the middle ("/home/user/…/prefixes/game") instead of wrapping it.
// A wrapped label makes the layout's height depend on its width, which a window minimum size cannot follow;
// this one has a fixed height. The full text is the tooltip unless another tooltip is set.
class ElidedLabel : public QLabel {
public:
    explicit ElidedLabel(QWidget* parent = nullptr) : QLabel(parent)
    {
        setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    }

    void setFullText(const QString& text)
    {
        m_full = text;
        setText(text);
        if (!m_customTip)
            QLabel::setToolTip(text);
        updateGeometry();
        update();
    }
    void setToolTip(const QString& tip)
    {
        m_customTip = !tip.isEmpty();
        QLabel::setToolTip(tip);
    }

    QSize sizeHint() const override
    {
        return QSize(qMin(fontMetrics().horizontalAdvance(m_full), 420) + 2 * margin(), fontMetrics().height() + 2);
    }
    QSize minimumSizeHint() const override { return QSize(fontMetrics().averageCharWidth() * 8, fontMetrics().height() + 2); }

protected:
    void paintEvent(QPaintEvent*) override
    {
        QPainter p(this);
        const QRect r = contentsRect().adjusted(margin(), 0, -margin(), 0);
        style()->drawItemText(&p, r, int(alignment()) | Qt::AlignVCenter, palette(), isEnabled(),
                              fontMetrics().elidedText(m_full, Qt::ElideMiddle, r.width()), foregroundRole());
    }

private:
    int margin() const { return 0; }
    QString m_full;
    bool m_customTip = false;
};
