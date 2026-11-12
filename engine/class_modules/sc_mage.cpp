// ==========================================================================
// Dedmonwakeen's DPS-DPM Simulator.
// Send questions to natehieter@gmail.com
// ==========================================================================
//
// AULA-RETTELSE (simc-worker/patches/mage.patch i AULA-Guild):
// Grov basismodel for Forever-mage, bygget efter warlock-modulets skabelon.
// Kun spells der ikke afhaenger af talenter (rank efter spillerens level),
// ingen talent-effekter, ingen Evocation. Alle tal kommer fra spildata.
// Fjernes naar upstream selv implementerer klassen.

#include "simulationcraft.hpp"
#include "action/parse_effects.hpp"

#include "class_modules/apl/mage.hpp"

namespace mage
{
struct mage_t final : public player_t
{
  mage_t( sim_t* sim, std::string_view name, race_e r = RACE_NONE ) :
    player_t( sim, MAGE, name, r ) {}

  resource_e primary_resource() const override { return RESOURCE_MANA; }

  std::string default_potion() const override { return mage_apl::potion( this ); }
  std::string default_flask() const override  { return mage_apl::flask( this ); }
  std::string default_food() const override   { return mage_apl::food( this ); }

  action_t* create_action( std::string_view name, std::string_view options_str ) override;
  void init_action_list() override;
  void init_base_stats() override;
  void init_spells() override;

  struct
  {
    spell_ranks_t arcane_missiles;
    spell_ranks_t fire_blast;
    spell_ranks_t fireball;
    spell_ranks_t frostbolt;
  } rank;
};

// MAGE MODULE INTERFACE ====================================================

struct mage_module_t final : public module_t
{
public:
  mage_module_t() :
    module_t( MAGE )
  { }

  player_t* create_player( sim_t* sim, std::string_view name, race_e r = RACE_NONE ) const override
  {
    auto p = new mage_t( sim, name, r );
    return p;
  }

  void register_hotfixes() const override {}

  bool valid() const override { return true; }

  void register_actor_initializers( sim_t* ) const override {}
};

template <class Base>
struct mage_action_t : public parse_action_effects_t<Base>
{
private:
  using ab = parse_action_effects_t<Base>;

public:
  using base_t = mage_action_t<Base>;

  template <typename... Args>
  mage_action_t( Args&&... args ) : ab( std::forward<Args>( args )... ) {}

  mage_t* p()
  { return debug_cast<mage_t*>( this->player ); }

