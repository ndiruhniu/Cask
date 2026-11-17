#include "ui/MainWindow.h"

#include "core/Component.h"
#include "core/Config.h"
#include "core/LaunchSettings.h"
#include "core/Runner.h"
#include "ui/EnvDialog.h"
#include "ui/NewPrefixDialog.h"
#include "ui/SettingsDialog.h"
#include "ui/WinetricksDialog.h"

#include <QAbstractItemModel>
#include <QAction>
#include <QCloseEvent>
#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDesktopServices>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QFrame>
#include <QGridLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QLocale>
#include <QMenu>
#include <QMessageBox>
#include <QProgressBar>
#include <QPushButton>
#include <QScrollArea>
#include <QSplitter>
#include <QStatusBar>
#include <QTimer>
#include <QStyle>
#include <QToolBar>
#include <QToolButton>
#include <QIcon>
#include <QPainter>
#include <QPixmap>
#include <QPolygonF>
#include <QtMath>
#include <QUrl>
#include <QVBoxLayout>

namespace {

// A plain monochrome "refresh" icon drawn in the current button text color, so it follows any theme.
class RefreshButton : public QToolButton {
public:
    explicit RefreshButton(QWidget* parent = nullptr) : QToolButton(parent)
    {
        setAutoRaise(true);
        updateIcon();
    }

protected:
    void changeEvent(QEvent* e) override
    {
        QToolButton::changeEvent(e);
        if (e->type() == QEvent::PaletteChange)
            updateIcon();
    }

private:
    void updateIcon()
    {
        const qreal dpr = devicePixelRatioF();
        const int size = 16;
        QIcon icon;
        for (const QIcon::Mode mode : {QIcon::Normal, QIcon::Disabled}) {
            const QColor color = palette().color(mode == QIcon::Normal ? QPalette::Active : QPalette::Disabled,
                                                 QPalette::ButtonText);
            QPixmap pm(QSize(size, size) * dpr);
            pm.setDevicePixelRatio(dpr);
            pm.fill(Qt::transparent);
            QPainter p(&pm);
            p.setRenderHint(QPainter::Antialiasing);

            // An almost full circle (gap on the right) with an arrow head at its end.
            const QPointF c(size / 2.0, size / 2.0);
            const qreal r = 5.0;
            p.setPen(QPen(color, 1.5, Qt::SolidLine, Qt::RoundCap));
            p.setBrush(Qt::NoBrush);
            p.drawArc(QRectF(c.x() - r, c.y() - r, 2 * r, 2 * r), 50 * 16, 280 * 16);

            const qreal a = qDegreesToRadians(330.0);  // where the arc ends
            const QPointF end(c.x() + r * qCos(a), c.y() - r * qSin(a));
            const QPointF t(-qSin(a), -qCos(a));       // direction of travel (screen coordinates)
            const QPointF n(-t.y(), t.x());
            QPolygonF head;
            head << end + t * 3.0 << end - t * 0.5 + n * 2.8 << end - t * 0.5 - n * 2.8;
            p.setPen(Qt::NoPen);
            p.setBrush(color);
            p.drawPolygon(head);
            p.end();
            icon.addPixmap(pm, mode);
        }
        setIcon(icon);
    }
};

}  // namespace

MainWindow::MainWindow(QWidget* parent) : QMainWindow(parent)
{
    setWindowTitle(QStringLiteral("Cask"));

    m_busy = new QProgressBar;
    m_busy->setRange(0, 0);
    m_busy->setMaximumWidth(120);
    m_busy->setTextVisible(false);
    m_busy->hide();
    statusBar()->addPermanentWidget(m_busy);

    buildUi();

    connect(&m_mgr, &PrefixManager::changed, this, &MainWindow::refresh);
    connect(&m_mgr, &PrefixManager::busyChanged, this, [this](bool busy) {
        m_busy->setVisible(busy);
        m_central->setEnabled(!busy);
    });
    connect(&m_mgr, &PrefixManager::message, this, [this](const QString& text, bool ok) {
        statusBar()->showMessage(text, 8000);
        if (!ok)
            QMessageBox::warning(this, QStringLiteral("Cask"), text);
    });

    refresh();
}

