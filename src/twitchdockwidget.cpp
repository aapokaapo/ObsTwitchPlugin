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
#include <QPushButton>
#include <QPointer>
#include <QRegularExpression>
#include <QSettings>
#include <QStandardPaths>
#include <QTextDocument>
#include <QTimer>
#include <QTime>
#include <QUrl>
#include <QUrlQuery>
#include <QVBoxLayout>

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
constexpr auto kTokenSettingsKey = "twitch_oauth_token";
constexpr auto kChatChannelSettingsKey = "chat_channel";
constexpr auto kClientIdSettingsKey = "twitch_client_id";
constexpr quint16 kOAuthRedirectPort = 38471;

QString oauthRedirectUrl()
{
    return QStringLiteral("http://localhost:%1").arg(kOAuthRedirectPort);
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
        return part.mid(prefix.size());
    }
    return {};
}
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

    loadPersistedUiState();
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
}

void TwitchDockWidget::buildUi()
{
    auto *layout = new QVBoxLayout(this);
    tabs_ = new QTabWidget(this);

    // Chat tab: a stylized text timeline plus one-click IRC join.
    auto *chatTab = new QWidget(this);
    auto *chatLayout = new QVBoxLayout(chatTab);
    chatText_ = new QTextEdit(chatTab);
    chatText_->setReadOnly(true);
    chatText_->document()->setMaximumBlockCount(2000);
    chatText_->setStyleSheet("QTextEdit { background-color: #0e0e10; color: #efeff1; border: 1px solid #2f3545; font-family: Inter, Segoe UI, sans-serif; font-size: 13px; }");
    chatText_->setHtml(QStringLiteral("<span style='color:#adadb8;'>Twitch chat panel ready. Provide OAuth token, then join channel via IRC.</span>"));

    auto *chatRow = new QHBoxLayout();
    channelEdit_ = new QLineEdit(chatTab);
    channelEdit_->setPlaceholderText(tr("Channel login (e.g. yourname)"));
    connect(channelEdit_, &QLineEdit::editingFinished, this, [this]() {
        persistChatChannel(sanitizeChannelLogin(channelEdit_->text()));
    });
    auto *chatConnectButton = new QPushButton(tr("Connect Chat"), chatTab);
    chatRow->addWidget(channelEdit_);
    chatRow->addWidget(chatConnectButton);

    connect(chatConnectButton, &QPushButton::clicked, this, &TwitchDockWidget::connectChat);

    chatLayout->addWidget(chatText_);
    chatLayout->addLayout(chatRow);
    tabs_->addTab(chatTab, tr("Chat"));

    // Stream tab: title/category updates plus OAuth acquisition fields.
    auto *streamTab = new QWidget(this);
    auto *streamLayout = new QGridLayout(streamTab);
    titleEdit_ = new QLineEdit(streamTab);
    gameIdEdit_ = new QLineEdit(streamTab);
    clientIdEdit_ = new QLineEdit(streamTab);
    clientIdEdit_->setEchoMode(QLineEdit::Password);
    clientSecretEdit_ = new QLineEdit(streamTab);
    clientSecretEdit_->setEchoMode(QLineEdit::Password);
    tokenEdit_ = new QLineEdit(streamTab);
    tokenEdit_->setEchoMode(QLineEdit::Password);
    auto *oauthHelpLabel = new QLabel(
        tr("Create a Twitch app in the Developer Console, add redirect URL %1, paste the Client ID and Client Secret here, then click Authorize in Browser.")
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
    streamLayout->addWidget(new QLabel(tr("Category Game ID"), streamTab), 1, 0);
    streamLayout->addWidget(gameIdEdit_, 1, 1);
    streamLayout->addWidget(updateButton, 2, 0, 1, 2);
    streamLayout->addWidget(refreshButton, 3, 0, 1, 2);
    streamLayout->addWidget(toggleAuthFieldsButton_, 4, 0, 1, 2);

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
    }
    appendChatSystemMessage(
        credentials.streamKey.isEmpty() ? tr("Stream key not found in current OBS service/profile settings.")
                                        : tr("Stream key found in OBS settings."));

    if (!tokenEdit_->text().trimmed().isEmpty() && !clientIdEdit_->text().trimmed().isEmpty()) {
        fetchCurrentChannelInfo();
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
    if (!tokenEdit_->text().trimmed().isEmpty()) {
        appendChatSystemMessage(tr("OAuth token already present; skipping login flow."));
        return;
    }

    const QString clientId = clientIdEdit_->text().trimmed();
    if (clientId.isEmpty()) {
        appendChatSystemMessage(
            tr("Provide Twitch Client ID before starting OAuth. Use Open Twitch Developer Console and register redirect URL %1.")
                .arg(oauthRedirectUrl()));
        return;
    }
    persistClientId(clientId);

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
    query.addQueryItem(QStringLiteral("scope"), QStringLiteral("channel:manage:broadcast chat:read"));
    query.addQueryItem(QStringLiteral("force_verify"), QStringLiteral("false"));
    url.setQuery(query);

    appendChatSystemMessage(tr("Opening Twitch OAuth authorization page..."));
    QDesktopServices::openUrl(url);
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
    tokenEdit_->setText(loadCachedOAuthToken());
}

void TwitchDockWidget::resolveIdentity(const QString &token, std::function<void(bool)> continuation)
{
    if (!broadcasterId_.isEmpty() && !twitchLogin_.isEmpty() && validatedToken_ == token) {
        continuation(true);
        return;
    }

    broadcasterId_.clear();
    twitchLogin_.clear();
    validatedToken_.clear();

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
            continuation(false);
            return;
        }

        const QJsonObject payload = QJsonDocument::fromJson(payloadBytes).object();
        const QString broadcasterId = payload.value(QStringLiteral("user_id")).toString().trimmed();
        const QString twitchLogin = payload.value(QStringLiteral("login")).toString().trimmed();

        if (broadcasterId.isEmpty() || twitchLogin.isEmpty()) {
            broadcasterId_.clear();
            twitchLogin_.clear();
            validatedToken_.clear();
            appendChatSystemMessage(tr("OAuth validation response did not include required identity fields."));
            continuation(false);
            return;
        }

        broadcasterId_ = broadcasterId;
        twitchLogin_ = twitchLogin;
        validatedToken_ = token;
        continuation(true);
    });
}

