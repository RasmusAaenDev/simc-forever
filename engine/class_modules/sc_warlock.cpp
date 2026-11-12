// AULA-RETTELSE (simc-worker/patches/warlock.patch i AULA-Guild): Life Tap, wand (Shoot), Bane of Agony,
// Siphon Life, Shadowburn, Conflagrate, Incinerate, Wrack, Improved Shadow Bolt og en rotation der vaelger
// efter talenter. Se PATCHES.md.
#include "simulationcraft.hpp"
#include "action/parse_effects.hpp"

#include "class_modules/apl/warlock.hpp"

namespace warlock
{
struct warlock_t final : public player_t
{
  warlock_t( sim_t* sim, util::string_view name, race_e r = RACE_NONE ) :
    player_t( sim, WARLOCK, name, r ) {}

  resource_e primary_resource() const override { return RESOURCE_MANA; }

  std::string default_potion() const override   { return warlock_apl::potion( this ); }
  std::string default_flask() const override    { return warlock_apl::flask( this ); }
  std::string default_food() const override     { return warlock_apl::food( this ); }

  action_t* create_action( util::string_view name, util::string_view options_str ) override;
  void init_action_list() override;
  void init_base_stats() override;
  void init_spells() override;
  void create_buffs() override;
  void init_gains() override;

  void create_pets() override;

  struct pets_t
  {
    pet_t* imp = nullptr;
  } pets;

  struct
  {
    spell_ranks_t corruption;
    spell_ranks_t firebolt;
    spell_ranks_t immolate;
    spell_ranks_t shadow_bolt;
    spell_ranks_t life_tap;
    spell_ranks_t bane_of_agony;
    spell_ranks_t siphon_life;
    spell_ranks_t shadowburn;
    spell_ranks_t conflagrate;
    spell_ranks_t incinerate;
    spell_ranks_t wrack;
  } rank;

  struct
  {
    player_talent_t improved_life_tap;
    player_talent_t siphon_life;
    player_talent_t shadowburn;
    player_talent_t conflagrate;
    player_talent_t incinerate;
    player_talent_t wrack;
    player_talent_t improved_shadow_bolt;
  } talent;

  struct
  {
    // Improved Shadow Bolt: Shadow Vulnerability (17794). Ligger paa warlocken (kun dens egen skade
    // paavirkes, og Patchwerk har eet maal); med flere maal gaelder den dermed for alle.
    buff_t* shadow_vulnerability = nullptr;
  } buff;

  struct
  {
    gain_t* life_tap = nullptr;
  } gains;

  // Til Conflagrate (forbruger Immolate) og Wrack (forstaerker de andre Shadow-DoTs).
  action_t* immolate_action = nullptr;
  action_t* wrack_action    = nullptr;
};

struct warlock_module_t : public module_t
{
  warlock_module_t() : module_t( WARLOCK )
  { }

  player_t* create_player( sim_t* sim, util::string_view name, race_e r = RACE_NONE ) const override
  {
    auto p = new warlock_t( sim, name, r );
    return p;
  }

  bool valid() const override
  { return true; }

  void register_hotfixes() const override {}

  void register_actor_initializers( sim_t* ) const override {}
};

template <class Base>
struct warlock_action_t : public parse_action_effects_t<Base>
{
private:
  using ab = parse_action_effects_t<Base>;

public:
  using base_t = warlock_action_t<Base>;

  template <typename... Args>
  warlock_action_t( Args&&... args ) : ab( std::forward<Args>( args )... ) {}

  warlock_t* p()
  { return debug_cast<warlock_t*>( this->player ); }

  const warlock_t* p() const
  { return debug_cast<const warlock_t*>( this->player ); }

  // Maalets debuffs fra warlocken: Shadow Vulnerability (Improved Shadow Bolt) paa al Shadow-skade, og
  // Wrack (+10 % paa warlockens andre Shadow-DoTs, mens den kanaliseres). Maal-multiplikatoren
  // genberegnes ved hvert tick, saa begge virker ogsaa paa DoTs, der allerede tikker.
  double composite_target_multiplier( player_t* t ) const override
  {
    double m = ab::composite_target_multiplier( t );
    if ( this->get_school() == SCHOOL_SHADOW && p()->buff.shadow_vulnerability &&
         p()->buff.shadow_vulnerability->check() )
      m *= 1.0 + p()->buff.shadow_vulnerability->check_value();
    return m;
  }

