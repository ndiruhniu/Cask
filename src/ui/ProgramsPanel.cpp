#include "ui/ProgramsPanel.h"

#include "ui/IconButton.h"

#include <QAbstractItemModel>
#include <QCursor>
#include <QDesktopServices>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QPushButton>
#include <QStackedWidget>
#include <QUrl>
#include <QVBoxLayout>

namespace {

// A list that asks for a modest height: the window should not become tall because of it.
class ShortList : public QListWidget {
public:
    using QListWidget::QListWidget;
    QSize sizeHint() const override { return {200, 112}; }
    QSize minimumSizeHint() const override { return {100, 84}; }
};

}  // namespace

ProgramsPanel::ProgramsPanel(PrefixManager* mgr, QWidget* parent) : QGroupBox(tr("Programs"), parent), m_mgr(mgr)
{
    auto* v = new QVBoxLayout(this);

    m_list = new ShortList;
    m_list->setSelectionMode(QAbstractItemView::SingleSelection);
    m_list->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
    m_list->setDragEnabled(true);
    m_list->setAcceptDrops(true);
    m_list->setDropIndicatorShown(true);
    m_list->setDragDropMode(QAbstractItemView::InternalMove);
    m_list->setDefaultDropAction(Qt::MoveAction);

    auto* hint = new QLabel(tr("No programs yet"));
    hint->setToolTip(tr("Programs installed in this prefix appear here automatically. You can also add one "
                        "with the button below."));
    hint->setAlignment(Qt::AlignCenter);
    hint->setEnabled(false);  // dimmed

    m_stack = new QStackedWidget;
    m_stack->addWidget(m_list);
    m_stack->addWidget(hint);
    v->addWidget(m_stack, 1);

    m_add = new QPushButton(tr("Add program…"));
    auto* bottom = new QHBoxLayout;
    bottom->addStretch(1);
    bottom->addWidget(m_add);
    v->addLayout(bottom);

    connect(m_add, &QPushButton::clicked, this, &ProgramsPanel::add);
    connect(m_list, &QListWidget::itemClicked, this, [this](QListWidgetItem* it) { clicked(m_list->row(it)); });
    // After a drop the row widgets are gone (the view recreates the items): save the order, then rebuild.
    connect(m_list->model(), &QAbstractItemModel::rowsMoved, this,
            [this] { QTimer::singleShot(0, this, [this] { applyOrder(); }); });

    m_poll.setInterval(2000);
    connect(&m_poll, &QTimer::timeout, this, &ProgramsPanel::updateRunning);
    m_poll.start();
}

void ProgramsPanel::setPrefix(const QString& name)
{
    m_prefix = name;
    m_running.clear();
    m_hadProcesses = false;
    if (name.isEmpty()) {
        m_path.clear();
        m_programs.clear();
        m_hidden.clear();
        rebuild();
        return;
    }
    m_mgr->syncPrograms(name);
    reload(true);
}

void ProgramsPanel::rescan()
{
    if (m_prefix.isEmpty() || m_editing >= 0 || m_list->state() == QAbstractItemView::DraggingState)
        return;
    m_mgr->syncPrograms(m_prefix);
    reload(false);
}

void ProgramsPanel::reload(bool force)
{
    const auto p = m_mgr->find(m_prefix);
    if (!p) {
        m_prefix.clear();
        m_programs.clear();
        m_hidden.clear();
        rebuild();
        return;
    }
    m_path = p->path;
    if (!force && p->programs == m_programs && p->hiddenPrograms == m_hidden)
        return;
    m_programs = p->programs;
    m_hidden = p->hiddenPrograms;
    rebuild();
}

QString ProgramsPanel::fileOf(int index) const
{
    return Programs::resolve(m_path, m_programs.at(index).path);
}

bool ProgramsPanel::isRunning(int index) const
{
    const QString file = fileOf(index);
    return file.endsWith(QLatin1String(".exe"), Qt::CaseInsensitive) && m_running.contains(Programs::imageName(file));
}

void ProgramsPanel::warn(const QString& text)
{
    QMessageBox::warning(this, QStringLiteral("Cask"), text);
}

