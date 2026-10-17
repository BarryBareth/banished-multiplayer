// The in-game multiplayer windows in the game's own UI (see ingame.h): session bar, trade, trade offers, settling.
// Layouts: the companion mod's Dialog/BmpBar.rsc, BmpTrade.rsc, BmpOffer.rsc, BmpSettle.rsc (gamedlg::Window).
#include "report.h"
#include "ingame.h"
#include "gamedlg.h"
#include "gameui.h"
#include "common.h"
#include "net.h"
#include "towns.h"
#include "commands.h"
#include "tint.h"
#include <algorithm>
#include <string>
#include <vector>

namespace ingame {

static const uint32_t kGood = 0xFF8CD873, kWarn = 0xFFFFCC59, kBad = 0xFFFF7366, kPlain = 0xFFC0C0C0;
static bool g_native = true;   // the mod's dialogs work (else the overlay's ImGui windows)
static bool g_tradeWanted;     // the player opened the trade window

static uint32_t TownColour(int t) {
    float rgb[3];
    tint::ColorOf(t, rgb);
    auto c = [](float f) { return (uint32_t)(f < 0 ? 0 : f > 1 ? 255 : f * 255); };
    return 0xFF000000 | c(rgb[0]) << 16 | c(rgb[1]) << 8 | c(rgb[2]);
}
static std::string NameOf(const net::UiInfo& u, int player) {
    for (auto& p : u.roster) if (p.id == player) return p.name;
    return "A player";
}

// ---------------------------------------------------------------- session bar
struct Bar : gamedlg::Window {
    enum { kPlayers = 3000, kInvite, kTrade, kResync, kLeave, kYes, kNo, kColours, kReport };
    bool players = false, confirm = false;
    net::UiInfo u;
    const char* Resource() const override { return "Dialog/BmpBar.rsc"; }
    void Init() override {
        Id("bmpBarPlayers", kPlayers); Id("bmpBarInvite", kInvite); Id("bmpBarTrade", kTrade); Id("bmpBarResync", kResync);
        Id("bmpBarLeave", kLeave); Id("bmpBarYes", kYes); Id("bmpBarNo", kNo); Id("bmpBarColours", kColours);
        Id("bmpBarReport", kReport);
        Text("bmpBarInvite", "Invite");
        Text("bmpBarTrade", "Trade");
        Text("bmpBarResync", "Resync");
        Text("bmpBarLeave", "Leave");
        Text("bmpBarYes", "Yes, leave");
        Text("bmpBarNo", "Stay");
        Text("bmpBarColours", "Colours");
    }
    void Refresh() override {
        bool desync = u.status.find("Out of sync") == 0;
        Text("bmpBarState", u.resyncing ? "SYNCING" : desync ? "OUT OF SYNC" : "MULTIPLAYER");
        gameui::SetColor(E("bmpBarState"), u.resyncing ? kWarn : desync ? kBad : kGood);
        char info[96];
        if (u.towns > 1) sprintf_s(info, "Town %d of %d  -  %d players", u.town + 1, u.towns, (int)u.roster.size());
        else sprintf_s(info, "%d players", (int)u.roster.size());
        Text("bmpBarInfo", std::string(info) + (u.paused ? "  -  paused" : ""));
        int shown = 0;  // the other towns' names in their colours (their buildings are outlined in it)
        for (int t = 0; t < u.towns && shown < 4; t++) {
            if (t == u.town) continue;
            std::string who = "Town " + std::to_string(t + 1);
            for (auto& p : u.roster)
                if (net::TownOfPlayer(p.id) == t) who = p.name;
            char name[24];
            sprintf_s(name, "bmpBarTown%d", shown++);
            Text(name, who);
            gameui::SetColor(E(name), TownColour(t));
            Vis(name, true);
        }
        for (int i = shown; i < 4; i++) { char name[24]; sprintf_s(name, "bmpBarTown%d", i); Vis(name, false); }
        Vis("bmpBarColours", u.towns > 1);
        Check("bmpBarColours", tint::Enabled());
        Text("bmpBarPlayers", players ? "Hide players" : "Players");
        Vis("bmpBarInvite", u.hosting);
        Vis("bmpBarTrade", u.towns > 1);
        std::string roster;
        for (auto& p : u.roster) {
            if (!roster.empty()) roster += ",   ";  // text elements have no line breaks
            roster += p.name + (p.id == 0 ? "  (host)" : "") + (p.id == u.me ? "  (you)" : "");
            if (u.started && !p.synced) roster += "  loading...";
        }
        Text("bmpBarRoster", roster);
        Vis("bmpBarRoster", players);
        bool status = u.resyncing || desync || (!u.status.empty() && u.status.find("Hosting") != 0 && u.status.find("Playing") != 0);
        Text("bmpBarStatus", u.status);
        Vis("bmpBarStatus", status);
        Text("bmpBarConfirmText", u.hosting ? "End the game for everybody?" : "Leave this game? (your town keeps running alone)");
        Vis("bmpBarConfirm", confirm);
    }
    void OnEvent(int id, int evt) override {
        if (evt != 0x12 && evt != 0x15) return;
        switch (id) {
        case kPlayers: players = !players; break;
        case kInvite: net::UiDo(net::kUiInvite); break;
        case kTrade: g_tradeWanted = !g_tradeWanted; break;
        case kResync: net::UiDo(net::kUiResync); break;
        case kLeave: confirm = true; break;
        case kYes: confirm = false; net::UiDo(net::kUiLeave); break;
        case kNo: confirm = false; break;
        case kColours: tint::SetEnabled(!tint::Enabled()); break;
        case kReport: report::Open(false); break;
        }
    }
};

// ---------------------------------------------------------------- trade
struct Trade : gamedlg::Window {
    enum { kPartner = 3100, kOnce = 3110, kSeason, kYear, kSend, kDeliver, kClose, kCancel = 3200, kGive = 3300, kAsk };
    int partner = -1, giveIdx = 0, askIdx = 0, giveN = 10, askN = 10, period = 0;
    std::vector<int> partners;              // player ids behind the partner checks
    std::vector<int> agreeIds;              // agreement ids behind the Cancel buttons
    std::vector<std::string> giveItems, askItems;
    std::vector<towns::Good> mine, theirs;
    net::UiInfo u;
    const char* Resource() const override { return "Dialog/BmpTrade.rsc"; }
    void Init() override {
        for (int i = 0; i < 4; i++) { char n[24]; sprintf_s(n, "bmpPartner%d", i); Id(n, kPartner + i); }
        for (int i = 0; i < 6; i++) { char n[24]; sprintf_s(n, "bmpAgreeCancel%d", i); Id(n, kCancel + i); Text(n, "Cancel"); }
        Id("bmpOnce", kOnce); Id("bmpSeason", kSeason); Id("bmpYear", kYear); Id("bmpSend", kSend); Id("bmpDeliver", kDeliver);
        Id("bmpTradeClose", kClose); Id("bmpGive", kGive); Id("bmpAsk", kAsk);
        Text("titleTrade", "Trade");
        Text("bmpPartnerLabel", "Trade with");
        Text("bmpGiveLabel", "You give");
        Text("bmpAskLabel", "You get");
        Text("bmpPeriodLabel", "How often");
        Text("bmpOnce", "Once");
        Text("bmpSeason", "Every season");
        Text("bmpYear", "Every year");
        Text("bmpStandingHeading", "Standing trades");
        Text("bmpWaitHeading", "Waiting to be carried");
        Text("bmpDeliver", "Hand over directly");
        Text("bmpNobody", "Nobody to trade with.");
        gameui::NumberSet(E("bmpGiveN"), giveN);
        gameui::NumberSet(E("bmpAskN"), askN);
        giveItems.clear();
        askItems.clear();
    }
    static std::vector<std::string> Items(const std::vector<towns::Good>& goods) {
        std::vector<std::string> out;
        for (auto& g : goods) out.push_back(commands::DescName(g.desc) + "  (" + std::to_string(g.count) + ")");
        return out;
    }
    void Refresh() override {
        partners.clear();
        for (auto& p : u.roster) if (p.id != u.me && partners.size() < 4) partners.push_back(p.id);
        bool any = !partners.empty();
        Vis("bmpNobody", !any);
        for (const char* n : {"bmpPartnerRow", "bmpOfferTable", "bmpPeriodRow", "bmpSend", "bmpTradeHint"}) Vis(n, any);
        if (!any) return;
        if (std::find(partners.begin(), partners.end(), partner) == partners.end()) partner = partners[0];
        for (int i = 0; i < 4; i++) {
            char n[24];
            sprintf_s(n, "bmpPartner%d", i);
            Vis(n, i < (int)partners.size());
            if (i < (int)partners.size()) { Text(n, NameOf(u, partners[i])); Check(n, partners[i] == partner); }
        }
        // goods (combos refilled when the lists change; the selection follows the good)
        int g = gameui::ComboGet(E("bmpGive")), a = gameui::ComboGet(E("bmpAsk"));
        void* giveDesc = g >= 0 && g < (int)mine.size() ? mine[g].desc : nullptr;
        void* askDesc = a >= 0 && a < (int)theirs.size() ? theirs[a].desc : nullptr;
        mine = towns::Goods(towns::Local());
        theirs = towns::Goods(net::TownOfPlayer(partner));
        auto refill = [&](const char* name, std::vector<std::string>& cache, const std::vector<towns::Good>& goods, void* keep) {
            auto items = Items(goods);
            if (items == cache) return;
            cache = items;
            gameui::ComboSet(E(name), items);
            int sel = 0;
            for (int i = 0; i < (int)goods.size(); i++) if (goods[i].desc == keep) sel = i;
            if (!items.empty()) gameui::ComboSelect(E(name), sel);
        };
        refill("bmpGive", giveItems, mine, giveDesc);
        refill("bmpAsk", askItems, theirs, askDesc);
        giveIdx = (std::max)(0, gameui::ComboGet(E("bmpGive")));
        askIdx = (std::max)(0, gameui::ComboGet(E("bmpAsk")));
        giveN = gameui::NumberGet(E("bmpGiveN"));
        askN = gameui::NumberGet(E("bmpAskN"));
        Check("bmpOnce", period == 0);
        Check("bmpSeason", period == 1);
        Check("bmpYear", period == 2);
        Text("bmpSend", period ? "Propose standing trade" : "Send offer");
        Enable("bmpSend", Valid());
        std::string hint = "Leave an amount at 0 to give or ask for nothing in return.";
        if (period) hint += std::string(" First delivery when accepted, then at the start of every ") + (period == 1 ? "season" : "year") +
                            " until one of you cancels.";
        Text("bmpTradeHint", hint);
        std::string news = net::TradeNews();
        Text("bmpNews", news);
        Vis("bmpNews", !news.empty());
        // standing trades
        agreeIds.clear();
        int me = towns::Local();
        for (auto& ag : towns::Agreements()) {
            if ((ag.a != me && ag.b != me) || agreeIds.size() >= 6) continue;
            bool iAmA = ag.a == me;
            auto amount = [](int n, void* d) { return d && n ? std::to_string(n) + " " + commands::DescName(d) : std::string("nothing"); };
            std::string line = "You give " + (iAmA ? amount(ag.nA, ag.descA) : amount(ag.nB, ag.descB)) + ", you get " +
                               (iAmA ? amount(ag.nB, ag.descB) : amount(ag.nA, ag.descA)) + " - every " + (ag.period == 1 ? "season" : "year");
            if (ag.lastOk == 0) line += "  (last time skipped: not enough goods on one side)";
            char n[24];
            sprintf_s(n, "bmpAgreeText%d", (int)agreeIds.size());
            Text(n, line);
            agreeIds.push_back(ag.id);
        }
        Vis("bmpStandingHeading", !agreeIds.empty());
        for (int i = 0; i < 6; i++) { char n[24]; sprintf_s(n, "bmpAgree%d", i); Vis(n, i < (int)agreeIds.size()); }
        // deliveries waiting on the ground
        std::string waiting;
        bool myStuck = false;
        for (auto& d : towns::Deliveries()) {
            if (d.from != me && d.to != me) continue;
            if (!waiting.empty()) waiting += ";   ";
            waiting += std::to_string(d.count) + " " + commands::DescName(d.desc) +
                       (d.from == me ? ": your villagers take it over" : ": on its way to you") + (d.stuck ? "  (no walking route)" : "");
            myStuck = myStuck || (d.stuck && d.from == me);
        }
        if (myStuck) waiting += ".   Your villagers can't walk to the other town (river or cliffs). Build a bridge, or hand the goods over directly.";
        Vis("bmpWaitHeading", !waiting.empty());
        Text("bmpWaiting", waiting);
        Vis("bmpWaiting", !waiting.empty());
        Vis("bmpDeliver", myStuck);
    }
    std::string GivePath() const { return mine.empty() || !giveN ? "" : commands::DescPath(mine[(std::min)(giveIdx, (int)mine.size() - 1)].desc); }
    std::string AskPath() const { return theirs.empty() || !askN ? "" : commands::DescPath(theirs[(std::min)(askIdx, (int)theirs.size() - 1)].desc); }
    bool Valid() const {
        std::string give = GivePath(), ask = AskPath();
        return (!give.empty() || !ask.empty()) && (give.empty() || period || giveN <= mine[(std::min)(giveIdx, (int)mine.size() - 1)].count);
    }
    void OnEvent(int id, int evt) override {
        if (evt != 0x12 && evt != 0x15) return;
        if (id >= kPartner && id < kPartner + 4 && id - kPartner < (int)partners.size()) partner = partners[id - kPartner];
        else if (id >= kCancel && id < kCancel + 6 && id - kCancel < (int)agreeIds.size()) commands::RequestEndAgreement(agreeIds[id - kCancel]);
        else if (id == kOnce) period = 0;
        else if (id == kSeason) period = 1;
        else if (id == kYear) period = 2;
        else if (id == kSend && Valid()) {
            std::string give = GivePath(), ask = AskPath();
            net::SendOffer(partner, give, give.empty() ? 0 : giveN, ask, ask.empty() ? 0 : askN, period);
        } else if (id == kDeliver) commands::RequestDeliverNow();
        else if (id == kClose) { g_tradeWanted = false; Close(); }
    }
    // destroyed: by the player (Escape, ...: same game UI -> stays closed) or by a session reload (new UI -> reopens)
    void* closedUi = nullptr;
    void Closed() override { closedUi = gamedlg::GameUi(); }
    int TitleButton() const override { return kClose; }
};

// ---------------------------------------------------------------- trade offers
struct Offer : gamedlg::Window {
    enum { kAccept = 3400, kDecline };
    int offerId = -1;
    net::UiInfo u;
    const char* Resource() const override { return "Dialog/BmpOffer.rsc"; }
    void Init() override {
        Id("bmpAccept", kAccept); Id("bmpDecline", kDecline);
        Text("bmpAccept", "Accept");
        Text("bmpDecline", "Decline");
    }
    void Refresh() override {
        auto offers = net::IncomingOffers();
        if (offers.empty()) { offerId = -1; return; }
        auto& o = offers[0];
        offerId = o.id;
        void* gd = o.give == "-" ? nullptr : commands::DescFromPath(o.give);
        void* ad = o.ask == "-" ? nullptr : commands::DescFromPath(o.ask);
        std::string gives = gd ? std::to_string(o.nGive) + " " + commands::DescName(gd) : "nothing";
        std::string wants = ad ? std::to_string(o.nAsk) + " " + commands::DescName(ad) : "nothing";
        Text("titleOffer", offers.size() > 1 ? "Trade offers (" + std::to_string(offers.size()) + ")" : "Trade offer");
        Text("bmpOfferText", NameOf(u, o.from) + " offers " + gives + " for " + wants +
                                 (o.period == 1 ? " - every season" : o.period == 2 ? " - every year" : ""));
        Text("bmpOfferHave", ad ? "(you have " + std::to_string(towns::CountOf(towns::Local(), ad)) + ")" : "");
    }
    void OnEvent(int id, int evt) override {
        if (evt != 0x12 || offerId < 0) return;
        if (id == kAccept) net::AnswerOffer(offerId, true);
        if (id == kDecline) net::AnswerOffer(offerId, false);
    }
};

// ---------------------------------------------------------------- choosing the start
struct Settle : gamedlg::Window {
    enum { kSettle = 3500 };
    DWORD sentAt = 0;
    int x = 0, y = 0;
    bool ok = false;
    const char* Resource() const override { return "Dialog/BmpSettle.rsc"; }
    void Init() override {
        Id("bmpSettle", kSettle);
        Text("titleSettle", "Choose your town's location");
        Text("bmpSettleText", "Move the camera until the marker is where your town should start.");
    }
    void Refresh() override {
        const char* why = "";
        ok = towns::CameraCell(&x, &y) && towns::CanSettle(towns::Local(), x, y, &why);
        Text("bmpSettleCell", "Map cell " + std::to_string(x) + ", " + std::to_string(y));
        Text("bmpSettleWhy", ok ? std::string("Good spot.") : "Can't settle here: " + std::string(why));
        gameui::SetColor(E("bmpSettleWhy"), ok ? kGood : kBad);
        bool waiting = sentAt && GetTickCount() - sentAt < 4000;
        Text("bmpSettle", waiting ? "Settling..." : "Settle here");
        Enable("bmpSettle", ok && !waiting);
    }
    void OnEvent(int id, int evt) override {
        if (evt == 0x12 && id == kSettle && ok && !(sentAt && GetTickCount() - sentAt < 4000)) {
            commands::RequestSettle(x, y);
            sentAt = GetTickCount();
        }
    }
};

static Bar g_bar;
static Trade g_trade;
static Offer g_offer;
static Settle g_settle;

// keep a window open while `want`, closed otherwise
static void Keep(gamedlg::Window& w, bool want, void* ui) {
    if (want && !w.IsOpen() && g_native && !w.Open(ui)) g_native = false;
    if (!want && w.IsOpen()) w.Close();
}

bool Frame(const net::UiInfo& u) {
    void* ui = gamedlg::GameUi();
    bool inGame = u.active && u.inGame && ui;
    g_bar.u = g_trade.u = g_offer.u = u;
    Keep(g_bar, inGame, ui);
    if (g_tradeWanted && !g_trade.IsOpen() && g_trade.closedUi && g_trade.closedUi == ui) g_tradeWanted = false;
    g_trade.closedUi = nullptr;
    Keep(g_trade, inGame && u.towns > 1 && g_tradeWanted, ui);
    Keep(g_offer, inGame && u.towns > 1 && !net::IncomingOffers().empty(), ui);
    Keep(g_settle, inGame && towns::PickMode() && !towns::Settled(towns::Local()), ui);
    return g_native;
}
bool Native() { return g_native; }
void ToggleTrade() { g_tradeWanted = !g_tradeWanted; }

}  // namespace ingame