void MainWindow::closeEvent(QCloseEvent* event)
{
    // Closing in the middle of wineboot/regedit would kill it and leave a half-initialized prefix.
    if (m_mgr.isBusy()) {
        statusBar()->showMessage(tr("Wait for the current operation to finish."), 5000);
        event->ignore();
        return;
    }
    QMainWindow::closeEvent(event);
}

// Size hints of widgets that are not polished yet (the style has not applied its fonts and margins) are
// too small with some themes, so the minimum size is recomputed once the window is shown and whenever the
// style or font changes.
void MainWindow::showEvent(QShowEvent* event)
{
    QMainWindow::showEvent(event);
    QTimer::singleShot(0, this, [this] { fitToContent(); });
}

// Some window managers (tiling, snapping to a screen corner) ignore the minimum size. The window then
// asks for its minimum size back, a few times at most so it never fights the window manager.
void MainWindow::resizeEvent(QResizeEvent* event)
{
    QMainWindow::resizeEvent(event);
    const QSize min = minimumSizeHint();
    if (width() >= min.width() && height() >= min.height()) {
        m_corrections = 0;
        return;
    }
    if (!isVisible() || m_corrections >= 3)
        return;
    ++m_corrections;
    QTimer::singleShot(0, this, [this] { resize(size().expandedTo(minimumSizeHint())); });
}

void MainWindow::changeEvent(QEvent* event)
{
    QMainWindow::changeEvent(event);
    if ((event->type() == QEvent::StyleChange || event->type() == QEvent::FontChange) && m_details)
        QTimer::singleShot(0, this, [this] { fitToContent(); });
}

