#include "twitchdockwidget.h"

#include <QDesktopServices>
#include <QApplication>
#include <QClipboard>
#include <QDir>
#include <QFile>
#include <QGridLayout>
#include <QGroupBox>
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
#include <QInputDialog>
#include <QMessageBox>
#include <QPushButton>
#include <QPointer>
#include <QRandomGenerator>
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
#include <limits>
#include <optional>
#include <utility>

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
constexpr auto kRefreshTokenSettingsKey = "twitch_refresh_token";
constexpr auto kTokenExpirySettingsKey = "twitch_token_expiry_utc";
constexpr auto kChatChannelSettingsKey = "chat_channel";
constexpr auto kClientIdSettingsKey = "twitch_client_id";
constexpr auto kClientSecretSettingsKey = "twitch_client_secret";
constexpr auto kCommandsArraySettingsKey = "custom_commands";
constexpr auto kCountersSettingsGroup = "Counters";
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
        QStringLiteral("moderator:manage:banned_users"),
        QStringLiteral("moderator:manage:chat_messages"),
        QStringLiteral("moderator:manage:chat_settings"),
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
        triggerEdit_->setPlaceholderText(tr("wins, !wins, or win*"));
        triggerEdit_->setText(initialTrigger);

        responseEdit_ = new QTextEdit(this);
        responseEdit_->setAcceptRichText(false);
        responseEdit_->setPlaceholderText(
            tr("Response, e.g. Hello {{ username }}! Visit #{{ counter1 }}"));
        responseEdit_->setPlainText(initialResponse);

        formLayout->addRow(tr("Trigger"), triggerEdit_);
        formLayout->addRow(tr("Response"), responseEdit_);
        layout->addLayout(formLayout);
        layout->addWidget(
            new QLabel(tr("Twitch chat responses are limited to %1 characters.").arg(kMaxTwitchChatMessageLength), this));
        auto *wildcardHelp = new QLabel(tr("* matches zero or more non-space characters. Example: win* matches win, windows, winning."), this);
        wildcardHelp->setWordWrap(true);
        layout->addWidget(wildcardHelp);

        auto *buttons = new QDialogButtonBox(QDialogButtonBox::Save | QDialogButtonBox::Cancel, this);
        connect(buttons, &QDialogButtonBox::accepted, this, [this]() {
            if (trigger().isEmpty()) {
                QMessageBox::warning(this, tr("Invalid Command"), tr("Command trigger cannot be empty."));
                return;
            }
            if (QString(trigger()).remove(QLatin1Char('*')).trimmed().isEmpty()) {
                QMessageBox::warning(this, tr("Invalid Command"), tr("A trigger cannot consist only of wildcards."));
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
    followerPollTimer_->setSingleShot(true);
    followerPollTimer_->setInterval(kFollowerPollIntervalMs);
    connect(followerPollTimer_, &QTimer::timeout, this, [this]() { pollLatestFollowers(false); });

    oauthRefreshTimer_ = new QTimer(this);
    oauthRefreshTimer_->setInterval(30000);
    connect(oauthRefreshTimer_, &QTimer::timeout, this, &TwitchDockWidget::checkOAuthExpiry);
    oauthRefreshTimer_->start();

    loadPersistedUiState();
    loadPersistedCommands();
    QTimer::singleShot(0, this, &TwitchDockWidget::refreshObsServiceData);
    QTimer::singleShot(0, this, &TwitchDockWidget::checkOAuthExpiry);
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
    auto *moderationHelp = new QLabel(
        tr("Moderation: /ban user [reason] · /timeout user [seconds] [reason] · /unban user · /clear · "
           "/delete message-id · /slow [seconds] · /slowoff · /followers [minutes] · /followersoff · "
           "/subscribers[off] · /emoteonly[off] · /uniquechat[off]"), chatTab);
    moderationHelp->setWordWrap(true);
    chatLayout->addWidget(moderationHelp);
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

    auto *variableHelp = new QLabel(
        tr("Triggers can appear anywhere in a message and do not need a ! prefix. "
           "A configured prefix is required; * matches zero or more non-space characters (win* matches windows). "
           "Variables: {{ username }}, {{ time_now }}, "
           "{{ date_now }}, {{ channel }}, {{ args }}, {{ random_number }} (1–100), "
           "and named counters such as {{ wins }}. Counter values are changed only using the controls below."), commandsTab);
    variableHelp->setWordWrap(true);
    commandsLayout->addWidget(variableHelp);

    commandsLayout->addLayout(commandButtonRow);
    commandsLayout->addWidget(commandsTable_);
    auto *counterHeader = new QHBoxLayout();
    auto *counterLabel = new QLabel(tr("Counters"), commandsTab);
    auto *addCounterButton = new QPushButton(tr("Add Counter"), commandsTab);
    counterHeader->addWidget(counterLabel);
    counterHeader->addStretch();
    counterHeader->addWidget(addCounterButton);
    connect(addCounterButton, &QPushButton::clicked, this, &TwitchDockWidget::addCounter);
    commandsLayout->addLayout(counterHeader);

    countersTable_ = new QTableWidget(commandsTab);
    countersTable_->setColumnCount(4);
    countersTable_->setHorizontalHeaderLabels({tr("Name"), tr("Value"), tr("Variable"), tr("Actions")});
    countersTable_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    countersTable_->setSelectionMode(QAbstractItemView::NoSelection);
    countersTable_->verticalHeader()->setVisible(false);
    countersTable_->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    countersTable_->horizontalHeader()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    countersTable_->horizontalHeader()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
    countersTable_->horizontalHeader()->setSectionResizeMode(3, QHeaderView::Stretch);
    commandsLayout->addWidget(countersTable_);
    tabs_->addTab(commandsTab, tr("Commands"));

    auto *scoreboardTab = new QWidget(this);
    auto *scoreboardLayout = new QVBoxLayout(scoreboardTab);
    auto *scoreboardHeading = new QLabel(tr("Scoreboard"), scoreboardTab);
    scoreboardHeading->setAlignment(Qt::AlignCenter);
    scoreboardHeading->setStyleSheet(QStringLiteral("font-size: 20px; font-weight: bold;"));
    scoreboardLayout->addWidget(scoreboardHeading);
    auto *scoresRow = new QHBoxLayout();
    const auto createScore = [this, scoreboardTab, scoresRow](const QString &name, const QString &title, QLabel *&valueLabel) {
        auto *panel = new QGroupBox(title, scoreboardTab);
        auto *panelLayout = new QVBoxLayout(panel);
        valueLabel = new QLabel(QStringLiteral("0"), panel);
        valueLabel->setAlignment(Qt::AlignCenter);
        valueLabel->setStyleSheet(QStringLiteral("font-size: 42px; font-weight: bold;"));
        auto *variableLabel = new QLabel(QStringLiteral("{{ %1 }}").arg(name), panel);
        variableLabel->setAlignment(Qt::AlignCenter);
        panelLayout->addWidget(valueLabel);
        panelLayout->addWidget(variableLabel);
        auto *buttons = new QHBoxLayout();
        auto *minus = new QPushButton(QStringLiteral("−"), panel);
        auto *plus = new QPushButton(QStringLiteral("+"), panel);
        auto *edit = new QPushButton(tr("Edit"), panel);
        buttons->addWidget(minus);
        buttons->addWidget(plus);
        buttons->addWidget(edit);
        connect(minus, &QPushButton::clicked, this, [this, name]() { changeCounterValue(name, -1); });
        connect(plus, &QPushButton::clicked, this, [this, name]() { changeCounterValue(name, 1); });
        connect(edit, &QPushButton::clicked, this, [this, name]() { editCounterValue(name); });
        panelLayout->addLayout(buttons);
        scoresRow->addWidget(panel);
    };
    createScore(QStringLiteral("wins_home"), tr("Home"), homeScoreLabel_);
    createScore(QStringLiteral("wins_away"), tr("Away"), awayScoreLabel_);
    scoreboardLayout->addLayout(scoresRow);
    auto *resetScoresButton = new QPushButton(tr("Reset Both Scores"), scoreboardTab);
    connect(resetScoresButton, &QPushButton::clicked, this, [this]() {
        if (QMessageBox::question(this, tr("Reset Scoreboard"), tr("Reset both scores to zero?")) != QMessageBox::Yes)
            return;
        counters_[QStringLiteral("wins_home")] = 0;
        counters_[QStringLiteral("wins_away")] = 0;
        persistCounters();
        refreshCountersTable();
    });
    scoreboardLayout->addWidget(resetScoresButton);
    scoreboardLayout->addStretch();
    tabs_->addTab(scoreboardTab, tr("Scoreboard"));
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

    // OBS's static OAuth setting may contain an older token. Prefer our own
    // rotating credential whenever a refresh token is already stored.
    QSettings oauthSettings(QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation) +
                                QStringLiteral("/obstwitchplugin.ini"), QSettings::IniFormat);
    oauthSettings.beginGroup(kTokenSettingsGroup);
    const bool hasRefreshToken = !oauthSettings.value(kRefreshTokenSettingsKey).toString().isEmpty();
    oauthSettings.endGroup();
    if (!credentials.oauthToken.isEmpty() && !hasRefreshToken) {
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

        storeOAuthCredentials(payload);
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

void TwitchDockWidget::storeOAuthCredentials(const QJsonObject &payload)
{
    const QString accessToken = payload.value(QStringLiteral("access_token")).toString().trimmed();
    if (accessToken.isEmpty()) {
        return;
    }
    const QString configDir = QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation);
    QDir().mkpath(configDir);
    const QString settingsPath = configDir + QStringLiteral("/obstwitchplugin.ini");
    QSettings settings(settingsPath, QSettings::IniFormat);
    settings.beginGroup(kTokenSettingsGroup);
    settings.setValue(kTokenSettingsKey, accessToken);
    const QString refreshToken = payload.value(QStringLiteral("refresh_token")).toString().trimmed();
    if (!refreshToken.isEmpty()) {
        settings.setValue(kRefreshTokenSettingsKey, refreshToken);
    }
    const qint64 seconds = payload.value(QStringLiteral("expires_in")).toVariant().toLongLong();
    if (seconds > 0) {
        settings.setValue(kTokenExpirySettingsKey, QDateTime::currentDateTimeUtc().addSecs(seconds).toString(Qt::ISODate));
    }
    settings.endGroup();
    settings.sync();
    QFile::setPermissions(settingsPath, QFile::ReadOwner | QFile::WriteOwner);

    const bool wasChatConnected = chatSocket_->state() != QAbstractSocket::UnconnectedState;
    broadcasterId_.clear();
    twitchLogin_.clear();
    validatedToken_.clear();
    validatedScopes_.clear();
    tokenEdit_->setText(accessToken);
    followerPollToken_ = accessToken;
    if (wasChatConnected) {
        chatSocket_->abort();
        QTimer::singleShot(0, this, &TwitchDockWidget::connectChat);
    }
}

void TwitchDockWidget::refreshOAuthToken(std::function<void(bool)> continuation)
{
    if (continuation) {
        oauthRefreshWaiters_.append(std::move(continuation));
    }
    if (oauthRefreshInFlight_) {
        return;
    }

    const QString settingsPath = QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation) +
                                 QStringLiteral("/obstwitchplugin.ini");
    QSettings settings(settingsPath, QSettings::IniFormat);
    settings.beginGroup(kTokenSettingsGroup);
    const QString refreshToken = settings.value(kRefreshTokenSettingsKey).toString().trimmed();
    settings.endGroup();
    const QString clientId = clientIdEdit_->text().trimmed();
    const QString clientSecret = clientSecretEdit_->text().trimmed();
    if (refreshToken.isEmpty() || clientId.isEmpty() || clientSecret.isEmpty()) {
        const auto waiters = std::exchange(oauthRefreshWaiters_, {});
        for (const auto &callback : waiters) callback(false);
        return;
    }

    oauthRefreshInFlight_ = true;
    QUrlQuery body;
    body.addQueryItem(QStringLiteral("grant_type"), QStringLiteral("refresh_token"));
    body.addQueryItem(QStringLiteral("refresh_token"), refreshToken);
    body.addQueryItem(QStringLiteral("client_id"), clientId);
    body.addQueryItem(QStringLiteral("client_secret"), clientSecret);
    QNetworkRequest request(QUrl(QStringLiteral("https://id.twitch.tv/oauth2/token")));
    request.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/x-www-form-urlencoded"));
    QNetworkReply *reply = networkManager_->post(request, body.query(QUrl::FullyEncoded).toUtf8());
    connect(reply, &QNetworkReply::finished, this, [this, reply]() {
        const QByteArray bytes = reply->readAll();
        const bool networkOk = reply->error() == QNetworkReply::NoError;
        reply->deleteLater();
        const QJsonObject payload = QJsonDocument::fromJson(bytes).object();
        const bool success = networkOk && !payload.value(QStringLiteral("access_token")).toString().isEmpty();
        if (success) {
            storeOAuthCredentials(payload);
            appendChatSystemMessage(tr("Twitch OAuth token refreshed automatically."));
        } else {
            oauthRefreshRetryAfter_ = QDateTime::currentDateTimeUtc().addSecs(120);
            appendChatSystemMessage(tr("Twitch token refresh failed. Check your connection or authorize again."));
        }
        oauthRefreshInFlight_ = false;
        const auto waiters = std::exchange(oauthRefreshWaiters_, {});
        for (const auto &callback : waiters) callback(success);
    });
}

void TwitchDockWidget::checkOAuthExpiry()
{
    const QString settingsPath = QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation) +
                                 QStringLiteral("/obstwitchplugin.ini");
    QSettings settings(settingsPath, QSettings::IniFormat);
    settings.beginGroup(kTokenSettingsGroup);
    const QString refreshToken = settings.value(kRefreshTokenSettingsKey).toString().trimmed();
    const QDateTime expiry = QDateTime::fromString(settings.value(kTokenExpirySettingsKey).toString(), Qt::ISODate);
    settings.endGroup();
    if (!refreshToken.isEmpty() &&
        (!oauthRefreshRetryAfter_.isValid() || QDateTime::currentDateTimeUtc() >= oauthRefreshRetryAfter_) &&
        (!expiry.isValid() || QDateTime::currentDateTimeUtc().secsTo(expiry) <= 120)) {
        refreshOAuthToken();
    }
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
    counters_.clear();
    deletedCounters_.clear();
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
    settings.beginGroup(kCountersSettingsGroup);
    for (const QString &deletedName : settings.value(QStringLiteral("deleted_names")).toStringList()) {
        deletedCounters_.insert(deletedName);
    }
    for (const QString &key : settings.childKeys()) {
        if (QRegularExpression(QStringLiteral("^[a-z][a-z0-9_]*$")).match(key).hasMatch() &&
            key != QStringLiteral("deleted_names")) {
            counters_.insert(key, settings.value(key).toULongLong());
        }
    }
    settings.endGroup();
    settings.endGroup();

    // These two built-in counters always exist, including after old settings
    // or an attempted deletion in a previous version.
    deletedCounters_.remove(QStringLiteral("wins_home"));
    deletedCounters_.remove(QStringLiteral("wins_away"));
    if (!counters_.contains(QStringLiteral("wins_home"))) counters_.insert(QStringLiteral("wins_home"), 0);
    if (!counters_.contains(QStringLiteral("wins_away"))) counters_.insert(QStringLiteral("wins_away"), 0);

    refreshCountersTable();

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
            if (channel.compare(twitchLogin_, Qt::CaseInsensitive) == 0) {
                startFollowerActivityPolling(token, clientId, broadcasterId_);
            } else {
                QPointer<QTcpSocket> trackedChatSocket(chatSocket_);
                resolveUserIdForLogin(token, clientId, channel, [this, trackedChatSocket, token, clientId, channel](const QString &channelBroadcasterId) {
                    if (!trackedChatSocket || trackedChatSocket->state() != QAbstractSocket::ConnectedState ||
                        sanitizeChannelLogin(channelEdit_->text()) != channel || validatedToken_ != token) {
                        return;
                    }
                    if (channelBroadcasterId.isEmpty()) {
                        appendChatSystemMessage(tr("Unable to resolve the joined channel for follower activity."));
                        return;
                    }
                    startFollowerActivityPolling(token, clientId, channelBroadcasterId);
                });
            }
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
    if (executeModerationCommand(message)) {
        messageEdit_->clear();
        return;
    }
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
        const QString username = displayName.isEmpty() ? senderLogin : displayName;
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
        const QString commandResponse = commandResponseForMessage(message, username);
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
                    const QString gifter =
                        msgId == QStringLiteral("anonsubgift") ? tr("An anonymous gifter")
                                                               : (actor.isEmpty() ? tr("A viewer") : actor);
                    activity =
                        tr("%1 gifted a subscription to %2.").arg(gifter,
                                                                   recipient.isEmpty() ? tr("another viewer") : recipient);
                } else if (msgId == QStringLiteral("submysterygift") || msgId == QStringLiteral("anonsubmysterygift")) {
                    const QString giftCount = ircTagValue(tags, QStringLiteral("msg-param-mass-gift-count"));
                    const QString gifter =
                        msgId == QStringLiteral("anonsubmysterygift") ? tr("An anonymous gifter")
                                                                      : (actor.isEmpty() ? tr("A viewer") : actor);
                    activity =
                        giftCount.isEmpty() ? tr("%1 gifted community subscriptions.").arg(gifter)
                                            : tr("%1 gifted %2 community subscriptions.").arg(gifter, giftCount);
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

QString TwitchDockWidget::commandResponseForMessage(const QString &message, const QString &username)
{
    const QString input = message.trimmed();
    if (input.isEmpty()) {
        return {};
    }

    // Match complete triggers anywhere in chat. For overlapping triggers,
    // choose the earliest occurrence, then the longest match at that position.
    // Boundaries prevent 'win' from matching 'window'. A leading '!' is a
    // literal part of a trigger: bare 'wins' must not also match '!wins'.
    QString selectedTrigger;
    QRegularExpressionMatch selectedMatch;
    int earliest = input.size();
    for (auto it = customCommands_.cbegin(); it != customCommands_.cend(); ++it) {
        const QString trigger = it.key().trimmed();
        if (trigger.isEmpty() || QString(trigger).remove(QLatin1Char('*')).trimmed().isEmpty()) continue;
        QString wildcardPattern = QRegularExpression::escape(trigger);
        wildcardPattern.replace(QStringLiteral("\\*"), QStringLiteral("\\S*"));
        const QRegularExpression pattern(
            QStringLiteral("(?<![\\p{L}\\p{N}_!])%1(?![\\p{L}\\p{N}_])")
                .arg(wildcardPattern),
            QRegularExpression::CaseInsensitiveOption);
        const QRegularExpressionMatch match = pattern.match(input);
        if (!match.hasMatch()) continue;
        if (match.capturedStart() < earliest ||
            (match.capturedStart() == earliest && match.capturedLength() > selectedMatch.capturedLength())) {
            earliest = match.capturedStart();
            selectedTrigger = it.key();
            selectedMatch = match;
        }
    }
    if (selectedTrigger.isEmpty()) {
        return {};
    }
    const QString arguments = input.mid(selectedMatch.capturedEnd()).trimmed();
    return expandCommandVariables(customCommands_.value(selectedTrigger), username, arguments);
}

QString TwitchDockWidget::expandCommandVariables(const QString &response, const QString &username, const QString &arguments)
{
    static const QRegularExpression variablePattern(QStringLiteral("\\{\\{\\s*([a-zA-Z0-9_]+)\\s*\\}\\}"));
    QString expanded;
    int previous = 0;
    auto matches = variablePattern.globalMatch(response);
    while (matches.hasNext()) {
        const QRegularExpressionMatch match = matches.next();
        expanded += response.mid(previous, match.capturedStart() - previous);
        const QString name = match.captured(1).toLower();
        QString value;
        if (name == QStringLiteral("username")) value = username;
        else if (name == QStringLiteral("time_now")) value = QTime::currentTime().toString(QStringLiteral("HH:mm:ss"));
        else if (name == QStringLiteral("date_now")) value = QDate::currentDate().toString(Qt::ISODate);
        else if (name == QStringLiteral("channel")) value = sanitizeChannelLogin(channelEdit_->text());
        else if (name == QStringLiteral("args")) value = arguments;
        else if (name == QStringLiteral("random_number"))
            value = QString::number(QRandomGenerator::global()->bounded(1, 101));
        else if (!deletedCounters_.contains(name) && (counters_.contains(name) ||
                 QRegularExpression(QStringLiteral("^counter[1-9]$")).match(name).hasMatch())) {
            value = QString::number(counters_.value(name));
        } else {
            value = match.captured(); // Leave unknown placeholders intact.
        }
        expanded += value;
        previous = match.capturedEnd();
    }
    expanded += response.mid(previous);
    return expanded;
}

void TwitchDockWidget::persistCounters() const
{
    const QString configDir = QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation);
    QDir().mkpath(configDir);
    QSettings settings(configDir + QStringLiteral("/obstwitchplugin.ini"), QSettings::IniFormat);
    settings.beginGroup(kTokenSettingsGroup);
    settings.beginGroup(kCountersSettingsGroup);
    settings.remove(QString());
    settings.setValue(QStringLiteral("deleted_names"), deletedCounters_.values());
    for (auto it = counters_.cbegin(); it != counters_.cend(); ++it) settings.setValue(it.key(), it.value());
    settings.endGroup();
    settings.endGroup();
    settings.sync();
}