  double composite_target_ta_multiplier( player_t* t ) const override
  {
    double m = ab::composite_target_ta_multiplier( t );
    const action_t* wrack = p()->wrack_action;
    if ( wrack && wrack != this && this->get_school() == SCHOOL_SHADOW )
    {
      const dot_t* d = wrack->find_dot( t );
      if ( d && d->is_ticking() )
        m *= 1.0 + wrack->data().effectN( 2 ).percent();
    }
    return m;
  }
};

using warlock_spell_t  = warlock_action_t<spell_t>;

// Shadow Bolt
struct shadow_bolt_t : public warlock_spell_t
{
  shadow_bolt_t( warlock_t* player, util::string_view options_str ) :
    warlock_spell_t( "shadow_bolt", player, player->rank.shadow_bolt.find_rank()->spell() )
  {
    parse_options( options_str );
  }

  // Improved Shadow Bolt: et crit giver Shadow Vulnerability (+4-20 % Shadow-skade efter rank, 12 s).
  void impact( action_state_t* s ) override
  {
    warlock_spell_t::impact( s );
    if ( p()->talent.improved_shadow_bolt.ok() && s->result == RESULT_CRIT )
      p()->buff.shadow_vulnerability->trigger( 1, p()->talent.improved_shadow_bolt->effectN( 1 ).percent() );
  }
};

// Immolate
struct immolate_t : public warlock_spell_t
{
  immolate_t( warlock_t* player, util::string_view options_str ) :
    warlock_spell_t( "immolate", player, player->rank.immolate.find_rank()->spell() )
  {
    parse_options( options_str );
    if ( !player->immolate_action )
      player->immolate_action = this;
  }
};

// Corruption
struct corruption_t : public warlock_spell_t
{
  corruption_t( warlock_t* player, util::string_view options_str ) :
    warlock_spell_t( "corruption", player, player->rank.corruption.find_rank()->spell() )
  {
    parse_options( options_str );
  }
};

// Shoot (wand, 5019): samme model som mage- og priest-rettelsen. Wandens vaabenskade fra
// ranged-pladsen, spell-tabellen (hit/crit), ingen rustning, wandens hastighed, ingen mana.
// Uden wand i ranged-pladsen er handlingen aldrig klar.
struct shoot_t : public warlock_spell_t
{
  shoot_t( warlock_t* player, util::string_view options_str ) :
    warlock_spell_t( "shoot", player, player->find_spell( 5019 ) )
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

  double composite_target_armor( const action_state_t* ) const override
  { return 0.0; }

  timespan_t execute_time() const override
  { return base_execute_time; }

  bool ready() override
  {
    if ( !has_wand() )
      return false;
    return warlock_spell_t::ready();
  }
};

// Life Tap (1454 ...): (effekt 1 + Spirit) liv bliver til lige saa meget mana, Improved Life Tap
// (18182) oeger kun manaen (som wowsims/forever lifetap.go). Spillerens liv er uendeligt i simc
// (ingen indgaaende skade i en DPS-simulering), saa livet traekkes ikke; antagelse: en healer
// eller pauser daekker det. Ingen mana-pris, saa femsekundsreglen nulstilles ikke.
struct life_tap_t : public warlock_spell_t
{
  life_tap_t( warlock_t* player, util::string_view options_str ) :
    warlock_spell_t( "life_tap", player, player->rank.life_tap.find_rank()->spell() )
  {
    parse_options( options_str );
    harmful = may_miss = may_crit = callbacks = false;
    target = player;
    base_dd_min = base_dd_max = 0;
    spell_power_mod.direct = 0;
  }

  double mana_gain() const
  {
    double amount = data().effectN( 1 ).average( p() ) + p()->cache.spirit();
    if ( p()->talent.improved_life_tap.ok() )
      amount *= 1.0 + p()->talent.improved_life_tap->effectN( 1 ).percent();
    return amount;
  }

