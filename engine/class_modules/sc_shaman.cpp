// ==========================================================================
// Dedmonwakeen's DPS-DPM Simulator.
// Send questions to natehieter@gmail.com
// ==========================================================================
// AULA-RETTELSE (simc-worker/patches/shaman.patch i AULA-Guild): Lightning Bolt, Chain Lightning,
// Elemental Focus, Lightning Overload, Lightning Mastery, Water Shield, Stormstrike, Flurry, Maelstrom
// Weapon, Windfury Totem, Rage of the Farseer og en caster-/naerkamp-rotation efter talenter. Se PATCHES.md.

#include "simulationcraft.hpp"
#include "action/parse_effects.hpp"
#include "class_modules/apl/shaman.hpp"

namespace {

struct shaman_t final : public player_t
{
  shaman_t( sim_t* sim, util::string_view name, race_e r = RACE_NONE ) :
    player_t( sim, SHAMAN, name, r ) {}

  resource_e primary_resource() const override { return RESOURCE_MANA; }

  void init_base_stats() override;
  void init_spells() override;
  void create_buffs() override;
  void create_actions() override;

  std::string default_potion() const override   { return shaman_apl::potion( this ); }
  std::string default_flask() const override    { return shaman_apl::flask( this ); }
  std::string default_food() const override     { return shaman_apl::food( this ); }

  double composite_melee_attack_power() const override;
  double composite_melee_auto_attack_speed() const override;
  void invalidate_cache( cache_e c ) override;
  void init_gains() override;

  cooldown_t* water_shield_icd = nullptr;
  bool water_shield_ready() const { return water_shield_icd->up(); }
  void trigger_water_shield();

  // Naerkamp-procs fra et main hand-slag (hvidt eller gult): Flurry, Windfury Totem, Maelstrom Weapon.
  void trigger_melee_procs( action_state_t* s, bool white );

  action_t* create_action( util::string_view name, util::string_view options_str ) override;
  void init_action_list() override;

  struct buffs_t
  {
    buff_t* flametongue_weapon = nullptr;
    buff_t* rockbiter_weapon = nullptr;

    buff_t* strength_of_earth = nullptr;

    buff_t* clearcasting = nullptr;       // Elemental Focus (16246)
    buff_t* lightning_mastery = nullptr;  // 16166
    buff_t* water_shield = nullptr;       // 408510
    buff_t* stormstrike = nullptr;        // 17364, effekt 2 (paa shamanen; eet maal)
    buff_t* flurry = nullptr;             // 16257
    buff_t* maelstrom_weapon = nullptr;   // 408505
    buff_t* rage_of_the_farseer = nullptr;// 425336
    buff_t* windfury_totem = nullptr;     // totemets aura (8515 ...)
  } buff;

  struct
  {
    gain_t* water_shield = nullptr;
  } gains;

  struct talents_t
  {
    // Enhancement
    // Row 1
    // player_talent_t thundering_strikes;

    // Row 2
    player_talent_t mental_dexterity;

    player_talent_t stormstrike;
    player_talent_t flurry;
    player_talent_t maelstrom_weapon;
    player_talent_t rage_of_the_farseer;

    // Elemental / Restoration
    player_talent_t elemental_focus;
    player_talent_t lightning_overload;
    player_talent_t lightning_mastery;
    player_talent_t water_shield;
  } talent;

  action_t* flametongue_attack = nullptr;
  action_t* windfury_attack = nullptr;
  cooldown_t* windfury_icd = nullptr;

  struct
  {
    spell_ranks_t earth_shock;
    spell_ranks_t flame_shock;
    spell_ranks_t flametongue_weapon;
    spell_ranks_t rockbiter_weapon;
    spell_ranks_t strength_of_earth_totem;
    spell_ranks_t lightning_bolt;
    spell_ranks_t chain_lightning;
    spell_ranks_t windfury_totem;
    spell_ranks_t searing_totem;
  } rank;
};

// SHAMAN MODULE INTERFACE ==================================================

struct shaman_module_t : public module_t
{
  shaman_module_t() : module_t( SHAMAN ) {}

  player_t* create_player( sim_t* sim, util::string_view name, race_e r = RACE_NONE ) const override
  {
    auto p = new shaman_t( sim, name, r );
    return p;
  }

  bool valid() const override
  {
    return true;
  }

  void register_actor_initializers( sim_t* sim ) const override
  {
    sim->register_actor_initializer( INIT_ACTOR_CREATE_BUFFS + offset(), []( player_t* p ) {
      p->buffs.bloodlust = make_buff( p, "bloodlust", p->find_spell( 2825 ) )
          ->set_cooldown( 0_ms )
          ->set_max_stack( 1 )
          ->set_default_value_from_effect_type( A_HASTE_ALL )
          ->add_invalidate( CACHE_HASTE );

      p->buffs.exhaustion = make_buff( p, "exhaustion", p->find_spell( 57723 ) )
          ->set_max_stack( 1 )
          ->set_quiet( true );
    }, "create_buffs_shaman" );
  }