void MainWindow::buildUi()
{
    // Toolbar
    m_toolbar = addToolBar(tr("Main"));
    m_toolbar->setMovable(false);
    auto addAct = [&](const QString& text, void (MainWindow::*slot)()) {
        QAction* a = m_toolbar->addAction(text);
        connect(a, &QAction::triggered, this, slot);
    };
    addAct(tr("New prefix"), &MainWindow::newPrefix);
    addAct(tr("Settings"), &MainWindow::openSettings);
    addAct(tr("About"), &MainWindow::showAbout);

    // Center: prefix list on the left, details on the right
    m_central = new QSplitter(this);
    m_central->setChildrenCollapsible(false);
    m_central->setHandleWidth(1);

    auto* left = new QWidget;
    left->setMinimumWidth(230);
    auto* lv = new QVBoxLayout(left);
    lv->setContentsMargins(14, 14, 8, 14);
    lv->setSpacing(10);

    auto* listTitle = new QLabel(tr("Prefixes"));
    QFont titleFont = listTitle->font();
    titleFont.setBold(true);
    listTitle->setFont(titleFont);

    // Refresh is a small icon button aligned to the right edge of the list.
    auto* refreshBtn = new RefreshButton;
    refreshBtn->setToolTip(tr("Refresh"));
    connect(refreshBtn, &QToolButton::clicked, this, &MainWindow::refresh);
    auto* head = new QHBoxLayout;
    head->setContentsMargins(0, 0, 0, 0);
    head->addWidget(listTitle);
    head->addStretch(1);
    head->addWidget(refreshBtn);
    lv->addLayout(head);

    // Plain list without custom styles: the look is fully determined by the system theme.
    m_list = new QListWidget;
    m_list->setSelectionMode(QAbstractItemView::SingleSelection);
    m_list->setDragEnabled(true);
    m_list->setAcceptDrops(true);
    m_list->setDropIndicatorShown(true);
    m_list->setDragDropMode(QAbstractItemView::InternalMove);
    m_list->setDefaultDropAction(Qt::MoveAction);
    m_list->setToolTip(tr("Drag prefixes to change their order"));
    connect(m_list, &QListWidget::currentRowChanged, this, [this](int) { onSelected(); });
    connect(m_list->model(), &QAbstractItemModel::rowsMoved, this, [this] { saveOrder(); });
    lv->addWidget(m_list, 1);
    m_central->addWidget(left);

    m_details = new QWidget;
    auto* v = new QVBoxLayout(m_details);
    v->setContentsMargins(12, 14, 16, 14);
    v->setSpacing(12);

    auto* info = new QGroupBox(tr("Prefix"));
    auto* form = new QFormLayout(info);
    m_name = new QLabel;
    m_path = new QLabel;
    m_path->setTextInteractionFlags(Qt::TextSelectableByMouse);
    m_path->setWordWrap(true);
    m_arch = new QLabel;
    // Wine changes the same way as components: a selection in the list is applied immediately.
    m_runnerBox = new QComboBox;
    connect(m_runnerBox, QOverload<int>::of(&QComboBox::activated), this, [this](int) {
        QTimer::singleShot(0, this, [this] { applyRunner(); });
    });
    form->addRow(tr("Name"), m_name);
    form->addRow(tr("Folder"), m_path);
    form->addRow(tr("Architecture"), m_arch);
    form->addRow(tr("Wine"), m_runnerBox);
    v->addWidget(info);

    // Components: "Disabled" by default; selecting a version applies it, "Disabled" removes it.
    auto* comps = new QGroupBox(tr("Components"));
    auto* grid = new QGridLayout(comps);
    int row = 0;
    for (const auto& t : Components::types()) {
        CompRow cr;
        cr.type = t.id;
        cr.versions = new QComboBox;
        cr.hint = new QLabel;
        cr.hint->setEnabled(false);  // dimmed color
        cr.hint->setWordWrap(true);
        cr.hint->hide();  // shown only when there is something to say
        cr.versions->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        grid->addWidget(new QLabel(t.title), row * 2, 0);
        grid->addWidget(cr.versions, row * 2, 1);
        grid->addWidget(cr.hint, row * 2 + 1, 1);
        const QString type = t.id;
        // activated fires only on user actions, not while the list is being filled.
        // Defer the handler so the list is not rebuilt inside its own signal.
        connect(cr.versions, QOverload<int>::of(&QComboBox::activated), this, [this, type](int) {
            QTimer::singleShot(0, this, [this, type] { componentChosen(type); });
        });
        m_rows.append(cr);
        ++row;
    }
    grid->setColumnStretch(1, 1);
    v->addWidget(comps);

    // Launch settings
    auto* launch = new QGroupBox(tr("Launch settings"));
    auto* lf = new QFormLayout(launch);

    m_localeBox = new QComboBox;
    m_localeBox->setEditable(true);
    m_localeBox->setInsertPolicy(QComboBox::NoInsert);
    m_localeBox->addItem(tr("System default"), QString());
    for (const QString& code : LaunchSettings::localePresets()) {
        const QString lang = QLocale(code.section(QLatin1Char('.'), 0, 0)).nativeLanguageName();
        m_localeBox->addItem(lang.isEmpty() ? code : QStringLiteral("%1 (%2)").arg(code, lang), code);
    }
    m_localeBox->lineEdit()->setPlaceholderText(tr("e.g. de_DE.UTF-8"));
    m_localeBox->setToolTip(
        tr("Language that Wine and the programs in this prefix see (sets LANG and LC_ALL). "
           "You can type your own value."));
    lf->addRow(tr("Wine language"), m_localeBox);

    m_gamemode = new QCheckBox(tr("GameMode"));
    m_gamemode->setToolTip(LaunchSettings::gamemodeBinary().isEmpty()
                               ? tr("gamemoderun was not found. Install the 'gamemode' package.")
                               : tr("Programs started with 'Run program…' are launched through gamemoderun."));

    m_mangohud = new QCheckBox(tr("MangoHud"));
    m_mangohud->setToolTip(LaunchSettings::mangohudBinary().isEmpty() && !LaunchSettings::hasPortableMangohud()
                               ? tr("mangohud was not found. Install the 'mangohud' package or put its files into %1.")
                                     .arg(LaunchSettings::mangohudDir() + QStringLiteral("/lib64"))
                               : tr("Programs started with 'Run program…' are launched through mangohud. "
                                    "Configure it with MANGOHUD_CONFIG in the environment variables."));
    auto* wrappers = new QHBoxLayout;
    wrappers->addWidget(m_gamemode);
    wrappers->addWidget(m_mangohud);
    wrappers->addStretch(1);
    lf->addRow(wrappers);

    m_envBtn = new QPushButton;
    lf->addRow(tr("Environment"), m_envBtn);
    updateEnvButton();

    connect(m_localeBox, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this] { saveSettings(); });
    connect(m_localeBox->lineEdit(), &QLineEdit::editingFinished, this, [this] { saveSettings(); });
    connect(m_gamemode, &QCheckBox::toggled, this, [this] { saveSettings(); });
    connect(m_mangohud, &QCheckBox::toggled, this, [this] { saveSettings(); });
    connect(m_envBtn, &QPushButton::clicked, this, &MainWindow::editEnv);
    v->addWidget(launch);

    // Actions
    auto* actions = new QHBoxLayout;
    auto* runBtn = new QPushButton(tr("Run program…"));
    connect(runBtn, &QPushButton::clicked, this, &MainWindow::runExe);
    auto* toolsBtn = new QPushButton(tr("Tools"));
    auto* menu = new QMenu(toolsBtn);
    auto addTool = [&](const QString& text, const QStringList& args) {
        QAction* a = menu->addAction(text);
        connect(a, &QAction::triggered, this, [this, args] { runTool(args); });
    };
    addTool(tr("Explorer"), {"explorer"});
    addTool(tr("Control panel"), {"control"});
    addTool(tr("Wine configuration (winecfg)"), {"winecfg"});
    addTool(tr("Registry editor (regedit)"), {"regedit"});
    addTool(tr("Add/remove programs"), {"uninstaller"});
    addTool(tr("Task manager"), {"taskmgr"});
    QAction* winetricks = menu->addAction(tr("Winetricks…"));
    connect(winetricks, &QAction::triggered, this, &MainWindow::openWinetricks);
    toolsBtn->setMenu(menu);
    auto* stopBtn = new QPushButton(tr("Stop all processes"));
    stopBtn->setToolTip(tr("Stops every Wine process of this prefix (wineserver -k, then kill)."));
    connect(stopBtn, &QPushButton::clicked, this, &MainWindow::stopAll);
    auto* folderBtn = new QPushButton(tr("Open folder"));
    connect(folderBtn, &QPushButton::clicked, this, &MainWindow::openFolder);
    auto* delBtn = new QPushButton(tr("Delete prefix"));
    connect(delBtn, &QPushButton::clicked, this, &MainWindow::deletePrefix);
    actions->addWidget(runBtn);
    actions->addWidget(toolsBtn);
    actions->addWidget(stopBtn);
    actions->addWidget(folderBtn);
    actions->addStretch(1);
    actions->addWidget(delBtn);
    v->addLayout(actions);
    v->addStretch(1);

    auto* scroll = new QScrollArea;
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    scroll->setWidget(m_details);
    m_central->addWidget(scroll);
    m_central->setStretchFactor(0, 0);
    m_central->setStretchFactor(1, 1);
    m_central->setSizes({270, 750});
    setCentralWidget(m_central);
    m_central->setEnabled(!m_mgr.isBusy());
    fitToContent();
}

