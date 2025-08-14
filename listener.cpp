#include "quickfix/Application.h"
#include "quickfix/FileStore.h"
#include "quickfix/MessageCracker.h"
#include "quickfix/Session.h"
#include "quickfix/SessionSettings.h"
#include "quickfix/ThreadedSocketAcceptor.h"
#include "quickfix/fix44/MarketDataRequest.h"
#include "quickfix/fix44/MarketDataSnapshotFullRefresh.h"
#include <atomic>
#include <chrono>
#include <iostream>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>
#include <algorithm>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>
#include <fcntl.h>

struct SessionIDHash {
    std::size_t operator()(const FIX::SessionID &id) const noexcept { return std::hash<std::string>()(id.toString()); }
};

struct SessionIDEqual {
    bool operator()(const FIX::SessionID &a, const FIX::SessionID &b) const noexcept { return a == b; }
};

class MarketDataApp : public FIX::Application, public FIX::MessageCracker {
public:
    void onCreate(const FIX::SessionID &sessionID) override {
        std::cout << "Session created: " << sessionID << std::endl;
    }

    void onLogon(const FIX::SessionID &sessionID) override {
        std::cout << "Logon received: " << sessionID << std::endl;
    }

    void onLogout(const FIX::SessionID &sessionID) override {
        std::cout << "Logout: " << sessionID << std::endl;
        std::lock_guard<std::mutex> lock(subMutex);
        subscribedSymbols.erase(sessionID);
    }

    void toAdmin(FIX::Message &, const FIX::SessionID &) override {}
    void fromAdmin(const FIX::Message& message, const FIX::SessionID& sessionID)
    throw(FIX::FieldNotFound, FIX::IncorrectDataFormat, FIX::IncorrectTagValue, FIX::RejectLogon) override {}
    void toApp(FIX::Message& message, const FIX::SessionID& sessionID)
    throw(FIX::DoNotSend) override {}
    void fromApp(const FIX::Message& message, const FIX::SessionID& sessionID)
    throw(FIX::FieldNotFound, FIX::IncorrectDataFormat, FIX::IncorrectTagValue, FIX::UnsupportedMessageType) override
{
    crack(message, sessionID);
}

    void onMessage(const FIX44::MarketDataRequest &request, const FIX::SessionID &sessionID) {
        FIX::MDReqID mdReqID;
        FIX::NoRelatedSym noRelatedSym;
        try {
            request.get(mdReqID);
            request.get(noRelatedSym);
        } catch (...) {
            std::cerr << "Invalid MarketDataRequest" << std::endl;
            return;
        }

        std::vector<std::string> symbols;
        for (int i = 1; i <= noRelatedSym; ++i) {
            FIX44::MarketDataRequest::NoRelatedSym group;
            request.getGroup(i, group);
            FIX::Symbol symbol;
            group.get(symbol);
            symbols.push_back(symbol.getString());
        }

        {
            std::lock_guard<std::mutex> lock(subMutex);
            subscribedSymbols[sessionID] = symbols;
        }

        std::cout << "Session " << sessionID << " subscribed to:";
        for (auto &s : symbols) std::cout << " " << s;
        std::cout << std::endl;
    }

