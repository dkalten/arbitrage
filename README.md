# Best Lines

A terminal app that compares moneylines and spreads for upcoming NBA, NFL, NHL and NCAAF games
across three books: Novig, Kalshi and Polymarket US. It shows the best book for each line, finds
arbitrage opportunities, and works out the best hedged use of a sportsbook promotion.

Every price shown is what a taker pays right now, with that book's trading fee included. The app
reads public price data only. It needs no account or API key, and it places no bets.

## What it does

Running the app opens a menu:

| Option | What you get |
| --- | --- |
| 1 | Every game in the coming days, with the best book for each side of the moneyline and spread |
| 2 | The top arbitrage opportunities: lines where backing both sides at the best prices returns the most |
| 3 | The best hedged use of a bonus bet |
| 4 | The best hedged use of a cash (non-bonus) bet |
| 5 | The best hedged use of a safety net bet (one that comes back as bonus bets if it loses) |
| 6 | The best hedged use of a profit boost token |
| 0 | Exit |

Options 3 to 6 are for a promotion at your own sportsbook. Each one pairs your bet with a cash
hedge on the other side at Novig, Kalshi or Polymarket, sized so the profit is the same whichever
side wins.

1. Enter the bet amount, such as `100`. You can add a cap on the hedge stake and minimum odds for
   the bet, such as `500 2000 -200`. For option 6, enter a boost other than 100% with its percent
   sign, such as `50%`.
2. The app lists the best lines. Enter `next` to see more.
3. Pick a line and enter the odds your own book gives on it, such as `-250` or `1.87x`. The app
   reprices the hedge against those odds.

Prices are a snapshot from the moment you choose an option, and they move. Check them at the book
before you bet.

## Run it

### macOS executable

Build it with the command under [Build from source](#build-from-source), then run:

```sh
./build/best_lines_mac
```

The executable runs on Apple Silicon and Intel Macs with macOS 13.3 or later. It uses only
libraries that come with macOS.

### Python

The same program is in `best_lines.py`. It needs Python 3.10 or later and nothing else:

```sh
python3 best_lines.py
```

## Options

Both versions take the same options:

| Option | Meaning | Default |
| --- | --- | --- |
| `--leagues LEAGUE [LEAGUE ...]` | Leagues to cover: `NBA`, `NFL`, `NHL`, `NCAAF` | all four |
| `--days DAYS` | Days to cover, starting today | 7 |
| `--top COUNT` | Arbitrage lines to show in option 2 | 10 |
| `--polymarket-fee COEFFICIENT` | Your Polymarket US taker fee coefficient, such as `0.0695` | 0.095, the base tier |
| `--min-stake DOLLARS` | Ignore prices with less than this much available to stake | 10 |
| `--favorite-cap ODDS` | In options 3 to 6, bet no favorite heavier than `-ODDS` | 800 |
| `--conversion-rate RATE` | In option 5, what a bonus bet is worth as a share of its amount | 0.70 |
| `-h`, `--help` | Show the help text | |

For example, to see only this weekend's NBA and NFL games:

```sh
./build/best_lines_mac --leagues NBA NFL --days 2
```

## Build from source

The C++ version is a single file, `best_lines.cpp`, written in C++23.

### macOS

You need Apple's command line tools (`xcode-select --install`). The repository includes the curl
headers and a link stub for the libcurl that ships with macOS, so there is nothing else to install.

```sh
mkdir -p build
clang++ -std=c++23 -O2 -arch arm64 -arch x86_64 -mmacosx-version-min=13.3 \
  -Imacos/include best_lines.cpp macos/libcurl.tbd -o build/best_lines_mac
```

### Windows and other platforms

Use CMake 3.20 or later with a C++23 compiler. On Windows the app uses WinHTTP, which is part of
Windows. Elsewhere it needs the libcurl development package.

```sh
cmake -B build
cmake --build build --config Release
```

## Share the macOS executable

Cloud drives such as Google Drive strip the permission that marks a file as executable, so an
executable uploaded on its own downloads as a plain document. Zip it first, because a zip keeps
the permission:

```sh
ditto -c -k --norsrc --noextattr build/best_lines_mac build/best_lines_mac.zip
```

Upload `best_lines_mac.zip`. On the Mac that downloads it:

1. Double-click the zip to unpack it.
2. Right-click `best_lines_mac`, choose **Open**, then click **Open** again. macOS asks this the
   first time because the app is not signed with an Apple developer ID. The Terminal equivalent is
   `xattr -d com.apple.quarantine best_lines_mac`.
3. After that, double-click it to run it in Terminal.