// The window may not become smaller than what the details panel needs (nothing is cut off or
// hidden behind a scroll bar) and starts at the size that exactly fits it.
void MainWindow::fitToContent()
{
    m_details->ensurePolished();  // hints are only reliable after the style has been applied
    const int leftMin = 230;
    const int handle = m_central->handleWidth();
    const QSize detailsMin = m_details->minimumSizeHint();
    const QSize detailsWanted = m_details->sizeHint().expandedTo(detailsMin);

    m_central->widget(1)->setMinimumSize(detailsMin);
    const QSize centralMin(leftMin + handle + detailsMin.width(), detailsMin.height());
    m_central->setMinimumSize(centralMin);

    layout()->activate();
    const QSize chrome = minimumSizeHint() - centralMin;  // tool bar, status bar, frame
    const QSize wanted = chrome + QSize(leftMin + 40 + handle + detailsWanted.width(), detailsWanted.height());
    if (!m_sized) {  // later calls keep the size and splitter position the user has chosen
        m_central->setSizes({leftMin + 40, detailsWanted.width()});
        resize(wanted.expandedTo(minimumSizeHint()));
        m_sized = true;
    } else {
        resize(size().expandedTo(minimumSizeHint()));  // the minimum may have grown (new style or font)
    }
}

void MainWindow::retranslate()
{
    const QString keep = currentName();
    removeToolBar(m_toolbar);
    delete m_toolbar;
    m_toolbar = nullptr;
    m_rows.clear();
    buildUi();  // setCentralWidget deletes the previous central widget
    refresh();
    if (!keep.isEmpty())
        selectByName(keep);
}