  void execute() override
  {
    warlock_spell_t::execute();
    p()->resource_gain( RESOURCE_MANA, mana_gain(), p()->gains.life_tap, this );
  }
};

// Bane of Agony (980 ...): Forever-navnet for Curse of Agony (instant, 24 s). Skaden bygger op: et
// tick i foerste tredjedel giver det halve af grundvaerdien, i anden hele, i sidste halvanden gang
// (wowsims/forever agony.go). Spell power-andelen stiger ikke. Summen er den samme som et jaevnt tick.
struct bane_of_agony_t : public warlock_spell_t
{
  bane_of_agony_t( warlock_t* player, util::string_view options_str ) :
    warlock_spell_t( "bane_of_agony", player, player->rank.bane_of_agony.find_rank()->spell() )
  {
    parse_options( options_str );
  }

  double base_ta( const action_state_t* s ) const override
  {
    double b = warlock_spell_t::base_ta( s );
    const dot_t* d = find_dot( s->target );
    if ( !d || !d->is_ticking() )
      return b;
    int n = d->num_ticks();
    int k = d->current_tick;
    if ( n <= 0 || k <= 0 )
      return b;
    if ( 3 * k <= n )
      return b * 0.5;
    if ( 3 * k <= 2 * n )
      return b;
    return b * 1.5;
  }
};

// Siphon Life (talent, 18265 ...): Periodic Health Leech; kun skaden simuleres (livet er uendeligt).
struct siphon_life_t : public warlock_spell_t
{
  siphon_life_t( warlock_t* player, util::string_view options_str ) :
    warlock_spell_t( "siphon_life", player, player->rank.siphon_life.find_rank()->spell() )
  {
    parse_options( options_str );
  }
};

// Shadowburn (talent, 17877 ...): instant Shadow-skade, 15 s nedkoeling (effekt 2; effekt 1 er
// soul shard-auraen ved maalets doed og simuleres ikke).
struct shadowburn_t : public warlock_spell_t
{
  shadowburn_t( warlock_t* player, util::string_view options_str ) :
    warlock_spell_t( "shadowburn", player, player->rank.shadowburn.find_rank()->spell() )
  {
    parse_options( options_str );
    dot_duration = 0_ms;
  }
};

// Conflagrate (talent, 1293817 ...): kraever warlockens Immolate paa maalet og fjerner den.
struct conflagrate_t : public warlock_spell_t
{
  conflagrate_t( warlock_t* player, util::string_view options_str ) :
    warlock_spell_t( "conflagrate", player, player->rank.conflagrate.find_rank()->spell() )
  {
    parse_options( options_str );
  }

  dot_t* immolate_dot( player_t* t ) const
  { return p()->immolate_action ? p()->immolate_action->find_dot( t ) : nullptr; }

  bool target_ready( player_t* t ) override
  {
    const dot_t* d = immolate_dot( t );
    if ( !d || !d->is_ticking() )
      return false;
    return warlock_spell_t::target_ready( t );
  }

  void impact( action_state_t* s ) override
  {
    warlock_spell_t::impact( s );
    if ( result_is_hit( s->result ) )
      if ( dot_t* d = immolate_dot( s->target ) )
        d->cancel();
  }
};

// Incinerate (talent, 412758 ...): Fire-skade, +25 % (effekt 2) hvis maalet har warlockens Immolate.
struct incinerate_t : public warlock_spell_t
{
  incinerate_t( warlock_t* player, util::string_view options_str ) :
    warlock_spell_t( "incinerate", player, player->rank.incinerate.find_rank()->spell() )
  {
    parse_options( options_str );
  }

