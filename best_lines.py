"""Moneylines and spreads for upcoming NBA, NFL, NHL and NCAAF games across Novig, Kalshi and
Polymarket US: a menu offers the best book for each game's lines, the top arbitrage
opportunities, or the best hedged use of a bonus bet, a cash bet, a safety net bet or a profit
boost.

Every price is what a taker pays now, with that book's trading fee included.
"""

import argparse
import gzip
import itertools
import json
import re
import sys
import time as clock
import urllib.error
import urllib.parse
import urllib.request
from collections import defaultdict
from concurrent.futures import ThreadPoolExecutor
from dataclasses import dataclass, replace
from datetime import date, datetime, time, timedelta, timezone
from decimal import Decimal, InvalidOperation
from functools import partial
from typing import NamedTuple

NOVIG = "https://api.novig.com/v3/public/catalog"
KALSHI = "https://api.elections.kalshi.com/trade-api/v2"
POLYMARKET = "https://gateway.polymarket.us"

# The user's own sportsbook: never fetched, only priced from odds they type in.
OWN_BOOK = "your book"
# How many hedged bets options 3 to 6 show at a time, whichever books they are with.
HEDGED_BETS_SHOWN = 5
# What a profit boost token adds to a winning bet's winnings, as a share of them, unless the
# user enters another.
PROFIT_BOOST = Decimal(1)
MONTHS = "JAN FEB MAR APR MAY JUN JUL AUG SEP OCT NOV DEC".split()
# Kalshi's general taker coefficient; each series scales it by its fee_multiplier.
KALSHI_TAKER_FEE = Decimal("0.07")
POLYMARKET_PAGE = 200

# Polymarket US allows 25 requests a second per IP.
BOOK_READERS = ThreadPoolExecutor(max_workers=4)


class League(NamedTuple):
    name: str  # also Novig's name for it
    kalshi_winners: str
    kalshi_spreads: str
    polymarket_tag: str
    polymarket_sport: str  # prefix of its sportsMarketType values


LEAGUES = [
    League("NBA", "KXNBAGAME", "KXNBASPREAD", "nba", "basketball"),
    League("NFL", "KXNFLGAME", "KXNFLSPREAD", "nfl", "football"),
    League("NHL", "KXNHLGAME", "KXNHLSPREAD", "nhl", "hockey"),
    League("NCAAF", "KXNCAAFGAME", "KXNCAAFSPREAD", "cfb", "football"),
]


@dataclass
class Team:
    abbreviations: list
    names: list


@dataclass
class Quote:
    book: str
    home: bool  # backs the home side
    handicap: Decimal | None  # that side's own spread; None on a moneyline
    price: Decimal  # cost of $1 of payout, before fees
    size: Decimal | None  # dollars of payout offered at that price; None until read from a book
    fee_rate: Decimal  # every book charges fee_rate * price * (1 - price) per $1 of payout
    polymarket_side: tuple | None = None  # (market slug, is the long side)

    @property
    def net(self):
        return self.price + self.fee_rate * self.price * (1 - self.price)

    @property
    def stake(self):
        return self.net * self.size

    @property
    def line(self):
        """Spread lines are keyed by the away side's handicap; the moneyline is None."""
        if self.handicap is None:
            return None
        return -self.handicap if self.home else self.handicap


@dataclass
class Game:
    league: str
    day: date
    away: Team
    home: Team
    quotes: list
    start: datetime | None = None
    live: bool = False


def warn(message):
    print(message, file=sys.stderr)


def get_json(url):
    headers = {"User-Agent": "best-lines", "Accept-Encoding": "gzip"}
    request = urllib.request.Request(url, headers=headers)
    for attempt in range(4):
        try:
            with urllib.request.urlopen(request, timeout=30) as response:
                body = response.read()
                if response.headers.get("Content-Encoding") == "gzip":
                    body = gzip.decompress(body)
                return json.loads(body)
        except urllib.error.HTTPError as error:
            if error.code != 429 or attempt == 3:
                raise
            clock.sleep(2**attempt)


def game_day(start):
    # US evening games run past midnight UTC, so date a game six hours behind it. That matches
    # the day in Kalshi's tickers, the only date Kalshi gives.
    return (start.astimezone(timezone.utc) - timedelta(hours=6)).date()