QString MainWindow::currentName() const
{
    const QListWidgetItem* it = m_list->currentItem();
    return it ? it->data(Qt::UserRole).toString() : QString();
}

void MainWindow::selectByName(const QString& name)
{
    for (int i = 0; i < m_list->count(); ++i) {
        if (m_list->item(i)->data(Qt::UserRole).toString() == name) {
            m_list->setCurrentRow(i);
            return;
        }
    }
}

void MainWindow::saveOrder()
{
    QStringList names;
    for (int i = 0; i < m_list->count(); ++i)
        names << m_list->item(i)->data(Qt::UserRole).toString();
    m_mgr.setOrder(names);
}

void MainWindow::refresh()
{
    const QString keep = currentName();
    {
        const QSignalBlocker blocker(m_list);
        m_list->clear();
        for (const Prefix& p : m_mgr.list()) {
            auto* it = new QListWidgetItem(p.name);
            it->setData(Qt::UserRole, p.name);
            it->setSizeHint(QSize(0, 30));  // slightly taller than the standard row: easier to hit with the mouse
            m_list->addItem(it);
        }
        if (!keep.isEmpty())
            selectByName(keep);
        if (!m_list->currentItem() && m_list->count() > 0)
            m_list->setCurrentRow(0);
    }
    onSelected();
}

QString MainWindow::localeValue() const
{
    // The text matched a list item: use its value, otherwise it is a manually entered locale.
    const QString text = m_localeBox->currentText().trimmed();
    const int idx = m_localeBox->findText(text, Qt::MatchFixedString);
    return idx >= 0 ? m_localeBox->itemData(idx).toString() : text;
}

void MainWindow::setLocaleValue(const QString& value)
{
    const int idx = m_localeBox->findData(value);
    if (idx >= 0)
        m_localeBox->setCurrentIndex(idx);
    else
        m_localeBox->setEditText(value);
}

void MainWindow::updateEnvButton()
{
    m_envBtn->setText(m_env.isEmpty() ? tr("Edit variables…") : tr("Edit variables (%1)…").arg(m_env.size()));
}

