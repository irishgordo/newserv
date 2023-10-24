#include "Client.hh"

#include <errno.h>
#include <event2/buffer.h>
#include <event2/bufferevent.h>
#include <event2/event.h>
#include <string.h>
#include <unistd.h>

#include <atomic>
#include <phosg/Network.hh>
#include <phosg/Time.hh>

#include "Loggers.hh"
#include "Server.hh"
#include "Version.hh"

using namespace std;

const uint64_t CLIENT_CONFIG_MAGIC = 0x492A890E82AC9839;

static atomic<uint64_t> next_id(1);

ClientOptions::ClientOptions()
    : switch_assist(false),
      infinite_hp(false),
      infinite_tp(false),
      debug(false),
      override_section_id(-1),
      override_lobby_event(-1),
      override_lobby_number(-1),
      override_random_seed(-1),
      save_files(false),
      enable_chat_commands(true),
      enable_chat_filter(true),
      enable_player_notifications(false),
      suppress_client_pings(false),
      suppress_remote_login(false),
      zero_remote_guild_card(false),
      ep3_infinite_meseta(false),
      ep3_infinite_time(false),
      red_name(false),
      blank_name(false),
      function_call_return_value(-1) {}

Client::Client(
    shared_ptr<Server> server,
    struct bufferevent* bev,
    GameVersion version,
    ServerBehavior server_behavior)
    : server(server),
      id(next_id++),
      log(string_printf("[C-%" PRIX64 "] ", this->id), client_log.min_level),
      bb_game_state(0),
      flags(flags_for_version(version, -1)),
      specific_version(default_specific_version_for_version(version, -1)),
      channel(bev, version, 1, nullptr, nullptr, this, string_printf("C-%" PRIX64, this->id), TerminalFormat::FG_YELLOW, TerminalFormat::FG_GREEN),
      server_behavior(server_behavior),
      should_disconnect(false),
      should_send_to_lobby_server(false),
      should_send_to_proxy_server(false),
      proxy_destination_address(0),
      proxy_destination_port(0),
      x(0.0f),
      z(0.0f),
      area(0),
      lobby_client_id(0),
      lobby_arrow_color(0),
      preferred_lobby_id(-1),
      save_game_data_event(
          event_new(
              bufferevent_get_base(bev), -1, EV_TIMEOUT | EV_PERSIST,
              &Client::dispatch_save_game_data, this),
          event_free),
      send_ping_event(
          event_new(
              bufferevent_get_base(bev), -1, EV_TIMEOUT,
              &Client::dispatch_send_ping, this),
          event_free),
      idle_timeout_event(
          event_new(
              bufferevent_get_base(bev), -1, EV_TIMEOUT,
              &Client::dispatch_idle_timeout, this),
          event_free),
      card_battle_table_number(-1),
      card_battle_table_seat_number(0),
      card_battle_table_seat_state(0),
      next_exp_value(0),
      can_chat(true),
      use_server_rare_tables(false),
      pending_bb_save_player_index(0),
      dol_base_addr(0) {
  this->last_switch_enabled_command.header.subcommand = 0;
  memset(&this->next_connection_addr, 0, sizeof(this->next_connection_addr));

  if (this->version() == GameVersion::BB) {
    struct timeval tv = usecs_to_timeval(60000000); // 1 minute
    event_add(this->save_game_data_event.get(), &tv);
  }
  this->reschedule_ping_and_timeout_events();

  this->log.info("Created");
}

Client::~Client() {
  if (!this->disconnect_hooks.empty()) {
    this->log.warning("Disconnect hooks pending at client destruction time:");
    for (const auto& it : this->disconnect_hooks) {
      this->log.warning("  %s", it.first.c_str());
    }
  }

  this->log.info("Deleted");
}

void Client::reschedule_ping_and_timeout_events() {
  struct timeval ping_tv = usecs_to_timeval(30000000); // 30 seconds
  event_add(this->send_ping_event.get(), &ping_tv);
  struct timeval idle_tv = usecs_to_timeval(60000000); // 1 minute
  event_add(this->idle_timeout_event.get(), &idle_tv);
}

