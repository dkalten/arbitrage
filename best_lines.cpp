// Moneylines and spreads for upcoming NBA, NFL, NHL and NCAAF games across Novig, Kalshi and
// Polymarket US: a menu offers the best book for each game's lines, the top arbitrage
// opportunities, or the best hedged use of a bonus bet, a cash bet, a safety net bet or a profit
// boost.
//
// Every price is what a taker pays now, with that book's trading fee included.

#ifdef _WIN32
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#define _CRT_SECURE_NO_WARNINGS
#include <windows.h>
#include <winhttp.h>
#else
#include <curl/curl.h>
#endif

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <format>
#include <functional>
#include <future>
#include <iostream>
#include <iterator>
#include <limits>
#include <memory>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <tuple>
#include <utility>
#include <variant>
#include <vector>

using Day = std::chrono::sys_days;
using Seconds = std::int64_t;  // Unix time

constexpr std::string_view NOVIG = "https://api.novig.com/v3/public/catalog";
constexpr std::string_view KALSHI = "https://api.elections.kalshi.com/trade-api/v2";
constexpr std::string_view POLYMARKET = "https://gateway.polymarket.us";

constexpr std::array<std::string_view, 12> MONTHS{"JAN", "FEB", "MAR", "APR", "MAY", "JUN",
                                                  "JUL", "AUG", "SEP", "OCT", "NOV", "DEC"};
// Kalshi's general taker coefficient; each series scales it by its fee_multiplier.
constexpr double KALSHI_TAKER_FEE = 0.07;
constexpr std::size_t POLYMARKET_PAGE = 200;
// Polymarket US allows 25 requests a second per IP.
constexpr std::size_t BOOK_READERS = 4;

// Own is the user's own sportsbook: never fetched, only priced from odds they type in.
enum class Book { Novig, Polymarket, Kalshi, Own };
constexpr std::array BOOKS{Book::Novig, Book::Polymarket, Book::Kalshi};
// How many hedged bets options 3 to 6 show at a time, whichever books they are with.
constexpr std::size_t HEDGED_BETS_SHOWN = 5;
// What a profit boost token adds to a winning bet's winnings, as a share of them, unless the
// user enters another.
constexpr double PROFIT_BOOST = 1.0;

constexpr std::string_view name(Book book) {
    constexpr std::string_view names[]{"Novig", "Polymarket", "Kalshi", "your book"};
    return names[static_cast<int>(book)];
}

struct League {
    std::string_view name;  // also Novig's name for it
    std::string_view kalshi_winners;
    std::string_view kalshi_spreads;
    std::string_view polymarket_tag;
    std::string_view polymarket_sport;  // prefix of its sportsMarketType values
};

constexpr std::array<League, 4> LEAGUES{{
    {"NBA", "KXNBAGAME", "KXNBASPREAD", "nba", "basketball"},
    {"NFL", "KXNFLGAME", "KXNFLSPREAD", "nfl", "football"},
    {"NHL", "KXNHLGAME", "KXNHLSPREAD", "nhl", "hockey"},
    {"NCAAF", "KXNCAAFGAME", "KXNCAAFSPREAD", "cfb", "football"},
}};

struct Team {
    std::vector<std::string> abbreviations;
    std::vector<std::string> names;
};

struct Quote {
    Book book;
    bool home;                       // backs the home side
    std::optional<double> handicap;  // that side's own spread; none on a moneyline
    double price;                    // cost of $1 of payout, before fees
    std::optional<double> size;      // dollars of payout offered at that price; none until read from a book
    double fee_rate;                 // every book charges fee_rate * price * (1 - price) per $1 of payout
    std::string polymarket_slug;     // empty on the other books
    bool polymarket_long = false;

    double net() const { return price + fee_rate * price * (1 - price); }
    double stake() const { return net() * *size; }

    // Spread lines are keyed by the away side's handicap; the moneyline has none.
    std::optional<double> line() const {
        return handicap && home ? std::optional(-*handicap) : handicap;
    }
};

struct Game {
    std::string league;
    Day day;
    Team away;
    Team home;
    std::vector<Quote> quotes;
    std::optional<Seconds> start;
    bool live = false;
};

struct Window {
    Seconds start;
    Seconds end;
    Day first_day;
    int days;
};

struct Args {
    std::vector<std::string> leagues;
    int days = 7;
    int top = 10;
    std::optional<double> polymarket_fee;
    double min_stake = 10;
    int favorite_cap = 800;  // American odds, without the minus sign
    double conversion_rate = 0.70;
};

enum class BetKind { Bonus, Cash, SafetyNet, Boosted };

constexpr std::string_view name(BetKind kind) {
    constexpr std::string_view names[]{"Bonus bet", "Cash bet", "Safety net bet", "Boosted bet"};
    return names[static_cast<int>(kind)];
}

struct Wager {
    double amount;
    std::optional<double> hedge_cap;  // the most the hedge may stake, if limited
    std::optional<double> min_odds;   // the most the bet may cost per $1 of payout, if limited
    BetKind kind;
    double boost;  // share added to the winnings; 0 unless the bet is boosted
};

struct HttpError : std::runtime_error {
    using std::runtime_error::runtime_error;
};

struct EndOfInput {};

template <typename... Values>
void say(std::format_string<Values...> pattern, Values&&... values) {
    std::cout << std::format(pattern, std::forward<Values>(values)...) << '\n';
}

// Prices from different books meet in comparisons, so keep float noise out of them.
double round9(double value) {
    return std::round(value * 1e9) / 1e9;
}

double parse_number(std::string_view text) {
    std::string_view digits = text;
    if (digits.starts_with('+'))
        digits.remove_prefix(1);
    double value = 0;
    const char* end = digits.data() + digits.size();
    auto [parsed_to, error] = std::from_chars(digits.data(), end, value);
    if (error != std::errc{} || parsed_to != end)
        throw std::runtime_error(std::format("'{}' is not a number", text));
    return value;
}

class Json {
public:
    using Array = std::vector<Json>;
    using Object = std::vector<std::pair<std::string, Json>>;

    static Json parse(std::string_view text) {
        std::size_t at = 0;
        Json value = parse_value(text, at);
        skip_space(text, at);
        if (at != text.size())
            throw std::runtime_error("unexpected text after the JSON response");
        return value;
    }

    const Json* find(std::string_view key) const {
        if (const Object* object = std::get_if<Object>(&value_))
            for (const auto& [member, value] : *object)
                if (member == key)
                    return &value;
        return nullptr;
    }

    const Json& at(std::string_view key) const {
        if (const Json* value = find(key))
            return *value;
        throw std::runtime_error(std::format("no '{}' in the response", key));
    }

    Json& at(std::string_view key) { return const_cast<Json&>(std::as_const(*this).at(key)); }

    const Array& items() const {
        if (const Array* array = std::get_if<Array>(&value_))
            return *array;
        throw std::runtime_error("expected a list in the response");
    }

    Array& items() { return const_cast<Array&>(std::as_const(*this).items()); }

    // A string, or a number as it was written.
    const std::string& text() const {
        if (const std::string* string = std::get_if<std::string>(&value_))
            return *string;
        if (const Number* number = std::get_if<Number>(&value_))
            return number->text;
        throw std::runtime_error("expected a string or number in the response");
    }

    // The APIs send some amounts as numbers and some as strings.
    double number() const { return parse_number(text()); }

    bool is(std::string_view expected) const {
        const std::string* string = std::get_if<std::string>(&value_);
        return string && *string == expected;
    }

    bool is_null() const { return std::holds_alternative<std::monostate>(value_); }

    bool truthy() const {
        struct Truth {
            bool operator()(std::monostate) const { return false; }
            bool operator()(bool value) const { return value; }
            bool operator()(const Number& number) const { return parse_number(number.text) != 0; }
            bool operator()(const std::string& string) const { return !string.empty(); }
            bool operator()(const Array& array) const { return !array.empty(); }
            bool operator()(const Object& object) const { return !object.empty(); }
        };
        return std::visit(Truth{}, value_);
    }

private:
    struct Number {
        std::string text;
    };

    static char peek(std::string_view text, std::size_t at) { return at < text.size() ? text[at] : '\0'; }

    static void skip_space(std::string_view text, std::size_t& at) {
        while (at < text.size() && (text[at] == ' ' || text[at] == '\n' || text[at] == '\r' || text[at] == '\t'))
            ++at;
    }

    static void expect(std::string_view text, std::size_t& at, char wanted) {
        skip_space(text, at);
        if (peek(text, at) != wanted)
            throw std::runtime_error(std::format("malformed JSON: expected '{}' at byte {}", wanted, at));
        ++at;
    }

    static unsigned parse_hex4(std::string_view text, std::size_t& at) {
        unsigned code = 0;
        if (at + 4 > text.size() || std::from_chars(text.data() + at, text.data() + at + 4, code, 16).ptr != text.data() + at + 4)
            throw std::runtime_error("malformed JSON: bad \\u escape");
        at += 4;
        return code;
    }

    static void append_utf8(std::string& out, unsigned code) {
        if (code < 0x80) {
            out += static_cast<char>(code);
        } else if (code < 0x800) {
            out += static_cast<char>(0xC0 | code >> 6);
            out += static_cast<char>(0x80 | (code & 0x3F));
        } else if (code < 0x10000) {
            out += static_cast<char>(0xE0 | code >> 12);
            out += static_cast<char>(0x80 | (code >> 6 & 0x3F));
            out += static_cast<char>(0x80 | (code & 0x3F));
        } else {
            out += static_cast<char>(0xF0 | code >> 18);
            out += static_cast<char>(0x80 | (code >> 12 & 0x3F));
            out += static_cast<char>(0x80 | (code >> 6 & 0x3F));
            out += static_cast<char>(0x80 | (code & 0x3F));
        }
    }