void TwitchDockWidget::refreshCountersTable()
{
    if (!countersTable_) return;
    countersTable_->setRowCount(0);
    for (auto it = counters_.cbegin(); it != counters_.cend(); ++it) {
        const QString name = it.key();
        if (isScoreboardCounter(name)) continue;
        const int row = countersTable_->rowCount();
        countersTable_->insertRow(row);
        countersTable_->setItem(row, 0, new QTableWidgetItem(name));
        countersTable_->setItem(row, 1, new QTableWidgetItem(QString::number(it.value())));
        const QString variable = QStringLiteral("{{ %1 }}").arg(name);
        countersTable_->setItem(row, 2, new QTableWidgetItem(variable));
        auto *actions = new QWidget(countersTable_);
        auto *layout = new QHBoxLayout(actions);
        layout->setContentsMargins(0, 0, 0, 0);
        layout->setSpacing(3);
        const auto addAction = [this, actions, layout](const QString &caption, std::function<void()> callback) {
            auto *button = new QPushButton(caption, actions);
            connect(button, &QPushButton::clicked, this, [callback = std::move(callback)]() { callback(); });
            layout->addWidget(button);
        };
        addAction(QStringLiteral("−"), [this, name]() { changeCounterValue(name, -1); });
        addAction(QStringLiteral("+"), [this, name]() { changeCounterValue(name, 1); });
        addAction(tr("Edit"), [this, name]() { editCounterValue(name); });
        addAction(tr("Reset"), [this, name]() { resetCounter(name); });
        addAction(tr("Copy"), [variable]() { QApplication::clipboard()->setText(variable); });
        addAction(tr("Delete"), [this, name]() { deleteCounter(name); });
        countersTable_->setCellWidget(row, 3, actions);
    }
    countersTable_->resizeRowsToContents();
    refreshScoreboard();
}

