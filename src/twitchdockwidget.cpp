#include "twitchdockwidget.h"

#include <QDesktopServices>
#include <QDir>
#include <QFile>
#include <QGridLayout>
#include <QHostAddress>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QListWidgetItem>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QColor>
#include <QDateTime>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QHeaderView>
#include <QMessageBox>
#include <QPushButton>
#include <QPointer>
#include <QRegularExpression>
#include <QSettings>
#include <QStandardPaths>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QTextBlock>
#include <QTextCursor>
#include <QTextDocument>
#include <QTextImageFormat>
#include <QTimer>
#include <QTime>
#include <QUrl>
#include <QUrlQuery>
#include <QVBoxLayout>

#include <algorithm>
#include <memory>
#include <optional>

extern "C" {
#if __has_include(<obs/obs-frontend-api.h>)
#include <obs/obs-frontend-api.h>
#else
#include <obs-frontend-api.h>
#endif
#include <obs-module.h>
}

namespace {
constexpr auto kTokenSettingsGroup = "ObsTwitchPlugin";
constexpr auto kCommandsSettingsGroup = "Commands";
constexpr auto kTokenSettingsKey = "twitch_oauth_token";
constexpr auto kChatChannelSettingsKey = "chat_channel";
constexpr auto kClientIdSettingsKey = "twitch_client_id";
constexpr auto kClientSecretSettingsKey = "twitch_client_secret";
constexpr auto kCommandsArraySettingsKey = "custom_commands";
constexpr auto kCommandTriggerSettingsKey = "trigger";
constexpr auto kCommandResponseSettingsKey = "response";
constexpr quint16 kOAuthRedirectPort = 38471;
constexpr auto kDefaultChatColor = "#bf94ff";
constexpr auto kChatMessageTextColor = "#efeff1";
constexpr int kCategorySuggestionLimit = 20;
constexpr qint64 kPendingEmoteWaitTimeoutMs = 5000;
constexpr int kFollowerPollIntervalMs = 30000;
constexpr int kMaxTwitchChatMessageLength = 500;
constexpr int kMaxCommandChainDepth = 16;

void ensureChatEntryStartsOnNewLine(QTextCursor &cursor)
{
    if (cursor.block().length() > 1 || cursor.positionInBlock() > 0) {
        cursor.insertBlock();
    }
}

QString oauthRedirectUrl()
{
    return QStringLiteral("http://localhost:%1").arg(kOAuthRedirectPort);
}

const QStringList &requiredOAuthScopes()
{
    static const QStringList scopes = {
        QStringLiteral("channel:manage:broadcast"),
        QStringLiteral("chat:read"),
        QStringLiteral("chat:edit"),
        QStringLiteral("moderator:read:followers"),
    };
    return scopes;
}

QString requiredOAuthScopesText()
{
    return requiredOAuthScopes().join(QLatin1Char(' '));
}

QString firstNonEmpty(obs_data_t *settings, const std::initializer_list<const char *> &keys)
{
    for (const char *key : keys) {
        const QString value = QString::fromUtf8(obs_data_get_string(settings, key));
        if (!value.trimmed().isEmpty()) {
            return value.trimmed();
        }
    }
    return {};
}

QString ircTagValue(const QString &tags, const QString &key)
{
    const QString prefix = key + QLatin1Char('=');
    const QStringList parts = tags.split(QLatin1Char(';'));
    for (const QString &part : parts) {
        if (!part.startsWith(prefix)) {
            continue;
        }

        const QString rawValue = part.mid(prefix.size());
        QString decodedValue;
        decodedValue.reserve(rawValue.size());
        for (int index = 0; index < rawValue.size(); ++index) {
            if (rawValue.at(index) != QLatin1Char('\\') || index + 1 >= rawValue.size()) {
                decodedValue.append(rawValue.at(index));
                continue;
            }

            ++index;
            switch (rawValue.at(index).unicode()) {
            case 's':
                decodedValue.append(QLatin1Char(' '));
                break;
            case ':':
                decodedValue.append(QLatin1Char(';'));
                break;
            case 'r':
                decodedValue.append(QLatin1Char('\r'));
                break;
            case 'n':
                decodedValue.append(QLatin1Char('\n'));
                break;
            case '\\':
                decodedValue.append(QLatin1Char('\\'));
                break;
            default:
                decodedValue.append(rawValue.at(index));
                break;
            }
        }
        return decodedValue;
    }
    return {};
}

QString sanitizeChatColor(const QString &colorValue)
{
    static const QRegularExpression hexColorPattern(QStringLiteral("^#[0-9A-Fa-f]{6}$"));
    return hexColorPattern.match(colorValue).hasMatch() ? colorValue : QString::fromLatin1(kDefaultChatColor);
}

QString ircCommand(const QString &line)
{
    static const QRegularExpression commandPattern(QStringLiteral("^(?:@[^\\s]+\\s+)?(?::[^\\s]+\\s+)?([^\\s]+)"));
    const QRegularExpressionMatch match = commandPattern.match(line);
    if (!match.hasMatch()) {
        return {};
    }
    return match.captured(1).toUpper();
}

QUrl twitchEmoteUrl(const QString &emoteId)
{
    return QUrl(QStringLiteral("https://static-cdn.jtvnw.net/emoticons/v2/%1/default/dark/2.0").arg(emoteId));
}

QUrl emoteResourceUrl(const QString &emoteId)
{
    return QUrl(QStringLiteral("twitch-emote://%1").arg(emoteId));
}

struct FollowerActivityRecord {
    QString id;
    QString displayName;
    QDateTime followedAt;
};

class CommandDialog final : public QDialog {
public:
    CommandDialog(const QString &windowTitle,
                  const QString &initialTrigger,
                  const QString &initialResponse,
                  QWidget *parent = nullptr)
        : QDialog(parent)
    {
        setWindowTitle(windowTitle);
        setModal(true);
        resize(520, 260);

        auto *layout = new QVBoxLayout(this);
        auto *formLayout = new QFormLayout();

        triggerEdit_ = new QLineEdit(this);
        triggerEdit_->setPlaceholderText(tr("command"));
        triggerEdit_->setText(initialTrigger);

        responseEdit_ = new QTextEdit(this);
        responseEdit_->setAcceptRichText(false);
        responseEdit_->setPlaceholderText(
            tr("Command response text (max %1 characters)").arg(kMaxTwitchChatMessageLength));
        responseEdit_->setPlainText(initialResponse);

        formLayout->addRow(tr("Trigger"), triggerEdit_);
        formLayout->addRow(tr("Response"), responseEdit_);
        layout->addLayout(formLayout);
        layout->addWidget(
            new QLabel(tr("Twitch chat responses are limited to %1 characters.").arg(kMaxTwitchChatMessageLength), this));

        auto *buttons = new QDialogButtonBox(QDialogButtonBox::Save | QDialogButtonBox::Cancel, this);
        connect(buttons, &QDialogButtonBox::accepted, this, [this]() {
            if (trigger().isEmpty()) {
                QMessageBox::warning(this, tr("Invalid Command"), tr("Command trigger cannot be empty."));
                return;
            }
            if (response().isEmpty()) {
                QMessageBox::warning(this, tr("Invalid Command"), tr("Command response cannot be empty."));
                return;
            }
            if (response().size() > kMaxTwitchChatMessageLength) {
                QMessageBox::warning(this,
                                     tr("Invalid Command"),
                                     tr("Command response exceeds Twitch's %1 character limit.")
                                         .arg(kMaxTwitchChatMessageLength));
                return;
            }
            accept();
        });
        connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
        layout->addWidget(buttons);

        triggerEdit_->setFocus();
        triggerEdit_->selectAll();
    }

    QString trigger() const
    {
        return triggerEdit_->text().trimmed();
    }

    QString response() const
    {
        return responseEdit_->toPlainText().trimmed();
    }

private:
    QLineEdit *triggerEdit_ = nullptr;
    QTextEdit *responseEdit_ = nullptr;
};

}

TwitchDockWidget::TwitchDockWidget(QWidget *parent)
    : QWidget(parent),
      oauthServer_(new QTcpServer(this)),
      chatSocket_(new QTcpSocket(this)),
      networkManager_(new QNetworkAccessManager(this))
{
    buildUi();

    connect(oauthServer_, &QTcpServer::newConnection, this, &TwitchDockWidget::onOAuthServerConnection);
    connect(chatSocket_, &QTcpSocket::readyRead, this, &TwitchDockWidget::onChatSocketReadyRead);
    connect(chatSocket_, &QTcpSocket::disconnected, this, [this]() { stopFollowerActivityPolling(); });
    categorySuggestTimer_ = new QTimer(this);
    categorySuggestTimer_->setSingleShot(true);
    categorySuggestTimer_->setInterval(250);
    connect(categorySuggestTimer_, &QTimer::timeout, this, [this]() {
        fetchCategorySuggestions(gameIdEdit_->text().trimmed());
    });
    followerPollTimer_ = new QTimer(this);
    followerPollTimer_->setInterval(kFollowerPollIntervalMs);
    connect(followerPollTimer_, &QTimer::timeout, this, [this]() { pollLatestFollowers(false); });

    loadPersistedUiState();
    loadPersistedCommands();
    QTimer::singleShot(0, this, &TwitchDockWidget::refreshObsServiceData);
}

TwitchDockWidget::~TwitchDockWidget()
{
    if (oauthServer_) {
        oauthServer_->close();
    }

    if (chatSocket_) {
        chatSocket_->abort();
    }

    stopFollowerActivityPolling();
}