def novig(league, window_start, window_end):
    def pages(path, **params):
        items = []
        while True:
            page = get_json(f"{NOVIG}/{path}?{urllib.parse.urlencode(params)}")
            items += page["items"]
            if "next" not in page:
                return items
            params["after"] = page["next"]

    # Both API bounds are exclusive Unix milliseconds.
    window = {
        "league": league.name,
        "startsAfter": int(window_start.timestamp() * 1000) - 1,
        "startsBefore": int(window_end.timestamp() * 1000),
    }
    events = pages("events", **window)
    markets_of = defaultdict(list)
    for market in pages("markets", marketType="MONEY,SPREAD", limit=5000, **window):
        markets_of[market["eventId"]].append(market)

    games = []
    for event in events:
        live = event["status"] == "OPEN_INGAME"
        offers, home_abbreviation = [], None
        for market in markets_of[event["eventId"]]:
            fee = market["fee"]
            charged = fee["charged"] == "ALWAYS" or (fee["charged"] == "WHEN_LIVE" and live)
            fee_rate = Decimal(fee["coefficient"]) if charged else Decimal(0)

            sides = []
            for outcome in market["outcomes"]:
                if market["marketType"] == "SPREAD":
                    # Named like "NYK -3.5"; strike is the home side's handicap.
                    abbreviation, _, handicap = outcome["name"].rpartition(" ")
                    handicap = Decimal(handicap)
                    if handicap and handicap == Decimal(market["strike"]):
                        home_abbreviation = abbreviation
                else:
                    abbreviation, handicap = outcome["name"], None
                sides.append((abbreviation, handicap, outcome))
            if len(sides) != 2:
                continue
            # The book holds only bids: backing one outcome crosses the best bid on the other
            # at 1 - its price. A contract pays out 1 cent.
            for (abbreviation, handicap, _), (_, _, other) in (sides, sides[::-1]):
                bid = other.get("bid")
                if bid:
                    price = 1 - Decimal(bid["price"])
                    size = Decimal(bid["qty"]) / 100
                    offers.append((abbreviation, handicap, price, size, fee_rate))

        abbreviations = {offer[0] for offer in offers}
        if home_abbreviation not in abbreviations or len(abbreviations) != 2:
            continue
        (away_abbreviation,) = abbreviations - {home_abbreviation}
        away_name, _, home_name = event["description"].partition(" @ ")
        quotes = [
            Quote("Novig", abbreviation == home_abbreviation, handicap, price, size, fee_rate)
            for abbreviation, handicap, price, size, fee_rate in offers
        ]
        start = datetime.fromtimestamp(event["startsTs"] / 1000, timezone.utc)
        away = Team([away_abbreviation], [away_name] if home_name else [])
        home = Team([home_abbreviation], [home_name] if home_name else [])
        games.append(Game(league.name, game_day(start), away, home, quotes, start, live))
    return games


def kalshi(league, window_start, window_end):
    # Event tickers look like KXNBAGAME-26NOV17NYKTOR: game day, away team, home team.
    days = {}
    day = window_start.date()
    while day < window_end.date():
        days[f"{day:%y}{MONTHS[day.month - 1]}{day:%d}"] = day
        day += timedelta(days=1)

    def open_markets(series):
        multiplier = get_json(f"{KALSHI}/series/{series}")["series"]["fee_multiplier"]
        fee_rate = KALSHI_TAKER_FEE * Decimal(str(multiplier))
        found, cursor = [], ""
        while True:
            query = {"series_ticker": series, "status": "open", "limit": 1000, "cursor": cursor}
            page = get_json(f"{KALSHI}/markets?{urllib.parse.urlencode(query)}")
            for market in page["markets"]:
                event = market["event_ticker"].split("-")[1]
                if event[:7] in days:
                    found.append((event, market, fee_rate))
            cursor = page.get("cursor")
            if not cursor:
                return found

    def yes_abbreviation(market):
        # The last ticker part is the Yes team, plus a line number on spreads: TOR, NYK4.
        return market["ticker"].rsplit("-", 1)[1].rstrip("0123456789")

    winners = open_markets(league.kalshi_winners)
    spreads = open_markets(league.kalshi_spreads)

    teams_in = defaultdict(dict)
    for event, market, _ in winners:
        teams_in[event][yes_abbreviation(market)] = market["yes_sub_title"]
    games = {}
    for event, teams in teams_in.items():
        if len(teams) != 2:
            continue
        first, second = teams
        if event[7:] == second + first:
            first, second = second, first
        elif event[7:] != first + second:
            continue
        away, home = Team([first], [teams[first]]), Team([second], [teams[second]])
        games[event] = Game(league.name, days[event[:7]], away, home, [])

    for (event, market, fee_rate), is_spread in [(m, False) for m in winners] + [
        (m, True) for m in spreads
    ]:
        game = games.get(event)
        abbreviation = yes_abbreviation(market)
        if not game or abbreviation not in game.away.abbreviations + game.home.abbreviations:
            continue
        yes_is_home = abbreviation in game.home.abbreviations
        # A spread market reads "<team> wins by over <floor_strike> points".
        margin = Decimal(str(market["floor_strike"])) if is_spread else None
        # Buying No on a team is backing its opponent, against the resting Yes bids.
        for home, handicap, price, size in (
            (yes_is_home, margin and -margin, market["yes_ask_dollars"], market["yes_ask_size_fp"]),
            (not yes_is_home, margin, market["no_ask_dollars"], market["yes_bid_size_fp"]),
        ):
            if Decimal(size) > 0:
                quote = Quote("Kalshi", home, handicap, Decimal(price), Decimal(size), fee_rate)
                game.quotes.append(quote)
    return list(games.values())


