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
#include <condition_variable>
#include <iostream>
#include <mutex>
#include <queue>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <algorithm>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>

struct SessionIDHash {
    std::size_t operator()(const FIX::SessionID &id) const noexcept { return std::hash<std::string>()(id.toString()); }
};
struct SessionIDEqual {
    bool operator()(const FIX::SessionID &a, const FIX::SessionID &b) const noexcept { return a == b; }
};

struct PriceUpdate {
    std::string symbol;
    double bid;
    double ask;
};

class MarketDataApp : public FIX::Application, public FIX::MessageCracker {
public:
    // ---- FIX callbacks ----
    void onCreate(const FIX::SessionID &sessionID) override { std::cout << "Session created: " << sessionID << std::endl; }
    void onLogon(const FIX::SessionID &sessionID) override { std::cout << "Logon: " << sessionID << std::endl; }
    void onLogout(const FIX::SessionID &sessionID) override {
        std::cout << "Logout: " << sessionID << std::endl;
        std::lock_guard<std::mutex> lock(subMutex);
        subscribedSymbols.erase(sessionID);
    }
    void toAdmin(FIX::Message &, const FIX::SessionID &) override {}
    void fromAdmin(const FIX::Message& msg, const FIX::SessionID& s)
        throw(FIX::FieldNotFound, FIX::IncorrectDataFormat, FIX::IncorrectTagValue, FIX::RejectLogon) override {}

    void toApp(FIX::Message& msg, const FIX::SessionID& s)
        throw(FIX::DoNotSend) override {}

    void fromApp(const FIX::Message& msg, const FIX::SessionID& s)
        throw(FIX::FieldNotFound, FIX::IncorrectDataFormat, FIX::IncorrectTagValue, FIX::UnsupportedMessageType) override {
        crack(msg, s);
    }

    void onMessage(const FIX44::MarketDataRequest &request, const FIX::SessionID &sessionID) {
        FIX::NoRelatedSym noRelatedSym;
        try { request.get(noRelatedSym); } catch (...) { return; }

        std::vector<std::string> symbols;
        for (int i = 1; i <= noRelatedSym; ++i) {
            FIX44::MarketDataRequest::NoRelatedSym group;
            request.getGroup(i, group);
            FIX::Symbol symbol;
            group.get(symbol);
            symbols.push_back(symbol.getString());
        }

        std::lock_guard<std::mutex> lock(subMutex);
        auto &subs = subscribedSymbols[sessionID];
        for (auto &s : symbols)
            if (std::find(subs.begin(), subs.end(), s) == subs.end())
                subs.push_back(s);
    }