void TwitchDockWidget::buildUi()
{
    auto *layout = new QVBoxLayout(this);
    tabs_ = new QTabWidget(this);

    // Chat tab: channel controls, a stylized timeline, and outbound chat sending.
    auto *chatTab = new QWidget(this);
    auto *chatLayout = new QVBoxLayout(chatTab);
    chatText_ = new QTextEdit(chatTab);
    chatText_->setReadOnly(true);
    chatText_->document()->setMaximumBlockCount(2000);
    chatText_->setStyleSheet("QTextEdit { background-color: #0e0e10; color: #efeff1; border: 1px solid #2f3545; font-family: Inter, Segoe UI, sans-serif; font-size: 13px; }");
    chatText_->setHtml(QStringLiteral(
        "<p style='margin:0; color:#adadb8;'>Twitch chat panel ready. Provide OAuth token, then join channel via IRC.</p>"));

    auto *chatRow = new QHBoxLayout();
    channelEdit_ = new QLineEdit(chatTab);
    channelEdit_->setPlaceholderText(tr("Channel login (e.g. yourname)"));
    connect(channelEdit_, &QLineEdit::editingFinished, this, [this]() {
        persistChatChannel(sanitizeChannelLogin(channelEdit_->text()));
    });
    auto *chatConnectButton = new QPushButton(tr("Connect Chat"), chatTab);
    chatRow->addWidget(channelEdit_);
    chatRow->addWidget(chatConnectButton);

    auto *messageRow = new QHBoxLayout();
    messageEdit_ = new QLineEdit(chatTab);
    messageEdit_->setPlaceholderText(tr("Type a chat message"));
    auto *sendMessageButton = new QPushButton(tr("Send"), chatTab);
    messageRow->addWidget(messageEdit_);
    messageRow->addWidget(sendMessageButton);

    connect(chatConnectButton, &QPushButton::clicked, this, &TwitchDockWidget::connectChat);
    connect(sendMessageButton, &QPushButton::clicked, this, &TwitchDockWidget::sendManualMessage);
    connect(messageEdit_, &QLineEdit::returnPressed, this, &TwitchDockWidget::sendManualMessage);

    chatLayout->addLayout(chatRow);
    chatLayout->addWidget(chatText_);
    chatLayout->addLayout(messageRow);
    tabs_->addTab(chatTab, tr("Chat"));

    // Stream tab: title/category updates plus OAuth acquisition fields.
    auto *streamTab = new QWidget(this);
    auto *streamLayout = new QGridLayout(streamTab);
    titleEdit_ = new QLineEdit(streamTab);
    gameIdEdit_ = new QLineEdit(streamTab);
    gameIdEdit_->setPlaceholderText(tr("Category name (e.g. Just Chatting)"));
    gameCategorySuggestionsModel_ = new QStringListModel(this);
    gameCategoryCompleter_ = new QCompleter(gameCategorySuggestionsModel_, gameIdEdit_);
    gameCategoryCompleter_->setCaseSensitivity(Qt::CaseInsensitive);
    gameCategoryCompleter_->setFilterMode(Qt::MatchContains);
    gameCategoryCompleter_->setCompletionMode(QCompleter::PopupCompletion);
    gameIdEdit_->setCompleter(gameCategoryCompleter_);
    connect(gameIdEdit_, &QLineEdit::textEdited, this, [this](const QString &text) {
        const QString query = text.trimmed();
        if (query.isEmpty()) {
            if (categorySuggestReply_) {
                categorySuggestReply_->abort();
            }
            gameCategorySuggestionsModel_->setStringList({});
            return;
        }
        categorySuggestTimer_->start();
    });
    clientIdEdit_ = new QLineEdit(streamTab);
    clientIdEdit_->setEchoMode(QLineEdit::Password);
    connect(clientIdEdit_, &QLineEdit::editingFinished, this, [this]() {
        persistClientId(clientIdEdit_->text());
    });
    clientSecretEdit_ = new QLineEdit(streamTab);
    clientSecretEdit_->setEchoMode(QLineEdit::Password);
    connect(clientSecretEdit_, &QLineEdit::editingFinished, this, [this]() {
        persistClientSecret(clientSecretEdit_->text());
    });
    tokenEdit_ = new QLineEdit(streamTab);
    tokenEdit_->setEchoMode(QLineEdit::Password);
    auto *oauthHelpLabel = new QLabel(
        tr("Create a Twitch app in the Developer Console, add redirect URL %1, paste the Client ID and Client Secret here, then click Authorize in Browser to grant channel update, chat read/write, and follower activity access.")
            .arg(oauthRedirectUrl()),
        nullptr);
    oauthHelpLabel->setWordWrap(true);

    auto *refreshButton = new QPushButton(tr("Refresh OBS Twitch Settings"), streamTab);
    auto *developerConsoleButton = new QPushButton(tr("Open Twitch Developer Console"), streamTab);
    auto *oauthButton = new QPushButton(tr("Authorize in Browser"), streamTab);
    auto *updateButton = new QPushButton(tr("Update Channel Info"), streamTab);
    toggleAuthFieldsButton_ = new QPushButton(tr("Authorization Settings"), streamTab);
    authFieldsContainer_ = new QWidget(this, Qt::Dialog);
    authFieldsContainer_->setWindowTitle(tr("Twitch Authorization"));
    authFieldsContainer_->setAttribute(Qt::WA_DeleteOnClose, false);
    authFieldsContainer_->hide();
    auto *authLayout = new QGridLayout(authFieldsContainer_);
    authLayout->setContentsMargins(0, 0, 0, 0);
    authLayout->addWidget(oauthHelpLabel, 0, 0, 1, 2);
    authLayout->addWidget(new QLabel(tr("Twitch Client ID"), authFieldsContainer_), 1, 0);
    authLayout->addWidget(clientIdEdit_, 1, 1);
    authLayout->addWidget(new QLabel(tr("Twitch Client Secret"), authFieldsContainer_), 2, 0);
    authLayout->addWidget(clientSecretEdit_, 2, 1);
    authLayout->addWidget(new QLabel(tr("OAuth Token"), authFieldsContainer_), 3, 0);
    authLayout->addWidget(tokenEdit_, 3, 1);
    authLayout->addWidget(developerConsoleButton, 4, 0, 1, 2);
    authLayout->addWidget(oauthButton, 5, 0, 1, 2);

    streamLayout->addWidget(new QLabel(tr("Stream Title"), streamTab), 0, 0);
    streamLayout->addWidget(titleEdit_, 0, 1);
    streamLayout->addWidget(new QLabel(tr("Category"), streamTab), 1, 0);
    streamLayout->addWidget(gameIdEdit_, 1, 1);
    streamLayout->addWidget(updateButton, 2, 0, 1, 2);
    streamLayout->addWidget(refreshButton, 3, 0, 1, 2);
    streamLayout->setRowStretch(4, 1);
    streamLayout->addWidget(toggleAuthFieldsButton_, 5, 0, 1, 2);

    connect(refreshButton, &QPushButton::clicked, this, &TwitchDockWidget::refreshObsServiceData);
    connect(developerConsoleButton, &QPushButton::clicked, this, &TwitchDockWidget::openTwitchDeveloperConsole);
    connect(oauthButton, &QPushButton::clicked, this, &TwitchDockWidget::ensureOAuthToken);
    connect(updateButton, &QPushButton::clicked, this, &TwitchDockWidget::updateChannelInfo);
    connect(toggleAuthFieldsButton_, &QPushButton::clicked, this, [this]() {
        if (authFieldsContainer_) {
            authFieldsContainer_->show();
            authFieldsContainer_->raise();
            authFieldsContainer_->activateWindow();
        }
    });

    tabs_->addTab(streamTab, tr("Stream Info"));

    // Friend streaming tab: quick open list for squad/friend channels.
    auto *friendTab = new QWidget(this);
    auto *friendLayout = new QVBoxLayout(friendTab);
    friendLinks_ = new QListWidget(friendTab);
    friendLinkInput_ = new QLineEdit(friendTab);
    friendLinkInput_->setPlaceholderText(tr("https://www.twitch.tv/<channel>"));

    auto *friendButtonRow = new QHBoxLayout();
    auto *addButton = new QPushButton(tr("Add"), friendTab);
    auto *removeButton = new QPushButton(tr("Remove"), friendTab);
    auto *openButton = new QPushButton(tr("Open"), friendTab);
    friendButtonRow->addWidget(addButton);
    friendButtonRow->addWidget(removeButton);
    friendButtonRow->addWidget(openButton);

    connect(addButton, &QPushButton::clicked, this, &TwitchDockWidget::addFriendLink);
    connect(removeButton, &QPushButton::clicked, this, &TwitchDockWidget::removeSelectedFriendLink);
    connect(openButton, &QPushButton::clicked, this, &TwitchDockWidget::openSelectedFriendLink);

    friendLayout->addWidget(friendLinkInput_);
    friendLayout->addWidget(friendLinks_);
    friendLayout->addLayout(friendButtonRow);

    tabs_->addTab(friendTab, tr("Friend Streaming"));

    auto *commandsTab = new QWidget(this);
    auto *commandsLayout = new QVBoxLayout(commandsTab);
    auto *commandButtonRow = new QHBoxLayout();
    commandButtonRow->addStretch();
    auto *addCommandButton = new QPushButton(tr("Add Command"), commandsTab);
    commandButtonRow->addWidget(addCommandButton);

    commandsTable_ = new QTableWidget(commandsTab);
    commandsTable_->setColumnCount(3);
    commandsTable_->setHorizontalHeaderLabels({tr("Trigger"), tr("Response"), tr("Actions")});
    commandsTable_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    commandsTable_->setSelectionMode(QAbstractItemView::NoSelection);
    commandsTable_->setFocusPolicy(Qt::NoFocus);
    commandsTable_->verticalHeader()->setVisible(false);
    commandsTable_->horizontalHeader()->setStretchLastSection(false);
    commandsTable_->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    commandsTable_->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
    commandsTable_->horizontalHeader()->setSectionResizeMode(2, QHeaderView::ResizeToContents);

    connect(addCommandButton, &QPushButton::clicked, this, &TwitchDockWidget::addCommand);

    commandsLayout->addLayout(commandButtonRow);
    commandsLayout->addWidget(commandsTable_);
    tabs_->addTab(commandsTab, tr("Commands"));
    layout->addWidget(tabs_);
}

void TwitchDockWidget::refreshObsServiceData()
{
    // Pull credentials from active OBS service first, then fallback to profile INI.
    TwitchCredentials credentials = extractCredentialsFromObs();
    if (credentials.streamKey.isEmpty() || credentials.oauthToken.isEmpty()) {
        const TwitchCredentials profileFallback = extractCredentialsFromObsProfileIni();
        if (credentials.streamKey.isEmpty()) {
            credentials.streamKey = profileFallback.streamKey;
        }
        if (credentials.oauthToken.isEmpty()) {
            credentials.oauthToken = profileFallback.oauthToken;
        }
    }

    if (!credentials.oauthToken.isEmpty()) {
        tokenEdit_->setText(credentials.oauthToken);
        persistOAuthToken(credentials.oauthToken);
    } else {
        tokenEdit_->setText(loadCachedOAuthToken());
    }
    if (!credentials.clientId.isEmpty()) {
        clientIdEdit_->setText(credentials.clientId);
        persistClientId(credentials.clientId);
    } else {
        clientIdEdit_->setText(loadCachedClientId());
    }
    if (!credentials.clientSecret.isEmpty()) {
        clientSecretEdit_->setText(credentials.clientSecret);
        persistClientSecret(credentials.clientSecret);
    } else {
        clientSecretEdit_->setText(loadCachedClientSecret());
    }
    appendChatSystemMessage(
        credentials.streamKey.isEmpty() ? tr("Stream key not found in current OBS service/profile settings.")
                                        : tr("Stream key found in OBS settings."));

    if (!tokenEdit_->text().trimmed().isEmpty() && !clientIdEdit_->text().trimmed().isEmpty()) {
        fetchCurrentChannelInfo();
    }

    if (!chatAutoConnectAttempted_) {
        const QString channel = sanitizeChannelLogin(channelEdit_->text());
        if (!channel.isEmpty() && !tokenEdit_->text().trimmed().isEmpty()) {
            chatAutoConnectAttempted_ = true;
            connectChat();
        }
    }
}