  double composite_target_da_multiplier( player_t* t ) const override
  {
    double m = warlock_spell_t::composite_target_da_multiplier( t );
    const dot_t* d = p()->immolate_action ? p()->immolate_action->find_dot( t ) : nullptr;
    if ( d && d->is_ticking() )
      m *= 1.0 + data().effectN( 2 ).percent();
    return m;
  }
};

// Wrack (talent, 1316697): kanaliseret 6 s, Shadow-skade hvert sekund; mens den tikker, tager maalet
// +10 % fra warlockens andre Shadow-DoTs (se composite_target_ta_multiplier ovenfor).
struct wrack_t : public warlock_spell_t
{
  wrack_t( warlock_t* player, util::string_view options_str ) :
    warlock_spell_t( "wrack", player, player->rank.wrack.find_rank()->spell() )
  {
    parse_options( options_str );
    channeled = true;
    if ( !player->wrack_action )
      player->wrack_action = this;
  }
};

struct summon_imp_t final : public warlock_spell_t
{
  summon_imp_t( warlock_t* player, std::string_view options_str ) :
    warlock_spell_t( "summon_imp", player, player->find_spell( 688 ) )
  {
    parse_options( options_str );
    harmful = false;
  }

  void execute() override
  {
    warlock_spell_t::execute();
    p()->pets.imp->summon();
  }
};

///////////////////////////////////////////
//
// Demons
//
///////////////////////////////////////////

namespace pets {
  struct warlock_pet_t : public pet_t
  {
    warlock_pet_t( sim_t* sim, warlock_t* owner, std::string_view pet_name, pet_e pet_type, bool guardian = false ) :
      pet_t( sim, owner, pet_name, pet_type, guardian, false )
    {
    }

    const warlock_t* o() const
    { return static_cast<warlock_t*>( owner ); }

    warlock_t* o()
    { return static_cast<warlock_t*>( owner ); }
  };

  template <class Base>
  struct warlock_pet_action_t : parse_action_effects_t<Base>
  {
  private:
    using ab = parse_action_effects_t<Base>;
  public:
    using base_t = warlock_pet_action_t<Base>;

    template <typename... Args>
    warlock_pet_action_t( Args&&... args ) : ab( std::forward<Args>( args )... ) {}

    warlock_pet_t* p()
    { return debug_cast<warlock_pet_t*>( this->player ); }

    const warlock_pet_t* p() const
    { return debug_cast<const warlock_pet_t*>( this->player ); }

    warlock_t* o()
    { return debug_cast<warlock_t*>( p()->owner ); }

    const warlock_t* o() const
    { return debug_cast<const warlock_t*>( p()->owner ); }
  };

  using warlock_pet_attack_t = warlock_pet_action_t<melee_attack_t>;
  using warlock_pet_spell_t  = warlock_pet_action_t<spell_t>;

  namespace imp {
    struct imp_pet_t : warlock_pet_t
    {
      imp_pet_t( sim_t* sim, warlock_t* owner ) :
        warlock_pet_t( sim, owner, "imp", PET_IMP )
      {
        // from level 6-10 imp character sheet
        owner_coeff.armor = 0.35;
        owner_coeff.ap_from_sp = 0.17;

        // TODO: Imp HP looks to come from a table
        owner_coeff.health = 1.5;
        stamina_per_owner = 0;

        // matches in-game stats but int might not actually do anything on the pet
        intellect_per_owner = 0.3;
      }

      resource_e primary_resource() const override { return RESOURCE_MANA; }

      // copies owner's crit which matches imp char sheet
      double pet_crit() const override
      { return current_pet_stats.composite_spell_crit; }

      double resource_regen_per_second( resource_e r ) const override
      {
        if ( r != RESOURCE_MANA )
        {
          return pet_t::resource_regen_per_second( r );
        }

        // TODO - this is at level 7, probably need a table for this
        return recent_cast() ? 1.65 : 2.73;
      }

      void init_base_stats() override
      {
        warlock_pet_t::init_base_stats();

        // TODO: figure this out - probably another table of int/mana at every level
        // fit to obsevations level 3-8 but starts to break down at level 9
        double mana = 29 + (9.5 * level());
        resources.base[ RESOURCE_MANA ] = std::floor(mana);
      }

      void init_action_list() override
      {
        action_list_str = "firebolt";
        warlock_pet_t::init_action_list();
      }

      action_t* create_action( std::string_view, std::string_view ) override;
    };