def polymarket(league, window_start, window_end, fee_override):
    winner_kind = f"{league.polymarket_sport}_team_full_game_winner"
    spread_kind = f"{league.polymarket_sport}_team_full_game_spread"

    def events(**filters):
        query = {
            "tagSlug": league.polymarket_tag,
            "closed": "false",
            "startTimeMin": f"{window_start.astimezone(timezone.utc):%Y-%m-%dT%H:%M:%SZ}",
            "startTimeMax": f"{window_end.astimezone(timezone.utc):%Y-%m-%dT%H:%M:%SZ}",
            "limit": POLYMARKET_PAGE,
            **filters,
        }
        seen = set()
        while True:
            query["offset"] = len(seen)
            page = get_json(f"{POLYMARKET}/v1/events?{urllib.parse.urlencode(query)}")["events"]
            new = [event for event in page if event["slug"] not in seen]
            seen.update(event["slug"] for event in new)
            yield from new
            if not new or len(page) < POLYMARKET_PAGE:
                return

    def quotes(market, is_spread):
        if not market["active"] or market["closed"]:
            return []
        long_side = next(side for side in market["marketSides"] if side["long"])
        long_is_home = long_side["team"]["ordering"] == "home"
        # One instrument per market: `line` is the long side's handicap, and backing the short
        # side means selling the long one into its bids.
        line = Decimal(str(market["line"])) if is_spread else None
        fee_rate = fee_override or Decimal(str(market["feeCoefficient"]))
        ask, bid = market.get("bestAskQuote"), market.get("bestBidQuote")
        found = []
        for home, handicap, price, is_long in (
            (long_is_home, line, ask and Decimal(ask["value"]), True),
            (not long_is_home, line and -line, bid and 1 - Decimal(bid["value"]), False),
        ):
            if price is not None and 0 < price < 1:
                side = (market["slug"], is_long)
                found.append(Quote("Polymarket", home, handicap, price, None, fee_rate, side))
        return found

    # Unfiltered, an event carries only its moneyline; filtered, only markets of that type.
    games = {}
    for event in events():
        market = next(
            (m for m in event["markets"] if m.get("sportsMarketType") == winner_kind), None
        )
        start = datetime.fromisoformat(event["startTime"])
        if not market or event.get("ended") or not window_start <= start < window_end:
            continue
        teams = {
            side["team"]["ordering"]: Team([side["team"]["abbreviation"].upper()], [side["team"]["name"]])
            for side in market["marketSides"]
            if side.get("team")
        }
        if set(teams) != {"away", "home"}:
            continue
        games[event["slug"]] = Game(
            league.name,
            game_day(start),
            teams["away"],
            teams["home"],
            quotes(market, is_spread=False),
            start,
            bool(event.get("live")),
        )
    for event in events(sportsMarketTypes=spread_kind):
        game = games.get(event["slug"])
        if game:
            for market in event["markets"]:
                if market.get("sportsMarketType") == spread_kind:
                    game.quotes += quotes(market, is_spread=True)
    return list(games.values())


def name_words(name):
    words = re.findall(r"[a-z0-9]+", name.lower().replace("&", "").replace("'", ""))
    if words and words[-1] == "st":
        words[-1] = "state"
    return words


def is_short_form(short, full):
    """Each word of `short` starts a later word of `full`: Edmonton, Oilers, New York G."""
    remaining = iter(full)
    return all(any(word.startswith(part) for word in remaining) for part in short)


def team_match(one, other):
    """3 for the same name, 2 for the same abbreviation, 1 for a shortened name, else 0."""
    names = [name_words(name) for name in one.names]
    other_names = [name_words(name) for name in other.names]
    pairs = [(a, b) for a in names for b in other_names if a and b]
    if any(a == b for a, b in pairs):
        return 3
    if {a.upper() for a in one.abbreviations} & {a.upper() for a in other.abbreviations}:
        return 2
    if any(is_short_form(*sorted(pair, key=len)) for pair in pairs):
        return 1
    return 0


def game_match(teams, targets):
    """How surely two listings are the same game, given they share a league and day; 0 if not.

    A team plays once a day, so one exact name is enough. Otherwise both teams have to match.
    """
    scores = [team_match(team, target) for team, target in zip(teams, targets)]
    if all(scores) or max(scores) == 3:
        return sum(scores)
    # A lone shared abbreviation counts if the other side's at least start alike: WSH and WAS.
    initials = [
        {a[0].upper() for a in team.abbreviations} & {a[0].upper() for a in target.abbreviations}
        for team, target in zip(teams, targets)
    ]
    if max(scores) == 2 and all(score or alike for score, alike in zip(scores, initials)):
        return 2
    return 0


def merge(books):
    """Combine each book's games into one list, joining the listings of the same game.

    Books share no IDs and abbreviate teams differently, so games are paired by league, day
    and team names. Books can disagree on which side is home at a neutral site.
    """
    merged = []
    for games in books:
        candidates = []
        for game in games:
            for target in merged:
                if (target.league, target.day) != (game.league, game.day):
                    continue
                for flipped in (False, True):
                    teams = (game.home, game.away) if flipped else (game.away, game.home)
                    score = game_match(teams, (target.away, target.home))
                    if score:
                        candidates.append((score, flipped, game, target))
        candidates.sort(key=lambda candidate: (-candidate[0], candidate[1]))

        paired = set()
        for _, flipped, game, target in candidates:
            if id(game) in paired or id(target) in paired:
                continue
            paired |= {id(game), id(target)}
            away, home = (game.home, game.away) if flipped else (game.away, game.home)
            for quote in game.quotes:
                quote.home = quote.home != flipped
            for known, new in ((target.away, away), (target.home, home)):
                known.abbreviations += new.abbreviations
                known.names += new.names
            target.quotes += game.quotes
            # Novig lists some starts a few minutes late.
            target.start = min(filter(None, (target.start, game.start)), default=None)
            target.live = target.live or game.live
        merged += [game for game in games if id(game) not in paired]
    return merged


