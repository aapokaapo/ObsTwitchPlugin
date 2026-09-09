#pragma once

#include <functional>

#include <QLineEdit>
#include <QListWidget>
#include <QNetworkAccessManager>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTabWidget>
#include <QTextEdit>
#include <QWidget>

class TwitchDockWidget final : public QWidget {
    Q_OBJECT

public:
    explicit TwitchDockWidget(QWidget *parent = nullptr);
    ~TwitchDockWidget() override;

private slots:
    void refreshObsServiceData();
    void openTwitchDeveloperConsole();
    void ensureOAuthToken();
    void updateChannelInfo();

    void connectChat();
    void onChatSocketReadyRead();

    void addFriendLink();
    void removeSelectedFriendLink();
    void openSelectedFriendLink();

    void onOAuthServerConnection();

private:
    struct TwitchCredentials {
        QString streamKey;
        QString oauthToken;
        QString clientId;
        QString clientSecret;
    };

    void buildUi();
    void loadPersistedUiState();
    void appendChatSystemMessage(const QString &message);
    void appendFormattedChatLine(const QByteArray &ircLine);
    QString sanitizeChannelLogin(const QString &value) const;
    void persistChatChannel(const QString &channel);
    QString loadCachedChatChannel() const;
    void persistClientId(const QString &clientId);
    QString loadCachedClientId() const;
    void fetchCurrentChannelInfo();

    TwitchCredentials extractCredentialsFromObs() const;
    TwitchCredentials extractCredentialsFromObsProfileIni() const;

    void startOAuthServer();
    void handleOAuthRedirectPayload(const QByteArray &requestPayload, QTcpSocket *clientSocket);
    void exchangeOAuthCodeForToken(const QString &authorizationCode);

    void persistOAuthToken(const QString &token);
    QString loadCachedOAuthToken() const;

    void resolveIdentity(const QString &token, std::function<void(bool)> continuation);
    QByteArray buildIrcPass(const QString &oauthToken) const;

    QTabWidget *tabs_ = nullptr;

    QTextEdit *chatText_ = nullptr;
    QLineEdit *channelEdit_ = nullptr;

    QLineEdit *titleEdit_ = nullptr;
    QLineEdit *gameIdEdit_ = nullptr;
    QLineEdit *clientIdEdit_ = nullptr;
    QLineEdit *clientSecretEdit_ = nullptr;
    QLineEdit *tokenEdit_ = nullptr;

    QListWidget *friendLinks_ = nullptr;
    QLineEdit *friendLinkInput_ = nullptr;

    QTcpServer *oauthServer_ = nullptr;
    QTcpSocket *chatSocket_ = nullptr;
    QNetworkAccessManager *networkManager_ = nullptr;

    QString broadcasterId_;
    QString twitchLogin_;
    QString validatedToken_;
};