    static std::string parse_string(std::string_view text, std::size_t& at) {
        expect(text, at, '"');
        std::string out;
        for (;;) {
            std::size_t stop = text.find_first_of("\"\\", at);
            if (stop == std::string_view::npos)
                throw std::runtime_error("malformed JSON: unterminated string");
            out += text.substr(at, stop - at);
            at = stop + 1;
            if (text[stop] == '"')
                return out;
            char escape = peek(text, at++);
            switch (escape) {
                case 'b': out += '\b'; break;
                case 'f': out += '\f'; break;
                case 'n': out += '\n'; break;
                case 'r': out += '\r'; break;
                case 't': out += '\t'; break;
                case 'u': {
                    unsigned code = parse_hex4(text, at);
                    bool high_surrogate = code >= 0xD800 && code < 0xDC00;
                    if (high_surrogate && text.substr(at, 2) == "\\u") {
                        at += 2;
                        code = 0x10000 + ((code - 0xD800) << 10) + (parse_hex4(text, at) - 0xDC00);
                    }
                    append_utf8(out, code);
                    break;
                }
                default: out += escape;
            }
        }
    }

    static Json parse_value(std::string_view text, std::size_t& at) {
        skip_space(text, at);
        Json json;
        char first = peek(text, at);
        if (first == '{') {
            ++at;
            Object object;
            skip_space(text, at);
            if (peek(text, at) == '}') {
                ++at;
            } else {
                for (;;) {
                    skip_space(text, at);
                    std::string key = parse_string(text, at);
                    expect(text, at, ':');
                    object.emplace_back(std::move(key), parse_value(text, at));
                    skip_space(text, at);
                    if (peek(text, at) != ',')
                        break;
                    ++at;
                }
                expect(text, at, '}');
            }
            json.value_ = std::move(object);
        } else if (first == '[') {
            ++at;
            Array array;
            skip_space(text, at);
            if (peek(text, at) == ']') {
                ++at;
            } else {
                for (;;) {
                    array.push_back(parse_value(text, at));
                    skip_space(text, at);
                    if (peek(text, at) != ',')
                        break;
                    ++at;
                }
                expect(text, at, ']');
            }
            json.value_ = std::move(array);
        } else if (first == '"') {
            json.value_ = parse_string(text, at);
        } else if (text.substr(at, 4) == "true") {
            at += 4;
            json.value_ = true;
        } else if (text.substr(at, 5) == "false") {
            at += 5;
            json.value_ = false;
        } else if (text.substr(at, 4) == "null") {
            at += 4;
        } else {
            std::size_t start = at;
            while (at < text.size() && std::string_view("+-.eE0123456789").contains(text[at]))
                ++at;
            if (at == start)
                throw std::runtime_error(std::format("malformed JSON at byte {}", at));
            json.value_ = Number{std::string(text.substr(start, at - start))};
        }
        return json;
    }

    std::variant<std::monostate, bool, Number, std::string, Array, Object> value_;
};

struct Response {
    long status = 0;
    std::string body;
};

#ifdef _WIN32
struct HandleCloser {
    void operator()(HINTERNET handle) const { WinHttpCloseHandle(handle); }
};
using Handle = std::unique_ptr<void, HandleCloser>;

HINTERNET http_session() {
    static const Handle session = [] {
        Handle opened(WinHttpOpen(L"best-lines", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME,
                                  WINHTTP_NO_PROXY_BYPASS, 0));
        if (!opened)
            throw HttpError(std::format("cannot start WinHTTP (error {})", GetLastError()));
        WinHttpSetTimeouts(opened.get(), 30000, 30000, 30000, 30000);
        return opened;
    }();
    return session.get();
}