@dataclass
class Opportunity:
    """Backing both sides of one line, each at its best price."""

    game: Game
    line: Decimal | None
    away: Quote
    home: Quote

    @property
    def cost(self):
        """What it costs to collect $1 whichever side wins."""
        return self.away.net + self.home.net

    @property
    def roi(self):
        return (1 - self.cost) / self.cost


def best_offer(offers, min_stake):
    usable = [q for q in offers if q.size is None or (q.size > 0 and q.stake >= min_stake)]
    return min(usable, key=lambda q: (q.net, q.size is None, -(q.size or 0)), default=None)


def read_polymarket_book(slug):
    try:
        return get_json(f"{POLYMARKET}/v1/markets/{slug}/book")["marketData"]
    except OSError:
        return {}


def size_polymarket(games, select):
    """Polymarket's event listings quote no size. Read the order book of each of its markets
    among the quotes `select()` returns, and select again on the books' own prices, until
    every selected quote has a size."""
    quotes_of_slug = defaultdict(list)
    for game in games:
        for quote in game.quotes:
            if quote.polymarket_side:
                quotes_of_slug[quote.polymarket_side[0]].append(quote)

    unreadable = 0
    while True:
        slugs = sorted({quote.polymarket_side[0] for quote in select() if quote.size is None})
        if not slugs:
            break
        for slug, book in zip(slugs, BOOK_READERS.map(read_polymarket_book, slugs)):
            unreadable += not book
            for quote in quotes_of_slug[slug]:
                is_long = quote.polymarket_side[1]
                levels = book.get("offers" if is_long else "bids")
                if levels:
                    top_price = Decimal(levels[0]["px"]["value"])
                    quote.price = top_price if is_long else 1 - top_price
                    quote.size = Decimal(levels[0]["qty"])
                else:
                    quote.size = Decimal(0)
    if unreadable:
        warn(f"{unreadable} Polymarket order books could not be read and were left out.")


def main_line(game):
    """The spread line whose best prices on the two sides are closest to even."""
    best = defaultdict(dict)
    for quote in game.quotes:
        if quote.line is not None:
            sides = best[quote.line]
            sides[quote.home] = min(sides.get(quote.home, quote.net), quote.net)
    two_sided = {line: sides for line, sides in best.items() if len(sides) == 2}
    return min(two_sided, key=lambda line: abs(two_sided[line][True] - two_sided[line][False]), default=None)


def top_opportunities(games, count, min_stake):
    """The lines that return the most from backing both sides, best first."""
    offers_of = defaultdict(list)
    for index, game in enumerate(games):
        for quote in game.quotes:
            offers_of[index, quote.line, quote.home].append(quote)

    def ranked():
        found = []
        for (index, line, home), away_offers in offers_of.items():
            if home:
                continue
            away = best_offer(away_offers, min_stake)
            home = best_offer(offers_of.get((index, line, True), []), min_stake)
            if away and home:
                found.append(Opportunity(games[index], line, away, home))
        return sorted(found, key=lambda opportunity: opportunity.roi, reverse=True)[:count]

    size_polymarket(games, lambda: [leg for top in ranked() for leg in (top.away, top.home)])
    return ranked()


BONUS, CASH, SAFETY_NET, BOOSTED = "Bonus bet", "Cash bet", "Safety net bet", "Boosted bet"


@dataclass
class HedgedBet:
    """A bet on one side of a line, hedged with cash on the other."""

    game: Game
    bet: Quote
    hedge: Quote | None
    amount: Decimal
    # Cash gone if the bet loses: none of a bonus bet, all of a cash or boosted bet, and a safety
    # net bet's stake less what the bonus bets it comes back as are worth.
    at_risk: Decimal
    boost: Decimal  # share added to the winnings; 0 unless the bet is boosted

    @property
    def winnings(self):
        return self.amount * (1 - self.bet.net) / self.bet.net * (1 + self.boost)

    @property
    def hedge_payout(self):
        """What the hedge has to collect for the profit to be the same whichever side wins."""
        return self.winnings + self.at_risk

    @property
    def hedge_stake(self):
        return self.hedge_payout * self.hedge.net

    @property
    def profit(self):
        return self.winnings - self.hedge_stake