bool TwitchDockWidget::isScoreboardCounter(const QString &name)
{
    return name == QStringLiteral("wins_home") || name == QStringLiteral("wins_away");
}

void TwitchDockWidget::refreshScoreboard()
{
    if (homeScoreLabel_) homeScoreLabel_->setText(QString::number(counters_.value(QStringLiteral("wins_home"))));
    if (awayScoreLabel_) awayScoreLabel_->setText(QString::number(counters_.value(QStringLiteral("wins_away"))));
}

void TwitchDockWidget::addCounter()
{
    bool accepted = false;
    const QString name = QInputDialog::getText(this, tr("Add Counter"),
        tr("Counter name (letters, digits, underscores):"), QLineEdit::Normal, {}, &accepted).trimmed().toLower();
    if (!accepted) return;
    static const QRegularExpression validName(QStringLiteral("^[a-z][a-z0-9_]*$"));
    static const QSet<QString> reserved = {QStringLiteral("username"), QStringLiteral("time_now"),
        QStringLiteral("date_now"), QStringLiteral("channel"), QStringLiteral("args"),
        QStringLiteral("random_number")};
    if (!validName.match(name).hasMatch() || reserved.contains(name) || counters_.contains(name) ||
        isScoreboardCounter(name)) {
        QMessageBox::warning(this, tr("Invalid Counter"),
            tr("Use a unique name starting with a letter, containing only letters, numbers, or underscores. "
               "Built-in variable names are reserved."));
        return;
    }
    counters_.insert(name, 0);
    deletedCounters_.remove(name);
    persistCounters();
    refreshCountersTable();
}

