#pragma once

#include "core/PrefixManager.h"
#include "core/Runner.h"
#include "core/Winetricks.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDialog>
#include <QFontDatabase>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QProcess>
#include <QPushButton>
#include <QSet>
#include <QTextCursor>
#include <QTreeWidget>
#include <QVBoxLayout>

// Winetricks for a single prefix: a list of verbs with search and categories, installation of the selected ones
// with output in the window, and cancel. Cancel also stops the child Wine processes.
class WinetricksDialog : public QDialog {
    Q_OBJECT
public:
    WinetricksDialog(PrefixManager* mgr, const Prefix& prefix, QWidget* parent = nullptr)
        : QDialog(parent), m_mgr(mgr), m_prefix(prefix)
    {
        setWindowTitle(tr("Winetricks: %1").arg(prefix.name));
        resize(880, 660);
        auto* v = new QVBoxLayout(this);

        m_status = new QLabel;
        m_status->setWordWrap(true);
        v->addWidget(m_status);

        auto* filters = new QHBoxLayout;
        m_search = new QLineEdit;
        m_search->setClearButtonEnabled(true);
        m_search->setPlaceholderText(tr("Search…"));
        m_category = new QComboBox;
        m_category->addItem(tr("All categories"), QString());
        filters->addWidget(m_search, 1);
        filters->addWidget(m_category);
        v->addLayout(filters);

        m_tree = new QTreeWidget;
        m_tree->setColumnCount(3);
        m_tree->setHeaderLabels({tr("Name"), tr("Description"), tr("Status")});
        m_tree->setRootIsDecorated(false);
        m_tree->setUniformRowHeights(true);
        m_tree->setColumnWidth(0, 210);
        m_tree->header()->setSectionResizeMode(1, QHeaderView::Stretch);
        v->addWidget(m_tree, 3);

        m_output = new QPlainTextEdit;
        m_output->setReadOnly(true);
        m_output->setMaximumBlockCount(5000);
        m_output->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
        v->addWidget(m_output, 2);

        auto* bottom = new QHBoxLayout;
        m_unattended = new QCheckBox(tr("Unattended (-q)"));
        m_unattended->setChecked(true);
        m_unattended->setToolTip(tr("Install without asking questions (winetricks -q)."));
        m_install = new QPushButton(tr("Install selected"));
        m_cancel = new QPushButton(tr("Cancel installation"));
        m_close = new QPushButton(tr("Close"));
        bottom->addWidget(m_unattended);
        bottom->addStretch(1);
        bottom->addWidget(m_install);
        bottom->addWidget(m_cancel);
        bottom->addWidget(m_close);
        v->addLayout(bottom);

        connect(m_search, &QLineEdit::textChanged, this, [this] { applyFilter(); });
        connect(m_category, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this] { applyFilter(); });
        connect(m_tree, &QTreeWidget::itemChanged, this, [this] { updateButtons(); });
        connect(m_install, &QPushButton::clicked, this, [this] { install(); });
        connect(m_cancel, &QPushButton::clicked, this, [this] { cancel(); });
        connect(m_close, &QPushButton::clicked, this, &QDialog::reject);

        const auto runner = RunnerManager::find(prefix.runner);
        if (!runner) {
            fatal(tr("The Wine of this prefix ('%1') was not found.").arg(prefix.runner));
            return;
        }
        m_runner = *runner;
        if (!Winetricks::command(&m_program, &m_baseArgs)) {
            fatal(tr("winetricks was not found. Put the winetricks script into %1 or install the "
                     "winetricks package.")
                      .arg(Winetricks::localDir()));
            return;
        }
        m_status->setText(tr("Loading the list of verbs…"));
        start(Listing, {QStringLiteral("list-all")});
    }

    void reject() override
    {
        if (m_mode == Installing)
            return;  // first "Cancel installation"
        if (m_proc) {
            m_proc->kill();
            m_proc->waitForFinished(1000);
        }
        QDialog::reject();
    }