  void register_hotfixes() const override {}
};

template <class Base>
struct shaman_action_t : public parse_action_effects_t<Base>
{
private:
  using ab = parse_action_effects_t<Base>;

public:
  using base_t = shaman_action_t<Base>;

  template <typename... Args>
  shaman_action_t( Args&&... args ) : ab( std::forward<Args>( args )... ) {}

  shaman_t* p()
  { return debug_cast<shaman_t*>( this->player ); }

  const shaman_t* p() const
  { return debug_cast<const shaman_t*>( this->player ); }
};


using shaman_attack_t = shaman_action_t<melee_attack_t>;
using shaman_spell_t  = shaman_action_t<spell_t>;

// Faelles for shamanens skade-spells (Fire, Frost, Nature): Clearcasting (Elemental Focus) goer den
// naeste gratis, Stormstrike (+20 %) bruges af Lightning Bolt, Chain Lightning og Earth Shock, og
// Water Shield giver 2 % af max mana ved et spell-crit (en kugle; 3,5 s mellem kuglerne).
struct shaman_damage_spell_t : public shaman_spell_t
{
  bool consumes_stormstrike = false;
  bool is_overload = false;

  template <typename... Args>
  shaman_damage_spell_t( Args&&... args ) : shaman_spell_t( std::forward<Args>( args )... ) {}

  double cost_pct_multiplier() const override
  {
    double m = shaman_spell_t::cost_pct_multiplier();
    if ( !is_overload && p()->buff.clearcasting->check() )
      m *= 1.0 + p()->buff.clearcasting->data().effectN( 1 ).percent();
    return m;
  }

  double composite_da_multiplier( const action_state_t* s ) const override
  {
    double m = shaman_spell_t::composite_da_multiplier( s );
    if ( consumes_stormstrike && p()->buff.stormstrike->check() )
      m *= 1.0 + p()->talent.stormstrike->effectN( 2 ).percent();
    return m;
  }

  void execute() override
  {
    bool cc = !is_overload && p()->buff.clearcasting->check() && cost() == 0;
    shaman_spell_t::execute();
    if ( is_overload )
      return;
    if ( cc )
      p()->buff.clearcasting->decrement();
    // Elemental Focus: et kast (ogsaa et miss) giver 10 % chance for Clearcasting (wowsims/forever).
    if ( p()->talent.elemental_focus.ok() && harmful &&
         rng().roll( p()->talent.elemental_focus->effectN( 1 ).percent() ) )
      p()->buff.clearcasting->trigger();
  }