def best_hedged_bets(games, amount, at_risk, boost, hedge_cap, dearest, bet_dearest, shown):
    """The HEDGED_BETS_SHOWN hedged bets that profit the most, best first, leaving out the bets
    already `shown`.

    Neither price may be above `dearest` nor the bet's above `bet_dearest`, both need enough
    behind them for the whole bet, and the hedge may not stake more than `hedge_cap`, if there
    is one.
    """
    offers_of = defaultdict(list)
    for index, game in enumerate(games):
        for quote in game.quotes:
            offers_of[index, quote.line, quote.home].append(quote)

    # Kalshi quotes a side twice, as Yes on it and No on its opponent: only the better is shown.
    def side_of(game, bet):
        return id(game), bet.book, bet.home, bet.handicap

    shown_sides = {side_of(hedged.game, hedged.bet) for hedged in shown}

    def best(depth):
        """`depth` times as many as are shown."""
        found = []
        for (index, line, home), offers in offers_of.items():
            hedges = offers_of.get((index, line, not home), [])
            for bet in offers:
                if side_of(games[index], bet) in shown_sides:
                    continue
                if bet.net > bet_dearest or (bet.size is not None and bet.stake < amount):
                    continue
                hedged = HedgedBet(games[index], bet, None, amount, at_risk, boost)
                usable = [
                    q
                    for q in hedges
                    if q.net <= dearest and (q.size is None or q.size >= hedged.hedge_payout)
                ]
                if not usable:
                    continue
                # The cheapest hedge also stakes the least, so no other one fits a cap it breaks.
                hedged.hedge = min(usable, key=lambda q: q.net)
                if hedge_cap is None or hedged.hedge_stake <= hedge_cap:
                    found.append(hedged)
        found.sort(key=lambda hedged: hedged.profit, reverse=True)
        kept = {}
        for hedged in found:
            if len(kept) == depth * HEDGED_BETS_SHOWN:
                break
            kept.setdefault(side_of(hedged.game, hedged.bet), hedged)
        return list(kept.values())

    # Usually the leaders fit. When they turn out too small, size ever more runners-up a round
    # rather than one market per round trip.
    depths = itertools.chain([2, 8], itertools.repeat(40))
    size_polymarket(
        games, lambda: [leg for found in best(next(depths)) for leg in (found.bet, found.hedge)]
    )
    return best(1)


def american(price):
    if price > Decimal("0.5"):
        return f"{-100 * price / (1 - price):.0f}"
    return f"+{100 * (1 - price) / price:.0f}"


def odds(price):
    """American odds, then decimal odds: "-500, 1.20x"."""
    return f"{american(price)}, {1 / price:.2f}x"


def side_label(game, quote):
    label = (game.home if quote.home else game.away).abbreviations[0]
    if quote.handicap is not None:
        label += f" {quote.handicap.normalize():+f}"
    return label


def matchup(game):
    away, home = (team.names[0] if team.names else team.abbreviations[0] for team in (game.away, game.home))
    when = f"{game.day:%a %b %d}"
    if game.start:
        local = game.start.astimezone()
        when = f"{local:%a %b %d} {local:%I:%M %p}".replace(" 0", " ")
    return f"{away} @ {home}  |  {when}  |  {'LIVE' if game.live else 'pregame'}"


def side_cells(game, line, home, offers, min_stake):
    label = (game.home if home else game.away).abbreviations[0]
    if line is not None:
        label += f" {(-line if home else line).normalize():+f}"
    best = best_offer(offers, min_stake)
    if not best:
        return f"{label:<12}{'-':>15}{'-':>13}  {'':<10}"
    return f"{label:<12}{odds(best.net):>15}{f'${best.stake:,.2f}':>13}  {best.book:<10}"


def print_game(game, shown, min_stake):
    print(f"\n{matchup(game)}")
    columns = f"{'Side':<12}{'Odds':>15}{'Avail':>13}  {'Book':<10}"
    print(f"  {'':<11}{columns}   {columns}".rstrip())
    for line in dict.fromkeys(line for line, _ in shown):
        label = "Moneyline" if line is None else "Spread"
        cells = "   ".join(
            side_cells(game, line, home, shown[line, home], min_stake) for home in (False, True)
        )
        print(f"  {label:<11}{cells}".rstrip())


def print_opportunity(rank, opportunity):
    game, line = opportunity.game, opportunity.line
    print(f"\n{rank:>2}. {opportunity.roi:+.2%}  {game.league}  {matchup(game)}")

    # Equal payouts on both sides, as large as the smaller offer allows.
    payout = min(opportunity.away.size, opportunity.home.size)
    for team, handicap, leg in (
        (game.away, line, opportunity.away),
        (game.home, line and -line, opportunity.home),
    ):
        label = team.abbreviations[0]
        if handicap is not None:
            label += f" {handicap.normalize():+f}"
        stake = f"${payout * leg.net:,.2f}"
        print(f"      {label:<12}{odds(leg.net):>15}  {leg.book:<10}  stake {stake:>12}")
    staked = payout * opportunity.cost
    result = "profit" if payout > staked else "loss"
    print(f"      Stake ${staked:,.2f} to collect ${payout:,.2f}: {result} ${abs(payout - staked):,.2f}")


def period(window):
    last_day = window[1] - timedelta(days=1)
    return f"{window[0]:%a %b %d} to {last_day:%a %b %d, %Y}"


def lines_shown(games, min_stake):
    """For each game, by id, the offers on each side of its moneyline and main spread."""
    shown_of = {}
    for game in games:
        spread = main_line(game)
        lines = [None] if spread is None else [None, spread]
        shown = {(line, home): [] for line in lines for home in (False, True)}
        for quote in game.quotes:
            if (quote.line, quote.home) in shown:
                shown[quote.line, quote.home].append(quote)
        shown_of[id(game)] = shown
    cells = [offers for shown in shown_of.values() for offers in shown.values()]
    size_polymarket(
        games, lambda: [best for offers in cells if (best := best_offer(offers, min_stake))]
    )
    return shown_of


