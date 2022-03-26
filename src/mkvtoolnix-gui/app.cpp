#include "common/common_pch.h"

#include <QColor>
#include <QDebug>
#include <QFile>
#include <QLibraryInfo>
#include <QLocalServer>
#include <QLocalSocket>
#include <QPalette>
#include <QSettings>
#include <QStyleFactory>
#include <QTextStream>
#include <QThread>
#include <QTranslator>

#include "common/character_sets.h"
#include "common/command_line.h"
#include "common/fs_sys_helpers.h"
#include "common/iso639.h"
#include "common/iso3166.h"
#include "common/kax_element_names.h"
#include "common/kax_info.h"
#include "common/qt.h"
#include "common/qt6_compat/library_info.h"
#include "common/unique_numbers.h"
#include "common/version.h"
#include "mkvtoolnix-gui/app.h"
#include "mkvtoolnix-gui/jobs/program_runner.h"
#include "mkvtoolnix-gui/main_window/main_window.h"
#include "mkvtoolnix-gui/merge/tool.h"
#include "mkvtoolnix-gui/util/container.h"
#include "mkvtoolnix-gui/util/file_type_filter.h"
#include "mkvtoolnix-gui/util/media_player.h"
#include "mkvtoolnix-gui/util/network_access_manager.h"
#include "mkvtoolnix-gui/util/process.h"
#include "mkvtoolnix-gui/util/settings.h"

namespace mtx::gui {

namespace {

QString
formatRegionDescription(mtx::iso3166::region_t const &region) {
  QStringList parts;

  if (!region.alpha_2_code.empty())
    parts << Q(region.alpha_2_code).toLower();

  if (region.number != 0)
    parts << Q(fmt::format("{0:03}", region.number));

  return Q("%1 (%2)").arg(Q(region.name)).arg(parts.join(Q("; ")));
}

} // anonymous namespace

static Iso639LanguageList s_iso639Languages, s_iso639_2Languages, s_commonIso639Languages;
static RegionList s_regions, s_commonRegions;
static CharacterSetList s_characterSets, s_commonCharacterSets;
static QHash<QString, QString> s_iso639_2LanguageCodeToDescription, s_regionToDescription;

static std::optional<bool> s_is_installed;

class AppPrivate {
  friend class App;

  std::unique_ptr<QTranslator> m_currentTranslator;
  std::unique_ptr<GuiCliParser> m_cliParser;
  std::unique_ptr<QLocalServer> m_instanceCommunicator;
  std::unique_ptr<Jobs::ProgramRunner> m_programRunner;
  std::unique_ptr<Util::MediaPlayer> m_mediaPlayer;
  Util::NetworkAccessManager *m_networkAccessManager{new Util::NetworkAccessManager{}};
  QThread m_networkAccessManagerThread;
  bool m_otherInstanceRunning{};
  QString m_defaultStyleSheet;
  QPalette m_defaultPalette;