  void impact( action_state_t* s ) override
  {
    shaman_spell_t::impact( s );
    if ( consumes_stormstrike && result_is_hit( s->result ) && p()->buff.stormstrike->check() )
      p()->buff.stormstrike->decrement();
    if ( s->result == RESULT_CRIT && p()->buff.water_shield->check() && p()->water_shield_ready() )
      p()->trigger_water_shield();
  }
};

void shaman_t::init_base_stats()
{
  base.attack_power_per_strength = 2.0;

  player_t::init_base_stats();

  base.stats.attack_power = 2.0 * level() - 20.0;

  // original classic crit rates according to
  // * https://github.com/wowsims/classic/blob/master/sim/core/base_stats.go#L84
  // TODO: Verify in-game
  base.attack_crit_chance = get_passive_player_value( 0.017, "all_crit" );
  base.spell_crit_chance = get_passive_player_value( 0.023, "all_crit" );
}

void shaman_t::init_spells()
{
  player_t::init_spells();

  rank.earth_shock = spell_ranks_t( this,
    spell_container_t().set_spell( 8042 ),
    spell_container_t().set_spell( 8044 ),
    spell_container_t().set_spell( 8045 ),
    spell_container_t().set_spell( 8046 ),
    spell_container_t().set_spell( 10412 ),
    spell_container_t().set_spell( 10413 ),
    spell_container_t().set_spell( 10414 )
  );
  rank.flame_shock = spell_ranks_t( this,
    spell_container_t().set_spell( 8050 ),
    spell_container_t().set_spell( 8052 ),
    spell_container_t().set_spell( 8053 ),
    spell_container_t().set_spell( 10447 ),
    spell_container_t().set_spell( 10448 ),
    spell_container_t().set_spell( 29228 )
  );
  rank.flametongue_weapon = spell_ranks_t( this,
    spell_container_t().set_spell( 8024 ).set_driver( 8026 ).set_attack( 29469 ),
    spell_container_t().set_spell( 8027 ).set_driver( 8028 ).set_attack( 29470 ),
    spell_container_t().set_spell( 8030 ).set_driver( 8029 ).set_attack( 10444 ),
    spell_container_t().set_spell( 16339 ).set_driver( 10445 ).set_attack( 10444 ),
    spell_container_t().set_spell( 16341 ).set_driver( 16343 ).set_attack( 10444 ),
    spell_container_t().set_spell( 16342 ).set_driver( 16344 ).set_attack( 10444 )
  );
  rank.rockbiter_weapon = spell_ranks_t( this,
    spell_container_t().set_spell( 8017 ).set_passive( 10400 ),
    spell_container_t().set_spell( 8018 ).set_passive( 15567 ),
    spell_container_t().set_spell( 8019 ).set_passive( 15568 ),
    spell_container_t().set_spell( 10399 ).set_passive( 15569 ),
    spell_container_t().set_spell( 16314 ).set_passive( 16311 ),
    spell_container_t().set_spell( 16315 ).set_passive( 16312 ),
    spell_container_t().set_spell( 16316 ).set_passive( 16313 )
  );
  rank.strength_of_earth_totem = spell_ranks_t( this,
    spell_container_t().set_spell( 8075 ).set_aura( 8076 ),
    spell_container_t().set_spell( 8160 ).set_aura( 8162 ),
    spell_container_t().set_spell( 8161 ).set_aura( 8163 ),
    spell_container_t().set_spell( 10442 ).set_aura( 10441 ),
    spell_container_t().set_spell( 25361 ).set_aura( 25362 )
  );

  parse_all_class_passives();
  parse_all_passive_talents();

  talent.mental_dexterity = find_talent_spell( talent_tree::CLASS, "Mental Dexterity" );

  talent.stormstrike         = find_talent_spell( talent_tree::CLASS, "Stormstrike" );
  talent.flurry              = find_talent_spell( talent_tree::CLASS, "Flurry" );
  talent.maelstrom_weapon    = find_talent_spell( talent_tree::CLASS, "Maelstrom Weapon" );
  talent.rage_of_the_farseer = find_talent_spell( talent_tree::CLASS, "Rage of the Farseer" );
  talent.elemental_focus     = find_talent_spell( talent_tree::CLASS, "Elemental Focus" );
  talent.lightning_overload  = find_talent_spell( talent_tree::CLASS, "Lightning Overload" );
  talent.lightning_mastery   = find_talent_spell( talent_tree::CLASS, "Lightning Mastery" );
  talent.water_shield        = find_talent_spell( talent_tree::CLASS, "Water Shield" );

  rank.lightning_bolt = spell_ranks_t( this,
    spell_container_t().set_spell( 403 ),    // 1
    spell_container_t().set_spell( 529 ),    // 8
    spell_container_t().set_spell( 548 ),    // 14
    spell_container_t().set_spell( 915 ),    // 20
    spell_container_t().set_spell( 943 ),    // 26
    spell_container_t().set_spell( 6041 ),   // 32
    spell_container_t().set_spell( 10391 ),  // 38
    spell_container_t().set_spell( 10392 ),  // 44
    spell_container_t().set_spell( 15207 ),  // 50
    spell_container_t().set_spell( 15208 )   // 56
  );
  rank.chain_lightning = spell_ranks_t( this,
    spell_container_t().set_spell( 421 ),    // 32
    spell_container_t().set_spell( 930 ),    // 40
    spell_container_t().set_spell( 2860 ),   // 48
    spell_container_t().set_spell( 10605 )   // 56
  );
  // Searing Totem: kast og totemets angreb.
  rank.searing_totem = spell_ranks_t( this,
    spell_container_t().set_spell( 3599 ).set_attack( 3606 ),    // 10
    spell_container_t().set_spell( 6363 ).set_attack( 6350 ),    // 20
    spell_container_t().set_spell( 6364 ).set_attack( 6351 ),    // 30
    spell_container_t().set_spell( 6365 ).set_attack( 6352 ),    // 40
    spell_container_t().set_spell( 10437 ).set_attack( 10435 ),  // 50
    spell_container_t().set_spell( 10438 ).set_attack( 10436 )   // 60
  );
  // Windfury Totem: kast, aura (proc-chance), proc (ekstra slag + AP).
  rank.windfury_totem = spell_ranks_t( this,
    spell_container_t().set_spell( 8512 ).set_aura( 8515 ).set_attack( 8516 ),     // 32
    spell_container_t().set_spell( 10613 ).set_aura( 10609 ).set_attack( 10608 ),  // 42
    spell_container_t().set_spell( 10614 ).set_aura( 10612 ).set_attack( 10610 )   // 52
  );
}

double shaman_t::composite_melee_attack_power() const
{
  double ap = player_t::composite_melee_attack_power();

  ap += std::floor( talent.mental_dexterity->effectN( 1 ).percent() * cache.intellect() );

  return ap;
}

void shaman_t::invalidate_cache( cache_e c )
{
  player_t::invalidate_cache( c );

  if ( c == CACHE_INTELLECT && talent.mental_dexterity.ok() )
    invalidate_cache( CACHE_ATTACK_POWER );
}

// ==========================================================================
// Shaman Shock Spells
// ==========================================================================

// TODO: Shared shock abstraction?

// Earth Shock

struct earth_shock_t : public shaman_damage_spell_t
{
  earth_shock_t( shaman_t* player, util::string_view options_str ) :
    shaman_damage_spell_t( "earth_shock", player, player->rank.earth_shock.find_rank()->spell() )
  {
    parse_options( options_str );
    consumes_stormstrike = true;

    cooldown           = player->get_cooldown( "shock" );
    cooldown->duration = data().cooldown();
  }
};

// Flame Shock

struct flame_shock_t : public shaman_damage_spell_t
{
  flame_shock_t( shaman_t* player, util::string_view options_str ) :
    shaman_damage_spell_t( "flame_shock", player, player->rank.flame_shock.find_rank()->spell() )
  {
    parse_options( options_str );

    cooldown           = player->get_cooldown( "shock" );
    cooldown->duration = data().cooldown();
  }
};

// ==========================================================================
// Lightning Bolt / Chain Lightning (AULA-RETTELSE)
// ==========================================================================

// Lightning Overload (talent 408438): 10/20/30 % chance for en kopi af Lightning Bolt/Chain Lightning
// paa samme maal med halv skade og uden pris. Kopien bruger hovedspellens spildata (antagelse: Forever
// har egne overload-raekker, fx 408439, med andre koefficienter; tooltippet siger «half damage»).
struct lightning_overload_t : public shaman_damage_spell_t
{
  lightning_overload_t( shaman_t* player, std::string_view n, const spell_data_t* s ) :
    shaman_damage_spell_t( n, player, s )
  {
    background = true;
    is_overload = true;
    base_costs[ RESOURCE_MANA ] = 0;
    base_multiplier *= 0.5;
  }
};

struct lightning_spell_t : public shaman_damage_spell_t
{
  action_t* overload = nullptr;