void TwitchDockWidget::openTwitchDeveloperConsole()
{
    const QUrl url(QStringLiteral("https://dev.twitch.tv/console/apps"));
    if (QDesktopServices::openUrl(url)) {
        appendChatSystemMessage(tr("Opened Twitch Developer Console in your browser."));
    } else {
        appendChatSystemMessage(tr("Could not open Twitch Developer Console in your browser."));
    }
}

TwitchDockWidget::TwitchCredentials TwitchDockWidget::extractCredentialsFromObs() const
{
    TwitchCredentials credentials;

    // OBS frontend API gives us the currently selected streaming service object.
    obs_service_t *service = obs_frontend_get_streaming_service();
    if (!service) {
        return credentials;
    }

    obs_data_t *settings = obs_service_get_settings(service);
    if (settings) {
        // Field names vary by service/profile; check common variants.
        credentials.streamKey = firstNonEmpty(settings, {"key", "stream_key", "streamKey"});
        credentials.oauthToken = firstNonEmpty(settings, {"oauth_token", "token", "access_token", "auth"});
        credentials.clientId = firstNonEmpty(settings, {"client_id", "clientid", "twitch_client_id"});
        credentials.clientSecret = firstNonEmpty(settings, {"client_secret", "clientsecret", "twitch_client_secret"});
        obs_data_release(settings);
    }

    return credentials;
}

TwitchDockWidget::TwitchCredentials TwitchDockWidget::extractCredentialsFromObsProfileIni() const
{
    TwitchCredentials credentials;

    // OBS Linux profile data typically lives in ~/.config/obs-studio/basic/profiles/*/basic.ini.
    const QString root = QDir::homePath() + QStringLiteral("/.config/obs-studio/basic/profiles");
    const QDir profilesDir(root);
    const QFileInfoList profileEntries = profilesDir.entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot);

    for (const QFileInfo &profile : profileEntries) {
        const QString basicIniPath = profile.filePath() + QStringLiteral("/basic.ini");
        if (!QFileInfo::exists(basicIniPath)) {
            continue;
        }

        QSettings ini(basicIniPath, QSettings::IniFormat);
        const QStringList keys = ini.allKeys();
        for (const QString &key : keys) {
            const QString lowerKey = key.toLower();
            const QString value = ini.value(key).toString().trimmed();
            if (value.isEmpty()) {
                continue;
            }

            if (credentials.streamKey.isEmpty() && (lowerKey.contains("streamkey") || lowerKey.endsWith("/key"))) {
                credentials.streamKey = value;
            }
            if (credentials.oauthToken.isEmpty() && (lowerKey.contains("oauth") || lowerKey.contains("token"))) {
                credentials.oauthToken = value;
            }
            if (credentials.clientId.isEmpty() && (lowerKey.contains("clientid") || lowerKey.contains("client_id"))) {
                credentials.clientId = value;
            }
            if (credentials.clientSecret.isEmpty() &&
                (lowerKey.contains("clientsecret") || lowerKey.contains("client_secret"))) {
                credentials.clientSecret = value;
            }
        }

        if (!credentials.streamKey.isEmpty() && !credentials.oauthToken.isEmpty() && !credentials.clientId.isEmpty()) {
            break;
        }
    }

    return credentials;
}

void TwitchDockWidget::ensureOAuthToken()
{
    const QString clientId = clientIdEdit_->text().trimmed();
    const QString clientSecret = clientSecretEdit_->text().trimmed();
    if (clientId.isEmpty()) {
        appendChatSystemMessage(
            tr("Provide Twitch Client ID before starting OAuth. Use Open Twitch Developer Console and register redirect URL %1.")
                .arg(oauthRedirectUrl()));
        return;
    }
    if (clientSecret.isEmpty()) {
        appendChatSystemMessage(tr("Provide Twitch Client Secret before starting OAuth."));
        return;
    }
    persistClientId(clientId);
    persistClientSecret(clientSecret);

    const auto startAuthorizationFlow = [this, clientId]() {
        startOAuthServer();
        if (!oauthServer_->isListening()) {
            return;
        }

        // Use authorization-code flow so the localhost callback can capture ?code=...
        QUrl url(QStringLiteral("https://id.twitch.tv/oauth2/authorize"));
        QUrlQuery query;
        query.addQueryItem(QStringLiteral("response_type"), QStringLiteral("code"));
        query.addQueryItem(QStringLiteral("client_id"), clientId);
        query.addQueryItem(QStringLiteral("redirect_uri"), oauthRedirectUrl());
        query.addQueryItem(QStringLiteral("scope"), requiredOAuthScopesText());
        query.addQueryItem(QStringLiteral("force_verify"), QStringLiteral("false"));
        url.setQuery(query);

        appendChatSystemMessage(tr("Opening Twitch OAuth authorization page..."));
        QDesktopServices::openUrl(url);
    };
    startAuthorizationFlow();
}

void TwitchDockWidget::startOAuthServer()
{
    if (oauthServer_->isListening()) {
        return;
    }

    if (!oauthServer_->listen(QHostAddress::LocalHost, kOAuthRedirectPort)) {
        appendChatSystemMessage(tr("OAuth callback server failed on localhost:%1").arg(kOAuthRedirectPort));
        return;
    }

    appendChatSystemMessage(tr("OAuth callback server listening on localhost:%1").arg(kOAuthRedirectPort));
}

void TwitchDockWidget::onOAuthServerConnection()
{
    while (oauthServer_->hasPendingConnections()) {
        QTcpSocket *socket = oauthServer_->nextPendingConnection();
        if (!socket) {
            continue;
        }

        socket->setParent(this);
        connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);

        connect(socket, &QTcpSocket::readyRead, this, [this, socket = QPointer<QTcpSocket>(socket)]() {
            if (!socket) {
                return;
            }

            handleOAuthRedirectPayload(socket->readAll(), socket.data());
        });
    }
}

void TwitchDockWidget::handleOAuthRedirectPayload(const QByteArray &requestPayload, QTcpSocket *clientSocket)
{
    const QString requestText = QString::fromUtf8(requestPayload);
    const QRegularExpression rx(QStringLiteral("GET\\s+/(?:\\?([^\\s]+))?\\s+HTTP/1"));
    const QRegularExpressionMatch match = rx.match(requestText);

    QString response = QStringLiteral("HTTP/1.1 400 Bad Request\r\nContent-Type: text/plain\r\n\r\nMissing OAuth authorization code.");

    if (match.hasMatch()) {
        const QUrlQuery query(match.captured(1));
        const QString code = query.queryItemValue(QStringLiteral("code")).trimmed();
        if (!code.isEmpty()) {
            exchangeOAuthCodeForToken(code);
            response = QStringLiteral("HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\n\r\nOBS Twitch authorization received. You can close this browser tab.");
        }
    }

    clientSocket->write(response.toUtf8());
    clientSocket->disconnectFromHost();
}

void TwitchDockWidget::exchangeOAuthCodeForToken(const QString &authorizationCode)
{
    const QString clientId = clientIdEdit_->text().trimmed();
    const QString clientSecret = clientSecretEdit_->text().trimmed();

    if (clientId.isEmpty() || clientSecret.isEmpty()) {
        appendChatSystemMessage(tr("OAuth code received but client secret is missing; cannot exchange token."));
        return;
    }

    QUrl url(QStringLiteral("https://id.twitch.tv/oauth2/token"));
    QUrlQuery body;
    body.addQueryItem(QStringLiteral("client_id"), clientId);
    body.addQueryItem(QStringLiteral("client_secret"), clientSecret);
    body.addQueryItem(QStringLiteral("code"), authorizationCode);
    body.addQueryItem(QStringLiteral("grant_type"), QStringLiteral("authorization_code"));
    body.addQueryItem(QStringLiteral("redirect_uri"), oauthRedirectUrl());

    QNetworkRequest request(url);
    request.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/x-www-form-urlencoded"));

    QNetworkReply *reply = networkManager_->post(request, body.query(QUrl::FullyEncoded).toUtf8());
    connect(reply, &QNetworkReply::finished, this, [this, reply]() {
        const QByteArray payloadBytes = reply->readAll();
        const QNetworkReply::NetworkError error = reply->error();
        const QString errorString = reply->errorString();
        reply->deleteLater();

        if (error != QNetworkReply::NoError) {
            appendChatSystemMessage(tr("OAuth token exchange failed: %1").arg(errorString));
            return;
        }

        const QJsonObject payload = QJsonDocument::fromJson(payloadBytes).object();
        const QString token = payload.value(QStringLiteral("access_token")).toString().trimmed();

        if (token.isEmpty()) {
            appendChatSystemMessage(tr("OAuth token exchange did not return an access token."));
            return;
        }

        broadcasterId_.clear();
        twitchLogin_.clear();
        validatedToken_.clear();
        validatedScopes_.clear();
        tokenEdit_->setText(token);
        persistOAuthToken(token);
        appendChatSystemMessage(tr("OAuth token exchange succeeded and token was cached."));
        if (authFieldsContainer_) {
            authFieldsContainer_->hide();
        }
        fetchCurrentChannelInfo();
    });
}

void TwitchDockWidget::persistOAuthToken(const QString &token)
{
    const QString configDir = QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation);
    QDir().mkpath(configDir);

    const QString settingsPath = configDir + QStringLiteral("/obstwitchplugin.ini");
    QSettings settings(settingsPath, QSettings::IniFormat);
    settings.beginGroup(kTokenSettingsGroup);
    settings.setValue(kTokenSettingsKey, token);
    settings.endGroup();
    settings.sync();

    // Restrict token cache file permissions to current user only.
    QFile::setPermissions(settingsPath, QFile::ReadOwner | QFile::WriteOwner);
}

QString TwitchDockWidget::loadCachedOAuthToken() const
{
    const QString configDir = QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation);
    const QString settingsPath = configDir + QStringLiteral("/obstwitchplugin.ini");

    QSettings settings(settingsPath, QSettings::IniFormat);
    settings.beginGroup(kTokenSettingsGroup);
    const QString token = settings.value(kTokenSettingsKey).toString().trimmed();
    settings.endGroup();
    return token;
}