void ProgramsPanel::rebuild()
{
    m_editing = -1;
    m_list->clear();
    m_rows.clear();

    for (int i = 0; i < m_programs.size(); ++i) {
        const Program& pr = m_programs.at(i);
        const QString file = fileOf(i);

        // The item carries the name (it is what the drag pixmap shows), the widget on it draws the row.
        auto* item = new QListWidgetItem(pr.name);
        item->setData(Qt::UserRole, pr.path);
        item->setSizeHint(QSize(0, 32));
        m_list->addItem(item);

        Row r;
        r.widget = new QWidget;
        r.widget->setAutoFillBackground(true);
        r.widget->setBackgroundRole(QPalette::Base);
        r.widget->setToolTip(file);
        auto* h = new QHBoxLayout(r.widget);
        h->setContentsMargins(8, 0, 4, 0);
        h->setSpacing(2);

        // The name does not take mouse events: a click on it reaches the list (itemClicked), so rows can
        // still be dragged; the click starts renaming.
        r.name = new QLabel(pr.name);
        r.name->setAttribute(Qt::WA_TransparentForMouseEvents);
        r.name->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
        r.edit = new QLineEdit;
        r.edit->hide();
        r.edit->installEventFilter(this);
        connect(r.edit, &QLineEdit::editingFinished, this, [this, i] {
            if (m_editing == i)
                finishEdit(i, true);
        });

        r.run = new IconButton(IconButton::Play);
        r.run->setToolTip(tr("Run"));
        auto* term = new IconButton(IconButton::Terminal);
        term->setToolTip(tr("Run in terminal"));
        auto* folder = new IconButton(IconButton::Folder);
        folder->setToolTip(tr("Show in folder"));
        auto* trash = new IconButton(IconButton::Trash);
        trash->setToolTip(tr("Remove from the list"));
        connect(r.run, &QToolButton::clicked, this, [this, i] { runOrStop(i); });
        connect(term, &QToolButton::clicked, this, [this, i] { runInTerminal(i); });
        connect(folder, &QToolButton::clicked, this, [this, i] { showFolder(i); });
        connect(trash, &QToolButton::clicked, this, [this, i] { remove(i); });

        h->addWidget(r.name, 1);
        h->addWidget(r.edit, 1);
        h->addWidget(r.run);
        h->addWidget(term);
        h->addWidget(folder);
        h->addWidget(trash);

        m_list->setItemWidget(item, r.widget);
        m_rows.append(r);
    }
    m_stack->setCurrentIndex(m_programs.isEmpty() ? 1 : 0);
    m_add->setEnabled(!m_prefix.isEmpty());
    updateRunning();
}

void ProgramsPanel::updateRunning()
{
    if (m_prefix.isEmpty() || !isVisible() || window()->isMinimized() || m_list->state() == QAbstractItemView::DraggingState)
        return;
    m_running = PrefixManager::runningImages(m_path);
    const bool any = !m_running.isEmpty();
    // Everything has finished (for example an installer): it may have created new shortcuts.
    if (m_hadProcesses && !any && m_editing < 0) {
        m_hadProcesses = false;
        rescan();
    }
    m_hadProcesses = any;

    for (int i = 0; i < m_rows.size() && i < m_programs.size(); ++i) {
        const bool running = isRunning(i);
        m_rows[i].run->setKind(running ? IconButton::Stop : IconButton::Play);
        m_rows[i].run->setToolTip(running ? tr("Stop") : tr("Run"));
    }
}

void ProgramsPanel::applyOrder()
{
    QList<Program> pool = m_programs;
    QList<Program> ordered;
    for (int i = 0; i < m_list->count(); ++i) {
        const QString path = m_list->item(i)->data(Qt::UserRole).toString();
        for (int k = 0; k < pool.size(); ++k) {
            if (pool.at(k).path == path) {
                ordered.append(pool.takeAt(k));
                break;
            }
        }
    }
    if (ordered.size() == m_programs.size() && ordered != m_programs) {
        m_programs = ordered;
        save();
    }
    rebuild();
}

void ProgramsPanel::save()
{
    QString err;
    if (!m_mgr->setPrograms(m_prefix, m_programs, m_hidden, &err))
        warn(err);
}

void ProgramsPanel::clicked(int index)
{
    if (index < 0 || index >= m_rows.size())
        return;
    const Row& r = m_rows.at(index);
    if (r.name->isVisible() && r.name->rect().contains(r.name->mapFromGlobal(QCursor::pos())))
        startEdit(index);
}

void ProgramsPanel::startEdit(int index)
{
    if (m_editing >= 0 && m_editing < m_rows.size())
        finishEdit(m_editing, true);
    if (index >= m_rows.size())
        return;
    m_editing = index;
    Row& r = m_rows[index];
    r.edit->setText(m_programs.at(index).name);
    r.name->hide();
    r.edit->show();
    r.edit->setFocus();
    r.edit->selectAll();
}

