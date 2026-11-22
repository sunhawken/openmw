#include "darkmessiahpage.hpp"

#include <QCheckBox>
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QProgressBar>
#include <QPushButton>
#include <QRegularExpression>
#include <QSettings>
#include <QTimer>
#include <QVBoxLayout>

#include <components/config/gamesettings.hpp>
#include <components/files/configurationmanager.hpp>
#include <components/files/qtconversion.hpp>
#include <components/misc/darkmessiahdata.hpp>

namespace
{
    constexpr int sSteamAppId = 2100;
    constexpr int sCancelledExitCode = 3;

    QString readTextFile(const QString& path)
    {
        QFile file(path);
        if (!file.open(QIODevice::ReadOnly))
            return {};
        return QString::fromUtf8(file.readAll());
    }

    // Steam's own installation folders on this system.
    QStringList steamRoots()
    {
        QStringList roots;
#ifdef Q_OS_WIN
        for (const QString& key : { QStringLiteral("HKEY_CURRENT_USER\\Software\\Valve\\Steam"),
                 QStringLiteral("HKEY_LOCAL_MACHINE\\SOFTWARE\\WOW6432Node\\Valve\\Steam"),
                 QStringLiteral("HKEY_LOCAL_MACHINE\\SOFTWARE\\Valve\\Steam") })
        {
            QSettings registry(key, QSettings::NativeFormat);
            for (const QString& name : { QStringLiteral("SteamPath"), QStringLiteral("InstallPath") })
            {
                const QString value = registry.value(name).toString();
                if (!value.isEmpty())
                    roots << QDir::fromNativeSeparators(value);
            }
        }
        roots << QStringLiteral("C:/Program Files (x86)/Steam") << QStringLiteral("C:/Program Files/Steam");
#else
        const QString home = QDir::homePath();
        roots << home + "/.steam/steam" << home + "/.steam/root" << home + "/.local/share/Steam"
              << home + "/.var/app/com.valvesoftware.Steam/.local/share/Steam"
              << home + "/snap/steam/common/.local/share/Steam";
#endif
        return roots;
    }

    // Library folders listed in steamapps/libraryfolders.vdf, the root itself first.
    QStringList steamLibraries(const QString& root)
    {
        QStringList libraries{ root };
        const QString vdf = readTextFile(root + "/steamapps/libraryfolders.vdf");
        static const QRegularExpression pathRe(QStringLiteral("\"path\"\\s+\"((?:[^\"\\\\]|\\\\.)*)\""));
        for (auto it = pathRe.globalMatch(vdf); it.hasNext();)
        {
            QString path = it.next().captured(1);
            path.replace(QStringLiteral("\\\\"), QStringLiteral("\\"));
            libraries << QDir::fromNativeSeparators(path);
        }
        return libraries;
    }
}

