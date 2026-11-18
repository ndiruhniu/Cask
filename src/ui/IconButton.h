#pragma once

#include <QEvent>
#include <QIcon>
#include <QPainter>
#include <QPainterPath>
#include <QPalette>
#include <QPixmap>
#include <QToolButton>
#include <QtMath>

// A flat tool button with a simple monochrome icon that is drawn in the colour of the current theme
// (so it fits any theme and any DPI, and no icon files or icon theme are needed).
class IconButton : public QToolButton {
public:
    enum Kind { Play, Stop, Terminal, Folder, Trash };

    explicit IconButton(Kind kind, QWidget* parent = nullptr) : QToolButton(parent), m_kind(kind)
    {
        setAutoRaise(true);
        setIconSize(QSize(16, 16));
        setFixedSize(26, 26);
        setFocusPolicy(Qt::NoFocus);
        rebuild();
    }

    void setKind(Kind kind)
    {
        if (kind == m_kind)
            return;
        m_kind = kind;
        rebuild();
    }

protected:
    void changeEvent(QEvent* e) override
    {
        QToolButton::changeEvent(e);
        if (e->type() == QEvent::PaletteChange)
            rebuild();
    }

private:
    void rebuild()
    {
        const qreal dpr = devicePixelRatioF();
        const qreal size = 16;
        QIcon icon;
        for (const QIcon::Mode mode : {QIcon::Normal, QIcon::Disabled}) {
            const QColor color = palette().color(mode == QIcon::Normal ? QPalette::Active : QPalette::Disabled,
                                                 QPalette::ButtonText);
            QPixmap pm(QSize(int(size), int(size)) * dpr);
            pm.setDevicePixelRatio(dpr);
            pm.fill(Qt::transparent);
            QPainter p(&pm);
            p.setRenderHint(QPainter::Antialiasing);
            draw(p, color);
            p.end();
            icon.addPixmap(pm, mode);
        }
        setIcon(icon);
    }

    void draw(QPainter& p, const QColor& color) const
    {
        const QPen line(color, 1.4, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin);
        p.setPen(Qt::NoPen);
        p.setBrush(Qt::NoBrush);
        switch (m_kind) {
        case Play:
            p.setBrush(color);
            p.drawPolygon(QPolygonF{{4.5, 2.5}, {13.0, 8.0}, {4.5, 13.5}});
            break;
        case Stop:
            p.setBrush(color);
            p.drawRoundedRect(QRectF(3.5, 3.5, 9.0, 9.0), 1.2, 1.2);
            break;
        case Terminal:
            p.setPen(line);
            p.drawRoundedRect(QRectF(1.5, 2.5, 13.0, 11.0), 1.8, 1.8);
            p.drawPolyline(QPolygonF{{4.5, 6.0}, {7.0, 8.0}, {4.5, 10.0}});
            p.drawLine(QPointF(8.5, 10.5), QPointF(11.5, 10.5));
            break;
        case Folder: {
            p.setPen(line);
            QPainterPath path;
            path.moveTo(1.5, 4.0);
            path.lineTo(6.0, 4.0);
            path.lineTo(7.5, 5.8);
            path.lineTo(14.5, 5.8);
            path.lineTo(14.5, 13.0);
            path.lineTo(1.5, 13.0);
            path.closeSubpath();
            p.drawPath(path);
            break;
        }
        case Trash: {
            p.setPen(line);
            p.drawLine(QPointF(2.5, 4.5), QPointF(13.5, 4.5));
            p.drawPolyline(QPolygonF{{6.0, 4.5}, {6.0, 2.8}, {10.0, 2.8}, {10.0, 4.5}});
            QPainterPath body;
            body.moveTo(3.8, 6.0);
            body.lineTo(4.6, 13.5);
            body.lineTo(11.4, 13.5);
            body.lineTo(12.2, 6.0);
            p.drawPath(body);
            p.drawLine(QPointF(6.6, 7.8), QPointF(6.8, 12.0));
            p.drawLine(QPointF(9.4, 7.8), QPointF(9.2, 12.0));
            break;
        }
        }
    }

    Kind m_kind;
};