void MainWindow::onSelected()
{
    const QString name = currentName();
    const auto po = m_mgr.find(name);
    m_details->setEnabled(po.has_value());
    if (!po) {
        m_name->clear();
        m_path->clear();
        m_arch->clear();
        m_runnerBox->clear();
        for (CompRow& cr : m_rows) {
            cr.versions->clear();
            cr.hint->clear();
            cr.hint->hide();
        }
        m_loading = true;
        m_env.clear();
        updateEnvButton();
        setLocaleValue(QString());
        m_gamemode->setChecked(false);
        m_mangohud->setChecked(false);
        m_loading = false;
        return;
    }
    const Prefix& p = *po;
    m_name->setText(p.name);
    m_path->setText(p.path);
    m_arch->setText(p.arch == "win64" ? "64-bit (win64)" : "32-bit (win32)");

    m_runnerBox->clear();
    for (const Runner& r : RunnerManager::available())
        m_runnerBox->addItem(r.system ? tr("System Wine") : r.name, r.name);
    int idx = m_runnerBox->findData(p.runner);
    if (idx < 0) {
        m_runnerBox->addItem(tr("%1 (not found)").arg(p.runner), p.runner);
        idx = m_runnerBox->count() - 1;
    }
    m_runnerBox->setCurrentIndex(idx);

    for (CompRow& cr : m_rows) {
        const QStringList vers = Components::versions(cr.type);
        cr.versions->clear();
        cr.versions->addItem(tr("Disabled"), QString());
        for (const QString& v : vers)
            cr.versions->addItem(v, v);

        // Show the version actually applied; if its folder is gone from data, add an item for it.
        const auto it = p.components.constFind(cr.type);
        int sel = 0;
        if (it != p.components.constEnd()) {
            sel = cr.versions->findData(it->version);
            if (sel < 0) {
                cr.versions->addItem(tr("%1 (not found)").arg(it->version), it->version);
                sel = cr.versions->count() - 1;
            }
        }
        cr.versions->setCurrentIndex(sel);
        cr.hint->setText(vers.isEmpty()
                             ? tr("Put version folders into %1").arg(Config::instance().componentsDir(cr.type))
                             : QString());
        cr.hint->setVisible(vers.isEmpty());
    }
    m_details->layout()->invalidate();
    fitToContent();  // a hint line may have appeared or disappeared

    m_loading = true;
    setLocaleValue(p.settings.locale);
    m_gamemode->setChecked(p.settings.gamemode);
    m_mangohud->setChecked(p.settings.mangohud);
    m_env = p.settings.env;
    updateEnvButton();
    m_loading = false;
}

void MainWindow::componentChosen(const QString& type)
{
    const QString name = currentName();
    const auto p = m_mgr.find(name);
    if (!p)
        return;
    for (const CompRow& cr : std::as_const(m_rows)) {
        if (cr.type != type)
            continue;
        const QString chosen = cr.versions->currentData().toString();
        const auto it = p->components.constFind(type);
        const QString applied = it == p->components.constEnd() ? QString() : it->version;
        if (chosen == applied)
            return;

        QString err;
        const bool ok = chosen.isEmpty() ? m_mgr.removeComponent(name, type, &err)
                                         : m_mgr.applyComponent(name, type, chosen, &err);
        if (!ok) {
            QMessageBox::warning(this, QStringLiteral("Cask"), err);
            onSelected();  // restore the list to the real state
        }
        return;
    }
}

void MainWindow::saveSettings()
{
    if (m_loading)
        return;
    const QString name = currentName();
    if (name.isEmpty())
        return;

    PrefixSettings s;
    s.locale = localeValue();
    s.gamemode = m_gamemode->isChecked();
    s.mangohud = m_mangohud->isChecked();
    s.env = m_env;

    QString err;
    if (!m_mgr.setSettings(name, s, &err))
        QMessageBox::warning(this, QStringLiteral("Cask"), err);
}

void MainWindow::editEnv()
{
    if (currentName().isEmpty())
        return;
    EnvDialog dlg(m_env, this);
    if (dlg.exec() != QDialog::Accepted)
        return;
    m_env = dlg.env();
    updateEnvButton();
    saveSettings();
}

void MainWindow::newPrefix()
{
    NewPrefixDialog dlg(this);
    if (dlg.exec() != QDialog::Accepted)
        return;
    QString err;
    if (!m_mgr.create(dlg.prefixName(), dlg.runner(), dlg.arch(), &err)) {
        QMessageBox::warning(this, QStringLiteral("Cask"), err);
        return;
    }
    selectByName(dlg.prefixName());
}

