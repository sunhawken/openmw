#ifndef LAUNCHER_DARKMESSIAHPAGE_H
#define LAUNCHER_DARKMESSIAHPAGE_H

#include <QProcess>
#include <QString>
#include <QWidget>

#include <filesystem>

class QLabel;
class QLineEdit;
class QPlainTextEdit;
class QProgressBar;
class QPushButton;

namespace Files
{
    struct ConfigurationManager;
}

namespace Config
{
    class GameSettings;
}

namespace Launcher
{
    // Finds Dark Messiah of Might and Magic and runs brutalmorrowind-import, which
    // builds the animations, sounds and textures the game uses from the user's own
    // Dark Messiah and Morrowind files.
    class DarkMessiahPage : public QWidget
    {
        Q_OBJECT

    public:
        DarkMessiahPage(const Files::ConfigurationManager& cfg, Config::GameSettings& gameSettings,
            QWidget* parent = nullptr);
        ~DarkMessiahPage() override;

        void loadSettings();
        void saveSettings();

        // The game uses the imported data only when it was made for this build.
        bool dataReady() const;
        // An import made by a previous version is present; this version needs a new one.
        bool olderDataPresent() const;
        bool gameFolderValid() const;
        bool importRunning() const;
        bool skipMissingWarning() const { return mSkipMissingWarning; }
        void setSkipMissingWarning(bool skip) { mSkipMissingWarning = skip; }

        // Starts the importer. With playAfter set, importFinished asks to start the game.
        void startImport(bool playAfter);
        void cancelImport();

        // Steam folders that hold Dark Messiah (app 2100), most likely first.
        static QStringList detectGameFolders();
        static bool isGameFolder(const QString& folder);

    signals:
        void importFinished(bool success, bool playAfter);

    private slots:
        void browse();
        void detect();
        void readOutput();
        void processFinished(int exitCode, QProcess::ExitStatus status);
        void processError(QProcess::ProcessError error);

    private:
        std::filesystem::path userDataPath() const;
        std::filesystem::path settingsFile() const;
        QString morrowindDataFolder() const;
        QString importerProgram() const;
        void updateStatus();
        void setRunning(bool running);
        void fail(const QString& message);
        // Delete the imports of previous versions after a successful import.
        void removeOlderData();

        const Files::ConfigurationManager& mCfgMgr;
        Config::GameSettings& mGameSettings;
        QLineEdit* mPathEdit;
        QLabel* mFolderStatus;
        QLabel* mDataStatus;
        QProgressBar* mProgress;
        QLabel* mStepLabel;
        QPushButton* mBrowseButton;
        QPushButton* mDetectButton;
        QPushButton* mImportButton;
        QPushButton* mCancelButton;
        QPlainTextEdit* mLog;
        QProcess* mProcess;
        QString mPendingLine;
        // Counts started imports, so a kill timer of a cancelled import spares the next one.
        unsigned mImportRun = 0;
        bool mPlayAfter = false;
        bool mCancelling = false;
        bool mSkipMissingWarning = false;
    };
}

#endif
