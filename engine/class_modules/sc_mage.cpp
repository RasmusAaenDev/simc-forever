// ==========================================================================
// Dedmonwakeen's DPS-DPM Simulator.
// Send questions to natehieter@gmail.com
// ==========================================================================
//
// AULA-RETTELSE (simc-worker/patches/mage.patch i AULA-Guild):
// Grov basismodel for Forever-mage, bygget efter warlock-modulets skabelon.
// Kun spells der ikke afhaenger af talenter (rank efter spillerens level).
// Talenternes passive effekter laeses fra spildata (parse_all_passive_talents);
// talent-spells, procs og frossen-tilstand er ikke med. Wand (Shoot) og
// Evocation til mana-styring.
// Alle tal kommer fra spildata og vaabendata.
// Fjernes naar upstream selv implementerer klassen.

#include "simulationcraft.hpp"
#include "action/parse_effects.hpp"

#include "class_modules/apl/mage.hpp"

namespace mage
{
struct mage_t final : public parse_player_effects_t
{
  mage_t( sim_t* sim, std::string_view name, race_e r = RACE_NONE ) :
    parse_player_effects_t( sim, MAGE, name, r ) {}

  resource_e primary_resource() const override { return RESOURCE_MANA; }

  std::string default_potion() const override { return mage_apl::potion( this ); }
  std::string default_flask() const override  { return mage_apl::flask( this ); }
  std::string default_food() const override   { return mage_apl::food( this ); }

  action_t* create_action( std::string_view name, std::string_view options_str ) override;
  void init_action_list() override;
  void init_base_stats() override;
  void init_spells() override;
  void create_buffs() override;
  void init_finished() override;

  struct
  {
    buff_t* evocation;
  } buff;

  struct
  {
    player_talent_t wand_specialization;
  } talent;

  struct
  {
    spell_ranks_t arcane_missiles;
    spell_ranks_t evocation;
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

// Shoot (wand, 5019): et skud med den udstyrede wand i ranged-pladsen. Spildataen siger
// Weapon Damage (NoSchool) og Spell Type Magic: skaden er wandens vaabenskade, skolen kommer
// fra wanden (staar ikke i simcs item-data), og skuddet bruger spell-tabellen (hit/crit).
// Hvert skud modelleres som en "cast" af wandens hastighed (auto-repeat afbrydes i spillet af
// andre spells), saa APL'en kan vende tilbage til Frostbolt naar manaen er der igen.
// Uden wand i ranged-pladsen er handlingen aldrig klar.
struct shoot_t : public mage_spell_t
{
  shoot_t( mage_t* player, std::string_view options_str ) :
    mage_spell_t( "shoot", player, player->find_spell( 5019 ) )
  {
    parse_options( options_str );

    weapon            = &player->equipped_weapons[ SLOT_RANGED ];
    weapon_multiplier = 1.0;
    base_execute_time = weapon->swing_time;
    may_crit          = true;
    // Wandens egen skole, hvis profilen angiver den (weapon=wand_..._shadow); ellers spildataens.
    if ( weapon->school != SCHOOL_PHYSICAL && weapon->school != SCHOOL_NONE )
      school = weapon->school;
  }

  bool has_wand() const
  { return weapon && weapon->type == WEAPON_WAND && weapon->max_dmg > 0; }

  // Wand-skade er magisk (Spell Type Magic); rustning gaelder ikke, selv om skolen er ukendt.
  double composite_target_armor( const action_state_t* ) const override
  { return 0.0; }

  // Wandens hastighed, ikke spell-haste.
  timespan_t execute_time() const override
  { return base_execute_time; }

  // Wand Specialization: +13/25 % wand-skade (rank-vaerdien fra spildataens kurve).
  double composite_da_multiplier( const action_state_t* s ) const override
  {
    double m = mage_spell_t::composite_da_multiplier( s );
    if ( p()->talent.wand_specialization.ok() )
      m *= 1.0 + p()->talent.wand_specialization->effectN( 1 ).percent();
    return m;
  }

  bool ready() override
  {
    if ( !has_wand() )
      return false;
    return mage_spell_t::ready();
  }
};

// Evocation (12051): kanaliseret paa magen selv i 8 s. Effekterne (Modify Power Regen% +1500 %
// og Allow Mana Regen While Casting 100 %) ligger paa buffen og laeses af parse_player_effects.
struct evocation_t : public mage_spell_t
{
  evocation_t( mage_t* player, std::string_view options_str ) :
    mage_spell_t( "evocation", player, player->rank.evocation.find_rank()->spell() )
  {
    parse_options( options_str );

    channeled = true;
    harmful = may_miss = may_crit = false;
    target = player;
  }

  void execute() override
  {
    mage_spell_t::execute();
    p()->buff.evocation->trigger( composite_dot_duration( execute_state ) );
  }

  void last_tick( dot_t* d ) override
  {
    mage_spell_t::last_tick( d );
    p()->buff.evocation->expire();
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
  if ( name == "evocation" )
    return new evocation_t( this, options_str );
  if ( name == "shoot" )
    return new shoot_t( this, options_str );

  return parse_player_effects_t::create_action( name, options_str );
}

void mage_t::init_base_stats()
{
  // Ingen egne konstanter: basis-crit m.m. kommer fra spildata via player_t.
  parse_player_effects_t::init_base_stats();
}

void mage_t::create_buffs()
{
  parse_player_effects_t::create_buffs();

  auto const evocation = rank.evocation.find_rank()->spell();
  buff.evocation = make_buff_fallback( evocation->ok(), this, "evocation", evocation )
                     ->set_cooldown( 0_ms )
                     ->set_affects_regen( true );
}

void mage_t::init_finished()
{
  // Evocations regen-effekter (aura 110 og 134) virker gennem buffen.
  parse_effects( buff.evocation );

  parse_player_effects_t::init_finished();
}

void mage_t::init_spells()
{
  parse_player_effects_t::init_spells();

  rank.evocation = { this,
    spell_container_t().set_spell( 12051 ),  // 20
  };

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

  parse_all_class_passives();
  // Talenternes passive effekter direkte fra spildata (som warlock, priest, shaman m.fl.):
  // Add Flat/Percent Modifier paa de beroerte spells (fx Improved Frostbolt, Ice Shards,
  // Arcane Impact) og spillerstats (fx Precision, Arcane Mind, Arcane Instability).
  parse_all_passive_talents();

  // Wand Specialization (6057) er i spildata Modify Damage Done% for alle magiske skoler med
  // kravet "Requires weapon: Wand", som simc ikke ser; registreret ville den give ALLE spells
  // bonussen. Den fjernes derfor (som i priest-modulet) og laegges kun paa Shoot.
  talent.wand_specialization = find_talent_spell( talent_tree::CLASS, "Wand Specialization" );
  deregister_passive_spell( talent.wand_specialization );
}

void mage_t::init_action_list()
{
  if ( !action_list_str.empty() )
  {
    parse_player_effects_t::init_action_list();
    return;
  }

  clear_action_priority_lists();
  mage_apl::mage( this );
  use_default_action_list = true;
  parse_player_effects_t::init_action_list();
}

}  // namespace mage

const module_t* module_t::mage()
{
  static mage::mage_module_t m;
  return &m;
}