def show_best_lines(games, window, args):
    print(f"\nBest lines for {period(window)}")
    if not games:
        print("No games with open moneyline or spread markets.")
        return
    print("Odds include the book's taker fee; Avail is the most you can stake at that price.")
    print(f"Prices with under ${args.min_stake:,.0f} available are ignored (--min-stake).")

    shown_of = lines_shown(games, args.min_stake)
    far_future = datetime.max.replace(tzinfo=timezone.utc)
    for league in LEAGUES:
        league_games = [game for game in games if game.league == league.name]
        if league_games:
            print(f"\n{'=' * 30} {league.name} {'=' * 30}")
        for game in sorted(league_games, key=lambda g: (g.day, g.start or far_future)):
            print_game(game, shown_of[id(game)], args.min_stake)


def show_top_arbitrage(games, window, args):
    top = top_opportunities(games, args.top, args.min_stake)
    print(f"\nTop {args.top} lines by arbitrage return, {period(window)}")
    if not top:
        print("No line is quoted on both sides.")
        return
    print("Return is from backing both sides at the best price for each, taker fees included.")
    print("A negative return is not an arbitrage. Stakes are the most those prices allow.")
    print(f"Prices with under ${args.min_stake:,.0f} available are ignored (--min-stake).")
    for rank, opportunity in enumerate(top, start=1):
        print_opportunity(rank, opportunity)


def ask_bet(kind):
    """The bet's amount, then optionally the most its hedge may stake and the minimum odds the
    bet may be at, as the cost of $1 of payout. The odds come third, or second when they are
    signed or end in x. A boosted bet may also give its boost, as an entry ending in %; the
    boost returned is 0 for every other kind of bet."""
    boosted = kind == BOOSTED
    options = " hedge and the minimum odds for the bet, e.g. 500 2000 -200"
    if boosted:
        options = (
            " hedge, the minimum odds for the bet and the boost if it is not"
            f" {PROFIT_BOOST:.0%}, e.g. 500 2000 -200 50%"
        )
    while True:
        entry = input(
            f"{kind} amount in dollars, then optionally the most to stake on the{options}"
            " (blank to go back): "
        )
        entries = entry.replace("$", "").replace(",", "").split()
        if not entries:
            return None
        boosts = [entry for entry in entries if entry.endswith("%")]
        entries = [entry for entry in entries if not entry.endswith("%")]
        has_odds = len(entries) == 3 or (
            len(entries) == 2 and (entries[1][0] in "+-" or entries[1].lower().endswith("x"))
        )
        min_odds = parse_odds(entries.pop()) if has_odds else None
        try:
            amounts = [Decimal(entry) for entry in entries]
            given = [Decimal(entry[:-1]) / 100 for entry in boosts]
        except InvalidOperation:
            amounts = []
        if (
            len(amounts) in (1, 2)
            and all(amount.is_finite() and amount > 0 for amount in amounts + given)
            and (min_odds is not None or not has_odds)
            and len(given) <= boosted
        ):
            boost = given[0] if given else PROFIT_BOOST if boosted else Decimal(0)
            return amounts[0], amounts[1] if len(amounts) == 2 else None, min_odds, boost
        print("Enter an amount such as 100, optionally followed by a hedge cap and minimum odds")
        print("for the bet, such as 500 2000 -200. Minimum odds alone need their sign: 500 -200.")
        if boosted:
            print("A boost is entered with its % sign, such as 50%.")


def hedged_bet_finder(kind, games, args, amount, hedge_cap, min_odds, boost):
    """best_hedged_bets for this bet, left to be given the bets already shown."""
    at_risk = {BONUS: Decimal(0), SAFETY_NET: amount * (1 - args.conversion_rate)}.get(kind, amount)
    dearest = Decimal(args.favorite_cap) / (args.favorite_cap + 100)
    bet_dearest = dearest if min_odds is None else min(dearest, min_odds)
    return partial(best_hedged_bets, games, amount, at_risk, boost, hedge_cap, dearest, bet_dearest)


def show_hedged_bets(kind, games, window, args, amount, hedge_cap, min_odds, boost):
    rate = args.conversion_rate
    more = hedged_bet_finder(kind, games, args, amount, hedge_cap, min_odds, boost)
    found = more([])

    print(f"\nBest use of a ${amount:,.2f} {kind.lower()}, {period(window)}")
    if kind == BONUS:
        print("A bonus bet's stake is not paid back, so a win pays only the winnings. The hedge is")
        print("cash on the other side, sized so the profit is the same whichever side wins.")
    elif kind == CASH:
        print("The hedge is cash on the other side, sized so the profit is the same whichever side")
        print("wins. A loss means no line is an arbitrage for a bet this size at that book.")
    elif kind == BOOSTED:
        print(f"A {(boost * 100).normalize():f}% profit boost adds that much to the winnings of a cash bet. The bet's odds are")
        print("shown before the boost. The hedge is cash on the other side, sized so the profit is the")
        print("same whichever side wins.")
    else:
        print("A safety net bet is cash that comes back as bonus bets if it loses. Those are valued")
        print(f"at {(rate * 100).normalize():f}% of their amount (--conversion-rate); option 3 shows what a bonus bet")
        print("keeps now. The hedge is cash on the other side, sized so the profit is the same")
        print("whichever side wins.")
    print("Every moneyline and spread is considered, taker fees included, and both prices have")
    print(f"enough available for the whole bet. Neither side is bet at odds beyond -{args.favorite_cap}")
    print("(--favorite-cap).")
    limits = ""
    if hedge_cap is not None:
        print(f"The hedge is kept to a stake of ${hedge_cap:,.2f} at most.")
        limits += f" and a hedge of ${hedge_cap:,.2f} or less"
    if min_odds is not None:
        print(f"The bet is kept to odds of {american(min_odds)} or longer.")
        limits += f" and the bet at {american(min_odds)} or longer"
    for number, hedged in enumerate(found, start=1):
        print_hedged_bet(kind, hedged, f"{number}. ")
    if not found:
        print(
            f"\n{kind}: nothing within -{args.favorite_cap} with enough available on both"
            f" sides{limits}."
        )
    reprice_at_own_book(kind, found, hedge_cap, min_odds, more)