  lightning_spell_t( shaman_t* player, std::string_view n, const spell_data_t* s, std::string_view options_str ) :
    shaman_damage_spell_t( n, player, s )
  {
    parse_options( options_str );
    consumes_stormstrike = true;
    if ( player->talent.lightning_overload.ok() && data().ok() )
    {
      overload = new lightning_overload_t( player, std::string( n ) + "_overload", s );
      add_child( overload );
    }
  }

  void impact( action_state_t* s ) override
  {
    shaman_damage_spell_t::impact( s );
    if ( overload && rng().roll( p()->talent.lightning_overload->effectN( 1 ).percent() ) )
      overload->execute_on_target( s->target );
  }
};

// Lightning Bolt (403 ...): Lightning Mastery -0,5 s; Maelstrom Weapon -4 % pr. point pr. stak paa
// kastetid og pris (talentets effekt 1 x stakke), og bruges op af kastet (wowsims/forever).
struct lightning_bolt_t : public lightning_spell_t
{
  lightning_bolt_t( shaman_t* player, util::string_view options_str ) :
    lightning_spell_t( player, "lightning_bolt", player->rank.lightning_bolt.find_rank()->spell(), options_str )
  {}

  double maelstrom_pct() const
  {
    if ( !p()->talent.maelstrom_weapon.ok() )
      return 0.0;
    return std::min( 1.0, -p()->talent.maelstrom_weapon->effectN( 1 ).percent() * p()->buff.maelstrom_weapon->check() );
  }

  timespan_t execute_time() const override
  {
    timespan_t t = lightning_spell_t::execute_time();
    if ( p()->buff.lightning_mastery->check() )
      t += timespan_t::from_millis( p()->talent.lightning_mastery->effectN( 1 ).base_value() );
    t *= 1.0 - maelstrom_pct();
    return std::max( 0_ms, t );
  }

  double cost_pct_multiplier() const override
  { return lightning_spell_t::cost_pct_multiplier() * ( 1.0 - maelstrom_pct() ); }