void TwitchDockWidget::persistClientId(const QString &clientId)
{
    const QString value = clientId.trimmed();
    if (value.isEmpty()) {
        return;
    }

    const QString configDir = QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation);
    QDir().mkpath(configDir);

    const QString settingsPath = configDir + QStringLiteral("/obstwitchplugin.ini");
    QSettings settings(settingsPath, QSettings::IniFormat);
    settings.beginGroup(kTokenSettingsGroup);
    settings.setValue(kClientIdSettingsKey, value);
    settings.endGroup();
    settings.sync();
}

QString TwitchDockWidget::loadCachedClientId() const
{
    const QString configDir = QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation);
    const QString settingsPath = configDir + QStringLiteral("/obstwitchplugin.ini");

    QSettings settings(settingsPath, QSettings::IniFormat);
    settings.beginGroup(kTokenSettingsGroup);
    const QString clientId = settings.value(kClientIdSettingsKey).toString().trimmed();
    settings.endGroup();
    return clientId;
}

void TwitchDockWidget::persistClientSecret(const QString &clientSecret)
{
    const QString value = clientSecret.trimmed();

    const QString configDir = QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation);
    QDir().mkpath(configDir);

    const QString settingsPath = configDir + QStringLiteral("/obstwitchplugin.ini");
    QSettings settings(settingsPath, QSettings::IniFormat);
    settings.beginGroup(kTokenSettingsGroup);
    if (value.isEmpty()) {
        settings.remove(kClientSecretSettingsKey);
    } else {
        settings.setValue(kClientSecretSettingsKey, value);
    }
    settings.endGroup();
    settings.sync();

    QFile::setPermissions(settingsPath, QFile::ReadOwner | QFile::WriteOwner);
}

QString TwitchDockWidget::loadCachedClientSecret() const
{
    const QString configDir = QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation);
    const QString settingsPath = configDir + QStringLiteral("/obstwitchplugin.ini");

    QSettings settings(settingsPath, QSettings::IniFormat);
    settings.beginGroup(kTokenSettingsGroup);
    const QString clientSecret = settings.value(kClientSecretSettingsKey).toString().trimmed();
    settings.endGroup();
    return clientSecret;
}

void TwitchDockWidget::persistChatChannel(const QString &channel)
{
    const QString normalized = sanitizeChannelLogin(channel);

    const QString configDir = QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation);
    QDir().mkpath(configDir);

    const QString settingsPath = configDir + QStringLiteral("/obstwitchplugin.ini");
    QSettings settings(settingsPath, QSettings::IniFormat);
    settings.beginGroup(kTokenSettingsGroup);
    settings.setValue(kChatChannelSettingsKey, normalized);
    settings.endGroup();
    settings.sync();
}

QString TwitchDockWidget::loadCachedChatChannel() const
{
    const QString configDir = QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation);
    const QString settingsPath = configDir + QStringLiteral("/obstwitchplugin.ini");

    QSettings settings(settingsPath, QSettings::IniFormat);
    settings.beginGroup(kTokenSettingsGroup);
    const QString channel = settings.value(kChatChannelSettingsKey).toString().trimmed();
    settings.endGroup();
    return sanitizeChannelLogin(channel);
}

QString TwitchDockWidget::sanitizeChannelLogin(const QString &value) const
{
    QString channel = value.trimmed();
    if (channel.startsWith(QLatin1Char('#'))) {
        channel.remove(0, 1);
    }
    return channel;
}

void TwitchDockWidget::loadPersistedUiState()
{
    channelEdit_->setText(loadCachedChatChannel());
    clientIdEdit_->setText(loadCachedClientId());
    clientSecretEdit_->setText(loadCachedClientSecret());
    tokenEdit_->setText(loadCachedOAuthToken());
}

bool TwitchDockWidget::showCommandDialog(const QString &windowTitle,
                                         const QString &initialTrigger,
                                         const QString &initialResponse,
                                         QString &trigger,
                                         QString &response)
{
    CommandDialog dialog(windowTitle, initialTrigger, initialResponse, this);
    if (dialog.exec() != QDialog::Accepted) {
        return false;
    }

    trigger = dialog.trigger();
    response = dialog.response();
    return true;
}

int TwitchDockWidget::commandRowForTrigger(const QString &trigger) const
{
    if (!commandsTable_) {
        return -1;
    }

    for (int row = 0; row < commandsTable_->rowCount(); ++row) {
        const QTableWidgetItem *item = commandsTable_->item(row, 0);
        if (item && item->text() == trigger) {
            return row;
        }
    }
    return -1;
}

void TwitchDockWidget::populateCommandRow(int row, const QString &trigger, const QString &response)
{
    if (!commandsTable_ || row < 0 || row >= commandsTable_->rowCount()) {
        return;
    }

    auto *triggerItem = commandsTable_->item(row, 0);
    if (!triggerItem) {
        triggerItem = new QTableWidgetItem();
        triggerItem->setFlags(triggerItem->flags() & ~Qt::ItemIsEditable);
        commandsTable_->setItem(row, 0, triggerItem);
    }
    triggerItem->setText(trigger);

    auto *responseItem = commandsTable_->item(row, 1);
    if (!responseItem) {
        responseItem = new QTableWidgetItem();
        responseItem->setFlags(responseItem->flags() & ~Qt::ItemIsEditable);
        commandsTable_->setItem(row, 1, responseItem);
    }
    responseItem->setText(response);

    QWidget *actionsWidget = commandsTable_->cellWidget(row, 2);
    QPushButton *editButton = nullptr;
    QPushButton *deleteButton = nullptr;
    if (!actionsWidget) {
        actionsWidget = new QWidget(commandsTable_);
        auto *actionsLayout = new QHBoxLayout(actionsWidget);
        actionsLayout->setContentsMargins(0, 0, 0, 0);
        actionsLayout->setSpacing(6);

        editButton = new QPushButton(tr("Edit"), actionsWidget);
        deleteButton = new QPushButton(tr("Delete"), actionsWidget);
        editButton->setObjectName(QStringLiteral("editButton"));
        deleteButton->setObjectName(QStringLiteral("deleteButton"));
        connect(editButton, &QPushButton::clicked, this, &TwitchDockWidget::editCommandFromButton);
        connect(deleteButton, &QPushButton::clicked, this, &TwitchDockWidget::deleteCommandFromButton);

        actionsLayout->addWidget(editButton);
        actionsLayout->addWidget(deleteButton);
        actionsLayout->addStretch();
        commandsTable_->setCellWidget(row, 2, actionsWidget);
    } else {
        editButton = actionsWidget->findChild<QPushButton *>(QStringLiteral("editButton"));
        deleteButton = actionsWidget->findChild<QPushButton *>(QStringLiteral("deleteButton"));
    }

    if (editButton) {
        editButton->setProperty("commandTrigger", trigger);
    }
    if (deleteButton) {
        deleteButton->setProperty("commandTrigger", trigger);
    }
    commandsTable_->resizeRowsToContents();
}

void TwitchDockWidget::editCommandFromButton()
{
    const auto *button = qobject_cast<const QPushButton *>(sender());
    if (!button) {
        return;
    }

    editCommand(button->property("commandTrigger").toString());
}

void TwitchDockWidget::deleteCommandFromButton()
{
    const auto *button = qobject_cast<const QPushButton *>(sender());
    if (!button) {
        return;
    }

    deleteCommand(button->property("commandTrigger").toString());
}

void TwitchDockWidget::addCommand()
{
    QString trigger;
    QString response;
    if (!showCommandDialog(tr("Add Command"), {}, {}, trigger, response)) {
        return;
    }

    if (customCommands_.contains(trigger)) {
        appendChatSystemMessage(tr("Command %1 already exists. Edit it instead.").arg(trigger));
        return;
    }

    customCommands_.insert(trigger, response);
    persistCommands();
    const int row = std::distance(customCommands_.cbegin(), customCommands_.constFind(trigger));
    commandsTable_->insertRow(row);
    populateCommandRow(row, trigger, response);
    appendChatSystemMessage(tr("Saved command %1.").arg(trigger));
}

void TwitchDockWidget::editCommand(const QString &existingTrigger)
{
    const auto it = customCommands_.constFind(existingTrigger);
    if (it == customCommands_.cend()) {
        return;
    }

    QString trigger;
    QString response;
    if (!showCommandDialog(tr("Edit Command"), existingTrigger, it.value(), trigger, response)) {
        return;
    }

    if (trigger != existingTrigger && customCommands_.contains(trigger)) {
        appendChatSystemMessage(tr("Command %1 already exists. Choose a different trigger.").arg(trigger));
        return;
    }

    const int existingRow = commandRowForTrigger(existingTrigger);
    customCommands_.remove(existingTrigger);
    if (existingRow >= 0) {
        commandsTable_->removeRow(existingRow);
    }

    customCommands_.insert(trigger, response);
    persistCommands();
    const int row = std::distance(customCommands_.cbegin(), customCommands_.constFind(trigger));
    commandsTable_->insertRow(row);
    populateCommandRow(row, trigger, response);
    appendChatSystemMessage(tr("Updated command %1.").arg(trigger));
}

void TwitchDockWidget::deleteCommand(const QString &trigger)
{
    if (customCommands_.remove(trigger) == 0) {
        return;
    }

    persistCommands();
    const int row = commandRowForTrigger(trigger);
    if (row >= 0) {
        commandsTable_->removeRow(row);
    }
    appendChatSystemMessage(tr("Deleted command %1.").arg(trigger));
}

void TwitchDockWidget::persistCommands() const
{
    const QString configDir = QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation);
    QDir().mkpath(configDir);

    const QString settingsPath = configDir + QStringLiteral("/obstwitchplugin.ini");
    QSettings settings(settingsPath, QSettings::IniFormat);
    settings.beginGroup(kTokenSettingsGroup);
    settings.beginGroup(kCommandsSettingsGroup);
    settings.remove(kCommandsArraySettingsKey);
    settings.beginWriteArray(kCommandsArraySettingsKey);
    int index = 0;
    for (auto it = customCommands_.cbegin(); it != customCommands_.cend(); ++it, ++index) {
        settings.setArrayIndex(index);
        settings.setValue(kCommandTriggerSettingsKey, it.key());
        settings.setValue(kCommandResponseSettingsKey, it.value());
    }
    settings.endArray();
    settings.endGroup();
    settings.endGroup();
    settings.sync();
}

