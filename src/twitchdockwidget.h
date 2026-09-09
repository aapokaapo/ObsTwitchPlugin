#pragma once

#include <functional>

#include <QHash>
#include <QImage>
#include <QLineEdit>
#include <QList>
#include <QListWidget>
#include <QNetworkAccessManager>
#include <QPushButton>
#include <QSet>
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

    struct ChatEmoteOccurrence {
        QString id;
        int start = 0;
        int end = -1;
    };

    struct PendingChatMessage {
        QString timestamp;
        QString username;
        QString color;
        QString message;
        QList<ChatEmoteOccurrence> emotes;
        QSet<QString> requiredEmoteIds;
    };

    void buildUi();
    void loadPersistedUiState();
    void appendChatSystemMessage(const QString &message);
    void appendFormattedChatLine(const QByteArray &ircLine);
    QList<ChatEmoteOccurrence> parseIrcEmotes(const QString &emotesTag) const;
    void enqueueChatMessage(const QString &timestamp,
                            const QString &username,
                            const QString &color,
                            const QString &message,
                            const QList<ChatEmoteOccurrence> &emotes);
    void flushPendingChatMessages();
    void renderChatMessage(const PendingChatMessage &message);
    void requestEmoteImage(const QString &emoteId);
    bool isEmoteAvailable(const QString &emoteId) const;
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
    QWidget *authFieldsContainer_ = nullptr;
    QPushButton *toggleAuthFieldsButton_ = nullptr;

    QListWidget *friendLinks_ = nullptr;
    QLineEdit *friendLinkInput_ = nullptr;

    QTcpServer *oauthServer_ = nullptr;
    QTcpSocket *chatSocket_ = nullptr;
    QNetworkAccessManager *networkManager_ = nullptr;

    QString broadcasterId_;
    QString twitchLogin_;
    QString validatedToken_;
    QHash<QString, QImage> emoteImages_;
    QSet<QString> pendingEmoteIds_;
    QSet<QString> unavailableEmoteIds_;
    QList<PendingChatMessage> pendingChatMessages_;
};