  void execute() override
  {
    lightning_spell_t::execute();
    p()->buff.maelstrom_weapon->expire();
  }
};

// Chain Lightning (421 ...): 6 s nedkoeling, ingen under Lightning Mastery.
struct chain_lightning_t : public lightning_spell_t
{
  chain_lightning_t( shaman_t* player, util::string_view options_str ) :
    lightning_spell_t( player, "chain_lightning", player->rank.chain_lightning.find_rank()->spell(), options_str )
  {}

  void update_ready( timespan_t cd ) override
  {
    if ( p()->buff.lightning_mastery->check() )
      cd = 0_ms;
    lightning_spell_t::update_ready( cd );
  }
};

// Lightning Mastery (talent 16166): 15 s buff, 180 s nedkoeling. Varer hele tiden (wowsims/forever).
struct lightning_mastery_t : public shaman_spell_t
{
  lightning_mastery_t( shaman_t* player, util::string_view options_str ) :
    shaman_spell_t( "lightning_mastery", player, player->talent.lightning_mastery )
  {
    parse_options( options_str );
    harmful = may_miss = may_crit = false;
    target = player;
    trigger_gcd = 0_ms;
  }

  void execute() override
  {
    shaman_spell_t::execute();
    p()->buff.lightning_mastery->trigger();
  }
};

// Water Shield (talent 408510): 3 kugler; et spell-crit giver 2 % af max mana (408511) og bruger en.
struct water_shield_t : public shaman_spell_t
{
  water_shield_t( shaman_t* player, util::string_view options_str ) :
    shaman_spell_t( "water_shield", player, player->talent.water_shield )
  {
    parse_options( options_str );
    harmful = may_miss = may_crit = false;
    target = player;
  }

  void execute() override
  {
    shaman_spell_t::execute();
    p()->buff.water_shield->trigger( p()->buff.water_shield->max_stack() );
  }

  bool ready() override
  {
    if ( p()->buff.water_shield->check() )
      return false;
    return shaman_spell_t::ready();
  }
};

// Rage of the Farseer (talent 425336): +30 % angrebshastighed i 25 s, 180 s nedkoeling.
struct rage_of_the_farseer_t : public shaman_spell_t
{
  rage_of_the_farseer_t( shaman_t* player, util::string_view options_str ) :
    shaman_spell_t( "rage_of_the_farseer", player, player->talent.rage_of_the_farseer )
  {
    parse_options( options_str );
    harmful = may_miss = may_crit = false;
    target = player;
  }

  void execute() override
  {
    shaman_spell_t::execute();
    p()->buff.rage_of_the_farseer->trigger();
  }
};

// Stormstrike (talent 17364): 150 % normaliseret vaabenskade (effekt 1 + 3) og +20 % paa naeste
// Lightning Bolt, Chain Lightning eller Earth Shock (effekt 2, 12 s, en ladning).
struct stormstrike_t : public shaman_attack_t
{
  stormstrike_t( shaman_t* player, util::string_view options_str ) :
    shaman_attack_t( "stormstrike", player, player->talent.stormstrike )
  {
    parse_options( options_str );
    weapon = &player->equipped_weapons[ SLOT_MAIN_HAND ];
    normalize_weapon_speed = true;
    weapon_multiplier = data().ok() ? data().effectN( 3 ).percent() : 1.0;
    base_dd_min = base_dd_max = 0;
    may_glance = false;
  }

  void impact( action_state_t* s ) override
  {
    shaman_attack_t::impact( s );
    if ( result_is_hit( s->result ) )
    {
      p()->buff.stormstrike->trigger();
      p()->trigger_melee_procs( s, false );
    }
  }
};

// ==========================================================================
// Shaman Weapon Imbues
// ==========================================================================

struct flametongue_attack_t : public shaman_spell_t
{
  flametongue_attack_t( shaman_t* player ) :
    shaman_spell_t( "flametongue_attack", player, player->rank.flametongue_weapon.find_rank()->attack() )
  {
    background = true;

    auto driver = player->rank.flametongue_weapon.find_rank()->driver();

    double proc_value = driver->effectN( 1 ).average( player, player->level() );
    double weapon_speed = player->equipped_weapons[ SLOT_MAIN_HAND ].swing_time.total_seconds();

    base_dd_min = base_dd_max = proc_value * weapon_speed / 100.0;
  }
};

struct flametongue_weapon_t : public shaman_spell_t
{
  flametongue_weapon_t( shaman_t* player, util::string_view options_str ) :
    shaman_spell_t( "flametongue_weapon", player, player->rank.flametongue_weapon.find_rank()->spell() )
  {
    parse_options( options_str );
    harmful = false;
    target = player;
  }

  void execute() override
  {
    shaman_spell_t::execute();

    p()->buff.rockbiter_weapon->expire();
    p()->buff.flametongue_weapon->trigger();
  }