    // TODO: There's some funky delay on beta. Some casts are back-to-back, some have a 0.5-1.0s gap even when
    // the imp has mana
    struct firebolt_t : warlock_pet_spell_t
    {
      firebolt_t( std::string_view n, imp_pet_t* p, std::string_view options_str  ) :
        warlock_pet_spell_t( n, p, p->o()->rank.firebolt.find_rank()->spell() )
      {
        parse_options( options_str );

        // beta casts have been wonky with gaps in between casts
        //ability_lag.mean   = 500_ms;
        //ability_lag.stddev = 100_ms;
      }
    };

    action_t* imp_pet_t::create_action( std::string_view name, std::string_view options_str )
    {
      if ( name == "firebolt" ) return new firebolt_t( name, this, options_str );

      return warlock_pet_t::create_action( name, options_str );
    }
  }
}

action_t* warlock_t::create_action( util::string_view name, util::string_view options_str )
{
  if ( name == "shadow_bolt" )
    return new shadow_bolt_t( this, options_str );
  if ( name == "immolate" )
    return new immolate_t( this, options_str );
  if ( name == "corruption" )
    return new corruption_t( this, options_str );

  if ( name == "summon_imp" )
    return new summon_imp_t( this, options_str );

  if ( name == "shoot" )
    return new shoot_t( this, options_str );
  if ( name == "life_tap" )
    return new life_tap_t( this, options_str );
  if ( name == "bane_of_agony" )
    return new bane_of_agony_t( this, options_str );
  if ( name == "siphon_life" )
    return new siphon_life_t( this, options_str );
  if ( name == "shadowburn" )
    return new shadowburn_t( this, options_str );
  if ( name == "conflagrate" )
    return new conflagrate_t( this, options_str );
  if ( name == "incinerate" )
    return new incinerate_t( this, options_str );
  if ( name == "wrack" )
    return new wrack_t( this, options_str );

  return player_t::create_action( name, options_str );
}

void warlock_t::create_pets()
{
  player_t::create_pets();

  pets.imp = new pets::imp::imp_pet_t( sim, this );
}

void warlock_t::init_base_stats()
{
  player_t::init_base_stats();

  base.attack_crit_chance = get_passive_player_value(0.02, "all_crit");
  base.spell_crit_chance = get_passive_player_value(0.01701, "all_crit");
}

void warlock_t::init_spells()
{
  player_t::init_spells();

  rank.corruption = { this,
    spell_container_t().set_spell(   172 ),
    spell_container_t().set_spell(  6222 ),
    spell_container_t().set_spell(  6223 ),
    spell_container_t().set_spell(  7648 ),
    spell_container_t().set_spell( 11671 ),
    spell_container_t().set_spell( 11672 ),
    spell_container_t().set_spell( 25311 ),
  };

  rank.firebolt = { this,
    spell_container_t().set_spell(  3110 ),  // 1
    spell_container_t().set_spell(  7799 ),  // 8
    spell_container_t().set_spell(  7800 ),  // 18
    spell_container_t().set_spell(  7801 ),  // 28
    spell_container_t().set_spell(  7802 ),  // 38
    spell_container_t().set_spell( 11762 ),  // 48
    spell_container_t().set_spell( 11763 ),  // 58
  };

  rank.immolate = { this,
    spell_container_t().set_spell(   348 ),
    spell_container_t().set_spell(   707 ),
    spell_container_t().set_spell(  1094 ),
    spell_container_t().set_spell(  2941 ),
    spell_container_t().set_spell( 11665 ),
    spell_container_t().set_spell( 11667 ),
    spell_container_t().set_spell( 11668 ),
    spell_container_t().set_spell( 25309 ),
  };

  rank.shadow_bolt = { this,
    spell_container_t().set_spell(   686 ),
    spell_container_t().set_spell(   695 ),
    spell_container_t().set_spell(   705 ),
    spell_container_t().set_spell(  1088 ),
    spell_container_t().set_spell(  1106 ),
    spell_container_t().set_spell(  7641 ),
    spell_container_t().set_spell( 11659 ),
    spell_container_t().set_spell( 11660 ),
    spell_container_t().set_spell( 11661 ),
    spell_container_t().set_spell( 25307 ),
  };

  talent.improved_life_tap    = find_talent_spell( talent_tree::CLASS, "Improved Life Tap" );
  talent.siphon_life          = find_talent_spell( talent_tree::CLASS, "Siphon Life" );
  talent.shadowburn           = find_talent_spell( talent_tree::CLASS, "Shadowburn" );
  talent.conflagrate          = find_talent_spell( talent_tree::CLASS, "Conflagrate" );
  talent.incinerate           = find_talent_spell( talent_tree::CLASS, "Incinerate" );
  talent.wrack                = find_talent_spell( talent_tree::CLASS, "Wrack" );
  talent.improved_shadow_bolt = find_talent_spell( talent_tree::CLASS, "Improved Shadow Bolt" );

  rank.life_tap = { this,
    spell_container_t().set_spell(  1454 ),  // 6
    spell_container_t().set_spell(  1455 ),  // 16
    spell_container_t().set_spell(  1456 ),  // 26
    spell_container_t().set_spell( 11687 ),  // 36
    spell_container_t().set_spell( 11688 ),  // 46
    spell_container_t().set_spell( 11689 ),  // 56
  };

  rank.bane_of_agony = { this,
    spell_container_t().set_spell(   980 ),  // 8
    spell_container_t().set_spell(  1014 ),  // 18
    spell_container_t().set_spell(  6217 ),  // 28
    spell_container_t().set_spell( 11711 ),  // 38
    spell_container_t().set_spell( 11712 ),  // 48
    spell_container_t().set_spell( 11713 ),  // 58
  };

  rank.siphon_life = { this, talent.siphon_life,
    spell_container_t().set_spell( 18265 ),  // 30
    spell_container_t().set_spell( 18879 ),  // 38
    spell_container_t().set_spell( 18880 ),  // 48
    spell_container_t().set_spell( 18881 ),  // 58
  };

  rank.shadowburn = { this, talent.shadowburn,
    spell_container_t().set_spell( 17877 ),  // 20
    spell_container_t().set_spell( 18867 ),  // 24
    spell_container_t().set_spell( 18868 ),  // 32
    spell_container_t().set_spell( 18869 ),  // 40
    spell_container_t().set_spell( 18870 ),  // 48
    spell_container_t().set_spell( 18871 ),  // 56
  };

  rank.conflagrate = { this, talent.conflagrate,
    spell_container_t().set_spell( 1293817 ),  // 25
    spell_container_t().set_spell( 1293818 ),  // 32
    spell_container_t().set_spell(   17962 ),  // 40
    spell_container_t().set_spell(   18930 ),  // 48
    spell_container_t().set_spell(   18931 ),  // 54
    spell_container_t().set_spell(   18932 ),  // 60
  };

  rank.incinerate = { this, talent.incinerate,
    spell_container_t().set_spell(  412758 ),  // 40
    spell_container_t().set_spell( 1293812 ),  // 50
    spell_container_t().set_spell( 1293813 ),  // 60
  };

  rank.wrack = { this, talent.wrack,
    spell_container_t().set_spell( 1316697 ),  // 40
  };

  parse_all_class_passives();
  parse_all_passive_talents();
}

void warlock_t::create_buffs()
{
  player_t::create_buffs();

  buff.shadow_vulnerability = make_buff_fallback( talent.improved_shadow_bolt.ok(), this, "shadow_vulnerability",
                                                  find_spell( 17794 ) );
}

void warlock_t::init_gains()
{
  player_t::init_gains();

  gains.life_tap = get_gain( "life_tap" );
}

void warlock_t::init_action_list()
{
  if ( !action_list_str.empty() )
  {
    player_t::init_action_list();
    return;
  }

  clear_action_priority_lists();
  warlock_apl::warlock( this );
  use_default_action_list = true;
  player_t::init_action_list();
}

}  // namespace warlock

const module_t* module_t::warlock()
{
  static warlock::warlock_module_t m;
  return &m;
}