Launcher::DarkMessiahPage::DarkMessiahPage(
    const Files::ConfigurationManager& cfg, Config::GameSettings& gameSettings, QWidget* parent)
    : QWidget(parent)
    , mCfgMgr(cfg)
    , mGameSettings(gameSettings)
    , mProcess(new QProcess(this))
{
    auto* layout = new QVBoxLayout(this);

    auto* gameBox = new QGroupBox(tr("Dark Messiah of Might and Magic"), this);
    auto* gameLayout = new QVBoxLayout(gameBox);
    auto* intro = new QLabel(tr("Brutal Morrowind takes its combat animations, sounds and blood effects from your "
                                "copy of Dark Messiah of Might and Magic. Nothing is copied from the game folder "
                                "itself: the importer builds them once in your user data folder."),
        gameBox);
    intro->setWordWrap(true);
    gameLayout->addWidget(intro);
    auto* pathRow = new QHBoxLayout();
    mPathEdit = new QLineEdit(gameBox);
    mPathEdit->setPlaceholderText(tr("Dark Messiah folder (VPK, GCF or extracted mm resources)"));
    mBrowseButton = new QPushButton(tr("Browse..."), gameBox);
    mDetectButton = new QPushButton(tr("Detect"), gameBox);
    pathRow->addWidget(mPathEdit, 1);
    pathRow->addWidget(mBrowseButton);
    pathRow->addWidget(mDetectButton);
    gameLayout->addLayout(pathRow);
    mFolderStatus = new QLabel(gameBox);
    mFolderStatus->setWordWrap(true);
    gameLayout->addWidget(mFolderStatus);
    layout->addWidget(gameBox);

    auto* dataBox = new QGroupBox(tr("Imported data"), this);
    auto* dataLayout = new QVBoxLayout(dataBox);
    mDataStatus = new QLabel(dataBox);
    mDataStatus->setWordWrap(true);
    dataLayout->addWidget(mDataStatus);
    mProgress = new QProgressBar(dataBox);
    mProgress->setRange(0, 1);
    mProgress->setValue(0);
    dataLayout->addWidget(mProgress);
    mStepLabel = new QLabel(dataBox);
    dataLayout->addWidget(mStepLabel);
    auto* buttons = new QHBoxLayout();
    mImportButton = new QPushButton(tr("Import"), dataBox);
    mCancelButton = new QPushButton(tr("Cancel"), dataBox);
    buttons->addWidget(mImportButton);
    buttons->addWidget(mCancelButton);
    buttons->addStretch(1);
    dataLayout->addLayout(buttons);
    mLog = new QPlainTextEdit(dataBox);
    mLog->setReadOnly(true);
    mLog->setMaximumBlockCount(500);
    mLog->setVisible(false);
    dataLayout->addWidget(mLog, 1);
    layout->addWidget(dataBox, 1);

    auto* note = new QLabel(tr("Without Dark Messiah the game uses Morrowind's animations, and the components that "
                               "need Dark Messiah animations, sounds or textures are switched off."),
        this);
    note->setWordWrap(true);
    layout->addWidget(note);

    connect(mBrowseButton, &QPushButton::clicked, this, &DarkMessiahPage::browse);
    connect(mDetectButton, &QPushButton::clicked, this, &DarkMessiahPage::detect);
    connect(mPathEdit, &QLineEdit::textChanged, this, &DarkMessiahPage::updateStatus);
    connect(mImportButton, &QPushButton::clicked, this, [this] { startImport(false); });
    connect(mCancelButton, &QPushButton::clicked, this, &DarkMessiahPage::cancelImport);
    mProcess->setProcessChannelMode(QProcess::MergedChannels);
    connect(mProcess, &QProcess::readyReadStandardOutput, this, &DarkMessiahPage::readOutput);
    connect(mProcess, qOverload<int, QProcess::ExitStatus>(&QProcess::finished), this,
        &DarkMessiahPage::processFinished);
    connect(mProcess, &QProcess::errorOccurred, this, &DarkMessiahPage::processError);
    setRunning(false);
}

Launcher::DarkMessiahPage::~DarkMessiahPage()
{
    if (importRunning())
    {
        mCancelling = true;
        mProcess->closeWriteChannel();
        if (!mProcess->waitForFinished(10000))
            mProcess->kill();
        mProcess->waitForFinished(3000);
    }
}

std::filesystem::path Launcher::DarkMessiahPage::userDataPath() const
{
    const QString configured = mGameSettings.value(QStringLiteral("user-data")).value;
    return configured.isEmpty() ? mCfgMgr.getUserDataPath() : Files::pathFromQString(configured);
}

std::filesystem::path Launcher::DarkMessiahPage::settingsFile() const
{
    return mCfgMgr.getUserConfigPath() / "dark-messiah.cfg";
}

void Launcher::DarkMessiahPage::loadSettings()
{
    QSettings settings(Files::pathToQString(settingsFile()), QSettings::IniFormat);
    QString folder = settings.value(QStringLiteral("folder")).toString();
    mSkipMissingWarning = settings.value(QStringLiteral("play-without-asking")).toBool();
    if (folder.isEmpty() || !isGameFolder(folder))
    {
        const QStringList found = detectGameFolders();
        if (!found.isEmpty())
            folder = found.front();
    }
    mPathEdit->setText(folder);
    updateStatus();
}

void Launcher::DarkMessiahPage::saveSettings()
{
    QSettings settings(Files::pathToQString(settingsFile()), QSettings::IniFormat);
    settings.setValue(QStringLiteral("folder"), mPathEdit->text().trimmed());
    settings.setValue(QStringLiteral("play-without-asking"), mSkipMissingWarning);
}

bool Launcher::DarkMessiahPage::isGameFolder(const QString& folder)
{
    if (folder.isEmpty())
        return false;
    for (const QString& candidate : { folder, folder + "/vpks", folder + "/mm", folder + "/mm/vpks" })
        if (!QDir(candidate).entryList({ QStringLiteral("*_dir.vpk"), QStringLiteral("*.gcf") }, QDir::Files).isEmpty()
            || QFileInfo::exists(candidate + "/models/npc/player/first_basic.mdl"))
            return true;
    return false;
}