    void startLivePriceServer(int port) {
        streaming = true;
        std::thread([this, port]() {
            int listenSock = socket(AF_INET, SOCK_STREAM, 0);
            if (listenSock < 0) {
                std::cerr << "socket failed\n";
                return;
            }

            sockaddr_in addr{};
            addr.sin_family = AF_INET;
            addr.sin_addr.s_addr = INADDR_ANY;
            addr.sin_port = htons(port);

            if (bind(listenSock, (sockaddr *)&addr, sizeof(addr)) < 0) {
                std::cerr << "bind failed\n";
                close(listenSock);
                return;
            }

            if (listen(listenSock, SOMAXCONN) < 0) {
                std::cerr << "listen failed\n";
                close(listenSock);
                return;
            }

            // Accept clients in background
            std::thread([this, listenSock]() {
                while (streaming) {
                    int client = accept(listenSock, nullptr, nullptr);
                    if (client >= 0) {
                        int flags = fcntl(client, F_GETFL, 0);
                        fcntl(client, F_SETFL, flags | O_NONBLOCK);
                        std::lock_guard<std::mutex> lock(clientMutex);
                        clients.push_back(client);
                        std::cout << "Client connected to live price server.\n";
                    }
                    std::this_thread::sleep_for(std::chrono::milliseconds(50));
                }
            }).detach();

            char buffer[1024];
            while (streaming) {
                std::lock_guard<std::mutex> lock(clientMutex);
                for (auto it = clients.begin(); it != clients.end();) {
                    int bytes = recv(*it, buffer, sizeof(buffer) - 1, 0);
                    if (bytes > 0) {
                        buffer[bytes] = '\0';
                        broadcastPriceToFIX(buffer);
                        ++it;
                    } else if (bytes == 0 || (bytes < 0 && errno != EWOULDBLOCK)) {
                        close(*it);
                        it = clients.erase(it);
                        std::cout << "Client disconnected.\n";
                    } else {
                        ++it;
                    }
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
            }

            for (auto c : clients) close(c);
            clients.clear();
            close(listenSock);
        }).detach();
    }

    void stopLivePriceServer() { streaming = false; }

private:
    std::atomic<bool> streaming{false};
    std::vector<int> clients;
    std::mutex clientMutex;

    std::unordered_map<FIX::SessionID, std::vector<std::string>, SessionIDHash, SessionIDEqual> subscribedSymbols;
    std::mutex subMutex;

    std::unordered_map<std::string, double> contractSizes{
        {"EURUSD", 100000}, {"XAUUSD", 100}, {"USOIL", 1000}, {"GBPUSD", 100000},
        {"USDJPY", 100000}, {"AUDUSD", 100000}, {"USDCHF", 100000}, {"NZDUSD", 100000},
        {"BTCUSD", 1}, {"ETHUSD", 1}
    };

    void broadcastPriceToFIX(const std::string &priceData) {
        std::istringstream ss(priceData);
        std::string token;
        auto sessions = FIX::Session::getSessions();

        while (std::getline(ss, token, ';')) {
            std::istringstream symbolStream(token);
            std::string symbolPart, bidPart, askPart;
            if (!std::getline(symbolStream, symbolPart, ',')) continue;
            if (!std::getline(symbolStream, bidPart, ',')) continue;
            if (!std::getline(symbolStream, askPart, ',')) continue;

            std::string symbol = symbolPart;
            double bid = std::stod(bidPart.substr(bidPart.find(':') + 1));
            double ask = std::stod(askPart.substr(askPart.find(':') + 1));

            for (const auto &sessID : sessions) {
                bool sendThisSymbol = false;
                {
                    std::lock_guard<std::mutex> lock(subMutex);
                    auto it = subscribedSymbols.find(sessID);
                    if (it != subscribedSymbols.end()) {
                        if (std::find(it->second.begin(), it->second.end(), symbol) != it->second.end()) {
                            sendThisSymbol = true;
                        }
                    }
                }

                if (!sendThisSymbol) continue;

                FIX44::MarketDataSnapshotFullRefresh mdSnap;
                mdSnap.setField(FIX::MDReqID("MDReq_" + symbol));
                mdSnap.setField(FIX::Symbol(symbol));

                FIX44::MarketDataSnapshotFullRefresh::NoMDEntries bidGroup;
                bidGroup.setField(FIX::MDEntryType(FIX::MDEntryType_BID));
                bidGroup.setField(FIX::MDEntryPx(bid));
                bidGroup.setField(FIX::MDEntrySize(100000));
                mdSnap.addGroup(bidGroup);

                FIX44::MarketDataSnapshotFullRefresh::NoMDEntries askGroup;
                askGroup.setField(FIX::MDEntryType(FIX::MDEntryType_OFFER));
                askGroup.setField(FIX::MDEntryPx(ask));
                askGroup.setField(FIX::MDEntrySize(100000));
                mdSnap.addGroup(askGroup);

                try {
                    FIX::Session::sendToTarget(mdSnap, sessID);
                } catch (FIX::SessionNotFound &) {
                    std::cerr << "Session not found: " << sessID << "\n";
                }
            }
        }
    }
};

int main() {
    const char *cfgFile = "feedsender.cfg";

    try {
        FIX::SessionSettings settings(cfgFile);
        MarketDataApp app;
        FIX::FileStoreFactory storeFactory(settings);
        FIX::ScreenLogFactory logFactory(settings);
        FIX::ThreadedSocketAcceptor acceptor(app, storeFactory, settings, logFactory);

        acceptor.start();
        std::cout << "Acceptor started, waiting for connections...\n";

        app.startLivePriceServer(3001);

        while (true) std::this_thread::sleep_for(std::chrono::seconds(1));

        app.stopLivePriceServer();
        acceptor.stop();
    } catch (std::exception &e) {
        std::cerr << "Exception: " << e.what() << std::endl;
        return 1;
    }

    return 0;
}