    // ---- Live price server per port ----
    void startLivePriceServer(int port, const std::string &symbol) {
        streaming = true;
        symbolMutexes[symbol];

        std::thread([this, port, symbol]() {
            int listenSock = socket(AF_INET, SOCK_STREAM, 0);
            if (listenSock < 0) { std::cerr << "socket failed\n"; return; }

            sockaddr_in addr{};
            addr.sin_family = AF_INET;
            addr.sin_addr.s_addr = INADDR_ANY;
            addr.sin_port = htons(port);

            if (bind(listenSock, (sockaddr*)&addr, sizeof(addr)) < 0) { close(listenSock); return; }
            if (listen(listenSock, SOMAXCONN) < 0) { close(listenSock); return; }

            // Accept loop
            std::thread([this, listenSock]() {
                while (streaming) {
                    int client = accept(listenSock, nullptr, nullptr);
                    if (client >= 0) {
                        int flags = fcntl(client, F_GETFL, 0);
                        fcntl(client, F_SETFL, flags | O_NONBLOCK);
                        std::lock_guard<std::mutex> lock(clientMutex);
                        clients.push_back(client);
                    }
                    std::this_thread::sleep_for(std::chrono::milliseconds(10));
                }
            }).detach();

            char buffer[1024];
            while (streaming) {
                std::lock_guard<std::mutex> lock(clientMutex);
                for (auto it = clients.begin(); it != clients.end();) {
                    int bytes = recv(*it, buffer, sizeof(buffer) - 1, 0);
                    if (bytes > 0) {
                        buffer[bytes] = '\0';
                        std::string data(buffer);

                        std::string parsedSymbol;
                        double bid, ask;
                        if (parsePriceData(data, parsedSymbol, bid, ask)) {
                            pushPrice({parsedSymbol, bid, ask});
                        }
                        ++it;
                    } else if (bytes == 0 || (bytes < 0 && errno != EWOULDBLOCK)) {
                        close(*it);
                        it = clients.erase(it);
                    } else ++it;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }

            for (auto c : clients) close(c);
            close(listenSock);
        }).detach();

        // Start dedicated sender thread for this symbol
        startSymbolSender(symbol);
    }

    // Start synthetic price generator
    void startSyntheticSender(const std::string &spotSymbol, const std::string &futureSymbol, const std::string &syntheticSymbol) {
        std::lock_guard<std::mutex> lock(senderThreadsMutex);
        if (activeSenderSymbols.find(syntheticSymbol) != activeSenderSymbols.end()) return;
        activeSenderSymbols.insert(syntheticSymbol);
        startSymbolSender(syntheticSymbol);

        std::thread([this, spotSymbol, futureSymbol, syntheticSymbol]() {
            while (streaming) {
                pushSyntheticPrice(spotSymbol, futureSymbol, syntheticSymbol);
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
        }).detach();
    }

    void stopLivePriceServer() { streaming = false; }

private:
    std::atomic<bool> streaming{false};
    std::vector<int> clients;
    std::mutex clientMutex;
    std::unordered_map<std::string, PriceUpdate> latestPrices;
    std::mutex latestPricesMutex;
    std::mutex senderThreadsMutex;
    std::unordered_set<std::string> activeSenderSymbols;

    std::unordered_map<FIX::SessionID, std::vector<std::string>, SessionIDHash, SessionIDEqual> subscribedSymbols;
    std::mutex subMutex;

    std::unordered_map<std::string, std::pair<double,double>> lastSentPrices;
    std::unordered_map<std::string, std::queue<PriceUpdate>> symbolQueues;
    std::unordered_map<std::string, std::mutex> symbolMutexes;

    bool parsePriceData(const std::string &priceData, std::string &symbol, double &bid, double &ask) {
        std::istringstream ss(priceData);
        std::string symPart, bidPart, askPart;
        if (!std::getline(ss, symPart, ',')) return false;
        if (!std::getline(ss, bidPart, ',')) return false;
        if (!std::getline(ss, askPart, ';')) return false;

        try {
            symbol = symPart.substr(symPart.find(':') + 1);
            bid = std::stod(bidPart.substr(bidPart.find(':') + 1));
            ask = std::stod(askPart.substr(askPart.find(':') + 1));
        } catch (...) { return false; }
        return true;
    }

    void pushPrice(const PriceUpdate &pu) {
        {
            std::lock_guard<std::mutex> lock(symbolMutexes[pu.symbol]);
            symbolQueues[pu.symbol].push(pu);
        }
        {
            std::lock_guard<std::mutex> lock(latestPricesMutex);
            latestPrices[pu.symbol] = pu;
        }
    }

    void pushSyntheticPrice(const std::string &spotSymbol, const std::string &futureSymbol, const std::string &syntheticSymbol) {
        PriceUpdate spot{spotSymbol,0,0}, future{futureSymbol,0,0};
        {
            std::lock_guard<std::mutex> lock(latestPricesMutex);
            if (latestPrices.count(spotSymbol)) spot = latestPrices[spotSymbol];
            if (latestPrices.count(futureSymbol)) future = latestPrices[futureSymbol];
        }

        if ((spot.bid == 0 && spot.ask == 0) || (future.bid == 0 && future.ask == 0)) return;

        PriceUpdate synthetic;
        synthetic.symbol = syntheticSymbol;
        synthetic.bid = future.bid - spot.ask;
        synthetic.ask = future.ask - spot.bid;

        pushPrice(synthetic);
    }

    void sendToAllSubscribers(const PriceUpdate &pu) {
        auto sessions = FIX::Session::getSessions();
        for (const auto &sessID : sessions) {
            bool sendThis = false;
            {
                std::lock_guard<std::mutex> lock(subMutex);
                auto itSub = subscribedSymbols.find(sessID);
                if (itSub != subscribedSymbols.end() &&
                    std::find(itSub->second.begin(), itSub->second.end(), pu.symbol) != itSub->second.end())
                    sendThis = true;
            }
            if (!sendThis) continue;

            FIX44::MarketDataSnapshotFullRefresh mdSnap;
            mdSnap.setField(FIX::MDReqID("MDReq_" + pu.symbol));
            mdSnap.setField(FIX::Symbol(pu.symbol));

            FIX44::MarketDataSnapshotFullRefresh::NoMDEntries bidGroup;
            bidGroup.setField(FIX::MDEntryType(FIX::MDEntryType_BID));
            bidGroup.setField(FIX::MDEntryPx(pu.bid));
            bidGroup.setField(FIX::MDEntrySize(100000));
            mdSnap.addGroup(bidGroup);

            FIX44::MarketDataSnapshotFullRefresh::NoMDEntries askGroup;
            askGroup.setField(FIX::MDEntryType(FIX::MDEntryType_OFFER));
            askGroup.setField(FIX::MDEntryPx(pu.ask));
            askGroup.setField(FIX::MDEntrySize(100000));
            mdSnap.addGroup(askGroup);

            try { FIX::Session::sendToTarget(mdSnap, sessID); } catch (...) {}
        }
    }

    void startSymbolSender(const std::string &symbol) {
        std::thread([this, symbol]() {
            while (streaming) {
                PriceUpdate pu;
                bool hasPrice = false;
                {
                    std::lock_guard<std::mutex> lock(symbolMutexes[symbol]);
                    if (!symbolQueues[symbol].empty()) {
                        pu = symbolQueues[symbol].front();
                        symbolQueues[symbol].pop();
                        hasPrice = true;
                    }
                }

                if (hasPrice) {
                    auto itLast = lastSentPrices.find(symbol);
                    if (itLast != lastSentPrices.end() &&
                        itLast->second.first == pu.bid &&
                        itLast->second.second == pu.ask) {
                        continue;
                    }
                    lastSentPrices[symbol] = {pu.bid, pu.ask};
                    sendToAllSubscribers(pu);
                } else {
                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
                }
            }
        }).detach();
    }
};

// ---- Main ----
int main() {
    const char *cfgFile = "feedsender.cfg";

    try {
        FIX::SessionSettings settings(cfgFile);
        MarketDataApp app;
        FIX::FileStoreFactory storeFactory(settings);
        FIX::ScreenLogFactory logFactory(settings);
        FIX::ThreadedSocketAcceptor acceptor(app, storeFactory, settings, logFactory);

        acceptor.start();
        std::cout << "Acceptor started...\n";

        // Start live feeds
        app.startLivePriceServer(3001, "GOLD");
        app.startLivePriceServer(3002, "GOLD_DEC25");

        // Start synthetic price generation
        app.startSyntheticSender("GOLD", "GOLD_DEC25", "GOLD_SYNAJAY");

        while (true) std::this_thread::sleep_for(std::chrono::seconds(1));

        app.stopLivePriceServer();
        acceptor.stop();
    } catch (std::exception &e) {
        std::cerr << "Exception: " << e.what() << std::endl;
        return 1;
    }

    return 0;
}
