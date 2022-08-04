#include "Map.hh"

#include <phosg/Filesystem.hh>
#include <phosg/Strings.hh>

#include "Loggers.hh"
#include "FileContentsCache.hh"

using namespace std;



BattleParamsIndex::BattleParamsIndex(const char* prefix) {
  for (uint8_t is_solo = 0; is_solo < 2; is_solo++) {
    for (uint8_t episode = 0; episode < 3; episode++) {
      string filename = prefix;
      if (episode == 1) {
        filename += "_lab";
      } else if (episode == 2) {
        filename += "_ep4";
      }
      if (!is_solo) {
        filename += "_on";
      }
      filename += ".dat";

      this->entries[is_solo][episode][0].reset(new TableT());
      this->entries[is_solo][episode][1].reset(new TableT());
      this->entries[is_solo][episode][2].reset(new TableT());
      this->entries[is_solo][episode][3].reset(new TableT());

      scoped_fd fd(filename, O_RDONLY);
      readx(fd, this->entries[is_solo][episode][0].get(), sizeof(TableT));
      readx(fd, this->entries[is_solo][episode][1].get(), sizeof(TableT));
      readx(fd, this->entries[is_solo][episode][2].get(), sizeof(TableT));
      readx(fd, this->entries[is_solo][episode][3].get(), sizeof(TableT));
    }
  }
}

const BattleParams& BattleParamsIndex::get(bool solo, uint8_t episode,
    uint8_t difficulty, uint8_t monster_type) const {
  if (episode > 3) {
    throw invalid_argument("incorrect episode");
  }
  if (difficulty > 4) {
    throw invalid_argument("incorrect difficulty");
  }
  if (monster_type > 0x60) {
    throw invalid_argument("incorrect monster type");
  }
  return (*this->entries[!!solo][episode][difficulty])[monster_type];
}

shared_ptr<const BattleParamsIndex::TableT>
BattleParamsIndex::get_subtable(
    bool solo, uint8_t episode, uint8_t difficulty) const {
  if (episode > 3) {
    throw invalid_argument("incorrect episode");
  }
  if (difficulty > 4) {
    throw invalid_argument("incorrect difficulty");
  }
  return this->entries[!!solo][episode][difficulty];
}



PSOEnemy::PSOEnemy(uint64_t id) : PSOEnemy(id, 0, 0, 0) { }

PSOEnemy::PSOEnemy(
    uint64_t id,
    uint16_t source_type,
    uint32_t experience,
    uint32_t rt_index)
  : id(id),
    source_type(source_type),
    hit_flags(0),
    last_hit(0),
    experience(experience),
    rt_index(rt_index) { }

string PSOEnemy::str() const {
  return string_printf("[Enemy E-%" PRIX64 " source_type=%hX hit=%02hhX/%hu exp=%" PRIu32 " rt_index=%" PRIX32 "]",
      this->id, this->source_type, this->hit_flags, this->last_hit, this->experience, this->rt_index);
}



struct EnemyEntry {
  uint32_t base;
  uint16_t reserved0;
  uint16_t num_clones;
  uint32_t reserved[11];
  float reserved12;
  uint32_t reserved13;
  uint32_t reserved14;
  uint32_t skin;
  uint32_t reserved15;
} __attribute__((packed));

static uint64_t next_enemy_id = 1;