void TwitchDockWidget::editCounterValue(const QString &name)
{
    if (!counters_.contains(name)) return;
    bool accepted = false;
    const QString input = QInputDialog::getText(this, tr("Set Counter Value"),
        tr("Value for %1 (non-negative integer):").arg(name), QLineEdit::Normal,
        QString::number(counters_.value(name)), &accepted).trimmed();
    if (!accepted) return;
    bool valid = false;
    const quint64 value = input.toULongLong(&valid);
    if (!valid || input.startsWith(QLatin1Char('-'))) {
        QMessageBox::warning(this, tr("Invalid Value"), tr("Enter a non-negative whole number."));
        return;
    }
    counters_[name] = value;
    persistCounters();
    refreshCountersTable();
}

void TwitchDockWidget::changeCounterValue(const QString &name, int delta)
{
    if (!counters_.contains(name)) return;
    quint64 &value = counters_[name];
    if (delta < 0 && value > 0) --value;
    if (delta > 0 && value < std::numeric_limits<quint64>::max()) ++value;
    persistCounters();
    refreshCountersTable();
}

void TwitchDockWidget::resetCounter(const QString &name)
{
    if (!counters_.contains(name)) return;
    if (QMessageBox::question(this, tr("Reset Counter"),
        tr("Reset %1 to zero?").arg(name)) != QMessageBox::Yes) return;
    counters_[name] = 0;
    persistCounters();
    refreshCountersTable();
}