QStringList Launcher::DarkMessiahPage::detectGameFolders()
{
    QStringList found;
    QStringList seenLibraries;
    for (const QString& root : steamRoots())
    {
        if (!QFileInfo(root).isDir())
            continue;
        for (const QString& library : steamLibraries(root))
        {
            const QString canonical = QFileInfo(library).canonicalFilePath();
            if (canonical.isEmpty() || seenLibraries.contains(canonical))
                continue;
            seenLibraries << canonical;
            QStringList folders;
            const QString manifest
                = readTextFile(canonical + QStringLiteral("/steamapps/appmanifest_%1.acf").arg(sSteamAppId));
            static const QRegularExpression installRe(QStringLiteral("\"installdir\"\\s+\"([^\"]+)\""));
            const QRegularExpressionMatch match = installRe.match(manifest);
            if (match.hasMatch())
                folders << canonical + "/steamapps/common/" + match.captured(1);
            folders << canonical + "/steamapps/common/Dark Messiah Might and Magic Single Player";
            for (const QString& folder : folders)
                if (isGameFolder(folder) && !found.contains(QDir::cleanPath(folder)))
                    found << QDir::cleanPath(folder);
        }
    }
    return found;
}

bool Launcher::DarkMessiahPage::gameFolderValid() const
{
    return isGameFolder(mPathEdit->text().trimmed());
}

bool Launcher::DarkMessiahPage::dataReady() const
{
    return Misc::DarkMessiahData::isReady(userDataPath());
}

bool Launcher::DarkMessiahPage::olderDataPresent() const
{
    for (const std::filesystem::path& folder : Misc::DarkMessiahData::olderImports(userDataPath()))
        if (Misc::DarkMessiahData::markerRecipe(folder) > 0)
            return true;
    return false;
}

void Launcher::DarkMessiahPage::removeOlderData()
{
    for (const std::filesystem::path& folder : Misc::DarkMessiahData::olderImports(userDataPath()))
    {
        std::error_code error;
        std::filesystem::remove_all(folder, error);
        if (error)
            mLog->appendPlainText(tr("Could not remove the old import %1: %2")
                                      .arg(Files::pathToQString(folder), QString::fromStdString(error.message())));
        else
            mLog->appendPlainText(tr("Removed the old import %1").arg(Files::pathToQString(folder)));
    }
}

bool Launcher::DarkMessiahPage::importRunning() const
{
    return mProcess->state() != QProcess::NotRunning;
}

QString Launcher::DarkMessiahPage::morrowindDataFolder() const
{
    // The importer reads the original Morrowind.bsa directly, never through mods.
    const auto dirs = mGameSettings.getDataDirs();
    for (auto it = dirs.rbegin(); it != dirs.rend(); ++it)
    {
        QDir dir(it->value);
        for (const QString& entry : dir.entryList(QDir::Files))
            if (entry.compare(QLatin1String("Morrowind.bsa"), Qt::CaseInsensitive) == 0)
                return dir.absolutePath();
    }
    return {};
}

QString Launcher::DarkMessiahPage::importerProgram() const
{
#ifdef Q_OS_WIN
    return QCoreApplication::applicationDirPath() + "/brutalmorrowind-import.exe";
#else
    return QCoreApplication::applicationDirPath() + "/brutalmorrowind-import";
#endif
}

void Launcher::DarkMessiahPage::browse()
{
    const QString folder = QFileDialog::getExistingDirectory(
        this, tr("Select the Dark Messiah of Might and Magic folder"), mPathEdit->text());
    if (!folder.isEmpty())
        mPathEdit->setText(QDir::cleanPath(folder));
}

void Launcher::DarkMessiahPage::detect()
{
    const QStringList found = detectGameFolders();
    if (found.isEmpty())
    {
        QMessageBox::information(this, tr("Dark Messiah not found"),
            tr("Dark Messiah of Might and Magic was not found in your Steam libraries. "
               "Select its folder with Browse."));
        return;
    }
    mPathEdit->setText(found.front());
}

void Launcher::DarkMessiahPage::updateStatus()
{
    const QString folder = mPathEdit->text().trimmed();
    if (folder.isEmpty())
        mFolderStatus->setText(tr("Not found. Select the game folder or press Detect."));
    else if (isGameFolder(folder))
        mFolderStatus->setText(tr("Game files found."));
    else
        mFolderStatus->setText(tr("This folder has no Dark Messiah resources (VPK, GCF or extracted mm folder)."));

    const std::filesystem::path root = Misc::DarkMessiahData::root(userDataPath());
    if (dataReady())
        mDataStatus->setText(tr("Imported and ready: %1").arg(Files::pathToQString(root)));
    else
        mDataStatus->setText(olderDataPresent()
                ? tr("Imported for an older version of Brutal Morrowind. Import again.")
                : tr("Not imported yet. The import takes about a minute."));
    if (!importRunning())
    {
        mImportButton->setText(dataReady() ? tr("Import again") : tr("Import"));
        mImportButton->setEnabled(isGameFolder(folder));
    }
}