void TwitchDockWidget::loadPersistedCommands()
{
    customCommands_.clear();
    if (commandsTable_) {
        commandsTable_->setRowCount(0);
    }

    const QString configDir = QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation);
    const QString settingsPath = configDir + QStringLiteral("/obstwitchplugin.ini");
    QSettings settings(settingsPath, QSettings::IniFormat);
    settings.beginGroup(kTokenSettingsGroup);
    settings.beginGroup(kCommandsSettingsGroup);
    const int size = settings.beginReadArray(kCommandsArraySettingsKey);
    for (int index = 0; index < size; ++index) {
        settings.setArrayIndex(index);
        const QString trigger = settings.value(kCommandTriggerSettingsKey).toString().trimmed();
        const QString response = settings.value(kCommandResponseSettingsKey).toString();
        if (!trigger.isEmpty() && !response.isEmpty()) {
            customCommands_.insert(trigger, response);
        }
    }
    settings.endArray();
    settings.endGroup();
    settings.endGroup();

    if (!commandsTable_) {
        return;
    }

    for (auto it = customCommands_.cbegin(); it != customCommands_.cend(); ++it) {
        const int row = commandsTable_->rowCount();
        commandsTable_->insertRow(row);
        populateCommandRow(row, it.key(), it.value());
    }
}

void TwitchDockWidget::resolveIdentity(const QString &token, std::function<void(bool, const QSet<QString> &)> continuation)
{
    if (!broadcasterId_.isEmpty() && !twitchLogin_.isEmpty() && validatedToken_ == token) {
        continuation(true, validatedScopes_);
        return;
    }

    broadcasterId_.clear();
    twitchLogin_.clear();
    validatedToken_.clear();
    validatedScopes_.clear();

    QNetworkRequest request(QUrl(QStringLiteral("https://id.twitch.tv/oauth2/validate")));
    request.setRawHeader("Authorization", QByteArray("OAuth ") + token.toUtf8());

    QNetworkReply *reply = networkManager_->get(request);
    connect(reply, &QNetworkReply::finished, this, [this, reply, token, continuation = std::move(continuation)]() mutable {
        const QByteArray payloadBytes = reply->readAll();
        const QNetworkReply::NetworkError error = reply->error();
        const QString errorString = reply->errorString();
        reply->deleteLater();

        if (error != QNetworkReply::NoError) {
            appendChatSystemMessage(tr("Unable to resolve broadcaster identity: %1").arg(errorString));
            continuation(false, {});
            return;
        }

        const QJsonObject payload = QJsonDocument::fromJson(payloadBytes).object();
        const QString broadcasterId = payload.value(QStringLiteral("user_id")).toString().trimmed();
        const QString twitchLogin = payload.value(QStringLiteral("login")).toString().trimmed();
        QSet<QString> scopes;
        for (const QJsonValue &scopeValue : payload.value(QStringLiteral("scopes")).toArray()) {
            const QString scope = scopeValue.toString().trimmed();
            if (!scope.isEmpty()) {
                scopes.insert(scope);
            }
        }

        if (broadcasterId.isEmpty() || twitchLogin.isEmpty()) {
            broadcasterId_.clear();
            twitchLogin_.clear();
            validatedToken_.clear();
            validatedScopes_.clear();
            appendChatSystemMessage(tr("OAuth validation response did not include required identity fields."));
            continuation(false, {});
            return;
        }

        broadcasterId_ = broadcasterId;
        twitchLogin_ = twitchLogin;
        validatedToken_ = token;
        validatedScopes_ = scopes;
        continuation(true, scopes);
    });
}

void TwitchDockWidget::updateChannelInfo()
{
    const QString token = tokenEdit_->text().trimmed();
    const QString clientId = clientIdEdit_->text().trimmed();
    const QString title = titleEdit_->text().trimmed();
    const QString categoryName = gameIdEdit_->text().trimmed();

    if (!clientId.isEmpty()) {
        persistClientId(clientId);
    }

    if (token.isEmpty() || clientId.isEmpty()) {
        appendChatSystemMessage(tr("Cannot patch channel info: missing OAuth token or client ID."));
        return;
    }

    resolveIdentity(token, [this, token, clientId, title, categoryName](bool ok, const QSet<QString> &) {
        if (!ok) {
            return;
        }

        resolveCategoryId(token, clientId, categoryName, [this, token, clientId, title](const QString &resolvedGameId) {
            if (resolvedGameId.isNull()) {
                appendChatSystemMessage(tr("Category not found. Choose a category suggestion or use an exact category name."));
                return;
            }

            QUrl url(QStringLiteral("https://api.twitch.tv/helix/channels"));
            QUrlQuery query;
            query.addQueryItem(QStringLiteral("broadcaster_id"), broadcasterId_);
            url.setQuery(query);

            QNetworkRequest request(url);
            request.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
            request.setRawHeader("Authorization", QByteArray("Bearer ") + token.toUtf8());
            request.setRawHeader("Client-Id", clientId.toUtf8());

            QJsonObject body;
            if (!title.isEmpty()) {
                body.insert(QStringLiteral("title"), title);
            }
            if (!resolvedGameId.isEmpty()) {
                body.insert(QStringLiteral("game_id"), resolvedGameId);
            }

            QNetworkReply *reply = networkManager_->sendCustomRequest(
                request, QByteArrayLiteral("PATCH"), QJsonDocument(body).toJson(QJsonDocument::Compact));
            connect(reply, &QNetworkReply::finished, this, [this, reply]() {
                const QNetworkReply::NetworkError error = reply->error();
                const QString errorString = reply->errorString();
                reply->deleteLater();

                if (error == QNetworkReply::NoError) {
                    appendChatSystemMessage(tr("Twitch Helix channel update succeeded."));
                } else {
                    appendChatSystemMessage(tr("Twitch Helix channel update failed: %1").arg(errorString));
                }
            });
        });
    });
}

void TwitchDockWidget::connectChat()
{
    const QString token = tokenEdit_->text().trimmed();
    chatCanSendMessages_ = false;
    stopFollowerActivityPolling();
    if (token.isEmpty()) {
        appendChatSystemMessage(tr("Chat connection requires an OAuth token with chat:read scope and chat:edit for command responses."));
        return;
    }

    resolveIdentity(token, [this, token](bool ok, const QSet<QString> &scopes) {
        if (!ok) {
            return;
        }

        if (!scopes.contains(QStringLiteral("chat:read"))) {
            appendChatSystemMessage(tr("OAuth token is missing chat:read scope. Click Authorize in Browser to reconnect chat."));
            return;
        }
        chatCanSendMessages_ = scopes.contains(QStringLiteral("chat:edit"));
        if (!scopes.contains(QStringLiteral("chat:edit"))) {
            appendChatSystemMessage(tr("OAuth token is missing chat:edit scope, so command responses cannot be sent until you re-authorize."));
        }

        const QString configuredChannel = sanitizeChannelLogin(channelEdit_->text());
        const QString channel = configuredChannel.isEmpty() ? twitchLogin_ : configuredChannel;
        channelEdit_->setText(channel);
        persistChatChannel(channel);

        if (chatSocket_->state() != QAbstractSocket::UnconnectedState) {
            chatSocket_->abort();
        }

        chatSocket_->connectToHost(QStringLiteral("irc.chat.twitch.tv"), 6667);
        if (!chatSocket_->waitForConnected(5000)) {
            appendChatSystemMessage(tr("Could not connect to Twitch IRC: %1").arg(chatSocket_->errorString()));
            return;
        }

        // Authenticate and join channel according to Twitch IRC protocol.
        chatSocket_->write("CAP REQ :twitch.tv/tags twitch.tv/commands\r\n");
        chatSocket_->write("PASS " + buildIrcPass(token) + "\r\n");
        chatSocket_->write("NICK " + twitchLogin_.toUtf8() + "\r\n");
        chatSocket_->write("JOIN #" + channel.toUtf8() + "\r\n");

        appendChatSystemMessage(tr("Connected to Twitch IRC and joined #%1.").arg(channel));

        const QString clientId = clientIdEdit_->text().trimmed();
        if (scopes.contains(QStringLiteral("moderator:read:followers")) && !clientId.isEmpty()) {
            followerMissingScopeWarningShown_ = false;
            followerMissingClientIdWarningShown_ = false;
            startFollowerActivityPolling(token, clientId);
        } else if (scopes.contains(QStringLiteral("moderator:read:followers"))) {
            if (!followerMissingClientIdWarningShown_) {
                appendChatSystemMessage(tr("Follower activity requires a Twitch Client ID in the authorization settings."));
                followerMissingClientIdWarningShown_ = true;
            }
        } else {
            if (!followerMissingScopeWarningShown_) {
                appendChatSystemMessage(
                    tr("OAuth token is missing moderator:read:followers scope, so follow activity will not appear until you re-authorize."));
                followerMissingScopeWarningShown_ = true;
            }
        }
    });
}

void TwitchDockWidget::sendManualMessage()
{
    if (!messageEdit_) {
        return;
    }

    const QString message = messageEdit_->text();
    if (sendChatMessage(message)) {
        messageEdit_->clear();
    }
}

void TwitchDockWidget::onChatSocketReadyRead()
{
    // Append every inbound IRC line and respond to keepalive PING frames.
    while (chatSocket_->canReadLine()) {
        const QByteArray line = chatSocket_->readLine().trimmed();
        if (line.startsWith("PING")) {
            chatSocket_->write("PONG :tmi.twitch.tv\r\n");
            continue;
        }
        appendFormattedChatLine(line);
    }
}

void TwitchDockWidget::appendChatSystemMessage(const QString &message)
{
    chatText_->moveCursor(QTextCursor::End);
    QTextCursor cursor = chatText_->textCursor();
    ensureChatEntryStartsOnNewLine(cursor);
    cursor.insertHtml(QStringLiteral("<span style='color:#adadb8;'>[system]</span> <span style='color:#d3d3da;'>%1</span>")
                          .arg(message.toHtmlEscaped()));
    cursor.insertBlock();
    chatText_->setTextCursor(cursor);
}

void TwitchDockWidget::appendChatActivityMessage(const QString &timestamp, const QString &message)
{
    chatText_->moveCursor(QTextCursor::End);
    QTextCursor cursor = chatText_->textCursor();
    ensureChatEntryStartsOnNewLine(cursor);
    cursor.insertHtml(QStringLiteral("<span style='color:#8f8fa3;'>%1</span> <span style='color:#00d084;'>[activity]</span> "
                                     "<span style='color:#efeff1;'>%2</span>")
                          .arg(timestamp.toHtmlEscaped(), message.toHtmlEscaped()));
    cursor.insertBlock();
    chatText_->setTextCursor(cursor);
}