void TwitchDockWidget::updateChannelInfo()
{
    const QString token = tokenEdit_->text().trimmed();
    const QString clientId = clientIdEdit_->text().trimmed();
    const QString title = titleEdit_->text().trimmed();
    const QString gameId = gameIdEdit_->text().trimmed();

    if (token.isEmpty() || clientId.isEmpty()) {
        appendChatSystemMessage(tr("Cannot patch channel info: missing OAuth token or client ID."));
        return;
    }

    resolveIdentity(token, [this, token, clientId, title, gameId](bool ok) {
        if (!ok) {
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
        if (!gameId.isEmpty()) {
            body.insert(QStringLiteral("game_id"), gameId);
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
}

void TwitchDockWidget::connectChat()
{
    const QString token = tokenEdit_->text().trimmed();
    if (token.isEmpty()) {
        appendChatSystemMessage(tr("Chat connection requires OAuth token with chat:read scope."));
        return;
    }

    resolveIdentity(token, [this, token](bool ok) {
        if (!ok) {
            return;
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
    });
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
    chatText_->append(QStringLiteral("<span style='color:#adadb8;'>[system]</span> <span style='color:#d3d3da;'>%1</span>")
                          .arg(message.toHtmlEscaped()));
}

void TwitchDockWidget::appendFormattedChatLine(const QByteArray &ircLine)
{
    const QString line = QString::fromUtf8(ircLine);
    const QString timestamp = QTime::currentTime().toString(QStringLiteral("HH:mm"));

    static const QRegularExpression messagePattern(
        QStringLiteral("^@([^\\s]+)\\s+:[^\\s]+\\s+PRIVMSG\\s+#[^\\s]+\\s+:(.*)$"));
    const QRegularExpressionMatch messageMatch = messagePattern.match(line);
    if (messageMatch.hasMatch()) {
        const QString tags = messageMatch.captured(1);
        const QString message = messageMatch.captured(2).toHtmlEscaped();
        const QString displayName = ircTagValue(tags, QStringLiteral("display-name"));
        const QString username = displayName.isEmpty() ? QStringLiteral("user") : displayName.toHtmlEscaped();
        const QString colorValue = ircTagValue(tags, QStringLiteral("color"));
        const QString color = colorValue.isEmpty() ? QStringLiteral("#bf94ff") : colorValue;
        chatText_->append(QStringLiteral("<span style='color:#8f8fa3;'>%1</span> <span style='color:%2; font-weight:600;'>%3</span>"
                                         "<span style='color:#efeff1;'>:</span> <span style='color:#efeff1;'>%4</span>")
                              .arg(timestamp, color, username, message));
        return;
    }

    static const QRegularExpression noticePattern(QStringLiteral("NOTICE\\s+#[^\\s]+\\s+:(.*)$"));
    const QRegularExpressionMatch noticeMatch = noticePattern.match(line);
    if (noticeMatch.hasMatch()) {
        chatText_->append(QStringLiteral("<span style='color:#8f8fa3;'>%1</span> <span style='color:#f7c843;'>[notice]</span> %2")
                              .arg(timestamp, noticeMatch.captured(1).toHtmlEscaped()));
        return;
    }

    chatText_->append(QStringLiteral("<span style='color:#8f8fa3;'>%1</span> <span style='color:#adadb8;'>%2</span>")
                          .arg(timestamp, line.toHtmlEscaped()));
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

    resolveIdentity(token, [this, token, clientId](bool ok) {
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
            gameIdEdit_->setText(channel.value(QStringLiteral("game_id")).toString().trimmed());
            const QString gameName = channel.value(QStringLiteral("game_name")).toString().trimmed();
            appendChatSystemMessage(
                tr("Loaded current stream info from Twitch (title and game%1).")
                    .arg(gameName.isEmpty() ? QString() : QStringLiteral(": %1").arg(gameName)));
        });
    });
}