QuestScriptVersion Client::quest_version() const {
  switch (this->version()) {
    case GameVersion::DC:
      if (this->flags & Flag::IS_DC_TRIAL_EDITION) {
        return QuestScriptVersion::DC_NTE;
      } else if (this->flags & Flag::IS_DC_V1) {
        return QuestScriptVersion::DC_V1;
      } else {
        return QuestScriptVersion::DC_V2;
      }
    case GameVersion::PC:
      return QuestScriptVersion::PC_V2;
    case GameVersion::GC:
      if (this->flags & Flag::IS_GC_TRIAL_EDITION) {
        return QuestScriptVersion::GC_NTE;
      } else if (this->flags & Flag::IS_EPISODE_3) {
        return QuestScriptVersion::GC_EP3;
      } else {
        return QuestScriptVersion::GC_V3;
      }
    case GameVersion::XB:
      return QuestScriptVersion::XB_V3;
    case GameVersion::BB:
      return QuestScriptVersion::BB_V4;
    default:
      throw logic_error("client\'s game version does not have a quest version");
  }
}

void Client::set_license(shared_ptr<License> l) {
  this->license = l;
  this->game_data.guild_card_number = this->license->serial_number;
  if (this->version() == GameVersion::BB) {
    this->game_data.bb_username = this->license->bb_username;
  }
}

shared_ptr<ServerState> Client::require_server_state() const {
  auto server = this->server.lock();
  if (!server) {
    throw logic_error("server is deleted");
  }
  return server->get_state();
}

shared_ptr<Lobby> Client::require_lobby() const {
  auto l = this->lobby.lock();
  if (!l) {
    throw runtime_error("client not in any lobby");
  }
  return l;
}

ClientConfig Client::export_config() const {
  ClientConfig cc;
  cc.magic = CLIENT_CONFIG_MAGIC;
  cc.flags = this->flags;
  cc.specific_version = this->specific_version;
  cc.proxy_destination_address = this->proxy_destination_address;
  cc.proxy_destination_port = this->proxy_destination_port;
  cc.unused.clear(0xFF);
  return cc;
}

ClientConfigBB Client::export_config_bb() const {
  ClientConfigBB cc;
  cc.cfg = this->export_config();
  cc.bb_game_state = this->bb_game_state;
  cc.bb_player_index = this->game_data.bb_player_index;
  cc.unused.clear(0xFF);
  return cc;
}

void Client::import_config(const ClientConfig& cc) {
  if (cc.magic != CLIENT_CONFIG_MAGIC) {
    throw invalid_argument("invalid client config");
  }
  this->flags = cc.flags;
  this->specific_version = cc.specific_version;
  this->proxy_destination_address = cc.proxy_destination_address;
  this->proxy_destination_port = cc.proxy_destination_port;
}

void Client::import_config(const ClientConfigBB& cc) {
  this->import_config(cc.cfg);
  this->bb_game_state = cc.bb_game_state;
  this->game_data.bb_player_index = cc.bb_player_index;
}

void Client::dispatch_save_game_data(evutil_socket_t, short, void* ctx) {
  reinterpret_cast<Client*>(ctx)->save_game_data();
}

void Client::save_game_data() {
  if (this->version() != GameVersion::BB) {
    throw logic_error("save_game_data called for non-BB client");
  }
  if (this->game_data.account(false)) {
    this->game_data.save_account_data();
  }
  if (this->game_data.player(false)) {
    this->game_data.save_player_data();
  }
}

void Client::dispatch_send_ping(evutil_socket_t, short, void* ctx) {
  reinterpret_cast<Client*>(ctx)->send_ping();
}

void Client::send_ping() {
  if (this->version() != GameVersion::PATCH) {
    this->log.info("Sending ping command");
    // The game doesn't use this timestamp; we only use it for debugging purposes
    be_uint64_t timestamp = now();
    try {
      this->channel.send(0x1D, 0x00, &timestamp, sizeof(be_uint64_t));
    } catch (const exception& e) {
      this->log.info("Failed to send ping: %s", e.what());
    }
  }
}

void Client::dispatch_idle_timeout(evutil_socket_t, short, void* ctx) {
  reinterpret_cast<Client*>(ctx)->idle_timeout();
}

void Client::idle_timeout() {
  this->log.info("Idle timeout expired");
  auto s = this->server.lock();
  if (s) {
    auto c = this->shared_from_this();
    s->disconnect_client(c);
  } else {
    this->log.info("Server is deleted; cannot disconnect client");
  }
}