  const mage_t* p() const
  { return debug_cast<const mage_t*>( this->player ); }
};

using mage_spell_t = mage_action_t<spell_t>;

// Frostbolt
struct frostbolt_t : public mage_spell_t
{
  frostbolt_t( mage_t* player, std::string_view options_str ) :
    mage_spell_t( "frostbolt", player, player->rank.frostbolt.find_rank()->spell() )
  {
    parse_options( options_str );
  }
};

// Fireball (direkte skade + DoT, begge fra spildata)
struct fireball_t : public mage_spell_t
{
  fireball_t( mage_t* player, std::string_view options_str ) :
    mage_spell_t( "fireball", player, player->rank.fireball.find_rank()->spell() )
  {
    parse_options( options_str );
  }
};

// Fire Blast (instant; nedkoeling fra spildata)
struct fire_blast_t : public mage_spell_t
{
  fire_blast_t( mage_t* player, std::string_view options_str ) :
    mage_spell_t( "fire_blast", player, player->rank.fire_blast.find_rank()->spell() )
  {
    parse_options( options_str );
  }
};

// Arcane Missiles: kanaliseret driver; hvert tick udloeser et Arcane Missile (spildataens trigger-spell)
struct arcane_missile_t : public mage_spell_t
{
  arcane_missile_t( mage_t* player ) :
    mage_spell_t( "arcane_missile", player,
                  player->rank.arcane_missiles.find_rank( RANK_MAX_USABLE, &spell_container_t::attack )->attack() )
  {
    background = dual = true;
  }
};

struct arcane_missiles_t : public mage_spell_t
{
  arcane_missiles_t( mage_t* player, std::string_view options_str ) :
    mage_spell_t( "arcane_missiles", player,
                  player->rank.arcane_missiles.find_rank( RANK_MAX_USABLE, &spell_container_t::attack )->spell() )
  {
    parse_options( options_str );
    channeled = true;
    may_miss = may_crit = false;
    tick_action = new arcane_missile_t( player );
  }
};

action_t* mage_t::create_action( std::string_view name, std::string_view options_str )
{
  if ( name == "frostbolt" )
    return new frostbolt_t( this, options_str );
  if ( name == "fireball" )
    return new fireball_t( this, options_str );
  if ( name == "fire_blast" )
    return new fire_blast_t( this, options_str );
  if ( name == "arcane_missiles" )
    return new arcane_missiles_t( this, options_str );

  return player_t::create_action( name, options_str );
}

void mage_t::init_base_stats()
{
  // Ingen egne konstanter: basis-crit m.m. kommer fra spildata via player_t.
  player_t::init_base_stats();
}

void mage_t::init_spells()
{
  player_t::init_spells();

  // spell = kanaliseret driver, attack = Arcane Missile (udloest pr. tick); kommentar = spell level
  rank.arcane_missiles = { this,
    spell_container_t().set_spell(  5143 ).set_attack(  7268 ),  // 8
    spell_container_t().set_spell(  5144 ).set_attack(  7269 ),  // 16
    spell_container_t().set_spell(  5145 ).set_attack(  7270 ),  // 24
    spell_container_t().set_spell(  8416 ).set_attack(  8419 ),  // 32
    spell_container_t().set_spell(  8417 ).set_attack(  8418 ),  // 40
    spell_container_t().set_spell( 10211 ).set_attack( 10273 ),  // 48
    spell_container_t().set_spell( 10212 ).set_attack( 10274 ),  // 56
    spell_container_t().set_spell( 25345 ).set_attack( 25346 ),  // 56 / 60
  };

  rank.fire_blast = { this,
    spell_container_t().set_spell(  2136 ),  // 6
    spell_container_t().set_spell(  2137 ),  // 14
    spell_container_t().set_spell(  2138 ),  // 22
    spell_container_t().set_spell(  8412 ),  // 30
    spell_container_t().set_spell(  8413 ),  // 38
    spell_container_t().set_spell( 10197 ),  // 46
    spell_container_t().set_spell( 10199 ),  // 54
  };

  rank.fireball = { this,
    spell_container_t().set_spell(   133 ),  // 1
    spell_container_t().set_spell(   143 ),  // 6
    spell_container_t().set_spell(   145 ),  // 12
    spell_container_t().set_spell(  3140 ),  // 18
    spell_container_t().set_spell(  8400 ),  // 24
    spell_container_t().set_spell(  8401 ),  // 30
    spell_container_t().set_spell(  8402 ),  // 36
    spell_container_t().set_spell( 10148 ),  // 42
    spell_container_t().set_spell( 10149 ),  // 48
    spell_container_t().set_spell( 10150 ),  // 54
    spell_container_t().set_spell( 10151 ),  // 60
    spell_container_t().set_spell( 25306 ),  // 60
  };

  rank.frostbolt = { this,
    spell_container_t().set_spell(   116 ),  // 4
    spell_container_t().set_spell(   205 ),  // 8
    spell_container_t().set_spell(   837 ),  // 14
    spell_container_t().set_spell(  7322 ),  // 20
    spell_container_t().set_spell(  8406 ),  // 26
    spell_container_t().set_spell(  8407 ),  // 32
    spell_container_t().set_spell(  8408 ),  // 38
    spell_container_t().set_spell( 10179 ),  // 44
    spell_container_t().set_spell( 10180 ),  // 50
    spell_container_t().set_spell( 10181 ),  // 56
    spell_container_t().set_spell( 25304 ),  // 60
  };

  // Kun klassens passiver; talenter laeses bevidst ikke (grov model).
  parse_all_class_passives();
}

void mage_t::init_action_list()
{
  if ( !action_list_str.empty() )
  {
    player_t::init_action_list();
    return;
  }

  clear_action_priority_lists();
  mage_apl::mage( this );
  use_default_action_list = true;
  player_t::init_action_list();
}

}  // namespace mage

const module_t* module_t::mage()
{
  static mage::mage_module_t m;
  return &m;
}
