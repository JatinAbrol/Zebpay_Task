#include <iostream>
#include <vector>
#include <string>
#include <chrono>
#include <atomic>
#include <algorithm>
#include <mutex>
#include <curl/curl.h>
#include <nlohmann/json.hpp>

using json = nlohmann::json;
using Clock = std::chrono::steady_clock;

struct Order {
    double price;
    double qty;
};

struct OrderBook {
    std::vector<Order> bids;
    std::vector<Order> asks;
};

class RateLimiter {
    std::atomic<long long> lastCallNs{0};

public:
    bool allow() {
        long long now =
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                Clock::now().time_since_epoch()).count();

        long long last = lastCallNs.load();
        if (now - last < 2'000'000'000LL) return false;
        return lastCallNs.compare_exchange_strong(last, now);
    }
};

static size_t WriteCallback(void* contents, size_t size, size_t nmemb, void* userp) {
    ((std::string*)userp)->append((char*)contents, size * nmemb);
    return size * nmemb;
}

class Exchange {
public:
    virtual OrderBook fetch() = 0;
    virtual ~Exchange() = default;
};

class CoinbaseExchange : public Exchange {
    RateLimiter limiter;

public:
    OrderBook fetch() override {
        OrderBook ob;
        if (!limiter.allow()) return ob;

        CURL* curl = curl_easy_init();
        std::string response;

        curl_easy_setopt(curl, CURLOPT_URL,
            "https://api.exchange.coinbase.com/products/BTC-USD/book?level=2");
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteCallback);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);

        curl_easy_perform(curl);
        curl_easy_cleanup(curl);

        auto j = json::parse(response);

        for (auto& b : j["bids"])
            ob.bids.push_back({ std::stod(b[0].get<std::string>()),
                                std::stod(b[1].get<std::string>()) });

        for (auto& a : j["asks"])
            ob.asks.push_back({ std::stod(a[0].get<std::string>()),
                                std::stod(a[1].get<std::string>()) });

        return ob;
    }
};

class GeminiExchange : public Exchange {
    RateLimiter limiter;

public:
    OrderBook fetch() override {
        OrderBook ob;
        if (!limiter.allow()) return ob;

        CURL* curl = curl_easy_init();
        std::string response;

        curl_easy_setopt(curl, CURLOPT_URL,
            "https://api.gemini.com/v1/book/BTCUSD");
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteCallback);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);

        curl_easy_perform(curl);
        curl_easy_cleanup(curl);

        auto j = json::parse(response);

        for (auto& b : j["bids"])
            ob.bids.push_back({ std::stod(b["price"].get<std::string>()),
                                std::stod(b["amount"].get<std::string>()) });

        for (auto& a : j["asks"])
            ob.asks.push_back({ std::stod(a["price"].get<std::string>()),
                                std::stod(a["amount"].get<std::string>()) });

        return ob;
    }
};

double executeBuy(std::vector<Order>& asks, double qty) {
    std::sort(asks.begin(), asks.end(),
              [](auto& a, auto& b) { return a.price < b.price; });

    double cost = 0;
    for (auto& o : asks) {
        double take = std::min(qty, o.qty);
        cost += take * o.price;
        qty -= take;
        if (qty <= 0) break;
    }
    return cost;
}

double executeSell(std::vector<Order>& bids, double qty) {
    std::sort(bids.begin(), bids.end(),
              [](auto& a, auto& b) { return a.price > b.price; });

    double revenue = 0;
    for (auto& o : bids) {
        double take = std::min(qty, o.qty);
        revenue += take * o.price;
        qty -= take;
        if (qty <= 0) break;
    }
    return revenue;
}

int main(int argc, char* argv[]) {
    double qty = 10.0;
    if (argc == 3 && std::string(argv[1]) == "--qty")
        qty = std::stod(argv[2]);

    CoinbaseExchange coinbase;
    GeminiExchange gemini;

    OrderBook cb = coinbase.fetch();
    OrderBook gm = gemini.fetch();

    std::vector<Order> bids, asks;
    bids.insert(bids.end(), cb.bids.begin(), cb.bids.end());
    bids.insert(bids.end(), gm.bids.begin(), gm.bids.end());
    asks.insert(asks.end(), cb.asks.begin(), cb.asks.end());
    asks.insert(asks.end(), gm.asks.begin(), gm.asks.end());


    double buyCost = executeBuy(asks, qty);
    double sellRevenue = executeSell(bids, qty);

    std::cout << "To buy " << qty << " BTC: $" << buyCost << "\n";
    std::cout << "To sell " << qty << " BTC: $" << sellRevenue << "\n";
}