  explicit AppPrivate()
  {
  }
};

App::App(int &argc,
         char **argv)
  : QApplication{argc, argv}
  , p_ptr{new AppPrivate{}}
{
  // The routines for handling unique numbers cannot cope with
  // multiple chapters being worked on at the same time as they safe
  // already-used numbers in one static container. So just disable them.
  ignore_unique_numbers(UNIQUE_CHAPTER_IDS);
  ignore_unique_numbers(UNIQUE_EDITION_IDS);

#ifdef SYS_WINDOWS
  QApplication::setStyle(Q("windowsvista"));
#endif

  Util::Settings::migrateFromRegistry();
  Util::Settings::get().load();

  setupInstanceCommunicator();
  setupNetworkAccessManager();

  QObject::connect(this, &App::aboutToQuit, this, &App::saveSettings);

  initializeLocale();

  p_ptr->m_defaultStyleSheet = styleSheet();
  p_ptr->m_defaultPalette    = palette();
}

App::~App() {
  auto p = p_func();

  p->m_networkAccessManagerThread.quit();
  p->m_networkAccessManagerThread.wait();
}

Util::NetworkAccessManager &
App::networkAccessManager() {
  return *p_func()->m_networkAccessManager;
}

void
App::setupNetworkAccessManager() {
  auto p = p_func();

  p->m_networkAccessManager->moveToThread(&p->m_networkAccessManagerThread);

  connect(&p->m_networkAccessManagerThread, &QThread::finished, p->m_networkAccessManager, &QObject::deleteLater);

  p->m_networkAccessManagerThread.start();
}

void
App::setupUiFont() {
  auto &cfg    = Util::Settings::get();
  auto newFont = App::font();

  newFont.setFamily(cfg.m_uiFontFamily);
  newFont.setPointSize(std::max(cfg.m_uiFontPointSize, 5));

  App::setFont(newFont);
}

QString
App::communicatorSocketName() {
  return Q("MKVToolNix-GUI-Instance-Communicator-%1").arg(Util::currentUserName());
}

void
App::setupInstanceCommunicator() {
  auto p          = p_func();
  auto socketName = communicatorSocketName();
  auto socket     = std::make_unique<QLocalSocket>(this);

  socket->connectToServer(socketName);

  if (socket->state() == QLocalSocket::ConnectedState) {
    socket->disconnect();

    p->m_otherInstanceRunning = true;

    return;
  }

  QLocalServer::removeServer(socketName);

  p->m_instanceCommunicator = std::make_unique<QLocalServer>(this);

  if (p->m_instanceCommunicator->listen(socketName))
    connect(p->m_instanceCommunicator.get(), &QLocalServer::newConnection, this, &App::receiveInstanceCommunication);

  else
    p->m_instanceCommunicator.reset(nullptr);
}

void
App::saveSettings()
  const {
  Util::Settings::get().save();
}

App *
App::instance() {
  return static_cast<App *>(QApplication::instance());
}

Jobs::ProgramRunner &
App::setupProgramRunner() {
  auto p = p_func();

  if (!p->m_programRunner)
    p->m_programRunner = Jobs::ProgramRunner::create();
  return *p->m_programRunner;
}

Jobs::ProgramRunner &
App::programRunner() {
  return instance()->setupProgramRunner();
}

Util::MediaPlayer &
App::setupMediaPlayer() {
  auto p = p_func();

  if (!p->m_mediaPlayer)
    p->m_mediaPlayer.reset(new Util::MediaPlayer);
  return *p->m_mediaPlayer;
}

Util::MediaPlayer &
App::mediaPlayer() {
  return instance()->setupMediaPlayer();
}

void
App::retranslateUi() {
  if (MainWindow::get())
    MainWindow::get()->retranslateUi();

  mtx::gui::Util::FileTypeFilter::reset();
}

void
App::reinitializeLanguageLists() {
  s_iso639Languages.clear();
  s_iso639_2Languages.clear();
  s_commonIso639Languages.clear();
  s_regions.clear();
  s_commonRegions.clear();
  s_iso639_2LanguageCodeToDescription.clear();
  s_regionToDescription.clear();
  s_characterSets.clear();
  s_commonCharacterSets.clear();

  initializeLanguageLists();

  kax_element_names_c::reset();
}

void
App::initializeLanguageLists() {
  if (!s_iso639Languages.empty())
    return;

  initializeIso639Languages();
  initializeRegions();
  initializeCharacterSets();
}

void
App::initializeIso639Languages() {
  auto &cfg = Util::Settings::get();

  s_iso639Languages.reserve(mtx::iso639::g_languages.size());
  s_iso639_2Languages.reserve(mtx::iso639::g_languages.size());
  s_commonIso639Languages.reserve(cfg.m_oftenUsedLanguages.size());

  for (auto const &language : mtx::iso639::g_languages) {
    auto languageCode  = Q(language.alpha_3_code);
    auto languageCodes = language.alpha_2_code.empty() ? languageCode : Q("%1; %2").arg(Q(language.alpha_2_code)).arg(languageCode);
    auto description   = Q("%1 (%2)").arg(Q(gettext(language.english_name.c_str()))).arg(languageCodes);
    auto isCommon      = cfg.m_oftenUsedLanguages.indexOf(languageCode) != -1;

    s_iso639Languages.emplace_back(description, languageCode);
    if (language.is_part_of_iso639_2)
      s_iso639_2Languages.emplace_back(description, languageCode);
    if (isCommon)
      s_commonIso639Languages.emplace_back(description, languageCode);

    s_iso639_2LanguageCodeToDescription[languageCode] = description;
  }

  std::sort(s_iso639Languages.begin(),       s_iso639Languages.end());
  std::sort(s_iso639_2Languages.begin(),     s_iso639_2Languages.end());
  std::sort(s_commonIso639Languages.begin(), s_commonIso639Languages.end());
}

void
App::initializeRegions() {
  auto &cfg = Util::Settings::get();

  s_regions.reserve(mtx::iso3166::g_regions.size());
  s_commonRegions.reserve(mtx::iso3166::g_regions.size());

  for (auto const &region : mtx::iso3166::g_regions) {
    auto countryCode = !region.alpha_2_code.empty() ? Q(region.alpha_2_code).toLower() : Q(fmt::format("{0:03}", region.number));
    auto description = formatRegionDescription(region);
    auto isCommon    = cfg.m_oftenUsedRegions.indexOf(countryCode) != -1;

    s_regions.emplace_back(description, countryCode);
    if (isCommon)
      s_commonRegions.emplace_back(description, countryCode);

    s_regionToDescription[countryCode] = description;
  }

  std::sort(s_regions.begin(),       s_regions.end());
  std::sort(s_commonRegions.begin(), s_commonRegions.end());
}

void
App::initializeCharacterSets() {
  auto &cfg = Util::Settings::get();

  s_characterSets.reserve(g_character_sets.size());
  s_commonCharacterSets.reserve(cfg.m_oftenUsedCharacterSets.size());

  for (auto const &characterSet : g_character_sets) {
    auto qCharacterSet = Q(characterSet);

    s_characterSets.emplace_back(qCharacterSet);
    if (cfg.m_oftenUsedCharacterSets.contains(qCharacterSet))
      s_commonCharacterSets.emplace_back(qCharacterSet);
  }

  std::sort(s_characterSets.begin(), s_characterSets.end());
  std::sort(s_commonCharacterSets.begin(), s_commonCharacterSets.end());
}

Iso639LanguageList const &
App::iso639Languages() {
  initializeLanguageLists();
  return s_iso639Languages;
}

Iso639LanguageList const &
App::iso639_2Languages() {
  initializeLanguageLists();
  return s_iso639_2Languages;
}

Iso639LanguageList const &
App::commonIso639Languages() {
  initializeLanguageLists();
  return s_commonIso639Languages;
}

RegionList const &
App::regions() {
  initializeLanguageLists();
  return s_regions;
}

RegionList const &
App::commonRegions() {
  initializeLanguageLists();
  return s_commonRegions;
}

CharacterSetList const &
App::characterSets() {
  initializeLanguageLists();
  return s_characterSets;
}

CharacterSetList const &
App::commonCharacterSets() {
  initializeLanguageLists();
  return s_commonCharacterSets;
}

QString const &
App::descriptionForRegion(QString const &region) {
  initializeLanguageLists();
  return s_regionToDescription.contains(region) ? s_regionToDescription[region] : region;
}

bool
App::isInstalled() {
  if (!s_is_installed)
    s_is_installed = mtx::sys::is_installed();
  return s_is_installed.value();
}

void
App::initializeLocale(QString const &requestedLocale) {
  auto locale = Util::Settings::get().localeToUse(requestedLocale);

#if defined(HAVE_LIBINTL_H)
  auto p = p_func();

  if (!locale.isEmpty()) {
    if (p->m_currentTranslator)
      removeTranslator(p->m_currentTranslator.get());
    p->m_currentTranslator.reset();

    auto translator = std::make_unique<QTranslator>();
    auto paths      = QStringList{} << Q("%1/locale/libqt").arg(applicationDirPath())
                                    << mtxQLibrarylocation(QLibraryInfo::TranslationsPath);

    for (auto const &path : paths)
      if (translator->load(Q("qt_%1").arg(locale), path))
        break;

    installTranslator(translator.get());

    p->m_currentTranslator           = std::move(translator);
    Util::Settings::get().m_uiLocale = locale;
  }

#endif  // HAVE_LIBINTL_H

  init_locales(to_utf8(locale));

  retranslateUi();
}

bool
App::isOtherInstanceRunning()
  const {
  auto p = p_func();

  return p->m_otherInstanceRunning;
}

void
App::sendCommandLineArgumentsToRunningInstance() {
  auto p    = p_func();
  auto args = p->m_cliParser->rebuildCommandLine();

  if (!args.isEmpty())
    sendArgumentsToRunningInstance(args);
}

void
App::raiseAndActivateRunningInstance() {
  if (isOtherInstanceRunning())
    sendArgumentsToRunningInstance(QStringList{} << "--activate");
}

void
App::sendArgumentsToRunningInstance(QStringList const &args) {
  auto size  = 0;
  auto block = QByteArray{};
  QDataStream out{&block, QIODevice::WriteOnly};

  out.setVersion(QDataStream::Qt_5_0);
  out << size;
  out << args;

  size = block.size() - sizeof(int);
  out.device()->seek(0);
  out << size;

  auto socket = std::make_unique<QLocalSocket>(this);
  socket->connectToServer(communicatorSocketName());

  if (socket->state() != QLocalSocket::ConnectedState)
    return;

  socket->write(block);
  socket->flush();

  if (!socket->waitForReadyRead(10000))
    return;

  bool ok = false;
  socket->read(reinterpret_cast<char *>(&ok), sizeof(bool));
}

bool
App::parseCommandLineArguments(QStringList const &args) {
  auto p = p_func();

  if (args.isEmpty()) {
    raiseAndActivateRunningInstance();
    return true;
  }

  p->m_cliParser.reset(new GuiCliParser{Util::toStdStringVector(args, 1)});
  p->m_cliParser->run();

  if (p->m_cliParser->exitAfterParsing())
    return false;

  if (!isOtherInstanceRunning())
    return true;

  raiseAndActivateRunningInstance();
  sendCommandLineArgumentsToRunningInstance();

  return false;
}

void
App::handleCommandLineArgumentsLocally() {
  auto p = p_func();

  if (!p->m_cliParser->configFiles().isEmpty())
    Q_EMIT openConfigFilesRequested(p->m_cliParser->configFiles());

  if (!p->m_cliParser->addToMerge().isEmpty())
    Q_EMIT addingFilesToMergeRequested(p->m_cliParser->addToMerge());

  if (!p->m_cliParser->runInfoOn().isEmpty())
    Q_EMIT runningInfoOnRequested(p->m_cliParser->runInfoOn());

  if (!p->m_cliParser->editChapters().isEmpty())
    Q_EMIT editingChaptersRequested(p->m_cliParser->editChapters());

  if (!p->m_cliParser->editHeaders().isEmpty())
    Q_EMIT editingHeadersRequested(p->m_cliParser->editHeaders());

  Q_EMIT toolRequested(p->m_cliParser->requestedTool());
}

void
App::receiveInstanceCommunication() {
  auto p      = p_func();
  auto socket = p->m_instanceCommunicator->nextPendingConnection();

  connect(socket, &QLocalSocket::disconnected, socket, &QLocalSocket::deleteLater);

  QDataStream in{socket};
  in.setVersion(QDataStream::Qt_5_0);

  auto start = QDateTime::currentDateTime();
  auto size  = 0;
  auto args  = QStringList{};
  auto ok    = false;

  while (true) {
    if (!socket->waitForReadyRead(10000))
      break;

    if (!size && (socket->bytesAvailable() >= static_cast<int>(sizeof(int))))
      in >> size;

    if (size && (socket->bytesAvailable()) >= size) {
      in >> args;
      ok = true;
      break;
    }

    auto elapsed = start.msecsTo(QDateTime::currentDateTime());
    if (elapsed >= 10000)
      break;
  }

  if (!ok) {
    socket->disconnectFromServer();
    return;
  }

  socket->write(reinterpret_cast<char *>(&ok), sizeof(bool));
  socket->flush();

  p->m_cliParser.reset(new GuiCliParser{Util::toStdStringVector(args)});
  p->m_cliParser->run();
  handleCommandLineArgumentsLocally();
}

QString
App::settingsBaseGroupName() {
  return Q("MKVToolNix GUI Settings");
}

void
App::setupAppearance() {
  setupColorMode();
  setupUiFont();
}

void
App::setupColorMode() {
#if defined(SYS_WINDOWS)
  if (Util::Settings::get().m_uiDisableDarkStyleSheet) {
    disableDarkMode();
    return;
  }

  QSettings regKey{Q("HKEY_CURRENT_USER\\Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize"), QSettings::NativeFormat};

  auto useLightTheme = regKey.value(Q("AppsUseLightTheme"));

  if (useLightTheme.isValid() && !useLightTheme.toBool())
    enableDarkMode();
  else
    disableDarkMode();
#endif  // SYS_WINDOWS
}

void
App::disableDarkMode() {
#if defined(SYS_WINDOWS)
  auto p = p_func();

  setStyle(QStyleFactory::create("windowsvista"));
  setStyleSheet(p->m_defaultStyleSheet);
  setPalette(p->m_defaultPalette);
#endif
}

void
App::enableDarkMode() {
#if defined(SYS_WINDOWS)
  QPalette darkPalette;
  auto darkColor     = QColor{45, 45, 45};
  auto disabledColor = QColor{127, 127, 127};

  darkPalette.setColor(QPalette::AlternateBase,   darkColor);
  darkPalette.setColor(QPalette::Base,            QColor{18, 18, 18});
  darkPalette.setColor(QPalette::BrightText,      Qt::red);
  darkPalette.setColor(QPalette::Button,          darkColor);
  darkPalette.setColor(QPalette::ButtonText,      Qt::white);
  darkPalette.setColor(QPalette::Disabled,        QPalette::ButtonText,      disabledColor);
  darkPalette.setColor(QPalette::Disabled,        QPalette::HighlightedText, disabledColor);
  darkPalette.setColor(QPalette::Disabled,        QPalette::Text,            disabledColor);
  darkPalette.setColor(QPalette::Disabled,        QPalette::WindowText,      disabledColor);
  darkPalette.setColor(QPalette::Highlight,       QColor{42, 130, 218});
  darkPalette.setColor(QPalette::HighlightedText, Qt::black);
  darkPalette.setColor(QPalette::Link,            QColor{42, 130, 218});
  darkPalette.setColor(QPalette::Text,            Qt::white);
  darkPalette.setColor(QPalette::ToolTipBase,     Qt::white);
  darkPalette.setColor(QPalette::ToolTipText,     Qt::white);
  darkPalette.setColor(QPalette::Window,          darkColor);
  darkPalette.setColor(QPalette::WindowText,      Qt::white);

  setStyle(QStyleFactory::create("Fusion"));
  setPalette(darkPalette);
  setStyleSheet("QToolTip { color: #ffffff; background-color: #2a82da; border: 1px solid white; }");
#endif
}

void
App::run() {
  if (!parseCommandLineArguments(App::arguments()))
    return;

  // Change directory after processing the command line arguments so
  // that relative file names are resolved correctly.
  QDir::setCurrent(QDir::homePath());

  setupAppearance();

  auto mainWindow = std::make_unique<MainWindow>();
  mainWindow->show();

  handleCommandLineArgumentsLocally();

  MainWindow::mergeTool()->addMergeTabIfNoneOpen();

  exec();
}

}