  bool ready() override
  {
    if ( p()->buff.flametongue_weapon->check() )
      return false;

    return shaman_spell_t::ready();
  }
};

// rockbiter weapon

struct rockbiter_weapon_t : public shaman_spell_t
{
  rockbiter_weapon_t( shaman_t* player, util::string_view options_str ) :
    shaman_spell_t( "rockbiter_weapon", player, player->rank.rockbiter_weapon.find_rank()->spell() )
  {
    parse_options( options_str );
    harmful = false;
    target = player;
  }

  void execute() override
  {
    shaman_spell_t::execute();

    p()->buff.flametongue_weapon->expire();
    p()->buff.rockbiter_weapon->trigger();
  }

  bool ready() override
  {
    if ( p()->buff.rockbiter_weapon->check() )
      return false;

    return shaman_spell_t::ready();
  }
};

// ==========================================================================
// Shaman Totems
// ==========================================================================

// TODO: Totem element exclusivity
// TODO: Make totems a unit for use with range?
// TODO: Party buff

struct strength_of_earth_totem_t : public shaman_spell_t
{
  strength_of_earth_totem_t( shaman_t* player, util::string_view options_str ) :
    shaman_spell_t( "strength_of_earth_totem", player, player->rank.strength_of_earth_totem.find_rank()->spell() )
  {
    parse_options( options_str );
    harmful = false;
    target = player;
  }

  void execute() override
  {
    shaman_spell_t::execute();

    p()->buff.strength_of_earth->trigger();
  }
};

// Searing Totem (3599 ...): totemet angriber maalet hvert 2,2 s (totemets «Attack», 3606 ...) i hele
// totemets levetid. Modelleret som en DoT fra shamanen: tick = angrebets skade (effekt 1, gennemsnit) og
// dets SP-koefficient, med shamanens hit og crit. Antagelse: totemet staar inden for raekkevidde, og
// talenter, der kun rammer totemets egen spell (Call of Flame), er ikke med.
struct searing_totem_t : public shaman_spell_t
{
  searing_totem_t( shaman_t* player, util::string_view options_str ) :
    shaman_spell_t( "searing_totem", player, player->rank.searing_totem.find_rank()->spell() )
  {
    parse_options( options_str );
    auto attack = player->rank.searing_totem.find_rank()->attack();
    school = SCHOOL_FIRE;
    base_dd_min = base_dd_max = 0;
    if ( data().ok() && attack->ok() )
    {
      dot_duration          = data().duration();
      base_tick_time        = attack->cast_time();
      base_td               = attack->effectN( 1 ).average( player );
      spell_power_mod.tick  = attack->effectN( 1 ).sp_coeff();
      tick_may_crit         = true;
      hasted_ticks          = false;
      tick_zero             = false;
    }
  }
};

// Windfury Totem (8512 ...): totemet som en selv-aura (som Strength of Earth ovenfor; Forever har ingen
// totem-twisting). Main hand-slag har 20 % chance (0,1 s ICD) for et ekstra slag med +AP (8516 ...).
struct windfury_totem_t : public shaman_spell_t
{
  windfury_totem_t( shaman_t* player, util::string_view options_str ) :
    shaman_spell_t( "windfury_totem", player, player->rank.windfury_totem.find_rank()->spell() )
  {
    parse_options( options_str );
    harmful = false;
    target = player;
  }

  void execute() override
  {
    shaman_spell_t::execute();
    p()->buff.windfury_totem->trigger();
  }
};

// ==========================================================================
// Shaman Attack
// ==========================================================================

// shaman_attack_t::impact ============================================

// Melee Attack =============================================================

struct shaman_melee_t : public shaman_attack_t
{
  shaman_melee_t( util::string_view name, player_t* player, weapon_t* w )
    : shaman_attack_t( name, player, spell_data_t::nil() )
  {
    weapon            = w;
    weapon_multiplier = 1.0;
    base_execute_time = w->swing_time;
    school            = SCHOOL_PHYSICAL;
    trigger_gcd       = 0_ms;

    background        = true;
    repeating         = true;
    may_glance        = true;
    may_crit          = true;
    may_dodge         = true;
    may_parry         = true;
    special           = false;
  }

  void impact( action_state_t* state ) override
  {
    shaman_attack_t::impact( state );

    if ( result_is_hit( state->result ) && p()->buff.flametongue_weapon->check() )
      p()->flametongue_attack->execute_on_target( state->target );

    p()->trigger_melee_procs( state, true );
  }