void ProgramsPanel::finishEdit(int index, bool commit)
{
    m_editing = -1;  // first: hiding the editor makes it lose the focus and report editingFinished again
    if (index < 0 || index >= m_rows.size() || index >= m_programs.size())
        return;
    Row& r = m_rows[index];
    if (commit) {
        QString name = r.edit->text().trimmed();
        if (name.isEmpty())
            name = QFileInfo(m_programs.at(index).path).fileName();
        if (name != m_programs.at(index).name) {
            m_programs[index].name = name;
            r.name->setText(name);
            save();
        }
    }
    r.edit->hide();
    r.name->show();
}

bool ProgramsPanel::eventFilter(QObject* watched, QEvent* event)
{
    if (event->type() == QEvent::KeyPress && static_cast<QKeyEvent*>(event)->key() == Qt::Key_Escape && m_editing >= 0
        && m_editing < m_rows.size() && watched == m_rows.at(m_editing).edit) {
        finishEdit(m_editing, false);
        return true;
    }
    return QGroupBox::eventFilter(watched, event);
}

void ProgramsPanel::runOrStop(int index)
{
    if (index < 0 || index >= m_programs.size())
        return;
    const QString file = fileOf(index);
    if (isRunning(index)) {
        m_mgr->stopProgram(m_path, Programs::imageName(file));
        QTimer::singleShot(600, this, [this] { updateRunning(); });
        QTimer::singleShot(3600, this, [this] { updateRunning(); });
        return;
    }
    if (!QFileInfo::exists(file)) {
        warn(tr("File not found: %1").arg(file));
        return;
    }
    QString err;
    if (!m_mgr->launch(m_prefix, PrefixManager::programArgs(file), QFileInfo(file).absolutePath(), &err, true))
        warn(err);
    QTimer::singleShot(1500, this, [this] { updateRunning(); });
    QTimer::singleShot(3500, this, [this] { updateRunning(); });
}

void ProgramsPanel::runInTerminal(int index)
{
    if (index < 0 || index >= m_programs.size())
        return;
    const QString file = fileOf(index);
    if (!QFileInfo::exists(file)) {
        warn(tr("File not found: %1").arg(file));
        return;
    }
    QString err;
    if (!m_mgr->launchInTerminal(m_prefix, PrefixManager::programArgs(file), QFileInfo(file).absolutePath(), &err))
        warn(err);
    QTimer::singleShot(1500, this, [this] { updateRunning(); });
}

void ProgramsPanel::showFolder(int index)
{
    if (index < 0 || index >= m_programs.size())
        return;
    const QString dir = QFileInfo(fileOf(index)).absolutePath();
    if (!QFileInfo(dir).isDir()) {
        warn(tr("Folder not found: %1").arg(dir));
        return;
    }
    QDesktopServices::openUrl(QUrl::fromLocalFile(dir));
}

void ProgramsPanel::remove(int index)
{
    if (index < 0 || index >= m_programs.size())
        return;
    // Remembered, so an installed program that was removed from the list is not added again.
    const Program pr = m_programs.takeAt(index);
    if (!m_hidden.contains(pr.path))
        m_hidden << pr.path;
    save();
    QTimer::singleShot(0, this, [this] { rebuild(); });  // not inside the click of a button that is deleted
}

void ProgramsPanel::add()
{
    if (m_prefix.isEmpty())
        return;
    QString start = m_path + "/drive_c";
    if (QFileInfo(start + "/Program Files").isDir())
        start += "/Program Files";
    const QString file = QFileDialog::getOpenFileName(
        this, tr("Choose a program"), start, tr("Windows programs (*.exe *.msi *.bat *.lnk);;All files (*)"));
    if (file.isEmpty())
        return;

    const QString stored = Programs::store(m_path, file);
    const QString resolved = Programs::resolve(m_path, stored);
    for (const Program& pr : std::as_const(m_programs)) {
        if (Programs::resolve(m_path, pr.path).compare(resolved, Qt::CaseInsensitive) == 0) {
            QMessageBox::information(this, QStringLiteral("Cask"), tr("This program is already in the list."));
            return;
        }
    }
    // Adding a program by hand also cancels an earlier removal of it.
    for (int i = m_hidden.size() - 1; i >= 0; --i)
        if (Programs::resolve(m_path, m_hidden.at(i)).compare(resolved, Qt::CaseInsensitive) == 0)
            m_hidden.removeAt(i);
    m_programs.append({QFileInfo(file).fileName(), stored});
    save();
    rebuild();
    m_list->scrollToBottom();
}