Response http_request(const std::string& url, const std::string& host_name) {
    std::size_t path_start = url.find('/', url.find("://") + 3);
    std::wstring host(host_name.begin(), host_name.end());
    std::wstring path(url.begin() + path_start, url.end());

    Handle connection(WinHttpConnect(http_session(), host.c_str(), INTERNET_DEFAULT_HTTPS_PORT, 0));
    Handle request(connection ? WinHttpOpenRequest(connection.get(), L"GET", path.c_str(), nullptr,
                                                   WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                                   WINHTTP_FLAG_SECURE)
                              : nullptr);
    auto fail = [&] { return HttpError(std::format("{}: network error {}", host_name, GetLastError())); };
    if (!request)
        throw fail();
    // With this set, WinHTTP asks for gzip and inflates the body itself.
    DWORD decompression = WINHTTP_DECOMPRESSION_FLAG_GZIP | WINHTTP_DECOMPRESSION_FLAG_DEFLATE;
    WinHttpSetOption(request.get(), WINHTTP_OPTION_DECOMPRESSION, &decompression, sizeof decompression);
    if (!WinHttpSendRequest(request.get(), WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ||
        !WinHttpReceiveResponse(request.get(), nullptr))
        throw fail();

    DWORD status = 0;
    DWORD status_size = sizeof status;
    if (!WinHttpQueryHeaders(request.get(), WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                             WINHTTP_HEADER_NAME_BY_INDEX, &status, &status_size, WINHTTP_NO_HEADER_INDEX))
        throw fail();

    Response response{static_cast<long>(status), {}};
    for (;;) {
        char buffer[16384];
        DWORD read = 0;
        if (!WinHttpReadData(request.get(), buffer, sizeof buffer, &read))
            throw fail();
        if (read == 0)
            return response;
        response.body.append(buffer, read);
    }
}
#else
Response http_request(const std::string& url, const std::string& host_name) {
    static const bool started = curl_global_init(CURL_GLOBAL_DEFAULT) == CURLE_OK;
    // A handle keeps its connections open, so each thread reuses its own.
    thread_local const std::unique_ptr<CURL, void (*)(CURL*)> request(started ? curl_easy_init() : nullptr,
                                                                      curl_easy_cleanup);
    if (!request)
        throw HttpError("cannot start libcurl");

    Response response;
    auto append = +[](char* data, std::size_t, std::size_t size, void* body) {
        static_cast<std::string*>(body)->append(data, size);
        return size;
    };
    curl_easy_setopt(request.get(), CURLOPT_URL, url.c_str());
    curl_easy_setopt(request.get(), CURLOPT_USERAGENT, "best-lines");
    // An empty list asks for every encoding libcurl can inflate itself.
    curl_easy_setopt(request.get(), CURLOPT_ACCEPT_ENCODING, "");
    curl_easy_setopt(request.get(), CURLOPT_CONNECTTIMEOUT, 30L);
    // Give up on a transfer that stalls for 30 seconds.
    curl_easy_setopt(request.get(), CURLOPT_LOW_SPEED_LIMIT, 1L);
    curl_easy_setopt(request.get(), CURLOPT_LOW_SPEED_TIME, 30L);
    // Otherwise libcurl times out DNS lookups with SIGALRM, which is unsafe across threads.
    curl_easy_setopt(request.get(), CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(request.get(), CURLOPT_WRITEFUNCTION, append);
    curl_easy_setopt(request.get(), CURLOPT_WRITEDATA, &response.body);
    CURLcode result = curl_easy_perform(request.get());
    if (result != CURLE_OK)
        throw HttpError(std::format("{}: {}", host_name, curl_easy_strerror(result)));
    curl_easy_getinfo(request.get(), CURLINFO_RESPONSE_CODE, &response.status);
    return response;
}
#endif

std::string http_get(const std::string& url) {
    std::size_t host_start = url.find("://") + 3;
    std::string host_name = url.substr(host_start, url.find('/', host_start) - host_start);
    for (int attempt = 0;; ++attempt) {
        Response response = http_request(url, host_name);
        if (response.status == 429 && attempt < 3) {
            std::this_thread::sleep_for(std::chrono::seconds(1 << attempt));
            continue;
        }
        if (response.status < 200 || response.status >= 300)
            throw HttpError(std::format("HTTP {} from {}", response.status, host_name));
        return std::move(response.body);
    }
}

Json get_json(const std::string& url) {
    return Json::parse(http_get(url));
}

using Params = std::vector<std::pair<std::string, std::string>>;

std::string query_string(const Params& params) {
    std::string query;
    for (const auto& [key, value] : params) {
        if (!query.empty())
            query += '&';
        query += key + '=';
        for (unsigned char c : value) {
            if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~')
                query += static_cast<char>(c);
            else
                query += std::format("%{:02X}", c);
        }
    }
    return query;
}

void set_param(Params& params, const std::string& key, const std::string& value) {
    auto found = std::ranges::find(params, key, &Params::value_type::first);
    if (found == params.end())
        params.emplace_back(key, value);
    else
        found->second = value;
}

std::string upper(std::string text) {
    for (char& c : text)
        c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return text;
}

std::string strip(std::string_view text) {
    constexpr std::string_view space = " \t\r\n";
    std::size_t first = text.find_first_not_of(space);
    if (first == std::string_view::npos)
        return {};
    return std::string(text.substr(first, text.find_last_not_of(space) - first + 1));
}

Day game_day(Seconds start) {
    // US evening games run past midnight UTC, so date a game six hours behind it. That matches
    // the day in Kalshi's tickers, the only date Kalshi gives.
    return std::chrono::floor<std::chrono::days>(std::chrono::sys_seconds(std::chrono::seconds(start)) -
                                                 std::chrono::hours(6));
}

std::string utc_time(Seconds when) {
    return std::format("{:%Y-%m-%dT%H:%M:%SZ}", std::chrono::sys_seconds(std::chrono::seconds(when)));
}

// 2026-11-18T23:00:00Z, optionally with fractional seconds or a +hh:mm offset.
Seconds parse_iso_time(const std::string& text) {
    int year = 0, month = 0, day = 0, hour = 0, minute = 0, second = 0, consumed = 0;
    if (std::sscanf(text.c_str(), "%d-%d-%dT%d:%d:%d%n", &year, &month, &day, &hour, &minute, &second, &consumed) < 6)
        throw std::runtime_error(std::format("'{}' is not a time", text));
    const char* rest = text.c_str() + consumed;
    if (*rest == '.')
        for (++rest; std::isdigit(static_cast<unsigned char>(*rest));)
            ++rest;
    int offset = 0;
    if (*rest == '+' || *rest == '-') {
        int offset_hours = 0, offset_minutes = 0;
        std::sscanf(rest + 1, "%d:%d", &offset_hours, &offset_minutes);
        offset = (offset_hours * 3600 + offset_minutes * 60) * (*rest == '-' ? -1 : 1);
    }
    Day date = std::chrono::year(year) / month / day;
    return std::chrono::seconds(date.time_since_epoch()).count() + hour * 3600 + minute * 60 + second - offset;
}

std::vector<Game> novig(const League& league, const Window& window) {
    auto pages = [](std::string_view path, Params params) {
        std::vector<Json> items;
        for (;;) {
            Json page = get_json(std::format("{}/{}?{}", NOVIG, path, query_string(params)));
            for (Json& item : page.at("items").items())
                items.push_back(std::move(item));
            const Json* next = page.find("next");
            if (!next || next->is_null())
                return items;
            set_param(params, "after", next->text());
        }
    };

    // Both API bounds are exclusive Unix milliseconds.
    Params in_window{
        {"league", std::string(league.name)},
        {"startsAfter", std::to_string(window.start * 1000 - 1)},
        {"startsBefore", std::to_string(window.end * 1000)},
    };
    std::vector<Json> events = pages("events", in_window);
    Params market_params = in_window;
    market_params.emplace_back("marketType", "MONEY,SPREAD");
    market_params.emplace_back("limit", "5000");
    std::vector<Json> markets = pages("markets", market_params);

    std::vector<std::pair<std::string, const Json*>> markets_of;
    for (const Json& market : markets)
        markets_of.emplace_back(market.at("eventId").text(), &market);
    std::ranges::stable_sort(markets_of, {}, &std::pair<std::string, const Json*>::first);

    struct Offer {
        std::string abbreviation;
        std::optional<double> handicap;
        double price;
        double size;
        double fee_rate;
    };
    struct Side {
        std::string abbreviation;
        std::optional<double> handicap;
        const Json* outcome;
    };

    std::vector<Game> games;
    for (const Json& event : events) {
        bool live = event.at("status").is("OPEN_INGAME");
        std::vector<Offer> offers;
        std::optional<std::string> home_abbreviation;
        auto of_event = std::ranges::equal_range(markets_of, event.at("eventId").text(), {},
                                                 &std::pair<std::string, const Json*>::first);
        for (const auto& [event_id, market_ptr] : of_event) {
            const Json& market = *market_ptr;
            const Json& fee = market.at("fee");
            bool charged = fee.at("charged").is("ALWAYS") || (fee.at("charged").is("WHEN_LIVE") && live);
            double fee_rate = charged ? fee.at("coefficient").number() : 0;

            std::vector<Side> sides;
            for (const Json& outcome : market.at("outcomes").items()) {
                const std::string& outcome_name = outcome.at("name").text();
                if (market.at("marketType").is("SPREAD")) {
                    // Named like "NYK -3.5"; strike is the home side's handicap.
                    std::size_t space = outcome_name.rfind(' ');
                    std::string abbreviation = space == std::string::npos ? "" : outcome_name.substr(0, space);
                    double handicap = parse_number(std::string_view(outcome_name).substr(space + 1));
                    if (handicap != 0 && handicap == market.at("strike").number())
                        home_abbreviation = abbreviation;
                    sides.push_back({abbreviation, handicap, &outcome});
                } else {
                    sides.push_back({outcome_name, std::nullopt, &outcome});
                }
            }
            if (sides.size() != 2)
                continue;
            // The book holds only bids: backing one outcome crosses the best bid on the other
            // at 1 - its price. A contract pays out 1 cent.
            for (int backed : {0, 1}) {
                const Json* bid = sides[1 - backed].outcome->find("bid");
                if (bid && bid->truthy())
                    offers.push_back({sides[backed].abbreviation, sides[backed].handicap,
                                      round9(1 - bid->at("price").number()), bid->at("qty").number() / 100,
                                      fee_rate});
            }
        }

        std::vector<std::string> abbreviations;
        for (const Offer& offer : offers)
            if (!std::ranges::contains(abbreviations, offer.abbreviation))
                abbreviations.push_back(offer.abbreviation);
        if (!home_abbreviation || abbreviations.size() != 2 || !std::ranges::contains(abbreviations, *home_abbreviation))
            continue;
        const std::string& away_abbreviation = abbreviations[abbreviations[0] == *home_abbreviation];

        Game game{.league = std::string(league.name)};
        const std::string& description = event.at("description").text();
        std::size_t at_sign = description.find(" @ ");
        if (at_sign != std::string::npos && at_sign + 3 < description.size()) {
            game.away.names = {description.substr(0, at_sign)};
            game.home.names = {description.substr(at_sign + 3)};
        }
        game.away.abbreviations = {away_abbreviation};
        game.home.abbreviations = {*home_abbreviation};
        for (const Offer& offer : offers)
            game.quotes.push_back({Book::Novig, offer.abbreviation == *home_abbreviation, offer.handicap,
                                   offer.price, offer.size, offer.fee_rate});
        game.start = static_cast<Seconds>(event.at("startsTs").number() / 1000);
        game.day = game_day(*game.start);
        game.live = live;
        games.push_back(std::move(game));
    }
    return games;
}

std::vector<Game> kalshi(const League& league, const Window& window) {
    // Event tickers look like KXNBAGAME-26NOV17NYKTOR: game day, away team, home team.
    std::vector<std::pair<std::string, Day>> days;
    for (int offset = 0; offset < window.days; ++offset) {
        Day day = window.first_day + std::chrono::days(offset);
        std::chrono::year_month_day date(day);
        days.emplace_back(std::format("{:02}{}{:02}", static_cast<int>(date.year()) % 100,
                                      MONTHS[static_cast<unsigned>(date.month()) - 1],
                                      static_cast<unsigned>(date.day())),
                          day);
    }
    auto day_of = [&](const std::string& event) {
        return std::ranges::find(days, event.substr(0, 7), &std::pair<std::string, Day>::first);
    };

    struct Listed {
        std::string event;
        Json market;
        double fee_rate;
    };
    auto open_markets = [&](std::string_view series) {
        double multiplier = get_json(std::format("{}/series/{}", KALSHI, series)).at("series").at("fee_multiplier").number();
        double fee_rate = KALSHI_TAKER_FEE * multiplier;
        std::vector<Listed> found;
        std::string cursor;
        for (;;) {
            Params query{{"series_ticker", std::string(series)}, {"status", "open"}, {"limit", "1000"}, {"cursor", cursor}};
            Json page = get_json(std::format("{}/markets?{}", KALSHI, query_string(query)));
            for (Json& market : page.at("markets").items()) {
                const std::string& ticker = market.at("event_ticker").text();
                std::size_t dash = ticker.find('-');
                if (dash == std::string::npos)
                    throw std::runtime_error(std::format("unexpected event ticker '{}'", ticker));
                std::string event = ticker.substr(dash + 1, ticker.find('-', dash + 1) - dash - 1);
                if (day_of(event) != days.end())
                    found.push_back({std::move(event), std::move(market), fee_rate});
            }
            const Json* next = page.find("cursor");
            if (!next || !next->truthy())
                return found;
            cursor = next->text();
        }
    };
    // The last ticker part is the Yes team, plus a line number on spreads: TOR, NYK4.
    auto yes_abbreviation = [](const Json& market) {
        const std::string& ticker = market.at("ticker").text();
        std::string last = ticker.substr(ticker.rfind('-') + 1);
        last.erase(last.find_last_not_of("0123456789") + 1);
        return last;
    };

    std::vector<Listed> winners = open_markets(league.kalshi_winners);
    std::vector<Listed> spreads = open_markets(league.kalshi_spreads);

    struct EventTeams {
        std::string event;
        std::vector<std::pair<std::string, std::string>> teams;  // abbreviation, name
    };
    std::vector<EventTeams> teams_in;
    for (const Listed& listed : winners) {
        auto found = std::ranges::find(teams_in, listed.event, &EventTeams::event);
        if (found == teams_in.end())
            found = teams_in.insert(teams_in.end(), {listed.event, {}});
        std::string abbreviation = yes_abbreviation(listed.market);
        const std::string& team_name = listed.market.at("yes_sub_title").text();
        auto team = std::ranges::find(found->teams, abbreviation, &std::pair<std::string, std::string>::first);
        if (team == found->teams.end())
            found->teams.emplace_back(abbreviation, team_name);
        else
            team->second = team_name;
    }

    std::vector<std::pair<std::string, Game>> games;
    for (const EventTeams& listing : teams_in) {
        if (listing.teams.size() != 2)
            continue;
        auto first = listing.teams[0], second = listing.teams[1];
        std::string matchup = listing.event.substr(7);
        if (matchup == second.first + first.first)
            std::swap(first, second);
        else if (matchup != first.first + second.first)
            continue;
        Game game{.league = std::string(league.name), .day = day_of(listing.event)->second};
        game.away = {{first.first}, {first.second}};
        game.home = {{second.first}, {second.second}};
        games.emplace_back(listing.event, std::move(game));
    }

    for (bool is_spread : {false, true}) {
        for (const Listed& listed : is_spread ? spreads : winners) {
            auto found = std::ranges::find(games, listed.event, &std::pair<std::string, Game>::first);
            if (found == games.end())
                continue;
            Game& game = found->second;
            std::string abbreviation = yes_abbreviation(listed.market);
            bool yes_is_home = abbreviation == game.home.abbreviations[0];
            if (!yes_is_home && abbreviation != game.away.abbreviations[0])
                continue;
            const Json& market = listed.market;
            // A spread market reads "<team> wins by over <floor_strike> points".
            std::optional<double> margin, yes_handicap;
            if (is_spread) {
                margin = market.at("floor_strike").number();
                yes_handicap = *margin == 0 ? 0 : -*margin;
            }
            // Buying No on a team is backing its opponent, against the resting Yes bids.
            struct Side {
                bool home;
                std::optional<double> handicap;
                const char* price;
                const char* size;
            };
            for (const Side& side : {Side{yes_is_home, yes_handicap, "yes_ask_dollars", "yes_ask_size_fp"},
                                     Side{!yes_is_home, margin, "no_ask_dollars", "yes_bid_size_fp"}}) {
                double size = market.at(side.size).number();
                if (size > 0)
                    game.quotes.push_back({Book::Kalshi, side.home, side.handicap, market.at(side.price).number(),
                                           size, listed.fee_rate});
            }
        }
    }

    std::vector<Game> listed_games;
    for (auto& [event, game] : games)
        listed_games.push_back(std::move(game));
    return listed_games;
}

std::vector<Game> polymarket(const League& league, const Window& window, std::optional<double> fee_override) {
    std::string winner_kind = std::format("{}_team_full_game_winner", league.polymarket_sport);
    std::string spread_kind = std::format("{}_team_full_game_spread", league.polymarket_sport);

    auto events = [&](const Params& filters) {
        Params query{
            {"tagSlug", std::string(league.polymarket_tag)},
            {"closed", "false"},
            {"startTimeMin", utc_time(window.start)},
            {"startTimeMax", utc_time(window.end)},
            {"limit", std::to_string(POLYMARKET_PAGE)},
        };
        query.insert(query.end(), filters.begin(), filters.end());
        std::vector<std::string> seen;
        std::vector<Json> found;
        for (;;) {
            set_param(query, "offset", std::to_string(seen.size()));
            Json response = get_json(std::format("{}/v1/events?{}", POLYMARKET, query_string(query)));
            Json::Array& page = response.at("events").items();
            std::size_t fresh = 0;
            for (Json& event : page) {
                const std::string& slug = event.at("slug").text();
                if (std::ranges::contains(seen, slug))
                    continue;
                seen.push_back(slug);
                found.push_back(std::move(event));
                ++fresh;
            }
            if (fresh == 0 || page.size() < POLYMARKET_PAGE)
                return found;
        }
    };

    auto quotes = [&](const Json& market, bool is_spread) {
        std::vector<Quote> found;
        if (!market.at("active").truthy() || market.at("closed").truthy())
            return found;
        const Json::Array& sides = market.at("marketSides").items();
        auto long_side = std::ranges::find_if(sides, [](const Json& side) { return side.at("long").truthy(); });
        if (long_side == sides.end())
            throw std::runtime_error("a Polymarket market has no long side");
        bool long_is_home = long_side->at("team").at("ordering").is("home");
        // One instrument per market: `line` is the long side's handicap, and backing the short
        // side means selling the long one into its bids.
        std::optional<double> line, short_line;
        if (is_spread) {
            line = market.at("line").number();
            short_line = *line == 0 ? 0 : -*line;
        }
        double fee_rate = fee_override && *fee_override != 0 ? *fee_override : market.at("feeCoefficient").number();
        const Json* ask = market.find("bestAskQuote");
        const Json* bid = market.find("bestBidQuote");
        std::optional<double> long_price, short_price;
        if (ask && ask->truthy())
            long_price = ask->at("value").number();
        if (bid && bid->truthy())
            short_price = round9(1 - bid->at("value").number());
        const std::string& slug = market.at("slug").text();
        if (long_price && *long_price > 0 && *long_price < 1)
            found.push_back({Book::Polymarket, long_is_home, line, *long_price, std::nullopt, fee_rate, slug, true});
        if (short_price && *short_price > 0 && *short_price < 1)
            found.push_back({Book::Polymarket, !long_is_home, short_line, *short_price, std::nullopt, fee_rate, slug, false});
        return found;
    };

    // Unfiltered, an event carries only its moneyline; filtered, only markets of that type.
    std::vector<std::pair<std::string, Game>> games;
    for (const Json& event : events({})) {
        const Json::Array& markets = event.at("markets").items();
        auto market = std::ranges::find_if(markets, [&](const Json& listed) {
            const Json* kind = listed.find("sportsMarketType");
            return kind && kind->is(winner_kind);
        });
        Seconds start = parse_iso_time(event.at("startTime").text());
        const Json* ended = event.find("ended");
        if (market == markets.end() || (ended && ended->truthy()) || start < window.start || start >= window.end)
            continue;

        std::optional<Team> away, home;
        bool unknown_side = false;
        for (const Json& side : market->at("marketSides").items()) {
            const Json* team = side.find("team");
            if (!team || !team->truthy())
                continue;
            Team listed{{upper(team->at("abbreviation").text())}, {team->at("name").text()}};
            if (team->at("ordering").is("away"))
                away = std::move(listed);
            else if (team->at("ordering").is("home"))
                home = std::move(listed);
            else
                unknown_side = true;
        }
        if (!away || !home || unknown_side)
            continue;

        const Json* live = event.find("live");
        Game game{std::string(league.name), game_day(start), *away, *home, quotes(*market, false), start,
                  live && live->truthy()};
        const std::string& slug = event.at("slug").text();
        auto known = std::ranges::find(games, slug, &std::pair<std::string, Game>::first);
        if (known == games.end())
            games.emplace_back(slug, std::move(game));
        else
            known->second = std::move(game);
    }
    for (const Json& event : events({{"sportsMarketTypes", spread_kind}})) {
        auto known = std::ranges::find(games, event.at("slug").text(), &std::pair<std::string, Game>::first);
        if (known == games.end())
            continue;
        for (const Json& market : event.at("markets").items()) {
            const Json* kind = market.find("sportsMarketType");
            if (kind && kind->is(spread_kind))
                for (Quote& quote : quotes(market, true))
                    known->second.quotes.push_back(std::move(quote));
        }
    }

    std::vector<Game> listed_games;
    for (auto& [slug, game] : games)
        listed_games.push_back(std::move(game));
    return listed_games;
}

using Words = std::vector<std::string>;

Words name_words(std::string_view team_name) {
    Words words;
    std::string word;
    for (char raw : std::string(team_name) + ' ') {
        char c = static_cast<char>(std::tolower(static_cast<unsigned char>(raw)));
        if (c == '&' || c == '\'')
            continue;
        if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9'))
            word += c;
        else if (!word.empty())
            words.push_back(std::exchange(word, {}));
    }
    if (!words.empty() && words.back() == "st")
        words.back() = "state";
    return words;
}

// Each word of `shortened` starts a later word of `full`: Edmonton, Oilers, New York G.
bool is_short_form(const Words& shortened, const Words& full) {
    std::size_t next = 0;
    for (const std::string& part : shortened) {
        while (next < full.size() && !full[next].starts_with(part))
            ++next;
        if (next++ >= full.size())
            return false;
    }
    return true;
}

// 3 for the same name, 2 for the same abbreviation, 1 for a shortened name, else 0.
int team_match(const Team& one, const Team& other) {
    bool same_name = false, short_name = false;
    for (const std::string& name_one : one.names) {
        Words a = name_words(name_one);
        for (const std::string& name_other : other.names) {
            Words b = name_words(name_other);
            if (a.empty() || b.empty())
                continue;
            same_name |= a == b;
            short_name |= a.size() <= b.size() ? is_short_form(a, b) : is_short_form(b, a);
        }
    }
    if (same_name)
        return 3;
    for (const std::string& a : one.abbreviations)
        for (const std::string& b : other.abbreviations)
            if (upper(a) == upper(b))
                return 2;
    return short_name;
}

// How surely two listings are the same game, given they share a league and day; 0 if not.
// A team plays once a day, so one exact name is enough. Otherwise both teams have to match.
int game_match(const std::array<const Team*, 2>& teams, const std::array<const Team*, 2>& targets) {
    int scores[2]{team_match(*teams[0], *targets[0]), team_match(*teams[1], *targets[1])};
    int best = std::max(scores[0], scores[1]);
    if ((scores[0] && scores[1]) || best == 3)
        return scores[0] + scores[1];
    // A lone shared abbreviation counts if the other side's at least start alike: WSH and WAS.
    auto alike = [](const Team& team, const Team& target) {
        for (const std::string& a : team.abbreviations)
            for (const std::string& b : target.abbreviations)
                if (!a.empty() && !b.empty() && std::toupper(static_cast<unsigned char>(a[0])) == std::toupper(static_cast<unsigned char>(b[0])))
                    return true;
        return false;
    };
    if (best == 2 && (scores[0] || alike(*teams[0], *targets[0])) && (scores[1] || alike(*teams[1], *targets[1])))
        return 2;
    return 0;
}

// Combine each book's games into one list, joining the listings of the same game. Books share
// no IDs and abbreviate teams differently, so games are paired by league, day and team names.
// Books can disagree on which side is home at a neutral site.
std::vector<Game> merge(std::vector<std::vector<Game>> books) {
    std::vector<Game> merged;
    for (std::vector<Game>& games : books) {
        struct Candidate {
            int score;
            bool flipped;
            std::size_t game;
            std::size_t target;
        };
        std::vector<Candidate> candidates;
        for (std::size_t g = 0; g < games.size(); ++g) {
            for (std::size_t t = 0; t < merged.size(); ++t) {
                const Game& game = games[g];
                const Game& target = merged[t];
                if (target.league != game.league || target.day != game.day)
                    continue;
                for (bool flipped : {false, true}) {
                    std::array<const Team*, 2> teams{&game.away, &game.home};
                    if (flipped)
                        std::swap(teams[0], teams[1]);
                    if (int score = game_match(teams, {&target.away, &target.home}))
                        candidates.push_back({score, flipped, g, t});
                }
            }
        }
        std::ranges::stable_sort(candidates, [](const Candidate& a, const Candidate& b) {
            return a.score != b.score ? a.score > b.score : a.flipped < b.flipped;
        });

        std::vector<char> game_paired(games.size()), target_paired(merged.size());
        for (const Candidate& candidate : candidates) {
            if (game_paired[candidate.game] || target_paired[candidate.target])
                continue;
            game_paired[candidate.game] = target_paired[candidate.target] = true;
            Game& game = games[candidate.game];
            Game& target = merged[candidate.target];
            if (candidate.flipped) {
                std::swap(game.away, game.home);
                for (Quote& quote : game.quotes)
                    quote.home = !quote.home;
            }
            for (auto [known, added] : {std::pair{&target.away, &game.away}, std::pair{&target.home, &game.home}}) {
                known->abbreviations.insert(known->abbreviations.end(), added->abbreviations.begin(), added->abbreviations.end());
                known->names.insert(known->names.end(), added->names.begin(), added->names.end());
            }
            target.quotes.insert(target.quotes.end(), game.quotes.begin(), game.quotes.end());
            // Novig lists some starts a few minutes late.
            if (game.start && (!target.start || *game.start < *target.start))
                target.start = game.start;
            target.live = target.live || game.live;
        }
        for (std::size_t g = 0; g < games.size(); ++g)
            if (!game_paired[g])
                merged.push_back(std::move(games[g]));
    }
    return merged;
}

struct Line {
    std::optional<double> handicap;  // the away side's; none on the moneyline
    std::vector<Quote*> away;
    std::vector<Quote*> home;
};

std::vector<Line> lines_of(Game& game) {
    std::vector<Line> lines;
    for (Quote& quote : game.quotes) {
        std::optional<double> handicap = quote.line();
        auto line = std::ranges::find(lines, handicap, &Line::handicap);
        if (line == lines.end())
            line = lines.insert(lines.end(), Line{handicap});
        (quote.home ? line->home : line->away).push_back(&quote);
    }
    return lines;
}

// Backing both sides of one line, each at its best price.
struct Opportunity {
    const Game* game;
    Quote* away;
    Quote* home;

    // What it costs to collect $1 whichever side wins.
    double cost() const { return round9(away->net() + home->net()); }
    double roi() const { return (1 - cost()) / cost(); }
};

Quote* best_offer(const std::vector<Quote*>& offers, double min_stake) {
    auto rank = [](const Quote* quote) { return std::tuple(quote->net(), !quote->size, -quote->size.value_or(0)); };
    Quote* best = nullptr;
    for (Quote* quote : offers) {
        bool usable = !quote->size || (*quote->size > 0 && quote->stake() >= min_stake);
        if (usable && (!best || rank(quote) < rank(best)))
            best = quote;
    }
    return best;
}

Json read_polymarket_book(const std::string& slug) {
    try {
        return get_json(std::format("{}/v1/markets/{}/book", POLYMARKET, slug)).at("marketData");
    } catch (const std::exception&) {
        return {};
    }
}

std::vector<Json> read_polymarket_books(const std::vector<std::string>& slugs) {
    std::vector<Json> books(slugs.size());
    std::atomic<std::size_t> next = 0;
    std::vector<std::jthread> readers;
    for (std::size_t reader = 0; reader < std::min(BOOK_READERS, slugs.size()); ++reader)
        readers.emplace_back([&] {
            for (std::size_t index; (index = next++) < slugs.size();)
                books[index] = read_polymarket_book(slugs[index]);
        });
    readers.clear();
    return books;
}

// Polymarket's event listings quote no size. Read the order book of each of its markets among
// the quotes `select()` returns, and select again on the books' own prices, until every
// selected quote has a size.
void size_polymarket(std::vector<Game>& games, const std::function<std::vector<Quote*>()>& select) {
    std::vector<Quote*> listed;
    for (Game& game : games)
        for (Quote& quote : game.quotes)
            if (!quote.polymarket_slug.empty())
                listed.push_back(&quote);
    std::ranges::stable_sort(listed, {}, &Quote::polymarket_slug);

    int unreadable = 0;
    for (;;) {
        std::vector<std::string> slugs;
        for (const Quote* quote : select())
            if (!quote->size)
                slugs.push_back(quote->polymarket_slug);
        std::ranges::sort(slugs);
        auto repeated = std::ranges::unique(slugs);
        slugs.erase(repeated.begin(), repeated.end());
        if (slugs.empty())
            break;

        std::vector<Json> books = read_polymarket_books(slugs);
        for (std::size_t index = 0; index < slugs.size(); ++index) {
            const Json& book = books[index];
            unreadable += !book.truthy();
            for (Quote* quote : std::ranges::equal_range(listed, slugs[index], {}, &Quote::polymarket_slug)) {
                const Json* levels = book.find(quote->polymarket_long ? "offers" : "bids");
                if (levels && levels->truthy()) {
                    const Json& top = levels->items().front();
                    double top_price = top.at("px").at("value").number();
                    quote->price = quote->polymarket_long ? top_price : round9(1 - top_price);
                    quote->size = top.at("qty").number();
                } else {
                    quote->size = 0;
                }
            }
        }
    }
    if (unreadable)
        std::cerr << std::format("{} Polymarket order books could not be read and were left out.\n", unreadable);
}

// The spread line whose best prices on the two sides are closest to even.
const Line* main_line(const std::vector<Line>& lines) {
    auto best_net = [](const std::vector<Quote*>& quotes) {
        return (*std::ranges::min_element(quotes, {}, &Quote::net))->net();
    };
    const Line* main = nullptr;
    double main_gap = 0;
    for (const Line& line : lines) {
        if (!line.handicap || line.away.empty() || line.home.empty())
            continue;
        double gap = round9(std::abs(best_net(line.home) - best_net(line.away)));
        if (!main || gap < main_gap) {
            main = &line;
            main_gap = gap;
        }
    }
    return main;
}

// The lines that return the most from backing both sides, best first.
std::vector<Opportunity> top_opportunities(std::vector<Game>& games, std::size_t count, double min_stake) {
    // Equal returns keep the order the away sides were first quoted in.
    std::vector<std::pair<const Game*, Line>> lines;
    for (Game& game : games) {
        std::vector<Line> two_sided = lines_of(game);
        std::erase_if(two_sided, [](const Line& line) { return line.away.empty() || line.home.empty(); });
        std::ranges::stable_sort(two_sided, {}, [](const Line& line) { return line.away.front(); });
        for (Line& line : two_sided)
            lines.emplace_back(&game, std::move(line));
    }

    auto ranked = [&] {
        std::vector<Opportunity> found;
        for (const auto& [game, line] : lines) {
            Quote* away = best_offer(line.away, min_stake);
            Quote* home = best_offer(line.home, min_stake);
            if (away && home)
                found.push_back({game, away, home});
        }
        std::ranges::stable_sort(found, std::greater{}, &Opportunity::roi);
        found.resize(std::min(found.size(), count));
        return found;
    };

    size_polymarket(games, [&] {
        std::vector<Quote*> legs;
        for (const Opportunity& top : ranked())
            legs.insert(legs.end(), {top.away, top.home});
        return legs;
    });
    return ranked();
}

// A bet on one side of a line, hedged with cash on the other.
struct HedgedBet {
    const Game* game;
    Quote* bet;
    Quote* hedge;
    double amount;
    // Cash gone if the bet loses: none of a bonus bet, all of a cash or boosted bet, and a safety
    // net bet's stake less what the bonus bets it comes back as are worth.
    double at_risk;
    double boost;  // share added to the winnings; 0 unless the bet is boosted

    double winnings() const { return amount * (1 - bet->net()) / bet->net() * (1 + boost); }
    // What the hedge has to collect for the profit to be the same whichever side wins.
    double hedge_payout() const { return winnings() + at_risk; }
    double hedge_stake() const { return hedge_payout() * hedge->net(); }
    double profit() const { return winnings() - hedge_stake(); }
};

// The HEDGED_BETS_SHOWN hedged bets that profit the most, best first, leaving out the bets already
// `shown`. Neither price may be beyond the favorite cap nor the bet's above the wager's minimum
// odds, both need enough behind them for the whole bet, and the hedge may not stake more than the
// wager's hedge cap, if it has one.
std::vector<HedgedBet> best_hedged_bets(std::vector<Game>& games, const Wager& wager, const Args& args,
                                        const std::vector<HedgedBet>& shown) {
    double at_risk = wager.kind == BetKind::Bonus       ? 0
                     : wager.kind == BetKind::SafetyNet ? wager.amount * (1 - args.conversion_rate)
                                                        : wager.amount;
    double dearest = args.favorite_cap / (args.favorite_cap + 100.0);
    double bet_dearest = round9(std::min(dearest, wager.min_odds.value_or(dearest)));
    struct Side {
        const Game* game;
        std::vector<Quote*> offers;
        std::vector<Quote*> hedges;
    };
    std::vector<Side> sides;
    for (Game& game : games) {
        std::vector<Side> of_game;
        for (Line& line : lines_of(game)) {
            if (!line.away.empty() && !line.home.empty()) {
                of_game.push_back({&game, line.away, line.home});
                of_game.push_back({&game, line.home, line.away});
            }
        }
        // Equal profits keep the order the sides were first quoted in.
        std::ranges::stable_sort(of_game, {}, [](const Side& side) { return side.offers.front(); });
        std::ranges::move(of_game, std::back_inserter(sides));
    }

    // Kalshi quotes a side twice, as Yes on it and No on its opponent: only the better is shown.
    auto among = [](const std::vector<HedgedBet>& bets, const Game* game, const Quote* bet) {
        return std::ranges::any_of(bets, [&](const HedgedBet& other) {
            return other.game == game && other.bet->book == bet->book && other.bet->home == bet->home &&
                   other.bet->handicap == bet->handicap;
        });
    };

    // `depth` times as many as are shown.
    auto best = [&](std::size_t depth) {
        std::vector<HedgedBet> found;
        for (const Side& side : sides)
            for (Quote* bet : side.offers) {
                if (among(shown, side.game, bet))
                    continue;
                if (round9(bet->net()) > bet_dearest || (bet->size && bet->stake() < wager.amount))
                    continue;
                HedgedBet hedged{side.game, bet, nullptr, wager.amount, at_risk, wager.boost};
                double payout = hedged.hedge_payout();
                for (Quote* quote : side.hedges)
                    if (quote->net() <= dearest && (!quote->size || *quote->size >= payout) &&
                        (!hedged.hedge || quote->net() < hedged.hedge->net()))
                        hedged.hedge = quote;
                // The cheapest hedge also stakes the least, so no other one fits a cap it breaks.
                if (hedged.hedge && (!wager.hedge_cap || round9(hedged.hedge_stake()) <= *wager.hedge_cap))
                    found.push_back(hedged);
            }
        std::ranges::stable_sort(found, std::greater{}, &HedgedBet::profit);
        std::vector<HedgedBet> kept;
        for (const HedgedBet& hedged : found) {
            if (kept.size() == depth * HEDGED_BETS_SHOWN)
                break;
            if (!among(kept, hedged.game, hedged.bet))
                kept.push_back(hedged);
        }
        return kept;
    };

    // Usually the leaders fit. When they turn out too small, size ever more runners-up a round
    // rather than one market per round trip.
    int round = 0;
    size_polymarket(games, [&] {
        std::size_t depth = round == 0 ? 2 : round == 1 ? 8 : 40;
        ++round;
        std::vector<Quote*> legs;
        for (const HedgedBet& found : best(depth))
            legs.insert(legs.end(), {found.bet, found.hedge});
        return legs;
    });
    return best(1);
}

// Amounts that are exactly halfway in decimal, such as $188.595, round to even as they do in
// the Python app; a double alone lands a hair to either side of the half.
std::string fixed(double value, int decimals) {
    double scale = std::pow(10.0, decimals);
    double scaled = std::round(value * scale * 1e6) / 1e6;
    return std::format("{:.{}f}", std::nearbyint(scaled) / scale, decimals);
}

std::string american(double price) {
    if (price > 0.5)
        return fixed(-100 * price / (1 - price), 0);
    return "+" + fixed(100 * (1 - price) / price, 0);
}

// American odds, then decimal odds: "-500, 1.20x".
std::string odds(double price) {
    return std::format("{}, {}x", american(price), fixed(1 / price, 2));
}

std::string money(double amount, int decimals = 2) {
    std::string text = fixed(amount, decimals);
    std::size_t whole_end = decimals ? text.find('.') : text.size();
    for (std::size_t at = whole_end; at > 3 && std::isdigit(static_cast<unsigned char>(text[at - 4])); at -= 3)
        text.insert(at - 3, ",");
    return "$" + text;
}

std::string handicap_text(double handicap) {
    return std::format("{:+g}", handicap == 0 ? 0.0 : handicap);
}

std::string side_label(const Game& game, bool home, std::optional<double> handicap) {
    std::string label = (home ? game.home : game.away).abbreviations[0];
    if (handicap)
        label += " " + handicap_text(*handicap);
    return label;
}

std::string side_label(const Game& game, const Quote& quote) {
    return side_label(game, quote.home, quote.handicap);
}

std::tm local_tm(std::time_t time) {
    std::tm local{};
#ifdef _WIN32
    localtime_s(&local, &time);
#else
    localtime_r(&time, &local);
#endif
    return local;
}

std::string local_time(Seconds when, const char* pattern) {
    std::tm local = local_tm(when);
    char text[64];
    std::strftime(text, sizeof text, pattern, &local);
    return text;
}

std::string matchup(const Game& game) {
    auto shown = [](const Team& team) { return team.names.empty() ? team.abbreviations[0] : team.names[0]; };
    std::string when = std::format("{:%a %b %d}", game.day);
    if (game.start) {
        when = local_time(*game.start, "%a %b %d %I:%M %p");
        for (std::size_t at; (at = when.find(" 0")) != std::string::npos;)
            when.erase(at + 1, 1);
    }
    return std::format("{} @ {}  |  {}  |  {}", shown(game.away), shown(game.home), when, game.live ? "LIVE" : "pregame");
}

std::string rstrip(std::string text) {
    text.erase(text.find_last_not_of(' ') + 1);
    return text;
}

std::string side_cells(const Game& game, const Line& line, bool home, double min_stake) {
    std::optional<double> handicap = line.handicap && home ? std::optional(-*line.handicap) : line.handicap;
    std::string label = side_label(game, home, handicap);
    const Quote* best = best_offer(home ? line.home : line.away, min_stake);
    if (!best)
        return std::format("{:<12}{:>15}{:>13}  {:<10}", label, "-", "-", "");
    return std::format("{:<12}{:>15}{:>13}  {:<10}", label, odds(best->net()), money(best->stake()), name(best->book));
}

// A game's moneyline, then its main spread if it has one.
struct Listing {
    const Game* game;
    std::vector<Line> lines;
};

void print_game(const Listing& listing, double min_stake) {
    say("\n{}", matchup(*listing.game));
    std::string columns = std::format("{:<12}{:>15}{:>13}  {:<10}", "Side", "Odds", "Avail", "Book");
    say("{}", rstrip(std::format("  {:<11}{}   {}", "", columns, columns)));
    for (const Line& line : listing.lines)
        say("{}", rstrip(std::format("  {:<11}{}   {}", line.handicap ? "Spread" : "Moneyline",
                                     side_cells(*listing.game, line, false, min_stake),
                                     side_cells(*listing.game, line, true, min_stake))));
}

void print_opportunity(int rank, const Opportunity& opportunity) {
    const Game& game = *opportunity.game;
    double roi = opportunity.roi() * 100;
    say("\n{:>2}. {}{}%  {}  {}", rank, roi < 0 ? "" : "+", fixed(roi, 2), game.league, matchup(game));

    // Equal payouts on both sides, as large as the smaller offer allows.
    double payout = std::min(*opportunity.away->size, *opportunity.home->size);
    for (const Quote* leg : {opportunity.away, opportunity.home})
        say("      {:<12}{:>15}  {:<10}  stake {:>12}", side_label(game, *leg), odds(leg->net()), name(leg->book),
            money(payout * leg->net()));
    double staked = payout * opportunity.cost();
    say("      Stake {} to collect {}: {} {}", money(staked), money(payout), payout > staked ? "profit" : "loss",
        money(std::abs(payout - staked)));
}

std::string period(const Window& window) {
    Day last_day = window.first_day + std::chrono::days(window.days - 1);
    return std::format("{:%a %b %d} to {:%a %b %d, %Y}", window.first_day, last_day);
}

void show_best_lines(std::vector<Game>& games, const Window& window, const Args& args) {
    say("\nBest lines for {}", period(window));
    if (games.empty()) {
        say("No games with open moneyline or spread markets.");
        return;
    }
    say("Odds include the book's taker fee; Avail is the most you can stake at that price.");
    say("Prices with under {} available are ignored (--min-stake).", money(args.min_stake, 0));

    std::vector<Listing> listings;
    for (Game& game : games) {
        std::vector<Line> lines = lines_of(game);
        Listing listing{&game, {Line{}}};
        for (const Line& line : lines)
            if (!line.handicap)
                listing.lines[0] = line;
        if (const Line* spread = main_line(lines))
            listing.lines.push_back(*spread);
        listings.push_back(std::move(listing));
    }
    size_polymarket(games, [&] {
        std::vector<Quote*> winners;
        for (const Listing& listing : listings)
            for (const Line& line : listing.lines)
                for (const std::vector<Quote*>* offers : {&line.away, &line.home})
                    if (Quote* best = best_offer(*offers, args.min_stake))
                        winners.push_back(best);
        return winners;
    });

    for (const League& league : LEAGUES) {
        std::vector<const Listing*> in_league;
        for (const Listing& listing : listings)
            if (listing.game->league == league.name)
                in_league.push_back(&listing);
        if (!in_league.empty())
            say("\n{0} {1} {0}", std::string(30, '='), league.name);
        std::ranges::stable_sort(in_league, {}, [](const Listing* listing) {
            return std::pair(listing->game->day, listing->game->start.value_or(std::numeric_limits<Seconds>::max()));
        });
        for (const Listing* listing : in_league)
            print_game(*listing, args.min_stake);
    }
}

void show_top_arbitrage(std::vector<Game>& games, const Window& window, const Args& args) {
    std::vector<Opportunity> top = top_opportunities(games, static_cast<std::size_t>(std::max(args.top, 0)), args.min_stake);
    say("\nTop {} lines by arbitrage return, {}", args.top, period(window));
    if (top.empty()) {
        say("No line is quoted on both sides.");
        return;
    }
    say("Return is from backing both sides at the best price for each, taker fees included.");
    say("A negative return is not an arbitrage. Stakes are the most those prices allow.");
    say("Prices with under {} available are ignored (--min-stake).", money(args.min_stake, 0));
    int rank = 0;
    for (const Opportunity& opportunity : top)
        print_opportunity(++rank, opportunity);
}

void print_hedged_bet(BetKind kind, const HedgedBet& found, std::string_view numbering) {
    const Game& game = *found.game;
    double amount = found.amount;
    std::string_view result = found.profit() < 0 ? "loss" : "profit";
    double profit = std::abs(found.profit());
    std::string hedge_won = money(found.hedge_payout() - found.hedge_stake());
    std::string outcome, hedge_wins;
    switch (kind) {
        case BetKind::Bonus:
            outcome = std::format("keep {}, {}% of it", money(found.profit()), fixed(found.profit() / amount * 100, 1));
            hedge_wins = std::format("the hedge makes {}.", money(found.profit()));
            break;
        case BetKind::Cash: {
            double staked = amount + found.hedge_stake();
            outcome = std::format("{} {}, {}% of the {} staked", result, money(profit),
                                  fixed(profit / staked * 100, 2), money(staked));
            hedge_wins = std::format("{} won less the {} bet.", hedge_won, money(amount));
            break;
        }
        case BetKind::SafetyNet:
            outcome = std::format("{} {}, {}% of the bet", result, money(profit), fixed(profit / amount * 100, 1));
            hedge_wins = std::format("{} won less the {} bet, plus {} in bonus bets worth {}.", hedge_won,
                                     money(amount), money(amount), money(amount - found.at_risk));
            break;
        case BetKind::Boosted:
            outcome = std::format("{} {}, {}% of the bet", result, money(profit), fixed(profit / amount * 100, 1));
            hedge_wins = std::format("{} won less the {} bet.", hedge_won, money(amount));
            break;
    }

    std::string_view kind_name = name(kind);
    say("\n{}{} at {}: {}", numbering, kind_name, name(found.bet->book), outcome);
    say("      {}  {}", game.league, matchup(game));
    for (auto [role, leg, stake] : {std::tuple{kind_name.substr(0, kind_name.find(' ')), found.bet, amount},
                                    std::tuple{std::string_view("Hedge"), found.hedge, found.hedge_stake()}})
        say("      {:<7}  {:<12}{:>15}  {:<10}  stake {:>12}", role, side_label(game, *leg), odds(leg->net()),
            name(leg->book), money(stake));
    say("      {} wins: {} {} less the {} hedge.  {} wins: {}", side_label(game, *found.bet),
        money(found.winnings()), found.boost ? "won with the boost" : "won", money(found.hedge_stake()),
        side_label(game, *found.hedge), hedge_wins);
}

std::vector<HedgedBet> show_hedged_bets(std::vector<Game>& games, const Window& window, const Args& args,
                                        const Wager& wager) {
    BetKind kind = wager.kind;
    double rate = args.conversion_rate;
    std::vector<HedgedBet> found = best_hedged_bets(games, wager, args, {});

    std::string lowered(name(kind));
    lowered[0] = static_cast<char>(std::tolower(static_cast<unsigned char>(lowered[0])));
    say("\nBest use of a {} {}, {}", money(wager.amount), lowered, period(window));
    switch (kind) {
        case BetKind::Bonus:
            say("A bonus bet's stake is not paid back, so a win pays only the winnings. The hedge is");
            say("cash on the other side, sized so the profit is the same whichever side wins.");
            break;
        case BetKind::Cash:
            say("The hedge is cash on the other side, sized so the profit is the same whichever side");
            say("wins. A loss means no line is an arbitrage for a bet this size at that book.");
            break;
        case BetKind::SafetyNet:
            say("A safety net bet is cash that comes back as bonus bets if it loses. Those are valued");
            say("at {:g}% of their amount (--conversion-rate); option 3 shows what a bonus bet", rate * 100);
            say("keeps now. The hedge is cash on the other side, sized so the profit is the same");
            say("whichever side wins.");
            break;
        case BetKind::Boosted:
            say("A {:g}% profit boost adds that much to the winnings of a cash bet. The bet's odds are",
                wager.boost * 100);
            say("shown before the boost. The hedge is cash on the other side, sized so the profit is the");
            say("same whichever side wins.");
            break;
    }
    say("Every moneyline and spread is considered, taker fees included, and both prices have");
    say("enough available for the whole bet. Neither side is bet at odds beyond -{}", args.favorite_cap);
    say("(--favorite-cap).");
    std::string limits;
    if (wager.hedge_cap) {
        say("The hedge is kept to a stake of {} at most.", money(*wager.hedge_cap));
        limits += std::format(" and a hedge of {} or less", money(*wager.hedge_cap));
    }
    if (wager.min_odds) {
        say("The bet is kept to odds of {} or longer.", american(*wager.min_odds));
        limits += std::format(" and the bet at {} or longer", american(*wager.min_odds));
    }
    int number = 0;
    for (const HedgedBet& hedged : found)
        print_hedged_bet(kind, hedged, std::format("{}. ", ++number));
    if (found.empty())
        say("\n{}: nothing within -{} with enough available on both sides{}.", name(kind), args.favorite_cap,
            limits);
    return found;
}

Window current_window(int days) {
    std::tm local = local_tm(std::time(nullptr));
    Day today = std::chrono::year(local.tm_year + 1900) / (local.tm_mon + 1) / local.tm_mday;
    local.tm_hour = local.tm_min = local.tm_sec = 0;
    local.tm_isdst = -1;
    Seconds start = std::mktime(&local);
    return {start, start + days * Seconds(86400), today, days};
}

// Every book's current lines for the chosen leagues.
std::vector<Game> load_games(const Args& args, const Window& window) {
    struct Pending {
        Book book;
        std::string_view league;
        std::future<std::vector<Game>> games;
    };
    std::vector<Pending> pending;
    for (Book book : BOOKS)
        for (const League& league : LEAGUES) {
            if (!std::ranges::contains(args.leagues, league.name))
                continue;
            pending.push_back({book, league.name, std::async(std::launch::async, [&args, &league, book, window] {
                                   switch (book) {
                                       case Book::Novig: return novig(league, window);
                                       case Book::Kalshi: return kalshi(league, window);
                                       default: return polymarket(league, window, args.polymarket_fee);
                                   }
                               })});
        }

    std::vector<std::vector<Game>> games_of(BOOKS.size());
    for (Pending& fetch : pending) {
        try {
            std::vector<Game> fetched = fetch.games.get();
            std::vector<Game>& games = games_of[static_cast<int>(fetch.book)];
            games.insert(games.end(), std::make_move_iterator(fetched.begin()), std::make_move_iterator(fetched.end()));
        } catch (const std::exception& error) {
            std::cerr << std::format("{} {} skipped: {}\n", name(fetch.book), fetch.league, error.what());
        }
    }
    std::vector<Game> games = merge(std::move(games_of));
    std::erase_if(games, [](const Game& game) { return game.quotes.empty(); });
    return games;
}

std::string read_line(std::string_view prompt) {
    std::cout << prompt << std::flush;
    std::string line;
    if (!std::getline(std::cin, line))
        throw EndOfInput{};
    return line;
}

// The cost of $1 of payout at American odds such as -250 or +150, or decimal odds such as 1.87x.
// An unsigned number is American from 100 up and decimal below it.
std::optional<double> parse_odds(std::string entry) {
    std::erase_if(entry, [](unsigned char c) { return std::isspace(c); });
    bool is_decimal = entry.ends_with('x') || entry.ends_with('X');
    if (is_decimal)
        entry.pop_back();
    bool is_signed = entry.starts_with('+') || entry.starts_with('-');
    double value = 0;
    try {
        value = parse_number(entry);
    } catch (const std::runtime_error&) {
        return std::nullopt;
    }
    if (!std::isfinite(value) || (is_decimal && is_signed))
        return std::nullopt;
    if (!is_decimal && (is_signed || value >= 100)) {
        if (std::abs(value) < 100)
            return std::nullopt;
        return value < 0 ? -value / (100 - value) : 100 / (value + 100);
    }
    return value > 1 ? std::optional(1 / value) : std::nullopt;
}

// The bet's amount, then optionally the most its hedge may stake and the minimum odds the bet may
// be at. The odds come third, or second when they are signed or end in x. A boosted bet may also
// give its boost, as an entry ending in %.
std::optional<Wager> ask_wager(BetKind kind) {
    bool boosted = kind == BetKind::Boosted;
    std::string options = " hedge and the minimum odds for the bet, e.g. 500 2000 -200";
    if (boosted)
        options = std::format(" hedge, the minimum odds for the bet and the boost if it is not {:g}%,"
                              " e.g. 500 2000 -200 50%",
                              PROFIT_BOOST * 100);
    for (;;) {
        std::string entry = read_line(std::format("{} amount in dollars, then optionally the most to stake on the{}"
                                                  " (blank to go back): ",
                                                  name(kind), options));
        std::erase_if(entry, [](char c) { return c == '$' || c == ','; });
        std::istringstream stream(entry);
        std::vector<std::string> words{std::istream_iterator<std::string>(stream), {}};
        if (words.empty())
            return std::nullopt;

        bool valid = true;
        auto positive = [&valid](std::string_view word) {
            double value = 0;
            auto [end, error] = std::from_chars(word.data(), word.data() + word.size(), value);
            valid &= error == std::errc{} && end == word.data() + word.size() && std::isfinite(value) && value > 0;
            return value;
        };
        double boost = boosted ? PROFIT_BOOST : 0;
        int boosts = 0;
        std::erase_if(words, [&](const std::string& word) {
            if (!word.ends_with('%'))
                return false;
            boost = positive(std::string_view(word).substr(0, word.size() - 1)) / 100;
            ++boosts;
            return true;
        });
        valid &= boosts <= (boosted ? 1 : 0);

        auto reads_as_odds = [](const std::string& word) {
            return word.starts_with('+') || word.starts_with('-') || word.ends_with('x') || word.ends_with('X');
        };
        std::optional<double> min_odds;
        if (words.size() == 3 || (words.size() == 2 && reads_as_odds(words[1]))) {
            min_odds = parse_odds(words.back());
            valid &= min_odds.has_value();
            words.pop_back();
        }
        std::vector<double> amounts;
        for (const std::string& word : words)
            amounts.push_back(positive(word));
        if (valid && (amounts.size() == 1 || amounts.size() == 2))
            return Wager{amounts[0], amounts.size() == 2 ? std::optional(amounts[1]) : std::nullopt, min_odds, kind,
                         boost};
        say("Enter an amount such as 100, optionally followed by a hedge cap and minimum odds");
        say("for the bet, such as 500 2000 -200. Minimum odds alone need their sign: 500 -200.");
        if (boosted)
            say("A boost is entered with its % sign, such as 50%.");
    }
}

// Asks which of the hedged bets shown the user is taking and the odds their own book gives on
// its bet side, then prices the hedge against those odds. Their book charges no fee. Answering
// `next` shows the next best bets, from `more(found)`, to choose from as well.
void reprice_at_own_book(std::vector<HedgedBet> found, const Wager& wager,
                         const std::function<std::vector<HedgedBet>(const std::vector<HedgedBet>&)>& more) {
    if (found.empty())
        return;
    const HedgedBet* chosen = nullptr;
    while (!chosen) {
        std::string entry = strip(read_line(std::format("\nWhich line will you be using? 1 to {}, or next for more"
                                                        " (blank to go back): ",
                                                        found.size())));
        if (entry.empty())
            return;
        if (upper(entry) == "NEXT") {
            std::vector<HedgedBet> added = more(found);
            for (const HedgedBet& hedged : added) {
                found.push_back(hedged);
                print_hedged_bet(wager.kind, hedged, std::format("{}. ", found.size()));
            }
            if (added.empty())
                say("No more lines fit.");
            continue;
        }
        std::size_t number = 0;
        const char* end = entry.data() + entry.size();
        auto [parsed_to, error] = std::from_chars(entry.data(), end, number);
        if (error == std::errc{} && parsed_to == end && number >= 1 && number <= found.size())
            chosen = &found[number - 1];
        else
            say("Enter a number from 1 to {}, or next.", found.size());
    }
    std::optional<double> price;
    while (!price) {
        std::string entry = strip(read_line(std::format("What line are you getting through your book on {}{}?"
                                                        " e.g. -250 or 1.87x (blank to go back): ",
                                                        side_label(*chosen->game, *chosen->bet),
                                                        wager.kind == BetKind::Boosted ? ", before the boost" : "")));
        if (entry.empty())
            return;
        if (!(price = parse_odds(entry)))
            say("Enter American odds such as -250 or +150, or decimal odds such as 1.87x.");
    }

    Quote own = *chosen->bet;
    own.book = Book::Own;
    own.price = *price;
    own.fee_rate = 0;
    HedgedBet repriced = *chosen;
    repriced.bet = &own;
    print_hedged_bet(wager.kind, repriced, "");

    const Quote& hedge = *repriced.hedge;
    if (round9(*hedge.size) < round9(repriced.hedge_payout()))
        say("      Only {} can be staked on the hedge at that price, short of the {} it needs.", money(hedge.stake()),
            money(repriced.hedge_stake()));
    if (wager.hedge_cap && round9(repriced.hedge_stake()) > *wager.hedge_cap)
        say("      The hedge is over the {} you set as the most to stake on it.", money(*wager.hedge_cap));
    if (wager.min_odds && round9(*price) > round9(*wager.min_odds))
        say("      {} is shorter than the {} you set as the minimum odds.", american(*price),
            american(*wager.min_odds));
}

std::string join(const std::vector<std::string>& parts, std::string_view separator) {
    std::string joined;
    for (const std::string& part : parts)
        joined += (joined.empty() ? "" : std::string(separator)) + part;
    return joined;
}

void menu(const Args& args) {
    for (;;) {
        say("\nBest Lines: {}", join(args.leagues, ", "));
        say("  1. All games in the next {} days, with the best book for each line", args.days);
        say("  2. Top {} arbitrage opportunities", args.top);
        say("  3. Best use of a bonus bet");
        say("  4. Best use of a cash (non-bonus) bet");
        say("  5. Best use of a safety net bet (bonus bets back if it loses)");
        say("  6. Best use of a profit boost token ({:g}% unless you enter another)", PROFIT_BOOST * 100);
        say("  0. Exit");
        std::string choice = strip(read_line("Option: "));
        if (choice == "0")
            return;
        if (choice.size() != 1 || !std::string_view("123456").contains(choice[0])) {
            say("'{}' is not an option.", choice);
            continue;
        }
        std::optional<Wager> wager;
        if (choice >= "3") {
            constexpr BetKind kinds[]{BetKind::Bonus, BetKind::Cash, BetKind::SafetyNet, BetKind::Boosted};
            if (!(wager = ask_wager(kinds[choice[0] - '3'])))
                continue;
        }

        say("Fetching current lines...");
        Window window = current_window(args.days);
        std::vector<Game> games = load_games(args, window);
        if (choice == "1")
            show_best_lines(games, window, args);
        else if (choice == "2")
            show_top_arbitrage(games, window, args);
        else
            reprice_at_own_book(show_hedged_bets(games, window, args, *wager), *wager,
                                [&](const std::vector<HedgedBet>& shown) {
                                    return best_hedged_bets(games, *wager, args, shown);
                                });
    }
}

constexpr std::string_view USAGE =
    "usage: best_lines [-h] [--leagues LEAGUE [LEAGUE ...]] [--days DAYS] [--top COUNT]\n"
    "                  [--polymarket-fee COEFFICIENT] [--min-stake DOLLARS] [--favorite-cap ODDS]\n"
    "                  [--conversion-rate RATE]\n";

constexpr std::string_view HELP =
    "\n"
    "Moneylines and spreads for upcoming NBA, NFL, NHL and NCAAF games across Novig, Kalshi and\n"
    "Polymarket US: a menu offers the best book for each game's lines, the top arbitrage\n"
    "opportunities, or the best hedged use of a bonus bet, a cash bet, a safety net bet or a profit\n"
    "boost.\n"
    "\n"
    "Every price is what a taker pays now, with that book's trading fee included.\n"
    "\n"
    "options:\n"
    "  -h, --help            show this help message and exit\n"
    "  --leagues LEAGUE [LEAGUE ...]\n"
    "                        leagues to cover: NBA NFL NHL NCAAF (default: all)\n"
    "  --days DAYS           days to cover, starting today (default: 7)\n"
    "  --top COUNT           arbitrage lines to show (default: 10)\n"
    "  --polymarket-fee COEFFICIENT\n"
    "                        your Polymarket US taker fee coefficient, e.g. 0.0695 (default: the\n"
    "                        0.095 base tier)\n"
    "  --min-stake DOLLARS   ignore prices with less than this much available to stake (default: 10)\n"
    "  --favorite-cap ODDS   in options 3 to 6, bet no favorite heavier than -ODDS (default: 800)\n"
    "  --conversion-rate RATE\n"
    "                        in option 5, what a bonus bet is worth as a share of its amount (default:\n"
    "                        0.70)\n";

[[noreturn]] void usage_error(const std::string& message) {
    std::cerr << USAGE << "best_lines: error: " << message << '\n';
    std::exit(2);
}

Args parse_args(int argc, char** argv) {
    Args args;
    for (const League& league : LEAGUES)
        args.leagues.emplace_back(league.name);

    std::vector<std::string> words(argv + 1, argv + argc);
    std::size_t at = 0;
    auto values = [&](const std::string& option, std::size_t at_least, std::size_t at_most) {
        std::vector<std::string> taken;
        while (at < words.size() && taken.size() < at_most && !words[at].starts_with("--"))
            taken.push_back(words[at++]);
        if (taken.size() < at_least)
            usage_error(std::format("argument {}: expected {} value{}", option, at_least == at_most ? std::to_string(at_least) : "at least one", at_least == 1 ? "" : "s"));
        return taken;
    };
    auto number = [&](const std::string& option, const std::string& text, bool whole) {
        try {
            double value = parse_number(text);
            if (!whole || value == std::trunc(value))
                return value;
        } catch (const std::runtime_error&) {
        }
        usage_error(std::format("argument {}: invalid value: '{}'", option, text));
    };

    while (at < words.size()) {
        std::string option = words[at++];
        if (option == "-h" || option == "--help") {
            std::cout << USAGE << HELP;
            std::exit(0);
        } else if (option == "--leagues") {
            args.leagues.clear();
            for (const std::string& word : values(option, 1, LEAGUES.size() * 4)) {
                std::string league = upper(word);
                if (!std::ranges::contains(LEAGUES, league, &League::name))
                    usage_error(std::format("argument --leagues: invalid choice: '{}' (choose from NBA, NFL, NHL, NCAAF)", league));
                args.leagues.push_back(league);
            }
        } else if (option == "--days") {
            args.days = static_cast<int>(number(option, values(option, 1, 1)[0], true));
        } else if (option == "--top") {
            args.top = static_cast<int>(number(option, values(option, 1, 1)[0], true));
        } else if (option == "--polymarket-fee") {
            args.polymarket_fee = number(option, values(option, 1, 1)[0], false);
        } else if (option == "--min-stake") {
            args.min_stake = number(option, values(option, 1, 1)[0], false);
        } else if (option == "--conversion-rate") {
            args.conversion_rate = number(option, values(option, 1, 1)[0], false);
            if (!(args.conversion_rate > 0 && args.conversion_rate <= 1))
                usage_error("argument --conversion-rate: must be above 0 and at most 1");
        } else if (option == "--favorite-cap") {
            args.favorite_cap = std::abs(static_cast<int>(number(option, values(option, 1, 1)[0], true)));
        } else {
            usage_error(std::format("unrecognized argument: {}", option));
        }
    }
    return args;
}

int main(int argc, char** argv) {
#ifdef _WIN32
    SetConsoleOutputCP(CP_UTF8);
#endif
    try {
        menu(parse_args(argc, argv));
    } catch (const EndOfInput&) {
        // The end of piped input, or Ctrl+Z, leaves the menu.
        std::cout << '\n';
    } catch (const std::exception& error) {
        std::cerr << "best_lines: " << error.what() << '\n';
        return 1;
    }
    return 0;
}
