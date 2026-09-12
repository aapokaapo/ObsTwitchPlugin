#pragma once

#include <functional>

#include <QHash>
#include <QImage>
#include <QLineEdit>
#include <QList>
#include <QListWidget>
#include <QMap>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QPushButton>
#include <QCompleter>
#include <QSet>
#include <QStringList>
#include <QStringListModel>
#include <QTableWidget>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTabWidget>
#include <QTextEdit>
#include <QTimer>
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
    void sendManualMessage();
    void onChatSocketReadyRead();

    void addCommand();

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
        QString commandResponse;
        QList<ChatEmoteOccurrence> emotes;
        QSet<QString> requiredEmoteIds;
        int commandDepth = 0;
        qint64 enqueuedAtMs = 0;
    };

    struct LocalChatEcho {
        QString message;
        int commandDepth = 0;
    };

    void buildUi();
    void loadPersistedUiState();
    bool showCommandDialog(const QString &windowTitle,
                           const QString &initialTrigger,
                           const QString &initialResponse,
                           QString &trigger,
                           QString &response);
    int commandRowForTrigger(const QString &trigger) const;
    void populateCommandRow(int row, const QString &trigger, const QString &response);
    void editCommandFromButton();
    void deleteCommandFromButton();
    void editCommand(const QString &existingTrigger);
    void deleteCommand(const QString &trigger);
    void persistCommands() const;
    void loadPersistedCommands();
    void appendChatSystemMessage(const QString &message);
    void appendChatActivityMessage(const QString &timestamp, const QString &message);
    void appendLocalOutgoingChatMessage(const QString &message, int commandDepth);
    bool sendChatMessage(const QString &message, int commandDepth = 0);
    void appendFormattedChatLine(const QByteArray &ircLine);
    QString commandResponseForMessage(const QString &message) const;
    QList<ChatEmoteOccurrence> parseIrcEmotes(const QString &emotesTag) const;
    void enqueueChatMessage(const QString &timestamp,
                            const QString &username,
                            const QString &color,
                            const QString &message,
                            const QList<ChatEmoteOccurrence> &emotes,
                            const QString &commandResponse = {},
                            int commandDepth = 0);
    void flushPendingChatMessages();
    void renderChatMessage(const PendingChatMessage &message);
    void requestEmoteImage(const QString &emoteId);
    bool isEmoteAvailable(const QString &emoteId) const;
    QString sanitizeChannelLogin(const QString &value) const;
    void persistChatChannel(const QString &channel);
    QString loadCachedChatChannel() const;
    void persistClientId(const QString &clientId);
    QString loadCachedClientId() const;
    void persistClientSecret(const QString &clientSecret);
    QString loadCachedClientSecret() const;
    void fetchCurrentChannelInfo();
    void startFollowerActivityPolling(const QString &token, const QString &clientId);
    void stopFollowerActivityPolling();
    void pollLatestFollowers(bool initializeSnapshot);
    void fetchCategorySuggestions(const QString &query);
    void resolveCategoryId(const QString &token,
                           const QString &clientId,
                           const QString &categoryName,
                           std::function<void(const QString &)> continuation);

    TwitchCredentials extractCredentialsFromObs() const;
    TwitchCredentials extractCredentialsFromObsProfileIni() const;

    void startOAuthServer();
    void handleOAuthRedirectPayload(const QByteArray &requestPayload, QTcpSocket *clientSocket);
    void exchangeOAuthCodeForToken(const QString &authorizationCode);

    void persistOAuthToken(const QString &token);
    QString loadCachedOAuthToken() const;

    void resolveIdentity(const QString &token, std::function<void(bool, const QSet<QString> &)> continuation);
    QByteArray buildIrcPass(const QString &oauthToken) const;

    QTabWidget *tabs_ = nullptr;

    QTextEdit *chatText_ = nullptr;
    QLineEdit *channelEdit_ = nullptr;
    QLineEdit *messageEdit_ = nullptr;

    QLineEdit *titleEdit_ = nullptr;
    QLineEdit *gameIdEdit_ = nullptr;
    QCompleter *gameCategoryCompleter_ = nullptr;
    QStringListModel *gameCategorySuggestionsModel_ = nullptr;
    QLineEdit *clientIdEdit_ = nullptr;
    QLineEdit *clientSecretEdit_ = nullptr;
    QLineEdit *tokenEdit_ = nullptr;
    QWidget *authFieldsContainer_ = nullptr;
    QPushButton *toggleAuthFieldsButton_ = nullptr;

    QListWidget *friendLinks_ = nullptr;
    QLineEdit *friendLinkInput_ = nullptr;
    QTableWidget *commandsTable_ = nullptr;

    QTcpServer *oauthServer_ = nullptr;
    QTcpSocket *chatSocket_ = nullptr;
    QNetworkAccessManager *networkManager_ = nullptr;
    QTimer *categorySuggestTimer_ = nullptr;
    QTimer *followerPollTimer_ = nullptr;
    QNetworkReply *categorySuggestReply_ = nullptr;

    QString broadcasterId_;
    QString twitchLogin_;
    QString validatedToken_;
    QSet<QString> validatedScopes_;
    QString followerPollToken_;
    QString followerPollClientId_;
    QHash<QString, QImage> emoteImages_;
    QMap<QString, QString> customCommands_;
    QSet<QString> pendingEmoteIds_;
    QSet<QString> unavailableEmoteIds_;
    QList<PendingChatMessage> pendingChatMessages_;
    QList<LocalChatEcho> pendingLocalChatEchoes_;
    QSet<QString> knownFollowerIds_;
    quint64 followerPollSessionId_ = 0;
    bool chatCanSendMessages_ = false;
    bool chatAutoConnectAttempted_ = false;
    bool followerSnapshotInitialized_ = false;
    bool followerPollRequestInFlight_ = false;
};
