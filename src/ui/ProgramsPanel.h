#pragma once

#include "core/PrefixManager.h"
#include "core/Program.h"

#include <QGroupBox>
#include <QList>
#include <QStringList>
#include <QTimer>

class IconButton;
class QLabel;
class QLineEdit;
class QListWidget;
class QListWidgetItem;
class QPushButton;
class QStackedWidget;

// The "Programs" panel of the main window: the shortcuts of the selected prefix. Programs installed in
// the prefix are added automatically; the user can add, rename, reorder (drag and drop), run, stop,
// run in a terminal, show the folder of and remove them.
class ProgramsPanel : public QGroupBox {
    Q_OBJECT
public:
    explicit ProgramsPanel(PrefixManager* mgr, QWidget* parent = nullptr);

    // Shows the programs of this prefix (empty name: nothing) and looks for newly installed ones.
    void setPrefix(const QString& name);
    // Looks for newly installed programs again (for example when the window is activated).
    void rescan();

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    struct Row {
        QWidget* widget = nullptr;
        QLabel* name = nullptr;
        QLineEdit* edit = nullptr;
        IconButton* run = nullptr;
    };

    void reload(bool force);
    void rebuild();
    void applyOrder();
    void save();
    void updateRunning();
    void clicked(int index);
    void startEdit(int index);
    void finishEdit(int index, bool commit);
    void runOrStop(int index);
    void runInTerminal(int index);
    void showFolder(int index);
    void remove(int index);
    void add();
    QString fileOf(int index) const;
    bool isRunning(int index) const;
    void warn(const QString& text);

    PrefixManager* m_mgr;
    QString m_prefix;
    QString m_path;  // folder of the prefix
    QList<Program> m_programs;
    QStringList m_hidden;
    QStringList m_running;  // image names of the running Wine processes
    QList<Row> m_rows;
    bool m_hadProcesses = false;
    int m_editing = -1;

    QListWidget* m_list = nullptr;
    QStackedWidget* m_stack = nullptr;
    QPushButton* m_add = nullptr;
    QTimer m_poll;
};