void TwitchDockWidget::deleteCounter(const QString &name)
{
    if (isScoreboardCounter(name)) return;
    if (!counters_.contains(name)) return;
    if (QMessageBox::question(this, tr("Delete Counter"),
        tr("Delete %1? Custom commands referencing this variable will no longer resolve it.").arg(name)) != QMessageBox::Yes) return;
    counters_.remove(name);
    deletedCounters_.insert(name);
    persistCounters();
    refreshCountersTable();
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

void TwitchDockWidget::sendModerationRequest(const QByteArray &method, const QUrl &url,
                                              const QJsonObject &body, const QString &action)
{
    QNetworkRequest request(url);
    request.setRawHeader("Authorization", QByteArray("Bearer ") + tokenEdit_->text().trimmed().toUtf8());
    request.setRawHeader("Client-Id", clientIdEdit_->text().trimmed().toUtf8());
    request.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
    QNetworkReply *reply = networkManager_->sendCustomRequest(request, method, QJsonDocument(body).toJson(QJsonDocument::Compact));
    connect(reply, &QNetworkReply::finished, this, [this, reply, action]() {
        const QByteArray payload = reply->readAll();
        const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        const bool ok = reply->error() == QNetworkReply::NoError && status >= 200 && status < 300;
        const QString error = QJsonDocument::fromJson(payload).object().value(QStringLiteral("message")).toString();
        reply->deleteLater();
        appendChatSystemMessage(ok ? tr("Moderation: %1 succeeded.").arg(action)
                                   : tr("Moderation: %1 failed (HTTP %2): %3").arg(action).arg(status).arg(error));
    });
}

bool TwitchDockWidget::executeModerationCommand(const QString &message)
{
    const QStringList words = message.simplified().split(QLatin1Char(' '), Qt::SkipEmptyParts);
    if (words.isEmpty() || !words.first().startsWith(QLatin1Char('/')) ||
        words.first().compare(QStringLiteral("/me"), Qt::CaseInsensitive) == 0) return false;

    const QString command = words.first().mid(1).toLower();
    const QSet<QString> bans = {QStringLiteral("ban"), QStringLiteral("timeout"), QStringLiteral("unban")};
    const QSet<QString> modes = {QStringLiteral("slow"), QStringLiteral("slowoff"),
        QStringLiteral("followers"), QStringLiteral("followersoff"), QStringLiteral("subscribers"),
        QStringLiteral("subscribersoff"), QStringLiteral("emoteonly"), QStringLiteral("emoteonlyoff"),
        QStringLiteral("uniquechat"), QStringLiteral("uniquechatoff")};
    if (!bans.contains(command) && !modes.contains(command) && command != QStringLiteral("clear") &&
        command != QStringLiteral("delete")) {
        appendChatSystemMessage(tr("Unknown slash command. See moderation commands above the chat."));
        return true;
    }
    if ((bans.contains(command) || command == QStringLiteral("delete")) && words.size() < 2) {
        appendChatSystemMessage(tr("Missing user or message ID for /%1.").arg(command));
        return true;
    }

    const QString token = tokenEdit_->text().trimmed();
    const QString clientId = clientIdEdit_->text().trimmed();
    if (token.isEmpty() || clientId.isEmpty()) {
        appendChatSystemMessage(tr("Moderation requires Twitch OAuth authorization."));
        return true;
    }
    resolveIdentity(token, [this, token, clientId, words, command, bans, modes](bool ok, const QSet<QString> &scopes) {
        if (!ok) return;
        const QString scope = bans.contains(command) ? QStringLiteral("moderator:manage:banned_users") :
            modes.contains(command) ? QStringLiteral("moderator:manage:chat_settings") :
                                      QStringLiteral("moderator:manage:chat_messages");
        if (!scopes.contains(scope)) {
            appendChatSystemMessage(tr("Moderation requires the %1 scope. Reauthorize in Stream Info.").arg(scope));
            return;
        }
        const QString moderatorId = broadcasterId_;
        const QString channel = sanitizeChannelLogin(channelEdit_->text());
        resolveUserIdForLogin(token, clientId, channel,
            [this, token, clientId, words, command, bans, moderatorId](const QString &channelId) {
            if (channelId.isEmpty()) {
                appendChatSystemMessage(tr("Cannot resolve channel ID for moderation."));
                return;
            }
            const auto endpoint = [channelId, moderatorId](const QString &path) {
                QUrl url(QStringLiteral("https://api.twitch.tv/helix/") + path);
                QUrlQuery query;
                query.addQueryItem(QStringLiteral("broadcaster_id"), channelId);
                query.addQueryItem(QStringLiteral("moderator_id"), moderatorId);
                url.setQuery(query);
                return url;
            };
            if (bans.contains(command)) {
                QString target = words.at(1);
                if (target.startsWith(QLatin1Char('@'))) target.remove(0, 1);
                resolveUserIdForLogin(token, clientId, target,
                    [this, words, command, channelId, moderatorId](const QString &targetId) {
                    if (targetId.isEmpty()) {
                        appendChatSystemMessage(tr("Unable to find moderation target."));
                        return;
                    }
                    QUrl url(QStringLiteral("https://api.twitch.tv/helix/moderation/bans"));
                    QUrlQuery query;
                    query.addQueryItem(QStringLiteral("broadcaster_id"), channelId);
                    query.addQueryItem(QStringLiteral("moderator_id"), moderatorId);
                    if (command == QStringLiteral("unban")) {
                        query.addQueryItem(QStringLiteral("user_id"), targetId);
                        url.setQuery(query);
                        sendModerationRequest("DELETE", url, {}, command);
                        return;
                    }
                    url.setQuery(query);
                    QJsonObject data{{QStringLiteral("user_id"), targetId}};
                    int reasonStart = 2;
                    if (command == QStringLiteral("timeout")) {
                        int seconds = 600;
                        if (words.size() > 2) {
                            bool numeric = false;
                            const int parsed = words.at(2).toInt(&numeric);
                            if (numeric) { seconds = parsed; reasonStart = 3; }
                        }
                        if (seconds < 1 || seconds > 1209600) {
                            appendChatSystemMessage(tr("Timeout must be between 1 and 1209600 seconds."));
                            return;
                        }
                        data.insert(QStringLiteral("duration"), seconds);
                    }
                    if (words.size() > reasonStart)
                        data.insert(QStringLiteral("reason"), words.mid(reasonStart).join(QLatin1Char(' ')).left(500));
                    sendModerationRequest("POST", url, {{QStringLiteral("data"), data}}, command);
                });
                return;
            }
            if (command == QStringLiteral("clear") || command == QStringLiteral("delete")) {
                QUrl url = endpoint(QStringLiteral("moderation/chat"));
                if (command == QStringLiteral("delete")) {
                    QUrlQuery query(url);
                    query.addQueryItem(QStringLiteral("message_id"), words.at(1));
                    url.setQuery(query);
                }
                sendModerationRequest("DELETE", url, {}, command);
                return;
            }
            QJsonObject body;
            if (command == QStringLiteral("slow") || command == QStringLiteral("followers")) {
                int duration = command == QStringLiteral("slow") ? 30 : 0;
                if (words.size() > 1) {
                    bool numeric = false;
                    duration = words.at(1).toInt(&numeric);
                    if (!numeric) { appendChatSystemMessage(tr("Duration must be a number.")); return; }
                }
                const int maximum = command == QStringLiteral("slow") ? 120 : 129600;
                const int minimum = command == QStringLiteral("slow") ? 3 : 0;
                if (duration < minimum || duration > maximum) {
                    appendChatSystemMessage(tr("Duration must be between %1 and %2.").arg(minimum).arg(maximum));
                    return;
                }
                body.insert(command == QStringLiteral("slow") ? QStringLiteral("slow_mode") : QStringLiteral("follower_mode"), true);
                body.insert(command == QStringLiteral("slow") ? QStringLiteral("slow_mode_wait_time") : QStringLiteral("follower_mode_duration"), duration);
            } else {
                QString setting = command;
                const bool enabled = !setting.endsWith(QStringLiteral("off"));
                if (!enabled) setting.chop(3);
                if (setting == QStringLiteral("slow")) setting = QStringLiteral("slow_mode");
                else if (setting == QStringLiteral("followers")) setting = QStringLiteral("follower_mode");
                else if (setting == QStringLiteral("subscribers")) setting = QStringLiteral("subscriber_mode");
                else if (setting == QStringLiteral("emoteonly")) setting = QStringLiteral("emote_mode");
                else if (setting == QStringLiteral("uniquechat")) setting = QStringLiteral("unique_chat_mode");
                body.insert(setting, enabled);
            }
            sendModerationRequest("PATCH", endpoint(QStringLiteral("chat/settings")), body, command);
        });
    });
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

void TwitchDockWidget::startFollowerActivityPolling(const QString &token, const QString &clientId, const QString &broadcasterId)
{
    if (followerPollTimer_) {
        followerPollTimer_->stop();
    }
    followerPollToken_ = token.trimmed();
    followerPollClientId_ = clientId.trimmed();
    followerPollBroadcasterId_ = broadcasterId.trimmed();
    followerPollModeratorId_ = this->broadcasterId_.trimmed();
    knownFollowerIds_.clear();
    newestKnownFollowerAt_ = {};
    followerSnapshotInitialized_ = false;
    followerPollErrorShown_ = false;
    followerPollRequestInFlight_ = false;
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
    followerPollBroadcasterId_.clear();
    followerPollModeratorId_.clear();
    knownFollowerIds_.clear();
    newestKnownFollowerAt_ = {};
    followerSnapshotInitialized_ = false;
    followerPollErrorShown_ = false;
    followerPollRequestInFlight_ = false;
}

void TwitchDockWidget::pollLatestFollowers(bool initializeSnapshot)
{
    if (followerPollToken_.isEmpty() || followerPollClientId_.isEmpty() || followerPollBroadcasterId_.isEmpty() ||
        followerPollModeratorId_.isEmpty() || followerPollRequestInFlight_) {
        return;
    }

    const quint64 sessionId = followerPollSessionId_;
    followerPollRequestInFlight_ = true;
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
        query.addQueryItem(QStringLiteral("broadcaster_id"), followerPollBroadcasterId_);
        query.addQueryItem(QStringLiteral("moderator_id"), followerPollModeratorId_);
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
                            followerPollRequestInFlight_ = false;
                            followerPollRequestSessionId_ = 0;
                        }
                        return;
                    }

                    if (error != QNetworkReply::NoError) {
                        if (followerPollRequestSessionId_ == sessionId) {
                            followerPollRequestInFlight_ = false;
                            followerPollRequestSessionId_ = 0;
                        }
                        if (followerPollTimer_) {
                            followerPollTimer_->start();
                        }
                        if (!followerPollErrorShown_) {
                            appendChatSystemMessage(initializeSnapshot
                                                       ? tr("Unable to load follow activity for chat: %1").arg(errorString)
                                                       : tr("Follower activity refresh failed: %1").arg(errorString));
                            followerPollErrorShown_ = true;
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
                            followerSnapshotInitialized_ && follower.followedAt.isValid() &&
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
                        followerPollRequestInFlight_ = false;
                        followerPollRequestSessionId_ = 0;
                    }
                    if (followerPollTimer_) {
                        followerPollTimer_->start();
                    }
                    followerPollErrorShown_ = false;

                    if (!newestSeenAt.isValid()) {
                        return;
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

                    if (newestSeenAt == newestKnownFollowerAt_) {
                        knownFollowerIds_.unite(newestSeenIds);
                    } else {
                        newestKnownFollowerAt_ = newestSeenAt;
                        knownFollowerIds_ = newestSeenIds;
                    }
                    followerSnapshotInitialized_ = true;
                });
    };

    (*fetchPage)({}, {}, {}, {});
}