private:
    enum Mode { Idle, Listing, ListingInstalled, Installing };

    void fatal(const QString& text)
    {
        m_status->setText(text);
        m_search->setEnabled(false);
        m_category->setEnabled(false);
        m_tree->setEnabled(false);
        m_unattended->setEnabled(false);
        m_install->setEnabled(false);
        m_cancel->setEnabled(false);
    }

    void start(Mode mode, const QStringList& args)
    {
        m_mode = mode;
        m_buffer.clear();
        m_proc = new QProcess(this);
        m_proc->setProcessEnvironment(Winetricks::makeEnv(m_prefix, m_runner));
        // Lists are read from stdout; warnings go to Cask's own stderr.
        // Installation: all output goes to the window.
        m_proc->setProcessChannelMode(mode == Installing ? QProcess::MergedChannels
                                                         : QProcess::ForwardedErrorChannel);
        m_proc->setProgram(m_program);
        m_proc->setArguments(m_baseArgs + args);
        connect(m_proc, &QProcess::readyRead, this, [this] { onOutput(); });
        connect(m_proc, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished), this,
                [this](int code, QProcess::ExitStatus st) { onFinished(code, st); });
        connect(m_proc, &QProcess::errorOccurred, this, [this](QProcess::ProcessError e) {
            if (e == QProcess::FailedToStart)
                onFinished(-1, QProcess::CrashExit);
        });
        m_proc->start();
        if (m_proc)  // a failed start has already finished (and cleared) the process
            m_proc->closeWriteChannel();  // no questions should appear in the terminal
        updateButtons();
    }

    void onOutput()
    {
        if (!m_proc)
            return;
        if (m_mode == Installing) {
            appendOutput(QString::fromLocal8Bit(m_proc->readAll()));
        } else {
            m_buffer += m_proc->readAllStandardOutput();
        }
    }

    void appendOutput(const QString& text)
    {
        m_output->moveCursor(QTextCursor::End);
        m_output->insertPlainText(text);
        m_output->moveCursor(QTextCursor::End);
    }

    void onFinished(int code, QProcess::ExitStatus st)
    {
        QProcess* proc = m_proc;
        if (!proc)
            return;
        if (m_mode == Installing)
            appendOutput(QString::fromLocal8Bit(proc->readAll()));
        else
            m_buffer += proc->readAllStandardOutput();
        m_proc = nullptr;
        proc->deleteLater();

        const Mode mode = m_mode;
        m_mode = Idle;
        const bool ok = st == QProcess::NormalExit && code == 0;

        if (mode == Listing) {
            const QList<Winetricks::Verb> verbs = Winetricks::parseList(m_buffer);
            if (!ok || verbs.isEmpty()) {
                fatal(tr("Could not load the list of verbs (winetricks exited with code %1).").arg(code));
                return;
            }
            fillVerbs(verbs);
            m_status->setText(tr("Loading installed verbs…"));
            start(ListingInstalled, {QStringLiteral("list-installed")});
            return;
        }
        if (mode == ListingInstalled) {
            m_installed = Winetricks::parseInstalled(m_buffer, m_known);
            updateStatuses();
            m_status->setText(tr("Select verbs and press “Install selected”."));
            updateButtons();
            return;
        }
        // Installing
        appendOutput(QStringLiteral("\n"));
        m_status->setText(m_cancelled ? tr("Installation cancelled.")
                          : ok        ? tr("Installation finished.")
                                      : tr("Installation failed (code %1). See the output below.").arg(code));
        m_status->setText(m_status->text() + QStringLiteral(" ") + tr("Loading installed verbs…"));
        start(ListingInstalled, {QStringLiteral("list-installed")});
    }

    void fillVerbs(const QList<Winetricks::Verb>& verbs)
    {
        m_tree->clear();
        m_known.clear();
        QStringList categories;
        for (const Winetricks::Verb& v : verbs) {
            if (Winetricks::isManagedByCask(v.name))
                continue;
            auto* it = new QTreeWidgetItem(m_tree, {v.name, v.description, QString()});
            it->setData(0, Qt::UserRole, v.name);
            it->setData(0, Qt::UserRole + 1, v.category);
            it->setFlags(it->flags() | Qt::ItemIsUserCheckable);
            it->setCheckState(0, Qt::Unchecked);
            m_known.insert(v.name);
            if (!v.category.isEmpty() && !categories.contains(v.category))
                categories << v.category;
        }
        const QSignalBlocker blocker(m_category);
        m_category->clear();
        m_category->addItem(tr("All categories"), QString());
        for (const QString& c : std::as_const(categories))
            m_category->addItem(c, c);
        applyFilter();
    }

    // "Installed" is marked in the status column; such rows cannot be selected.
    void updateStatuses()
    {
        const QSignalBlocker blocker(m_tree);
        for (int i = 0; i < m_tree->topLevelItemCount(); ++i) {
            QTreeWidgetItem* it = m_tree->topLevelItem(i);
            const bool done = m_installed.contains(it->data(0, Qt::UserRole).toString());
            it->setText(2, done ? tr("Installed") : QString());
            if (done) {
                it->setCheckState(0, Qt::Unchecked);
                it->setFlags(it->flags() & ~Qt::ItemIsUserCheckable);
            } else {
                it->setFlags(it->flags() | Qt::ItemIsUserCheckable);
            }
        }
    }

    void applyFilter()
    {
        const QString text = m_search->text().trimmed().toLower();
        const QString cat = m_category->currentData().toString();
        m_tree->setUpdatesEnabled(false);  // ~1500 rows: avoid relayout after every hidden row
        for (int i = 0; i < m_tree->topLevelItemCount(); ++i) {
            QTreeWidgetItem* it = m_tree->topLevelItem(i);
            const bool catOk = cat.isEmpty() || it->data(0, Qt::UserRole + 1).toString() == cat;
            const bool textOk = text.isEmpty() || it->text(0).toLower().contains(text) ||
                                it->text(1).toLower().contains(text);
            it->setHidden(!(catOk && textOk));
        }
        m_tree->setUpdatesEnabled(true);
    }

    QStringList checkedVerbs() const
    {
        QStringList out;
        for (int i = 0; i < m_tree->topLevelItemCount(); ++i) {
            const QTreeWidgetItem* it = m_tree->topLevelItem(i);
            if (it->checkState(0) == Qt::Checked)
                out << it->data(0, Qt::UserRole).toString();
        }
        return out;
    }

    void updateButtons()
    {
        const bool idle = m_mode == Idle;
        m_search->setEnabled(idle);
        m_category->setEnabled(idle);
        m_tree->setEnabled(idle);
        m_unattended->setEnabled(idle);
        m_install->setEnabled(idle && !checkedVerbs().isEmpty());
        m_cancel->setEnabled(m_mode == Installing);
        m_close->setEnabled(m_mode != Installing);
    }

    void install()
    {
        const QStringList verbs = checkedVerbs();
        if (verbs.isEmpty() || m_mode != Idle)
            return;
        QStringList args;
        if (m_unattended->isChecked())
            args << QStringLiteral("-q");
        args += verbs;
        m_cancelled = false;
        m_status->setText(tr("Installing: %1").arg(verbs.join(QStringLiteral(", "))));
        appendOutput(QStringLiteral("$ winetricks %1\n").arg(args.join(QLatin1Char(' '))));
        start(Installing, args);
    }

    void cancel()
    {
        if (m_mode != Installing || !m_proc)
            return;
        m_cancelled = true;
        appendOutput(tr("Cancelling…") + QLatin1Char('\n'));
        m_proc->terminate();
        // Also stops whatever winetricks has started (installers, wineserver).
        QString err;
        m_mgr->stopAll(m_prefix.name, &err);
    }

    PrefixManager* m_mgr;
    Prefix m_prefix;
    Runner m_runner;
    QString m_program;
    QStringList m_baseArgs;

    QLabel* m_status = nullptr;
    QLineEdit* m_search = nullptr;
    QComboBox* m_category = nullptr;
    QTreeWidget* m_tree = nullptr;
    QPlainTextEdit* m_output = nullptr;
    QCheckBox* m_unattended = nullptr;
    QPushButton* m_install = nullptr;
    QPushButton* m_cancel = nullptr;
    QPushButton* m_close = nullptr;

    QProcess* m_proc = nullptr;
    Mode m_mode = Idle;
    QByteArray m_buffer;
    QSet<QString> m_known;
    QSet<QString> m_installed;
    bool m_cancelled = false;
};