  // Flurry: hvert hvidt slag bruger en ladning (ogsaa et miss).
  void execute() override
  {
    if ( p()->buff.flurry->check() )
      p()->buff.flurry->decrement();
    shaman_attack_t::execute();
  }
};

// Windfury Totems ekstra slag: et main hand-slag med +AP fra procens effekt 1.
struct windfury_attack_t : public shaman_attack_t
{
  double bonus_ap = 0;

  windfury_attack_t( shaman_t* p, const spell_data_t* proc ) :
    shaman_attack_t( "windfury_attack", p, proc )
  {
    background = true;
    weapon = &p->equipped_weapons[ SLOT_MAIN_HAND ];
    weapon_multiplier = 1.0;
    base_dd_min = base_dd_max = 0;
    may_glance = false;
    may_crit = true;
    bonus_ap = proc->ok() ? proc->effectN( 1 ).average( p ) : 0.0;
  }

  double composite_total_attack_power() const override
  { return shaman_attack_t::composite_total_attack_power() + bonus_ap * player->composite_attack_power_multiplier(); }

  void impact( action_state_t* s ) override
  {
    shaman_attack_t::impact( s );
    if ( result_is_hit( s->result ) && p()->buff.flametongue_weapon->check() )
      p()->flametongue_attack->execute_on_target( s->target );
    p()->trigger_melee_procs( s, false );
  }
};


struct shaman_auto_attack_t : public shaman_attack_t
{
  shaman_auto_attack_t( shaman_t* p, util::string_view options_str )
    : shaman_attack_t( "auto_attack", p, spell_data_t::nil() )
  {
    parse_options( options_str );
    trigger_gcd           = 0_ms;
    ignore_false_positive = true;

    assert( p->equipped_weapons[ SLOT_MAIN_HAND ].type != WEAPON_NONE );

    p->main_hand_attack = new shaman_melee_t( "melee", p, &p->equipped_weapons[ SLOT_MAIN_HAND ] );
  }

  void execute() override
  {
    player->main_hand_attack->schedule_execute();
  }