def parse_odds(entry):
    """The cost of $1 of payout at American odds such as -250 or +150, or decimal odds such as
    1.87x. An unsigned number is American from 100 up and decimal below it."""
    text = "".join(entry.split()).lower()
    is_decimal = text.endswith("x")
    is_signed = text.startswith(("+", "-"))
    try:
        value = Decimal(text.removesuffix("x"))
    except InvalidOperation:
        return None
    if not value.is_finite() or (is_decimal and is_signed):
        return None
    if not is_decimal and (is_signed or value >= 100):
        if abs(value) < 100:
            return None
        return -value / (100 - value) if value < 0 else 100 / (value + 100)
    return 1 / value if value > 1 else None


def reprice_at_own_book(kind, found, hedge_cap, min_odds, more):
    """Asks which of the hedged bets shown the user is taking and the odds their own book gives
    on its bet side, then prices the hedge against those odds. Their book charges no fee.
    Answering `next` shows the next best bets, from `more(found)`, to choose from as well."""
    if not found:
        return
    while True:
        entry = input(
            f"\nWhich line will you be using? 1 to {len(found)}, or next for more"
            " (blank to go back): "
        ).strip()
        if not entry:
            return
        if entry.lower() == "next":
            added = more(found)
            for number, hedged in enumerate(added, start=len(found) + 1):
                print_hedged_bet(kind, hedged, f"{number}. ")
            if not added:
                print("No more lines fit.")
            found += added
            continue
        if entry.isdecimal() and 1 <= int(entry) <= len(found):
            chosen = found[int(entry) - 1]
            break
        print(f"Enter a number from 1 to {len(found)}, or next.")
    before_boost = ", before the boost" if kind == BOOSTED else ""
    while True:
        entry = input(
            f"What line are you getting through your book on {side_label(chosen.game, chosen.bet)}"
            f"{before_boost}? e.g. -250 or 1.87x (blank to go back): "
        ).strip()
        if not entry:
            return
        price = parse_odds(entry)
        if price is not None:
            break
        print("Enter American odds such as -250 or +150, or decimal odds such as 1.87x.")

    repriced = repriced_at_own_book(chosen, price)
    print_hedged_bet(kind, repriced)
    for caution in reprice_cautions(repriced, hedge_cap, min_odds):
        print(f"      {caution}")


def repriced_at_own_book(chosen, price):
    own = replace(chosen.bet, book=OWN_BOOK, price=price, fee_rate=Decimal(0))
    return replace(chosen, bet=own)


def reprice_cautions(repriced, hedge_cap, min_odds):
    """Where a bet repriced at the user's own book no longer fits what was searched for."""
    cautions = []
    hedge, price = repriced.hedge, repriced.bet.price
    if hedge.size < repriced.hedge_payout:
        cautions.append(
            f"Only ${hedge.stake:,.2f} can be staked on the hedge at that price, short of the"
            f" ${repriced.hedge_stake:,.2f} it needs."
        )
    if hedge_cap is not None and repriced.hedge_stake > hedge_cap:
        cautions.append(f"The hedge is over the ${hedge_cap:,.2f} you set as the most to stake on it.")
    if min_odds is not None and price > min_odds:
        cautions.append(
            f"{american(price)} is shorter than the {american(min_odds)} you set as the minimum odds."
        )
    return cautions


def hedged_bet_outcomes(kind, found):
    """How the hedged bet comes out, and what it makes when the hedge's side wins."""
    amount = found.amount
    result = "loss" if found.profit < 0 else "profit"
    profit = abs(found.profit)
    hedge_won = found.hedge_payout - found.hedge_stake
    if kind == BONUS:
        outcome = f"keep ${found.profit:,.2f}, {found.profit / amount:.1%} of it"
        hedge_wins = f"the hedge makes ${found.profit:,.2f}."
    elif kind == CASH:
        staked = amount + found.hedge_stake
        outcome = f"{result} ${profit:,.2f}, {profit / staked:.2%} of the ${staked:,.2f} staked"
        hedge_wins = f"${hedge_won:,.2f} won less the ${amount:,.2f} bet."
    elif kind == BOOSTED:
        outcome = f"{result} ${profit:,.2f}, {profit / amount:.1%} of the bet"
        hedge_wins = f"${hedge_won:,.2f} won less the ${amount:,.2f} bet."
    else:
        outcome = f"{result} ${profit:,.2f}, {profit / amount:.1%} of the bet"
        hedge_wins = (
            f"${hedge_won:,.2f} won less the ${amount:,.2f} bet, plus ${amount:,.2f} in bonus"
            f" bets worth ${amount - found.at_risk:,.2f}."
        )
    return outcome, hedge_wins