vector<PSOEnemy> parse_map(
    uint8_t episode,
    uint8_t difficulty,
    shared_ptr<const BattleParamsIndex::TableT> battle_params_table,
    const void* data,
    size_t size,
    bool alt_enemies) {

  const auto* map = reinterpret_cast<const EnemyEntry*>(data);
  size_t entry_count = size / sizeof(EnemyEntry);
  if (size != entry_count * sizeof(EnemyEntry)) {
    throw runtime_error("data size is not a multiple of entry size");
  }

  vector<PSOEnemy> enemies;
  auto create_clones = [&](size_t count) {
    for (; count > 0; count--) {
      enemies.emplace_back(next_enemy_id++);
    }
  };

  const auto& battle_params = *battle_params_table;
  for (size_t y = 0; y < entry_count; y++) {
    const auto& e = map[y];
    size_t num_clones = e.num_clones;

    switch (e.base) {
      case 0x40: // Hildebear and Hildetorr
        enemies.emplace_back(next_enemy_id++, e.base,
            battle_params[0x49 + (e.skin & 0x01)].experience,
            0x01 + (e.skin & 0x01));
        break;
      case 0x41: // Rappies
        if (episode == 3) { // Del Rappy and Sand Rappy
          if (alt_enemies) {
            enemies.emplace_back(next_enemy_id++, e.base,
                battle_params[0x17 + (e.skin & 0x01)].experience,
                17 + (e.skin & 0x01));
          } else {
            enemies.emplace_back(next_enemy_id++, e.base,
                battle_params[0x05 + (e.skin & 0x01)].experience,
                17 + (e.skin & 0x01));
          }
        } else { // Rag Rappy and Al Rappy (Love for Episode II)
          if (e.skin & 0x01) {
            enemies.emplace_back(next_enemy_id++, e.base,
                battle_params[0x18 + (e.skin & 0x01)].experience,
                0xFF); // Don't know (yet) which rare Rappy it is
          } else {
            enemies.emplace_back(next_enemy_id++, e.base,
                battle_params[0x18 + (e.skin & 0x01)].experience,
                5);
          }
        }
        break;
      case 0x42: // Monest + 30 Mothmants
        enemies.emplace_back(next_enemy_id++, e.base, battle_params[0x01].experience, 4);
        for (size_t x = 0; x < 30; x++) {
          enemies.emplace_back(next_enemy_id++, e.base, battle_params[0x00].experience, 3);
        }
        break;
      case 0x43: // Savage Wolf and Barbarous Wolf
        enemies.emplace_back(next_enemy_id++, e.base,
            battle_params[0x02 + ((e.reserved[10] & 0x800000) ? 1 : 0)].experience,
            7 + ((e.reserved[10] & 0x800000) ? 1 : 0));
        break;
      case 0x44: // Booma family
        enemies.emplace_back(next_enemy_id++, e.base,
            battle_params[0x4B + (e.skin % 3)].experience,
            9 + (e.skin % 3));
        break;
      case 0x60: // Grass Assassin
        enemies.emplace_back(next_enemy_id++, e.base, battle_params[0x4E].experience, 12);
        break;
      case 0x61: // Del Lily, Poison Lily, Nar Lily
        if ((episode == 2) && (alt_enemies)) {
          enemies.emplace_back(next_enemy_id++, e.base, battle_params[0x25].experience, 83);
        } else {
          enemies.emplace_back(next_enemy_id++, e.base,
              battle_params[0x04 + ((e.reserved[10] & 0x800000) ? 1 : 0)].experience,
              13 + ((e.reserved[10] & 0x800000) ? 1 : 0));
        }
        break;
      case 0x62: // Nano Dragon
        enemies.emplace_back(next_enemy_id++, e.base, battle_params[0x1A].experience, 15);
        break;
      case 0x63: // Shark family
        enemies.emplace_back(next_enemy_id++, e.base,
            battle_params[0x4F + (e.skin % 3)].experience,
            16 + (e.skin % 3));
        break;
      case 0x64: // Slime + 4 clones
        enemies.emplace_back(next_enemy_id++, e.base,
            battle_params[0x2F + ((e.reserved[10] & 0x800000) ? 0 : 1)].experience,
            19 + ((e.reserved[10] & 0x800000) ? 1 : 0));
        for (size_t x = 0; x < 4; x++) {
          enemies.emplace_back(next_enemy_id++, e.base, battle_params[0x30].experience, 19);
        }
        break;
      case 0x65: // Pan Arms, Migium, Hidoom
        for (size_t x = 0; x < 3; x++) {
          enemies.emplace_back(next_enemy_id++, e.base, battle_params[0x31 + x].experience, 21 + x);
        }
        break;
      case 0x80: // Dubchic and Gillchic
        if (e.skin & 0x01) {
          enemies.emplace_back(next_enemy_id++, e.base,
              battle_params[0x1B + (e.skin & 0x01)].experience, 50);
        } else {
          enemies.emplace_back(next_enemy_id++, e.base,
              battle_params[0x1B + (e.skin & 0x01)].experience, 24);
        }
        break;
      case 0x81: // Garanz
        enemies.emplace_back(next_enemy_id++, e.base, battle_params[0x1D].experience, 25);
        break;
      case 0x82: // Sinow Beat and Gold
        if (e.reserved[10] & 0x800000) {
          enemies.emplace_back(next_enemy_id++, e.base,
              battle_params[0x13].experience,
              26 + ((e.reserved[10] & 0x800000) ? 1 : 0));
        } else {
          enemies.emplace_back(next_enemy_id++, e.base,
              battle_params[0x06].experience,
              26 + ((e.reserved[10] & 0x800000) ? 1 : 0));
        }
        if (e.num_clones == 0) {
          create_clones(4);
        }
        break;
      case 0x83: // Canadine
        enemies.emplace_back(next_enemy_id++, e.base, battle_params[0x07].experience, 28);
        break;
      case 0x84: // Canadine Group
        enemies.emplace_back(next_enemy_id++, e.base, battle_params[0x09].experience, 29);
        for (size_t x = 0; x < 8; x++) {
          enemies.emplace_back(next_enemy_id++, e.base, battle_params[0x08].experience, 28);
        }
        break;
      case 0x85: // Dubwitch
        break;
      case 0xA0: // Delsaber
        enemies.emplace_back(next_enemy_id++, e.base, battle_params[0x52].experience, 30);
        break;
      case 0xA1: // Chaos Sorcerer + 2 Bits
        enemies.emplace_back(next_enemy_id++, e.base, battle_params[0x0A].experience, 31);
        create_clones(2);
        break;
      case 0xA2: // Dark Gunner
        enemies.emplace_back(next_enemy_id++, e.base, battle_params[0x1E].experience, 34);
        break;
      case 0xA4: // Chaos Bringer
        enemies.emplace_back(next_enemy_id++, e.base, battle_params[0x0D].experience, 36);
        break;
      case 0xA5: // Dark Belra
        enemies.emplace_back(next_enemy_id++, e.base, battle_params[0x0E].experience, 37);
        break;
      case 0xA6: // Dimenian family
        enemies.emplace_back(next_enemy_id++, e.base,
            battle_params[0x53 + (e.skin % 3)].experience, 41 + (e.skin % 3));
        break;
      case 0xA7: // Bulclaw + 4 claws
        enemies.emplace_back(next_enemy_id++, e.base, battle_params[0x1F].experience, 40);
        for (size_t x = 0; x < 4; x++) {
          enemies.emplace_back(next_enemy_id++, e.base, battle_params[0x20].experience, 38);
        }
        break;
      case 0xA8: // Claw
        enemies.emplace_back(next_enemy_id++, e.base, battle_params[0x20].experience, 38);
        break;
      case 0xC0: // Dragon or Gal Gryphon
        if (episode == 1) {
          enemies.emplace_back(next_enemy_id++, e.base, battle_params[0x12].experience, 44);
        } else if (episode == 2) {
          enemies.emplace_back(next_enemy_id++, e.base, battle_params[0x1E].experience, 77);
        }
        break;
      case 0xC1: // De Rol Le
        enemies.emplace_back(next_enemy_id++, e.base, battle_params[0x0F].experience, 45);
        break;
      case 0xC2: // Vol Opt form 1
        break;
      case 0xC5: // Vol Opt form 2
        enemies.emplace_back(next_enemy_id++, e.base, battle_params[0x25].experience, 46);
        break;
      case 0xC8: // Dark Falz + 510 Helpers
        if (difficulty) {
          enemies.emplace_back(next_enemy_id++, e.base, battle_params[0x38].experience, 47); // Final form
        } else {
          enemies.emplace_back(next_enemy_id++, e.base, battle_params[0x37].experience, 47); // Second form
        }
        for (size_t x = 0; x < 510; x++) {
          enemies.emplace_back(next_enemy_id++, e.base, battle_params[0x35].experience, 0);
        }
        break;
      case 0xCA: // Olga Flow
        enemies.emplace_back(next_enemy_id++, e.base, battle_params[0x2C].experience, 78);
        create_clones(0x200);
        break;
      case 0xCB: // Barba Ray
        enemies.emplace_back(next_enemy_id++, e.base, battle_params[0x0F].experience, 73);
        create_clones(0x2F);
        break;
      case 0xCC: // Gol Dragon
        enemies.emplace_back(next_enemy_id++, e.base, battle_params[0x12].experience, 76);
        create_clones(5);
        break;
      case 0xD4: // Sinows Berill & Spigell
        enemies.emplace_back(next_enemy_id++, e.base,
            battle_params[(e.reserved[10] & 0x800000) ? 0x13 : 0x06].experience,
            62 + ((e.reserved[10] & 0x800000) ? 1 : 0));
        create_clones(4);
        break;
      case 0xD5: // Merillia & Meriltas
        enemies.emplace_back(next_enemy_id++, e.base,
            battle_params[0x4B + (e.skin & 0x01)].experience,
            52 + (e.skin & 0x01));
        break;
      case 0xD6: // Mericus, Merikle, & Mericarol
        if (e.skin) {
          enemies.emplace_back(next_enemy_id++, e.base,
              battle_params[0x44 + (e.skin % 3)].experience, 56 + (e.skin % 3));
        } else {
          enemies.emplace_back(next_enemy_id++, e.base,
              battle_params[0x3A].experience, 56 + (e.skin % 3));
        }
        break;
      case 0xD7: // Ul Gibbon and Zol Gibbon
        enemies.emplace_back(next_enemy_id++, e.base,
              battle_params[0x3B + (e.skin & 0x01)].experience,
              59 + (e.skin & 0x01));
        break;
      case 0xD8: // Gibbles
        enemies.emplace_back(next_enemy_id++, e.base, battle_params[0x3D].experience, 61);
        break;
      case 0xD9: // Gee
        enemies.emplace_back(next_enemy_id++, e.base, battle_params[0x07].experience, 54);
        break;
      case 0xDA: // Gi Gue
        enemies.emplace_back(next_enemy_id++, e.base, battle_params[0x1A].experience, 55);
        break;
      case 0xDB: // Deldepth
        enemies.emplace_back(next_enemy_id++, e.base, battle_params[0x30].experience, 71);
        break;
      case 0xDC: // Delbiter
        enemies.emplace_back(next_enemy_id++, e.base, battle_params[0x0D].experience, 72);
        break;
      case 0xDD: // Dolmolm and Dolmdarl
        enemies.emplace_back(next_enemy_id++, e.base,
            battle_params[0x4F + (e.skin & 0x01)].experience,
            64 + (e.skin & 0x01));
        break;
      case 0xDE: // Morfos
        enemies.emplace_back(next_enemy_id++, e.base, battle_params[0x40].experience, 66);
        break;
      case 0xDF: // Recobox & Recons
        enemies.emplace_back(next_enemy_id++, e.base, battle_params[0x41].experience, 67);
        for (size_t x = 0; x < e.num_clones; x++) {
          enemies.emplace_back(next_enemy_id++, e.base, battle_params[0x42].experience, 68);
        }
        break;
      case 0xE0: // Epsilon, Sinow Zoa and Zele
        if ((episode == 2) && (alt_enemies)) {
          enemies.emplace_back(next_enemy_id++, e.base, battle_params[0x23].experience, 84);
          create_clones(4);
        } else {
          enemies.emplace_back(next_enemy_id++, e.base,
              battle_params[0x43 + (e.skin & 0x01)].experience,
              69 + (e.skin & 0x01));
        }
        break;
      case 0xE1: // Ill Gill
        enemies.emplace_back(next_enemy_id++, e.base, battle_params[0x26].experience, 82);
        break;
      case 0x0110: // Astark
        enemies.emplace_back(next_enemy_id++, e.base, battle_params[0x09].experience, 1);
        break;
      case 0x0111: // Satellite Lizard and Yowie
        enemies.emplace_back(next_enemy_id++, e.base,
            battle_params[0x0D + ((e.reserved[10] & 0x800000) ? 1 : 0) + (alt_enemies ? 0x10 : 0)].experience,
            2 + ((e.reserved[10] & 0x800000) ? 0 : 1));
        break;
      case 0x0112: // Merissa A/AA
        enemies.emplace_back(next_enemy_id++, e.base,
            battle_params[0x19 + (e.skin & 0x01)].experience,
            4 + (e.skin & 0x01));
        break;
      case 0x0113: // Girtablulu
        enemies.emplace_back(next_enemy_id++, e.base, battle_params[0x1F].experience, 6);
        break;
      case 0x0114: // Zu and Pazuzu
        enemies.emplace_back(next_enemy_id++, e.base,
            battle_params[0x0B + (e.skin & 0x01) + (alt_enemies ? 0x14: 0x00)].experience,
            7 + (e.skin & 0x01));
        break;
      case 0x0115: // Boota family
        if (e.skin & 2) {
          enemies.emplace_back(next_enemy_id++, e.base,
              battle_params[0x03].experience, 9 + (e.skin % 3));
        } else {
          enemies.emplace_back(next_enemy_id++, e.base,
              battle_params[0x00 + (e.skin % 3)].experience,
              9 + (e.skin % 3));
        }
        break;
      case 0x0116: // Dorphon and Eclair
        enemies.emplace_back(next_enemy_id++, e.base,
            battle_params[0x0F + (e.skin & 0x01)].experience,
            12 + (e.skin & 0x01));
        break;
      case 0x0117: // Goran family
        enemies.emplace_back(next_enemy_id++, e.base,
            battle_params[0x11 + (e.skin % 3)].experience,
            (e.skin & 2) ? 15 : ((e.skin & 1) ? 16 : 14));
        break;
      case 0x0119: // Saint Million, Shambertin, Kondrieu
        enemies.emplace_back(next_enemy_id++, e.base,
            battle_params[0x22].experience,
            (e.reserved[10] & 0x800000) ? 21 : (19 + (e.skin & 0x01)));
        break;
      default:
        enemies.emplace_back(next_enemy_id++, e.base, 0xFFFFFFFF, 0);
        static_game_data_log.warning(
            "(Entry %zu, offset %zX in file) Unknown enemy type %08" PRIX32 " %08" PRIX32,
            y, y * sizeof(EnemyEntry), e.base, e.skin);
        break;
    }
    create_clones(num_clones);
  }

  return enemies;
}