void Launcher::DarkMessiahPage::setRunning(bool running)
{
    mImportButton->setEnabled(!running && gameFolderValid());
    mCancelButton->setEnabled(running);
    mPathEdit->setEnabled(!running);
    mBrowseButton->setEnabled(!running);
    mDetectButton->setEnabled(!running);
}

void Launcher::DarkMessiahPage::startImport(bool playAfter)
{
    if (importRunning())
        return;
    const QString folder = mPathEdit->text().trimmed();
    if (!isGameFolder(folder))
        return fail(tr("Select the Dark Messiah of Might and Magic folder first."));
    const QString morrowind = morrowindDataFolder();
    if (morrowind.isEmpty())
        return fail(tr("Morrowind.bsa was not found in your data folders. Set up Morrowind first."));
    const QString program = importerProgram();
    if (!QFileInfo(program).isExecutable())
        return fail(tr("The importer is missing: %1").arg(program));

    saveSettings();
    mPlayAfter = playAfter;
    mCancelling = false;
    ++mImportRun;
    mPendingLine.clear();
    mLog->clear();
    mLog->setVisible(true);
    mProgress->setRange(0, 0);
    mStepLabel->setText(tr("Starting..."));
    setRunning(true);
    const QStringList arguments{ QStringLiteral("--dark-messiah"), folder, QStringLiteral("--morrowind"), morrowind,
        QStringLiteral("--output"), Files::pathToQString(Misc::DarkMessiahData::root(userDataPath())),
        QStringLiteral("--progress"), QStringLiteral("--cancel-on-stdin-close") };
    mLog->appendPlainText(QStringLiteral("%1 %2").arg(program, arguments.join(' ')));
    mProcess->start(program, arguments);
}

void Launcher::DarkMessiahPage::cancelImport()
{
    if (!importRunning())
        return;
    mCancelling = true;
    mCancelButton->setEnabled(false);
    mStepLabel->setText(tr("Cancelling..."));
    mLog->appendPlainText(tr("Cancel requested by the user."));
    // The importer stops its steps when its input closes; kill it if it does not.
    mProcess->closeWriteChannel();
    QTimer::singleShot(15000, this, [this, run = mImportRun] {
        if (run == mImportRun && importRunning())
            mProcess->kill();
    });
}

void Launcher::DarkMessiahPage::readOutput()
{
    mPendingLine += QString::fromLocal8Bit(mProcess->readAllStandardOutput());
    qsizetype end;
    while ((end = mPendingLine.indexOf('\n')) >= 0)
    {
        const QString line = mPendingLine.left(end).trimmed();
        mPendingLine.remove(0, end + 1);
        static const QRegularExpression progressRe(QStringLiteral("^PROGRESS (\\d+) (\\d+) (.*)$"));
        const QRegularExpressionMatch match = progressRe.match(line);
        if (match.hasMatch())
        {
            mProgress->setRange(0, match.captured(2).toInt());
            mProgress->setValue(match.captured(1).toInt());
            mStepLabel->setText(match.captured(3));
        }
        else if (!line.isEmpty())
            mLog->appendPlainText(line);
    }
}

void Launcher::DarkMessiahPage::processFinished(int exitCode, QProcess::ExitStatus status)
{
    readOutput();
    setRunning(false);
    const bool success = status == QProcess::NormalExit && exitCode == 0 && dataReady();
    mProgress->setRange(0, 1);
    mProgress->setValue(success ? 1 : 0);
    if (success)
    {
        mStepLabel->setText(tr("Import finished."));
        removeOlderData();
    }
    else if (mCancelling || exitCode == sCancelledExitCode)
        mStepLabel->setText(tr("Import cancelled."));
    else
    {
        mStepLabel->setText(tr("Import failed."));
        QMessageBox::warning(this, tr("Import failed"),
            tr("The Dark Messiah import failed. The log on the Dark Messiah page shows the reason."));
    }
    updateStatus();
    emit importFinished(success, mPlayAfter && success);
    mPlayAfter = false;
}

void Launcher::DarkMessiahPage::processError(QProcess::ProcessError error)
{
    if (error != QProcess::FailedToStart)
        return;
    setRunning(false);
    mProgress->setRange(0, 1);
    mProgress->setValue(0);
    fail(tr("The importer could not be started: %1").arg(mProcess->errorString()));
    emit importFinished(false, false);
    mPlayAfter = false;
}

void Launcher::DarkMessiahPage::fail(const QString& message)
{
    mStepLabel->setText(message);
    QMessageBox::warning(this, tr("Dark Messiah import"), message);
}