void TwitchDockWidget::appendFormattedChatLine(const QByteArray &ircLine)
{
    const QString line = QString::fromUtf8(ircLine);
    const QString timestamp = QTime::currentTime().toString(QStringLiteral("HH:mm"));

    static const QRegularExpression messagePattern(
        QStringLiteral("^@([^\\s]+)\\s+:([^!\\s]+)![^\\s]+\\s+PRIVMSG\\s+#[^\\s]+\\s+:(.*)$"));
    const QRegularExpressionMatch messageMatch = messagePattern.match(line);
    if (messageMatch.hasMatch()) {
        const QString tags = messageMatch.captured(1);
        const QString senderLogin = messageMatch.captured(2);
        const QString message = messageMatch.captured(3);
        const QString displayName = ircTagValue(tags, QStringLiteral("display-name"));
        const QString username = displayName.isEmpty() ? QStringLiteral("user") : displayName;
        const QString color = sanitizeChatColor(ircTagValue(tags, QStringLiteral("color")));
        const bool isOwnMessage = !twitchLogin_.isEmpty() && senderLogin.compare(twitchLogin_, Qt::CaseInsensitive) == 0;
        std::optional<int> localEchoCommandDepth;
        if (isOwnMessage) {
            const QString trimmedMessage = message.trimmed();
            const auto echoedMessageIt = std::find_if(
                pendingLocalChatEchoes_.begin(), pendingLocalChatEchoes_.end(),
                [&trimmedMessage](const LocalChatEcho &echo) { return echo.message == trimmedMessage; });
            if (echoedMessageIt != pendingLocalChatEchoes_.end()) {
                localEchoCommandDepth = echoedMessageIt->commandDepth;
                pendingLocalChatEchoes_.erase(echoedMessageIt);
            }
        }
        const QString commandResponse = commandResponseForMessage(message);
        if (localEchoCommandDepth.has_value()) {
            if (commandResponse.isEmpty()) {
                return;
            }
            if (*localEchoCommandDepth >= kMaxCommandChainDepth) {
                appendChatSystemMessage(
                    tr("Skipping nested command response because the command chain limit was reached."));
                return;
            }
            sendChatMessage(commandResponse, *localEchoCommandDepth + 1);
            return;
        }
        enqueueChatMessage(timestamp,
                           username,
                           color,
                           message,
                           parseIrcEmotes(ircTagValue(tags, QStringLiteral("emotes"))),
                           commandResponse,
                           0);
        return;
    }

    static const QRegularExpression userNoticePattern(
        QStringLiteral("^@([^\\s]+)\\s+:tmi\\.twitch\\.tv\\s+USERNOTICE\\s+#[^\\s]+(?:\\s+:(.*))?$"));
    const QRegularExpressionMatch userNoticeMatch = userNoticePattern.match(line);
    if (userNoticeMatch.hasMatch()) {
        const QString tags = userNoticeMatch.captured(1);
        const QString msgId = ircTagValue(tags, QStringLiteral("msg-id"));
        const QSet<QString> supportedNoticeIds = {
            QStringLiteral("sub"),          QStringLiteral("resub"),        QStringLiteral("subgift"),
            QStringLiteral("anonsubgift"),  QStringLiteral("submysterygift"), QStringLiteral("anonsubmysterygift"),
            QStringLiteral("giftpaidupgrade"), QStringLiteral("anongiftpaidupgrade"),
            QStringLiteral("primepaidupgrade"), QStringLiteral("raid"),
        };
        if (supportedNoticeIds.contains(msgId)) {
            QString activity = ircTagValue(tags, QStringLiteral("system-msg")).trimmed();
            if (activity.isEmpty()) {
                const QString actor = ircTagValue(tags, QStringLiteral("display-name"));
                const QString recipient = ircTagValue(tags, QStringLiteral("msg-param-recipient-display-name"));
                const QString raidViewerCount = ircTagValue(tags, QStringLiteral("msg-param-viewerCount"));
                if (msgId == QStringLiteral("sub") || msgId == QStringLiteral("resub") ||
                    msgId == QStringLiteral("primepaidupgrade")) {
                    activity = tr("%1 subscribed.").arg(actor.isEmpty() ? tr("A viewer") : actor);
                } else if (msgId == QStringLiteral("subgift") || msgId == QStringLiteral("anonsubgift")) {
                    activity =
                        tr("%1 gifted a subscription to %2.")
                            .arg(actor.isEmpty() ? tr("A viewer") : actor, recipient.isEmpty() ? tr("another viewer") : recipient);
                } else if (msgId == QStringLiteral("submysterygift") || msgId == QStringLiteral("anonsubmysterygift")) {
                    activity = tr("%1 gifted community subscriptions.").arg(actor.isEmpty() ? tr("A viewer") : actor);
                } else if (msgId == QStringLiteral("giftpaidupgrade") || msgId == QStringLiteral("anongiftpaidupgrade")) {
                    const QString continuingViewer = ircTagValue(tags, QStringLiteral("msg-param-sender-name"));
                    const QString continuingViewerLogin = ircTagValue(tags, QStringLiteral("msg-param-sender-login"));
                    activity =
                        tr("%1 continued a gifted subscription.")
                            .arg(!continuingViewer.isEmpty() ? continuingViewer
                                                             : (continuingViewerLogin.isEmpty() ? tr("A viewer")
                                                                                                : continuingViewerLogin));
                } else if (msgId == QStringLiteral("raid")) {
                    activity = tr("%1 is raiding with a party of %2.")
                                   .arg(actor.isEmpty() ? tr("A channel") : actor,
                                        raidViewerCount.isEmpty() ? tr("unknown size") : raidViewerCount);
                }
            }

            const QString userMessage = userNoticeMatch.captured(2).trimmed();
            if (!userMessage.isEmpty()) {
                activity += tr(" — %1").arg(userMessage);
            }
            if (!activity.isEmpty()) {
                appendChatActivityMessage(timestamp, activity);
            }
            return;
        }
    }

    static const QRegularExpression noticePattern(QStringLiteral("NOTICE\\s+#[^\\s]+\\s+:(.*)$"));
    const QRegularExpressionMatch noticeMatch = noticePattern.match(line);
    if (noticeMatch.hasMatch()) {
        chatText_->moveCursor(QTextCursor::End);
        QTextCursor cursor = chatText_->textCursor();
        ensureChatEntryStartsOnNewLine(cursor);
        cursor.insertHtml(QStringLiteral("<span style='color:#8f8fa3;'>%1</span> <span style='color:#f7c843;'>[notice]</span> "
                                         "<span style='color:#efeff1;'>%2</span>")
                              .arg(timestamp, noticeMatch.captured(1).toHtmlEscaped()));
        cursor.insertBlock();
        chatText_->setTextCursor(cursor);
        return;
    }

    const QString command = ircCommand(line);
    if (command == QStringLiteral("JOIN") || command == QStringLiteral("PART") || command == QStringLiteral("CAP") ||
        command == QStringLiteral("GLOBALUSERSTATE") || command == QStringLiteral("USERSTATE") ||
        command == QStringLiteral("ROOMSTATE") || command == QStringLiteral("353") || command == QStringLiteral("366") ||
        command == QStringLiteral("001") || command == QStringLiteral("002") || command == QStringLiteral("003") ||
        command == QStringLiteral("004") || command == QStringLiteral("372") || command == QStringLiteral("375") ||
        command == QStringLiteral("376")) {
        return;
    }
}

QString TwitchDockWidget::commandResponseForMessage(const QString &message) const
{
    const QString trigger = message.trimmed();
    if (trigger.isEmpty()) {
        return {};
    }

    return customCommands_.value(trigger);
}

QList<TwitchDockWidget::ChatEmoteOccurrence> TwitchDockWidget::parseIrcEmotes(const QString &emotesTag) const
{
    QList<ChatEmoteOccurrence> emotes;
    if (emotesTag.isEmpty()) {
        return emotes;
    }

    const QStringList emoteGroups = emotesTag.split(QLatin1Char('/'), Qt::SkipEmptyParts);
    for (const QString &emoteGroup : emoteGroups) {
        const int separatorIndex = emoteGroup.indexOf(QLatin1Char(':'));
        if (separatorIndex <= 0) {
            continue;
        }

        const QString emoteId = emoteGroup.left(separatorIndex);
        const QStringList ranges = emoteGroup.mid(separatorIndex + 1).split(QLatin1Char(','), Qt::SkipEmptyParts);
        for (const QString &range : ranges) {
            const QStringList bounds = range.split(QLatin1Char('-'));
            if (bounds.size() != 2) {
                continue;
            }

            bool startOk = false;
            bool endOk = false;
            const int start = bounds.at(0).toInt(&startOk);
            const int end = bounds.at(1).toInt(&endOk);
            if (!startOk || !endOk || start < 0 || end < start) {
                continue;
            }

            emotes.append({emoteId, start, end});
        }
    }

    std::sort(emotes.begin(), emotes.end(), [](const ChatEmoteOccurrence &lhs, const ChatEmoteOccurrence &rhs) {
        if (lhs.start == rhs.start) {
            return lhs.end < rhs.end;
        }
        return lhs.start < rhs.start;
    });
    return emotes;
}

void TwitchDockWidget::enqueueChatMessage(const QString &timestamp,
                                          const QString &username,
                                          const QString &color,
                                          const QString &message,
                                          const QList<ChatEmoteOccurrence> &emotes,
                                          const QString &commandResponse,
                                          int commandDepth)
{
    PendingChatMessage pendingMessage;
    pendingMessage.timestamp = timestamp;
    pendingMessage.username = username;
    pendingMessage.color = color;
    pendingMessage.message = message;
    pendingMessage.commandResponse = commandResponse;
    pendingMessage.emotes = emotes;
    pendingMessage.commandDepth = commandDepth;
    pendingMessage.enqueuedAtMs = QDateTime::currentMSecsSinceEpoch();

    for (const ChatEmoteOccurrence &emote : emotes) {
        pendingMessage.requiredEmoteIds.insert(emote.id);
        if (!isEmoteAvailable(emote.id) && !pendingEmoteIds_.contains(emote.id)) {
            requestEmoteImage(emote.id);
        }
    }

    pendingChatMessages_.append(pendingMessage);
    flushPendingChatMessages();
}