  bool ready() override
  {
    if ( player->is_moving() )
    {
      return false;
    }
    return player->main_hand_attack->execute_event == nullptr;
  }
};

action_t* shaman_t::create_action( util::string_view name, util::string_view options_str )
{
  if ( name == "auto_attack" )
    return new shaman_auto_attack_t( this, options_str );

  if ( name == "earth_shock" )
    return new earth_shock_t( this, options_str );
  if ( name == "flame_shock" )
    return new flame_shock_t( this, options_str );

  if ( name == "flametongue_weapon" )
    return new flametongue_weapon_t( this, options_str );
  if ( name == "rockbiter_weapon" )
    return new rockbiter_weapon_t( this, options_str );

  if ( name == "strength_of_earth_totem" )
    return new strength_of_earth_totem_t( this, options_str );

  if ( name == "lightning_bolt" )
    return new lightning_bolt_t( this, options_str );
  if ( name == "chain_lightning" )
    return new chain_lightning_t( this, options_str );
  if ( name == "lightning_mastery" )
    return new lightning_mastery_t( this, options_str );
  if ( name == "water_shield" )
    return new water_shield_t( this, options_str );
  if ( name == "stormstrike" )
    return new stormstrike_t( this, options_str );
  if ( name == "rage_of_the_farseer" )
    return new rage_of_the_farseer_t( this, options_str );
  if ( name == "windfury_totem" )
    return new windfury_totem_t( this, options_str );
  if ( name == "searing_totem" )
    return new searing_totem_t( this, options_str );

  return player_t::create_action( name, options_str );
}

void shaman_t::create_buffs()
{
  player_t::create_buffs();

  buff.flametongue_weapon = make_buff( this, "flametongue_weapon", rank.flametongue_weapon.find_rank()->spell() );
  buff.rockbiter_weapon = make_buff<stat_buff_t>( this, "rockbiter_weapon", rank.rockbiter_weapon.find_rank()->passive() );

  auto soet_rank = rank.strength_of_earth_totem.find_rank();
  buff.strength_of_earth = make_buff<stat_buff_t>( this, "strength_of_earth", soet_rank->aura() )
    ->set_duration( soet_rank->spell()->duration() );

  buff.clearcasting = make_buff_fallback( talent.elemental_focus.ok(), this, "clearcasting", find_spell( 16246 ) );
  buff.lightning_mastery = make_buff_fallback( talent.lightning_mastery.ok(), this, "lightning_mastery", talent.lightning_mastery )
    ->set_cooldown( 0_ms );
  buff.water_shield = make_buff_fallback( talent.water_shield.ok(), this, "water_shield", talent.water_shield )
    ->set_cooldown( 0_ms );
  buff.stormstrike = make_buff_fallback( talent.stormstrike.ok(), this, "stormstrike", talent.stormstrike )
    ->set_cooldown( 0_ms )
    ->set_max_stack( 1 );
  // Flurry: talentets rank (6-30 %) paa 3 slag; kun kraeves af naerkamp.
  buff.flurry = make_buff_fallback( talent.flurry.ok(), this, "flurry", find_spell( 16257 ) );
  if ( talent.flurry.ok() )
    buff.flurry->set_default_value( talent.flurry->effectN( 1 ).percent() )
      ->set_cooldown( 0_ms )
      ->add_invalidate( CACHE_AUTO_ATTACK_SPEED );
  buff.maelstrom_weapon = make_buff_fallback( talent.maelstrom_weapon.ok(), this, "maelstrom_weapon", find_spell( 408505 ) );
  buff.rage_of_the_farseer = make_buff_fallback( talent.rage_of_the_farseer.ok(), this, "rage_of_the_farseer",
                                                 talent.rage_of_the_farseer );
  if ( talent.rage_of_the_farseer.ok() )
    buff.rage_of_the_farseer->set_default_value_from_effect( 1 )
      ->set_cooldown( 0_ms )
      ->add_invalidate( CACHE_AUTO_ATTACK_SPEED );
  auto wft = rank.windfury_totem.find_rank();
  buff.windfury_totem = make_buff_fallback( wft->spell()->ok(), this, "windfury_totem", wft->spell() )
    ->set_duration( wft->spell()->duration() )
    ->set_cooldown( 0_ms );
}

void shaman_t::init_gains()
{
  player_t::init_gains();
  gains.water_shield = get_gain( "water_shield" );
  water_shield_icd = get_cooldown( "water_shield_icd" );
  water_shield_icd->duration = talent.water_shield.ok() ? talent.water_shield->internal_cooldown() : 0_ms;
  windfury_icd = get_cooldown( "windfury_totem_icd" );
  windfury_icd->duration = 100_ms;
}

void shaman_t::trigger_water_shield()
{
  water_shield_icd->start();
  resource_gain( RESOURCE_MANA, find_spell( 408511 )->effectN( 1 ).percent() * resources.max[ RESOURCE_MANA ],
                 gains.water_shield );
  buff.water_shield->decrement();
}

double shaman_t::composite_melee_auto_attack_speed() const
{
  double h = player_t::composite_melee_auto_attack_speed();
  if ( buff.flurry && buff.flurry->check() )
    h /= 1.0 + buff.flurry->check_value();
  if ( buff.rage_of_the_farseer && buff.rage_of_the_farseer->check() )
    h /= 1.0 + buff.rage_of_the_farseer->check_value();
  return h;
}

void shaman_t::trigger_melee_procs( action_state_t* s, bool white )
{
  (void)white;
  if ( !action_t::result_is_hit( s->result ) )
    return;

  // Flurry: et naerkamps-crit giver 3 ladninger.
  if ( talent.flurry.ok() && s->result == RESULT_CRIT )
    buff.flurry->trigger( buff.flurry->max_stack() );

  // Maelstrom Weapon: 20 slag pr. minut-rate efter main hand-hastigheden (wowsims/forever: «TODO»,
  // maalt 10 og fordoblet i patch notes). Et ekstra slag kan ikke selv give et Windfury-slag.
  if ( talent.maelstrom_weapon.ok() )
  {
    double chance = 20.0 * equipped_weapons[ SLOT_MAIN_HAND ].swing_time.total_seconds() / 60.0;
    if ( rng().roll( chance ) )
      buff.maelstrom_weapon->trigger();
  }

  if ( windfury_attack && buff.windfury_totem->check() && s->action != windfury_attack && windfury_icd->up() &&
       rng().roll( rank.windfury_totem.find_rank()->aura()->proc_chance() ) )
  {
    windfury_icd->start();
    windfury_attack->execute_on_target( s->target );
  }
}

void shaman_t::create_actions()
{
  flametongue_attack = new flametongue_attack_t( this );
  {
    auto wft = rank.windfury_totem.find_rank();
    if ( wft->spell()->ok() && equipped_weapons[ SLOT_MAIN_HAND ].type != WEAPON_NONE )
      windfury_attack = new windfury_attack_t( this, wft->attack() );
  }

  player_t::create_actions();
}

void shaman_t::init_action_list()
{
  if ( !action_list_str.empty() )
  {
    player_t::init_action_list();
    return;
  }

  clear_action_priority_lists();
  shaman_apl::shaman( this );
  use_default_action_list = true;
  player_t::init_action_list();
}

}  // namespace

const module_t* module_t::shaman()
{
  static ::shaman_module_t m;
  return &m;
}