void MainWindow::deletePrefix()
{
    const QString name = currentName();
    if (name.isEmpty())
        return;
    // Standard buttons (not custom ones), so they get the same icons as the other dialogs.
    QMessageBox box(QMessageBox::Question, QStringLiteral("Cask"),
                    tr("Delete prefix '%1' with all its data? This cannot be undone.").arg(name),
                    QMessageBox::Yes | QMessageBox::No, this);
    box.setDefaultButton(QMessageBox::No);
    box.button(QMessageBox::Yes)->setText(tr("Yes"));
    box.button(QMessageBox::No)->setText(tr("No"));
    if (box.exec() != QMessageBox::Yes)
        return;
    QString err;
    if (!m_mgr.remove(name, &err))
        QMessageBox::warning(this, QStringLiteral("Cask"), err);
}

void MainWindow::openSettings()
{
    SettingsDialog dlg(this);
    if (dlg.exec() != QDialog::Accepted)
        return;
    if (dlg.languageChanged())
        retranslate();
    else
        refresh();
}

void MainWindow::showAbout()
{
    QMessageBox box(this);
    box.setWindowTitle(tr("About"));
    box.setIcon(QMessageBox::NoIcon);
    box.setTextFormat(Qt::RichText);
    box.setText(QStringLiteral("<h2>Cask</h2><p>%1</p><p>%2</p>")
                    .arg(tr("Minimalist Wine prefix manager").toHtmlEscaped(),
                         tr("Version %1").arg(QStringLiteral(CASK_VERSION)).toHtmlEscaped()));
    box.exec();
}

void MainWindow::runExe()
{
    const QString name = currentName();
    if (name.isEmpty())
        return;
    const QString file = QFileDialog::getOpenFileName(
        this, tr("Choose a program"), QString(),
        tr("Windows programs (*.exe *.msi *.bat *.lnk);;All files (*)"));
    if (file.isEmpty())
        return;
    const QFileInfo fi(file);
    QString err;
    if (!m_mgr.launch(name, PrefixManager::programArgs(file), fi.absolutePath(), &err, true))
        QMessageBox::warning(this, QStringLiteral("Cask"), err);
}

void MainWindow::runTool(const QStringList& wineArgs)
{
    const QString name = currentName();
    if (name.isEmpty())
        return;
    QString err;
    if (!m_mgr.launch(name, wineArgs, QString(), &err))
        QMessageBox::warning(this, QStringLiteral("Cask"), err);
}

void MainWindow::openFolder()
{
    const auto p = m_mgr.find(currentName());
    if (p)
        QDesktopServices::openUrl(QUrl::fromLocalFile(p->path));
}

void MainWindow::applyRunner()
{
    const QString name = currentName();
    const QString runner = m_runnerBox->currentData().toString();
    if (name.isEmpty() || runner.isEmpty())
        return;
    const auto p = m_mgr.find(name);
    if (p && p->runner == runner)
        return;
    QString err;
    if (!m_mgr.setRunner(name, runner, &err)) {
        QMessageBox::warning(this, QStringLiteral("Cask"), err);
        onSelected();  // restore the list to the prefix's real Wine
    }
}

void MainWindow::stopAll()
{
    const QString name = currentName();
    if (name.isEmpty())
        return;
    QApplication::setOverrideCursor(Qt::WaitCursor);  // stopping may take a few seconds
    QString err;
    const bool ok = m_mgr.stopAll(name, &err);
    QApplication::restoreOverrideCursor();
    if (!ok)
        QMessageBox::warning(this, QStringLiteral("Cask"), err);
}

void MainWindow::openWinetricks()
{
    const auto p = m_mgr.find(currentName());
    if (!p)
        return;
    WinetricksDialog dlg(&m_mgr, *p, this);
    dlg.exec();
}