void TwitchDockWidget::resolveUserIdForLogin(const QString &token,
                                             const QString &clientId,
                                             const QString &login,
                                             std::function<void(const QString &)> continuation)
{
    const QString trimmedLogin = sanitizeChannelLogin(login);
    if (trimmedLogin.isEmpty()) {
        continuation({});
        return;
    }

    QUrl url(QStringLiteral("https://api.twitch.tv/helix/users"));
    QUrlQuery query;
    query.addQueryItem(QStringLiteral("login"), trimmedLogin);
    url.setQuery(query);

    QNetworkRequest request(url);
    request.setRawHeader("Authorization", QByteArray("Bearer ") + token.toUtf8());
    request.setRawHeader("Client-Id", clientId.toUtf8());

    QNetworkReply *reply = networkManager_->get(request);
    connect(reply, &QNetworkReply::finished, this, [reply, continuation = std::move(continuation)]() mutable {
        const QByteArray payloadBytes = reply->readAll();
        const QNetworkReply::NetworkError error = reply->error();
        reply->deleteLater();

        if (error != QNetworkReply::NoError) {
            continuation({});
            return;
        }

        const QJsonArray data = QJsonDocument::fromJson(payloadBytes).object().value(QStringLiteral("data")).toArray();
        if (data.isEmpty()) {
            continuation({});
            return;
        }

        continuation(data.first().toObject().value(QStringLiteral("id")).toString().trimmed());
    });
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
