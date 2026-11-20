#pragma once

#include "core/PrefixManager.h"

#include <QList>
#include <QMainWindow>
#include <QMap>

class QCheckBox;
class QCloseEvent;
class QComboBox;
class QLabel;
class QListWidget;
class QProgressBar;
class QPushButton;
class QSplitter;
class QToolBar;
class QWidget;

class ElidedLabel;
class ProgramsPanel;

class MainWindow : public QMainWindow {
    Q_OBJECT
public:
    explicit MainWindow(QWidget* parent = nullptr);

protected:
    void closeEvent(QCloseEvent* event) override;
    void showEvent(QShowEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;
    void changeEvent(QEvent* event) override;

private slots:
    void refresh();
    void onSelected();
    void newPrefix();
    void deletePrefix();
    void openSettings();
    void showAbout();
    void runExe();
    void openFolder();
    void applyRunner();
    void stopAll();
    void openWinetricks();
    void saveSettings();
    void editEnv();

private:
    // Component row: a "None / versions" list. Selecting a version applies it,
    // selecting "Disabled" removes the component.
    struct CompRow {
        QString type;
        QComboBox* versions = nullptr;
        ElidedLabel* hint = nullptr;
    };

    void buildUi();
    void fitToContent();
    void retranslate();  // rebuilds the UI after a language change
    void runTool(const QStringList& wineArgs);
    void componentChosen(const QString& type);
    void saveOrder();
    void selectByName(const QString& name);
    void updateEnvButton();
    QString localeValue() const;
    void setLocaleValue(const QString& value);
    QString currentName() const;

    PrefixManager m_mgr;
    QToolBar* m_toolbar = nullptr;
    QSplitter* m_central = nullptr;
    QListWidget* m_list = nullptr;
    QWidget* m_details = nullptr;
    ProgramsPanel* m_programs = nullptr;
    QLabel* m_name = nullptr;
    ElidedLabel* m_path = nullptr;
    QLabel* m_arch = nullptr;
    QComboBox* m_runnerBox = nullptr;
    QList<CompRow> m_rows;

    // Launch settings
    QComboBox* m_localeBox = nullptr;
    QCheckBox* m_gamemode = nullptr;
    QCheckBox* m_mangohud = nullptr;
    QCheckBox* m_discreteGpu = nullptr;
    QPushButton* m_envBtn = nullptr;
    QMap<QString, QString> m_env;
    bool m_loading = false;  // true while widgets are being filled from the prefix (do not save)

    QProgressBar* m_busy = nullptr;
    int m_corrections = 0;  // consecutive attempts to undo a too small size
    bool m_sized = false;  // the initial size has been set
};