void TwitchDockWidget::flushPendingChatMessages()
{
    while (!pendingChatMessages_.isEmpty()) {
        const PendingChatMessage &message = pendingChatMessages_.front();
        bool ready = true;
        for (const QString &emoteId : message.requiredEmoteIds) {
            if (!isEmoteAvailable(emoteId)) {
                ready = false;
                break;
            }
        }

        if (!ready) {
            const qint64 waitMs = QDateTime::currentMSecsSinceEpoch() - message.enqueuedAtMs;
            if (waitMs < kPendingEmoteWaitTimeoutMs) {
                return;
            }
        }

        renderChatMessage(message);
        if (!message.commandResponse.isEmpty()) {
            if (message.commandDepth >= kMaxCommandChainDepth) {
                appendChatSystemMessage(
                    tr("Skipping nested command response because the command chain limit was reached."));
            } else {
                sendChatMessage(message.commandResponse, message.commandDepth + 1);
            }
        }
        pendingChatMessages_.removeFirst();
    }
}

void TwitchDockWidget::renderChatMessage(const PendingChatMessage &message)
{
    chatText_->moveCursor(QTextCursor::End);
    QTextCursor cursor = chatText_->textCursor();
    ensureChatEntryStartsOnNewLine(cursor);
    cursor.insertHtml(QStringLiteral("<span style='color:#8f8fa3;'>%1</span> <span style='color:%2;'>%3</span>"
                                     "<span style='color:#efeff1;'>: </span>")
                          .arg(message.timestamp.toHtmlEscaped(), message.color, message.username.toHtmlEscaped()));

    QTextCharFormat messageFormat = cursor.charFormat();
    messageFormat.setForeground(QColor(QString::fromLatin1(kChatMessageTextColor)));
    cursor.setCharFormat(messageFormat);

    int currentIndex = 0;
    for (const ChatEmoteOccurrence &emote : message.emotes) {
        if (emote.start < currentIndex || emote.start >= message.message.size()) {
            continue;
        }

        const int boundedEnd = std::min(emote.end, static_cast<int>(message.message.size()) - 1);
        if (boundedEnd < emote.start) {
            continue;
        }

        cursor.insertText(message.message.mid(currentIndex, emote.start - currentIndex));

        if (emoteImages_.contains(emote.id)) {
            QTextImageFormat imageFormat;
            imageFormat.setName(emoteResourceUrl(emote.id).toString());
            imageFormat.setWidth(28);
            imageFormat.setHeight(28);
            imageFormat.setVerticalAlignment(QTextCharFormat::AlignMiddle);
            cursor.insertImage(imageFormat);
        } else {
            cursor.insertText(message.message.mid(emote.start, boundedEnd - emote.start + 1));
        }

        currentIndex = boundedEnd + 1;
    }

    if (currentIndex < message.message.size()) {
        cursor.insertText(message.message.mid(currentIndex));
    }
    cursor.insertBlock();
    chatText_->setTextCursor(cursor);
}

void TwitchDockWidget::appendLocalOutgoingChatMessage(const QString &message, int commandDepth)
{
    const QString trimmedMessage = message.trimmed();
    if (trimmedMessage.isEmpty()) {
        return;
    }

    pendingLocalChatEchoes_.append({trimmedMessage, commandDepth});
    while (pendingLocalChatEchoes_.size() > 50) {
        pendingLocalChatEchoes_.removeFirst();
    }
    chatText_->moveCursor(QTextCursor::End);
    QTextCursor cursor = chatText_->textCursor();
    ensureChatEntryStartsOnNewLine(cursor);
    cursor.insertHtml(QStringLiteral("<span style='color:#8f8fa3;'>%1</span> <span style='color:%2;'>%3</span>"
                                     "<span style='color:#efeff1;'>: </span>")
                          .arg(QTime::currentTime().toString(QStringLiteral("HH:mm")).toHtmlEscaped(),
                               QString::fromLatin1(kDefaultChatColor),
                               (twitchLogin_.isEmpty() ? tr("You") : twitchLogin_).toHtmlEscaped()));
    QTextCharFormat messageFormat = cursor.charFormat();
    messageFormat.setForeground(QColor(QString::fromLatin1(kChatMessageTextColor)));
    cursor.setCharFormat(messageFormat);
    cursor.insertText(trimmedMessage);
    cursor.insertBlock();
    chatText_->setTextCursor(cursor);
}

bool TwitchDockWidget::sendChatMessage(const QString &message, int commandDepth)
{
    if (chatSocket_->state() != QAbstractSocket::ConnectedState) {
        appendChatSystemMessage(tr("Cannot send chat message: chat is not connected."));
        return false;
    }

    if (!chatCanSendMessages_) {
        appendChatSystemMessage(tr("Cannot send chat message: OAuth token is missing chat:edit scope."));
        return false;
    }

    const QString channel = sanitizeChannelLogin(channelEdit_->text());
    if (channel.isEmpty()) {
        appendChatSystemMessage(tr("Cannot send chat message: channel is not configured."));
        return false;
    }

    QString outbound = message;
    outbound.replace(QLatin1Char('\r'), QLatin1Char(' '));
    outbound.replace(QLatin1Char('\n'), QLatin1Char(' '));
    outbound = outbound.trimmed();
    if (outbound.isEmpty()) {
        return false;
    }
    if (outbound.size() > kMaxTwitchChatMessageLength) {
        appendChatSystemMessage(
            tr("Cannot send chat message: Twitch messages are limited to %1 characters.")
                .arg(kMaxTwitchChatMessageLength));
        return false;
    }

    const qint64 written =
        chatSocket_->write("PRIVMSG #" + channel.toUtf8() + " :" + outbound.toUtf8() + "\r\n");
    if (written < 0) {
        appendChatSystemMessage(tr("Failed to send chat message: %1").arg(chatSocket_->errorString()));
        return false;
    }

    appendLocalOutgoingChatMessage(outbound, commandDepth);
    return true;
}

void TwitchDockWidget::requestEmoteImage(const QString &emoteId)
{
    pendingEmoteIds_.insert(emoteId);

    QNetworkReply *reply = networkManager_->get(QNetworkRequest(twitchEmoteUrl(emoteId)));
    connect(reply, &QNetworkReply::finished, this, [this, reply, emoteId]() {
        const QByteArray payload = reply->readAll();
        const QNetworkReply::NetworkError error = reply->error();
        reply->deleteLater();
        pendingEmoteIds_.remove(emoteId);

        if (error == QNetworkReply::NoError) {
            QImage image;
            if (image.loadFromData(payload)) {
                emoteImages_.insert(emoteId, image);
                chatText_->document()->addResource(QTextDocument::ImageResource, emoteResourceUrl(emoteId), image);
                flushPendingChatMessages();
                return;
            }
        }

        unavailableEmoteIds_.insert(emoteId);
        flushPendingChatMessages();
    });
}

bool TwitchDockWidget::isEmoteAvailable(const QString &emoteId) const
{
    return emoteImages_.contains(emoteId) || unavailableEmoteIds_.contains(emoteId);
}

QByteArray TwitchDockWidget::buildIrcPass(const QString &oauthToken) const
{
    QByteArray token = oauthToken.trimmed().toUtf8();
    if (!token.startsWith("oauth:")) {
        token.prepend("oauth:");
    }
    return token;
}

void TwitchDockWidget::addFriendLink()
{
    const QUrl candidate(friendLinkInput_->text().trimmed());
    if (!candidate.isValid() || candidate.scheme().isEmpty()) {
        appendChatSystemMessage(tr("Friend streaming link is invalid."));
        return;
    }

    friendLinks_->addItem(candidate.toString());
    friendLinkInput_->clear();
}

void TwitchDockWidget::removeSelectedFriendLink()
{
    delete friendLinks_->takeItem(friendLinks_->currentRow());
}

void TwitchDockWidget::openSelectedFriendLink()
{
    const QListWidgetItem *item = friendLinks_->currentItem();
    if (!item) {
        return;
    }

    QDesktopServices::openUrl(QUrl(item->text()));
}

void TwitchDockWidget::fetchCurrentChannelInfo()
{
    const QString token = tokenEdit_->text().trimmed();
    const QString clientId = clientIdEdit_->text().trimmed();
    if (token.isEmpty() || clientId.isEmpty()) {
        return;
    }

    resolveIdentity(token, [this, token, clientId](bool ok, const QSet<QString> &) {
        if (!ok) {
            return;
        }

        QUrl url(QStringLiteral("https://api.twitch.tv/helix/channels"));
        QUrlQuery query;
        query.addQueryItem(QStringLiteral("broadcaster_id"), broadcasterId_);
        url.setQuery(query);

        QNetworkRequest request(url);
        request.setRawHeader("Authorization", QByteArray("Bearer ") + token.toUtf8());
        request.setRawHeader("Client-Id", clientId.toUtf8());

        QNetworkReply *reply = networkManager_->get(request);
        connect(reply, &QNetworkReply::finished, this, [this, reply]() {
            const QByteArray payloadBytes = reply->readAll();
            const QNetworkReply::NetworkError error = reply->error();
            const QString errorString = reply->errorString();
            reply->deleteLater();

            if (error != QNetworkReply::NoError) {
                appendChatSystemMessage(tr("Unable to fetch current stream title/game: %1").arg(errorString));
                return;
            }

            const QJsonObject payload = QJsonDocument::fromJson(payloadBytes).object();
            const QJsonArray data = payload.value(QStringLiteral("data")).toArray();
            if (data.isEmpty()) {
                appendChatSystemMessage(tr("No current channel info returned from Twitch API."));
                return;
            }

            const QJsonObject channel = data.first().toObject();
            titleEdit_->setText(channel.value(QStringLiteral("title")).toString().trimmed());
            gameIdEdit_->setText(channel.value(QStringLiteral("game_name")).toString().trimmed());
            const QString gameName = channel.value(QStringLiteral("game_name")).toString().trimmed();
            appendChatSystemMessage(
                tr("Loaded current stream info from Twitch (title and game%1).")
                    .arg(gameName.isEmpty() ? QString() : QStringLiteral(": %1").arg(gameName)));
        });
    });
}

void TwitchDockWidget::startFollowerActivityPolling(const QString &token, const QString &clientId)
{
    if (followerPollTimer_) {
        followerPollTimer_->stop();
    }
    followerPollToken_ = token.trimmed();
    followerPollClientId_ = clientId.trimmed();
    knownFollowerIds_.clear();
    newestKnownFollowerAt_ = {};
    followerSnapshotInitialized_ = false;
    ++followerPollSessionId_;
    followerPollRequestSessionId_ = 0;
    pollLatestFollowers(true);
}

void TwitchDockWidget::stopFollowerActivityPolling()
{
    if (followerPollTimer_) {
        followerPollTimer_->stop();
    }
    ++followerPollSessionId_;
    followerPollRequestSessionId_ = 0;
    followerPollToken_.clear();
    followerPollClientId_.clear();
    knownFollowerIds_.clear();
    newestKnownFollowerAt_ = {};
    followerSnapshotInitialized_ = false;
}