def print_hedged_bet(kind, found, numbering=""):
    game, bet, hedge, amount = found.game, found.bet, found.hedge, found.amount
    outcome, hedge_wins = hedged_bet_outcomes(kind, found)

    print(f"\n{numbering}{kind} at {bet.book}: {outcome}")
    print(f"      {game.league}  {matchup(game)}")
    for role, leg, stake in ((kind.split()[0], bet, amount), ("Hedge", hedge, found.hedge_stake)):
        cells = f"{side_label(game, leg):<12}{odds(leg.net):>15}  {leg.book:<10}"
        print(f"      {role:<7}  {cells}  stake {f'${stake:,.2f}':>12}")
    won = "won with the boost" if found.boost else "won"
    print(
        f"      {side_label(game, bet)} wins: ${found.winnings:,.2f} {won} less the"
        f" ${found.hedge_stake:,.2f} hedge.  {side_label(game, hedge)} wins: {hedge_wins}"
    )


def load_games(args):
    """Every book's current lines for the chosen leagues, and the days they cover: args.days of
    them from the start of today."""
    window_start = datetime.combine(date.today(), time.min).astimezone()
    window = window_start, window_start + timedelta(days=args.days)
    fetchers = {
        "Novig": novig,
        "Polymarket": lambda *league_window: polymarket(*league_window, args.polymarket_fee),
        "Kalshi": kalshi,
    }
    leagues = [league for league in LEAGUES if league.name in args.leagues]
    with ThreadPoolExecutor(max_workers=len(fetchers) * len(leagues)) as pool:
        pending = {
            (book, league.name): pool.submit(fetch, league, *window)
            for book, fetch in fetchers.items()
            for league in leagues
        }
    games_of = defaultdict(list)
    for (book, league), result in pending.items():
        try:
            games_of[book] += result.result()
        except Exception as error:
            warn(f"{book} {league} skipped: {type(error).__name__}: {error}")
    games = [game for game in merge(games_of[book] for book in fetchers) if game.quotes]
    return games, window


def menu(args):
    options = {
        "1": (f"All games in the next {args.days} days, with the best book for each line", show_best_lines, None),
        "2": (f"Top {args.top} arbitrage opportunities", show_top_arbitrage, None),
        "3": ("Best use of a bonus bet", partial(show_hedged_bets, BONUS), partial(ask_bet, BONUS)),
        "4": ("Best use of a cash (non-bonus) bet", partial(show_hedged_bets, CASH), partial(ask_bet, CASH)),
        "5": (
            "Best use of a safety net bet (bonus bets back if it loses)",
            partial(show_hedged_bets, SAFETY_NET),
            partial(ask_bet, SAFETY_NET),
        ),
        "6": (
            f"Best use of a profit boost token ({PROFIT_BOOST:.0%} unless you enter another)",
            partial(show_hedged_bets, BOOSTED),
            partial(ask_bet, BOOSTED),
        ),
    }
    while True:
        print(f"\nBest Lines: {', '.join(args.leagues)}")
        for key, (label, _, _) in options.items():
            print(f"  {key}. {label}")
        print("  0. Exit")
        choice = input("Option: ").strip()
        if choice == "0":
            return
        if choice not in options:
            print(f"'{choice}' is not an option.")
            continue
        _, show, ask = options[choice]
        answers = ask() if ask else ()
        if answers is None:
            continue
        print("Fetching current lines...")
        games, window = load_games(args)
        show(games, window, args, *answers)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--leagues",
        nargs="+",
        type=str.upper,
        choices=[league.name for league in LEAGUES],
        default=[league.name for league in LEAGUES],
        metavar="LEAGUE",
        help="leagues to cover: NBA NFL NHL NCAAF (default: all)",
    )
    parser.add_argument(
        "--days", type=int, default=7, help="days to cover, starting today (default: 7)"
    )
    parser.add_argument(
        "--top", type=int, default=10, metavar="COUNT", help="arbitrage lines to show (default: 10)"
    )
    parser.add_argument(
        "--polymarket-fee",
        type=Decimal,
        metavar="COEFFICIENT",
        help="your Polymarket US taker fee coefficient, e.g. 0.0695 (default: the 0.095 base tier)",
    )
    parser.add_argument(
        "--min-stake",
        type=Decimal,
        default=Decimal(10),
        metavar="DOLLARS",
        help="ignore prices with less than this much available to stake (default: 10)",
    )
    parser.add_argument(
        "--favorite-cap",
        type=lambda odds: abs(int(odds)),
        default=800,
        metavar="ODDS",
        help="in options 3 to 6, bet no favorite heavier than -ODDS (default: 800)",
    )
    parser.add_argument(
        "--conversion-rate",
        type=Decimal,
        default=Decimal("0.70"),
        metavar="RATE",
        help="in option 5, what a bonus bet is worth as a share of its amount (default: 0.70)",
    )
    args = parser.parse_args()
    if not 0 < args.conversion_rate <= 1:
        parser.error("argument --conversion-rate: must be above 0 and at most 1")
    # Ctrl+C, or the end of piped input, leaves the menu.
    try:
        menu(args)
    except (EOFError, KeyboardInterrupt):
        print()


if __name__ == "__main__":
    main()