void TwitchDockWidget::pollLatestFollowers(bool initializeSnapshot)
{
    if (followerPollToken_.isEmpty() || followerPollClientId_.isEmpty() || broadcasterId_.isEmpty() ||
        followerPollRequestSessionId_ == followerPollSessionId_) {
        return;
    }

    const quint64 sessionId = followerPollSessionId_;
    followerPollRequestSessionId_ = sessionId;

    auto fetchPage = std::make_shared<std::function<void(const QString &,
                                                         QList<FollowerActivityRecord>,
                                                         QDateTime,
                                                         QSet<QString>)>>();
    *fetchPage = [this, initializeSnapshot, sessionId, fetchPage](const QString &afterCursor,
                                                                  QList<FollowerActivityRecord> collectedFollowers,
                                                                  QDateTime newestSeenAt,
                                                                  QSet<QString> newestSeenIds) mutable {
        QUrl url(QStringLiteral("https://api.twitch.tv/helix/channels/followers"));
        QUrlQuery query;
        query.addQueryItem(QStringLiteral("broadcaster_id"), broadcasterId_);
        query.addQueryItem(QStringLiteral("moderator_id"), broadcasterId_);
        query.addQueryItem(QStringLiteral("first"), QStringLiteral("100"));
        if (!afterCursor.isEmpty()) {
            query.addQueryItem(QStringLiteral("after"), afterCursor);
        }
        url.setQuery(query);

        QNetworkRequest request(url);
        request.setRawHeader("Authorization", QByteArray("Bearer ") + followerPollToken_.toUtf8());
        request.setRawHeader("Client-Id", followerPollClientId_.toUtf8());

        QNetworkReply *reply = networkManager_->get(request);
        connect(reply,
                &QNetworkReply::finished,
                this,
                [this, reply, initializeSnapshot, sessionId, fetchPage, collectedFollowers = std::move(collectedFollowers),
                 newestSeenAt, newestSeenIds = std::move(newestSeenIds)]() mutable {
                    const QByteArray payloadBytes = reply->readAll();
                    const QNetworkReply::NetworkError error = reply->error();
                    const QString errorString = reply->errorString();
                    reply->deleteLater();

                    if (sessionId != followerPollSessionId_) {
                        if (followerPollRequestSessionId_ == sessionId) {
                            followerPollRequestSessionId_ = 0;
                        }
                        return;
                    }

                    if (error != QNetworkReply::NoError) {
                        if (followerPollRequestSessionId_ == sessionId) {
                            followerPollRequestSessionId_ = 0;
                        }
                        if (initializeSnapshot && followerPollTimer_) {
                            followerPollTimer_->start();
                        }
                        if (initializeSnapshot) {
                            appendChatSystemMessage(tr("Unable to load follow activity for chat: %1").arg(errorString));
                        }
                        return;
                    }

                    const QJsonObject payload = QJsonDocument::fromJson(payloadBytes).object();
                    const QJsonArray data = payload.value(QStringLiteral("data")).toArray();
                    QDateTime oldestSeenAtInPage;
                    for (const QJsonValue &entryValue : data) {
                        const QJsonObject entry = entryValue.toObject();
                        const QString followerId = entry.value(QStringLiteral("user_id")).toString().trimmed();
                        if (followerId.isEmpty()) {
                            continue;
                        }

                        FollowerActivityRecord follower;
                        follower.id = followerId;
                        follower.displayName = entry.value(QStringLiteral("user_name")).toString().trimmed();
                        if (follower.displayName.isEmpty()) {
                            follower.displayName = entry.value(QStringLiteral("user_login")).toString().trimmed();
                        }
                        follower.followedAt =
                            QDateTime::fromString(entry.value(QStringLiteral("followed_at")).toString().trimmed(), Qt::ISODate);

                        if (!newestSeenAt.isValid() || (follower.followedAt.isValid() && follower.followedAt > newestSeenAt)) {
                            newestSeenAt = follower.followedAt;
                            newestSeenIds.clear();
                            newestSeenIds.insert(follower.id);
                        } else if (follower.followedAt.isValid() && follower.followedAt == newestSeenAt) {
                            newestSeenIds.insert(follower.id);
                        }

                        if (follower.followedAt.isValid()) {
                            oldestSeenAtInPage = follower.followedAt;
                        }

                        const bool isNewFollower =
                            followerSnapshotInitialized_ &&
                            (!newestKnownFollowerAt_.isValid() || follower.followedAt > newestKnownFollowerAt_ ||
                             (follower.followedAt.isValid() && follower.followedAt == newestKnownFollowerAt_ &&
                              !knownFollowerIds_.contains(follower.id)));
                        if (isNewFollower) {
                            collectedFollowers.append(follower);
                        }
                    }

                    const QString nextCursor =
                        payload.value(QStringLiteral("pagination")).toObject().value(QStringLiteral("cursor")).toString().trimmed();
                    const bool shouldFetchNextPage =
                        !initializeSnapshot && !nextCursor.isEmpty() &&
                        (!newestKnownFollowerAt_.isValid() ||
                         (oldestSeenAtInPage.isValid() && oldestSeenAtInPage >= newestKnownFollowerAt_));
                    if (shouldFetchNextPage) {
                        (*fetchPage)(nextCursor,
                                     std::move(collectedFollowers),
                                     newestSeenAt,
                                     std::move(newestSeenIds));
                        return;
                    }

                    if (followerPollRequestSessionId_ == sessionId) {
                        followerPollRequestSessionId_ = 0;
                    }
                    if (initializeSnapshot && followerPollTimer_) {
                        followerPollTimer_->start();
                    }

                    if (followerSnapshotInitialized_) {
                        std::sort(collectedFollowers.begin(),
                                  collectedFollowers.end(),
                                  [](const FollowerActivityRecord &lhs, const FollowerActivityRecord &rhs) {
                                      return lhs.followedAt < rhs.followedAt;
                                  });
                        for (const FollowerActivityRecord &follower : collectedFollowers) {
                            const QString timestamp = follower.followedAt.isValid()
                                                          ? follower.followedAt.toLocalTime().toString(QStringLiteral("HH:mm"))
                                                          : QTime::currentTime().toString(QStringLiteral("HH:mm"));
                            appendChatActivityMessage(
                                timestamp,
                                tr("%1 followed the channel.").arg(follower.displayName.isEmpty() ? tr("A viewer")
                                                                                                   : follower.displayName));
                        }
                    }

                    if (newestSeenAt.isValid()) {
                        newestKnownFollowerAt_ = newestSeenAt;
                        knownFollowerIds_ = newestSeenIds;
                    }
                    followerSnapshotInitialized_ = true;
                });
    };

    (*fetchPage)({}, {}, {}, {});
}

void TwitchDockWidget::fetchCategorySuggestions(const QString &query)
{
    const QString searchQuery = query.trimmed();
    const QString token = tokenEdit_->text().trimmed();
    const QString clientId = clientIdEdit_->text().trimmed();
    if (searchQuery.isEmpty() || token.isEmpty() || clientId.isEmpty()) {
        gameCategorySuggestionsModel_->setStringList({});
        return;
    }

    if (categorySuggestReply_) {
        categorySuggestReply_->abort();
        categorySuggestReply_ = nullptr;
    }

    QUrl url(QStringLiteral("https://api.twitch.tv/helix/search/categories"));
    QUrlQuery urlQuery;
    urlQuery.addQueryItem(QStringLiteral("query"), searchQuery);
    urlQuery.addQueryItem(QStringLiteral("first"), QString::number(kCategorySuggestionLimit));
    url.setQuery(urlQuery);

    QNetworkRequest request(url);
    request.setRawHeader("Authorization", QByteArray("Bearer ") + token.toUtf8());
    request.setRawHeader("Client-Id", clientId.toUtf8());

    QNetworkReply *reply = networkManager_->get(request);
    categorySuggestReply_ = reply;
    connect(reply, &QNetworkReply::finished, this, [this, reply, expectedQuery = searchQuery]() {
        if (reply == categorySuggestReply_) {
            categorySuggestReply_ = nullptr;
        }

        const QByteArray payloadBytes = reply->readAll();
        const QNetworkReply::NetworkError error = reply->error();
        reply->deleteLater();

        if (error != QNetworkReply::NoError || expectedQuery != gameIdEdit_->text().trimmed()) {
            return;
        }

        const QJsonObject payload = QJsonDocument::fromJson(payloadBytes).object();
        const QJsonArray data = payload.value(QStringLiteral("data")).toArray();
        QStringList categories;
        categories.reserve(data.size());
        for (const QJsonValue &entry : data) {
            const QString name = entry.toObject().value(QStringLiteral("name")).toString().trimmed();
            if (!name.isEmpty() && !categories.contains(name)) {
                categories.append(name);
            }
        }
        gameCategorySuggestionsModel_->setStringList(categories);
    });
}

void TwitchDockWidget::resolveCategoryId(const QString &token,
                                         const QString &clientId,
                                         const QString &categoryName,
                                         std::function<void(const QString &)> continuation)
{
    const QString trimmedName = categoryName.trimmed();
    if (trimmedName.isEmpty()) {
        continuation(QStringLiteral(""));
        return;
    }

    QUrl url(QStringLiteral("https://api.twitch.tv/helix/search/categories"));
    QUrlQuery query;
    query.addQueryItem(QStringLiteral("query"), trimmedName);
    query.addQueryItem(QStringLiteral("first"), QStringLiteral("25"));
    url.setQuery(query);

    QNetworkRequest request(url);
    request.setRawHeader("Authorization", QByteArray("Bearer ") + token.toUtf8());
    request.setRawHeader("Client-Id", clientId.toUtf8());

    QNetworkReply *reply = networkManager_->get(request);
    connect(reply, &QNetworkReply::finished, this, [reply, trimmedName, continuation = std::move(continuation)]() mutable {
        const QByteArray payloadBytes = reply->readAll();
        const QNetworkReply::NetworkError error = reply->error();
        reply->deleteLater();

        if (error != QNetworkReply::NoError) {
            continuation(QString());
            return;
        }

        const QJsonObject payload = QJsonDocument::fromJson(payloadBytes).object();
        const QJsonArray data = payload.value(QStringLiteral("data")).toArray();
        for (const QJsonValue &entry : data) {
            const QJsonObject category = entry.toObject();
            if (category.value(QStringLiteral("name")).toString().trimmed().compare(trimmedName, Qt::CaseInsensitive) == 0) {
                continuation(category.value(QStringLiteral("id")).toString().trimmed());
                return;
            }
        }
        continuation(QString());
    });
}
